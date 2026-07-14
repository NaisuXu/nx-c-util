/**
 * @file    nx_tiered_mem_pool.c
 * @brief   Implementation of the nx_tiered_mem_pool tiered static memory pool.
 */
#include "nx_tiered_mem_pool.h"

/* Alignment that every block (and every returned pointer) is rounded up to. */
#define NX_TIERED_ALIGN  (_Alignof(max_align_t))

/* Round n up to a multiple of the power-of-two alignment a. */
#define NX_TIERED_ALIGN_UP(n, a)  (((n) + (a) - 1u) & ~((size_t)(a) - 1u))

/**
 * @brief Build the intrusive free list for a single tier and reset its counters.
 *
 * Blocks are chained in address order; each free block stores the address of
 * the next free block in its first sizeof(void*) bytes.
 */
static void nx_tiered_tier_build_freelist(nx_tiered_level_t *t)
{
    void *head = NULL;

    /* Chain from the last block back to the first so the head ends up at base. */
    for (size_t i = t->block_count; i > 0u; i--) {
        void *block = t->base + (i - 1u) * t->block_size;
        *(void **)block = head;
        head = block;
    }

    t->free_list      = head;
    t->free_count     = t->block_count;
    t->min_free_count = t->block_count;
}

/**
 * @brief Validate a single tier's config, lay it out at @p base and populate its
 *        runtime state (unsorted).
 *
 * @param t     Tier runtime state to fill in.
 * @param cfg   Tier config (block_size / block_count).
 * @param base  Start address of this tier within the whole buffer (must be aligned).
 * @return NX_TIERED_OK on success; NX_TIERED_ERR_CONFIG if the config is invalid.
 */
static nx_tiered_ret_t nx_tiered_tier_setup(nx_tiered_level_t           *t,
                                        const nx_tiered_level_cfg_t *cfg,
                                        uint8_t                  *base)
{
    if (cfg->block_count == 0u || cfg->block_size < sizeof(void *)) {
        return NX_TIERED_ERR_CONFIG;
    }

    size_t block_size = NX_TIERED_ALIGN_UP(cfg->block_size, NX_TIERED_ALIGN);

    t->base        = base;
    t->block_size  = block_size;
    t->block_count = cfg->block_count;
    t->end         = base + block_size * cfg->block_count;

    nx_tiered_tier_build_freelist(t);

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
    if (cfg->block_count == 0u || cfg->block_size < sizeof(void *)) {
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
    if (cfg->memory == NULL || cfg->memory_size == 0u || cfg->tier_count == 0u) {
        return NX_TIERED_ERR_PARAM;
    }
    if (cfg->tier_count > NX_TIERED_MEM_POOL_MAX_TIERS) {
        return NX_TIERED_ERR_CONFIG;
    }

    /* First pass: validate every tier and sum the total bytes required, so the
     * caller learns the exact size even when the buffer turns out too small. */
    size_t required = 0u;
    for (size_t i = 0u; i < cfg->tier_count; i++) {
        size_t        seg_bytes;
        nx_tiered_ret_t r = nx_tiered_tier_bytes(&cfg->tiers[i], &seg_bytes);
        if (r != NX_TIERED_OK) {
            return r;   /* invalid tier config; required stays 0 */
        }
        if (seg_bytes > (SIZE_MAX - required)) {
            return NX_TIERED_ERR_CONFIG;   /* total overflows */
        }
        required += seg_bytes;
    }
    if (out_required_bytes != NULL) {
        *out_required_bytes = required;
    }

    /* The whole buffer must be aligned so every tier's and every block's start
     * address is aligned too. */
    if (((uintptr_t)cfg->memory % NX_TIERED_ALIGN) != 0u) {
        return NX_TIERED_ERR_CONFIG;
    }
    if (cfg->memory_size < required) {
        return NX_TIERED_ERR_CONFIG;   /* buffer too small; required already reported */
    }

    /* Second pass: carve the buffer among the tiers, insertion-sorting them by
     * ascending block size. */
    uint8_t *cursor = (uint8_t *)cfg->memory;

    pool->tier_count      = 0u;
    pool->forbid_fallback = cfg->forbid_fallback;
    for (size_t i = 0u; i < cfg->tier_count; i++) {
        size_t seg_bytes;
        (void)nx_tiered_tier_bytes(&cfg->tiers[i], &seg_bytes);   /* validated in pass 1 */

        nx_tiered_level_t t;
        (void)nx_tiered_tier_setup(&t, &cfg->tiers[i], cursor);   /* validated in pass 1 */
        cursor += seg_bytes;

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

    /* Tiers are sorted ascending, so the first tier that is both large enough
     * and has a free block is the best available fit (and this naturally falls
     * back to larger tiers).
     *
     * When forbid_fallback is true, borrowing from a "larger" tier is not
     * allowed: only tiers whose block size equals the ideal tier's (the first
     * tier with block_size >= size) may be used; once a tier's block size
     * exceeds the ideal size, the search stops and NULL is returned. Note that
     * multiple tiers sharing the same block size are equivalent, not "larger",
     * so they may still be used interchangeably. */
    size_t ideal_block_size = 0u;

    for (size_t i = 0u; i < pool->tier_count; i++) {
        nx_tiered_level_t *t = &pool->tiers[i];
        if (t->block_size < size) {
            continue;   /* block too small, skip */
        }

        /* Record the ideal tier's block size (the first large-enough tier). */
        if (ideal_block_size == 0u) {
            ideal_block_size = t->block_size;
        }

        /* Fallback forbidden: block size already exceeds the ideal tier, do not
         * borrow from a larger tier. */
        if (pool->forbid_fallback && t->block_size > ideal_block_size) {
            break;
        }

        if (t->free_list == NULL) {
            continue;   /* this tier is exhausted; try the next same-size tier (or fall back to a larger one) */
        }

        void *block = t->free_list;
        t->free_list = *(void **)block;   /* pop the head */
        t->free_count--;
        if (t->free_count < t->min_free_count) {
            t->min_free_count = t->free_count;
        }
        return block;
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
        if (((size_t)(p - t->base) % t->block_size) != 0u) {
            return NX_TIERED_ERR_INVALID;
        }

        *(void **)ptr = t->free_list;   /* push back onto the free list */
        t->free_list = ptr;
        t->free_count++;
        return NX_TIERED_OK;
    }

    return NX_TIERED_ERR_INVALID;   /* not owned by this pool */
}

nx_tiered_ret_t nx_tiered_mem_pool_get_stat(const nx_tiered_mem_pool_t *pool,
                                        nx_tiered_pool_stat_t      *out)
{
    if (pool == NULL || out == NULL) {
        return NX_TIERED_ERR_PARAM;
    }

    out->tier_count = pool->tier_count;
    for (size_t i = 0u; i < pool->tier_count; i++) {
        const nx_tiered_level_t *t = &pool->tiers[i];
        out->tiers[i].block_size  = t->block_size;
        out->tiers[i].block_count = t->block_count;
        out->tiers[i].free_count  = t->free_count;
        out->tiers[i].peak_used   = t->block_count - t->min_free_count;
    }

    return NX_TIERED_OK;
}
