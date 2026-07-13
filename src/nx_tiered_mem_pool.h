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
 * @brief  Initialize the pool with one buffer and a set of tier configs.
 *
 * The pool carves @p memory into consecutive regions by each tier's block_size
 * (rounded up to max_align_t alignment) x block_count. Tiers may be passed in
 * any order; they are sorted internally by ascending block size.
 *
 * On buffer size: each tier actually occupies "block_size rounded up to
 * max_align_t alignment" x block_count. The whole buffer must be >= the sum of
 * all tiers' occupancy, otherwise init returns NX_TIERED_ERR_CONFIG. If each
 * block_size is already a multiple of max_align_t (a common practice), the
 * occupancy is just block_size x block_count and can be summed directly;
 * otherwise round up by the alignment first, or simply oversize the buffer.
 *
 * @param  memory      Caller-provided static storage, must be max_align_t
 *                     aligned and not NULL. Declaring it with
 *                     _Alignas(max_align_t) is recommended.
 * @param  memory_size Size of @p memory in bytes; must be >= the total bytes the
 *                     tiers need.
 * @param  pool        Pool handle, must not be NULL.
 * @param  tiers       Array of tier configs, must not be NULL.
 * @param  tier_count  Number of tiers, in [1, NX_TIERED_MEM_POOL_MAX_TIERS].
 * @param  forbid_fallback  Fallback policy on tier exhaustion:
 *                     - false: when an ideal tier fills up, alloc automatically
 *                       falls back to a larger tier;
 *                     - true: allocation is restricted strictly to tiers whose
 *                       block size equals the ideal tier's, never borrowing from
 *                       a larger tier; when those are also exhausted, alloc
 *                       returns NULL.
 *
 * @return NX_TIERED_OK on success;
 *         NX_TIERED_ERR_PARAM if an argument is NULL/zero;
 *         NX_TIERED_ERR_CONFIG if a tier config is invalid, memory is unaligned,
 *         there are too many tiers, or the buffer is too small.
 */
nx_tiered_ret_t nx_tiered_mem_pool_init(nx_tiered_mem_pool_t    *pool,
                                    void                        *memory,
                                    size_t                      memory_size,
                                    const nx_tiered_level_cfg_t *tiers,
                                    size_t                      tier_count,
                                    bool                        forbid_fallback);

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
