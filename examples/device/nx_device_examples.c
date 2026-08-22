/**
 * @file    nx_device_examples.c
 * @brief   Example driver: runs each device driver's examples in turn.
 *
 * The actual examples live in per-device files:
 *   - nx_ws2812_example.c -> nx_ws2812_example_run()
 *   - nx_mfrc522_example.c -> nx_mfrc522_example_run()
 */
#include "nx_device_examples.h"

#include <stdio.h>

int main(void)
{
    int rc = 0;

    rc |= nx_ws2812_example_run();
    rc |= nx_mfrc522_example_run();

    return rc;
}
