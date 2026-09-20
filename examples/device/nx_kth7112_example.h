/**
 * @file    nx_kth7112_example.h
 * @brief   Declarations for the KTH7112 driver example.
 */
#ifndef NX_KTH7112_EXAMPLE_H
#define NX_KTH7112_EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Run the KTH7112 example: a fake SPI slave and its self-checks.
 *
 * @return 0 on success. Any failed check aborts the process through assert.
 */
int nx_kth7112_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_KTH7112_EXAMPLE_H */
