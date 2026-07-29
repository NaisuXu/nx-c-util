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
 *     address range, and an in-use bitmap lives in the pool handle), so block
 *     sizes down to a single alignment unit are allowed.
 *   - Built-in double-free detection via the bitmap, at no extra cost.
 *   - Exhaustion fallback to a larger tier (optional).
 *   - Not thread-safe: concurrent access must be locked by the caller.
 */
#ifndef NX_TIERED_MEM_POOL_H
#define NX_TIERED_MEM_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum tiers per pool. Override before including this header. */
#ifndef NX_TIERED_MEM_POOL_MAX_TIERS
#define NX_TIERED_MEM_POOL_MAX_TIERS 4
#endif

/** Maximum blocks per tier (bounds the per-tier in-use bitmap). Override before including. */
#ifndef NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER
#define NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER 64
#endif

/** Bitmap bytes needed to track NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER blocks. */
#define NX_TIERED_MEM_POOL_BITMAP_BYTES \
    (((NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER) + 7u) / 8u)

/**
 * @brief Return codes for pool operations.
 */
typedef enum {
    NX_TIERED_OK = 0,        /**< Operation succeeded */
    NX_TIERED_ERR_PARAM,     /**< Invalid argument (NULL pointer, etc.) */
    NX_TIERED_ERR_CONFIG,    /**< Invalid config: bad tier, too many tiers, or buffer too small */
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
    uint8_t  used[NX_TIERED_MEM_POOL_BITMAP_BYTES];  /**< In-use bitmap, one bit per block (1 = allocated) */
} nx_tiered_level_t;

/**
 * @brief Tiered memory pool handle.
 *
 * @note  The struct members are implementation details; use the API instead.
 */
typedef struct {
    nx_tiered_level_t tiers[NX_TIERED_MEM_POOL_MAX_TIERS];  /**< The tiers, sorted by ascending block_size */
    size_t         tier_count;                         /**< Number of active tiers */
    bool           forbid_fallback;                     /**< When true, forbid falling back to a larger tier on exhaustion (set at init) */
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
    size_t  block_count;   /**< Number of blocks in this tier; must be in [1, NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER] */
} nx_tiered_level_cfg_t;

/**
 * @brief Config for initializing a pool (everything except the pool handle).
 *
 * Fill one with a designated initializer (forbid_fallback may be omitted, it
 * defaults to false) and pass it to nx_tiered_mem_pool_init(). The tier list is
 * embedded inline. The buffer needs no special alignment; init aligns internally.
 *
 * @code
 *   static uint8_t mem[2048];
 *   nx_tiered_mem_pool_cfg_t cfg = {
 *       .memory      = mem,
 *       .memory_size = sizeof(mem),
 *       .tiers       = { {32, 8}, {128, 4}, {512, 2} },
 *       .tier_count  = 3,
 *   };
 * @endcode
 */
typedef struct {
    void                 *memory;       /**< Caller buffer, must not be NULL; any alignment (init aligns internally, wasting up to alignof(max_align_t)-1 bytes) */
    size_t                memory_size;  /**< Size of @c memory in bytes */
    nx_tiered_level_cfg_t tiers[NX_TIERED_MEM_POOL_MAX_TIERS];  /**< Embedded tier list (first tier_count entries used) */
    size_t                tier_count;   /**< Number of tiers, in [1, NX_TIERED_MEM_POOL_MAX_TIERS] */
    bool                  forbid_fallback;  /**< Fallback policy on exhaustion; false (default) allows falling back to a larger tier */
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
 * @brief Statistics snapshot of the whole pool (for tuning / diagnostics).
 */
typedef struct {
    size_t              tier_count;                          /**< Number of active tiers */
    nx_tiered_level_stat_t tiers[NX_TIERED_MEM_POOL_MAX_TIERS];   /**< Per-tier stats, sorted by ascending block_size (first tier_count entries are valid) */
} nx_tiered_pool_stat_t;

/**
 * @brief  Initialize the pool from a configuration struct.
 *
 * The pool carves @c cfg->memory into one region per tier (block_size rounded up
 * to max_align_t alignment, x block_count). Tiers may be given in any order; they
 * are sorted internally by ascending block size.
 *
 * @p out_required_bytes reports the exact bytes the tiers need, so you can
 * oversize the buffer, run once, then shrink to fit. It is written whenever the
 * tier list is valid, even when the buffer is too small. An unaligned buffer
 * costs up to alignof(max_align_t)-1 padding bytes on top of that, so leave
 * headroom (or align the buffer).
 *
 * @param  pool               Pool handle, must not be NULL.
 * @param  cfg                Configuration, must not be NULL (see nx_tiered_mem_pool_cfg_t).
 * @param  out_required_bytes May be NULL; if non-NULL, receives the total bytes the
 *                            tiers require (valid whenever the tier list is valid).
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool/cfg is NULL, or memory/memory_size/tier_count is zero/NULL;
 *         NX_TIERED_ERR_CONFIG if a tier config is invalid (block_size is 0, or
 *         block_count is 0 or exceeds NX_TIERED_MEM_POOL_MAX_BLOCKS_PER_TIER),
 *         there are too many tiers, or the buffer is too small (including once
 *         internal alignment padding is accounted for).
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
 * @brief  Read statistics for the whole pool.
 *
 * Fills @p out in one shot: the number of tiers plus a per-tier snapshot (sorted
 * by ascending block_size, index 0 being the smallest block).
 *
 * @param  pool Pool handle, must not be NULL.
 * @param  out  Receives the statistics, must not be NULL.
 *
 * @return NX_TIERED_OK on success; NX_TIERED_ERR_PARAM on invalid argument.
 */
nx_tiered_ret_t nx_tiered_mem_pool_get_stat(const nx_tiered_mem_pool_t *pool,
                                        nx_tiered_pool_stat_t          *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_TIERED_MEM_POOL_H */
