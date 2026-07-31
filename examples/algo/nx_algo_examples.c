/**
 * @file    nx_algo_examples.c
 * @brief   Example driver: runs each nx-c-util algorithm component's examples in turn.
 *
 * The actual examples live in per-component files:
 *   - nx_crc_example.c            -> nx_crc_example_run()
 *   - nx_sha256_example.c         -> nx_sha256_example_run()
 */
#include "nx_algo_examples.h"

#include <stdio.h>

int main(void) {
    int ret = 0;

    ret |= nx_crc_example_run();
    ret |= nx_sha256_example_run();

    return ret;
}
