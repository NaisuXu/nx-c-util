/**
 * @file    nx_c_util_examples.h
 * @brief   Entry points for the per-component example/test routines.
 *
 * Each nx-c-util component has its own example file; nx_c_util_examples.c runs
 * them in turn.
 */
#ifndef NX_C_UTIL_EXAMPLES_H
#define NX_C_UTIL_EXAMPLES_H

/** Run the nx_queue usage examples. Returns 0 on success, non-zero on failure. */
int nx_queue_example_run(void);

/** Run the nx_tiered_mem_pool usage examples. Returns 0 on success, non-zero on failure. */
int nx_tiered_mem_pool_example_run(void);

#endif /* NX_C_UTIL_EXAMPLES_H */
