/**
 * @file    nx_crc.h
 * @brief   CRC-8 / CRC-16 / CRC-32 checksum routines, in pure C.
 *
 * Provides three layers, from most convenient to most flexible:
 *   - Named wrappers for common, standard CRC variants (CRC-8, CRC-16/MODBUS,
 *     CRC-32, ...), each documented with its exact parameters and its check
 *     value (the CRC of the ASCII string "123456789").
 *   - Generic, one-shot computation functions (@c nx_crc8_compute,
 *     @c nx_crc16_compute, @c nx_crc32_compute) driven by the Rocksoft model
 *     parameters (polynomial, initial value, input/output reflection, final XOR).
 *     Use these for any variant, including ones not listed below.
 *   - An incremental (streaming) API (@c nx_crc_ctx_t + @c nx_crc_init /
 *     @c nx_crc_update / @c nx_crc_final) for data that arrives in pieces; a
 *     chunked computation yields exactly the same result as the one-shot call.
 *
 * The implementation is bit-wise (no lookup tables), so it needs no table
 * storage and is fully deterministic. Storage is entirely caller-owned; the
 * library uses no dynamic memory.
 *
 * @note All routines are NULL-safe: a NULL data pointer contributes no bytes
 *       (it is treated as a zero-length buffer) rather than dereferencing, and a
 *       NULL context is a harmless no-op. Passing a non-NULL @c data with a
 *       @c length that overruns the buffer is still the caller's responsibility.
 */
#ifndef NX_CRC_H
#define NX_CRC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Named standard variants.                                           */
/* Each @c check value is the CRC of the ASCII string "123456789".    */
/* ------------------------------------------------------------------ */

/** CRC-8:        poly 0x07, init 0x00, refin false, refout false, xorout 0x00; check 0xF4. */
uint8_t nx_crc8(const void *data, size_t length);
/** CRC-8/ITU:    poly 0x07, init 0x00, refin false, refout false, xorout 0x55; check 0xA1. */
uint8_t nx_crc8_itu(const void *data, size_t length);
/** CRC-8/ROHC:   poly 0x07, init 0xFF, refin true,  refout true,  xorout 0x00; check 0xD0. */
uint8_t nx_crc8_rohc(const void *data, size_t length);
/** CRC-8/MAXIM:  poly 0x31, init 0x00, refin true,  refout true,  xorout 0x00; check 0xA1. */
uint8_t nx_crc8_maxim(const void *data, size_t length);

/** CRC-16/IBM (ARC):    poly 0x8005, init 0x0000, refin true,  refout true,  xorout 0x0000; check 0xBB3D. */
uint16_t nx_crc16_ibm(const void *data, size_t length);
/** CRC-16/MAXIM:        poly 0x8005, init 0x0000, refin true,  refout true,  xorout 0xFFFF; check 0x44C2. */
uint16_t nx_crc16_maxim(const void *data, size_t length);
/** CRC-16/USB:          poly 0x8005, init 0xFFFF, refin true,  refout true,  xorout 0xFFFF; check 0xB4C8. */
uint16_t nx_crc16_usb(const void *data, size_t length);
/** CRC-16/MODBUS:       poly 0x8005, init 0xFFFF, refin true,  refout true,  xorout 0x0000; check 0x4B37. */
uint16_t nx_crc16_modbus(const void *data, size_t length);
/** CRC-16/CCITT (KERMIT): poly 0x1021, init 0x0000, refin true,  refout true,  xorout 0x0000; check 0x2189. */
uint16_t nx_crc16_ccitt(const void *data, size_t length);
/** CRC-16/CCITT-FALSE:  poly 0x1021, init 0xFFFF, refin false, refout false, xorout 0x0000; check 0x29B1. */
uint16_t nx_crc16_ccitt_false(const void *data, size_t length);
/** CRC-16/X25:          poly 0x1021, init 0xFFFF, refin true,  refout true,  xorout 0xFFFF; check 0x906E. */
uint16_t nx_crc16_x25(const void *data, size_t length);
/** CRC-16/XMODEM:       poly 0x1021, init 0x0000, refin false, refout false, xorout 0x0000; check 0x31C3. */
uint16_t nx_crc16_xmodem(const void *data, size_t length);

