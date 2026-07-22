/**
 * @file    nx_sha256.c
 * @brief   Implementation of SHA-256 (FIPS 180-4).
 *
 * A straightforward, table-free reference implementation: 64-byte blocks are
 * absorbed into eight 32-bit state words via the standard compression function.
 * Partial blocks are staged in the context so data can be fed in arbitrary
 * chunks. Message length is tracked in bits for the final padding.
 */
#include "nx_sha256.h"

#include <string.h>

/* SHA-256 operates on 64-byte blocks. Internal detail, not part of the API. */
#define NX_SHA256_BLOCK_SIZE 64u

/* Rotate a 32-bit value right by n bits (0 < n < 32). */
static uint32_t nx_sha256_rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

/* SHA-256 round constants: first 32 bits of the fractional parts of the cube
 * roots of the first 64 primes. */
static const uint32_t NX_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* Compress one 64-byte block into the eight state words. */
static void nx_sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    /* Prepare the message schedule: first 16 words are the block, big-endian. */
    for (unsigned i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u]     << 24) |
               ((uint32_t)block[i * 4u + 1] << 16) |
               ((uint32_t)block[i * 4u + 2] << 8)  |
               ((uint32_t)block[i * 4u + 3]);
    }
    for (unsigned i = 16u; i < 64u; i++) {
        uint32_t s0 = nx_sha256_rotr(w[i - 15], 7) ^
                      nx_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = nx_sha256_rotr(w[i - 2], 17) ^
                      nx_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (unsigned i = 0; i < 64u; i++) {
        uint32_t s1    = nx_sha256_rotr(e, 6) ^ nx_sha256_rotr(e, 11) ^ nx_sha256_rotr(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + NX_SHA256_K[i] + w[i];
        uint32_t s0    = nx_sha256_rotr(a, 2) ^ nx_sha256_rotr(a, 13) ^ nx_sha256_rotr(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void nx_sha256_init(nx_sha256_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /* Initial hash values: fractional parts of the square roots of the first
     * eight primes. */
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_count  = 0u;
    ctx->buffer_len = 0u;
}

void nx_sha256_update(nx_sha256_ctx_t *ctx, const void *data, size_t length)
{
    if (ctx == NULL || data == NULL) {
        return;
    }

    const uint8_t *p = (const uint8_t *)data;
    ctx->bit_count += (uint64_t)length * 8u;

    /* Top up any partially filled block first. */
    if (ctx->buffer_len > 0u) {
        size_t need = NX_SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t take = (length < need) ? length : need;
        memcpy(ctx->buffer + ctx->buffer_len, p, take);
        ctx->buffer_len += take;
        p      += take;
        length -= take;
        if (ctx->buffer_len == NX_SHA256_BLOCK_SIZE) {
            nx_sha256_transform(ctx->state, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }

    /* Consume as many whole blocks as possible directly from the input. */
    while (length >= NX_SHA256_BLOCK_SIZE) {
        nx_sha256_transform(ctx->state, p);
        p      += NX_SHA256_BLOCK_SIZE;
        length -= NX_SHA256_BLOCK_SIZE;
    }

    /* Stash the remainder for next time. */
    if (length > 0u) {
        memcpy(ctx->buffer, p, length);
        ctx->buffer_len = length;
    }
}

void nx_sha256_final(nx_sha256_ctx_t *ctx, uint8_t digest[NX_SHA256_DIGEST_SIZE])
{
    if (ctx == NULL || digest == NULL) {
        return;
    }

    uint64_t bit_count = ctx->bit_count;

    /* Append the 0x80 byte, then pad with zeros until 8 bytes remain in the
     * final block, then append the 64-bit big-endian bit length. */
    static const uint8_t pad = 0x80u;
    nx_sha256_update(ctx, &pad, 1u);
    /* update() bumped bit_count; restore the true length for the trailer. */
    ctx->bit_count = bit_count;

    static const uint8_t zeros[NX_SHA256_BLOCK_SIZE] = { 0 };
    while (ctx->buffer_len != (NX_SHA256_BLOCK_SIZE - 8u)) {
        nx_sha256_update(ctx, zeros, 1u);
        ctx->bit_count = bit_count;
    }

    uint8_t len_bytes[8];
    for (unsigned i = 0; i < 8u; i++) {
        len_bytes[i] = (uint8_t)(bit_count >> (56u - i * 8u));
    }
    nx_sha256_update(ctx, len_bytes, 8u);

    /* Emit the state, big-endian. */
    for (unsigned i = 0; i < 8u; i++) {
        digest[i * 4u]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4u + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4u + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4u + 3] = (uint8_t)(ctx->state[i]);
    }
}

void nx_sha256(const void *data, size_t length,
               uint8_t digest[NX_SHA256_DIGEST_SIZE])
{
    nx_sha256_ctx_t ctx;
    nx_sha256_init(&ctx);
    nx_sha256_update(&ctx, data, length);
    nx_sha256_final(&ctx, digest);
}
