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

/** Run the nx_uds_client usage examples. Returns 0 on success, non-zero on failure. */
int nx_uds_client_example_run(void);

/** Run the nx_uds_server usage examples. Returns 0 on success, non-zero on failure. */
int nx_uds_server_example_run(void);

/**
 * @brief  Runs the always-needed-services example (0x10, 0x11, 0x3E, 0x27).
 * @return 0 on success.
 */
int nx_uds_svc_session_example_run(void);

/**
 * @brief  Runs the memory transfer example (0x34, 0x35, 0x36, 0x37).
 * @return 0 on success.
 */
int nx_uds_svc_transfer_example_run(void);

/**
 * @brief  Runs the transport binding example: UDS over a real ISO-TP path.
 * @return 0 on success.
 */
int nx_uds_tp_bind_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_MIDDLEWARE_EXAMPLES_H */
