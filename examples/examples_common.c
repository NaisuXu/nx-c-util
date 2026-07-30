/**
 * @file    examples_common.c
 * @brief   Example driver: runs each nx-c-util component's examples in turn.
 *
 * The actual examples live in per-component files:
 *   - nx_list_example.c           -> nx_list_example_run()
 *   - nx_queue_example.c          -> nx_queue_example_run()
 *   - nx_ringbuf_example.c        -> nx_ringbuf_example_run()
 *   - nx_tiered_mem_pool_example.c  -> nx_tiered_mem_pool_example_run()
 *   - nx_ref_msg_example.c        -> nx_ref_msg_example_run()
 *   - nx_timer_example.c          -> nx_timer_example_run()
 *   - nx_can_bus_example.c        -> nx_can_bus_example_run()
 *   - nx_modbus_rtu_example.c     -> nx_modbus_rtu_example_run()
 *   - nx_crc_example.c            -> nx_crc_example_run()
 *   - nx_sha256_example.c         -> nx_sha256_example_run()
 */
#include "examples_common.h"

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
    rc |= nx_can_bus_example_run();
    rc |= nx_modbus_rtu_example_run();
    rc |= nx_crc_example_run();
    rc |= nx_sha256_example_run();

    return rc;
}
