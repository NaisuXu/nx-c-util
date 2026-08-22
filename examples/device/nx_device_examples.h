/**
 * @file    nx_device_examples.h
 * @brief   Entry points for the per-device example/test routines.
 *
 * Each device driver has its own example file; nx_device_examples.c runs
 * them in turn.
 */
#ifndef NX_DEVICE_EXAMPLES_H
#define NX_DEVICE_EXAMPLES_H

#ifdef __cplusplus
extern "C" {
#endif

/** Run the nx_ws2812 usage examples. Returns 0 on success, non-zero on failure. */
int nx_ws2812_example_run(void);

/** Run the nx_mfrc522 usage examples. Returns 0 on success, non-zero on failure. */
int nx_mfrc522_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_DEVICE_EXAMPLES_H */
