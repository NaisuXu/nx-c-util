/**
 * @file    nx_tiered_mem_pool.h
 * @brief   A tiered static memory pool implemented in pure C.
 *
 * Design goals: aimed at embedded development, as a deterministic replacement
 * for malloc/free - simple, predictable, fragmentation-free, heap-free.
 *
 * The pool consists of several "tiers", each a batch of equally sized blocks
 * carved from one caller-provided buffer. An allocation is rounded up to the
 * smallest tier large enough; a free block in that tier is found via its in-use
 * bitmap, falling back to a larger tier when exhausted. Free is O(1); alloc scans
 * a small per-tier bitmap.
 *
 * Features:
 *   - Purely static: no dynamic memory, no malloc/free; the caller owns the buffer.
 *   - Deterministic: bounded, predictable timing; no fragmentation within a tier.
 *   - Zero per-block overhead: blocks carry no header (the owning tier is found by
 *     address range, and the in-use bitmaps sit in a small metadata region carved
 *     from the same buffer), so block sizes down to a single alignment unit are
 *     allowed.
 *   - Built-in double-free detection via the bitmap, at no extra cost.
 *   - Exhaustion fallback to a larger tier (optional).
 *   - Optional locking: a single-context user needs no lock; when alloc/free run
 *     from several contexts the caller supplies an nx_lock that wraps each of them.
 *     This module introduces no locks of its own.
 */
#ifndef NX_TIERED_MEM_POOL_H
#define NX_TIERED_MEM_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return codes for pool operations.
 */
typedef enum {
    NX_TIERED_OK = 0,        /**< Operation succeeded */
    NX_TIERED_ERR_PARAM,     /**< Invalid argument (NULL pointer, etc.) */
    NX_TIERED_ERR_CONFIG,    /**< Invalid config: bad tier, or buffer too small */
    NX_TIERED_ERR_INVALID,   /**< Freed a pointer that this pool does not own, or not on a block boundary */
    NX_TIERED_ERR_DOUBLE_FREE /**< Freed a block that is already free */
} nx_tiered_ret_t;

/**
 * @brief Runtime state of a single tier.
 *
 * @note  Implementation detail; do not access directly.
 */
typedef struct {
    uint8_t *base;            /**< First byte of the tier buffer */
    uint8_t *end;             /**< One past the last usable byte (base + block_size * block_count) */
    size_t   block_size;      /**< Effective (alignment-rounded) block size in bytes */
    size_t   block_count;     /**< Number of blocks */
    size_t   free_count;      /**< Number of currently free blocks */
    size_t   min_free_count;  /**< Lowest free-block count ever seen (i.e. peak usage) */
    size_t   next_free_hint;  /**< Block index to start the next allocation scan from */
    uint8_t *used;            /**< In-use bitmap (carved from the arena), one bit per block (1 = allocated) */
} nx_tiered_level_t;

/**
 * @brief Tiered memory pool handle.
 *
 * A fixed-size handle independent of the tier count and block counts: the tier
 * table, the per-tier bitmaps and the block storage are all carved from the
 * caller's buffer at init time. Cheap to keep an array of.
 *
 * @note  The struct members are implementation details; use the API instead.
 */
typedef struct {
    nx_tiered_level_t *tiers;   /**< Tier table (carved from the arena), sorted by ascending block_size */
    size_t         tier_count;  /**< Number of active tiers */
    bool           forbid_fallback;  /**< When true, forbid falling back to a larger tier on exhaustion (set at init) */
    const nx_lock_t *lock;      /**< Critical section around alloc/free; NULL = none (set at init) */
} nx_tiered_mem_pool_t;

/**
 * @brief Config for a single tier, provided by the caller at init time.
 *
 * Storage is not specified here - all tiers share the one buffer passed to
 * nx_tiered_mem_pool_init(), which the pool carves by block_size * block_count
 * in turn.
 */
typedef struct {
    size_t  block_size;    /**< Size of a single block in bytes (rounded up to the alignment at init); must be > 0 */
    size_t  block_count;   /**< Number of blocks in this tier; must be > 0 (bounded only by the buffer size) */
} nx_tiered_level_cfg_t;

/**
 * @brief Config for initializing a pool (everything except the pool handle).
 *
 * Fill one with a designated initializer (forbid_fallback may be omitted, it
 * defaults to false) and pass it to nx_tiered_mem_pool_init(). @c tiers points at
 * a caller-owned array of @c tier_count entries; it is only read during init and
 * need not outlive the call. The buffer needs no special alignment; init aligns
 * internally.
 *
 * @code
 *   static uint8_t mem[2048];
 *   const nx_tiered_level_cfg_t tiers[] = { {32, 8}, {128, 4}, {512, 2} };
 *   nx_tiered_mem_pool_cfg_t cfg = {
 *       .memory      = mem,
 *       .memory_size = sizeof(mem),
 *       .tiers       = tiers,
 *       .tier_count  = 3,
 *   };
 * @endcode
 */
typedef struct {
    void                 *memory;       /**< Caller buffer, must not be NULL; any alignment (init aligns internally, wasting up to alignof(max_align_t)-1 bytes) */
    size_t                memory_size;  /**< Size of @c memory in bytes */
    const nx_tiered_level_cfg_t *tiers; /**< Caller-owned tier list of @c tier_count entries (read only during init) */
    size_t                tier_count;   /**< Number of tiers; must be > 0 */
    bool                  forbid_fallback;  /**< Fallback policy on exhaustion; false (default) allows falling back to a larger tier */
    const nx_lock_t      *lock;         /**< Critical section around alloc/free; NULL (default) = no locking (single-context use) */
} nx_tiered_mem_pool_cfg_t;

