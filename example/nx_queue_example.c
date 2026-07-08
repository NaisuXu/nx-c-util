/**
 * @file    nx_queue_example.c
 * @brief   Usage examples for the nx_queue ring-buffer queue.
 *
 * Demonstrates:
 *   1. A basic FIFO of integers (NX_QUEUE_ON_FULL_REJECT).
 *   2. An overwriting ring buffer of structs (NX_QUEUE_ON_FULL_OVERWRITE),
 *      keeping only the most recent samples.
 *   3. peek / clear / size / capacity helpers.
 *
 * The queue never allocates: every example provides its own static buffer.
 */
#include "nx_c_util_examples.h"
#include "nx_queue.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Example 1: basic integer FIFO, reject-when-full                    */
/* ------------------------------------------------------------------ */
static void example_basic_fifo(void)
{
    printf("=== Example 1: basic integer FIFO (reject when full) ===\n");

    int         storage[4];   /* caller-owned backing storage */
    nx_queue_t  q;

    nx_queue_init(&q, storage, sizeof(int), 4, NX_QUEUE_ON_FULL_REJECT);

    /* Enqueue 0..4; the 5th push is rejected because capacity is 4. */
    for (int i = 0; i < 5; i++) {
        nx_queue_ret_t r = nx_queue_push(&q, &i);
        printf("  push %d -> %s\n", i,
               (r == NX_QUEUE_OK) ? "OK" : "FULL (rejected)");
    }

    printf("  size=%zu capacity=%zu\n",
           nx_queue_size(&q), nx_queue_capacity(&q));

    /* peek the front without removing it. */
    int front;
    if (nx_queue_peek(&q, &front) == NX_QUEUE_OK) {
        printf("  peek front = %d\n", front);
    }

    /* Drain the queue in FIFO order. */
    printf("  drain:");
    int v;
    while (nx_queue_pop(&q, &v) == NX_QUEUE_OK) {
        printf(" %d", v);
    }
    printf("\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: overwriting ring buffer of structs (keep newest)        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t tick;   /* timestamp */
    int16_t  value;  /* sensor reading */
} sample_t;

static void example_overwrite_ring(void)
{
    printf("=== Example 2: overwriting ring buffer (keep newest 3) ===\n");

    sample_t    storage[3];
    nx_queue_t  q;

    nx_queue_init(&q, storage, sizeof(sample_t), 3, NX_QUEUE_ON_FULL_OVERWRITE);

    /* Feed 6 samples into a 3-slot buffer: the oldest are overwritten,
     * so only the last 3 survive. */
    for (uint32_t t = 1; t <= 6; t++) {
        sample_t s = { .tick = t, .value = (int16_t)(t * 10) };
        nx_queue_push(&q, &s);   /* always OK: overwrite policy */
    }

    printf("  retained %zu samples:\n", nx_queue_size(&q));
    sample_t s;
    while (nx_queue_pop(&q, &s) == NX_QUEUE_OK) {
        printf("    tick=%u value=%d\n", s.tick, s.value);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: clear + empty/full state                                */
/* ------------------------------------------------------------------ */
static void example_clear_and_state(void)
{
    printf("=== Example 3: clear and state queries ===\n");

    char        storage[8];   /* a queue of single bytes/chars */
    nx_queue_t  q;

    nx_queue_init(&q, storage, sizeof(char), 8, NX_QUEUE_ON_FULL_REJECT);

    const char *msg = "hello";
    for (const char *p = msg; *p != '\0'; p++) {
        nx_queue_push(&q, p);
    }
    printf("  after pushing \"%s\": size=%zu empty=%d full=%d\n",
           msg, nx_queue_size(&q),
           nx_queue_is_empty(&q), nx_queue_is_full(&q));

    nx_queue_clear(&q);
    printf("  after clear:            size=%zu empty=%d full=%d\n",
           nx_queue_size(&q),
           nx_queue_is_empty(&q), nx_queue_is_full(&q));
    printf("\n");
}

int nx_queue_example_run(void)
{
    printf("########## nx_queue examples ##########\n");
    example_basic_fifo();
    example_overwrite_ring();
    example_clear_and_state();
    return 0;
}
