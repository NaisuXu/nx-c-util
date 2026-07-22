/**
 * @file    nx_sha256.h
 * @brief   SHA-256 cryptographic hash (FIPS 180-4), in pure C.
 *
 * Provides two ways to compute a SHA-256 digest:
 *   - A one-shot helper (@c nx_sha256) that hashes a whole buffer in one call.
 *   - An incremental (streaming) API (@c nx_sha256_ctx_t + @c nx_sha256_init /
 *     @c nx_sha256_update / @c nx_sha256_final) for data that arrives in pieces;
 *     a chunked computation yields exactly the same digest as the one-shot call.
 *
 * The digest is 32 bytes (256 bits), output in big-endian byte order as defined
 * by the standard. Storage is entirely caller-owned; the library uses no dynamic
 * memory and is fully deterministic.
 *
 * @note This is a plain hash, not a keyed MAC. For message authentication build
 *       HMAC-SHA256 on top of it.
 * @note All routines are NULL-safe: a NULL data pointer contributes no bytes and
 *       a NULL context is a harmless no-op. Passing a non-NULL @c data with a
 *       @c length that overruns the buffer is still the caller's responsibility.
 */
#ifndef NX_SHA256_H
#define NX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SHA-256 digest size in bytes. */
#define NX_SHA256_DIGEST_SIZE 32u

/**
 * @brief Running state for an incremental SHA-256 computation.
 *
 * @note The members are implementation details; do not access them directly.
 *       Create one on the stack and drive it through the API.
 */
typedef struct {
    uint32_t state[8];    /**< Eight 32-bit working hash words */
    uint64_t bit_count;   /**< Total message length in bits */
    uint8_t  buffer[64];  /**< Partial-block staging buffer (one 64-byte block) */
    size_t   buffer_len;  /**< Bytes currently held in buffer */
} nx_sha256_ctx_t;

/**
 * @brief  Begin a new SHA-256 computation.
 *
 * @param  ctx Context to initialize; NULL is a no-op.
 */
void nx_sha256_init(nx_sha256_ctx_t *ctx);

/**
 * @brief  Feed more data into an in-progress SHA-256 computation.
 *
 * May be called any number of times; the result is identical to hashing the
 * concatenation of all chunks in one call.
 *
 * @param  ctx    Context previously passed to nx_sha256_init(); NULL is a no-op.
 * @param  data   Input bytes; may be NULL (treated as zero-length).
 * @param  length Number of input bytes.
 */
void nx_sha256_update(nx_sha256_ctx_t *ctx, const void *data, size_t length);

/**
 * @brief  Finish the computation and write the 32-byte digest.
 *
 * The context must not be reused after this without a fresh nx_sha256_init().
 *
 * @param  ctx    Context to finalize; if NULL, the function does nothing.
 * @param  digest Output buffer of at least NX_SHA256_DIGEST_SIZE bytes; if NULL,
 *                the function does nothing.
 */
void nx_sha256_final(nx_sha256_ctx_t *ctx, uint8_t digest[NX_SHA256_DIGEST_SIZE]);

/**
 * @brief  Compute the SHA-256 digest of a whole buffer in one call.
 *
 * Equivalent to init + a single update + final.
 *
 * @param  data   Input bytes; may be NULL (treated as zero-length).
 * @param  length Number of input bytes.
 * @param  digest Output buffer of at least NX_SHA256_DIGEST_SIZE bytes; if NULL,
 *                the function does nothing.
 */
void nx_sha256(const void *data, size_t length,
               uint8_t digest[NX_SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NX_SHA256_H */
