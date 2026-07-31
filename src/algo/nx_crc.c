/**
 * @file    nx_crc.c
 * @brief   Implementation of the nx_crc CRC-8 / CRC-16 / CRC-32 routines.
 *
 * Everything runs on one bit-wise core (the incremental API). The register is
 * held in the internal, non-reflected (MSB-first) domain, right-aligned to the
 * low @c width bits. Rather than reflecting the polynomial, the core optionally
 * reflects each input byte on the way in and reflects the whole register once at
 * the end; this keeps a single code path correct for every refin/refout
 * combination. The one-shot and named functions are thin wrappers over this core.
 */
#include "nx_crc.h"

/* ------------------------------------------------------------------ */
/* Bit-reflection helpers.                                            */
/* ------------------------------------------------------------------ */

/** @brief Reverse the bit order of an 8-bit value. */
static uint8_t nx_crc_reflect8(uint8_t v)
{
    v = (uint8_t)(((v & 0x55u) << 1) | ((v & 0xAAu) >> 1));
    v = (uint8_t)(((v & 0x33u) << 2) | ((v & 0xCCu) >> 2));
    v = (uint8_t)(((v & 0x0Fu) << 4) | ((v & 0xF0u) >> 4));
    return v;
}

/** @brief Reverse the bit order of a 32-bit value. */
static uint32_t nx_crc_reflect32(uint32_t v)
{
    v = ((v & 0x55555555u) << 1)  | ((v & 0xAAAAAAAAu) >> 1);
    v = ((v & 0x33333333u) << 2)  | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x0F0F0F0Fu) << 4)  | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x00FF00FFu) << 8)  | ((v & 0xFF00FF00u) >> 8);
    v = ((v & 0x0000FFFFu) << 16) | ((v & 0xFFFF0000u) >> 16);
    return v;
}

/** @brief Reflect the low @p width bits of @p v (width in 1..32). */
static uint32_t nx_crc_reflect_width(uint32_t v, uint8_t width)
{
    return nx_crc_reflect32(v) >> (32u - width);
}

