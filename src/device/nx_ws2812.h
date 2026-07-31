/**
 * @file    nx_ws2812.h
 * @brief   WS2812(B) RGB LED strip driver with bit-expansion encoding.
 *
 * This module drives WS2812/WS2812B addressable RGB LEDs using a hardware
 * peripheral (typically SPI, UART, or timer+DMA) in bit-expansion mode: each
 * data bit is encoded as one byte on the wire, where the byte pattern represents
 * the PWM duty cycle for that bit (0 or 1).
 *
 * Design:
 *   - The caller provides the number of LEDs and a write callback that pushes
 *     the encoded byte stream to the hardware.
 *   - nx_ws2812_update builds the bit-expanded stream into a user-provided
 *     buffer and invokes the write callback.
 *   - The caller is responsible for the hardware peripheral (SPI/UART/Timer+DMA)
 *     setup and timing - this module only handles the protocol encoding.
 *
 * Typical flow:
 *   1. User calls nx_ws2812_init with LED count, write callback, and busy check.
 *   2. User sets pixel colors with nx_ws2812_set_pixel or nx_ws2812_set_all.
 *   3. User calls nx_ws2812_update to commit changes to the strip (blocking or
 *      async depending on the callback).
 *
 * Bit encoding:
 *   WS2812 expects a strict timing protocol (+/-150ns tolerance). To generate it
 *   with a fixed-frequency peripheral, each data bit is expanded to one byte whose
 *   high-bit run length sets the high time. The caller supplies both patterns in
 *   nx_ws2812_cfg_t (bit0_pattern / bit1_pattern), so any peripheral, bit order,
 *   and clock can be accommodated; that struct documents reference values and the
 *   usable frequency range.
 *
 * Memory:
 *   This module does not allocate memory. The caller provides:
 *     - A pixel buffer (3 bytes per LED, GRB order) for color state.
 *     - A transfer buffer (sized per NX_WS2812_TX_BUF_SIZE) for the bit-expanded
 *       wire data during updates.
 *
 * Thread safety:
 *   Not thread-safe. Serialize access from multiple contexts yourself.
 */
#ifndef NX_WS2812_H
#define NX_WS2812_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes per LED in the pixel buffer: one each for green, red, blue. */
#define NX_WS2812_BYTES_PER_LED 3U

/**
 * @brief Bytes of pixel buffer needed for @p led_count LEDs.
 *
 * Pass the result (or an array sized with it) to nx_ws2812_init.
 *
 * @code
 * static uint8_t pixels[NX_WS2812_PIXEL_BUF_SIZE(60)];
 * @endcode
 */
#define NX_WS2812_PIXEL_BUF_SIZE(led_count) \
    ((size_t)(led_count) * NX_WS2812_BYTES_PER_LED)

/**
 * @brief Bytes of transfer buffer needed by nx_ws2812_update.
 *
 * Each LED contributes 24 bits and every bit expands to one byte, plus the reset
 * bytes configured in nx_ws2812_cfg_t. Both arguments must match that config.
 *
 * These are macros rather than functions so they work where a constant
 * expression is required - sizing a static array, for instance.
 *
 * @code
 * static uint8_t tx[NX_WS2812_TX_BUF_SIZE(60, 50)];
 * @endcode
 */
#define NX_WS2812_TX_BUF_SIZE(led_count, reset_bytes) \
    ((size_t)(led_count) * NX_WS2812_BYTES_PER_LED * 8U + (size_t)(reset_bytes))

/**
 * @brief WS2812 driver configuration, supplied by the caller at init time.
 *
 * Bit patterns: one data bit becomes one byte, so a pattern's high-bit run sets
 * the high time. WS2812B wants ~0.4us high for a 0 and ~0.8us for a 1, with a
 * ~1.25us total bit period and +/-150ns tolerance - that window is what fixes the
 * usable clock range. Reference values:
 *   - SPI, MSB first @ 2.4-3.6 MHz:                    0xC0 / 0xF8
 *   - UART, LSB first @ 2.4-3.2 Mbaud (8N1, inv. TX):  0x03 / 0x1F
 *
 * write_fn: pushes the encoded stream to the peripheral. Must block until the
 * write is accepted, though transmission itself may continue in the background
 * (DMA). Returns true on success, false on peripheral error.
 *
 * busy_fn: optional; returns true while the previous write is still transmitting.
 * nx_ws2812_update checks it once and returns NX_WS2812_BUSY rather than waiting,
 * and nx_ws2812_busy exposes it so callers can poll on their own terms. NULL means
 * writes are assumed complete when write_fn returns, so the strip never reports
 * busy - correct for blocking transfers, wrong for DMA.
 *
 * reset_bytes: zero bytes appended after the data to hold the line low and latch
 * the frame. WS2812 needs >50us, WS2812B >280us on some revisions - check your
 * datasheet. At one byte per bit-slot, 50 bytes is ~125us at 3.2 MHz, so scale
 * with your clock. 0 is allowed if the gap is guaranteed some other way (an idle
 * period between DMA transfers, for example).
 */
