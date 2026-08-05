/**
 * @file    nx_tiered_mem_pool.c
 * @brief   Implementation of the nx_tiered_mem_pool tiered static memory pool.
 */
#include "nx_tiered_mem_pool.h"

/* Alignment that every block (and every returned pointer) is rounded up to. */
#define NX_TIERED_ALIGN  (_Alignof(max_align_t))

/* Round n up to a multiple of the power-of-two alignment a. */
#define NX_TIERED_ALIGN_UP(n, a)  (((n) + (a) - 1u) & ~((size_t)(a) - 1u))

/** Test whether block @p idx is currently in use (bit set). */
static inline bool nx_tiered_bit_used(const nx_tiered_level_t *t, size_t idx)
{
    return (t->used[idx >> 3] & (uint8_t)(1u << (idx & 7u))) != 0u;
}

/** Mark block @p idx as in use. */
static inline void nx_tiered_bit_set(nx_tiered_level_t *t, size_t idx)
{
    t->used[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

/** Mark block @p idx as free. */
static inline void nx_tiered_bit_clear(nx_tiered_level_t *t, size_t idx)
{
    t->used[idx >> 3] &= (uint8_t)~(1u << (idx & 7u));
}

/** Bytes of in-use bitmap needed to track @p block_count blocks. */
static inline size_t nx_tiered_bitmap_bytes(size_t block_count)
{
    return (block_count + 7u) / 8u;
}

/**
 * @brief Reset a tier to "all free": clear the in-use bitmap and reset counters.
 */
static void nx_tiered_tier_reset(nx_tiered_level_t *t)
{
    size_t bitmap_bytes = nx_tiered_bitmap_bytes(t->block_count);
    for (size_t i = 0u; i < bitmap_bytes; i++) {
        t->used[i] = 0u;
    }
    t->free_count     = t->block_count;
    t->min_free_count = t->block_count;
    t->next_free_hint = 0u;
}

/**
 * @brief Validate a single tier's config, lay it out at @p base with its bitmap at
 *        @p used, and populate its runtime state (unsorted).
 *
 * @param t     Tier runtime state to fill in.
 * @param cfg   Tier config (block_size / block_count).
 * @param base  Start address of this tier's block storage (must be aligned).
 * @param used  Start address of this tier's in-use bitmap (from the arena).
 * @return NX_TIERED_OK on success; NX_TIERED_ERR_CONFIG if the config is invalid.
 */
static nx_tiered_ret_t nx_tiered_tier_setup(nx_tiered_level_t           *t,
                                        const nx_tiered_level_cfg_t *cfg,
                                        uint8_t                  *base,
                                        uint8_t                  *used)
{
    if (cfg->block_count == 0u || cfg->block_size == 0u) {
        return NX_TIERED_ERR_CONFIG;
    }

    size_t block_size = NX_TIERED_ALIGN_UP(cfg->block_size, NX_TIERED_ALIGN);

    t->base        = base;
    t->block_size  = block_size;
    t->block_count = cfg->block_count;
    t->end         = base + block_size * cfg->block_count;
    t->used        = used;

    nx_tiered_tier_reset(t);

    return NX_TIERED_OK;
}

/**
 * @brief Compute the bytes a tier needs for layout (aligned block size x block
 *        count) and detect multiplication overflow.
 *
 * @param cfg        Tier config.
 * @param out_bytes  On success, receives the required byte count.
 * @return NX_TIERED_OK on success; NX_TIERED_ERR_CONFIG if invalid or overflowing.
 */
static nx_tiered_ret_t nx_tiered_tier_bytes(const nx_tiered_level_cfg_t *cfg,
                                        size_t                   *out_bytes)
{
    if (cfg->block_count == 0u || cfg->block_size == 0u) {
        return NX_TIERED_ERR_CONFIG;
    }

    size_t block_size = NX_TIERED_ALIGN_UP(cfg->block_size, NX_TIERED_ALIGN);
    if (block_size > (SIZE_MAX / cfg->block_count)) {
        return NX_TIERED_ERR_CONFIG;   /* block_size * block_count overflows */
    }

    *out_bytes = block_size * cfg->block_count;
    return NX_TIERED_OK;
}

nx_tiered_ret_t nx_tiered_mem_pool_init(nx_tiered_mem_pool_t           *pool,
                                        const nx_tiered_mem_pool_cfg_t *cfg,
                                        size_t                         *out_required_bytes)
{
    if (out_required_bytes != NULL) {
        *out_required_bytes = 0u;
    }
    if (pool == NULL || cfg == NULL) {
        return NX_TIERED_ERR_PARAM;
    }
    if (cfg->memory == NULL || cfg->memory_size == 0u ||
        cfg->tiers == NULL || cfg->tier_count == 0u) {
        return NX_TIERED_ERR_PARAM;
    }

    /* First pass: validate every tier and sum both the block-storage bytes and the
     * metadata bytes (tier table + one bitmap per tier), so the caller learns the
     * exact size even when the buffer turns out too small. Everything is carved from
     * cfg->memory: [tier table][bitmaps] then, re-aligned, [block storage]. */
    size_t block_total  = 0u;   /* block storage across all tiers */
    size_t bitmap_total = 0u;   /* in-use bitmaps across all tiers */
    for (size_t i = 0u; i < cfg->tier_count; i++) {
        size_t        seg_bytes;
        nx_tiered_ret_t r = nx_tiered_tier_bytes(&cfg->tiers[i], &seg_bytes);
        if (r != NX_TIERED_OK) {
            return r;   /* invalid tier config; required stays 0 */
        }
        if (seg_bytes > (SIZE_MAX - block_total)) {
            return NX_TIERED_ERR_CONFIG;   /* total overflows */
        }
        block_total += seg_bytes;
        bitmap_total += nx_tiered_bitmap_bytes(cfg->tiers[i].block_count);
    }

    /* Metadata: the tier table (max_align_t-aligned when the arena is) directly
     * followed by the byte-aligned bitmaps, then padded up so block storage lands
     * on a max_align_t boundary. */
    if (cfg->tier_count > (SIZE_MAX / sizeof(nx_tiered_level_t))) {
        return NX_TIERED_ERR_CONFIG;   /* tier table size overflows */
    }
    size_t table_bytes = cfg->tier_count * sizeof(nx_tiered_level_t);
    if (bitmap_total > (SIZE_MAX - table_bytes)) {
        return NX_TIERED_ERR_CONFIG;
    }
    size_t meta_bytes  = table_bytes + bitmap_total;
    size_t meta_padded = NX_TIERED_ALIGN_UP(meta_bytes, NX_TIERED_ALIGN);
    if (meta_padded < meta_bytes || block_total > (SIZE_MAX - meta_padded)) {
        return NX_TIERED_ERR_CONFIG;   /* metadata + block total overflows */
    }
    size_t required    = meta_padded + block_total;
    if (out_required_bytes != NULL) {
        *out_required_bytes = required;
    }

    /* Align the working base up (buffer need not be aligned), costing at most
     * NX_TIERED_ALIGN - 1 padding bytes, which the buffer must have room for. */
    uintptr_t raw     = (uintptr_t)cfg->memory;
    uintptr_t aligned = (raw + (NX_TIERED_ALIGN - 1u)) & ~((uintptr_t)NX_TIERED_ALIGN - 1u);
    size_t    padding = (size_t)(aligned - raw);
    if (cfg->memory_size < padding || (cfg->memory_size - padding) < required) {
        return NX_TIERED_ERR_CONFIG;   /* buffer too small; required already reported */
    }

    /* Carve the metadata region: the tier table first (arena is max_align_t-aligned,
     * which satisfies the table's own alignment), then the bitmaps. */
    uint8_t *meta_base   = (uint8_t *)aligned;
    pool->tiers          = (nx_tiered_level_t *)meta_base;
    uint8_t *bitmap_cur  = meta_base + table_bytes;

    /* Block storage starts after the padded metadata region. */
    uint8_t *block_cur   = meta_base + meta_padded;

    /* Second pass: lay out each tier's blocks + bitmap, insertion-sorting the tier
     * table by ascending block size. Bitmaps and blocks are assigned in config order;
     * only the table slots are sorted, and each tier keeps its own base/used. */
    pool->tier_count      = 0u;
    pool->forbid_fallback = cfg->forbid_fallback;
    for (size_t i = 0u; i < cfg->tier_count; i++) {
        size_t seg_bytes;
        (void)nx_tiered_tier_bytes(&cfg->tiers[i], &seg_bytes);   /* validated in pass 1 */

        nx_tiered_level_t t;
        (void)nx_tiered_tier_setup(&t, &cfg->tiers[i], block_cur, bitmap_cur);  /* validated in pass 1 */
        block_cur  += seg_bytes;
        bitmap_cur += nx_tiered_bitmap_bytes(cfg->tiers[i].block_count);

        size_t pos = pool->tier_count;
        while (pos > 0u && pool->tiers[pos - 1u].block_size > t.block_size) {
            pool->tiers[pos] = pool->tiers[pos - 1u];
            pos--;
        }
        pool->tiers[pos] = t;
        pool->tier_count++;
    }

    return NX_TIERED_OK;
}

void *nx_tiered_mem_pool_alloc(nx_tiered_mem_pool_t *pool, size_t size)
{
    if (pool == NULL || size == 0u) {
        return NULL;
    }

    /* Tiers are sorted ascending: the first large-enough tier with a free block
     * is the best fit, which naturally falls back to larger tiers. With
     * forbid_fallback, the search stops once block_size exceeds the ideal tier's
     * (equal-size tiers are equivalent, not "larger", so they stay usable). */
    size_t ideal_block_size = 0u;

    for (size_t i = 0u; i < pool->tier_count; i++) {
        nx_tiered_level_t *t = &pool->tiers[i];
        if (t->block_size < size) {
            continue;
        }
        if (ideal_block_size == 0u) {
            ideal_block_size = t->block_size;
        }
        if (pool->forbid_fallback && t->block_size > ideal_block_size) {
            break;
        }
        if (t->free_count == 0u) {
            continue;
        }

        /* Scan from the rolling hint for a free block; free_count > 0 guarantees
         * one exists, so this terminates. */
        size_t idx = t->next_free_hint;
        for (size_t scanned = 0u; scanned < t->block_count; scanned++) {
            if (idx >= t->block_count) {
                idx = 0u;
            }
            if (!nx_tiered_bit_used(t, idx)) {
                break;
            }
            idx++;
        }

        nx_tiered_bit_set(t, idx);
        t->next_free_hint = idx + 1u;
        t->free_count--;
        if (t->free_count < t->min_free_count) {
            t->min_free_count = t->free_count;
        }
        return t->base + idx * t->block_size;
    }

    return NULL;
}

nx_tiered_ret_t nx_tiered_mem_pool_free(nx_tiered_mem_pool_t *pool, void *ptr)
{
    if (pool == NULL) {
        return NX_TIERED_ERR_PARAM;
    }
    if (ptr == NULL) {
        return NX_TIERED_OK;   /* free(NULL) is a no-op, consistent with standard free() */
    }

    uint8_t *p = (uint8_t *)ptr;

    /* Locate the owning tier purely by address range (zero per-block overhead). */
    for (size_t i = 0u; i < pool->tier_count; i++) {
        nx_tiered_level_t *t = &pool->tiers[i];
        if (p < t->base || p >= t->end) {
            continue;
        }

        /* Must land exactly on a block boundary, otherwise the pointer is invalid. */
        size_t offset = (size_t)(p - t->base);
        if ((offset % t->block_size) != 0u) {
            return NX_TIERED_ERR_INVALID;
        }
        size_t idx = offset / t->block_size;

        /* A clear bit means the block is already free: reject the double free. */
        if (!nx_tiered_bit_used(t, idx)) {
            return NX_TIERED_ERR_DOUBLE_FREE;
        }

        nx_tiered_bit_clear(t, idx);
        t->free_count++;
        return NX_TIERED_OK;
    }

    return NX_TIERED_ERR_INVALID;   /* not owned by this pool */
}

size_t nx_tiered_mem_pool_tier_count(const nx_tiered_mem_pool_t *pool)
{
    return (pool == NULL) ? 0u : pool->tier_count;
}

nx_tiered_ret_t nx_tiered_mem_pool_get_tier_stat(const nx_tiered_mem_pool_t *pool,
                                                 size_t                      tier_index,
                                                 nx_tiered_level_stat_t     *out)
{
    if (pool == NULL || out == NULL) {
        return NX_TIERED_ERR_PARAM;
    }
    if (tier_index >= pool->tier_count) {
        return NX_TIERED_ERR_CONFIG;
    }

    const nx_tiered_level_t *t = &pool->tiers[tier_index];
    out->block_size  = t->block_size;
    out->block_count = t->block_count;
    out->free_count  = t->free_count;
    out->peak_used   = t->block_count - t->min_free_count;

    return NX_TIERED_OK;
}
