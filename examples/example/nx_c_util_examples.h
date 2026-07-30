/**
 * @file    nx_c_util_examples.h
 * @brief   Entry points for the per-component example/test routines.
 *
 * Each nx-c-util component has its own example file; nx_c_util_examples.c runs
 * them in turn.
 */
#ifndef NX_C_UTIL_EXAMPLES_H
#define NX_C_UTIL_EXAMPLES_H

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

/** Run the nx_lock usage examples. Returns 0 on success, non-zero on failure. */
int nx_lock_example_run(void);

/** Run the nx_can_bus usage examples. Returns 0 on success, non-zero on failure. */
int nx_can_bus_example_run(void);

/** Run the nx_modbus_rtu usage examples. Returns 0 on success, non-zero on failure. */
int nx_modbus_rtu_example_run(void);

/** Run the nx_crc usage examples. Returns 0 on success, non-zero on failure. */
int nx_crc_example_run(void);

/** Run the nx_sha256 usage examples. Returns 0 on success, non-zero on failure. */
int nx_sha256_example_run(void);

#endif /* NX_C_UTIL_EXAMPLES_H */
