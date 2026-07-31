/**
 * @file    nx_algo_examples.h
 * @brief   Entry points for the algorithm module example/test routines.
 *
 * Each nx-c-util algorithm component has its own example file; nx_algo_examples.c runs
 * them in turn.
 */
#ifndef NX_ALGO_EXAMPLES_H
#define NX_ALGO_EXAMPLES_H

#ifdef __cplusplus
extern "C" {
#endif

/** Run the nx_crc usage examples. Returns 0 on success, non-zero on failure. */
int nx_crc_example_run(void);

/** Run the nx_sha256 usage examples. Returns 0 on success, non-zero on failure. */
int nx_sha256_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_ALGO_EXAMPLES_H */
