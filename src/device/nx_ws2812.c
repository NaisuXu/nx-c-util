/**
 * @file    nx_ws2812.c
 * @brief   Implementation of the nx_ws2812 LED strip driver (no dynamic memory).
 */
#include "nx_ws2812.h"

#include <string.h>

/**
 * Apply the global brightness scale to one channel.
 *
 * (v * (b + 1)) >> 8 is exact at both ends - 255 maps to 255 and 0 maps to 0 -
 * and needs only a multiply and a shift, no division. The pixel buffer is never
 * written back, so this is lossless with respect to the stored color.
 */
static inline uint8_t nx_ws2812_scale(uint8_t v, uint8_t brightness)
{
    if (brightness == 255U) {
        return v;   /* unscaled: skip the arithmetic entirely */
    }
    return (uint8_t)(((uint16_t)v * ((uint16_t)brightness + 1U)) >> 8);
}

bool nx_ws2812_init(nx_ws2812_t           *ws2812,
                    const nx_ws2812_cfg_t *cfg,
                    uint8_t               *pixel_buffer,
                    uint8_t               *tx_buffer)
{
    if (ws2812 == NULL || cfg == NULL || pixel_buffer == NULL || tx_buffer == NULL) {
        return false;
    }
    if (cfg->led_count == 0U || cfg->write_fn == NULL) {
        return false;
    }

    ws2812->cfg        = cfg;
    ws2812->pixels     = pixel_buffer;
    ws2812->tx         = tx_buffer;
    ws2812->brightness = 255U;   /* unscaled until the caller says otherwise */

    memset(pixel_buffer, 0, cfg->led_count * NX_WS2812_BYTES_PER_LED);

    return true;
}

bool nx_ws2812_set_pixel(nx_ws2812_t *ws2812,
                         size_t       index,
                         uint8_t      r,
                         uint8_t      g,
                         uint8_t      b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }
    if (index >= ws2812->cfg->led_count) {
        return false;
    }

    /* WS2812 expects GRB on the wire, so store in that order. */
    uint8_t *px = &ws2812->pixels[index * NX_WS2812_BYTES_PER_LED];
    px[0] = g;
    px[1] = r;
    px[2] = b;

    return true;
}

bool nx_ws2812_fill(nx_ws2812_t *ws2812,
                    size_t       first,
                    size_t       count,
                    uint8_t      r,
                    uint8_t      g,
                    uint8_t      b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }
    if (count == 0U) {
        return true;   /* empty range is a no-op, not an error */
    }

    /* Reject out-of-range without overflowing: first + count could wrap. */
    size_t led_count = ws2812->cfg->led_count;
    if (first >= led_count || count > led_count - first) {
        return false;
    }

    uint8_t *px = &ws2812->pixels[first * NX_WS2812_BYTES_PER_LED];
    for (size_t i = 0U; i < count; i++) {
        *px++ = g;
        *px++ = r;
        *px++ = b;
    }

    return true;
}

bool nx_ws2812_push(nx_ws2812_t *ws2812,
                    size_t       count,
                    uint8_t      r,
                    uint8_t      g,
                    uint8_t      b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }
    if (count == 0U) {
        return true;   /* nothing pushed in, nothing shifted */
    }

    size_t led_count = ws2812->cfg->led_count;

    /* Pushing in at least a full strip's worth shifts everything off the end. */
    if (count >= led_count) {
        return nx_ws2812_fill(ws2812, 0, led_count, r, g, b);
    }

    /* Shift the survivors away from the controller. The regions overlap, so this
     * has to be memmove; the tail that would land past the end is simply not
     * copied, which is how the overflow gets discarded. */
    size_t keep = led_count - count;
    memmove(&ws2812->pixels[count * NX_WS2812_BYTES_PER_LED],
            ws2812->pixels,
            keep * NX_WS2812_BYTES_PER_LED);

    return nx_ws2812_fill(ws2812, 0, count, r, g, b);
}

