/**
 * @file    nx_c_util_examples.c
 * @brief   Example driver: runs each nx-c-util component's examples in turn.
 *
 * The actual examples live in per-component files:
 *   - nx_queue_example.c          -> nx_queue_example_run()
 *   - nx_tiered_mem_pool_example.c  -> nx_tiered_mem_pool_example_run()
 *   - nx_ref_msg_example.c        -> nx_ref_msg_example_run()
 *   - nx_sha256_example.c         -> nx_sha256_example_run()
 */
#include "nx_c_util_examples.h"

#include <stdio.h>

int main(void)
{
    int rc = 0;

    rc |= nx_queue_example_run();
    rc |= nx_tiered_mem_pool_example_run();
    rc |= nx_ref_msg_example_run();
    rc |= nx_sha256_example_run();

    return rc;
}
