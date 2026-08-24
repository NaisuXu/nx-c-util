/**
 * @file    nx_middleware_examples.h
 * @brief   Entry points for the per-component middleware example routines.
 *
 * Each nx-c-util middleware component has its own example file;
 * nx_middleware_examples.c runs them in turn.
 */
#ifndef NX_MIDDLEWARE_EXAMPLES_H
#define NX_MIDDLEWARE_EXAMPLES_H

#ifdef __cplusplus
extern "C" {
#endif

/** Run the nx_modbus_rtu_slave usage examples. Returns 0 on success, non-zero on failure. */
int nx_modbus_rtu_slave_example_run(void);

/** Run the nx_modbus_rtu_master usage examples. Returns 0 on success, non-zero on failure. */
int nx_modbus_rtu_master_example_run(void);

/** Run the nx_can_isotp usage examples. Returns 0 on success, non-zero on failure. */
int nx_can_isotp_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_MIDDLEWARE_EXAMPLES_H */
