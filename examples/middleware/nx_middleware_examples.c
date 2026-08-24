/**
 * @file    nx_middleware_examples.c
 * @brief   Example driver: runs each nx-c-util middleware component's examples.
 *
 * The actual examples live in per-component files:
 *   - nx_modbus_rtu_slave_example.c -> nx_modbus_rtu_slave_example_run()
 *   - nx_modbus_rtu_master_example.c -> nx_modbus_rtu_master_example_run()
 *   - nx_can_isotp_example.c        -> nx_can_isotp_example_run()
 */
#include "nx_middleware_examples.h"

#include <stdio.h>

int main(void)
{
    int rc = 0;

    rc |= nx_modbus_rtu_slave_example_run();
    rc |= nx_modbus_rtu_master_example_run();
    rc |= nx_can_isotp_example_run();

    return rc;
}