typedef struct {
    size_t  led_count;     /**< Number of LEDs in the strip; must be > 0 */
    size_t  reset_bytes;   /**< Trailing zero bytes that latch the frame */
    uint8_t bit0_pattern;  /**< Byte encoding a "0" bit */
    uint8_t bit1_pattern;  /**< Byte encoding a "1" bit */
    bool (*write_fn)(const uint8_t *data, size_t len, void *arg);  /**< Required */
    bool (*busy_fn)(void *arg);                                    /**< Optional; may be NULL */
    void *write_arg;       /**< User context passed to write_fn */
    void *busy_arg;        /**< User context passed to busy_fn */
} nx_ws2812_cfg_t;

/**
 * @brief WS2812 driver instance.
 *
 * Opaque handle; initialize with nx_ws2812_init.
 */
typedef struct {
    const nx_ws2812_cfg_t *cfg;  /**< Config reference (must outlive this instance) */
    uint8_t *pixels;             /**< Pixel buffer: 3 bytes per LED (GRB order), caller-owned */
    uint8_t *tx;                 /**< Transfer buffer for encoded output, caller-owned */
    uint8_t  brightness;         /**< Global scale applied at encode time; 255 = full */
} nx_ws2812_t;

/**
 * @brief  Initialize a WS2812 driver instance.
 *
 * Both buffers stay owned by the caller and must outlive @p ws2812; the module
 * allocates nothing. Neither may alias the other.
 *
 * @param  ws2812       Driver instance, must not be NULL.
 * @param  cfg          Configuration, must not be NULL and must outlive @p ws2812.
 * @param  pixel_buffer Color state, NX_WS2812_PIXEL_BUF_SIZE(cfg->led_count) bytes.
 * @param  tx_buffer    Encoded output, at least
 *                      NX_WS2812_TX_BUF_SIZE(cfg->led_count, cfg->reset_bytes)
 *                      bytes. nx_ws2812_update rebuilds it on every call, so its
 *                      contents need no initialization and may be treated as
 *                      scratch between updates.
 *
 * Brightness starts at 255 (unscaled) and the pixel buffer is cleared to black.
 *
 * @return true on success, false on invalid arguments (NULL pointers,
 *         led_count == 0, or a NULL write_fn).
 */
bool nx_ws2812_init(nx_ws2812_t           *ws2812,
                    const nx_ws2812_cfg_t *cfg,
                    uint8_t               *pixel_buffer,
                    uint8_t               *tx_buffer);

/**
 * @brief  Set the color of a single LED (0-indexed).
 *
 * Changes take effect on the next nx_ws2812_update call.
 *
 * @param  ws2812 Driver instance.
 * @param  index  LED index (0 .. led_count-1).
 * @param  r      Red intensity (0..255).
 * @param  g      Green intensity (0..255).
 * @param  b      Blue intensity (0..255).
 * @return true on success, false if @p ws2812 is NULL or @p index is out of range.
 */
bool nx_ws2812_set_pixel(nx_ws2812_t *ws2812,
                         size_t       index,
                         uint8_t      r,
                         uint8_t      g,
                         uint8_t      b);

/**
 * @brief  Set a contiguous run of LEDs to the same RGB color.
 *
 * Use for segments, progress bars, or zoned strips. Changes take effect on the
 * next nx_ws2812_update call. nx_ws2812_set_all is the whole-strip shorthand.
 *
 * @param  ws2812 Driver instance.
 * @param  first  Index of the first LED to write (0 .. led_count-1).
 * @param  count  Number of LEDs to write; 0 is a no-op that still returns true.
 * @param  r      Red intensity (0..255).
 * @param  g      Green intensity (0..255).
 * @param  b      Blue intensity (0..255).
 *
 * @return true on success, false if @p ws2812 is NULL or the range
 *         [first, first + count) runs past the end of the strip.
 */
bool nx_ws2812_fill(nx_ws2812_t *ws2812,
                    size_t       first,
                    size_t       count,
                    uint8_t      r,
                    uint8_t      g,
                    uint8_t      b);

/**
 * @brief  Set every LED to the same RGB color (whole-strip nx_ws2812_fill).
 *
 * Changes take effect on the next nx_ws2812_update call.
 *
 * @param  ws2812 Driver instance.
 * @param  r      Red intensity (0..255).
 * @param  g      Green intensity (0..255).
 * @param  b      Blue intensity (0..255).
 * @return true on success, false if @p ws2812 is NULL or uninitialized.
 */
static inline bool nx_ws2812_set_all(nx_ws2812_t *ws2812,
                                     uint8_t      r,
                                     uint8_t      g,
                                     uint8_t      b)
{
    if (ws2812 == NULL || ws2812->cfg == NULL) {
        return false;
    }
    return nx_ws2812_fill(ws2812, 0, ws2812->cfg->led_count, r, g, b);
}