/** @brief All-ones mask covering the low @p width bits (width in 1..32). */
static uint32_t nx_crc_width_mask(uint8_t width)
{
    return (width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
}

/* ------------------------------------------------------------------ */
/* Incremental (streaming) core.                                      */
/* ------------------------------------------------------------------ */

void nx_crc_init(nx_crc_ctx_t *ctx, uint8_t width,
                 uint32_t poly, uint32_t init,
                 bool refin, bool refout, uint32_t xorout)
{
    if (ctx == NULL) {
        return;
    }
    uint32_t mask = nx_crc_width_mask(width);
    ctx->reg    = init & mask;
    ctx->poly   = poly & mask;
    ctx->xorout = xorout & mask;
    ctx->width  = width;
    ctx->refin  = refin;
    ctx->refout = refout;
}

void nx_crc_update(nx_crc_ctx_t *ctx, const void *data, size_t length)
{
    if (ctx == NULL || data == NULL) {
        return;
    }

    const uint8_t *p       = (const uint8_t *)data;
    const uint8_t  width   = ctx->width;
    const uint32_t poly    = ctx->poly;
    const uint32_t mask    = nx_crc_width_mask(width);
    const uint32_t topbit  = 1u << (width - 1u);
    const unsigned feed    = (unsigned)(width - 8u);   /* left shift to align a byte with the MSBs */
    uint32_t       reg     = ctx->reg;

    for (size_t i = 0; i < length; i++) {
        uint8_t byte = p[i];
        if (ctx->refin) {
            byte = nx_crc_reflect8(byte);
        }
        reg ^= (uint32_t)byte << feed;
        for (int b = 0; b < 8; b++) {
            reg = (reg & topbit) ? (((reg << 1) ^ poly) & mask) : ((reg << 1) & mask);
        }
    }

    ctx->reg = reg;
}

uint32_t nx_crc_final(const nx_crc_ctx_t *ctx)
{
    if (ctx == NULL) {
        return 0u;
    }
    uint32_t reg = ctx->reg;
    if (ctx->refout) {
        reg = nx_crc_reflect_width(reg, ctx->width);
    }
    return (reg ^ ctx->xorout) & nx_crc_width_mask(ctx->width);
}

/* ------------------------------------------------------------------ */
/* Generic, one-shot computation (wrappers over the streaming core).  */
/* ------------------------------------------------------------------ */

uint8_t nx_crc8_compute(const void *data, size_t length,
                        uint8_t poly, uint8_t init,
                        bool refin, bool refout, uint8_t xorout)
{
    nx_crc_ctx_t ctx;
    nx_crc_init(&ctx, 8u, poly, init, refin, refout, xorout);
    nx_crc_update(&ctx, data, length);
    return (uint8_t)nx_crc_final(&ctx);
}

uint16_t nx_crc16_compute(const void *data, size_t length,
                          uint16_t poly, uint16_t init,
                          bool refin, bool refout, uint16_t xorout)
{
    nx_crc_ctx_t ctx;
    nx_crc_init(&ctx, 16u, poly, init, refin, refout, xorout);
    nx_crc_update(&ctx, data, length);
    return (uint16_t)nx_crc_final(&ctx);
}

uint32_t nx_crc32_compute(const void *data, size_t length,
                          uint32_t poly, uint32_t init,
                          bool refin, bool refout, uint32_t xorout)
{
    nx_crc_ctx_t ctx;
    nx_crc_init(&ctx, 32u, poly, init, refin, refout, xorout);
    nx_crc_update(&ctx, data, length);
    return nx_crc_final(&ctx);
}

/* ------------------------------------------------------------------ */
/* Named standard variants.                                           */
/* ------------------------------------------------------------------ */

uint8_t nx_crc8(const void *data, size_t length)
{
    return nx_crc8_compute(data, length, 0x07u, 0x00u, false, false, 0x00u);
}

uint8_t nx_crc8_itu(const void *data, size_t length)
{
    return nx_crc8_compute(data, length, 0x07u, 0x00u, false, false, 0x55u);
}

uint8_t nx_crc8_rohc(const void *data, size_t length)
{
    return nx_crc8_compute(data, length, 0x07u, 0xFFu, true, true, 0x00u);
}

uint8_t nx_crc8_maxim(const void *data, size_t length)
{
    return nx_crc8_compute(data, length, 0x31u, 0x00u, true, true, 0x00u);
}

uint16_t nx_crc16_ibm(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x8005u, 0x0000u, true, true, 0x0000u);
}

uint16_t nx_crc16_maxim(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x8005u, 0x0000u, true, true, 0xFFFFu);
}

uint16_t nx_crc16_usb(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x8005u, 0xFFFFu, true, true, 0xFFFFu);
}

uint16_t nx_crc16_modbus(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x8005u, 0xFFFFu, true, true, 0x0000u);
}

uint16_t nx_crc16_ccitt(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x1021u, 0x0000u, true, true, 0x0000u);
}

uint16_t nx_crc16_ccitt_false(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x1021u, 0xFFFFu, false, false, 0x0000u);
}

uint16_t nx_crc16_x25(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x1021u, 0xFFFFu, true, true, 0xFFFFu);
}

uint16_t nx_crc16_xmodem(const void *data, size_t length)
{
    return nx_crc16_compute(data, length, 0x1021u, 0x0000u, false, false, 0x0000u);
}

uint32_t nx_crc32(const void *data, size_t length)
{
    return nx_crc32_compute(data, length, 0x04C11DB7u, 0xFFFFFFFFu, true, true, 0xFFFFFFFFu);
}

uint32_t nx_crc32_mpeg2(const void *data, size_t length)
{
    return nx_crc32_compute(data, length, 0x04C11DB7u, 0xFFFFFFFFu, false, false, 0x00000000u);
}