bool nx_ws2812_push_tail(nx_ws2812_t *ws2812,
                         size_t       count,
                         uint8_t      r,
                         uint8_t      g,
                         uint8_t      b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }
    if (count == 0U) {
        return true;   /* nothing pushed in, nothing shifted */
    }

    size_t led_count = ws2812->cfg->led_count;

    /* Pushing in at least a full strip's worth shifts everything off the head. */
    if (count >= led_count) {
        return nx_ws2812_fill(ws2812, 0, led_count, r, g, b);
    }

    /* Mirror of nx_ws2812_push: shift the survivors toward the controller, so the
     * leading `count` LEDs fall off the head and are discarded. */
    size_t keep = led_count - count;
    memmove(ws2812->pixels,
            &ws2812->pixels[count * NX_WS2812_BYTES_PER_LED],
            keep * NX_WS2812_BYTES_PER_LED);

    return nx_ws2812_fill(ws2812, keep, count, r, g, b);
}

bool nx_ws2812_clear(nx_ws2812_t *ws2812)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }

    memset(ws2812->pixels, 0, ws2812->cfg->led_count * NX_WS2812_BYTES_PER_LED);

    return true;
}

void nx_ws2812_set_brightness(nx_ws2812_t *ws2812, uint8_t brightness)
{
    if (ws2812 == NULL) {
        return;
    }
    ws2812->brightness = brightness;
}

uint8_t nx_ws2812_get_brightness(const nx_ws2812_t *ws2812)
{
    return (ws2812 != NULL) ? ws2812->brightness : 0U;
}

bool nx_ws2812_busy(const nx_ws2812_t *ws2812)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->cfg->busy_fn == NULL) {
        return false;   /* no way to tell, so report idle */
    }
    return ws2812->cfg->busy_fn(ws2812->cfg->busy_arg);
}

bool nx_ws2812_update(nx_ws2812_t *ws2812)
{
    if (ws2812 == NULL || ws2812->cfg == NULL ||
        ws2812->pixels == NULL || ws2812->tx == NULL) {
        return false;
    }

    const nx_ws2812_cfg_t *cfg = ws2812->cfg;

    /* Bail out before writing a single byte: the transfer buffer may still be
     * feeding the peripheral, so re-encoding into it now would corrupt the frame
     * in flight. Leaving it untouched is what makes a retry safe. */
    if (nx_ws2812_busy(ws2812)) {
        return false;
    }

    const uint8_t bit0  = cfg->bit0_pattern;
    const uint8_t bit1  = cfg->bit1_pattern;
    const size_t  n_src = cfg->led_count * NX_WS2812_BYTES_PER_LED;

    /* Expand each data bit to one byte, MSB first within every color byte. */
    uint8_t *out = ws2812->tx;
    for (size_t i = 0U; i < n_src; i++) {
        uint8_t v = nx_ws2812_scale(ws2812->pixels[i], ws2812->brightness);

        for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1) {
            *out++ = (v & mask) ? bit1 : bit0;
        }
    }

    /* Trailing low period latches the frame into the LEDs. */
    if (cfg->reset_bytes > 0U) {
        memset(out, 0, cfg->reset_bytes);
        out += cfg->reset_bytes;
    }

    return cfg->write_fn(ws2812->tx, (size_t)(out - ws2812->tx), cfg->write_arg);
}

bool nx_ws2812_get_pixel(const nx_ws2812_t *ws2812,
                         size_t             index,
                         uint8_t           *out_r,
                         uint8_t           *out_g,
                         uint8_t           *out_b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL || ws2812->pixels == NULL) {
        return false;
    }
    if (index >= ws2812->cfg->led_count) {
        return false;
    }

    const uint8_t *px = &ws2812->pixels[index * NX_WS2812_BYTES_PER_LED];
    if (out_g != NULL) { *out_g = px[0]; }
    if (out_r != NULL) { *out_r = px[1]; }
    if (out_b != NULL) { *out_b = px[2]; }

    return true;
}