/**
 * @brief  Push @p count LEDs of one color in at the head, shifting the rest along.
 *
 * The strip's existing colors move away from the controller by @p count
 * positions; whatever runs off the far end is discarded. The freed head - index 0
 * onward, the end nearest the controller - is filled with the given color. Call
 * it repeatedly for marquee, comet, and VU-meter style effects.
 *
 * For a 5-LED strip holding [A B C D E], push(2, X) leaves [X X A B C].
 *
 * A @p count at or beyond led_count pushes the whole strip off the end, which is
 * equivalent to nx_ws2812_set_all - overflow is discarded by design, so this is
 * not treated as an out-of-range error.
 *
 * Changes take effect on the next nx_ws2812_update call.
 *
 * @param  ws2812 Driver instance.
 * @param  count  Number of LEDs to insert at the head; 0 is a no-op that still
 *                returns true.
 * @param  r      Red intensity (0..255).
 * @param  g      Green intensity (0..255).
 * @param  b      Blue intensity (0..255).
 *
 * @return true on success, false if @p ws2812 is NULL or uninitialized.
 */
bool nx_ws2812_push(nx_ws2812_t *ws2812,
                    size_t       count,
                    uint8_t      r,
                    uint8_t      g,
                    uint8_t      b);

/**
 * @brief  Mirror of nx_ws2812_push: inserts at the tail, shifting colors toward
 *         the controller and discarding whatever runs off the head.
 *
 * For a 5-LED strip holding [A B C D E], push_tail(2, X) leaves [C D E X X].
 * Same argument and return semantics as nx_ws2812_push.
 */
bool nx_ws2812_push_tail(nx_ws2812_t *ws2812,
                         size_t       count,
                         uint8_t      r,
                         uint8_t      g,
                         uint8_t      b);

/**
 * @brief  Clear all LEDs (set to black / off).
 *
 * Changes take effect on the next nx_ws2812_update call.
 *
 * @param  ws2812 Driver instance.
 * @return true on success, false if @p ws2812 is NULL.
 */
bool nx_ws2812_clear(nx_ws2812_t *ws2812);

/**
 * @brief  Set a global brightness scale applied to every LED.
 *
 * WS2812 has no brightness register, so this is a per-channel multiply. It is
 * applied while encoding inside nx_ws2812_update, NOT stored into the pixel
 * buffer - the colors you set are kept at full resolution, so repeated
 * brightness changes never accumulate rounding error and raising the level
 * again restores the original values exactly.
 *
 * Takes effect on the next nx_ws2812_update call.
 *
 * @param  ws2812     Driver instance; NULL is ignored.
 * @param  brightness 0 = off, 255 = unscaled (the value set by nx_ws2812_init).
 */
void nx_ws2812_set_brightness(nx_ws2812_t *ws2812, uint8_t brightness);

/**
 * @brief  Return the current global brightness scale.
 *
 * @param  ws2812 Driver instance.
 * @return The brightness (0..255), or 0 if @p ws2812 is NULL.
 */
uint8_t nx_ws2812_get_brightness(const nx_ws2812_t *ws2812);

/**
 * @brief  Report whether the peripheral is still transmitting a frame.
 *
 * Serves both ends of a transfer. Before nx_ws2812_update it tells you whether the
 * call would return NX_WS2812_BUSY; after a successful one it tells you whether
 * that frame has finished going out - useful when the write is DMA-backed and you
 * need to know the transfer landed (before cutting power, say).
 *
 * Reflects cfg->busy_fn, so with no busy callback configured this is always false.
 *
 * @param  ws2812 Driver instance.
 * @return true if a transfer is in flight; false if idle, or if @p ws2812 is NULL
 *         or has no busy callback.
 */
bool nx_ws2812_busy(const nx_ws2812_t *ws2812);

/**
 * @brief  Commit pending color changes to the LED strip. Never blocks.
 *
 * Builds the bit-expanded byte stream into the transfer buffer given at init,
 * applying the global brightness scale as it encodes, then hands it to the write
 * callback.
 *
 * If the peripheral is still sending the previous frame this returns false
 * immediately, without touching the transfer buffer - the in-flight data is left
 * intact and the caller can simply try again later. Poll nx_ws2812_busy to find a
 * good moment, or just retry on the next tick.
 *
 * @param  ws2812 Driver instance.
 *
 * @return true if the frame was handed to the peripheral (success or accepted for
 *         DMA); false if the peripheral is busy, if @p ws2812 is NULL or
 *         uninitialized, or if write_fn failed.
 *
 * @note   Returning true means the peripheral accepted the write, not that the
 *         LEDs have latched it; with DMA the transfer continues in the background.
 */
bool nx_ws2812_update(nx_ws2812_t *ws2812);

/**
 * @brief  Get the current RGB value of a single LED.
 *
 * @param  ws2812 Driver instance.
 * @param  index  LED index (0 .. led_count-1).
 * @param  out_r  Receives red intensity; may be NULL.
 * @param  out_g  Receives green intensity; may be NULL.
 * @param  out_b  Receives blue intensity; may be NULL.
 * @return true on success, false if @p ws2812 is NULL or @p index is out of range.
 */
bool nx_ws2812_get_pixel(const nx_ws2812_t *ws2812,
                         size_t             index,
                         uint8_t           *out_r,
                         uint8_t           *out_g,
                         uint8_t           *out_b);

#ifdef __cplusplus
}
#endif

#endif /* NX_WS2812_H */

