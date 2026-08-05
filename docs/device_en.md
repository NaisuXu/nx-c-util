# Device Modules

## nx_ws2812 — WS2812(B) RGB LED strip driver

A driver for WS2812/WS2812B addressable RGB LEDs that handles only the protocol
encoding: the caller owns the hardware peripheral (SPI, UART, or timer+DMA) and
supplies a write callback, while the module turns pixel colors into the exact
byte stream that peripheral must send. No dynamic memory.

- **Bit-expansion encoding** — WS2812 wants a strict timing protocol (~1.25us bit
  period, ±150ns tolerance) that no UART/SPI clock hits directly. So each data bit
  is expanded to one byte on the wire whose high-bit run length sets the high time.
  The caller supplies both byte patterns in the config (`bit0_pattern` /
  `bit1_pattern`), so any peripheral, clock, and bit order works — reference values:
  SPI MSB-first @ 2.4–3.6 MHz is `0xC0` / `0xF8`; UART LSB-first (8N1, inverted TX)
  @ 2.4–3.2 Mbaud is `0x03` / `0x1F`.
- **Zero allocation, caller-owned buffers** — two buffers, both sized by macros so
  they can size static arrays: a pixel buffer (`NX_WS2812_PIXEL_BUF_SIZE`, 3 bytes
  per LED in GRB order) holding the color state, and a transfer buffer
  (`NX_WS2812_TX_BUF_SIZE`, 24 bytes per LED plus the reset bytes) that
  `nx_ws2812_update` rebuilds on every call and may otherwise be treated as scratch.
- **Non-blocking update with a busy handshake** — `nx_ws2812_update` never blocks.
  If the peripheral is still sending the previous frame it returns `false`
  immediately without touching the transfer buffer, leaving the in-flight data
  intact so a retry is safe. The optional `is_busy` callback drives this and is
  also exposed as `nx_ws2812_busy`, so a caller polls on its own terms instead of
  spinning inside the driver — the fit for a DMA-backed transfer. A NULL `is_busy`
  means writes are assumed complete when `write` returns (correct for blocking
  transfers).
- **Lossless global brightness** — WS2812 has no brightness register, so brightness
  is a per-channel multiply applied while encoding inside `nx_ws2812_update`, never
  written back into the pixel buffer. The colors you set stay at full resolution, so
  dimming and raising the level again restores the originals exactly, with no
  accumulated rounding error.
- **Pixel operations** — `set_pixel` / `fill` / `set_all` set colors; `get_pixel`
  reads them back; `clear` blacks the strip; `push` / `push_tail` shift the strip
  one way or the other and feed a new color in at the freed end, discarding whatever
  runs off — the primitive for marquee, comet, and VU-meter effects.
- **Single serial context** — the `write` and `is_busy` callbacks drive the same
  peripheral, so they share one `io_ctx` passed as their first argument.
- **Not thread-safe** — serialize access from multiple contexts yourself.

```c
#include "nx_ws2812.h"

#define LED_COUNT   60u
#define RESET_BYTES 50u          /* trailing low period that latches the frame */

/* SPI, MSB-first @ ~3.2 MHz: one data bit -> one byte on the wire. */
#define BIT0 0xC0u               /* ~0.4us high = a "0" bit */
#define BIT1 0xF8u               /* ~0.8us high = a "1" bit */

/* caller-owned storage; the driver allocates nothing. Both sizes are macros, so
 * they work where a constant expression is required (a static array here). */
static uint8_t pixels[NX_WS2812_PIXEL_BUF_SIZE(LED_COUNT)];
static uint8_t tx[NX_WS2812_TX_BUF_SIZE(LED_COUNT, RESET_BYTES)];

static const nx_ws2812_cfg_t cfg = {
    .led_count    = LED_COUNT,
    .reset_bytes  = RESET_BYTES,
    .bit0_pattern = BIT0,
    .bit1_pattern = BIT1,
    .write        = spi_write,   /* pushes the encoded stream to the peripheral */
    .is_busy      = spi_busy,    /* NULL if write blocks until done (no DMA)     */
    .io_ctx       = &spi,        /* passed to write / is_busy                    */
};

nx_ws2812_t strip;
nx_ws2812_init(&strip, &cfg, pixels, tx);

nx_ws2812_set_pixel(&strip, 0, 255, 0, 0);   /* LED 0 red                       */
nx_ws2812_set_brightness(&strip, 128);       /* half brightness, applied at encode */

/* Non-blocking: refuses (returns false) if a prior frame is still on the wire. */
if (!nx_ws2812_update(&strip)) {
    /* peripheral busy or IO error; retry next tick, or poll nx_ws2812_busy */
}
```

> **Note:** `nx_ws2812_update` returning `true` means the peripheral *accepted* the
> write, not that the LEDs have latched it — with DMA the transfer continues in the
> background, and `nx_ws2812_busy` reports when it has finished. Pick `bit0_pattern`
> / `bit1_pattern` and `reset_bytes` for your actual clock: the high-time window is
> only ±150ns wide, and the reset gap must hold the line low long enough to latch
> (>50us for WS2812, >280us on some WS2812B revisions).
