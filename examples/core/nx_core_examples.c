/**
 * @file    nx_core_examples.c
 * @brief   Example driver: runs each nx-c-util core component's examples in turn.
 *
 * The actual examples live in per-component files:
 *   - nx_list_example.c           -> nx_list_example_run()
 *   - nx_queue_example.c          -> nx_queue_example_run()
 *   - nx_ringbuf_example.c        -> nx_ringbuf_example_run()
 *   - nx_tiered_mem_pool_example.c  -> nx_tiered_mem_pool_example_run()
 *   - nx_ref_msg_example.c        -> nx_ref_msg_example_run()
 *   - nx_timer_example.c          -> nx_timer_example_run()
 *   - nx_coro_example.c           -> nx_coro_example_run()
 *   - nx_log_example.c            -> nx_log_example_run()
 */
#include "nx_core_examples.h"

#include <stdio.h>

int main(void)
{
    int rc = 0;

    rc |= nx_list_example_run();
    rc |= nx_queue_example_run();
    rc |= nx_ringbuf_example_run();
    rc |= nx_tiered_mem_pool_example_run();
    rc |= nx_ref_msg_example_run();
    rc |= nx_timer_example_run();
    rc |= nx_coro_example_run();
    rc |= nx_log_example_run();

    return rc;
}
