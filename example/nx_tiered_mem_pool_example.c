/**
 * @file    nx_tiered_mem_pool_example.c
 * @brief   Usage examples for the nx_tiered_mem_pool hierarchical static memory pool.
 *
 * Demonstrates using the pool as a deterministic replacement for malloc/free:
 *   1. One-time setup from a single caller-owned static buffer.
 *   2. Allocating various sizes and seeing which tier serves each.
 *   3. Fall-back to a larger tier when the ideal one is exhausted.
 *   4. Freeing (by address, no size needed) and reusing blocks.
 *   5. Reading per-tier statistics, including the peak-usage high-water mark.
 *
 * The pool never allocates: all storage is the static g_pool_mem array below.
 */
#include "nx_c_util_examples.h"
#include "nx_tiered_mem_pool.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Pool storage: one whole static block, sized from the tier layout.  */
/* ------------------------------------------------------------------ */
#define TIER_S_SIZE  32    /* small blocks */
#define TIER_S_COUNT 8
#define TIER_M_SIZE  128   /* medium blocks */
#define TIER_M_COUNT 4
#define TIER_L_SIZE  512   /* large blocks */
#define TIER_L_COUNT 2

/* Total bytes = sum of (block_size * block_count) over all tiers. The block
 * sizes here are already multiples of max_align_t's alignment, so no rounding
 * is needed; if yours are not, round each block size up (or just oversize). */
#define POOL_BYTES ( TIER_S_SIZE * TIER_S_COUNT + \
                     TIER_M_SIZE * TIER_M_COUNT + \
                     TIER_L_SIZE * TIER_L_COUNT )

/* Must be aligned to max_align_t so every carved block start is aligned. */
static _Alignas(max_align_t) uint8_t g_pool_mem[POOL_BYTES];

/* Print a one-line summary of every tier's current usage. */
static void dump_stats(const nx_tiered_mem_pool_t *pool)
{
    nx_tiered_pool_stat_t stat;
    nx_tiered_mem_pool_get_stat(pool, &stat);
    for (size_t i = 0; i < stat.tier_count; i++) {
        const nx_tiered_level_stat_t *st = &stat.tiers[i];
        printf("    tier %zu: block=%3zu B  used=%zu/%zu  peak=%zu\n",
               i, st->block_size,
               st->block_count - st->free_count, st->block_count, st->peak_used);
    }
}

int nx_tiered_mem_pool_example_run(void)
{
    nx_tiered_mem_pool_t pool;

    /* ---- 1. setup ---------------------------------------------------- */
    printf("########## nx_tiered_mem_pool examples ##########\n");
    printf("pool buffer = %zu bytes\n\n", (size_t)POOL_BYTES);

    /* The whole configuration lives in one struct; the tier list is embedded. */
    nx_tiered_mem_pool_cfg_t cfg = {
        .memory      = g_pool_mem,
        .memory_size = sizeof(g_pool_mem),
        .tiers       = {
            { TIER_S_SIZE, TIER_S_COUNT },
            { TIER_M_SIZE, TIER_M_COUNT },
            { TIER_L_SIZE, TIER_L_COUNT },
        },
        .tier_count  = 3,
        /* forbid_fallback omitted -> false: allow fallback to a larger tier */
    };

    /* out_required_bytes tells you the exact size the tiers need; oversize the
     * buffer at first, run once, then shrink it to this number. */
    size_t          required = 0;
    nx_tiered_ret_t r = nx_tiered_mem_pool_init(&pool, &cfg, &required);
    if (r != NX_TIERED_OK) {
        printf("pool init failed: %d (tiers need %zu bytes)\n", (int)r, required);
        return 1;
    }
    printf("tiers require %zu of %zu buffer bytes\n\n", required, (size_t)POOL_BYTES);

    /* ---- 2. allocate various sizes ---------------------------------- */
    printf("Example 1: allocate different sizes\n");
    void *p20  = nx_tiered_mem_pool_alloc(&pool, 20);   /* -> 32B tier  */
    void *p100 = nx_tiered_mem_pool_alloc(&pool, 100);  /* -> 128B tier */
    void *p500 = nx_tiered_mem_pool_alloc(&pool, 500);  /* -> 512B tier */
    printf("  alloc(20)=%p  alloc(100)=%p  alloc(500)=%p\n",
           (void *)p20, (void *)p100, (void *)p500);

    /* The blocks are real, writable memory. */
    strcpy((char *)p20, "hello pool");
    printf("  wrote to alloc(20): \"%s\"\n", (char *)p20);
    dump_stats(&pool);
    printf("\n");

    /* ---- 3. fall-back to a larger tier ------------------------------ */
    printf("Example 2: exhaust the 32B tier, then borrow from a larger tier\n");
    void *small[TIER_S_COUNT];
    int   got = 0;
    /* p20 already took one 32B block; grab the remaining ones to empty the tier. */
    for (int i = 0; i < TIER_S_COUNT - 1; i++) {
        small[i] = nx_tiered_mem_pool_alloc(&pool, 16);
        if (small[i] != NULL) {
            got++;
        }
    }
    printf("  grabbed %d more 32B-tier blocks (tier now empty)\n", got);

    void *borrowed = nx_tiered_mem_pool_alloc(&pool, 16);   /* 32B tier empty */
    printf("  alloc(16) after exhaustion = %p (borrowed from a larger tier)\n",
           (void *)borrowed);
    dump_stats(&pool);
    printf("\n");

    /* ---- 4. free and reuse ------------------------------------------ */
    printf("Example 3: free returns blocks to the pool for reuse\n");
    nx_tiered_mem_pool_free(&pool, p100);
    void *again = nx_tiered_mem_pool_alloc(&pool, 100);
    printf("  freed p100=%p, re-alloc(100)=%p (same block reused: %s)\n",
           (void *)p100, (void *)again, (p100 == again) ? "yes" : "no");

    /* free(NULL) is a harmless no-op, like standard free(). */
    nx_tiered_mem_pool_free(&pool, NULL);

    /* Freeing a pointer the pool does not own is detected and reported. */
    int stack_var = 0;
    nx_tiered_ret_t bad = nx_tiered_mem_pool_free(&pool, &stack_var);
    printf("  free(&stack_var) -> %s\n",
           (bad == NX_TIERED_ERR_INVALID) ? "NX_TIERED_ERR_INVALID (rejected)" : "?");
    printf("\n");

    /* ---- 5. final stats --------------------------------------------- */
    printf("Final pool stats (note the peak high-water marks):\n");
    dump_stats(&pool);

    printf("\n");

    return 0;
}