/** CRC-32:        poly 0x04C11DB7, init 0xFFFFFFFF, refin true,  refout true,  xorout 0xFFFFFFFF; check 0xCBF43926. */
uint32_t nx_crc32(const void *data, size_t length);
/** CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, refin false, refout false, xorout 0x00000000; check 0x0376E6E7. */
uint32_t nx_crc32_mpeg2(const void *data, size_t length);

/* ------------------------------------------------------------------ */
/* Generic, one-shot computation (Rocksoft model).                    */
/* ------------------------------------------------------------------ */

/**
 * @brief  Compute an 8-bit CRC over a whole buffer using explicit parameters.
 *
 * @param  data    Input bytes; may be NULL (treated as zero-length).
 * @param  length  Number of input bytes.
 * @param  poly    Generator polynomial (normal, non-reflected form).
 * @param  init    Initial CRC register value.
 * @param  refin   Reflect each input byte (LSB first) before processing.
 * @param  refout  Reflect the CRC register before the final XOR.
 * @param  xorout  Value XORed into the CRC to produce the result.
 *
 * @return The 8-bit CRC.
 */
uint8_t nx_crc8_compute(const void *data, size_t length,
                        uint8_t poly, uint8_t init,
                        bool refin, bool refout, uint8_t xorout);

/** @brief Compute a 16-bit CRC over a whole buffer. See nx_crc8_compute(). */
uint16_t nx_crc16_compute(const void *data, size_t length,
                          uint16_t poly, uint16_t init,
                          bool refin, bool refout, uint16_t xorout);

/** @brief Compute a 32-bit CRC over a whole buffer. See nx_crc8_compute(). */
uint32_t nx_crc32_compute(const void *data, size_t length,
                          uint32_t poly, uint32_t init,
                          bool refin, bool refout, uint32_t xorout);

/* ------------------------------------------------------------------ */
/* Incremental (streaming) computation.                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Running state for an incremental CRC computation.
 *
 * Holds the parameters and the running register so a CRC can be built up over
 * several @c nx_crc_update() calls. The register is kept in the internal
 * (non-reflected) domain; output reflection and the final XOR are applied only
 * by @c nx_crc_final(), so feeding data in chunks gives the same result as one
 * one-shot call.
 *
 * @note The members are an implementation detail; do not access them directly.
 *       Set the context up with @c nx_crc_init().
 */
typedef struct {
    uint32_t reg;      /**< Running register (low @c width bits, non-reflected domain) */
    uint32_t poly;     /**< Generator polynomial (normal form) */
    uint32_t xorout;   /**< Value XORed in by nx_crc_final() */
    uint8_t  width;    /**< CRC width in bits: 8, 16, or 32 */
    bool     refin;    /**< Reflect each input byte before processing */
    bool     refout;   /**< Reflect the register in nx_crc_final() */
} nx_crc_ctx_t;

/**
 * @brief  Initialize an incremental CRC context with explicit model parameters.
 *
 * @param  ctx     Context to initialize; a NULL @p ctx is a no-op.
 * @param  width   CRC width in bits; must be 8, 16, or 32.
 * @param  poly    Generator polynomial (normal, non-reflected form).
 * @param  init    Initial CRC register value.
 * @param  refin   Reflect each input byte (LSB first) before processing.
 * @param  refout  Reflect the CRC register before the final XOR.
 * @param  xorout  Value XORed into the CRC to produce the result.
 */
void nx_crc_init(nx_crc_ctx_t *ctx, uint8_t width,
                 uint32_t poly, uint32_t init,
                 bool refin, bool refout, uint32_t xorout);

/**
 * @brief  Feed a chunk of data into an incremental CRC computation.
 *
 * May be called any number of times between @c nx_crc_init() and
 * @c nx_crc_final(). A NULL @p ctx or a NULL @p data contributes nothing.
 *
 * @param  ctx     Context previously set up with nx_crc_init().
 * @param  data    Input bytes; may be NULL (treated as zero-length).
 * @param  length  Number of input bytes in this chunk.
 */
void nx_crc_update(nx_crc_ctx_t *ctx, const void *data, size_t length);

/**
 * @brief  Finish an incremental CRC and return the result.
 *
 * Applies output reflection and the final XOR to the running register. Does not
 * modify @p ctx, so it may be called more than once; continue calling
 * @c nx_crc_update() afterwards to extend the same computation.
 *
 * @param  ctx  Context previously set up with nx_crc_init().
 * @return The CRC (in the low @c width bits); 0 if @p ctx is NULL.
 */
uint32_t nx_crc_final(const nx_crc_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NX_CRC_H */
