/**
 * @file    nx_ref_msg_example.c
 * @brief   Usage examples for nx_ref_msg (reference-counted zero-copy messages).
 *
 * Demonstrates a realistic producer/consumer flow:
 *   1. A producer allocates a message from a tiered pool and fills a structured
 *      payload (a sensor reading).
 *   2. It publishes that one message to several queues in a single call (zero
 *      copy - every queue holds the same object), then releases its own reference.
 *   3. Each consumer module drains its own queue, reads the payload by its type,
 *      "processes" it, and releases; the block returns to the pool only after the
 *      last reference (producer + all consumers) is gone.
 *   4. A message delivered to no queue is still freed cleanly, with no leak.
 *
 * All storage is static: the pool buffer and every queue buffer live below.
 * The example self-checks with a few asserts and prints what happens.
 */
#include "nx_c_util_examples.h"
#include "nx_ref_msg.h"

#include <assert.h>
#include <stdio.h>

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

/* A structured payload, as a real producer would send (not a bare string). */
typedef struct {
    uint32_t sensor_id;
    int32_t  value;
} sensor_reading_t;

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

/* One consumer module: pop from its own queue, use the data, then release.
 * This is the real usage pattern - the consumer neither knows nor cares that
 * the message is shared; it just reads what it needs and drops its reference. */
static void consumer_drain(const char *name, nx_queue_t *q)
{
    nx_ref_msg_t *msg = NULL;
    while (nx_queue_pop(q, &msg) == NX_QUEUE_OK) {
        /* Interpret the payload by its type and "process" it. */
        const sensor_reading_t *r = (const sensor_reading_t *)nx_ref_msg_data(msg);
        printf("  [%s] got sensor #%u = %d  (refcount=%zu, releasing)\n",
               name, r->sensor_id, r->value, nx_ref_msg_refcount(msg));

        /* Done with it: release this consumer's reference. The message frees
         * itself once every consumer (and the producer) has released. */
        nx_ref_msg_release(msg);
    }
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
    nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, sizeof(sensor_reading_t));
    assert(m != NULL);
    /* Fill the payload as a producer would (a sensor reading). */
    sensor_reading_t *reading = (sensor_reading_t *)nx_ref_msg_data(m);
    reading->sensor_id = 7;
    reading->value     = 42;
    printf("  sensor #%u = %d, len=%zu refcount=%zu (producer)\n",
           reading->sensor_id, reading->value,
           nx_ref_msg_len(m), nx_ref_msg_refcount(m));
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

    /* ---- 3. each consumer drains its own queue, uses data, releases ---- */
    printf("Example 3: consumers read their queues and release\n");
    /* Three independent modules, each with its own queue. Each reads the shared
     * reading (zero copy) and releases when done - exactly how it works in a
     * real system. Only after the last release does the block return to the pool. */
    consumer_drain("logger",  &qa);
    consumer_drain("display", &qb);
    consumer_drain("uploader", &qc);

    /* Last reference gone -> whole block returned to the pool. */
    printf("  all consumers done; pool free blocks = %zu/%d\n",
           pool_free_blocks(&pool), POOL_NBLK);
    assert(pool_free_blocks(&pool) == POOL_NBLK);
    printf("\n");

    /* ---- 4. a message delivered to nobody still frees cleanly ---- */
    printf("Example 4: publish to zero queues, no leak\n");
    nx_ref_msg_t *m2 = nx_ref_msg_alloc(&pool, sizeof(sensor_reading_t));
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