/**
 * @brief Statistics snapshot of a single tier (for tuning / diagnostics).
 */
typedef struct {
    size_t block_size;    /**< Effective block size in bytes */
    size_t block_count;   /**< Total number of blocks in the tier */
    size_t free_count;    /**< Number of currently free blocks */
    size_t peak_used;     /**< High-water mark of simultaneously used blocks */
} nx_tiered_level_stat_t;

/**
 * @brief  Initialize the pool from a configuration struct.
 *
 * The pool carves @c cfg->memory into a small metadata region (the tier table and
 * one in-use bitmap per tier) followed by one block-storage region per tier
 * (block_size rounded up to max_align_t alignment, x block_count). Tiers may be
 * given in any order; they are sorted internally by ascending block size.
 *
 * Because the tier table and bitmaps live in the same buffer as the blocks, a
 * caller write that runs past a block can corrupt pool metadata; size blocks
 * correctly. Metadata scales with the tier count and total block count only, so
 * it stays small.
 *
 * @p out_required_bytes reports the exact bytes needed (metadata + blocks), so you
 * can oversize the buffer, run once, then shrink to fit. It is written whenever the
 * tier list is valid, even when the buffer is too small. An unaligned buffer costs
 * up to alignof(max_align_t)-1 padding bytes on top of that, so leave headroom (or
 * align the buffer).
 *
 * @param  pool               Pool handle, must not be NULL.
 * @param  cfg                Configuration, must not be NULL (see nx_tiered_mem_pool_cfg_t).
 * @param  out_required_bytes May be NULL; if non-NULL, receives the total bytes
 *                            required (valid whenever the tier list is valid).
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool/cfg is NULL, or memory/memory_size/tiers/tier_count is zero/NULL;
 *         NX_TIERED_ERR_CONFIG if a tier config is invalid (block_size or
 *         block_count is 0), or the buffer is too small (including once internal
 *         alignment padding is accounted for).
 */
nx_tiered_ret_t nx_tiered_mem_pool_init(nx_tiered_mem_pool_t           *pool,
                                        const nx_tiered_mem_pool_cfg_t *cfg,
                                        size_t                         *out_required_bytes);

/**
 * @brief  Allocate a block of at least @p size bytes.
 *
 * Takes a block from the smallest large-enough tier with a free block, falling
 * back to a larger tier when exhausted (unless forbid_fallback was set at init,
 * in which case it returns NULL rather than borrow from a larger tier). The
 * returned block is max_align_t aligned; contents are uninitialized (like malloc).
 *
 * The whole operation is wrapped in the configured lock (cfg.lock); NULL means no
 * locking, for single-context use.
 *
 * @param  pool Pool handle.
 * @param  size Requested byte count; returns NULL when 0.
 *
 * @return Pointer to the block; NULL if no suitable tier still has a free block.
 */
void *nx_tiered_mem_pool_alloc(nx_tiered_mem_pool_t *pool, size_t size);

/**
 * @brief  Return a block previously obtained from nx_tiered_mem_pool_alloc().
 *
 * The owning tier is inferred from the pointer address, so no size is needed.
 * Passing NULL is a no-op (returns NX_TIERED_OK), consistent with free().
 * Freeing a block that is already free is rejected (NX_TIERED_ERR_DOUBLE_FREE)
 * via the in-use bitmap, always on and O(1).
 *
 * The whole operation is wrapped in the configured lock (cfg.lock); NULL means no
 * locking, for single-context use.
 *
 * @param  pool Pool handle.
 * @param  ptr  Block to return, or NULL.
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool is NULL;
 *         NX_TIERED_ERR_INVALID if ptr is not a block owned by this pool, or does
 *         not land on a block boundary;
 *         NX_TIERED_ERR_DOUBLE_FREE if ptr points to a block that is already free.
 */
nx_tiered_ret_t nx_tiered_mem_pool_free(nx_tiered_mem_pool_t *pool, void *ptr);

/**
 * @brief  Number of active tiers in the pool.
 *
 * Tiers are sorted by ascending block_size, so valid indices for
 * nx_tiered_mem_pool_get_tier_stat() are 0 (smallest block) .. count-1.
 *
 * @param  pool Pool handle.
 * @return The tier count, or 0 if @p pool is NULL.
 */
size_t nx_tiered_mem_pool_tier_count(const nx_tiered_mem_pool_t *pool);

/**
 * @brief  Read statistics for one tier, by index.
 *
 * Tiers are ordered by ascending block_size (index 0 is the smallest block).
 *
 * @param  pool        Pool handle, must not be NULL.
 * @param  tier_index  Tier to read, in [0, nx_tiered_mem_pool_tier_count()).
 * @param  out         Receives the tier snapshot, must not be NULL.
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool/out is NULL;
 *         NX_TIERED_ERR_CONFIG if tier_index is out of range.
 */
nx_tiered_ret_t nx_tiered_mem_pool_get_tier_stat(const nx_tiered_mem_pool_t *pool,
                                                 size_t                      tier_index,
                                                 nx_tiered_level_stat_t     *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_TIERED_MEM_POOL_H */
