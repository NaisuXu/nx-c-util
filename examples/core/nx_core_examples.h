/**
 * @file    nx_core_examples.h
 * @brief   Entry points for the per-component example/test routines.
 *
 * Each nx-c-util component has its own example file; nx_core_examples.c runs
 * them in turn.
 */
#ifndef NX_CORE_EXAMPLES_H
#define NX_CORE_EXAMPLES_H

#ifdef __cplusplus
extern "C" {
#endif

/** Run the nx_list usage examples. Returns 0 on success, non-zero on failure. */
int nx_list_example_run(void);

/** Run the nx_queue usage examples. Returns 0 on success, non-zero on failure. */
int nx_queue_example_run(void);

/** Run the nx_ringbuf usage examples. Returns 0 on success, non-zero on failure. */
int nx_ringbuf_example_run(void);

/** Run the nx_tiered_mem_pool usage examples. Returns 0 on success, non-zero on failure. */
int nx_tiered_mem_pool_example_run(void);

/** Run the nx_ref_msg usage examples. Returns 0 on success, non-zero on failure. */
int nx_ref_msg_example_run(void);

/** Run the nx_timer usage examples. Returns 0 on success, non-zero on failure. */
int nx_timer_example_run(void);

/** Run the nx_coro examples and tests. Returns 0 on success, non-zero on failure. */
int nx_coro_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_CORE_EXAMPLES_H */
