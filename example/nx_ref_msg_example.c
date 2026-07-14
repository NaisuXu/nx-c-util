/**
 * @file    nx_ref_msg_example.c
 * @brief   Usage examples for nx_ref_msg (reference-counted zero-copy messages).
 *
 * Demonstrates:
 *   1. Allocating a message from a tiered pool and filling its data.
 *   2. Publishing one message to multiple queues in one call (zero copy: every
 *      queue receives the same pointer), and the producer-reference convention.
 *   3. Consumers popping the shared message and releasing their reference; the
 *      block returns to the pool only when the last reference is gone.
 *   4. A NULL entry in the queue array being skipped.
 *
 * All storage is static: the pool buffer and every queue buffer live below.
 * The example self-checks with a few asserts and prints what happens.
 */
#include "nx_c_util_examples.h"
#include "nx_ref_msg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Static storage: one pool + three consumer queues.                  */
/* ------------------------------------------------------------------ */
#define POOL_BLK   64          /* block size; fits msg header + small payload */
#define POOL_NBLK  8           /* number of blocks */
static _Alignas(max_align_t) uint8_t g_pool_mem[POOL_BLK * POOL_NBLK];

#define QCAP 4                 /* capacity of each consumer queue */
static nx_ref_msg_t *g_qbuf_a[QCAP];
static nx_ref_msg_t *g_qbuf_b[QCAP];
static nx_ref_msg_t *g_qbuf_c[QCAP];

/* Sum of free blocks across all tiers (used to prove memory is reclaimed). */
static size_t pool_free_blocks(nx_tiered_mem_pool_t *pool)
{
    nx_tiered_pool_stat_t st;
    nx_tiered_mem_pool_get_stat(pool, &st);
    size_t f = 0;
    for (size_t i = 0; i < st.tier_count; i++) {
        f += st.tiers[i].free_count;
    }
    return f;
}

int nx_ref_msg_example_run(void)
{
    printf("########## nx_ref_msg examples ##########\n");

    /* ---- setup: a single-tier pool + three queues ---- */
    nx_tiered_mem_pool_t pool;
    nx_tiered_mem_pool_cfg_t cfg = {
        .memory      = g_pool_mem,
        .memory_size = sizeof(g_pool_mem),
        .tiers       = { { POOL_BLK, POOL_NBLK } },
        .tier_count  = 1,
    };
    if (nx_tiered_mem_pool_init(&pool, &cfg, NULL) != NX_TIERED_OK) {
        printf("pool init failed\n");
        return 1;
    }

    nx_queue_t qa, qb, qc;
    nx_ref_msg_queue_init(&qa, g_qbuf_a, QCAP);
    nx_ref_msg_queue_init(&qb, g_qbuf_b, QCAP);
    nx_ref_msg_queue_init(&qc, g_qbuf_c, QCAP);

    /* ---- 1. producer allocates and fills a message ---- */
    printf("Example 1: allocate a message and fill it\n");
    nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, 16);
    assert(m != NULL);
    strcpy((char *)nx_ref_msg_data(m), "hello world");
    printf("  data=\"%s\" len=%zu refcount=%zu (producer)\n",
           (char *)nx_ref_msg_data(m), nx_ref_msg_len(m), nx_ref_msg_refcount(m));
    assert(nx_ref_msg_refcount(m) == 1);
    printf("\n");

    /* ---- 2. publish to multiple queues at once ---- */
    printf("Example 2: publish to 3 queues in one call (zero copy)\n");
    /* NULL-terminated array: the trailing NULL marks the end of the list. */
    nx_queue_t *group[] = { &qa, &qb, &qc, NULL };
    size_t delivered = 0;
    nx_ref_msg_publish_multi(m, group, &delivered);
    printf("  delivered=%zu, refcount=%zu (1 producer + %zu queues)\n",
           delivered, nx_ref_msg_refcount(m), delivered);
    assert(delivered == 3);
    assert(nx_ref_msg_refcount(m) == 4);

    /* Producer is done: give up its own reference. */
    nx_ref_msg_release(m);
    printf("  producer released -> refcount=%zu\n", nx_ref_msg_refcount(m));
    assert(nx_ref_msg_refcount(m) == 3);
    printf("\n");

    /* ---- 3. consumers pop the shared message and release ---- */
    printf("Example 3: consumers pop (same pointer) and release\n");
    nx_queue_t *queues[] = { &qa, &qb, &qc };
    for (int i = 0; i < 3; i++) {
        nx_ref_msg_t *got = NULL;
        nx_queue_pop(queues[i], &got);
        assert(got == m);   /* every queue got the very same object: zero copy */
        printf("  consumer %d: data=\"%s\" refcount(before release)=%zu\n",
               i, (char *)nx_ref_msg_data(got), nx_ref_msg_refcount(got));
        nx_ref_msg_release(got);
    }

    /* Last reference gone -> whole block returned to the pool. */
    printf("  all released; pool free blocks = %zu/%d\n",
           pool_free_blocks(&pool), POOL_NBLK);
    assert(pool_free_blocks(&pool) == POOL_NBLK);
    printf("\n");

    /* ---- 4. a message delivered to nobody still frees cleanly ---- */
    printf("Example 4: publish to zero queues, no leak\n");
    nx_ref_msg_t *m2 = nx_ref_msg_alloc(&pool, 8);
    nx_queue_t *none[] = { NULL };   /* immediately NULL: an empty list */
    nx_ref_msg_publish_multi(m2, none, &delivered);
    printf("  delivered=%zu, refcount=%zu\n", delivered, nx_ref_msg_refcount(m2));
    assert(delivered == 0 && nx_ref_msg_refcount(m2) == 1);
    nx_ref_msg_release(m2);   /* producer reference -> 0, freed */
    assert(pool_free_blocks(&pool) == POOL_NBLK);
    printf("  producer released; pool fully reclaimed (%zu/%d)\n",
           pool_free_blocks(&pool), POOL_NBLK);
    printf("\n");
    
    return 0;
}
