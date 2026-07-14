/**
 * @file    nx_tiered_mem_pool.h
 * @brief   A tiered static memory pool implemented in pure C.
 *
 * Design goals: aimed at embedded development, as a deterministic replacement
 * for malloc/free - simple, predictable, fragmentation-free, heap-free.
 *
 * The pool consists of several "tiers", each a batch of equally sized blocks.
 * The caller only provides one aligned static buffer; at init time the pool
 * carves it into several regions according to the per-tier config. An allocation
 * request is rounded up to the smallest tier whose block is large enough, and a
 * block is taken from that tier's free list; if that tier is exhausted, it falls
 * back to a larger tier. Both allocation and free are O(1).
 *
 * Features:
 *   - Purely static: all storage is provided by the caller as one buffer; this
 *     library uses no dynamic memory and does not depend on malloc/free.
 *   - Deterministic: allocation/free run in constant time, no fragmentation
 *     within a tier.
 *   - Zero per-block overhead: on free, the pointer is mapped back to its owning
 *     tier by address range; blocks carry no header of their own.
 *   - Exhaustion fallback: when the ideal tier is full, a larger tier is used
 *     automatically.
 *   - Not thread-safe: concurrent access must be locked by the caller (this
 *     library introduces no locks).
 */
#ifndef NX_TIERED_MEM_POOL_H
#define NX_TIERED_MEM_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of tiers a single pool can hold.
 *
 * Define this macro before including this header (or via the build system) to
 * override the default.
 */
#ifndef NX_TIERED_MEM_POOL_MAX_TIERS
#define NX_TIERED_MEM_POOL_MAX_TIERS 4
#endif

/**
 * @brief Return codes for pool operations.
 */
typedef enum {
    NX_TIERED_OK = 0,        /**< Operation succeeded */
    NX_TIERED_ERR_PARAM,     /**< Invalid argument (NULL pointer, etc.) */
    NX_TIERED_ERR_CONFIG,    /**< Invalid tier config: block too small, count is 0, buffer not aligned, or too many tiers */
    NX_TIERED_ERR_NOMEM,     /**< Allocation failed: every large-enough tier is exhausted */
    NX_TIERED_ERR_INVALID    /**< Freed a pointer that this pool does not own */
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
    void    *free_list;       /**< Head of the intrusive singly-linked free list */
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
    size_t  block_size;    /**< Size of a single block in bytes (rounded up to the alignment at init); must be >= sizeof(void*) */
    size_t  block_count;   /**< Number of blocks in this tier; must be > 0 */
} nx_tiered_level_cfg_t;

/**
 * @brief Config for initializing a pool (everything except the pool handle).
 *
 * Declare one of these, fill it (a designated initializer reads nicely and lets
 * you omit forbid_fallback, which then defaults to false), and pass it to
 * nx_tiered_mem_pool_init(). The tier list is embedded inline, so the whole
 * configuration lives in one place - no separate tier array to declare.
 *
 * @code
 *   static _Alignas(max_align_t) uint8_t mem[2048];
 *   nx_tiered_mem_pool_cfg_t cfg = {
 *       .memory      = mem,
 *       .memory_size = sizeof(mem),
 *       .tiers       = { {32, 8}, {128, 4}, {512, 2} },
 *       .tier_count  = 3,
 *   };
 * @endcode
 */
typedef struct {
    void                 *memory;       /**< Caller buffer, must be max_align_t aligned and not NULL */
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
 * The pool carves @c cfg->memory into consecutive regions by each tier's
 * block_size (rounded up to max_align_t alignment) x block_count. Tiers may be
 * given in any order; they are sorted internally by ascending block size.
 *
 * On buffer size: each tier occupies "block_size rounded up to max_align_t
 * alignment" x block_count, and the buffer must be >= the sum over all tiers.
 * Rather than compute that by hand, you can oversize the buffer and read the
 * exact requirement back through @p out_required_bytes: it is written whenever
 * the tier list is valid - including when the buffer is too small (which returns
 * NX_TIERED_ERR_CONFIG) - so you can run once, see the number, then shrink the
 * buffer to fit.
 *
 * @param  pool               Pool handle, must not be NULL.
 * @param  cfg                Configuration, must not be NULL (see nx_tiered_mem_pool_cfg_t).
 * @param  out_required_bytes May be NULL; if non-NULL, receives the total bytes the
 *                            tiers require (valid whenever the tier list is valid).
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool/cfg is NULL, or memory/memory_size/tier_count is zero/NULL;
 *         NX_TIERED_ERR_CONFIG if a tier config is invalid, memory is unaligned,
 *         there are too many tiers, or the buffer is too small.
 */
nx_tiered_ret_t nx_tiered_mem_pool_init(nx_tiered_mem_pool_t           *pool,
                                        const nx_tiered_mem_pool_cfg_t *cfg,
                                        size_t                         *out_required_bytes);

/**
 * @brief  Allocate a block of at least @p size bytes.
 *
 * Takes a block from the smallest tier that is large enough and has a free
 * block; when the ideal tier is exhausted it falls back to a larger tier. If
 * fallback was forbidden at init time (forbid_fallback), then once the ideal
 * tier (and any tier with the same block size) is exhausted it returns NULL
 * instead of borrowing from a larger tier. The returned block is max_align_t
 * aligned. Block contents are uninitialized (like malloc).
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
 *
 * @param  pool Pool handle.
 * @param  ptr  Block to return, or NULL.
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if pool is NULL;
 *         NX_TIERED_ERR_INVALID if ptr is not a block owned by this pool.
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
