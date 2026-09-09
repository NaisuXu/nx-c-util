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

## nx_ssd1306 — SSD1306-family OLED controller driver

A driver for SSD1306-family OLED controllers (SSD1306, SSD1315, SSD1309, SH1106)
that owns the panel's command set and a full-resolution framebuffer: the caller
owns the wire (I²C or SPI) and supplies command/data write callbacks, while the
module turns framebuffer changes into the exact byte stream that panel expects.
No dynamic memory.

- **Panel descriptors** — the bundled `nx_ssd1306_panel_128x64` and
  `nx_ssd1306_panel_128x32` carry the geometry and the init sequence; a panel with
  a different resolution or wiring provides its own descriptor (`width`,
  `height`, `col_offset`, `init_seq`, `init_len`). Commands and the page-oriented
  data layout are shared across the family, so only the config bytes differ.
- **Zero allocation, caller-owned framebuffer** — the framebuffer size is a macro
  (`NX_SSD1306_FB_SIZE(width, height)`, 1 byte per pixel column per 8-row page) so
  it can size a static array. The caller passes it to `nx_ssd1306_init` and owns
  it for the life of the handle.
- **Page-oriented dirty tracking** — only the 8-row pages you changed are sent.
  `set_pixel` / `fill` / `clear` mark pages dirty directly; raw edits through
  `nx_ssd1306_framebuffer` must be announced with `nx_ssd1306_touch`, which
  touches only the pages that byte falls in (and clamps row positions). Every page
  starts dirty after init, since controller RAM holds undefined contents on power-up.
- **Non-blocking pump with a busy handshake** — `nx_ssd1306_process` sends one
  page per call and returns `NX_SSD1306_OK` (frame complete) or `NX_SSD1306_BUSY`
  (more pages pending, or the bus has not finished). The optional `is_busy`
  callback defers a page while a transfer is in flight, so a busy poll never eats
  a partial payload. `nx_ssd1306_busy` reports that deferral, and `flush` loops
  `process` to completion for a blocking call site.
- **I²C or SPI, no hardware access** — the driver emits command bytes and data
  bytes separately through two callbacks; on I²C the caller's `write_cmd`/`write_data`
  add the control byte (`0x00`/`0x40`), on SPI they drive the D/C line. The two
  callbacks share one `io_ctx` (the same bus), and neither is ever called while a
  busy-deferred write is pending.
- **Retry on I/O error** — a failed write leaves its page dirty and reports
  `NX_SSD1306_ERR_IO`, so the next `process` resends that page rather than
  advancing to a permanent gap.
- **Command wrappers** — `send_cmd` / `display_on` / `set_contrast` / `invert` /
  `flip` for the common operations; `flip` and the bundled descriptors' remap bytes
  both mirror the panel so a display wired upside-down still shows upright text.

```c
#include "src/device/nx_ssd1306.h"

/* caller-owned storage; the driver allocates nothing. */
static uint8_t fb[NX_SSD1306_FB_SIZE(128, 64)];

static const nx_ssd1306_cfg_t cfg = {
    .panel      = &nx_ssd1306_panel_128x64,
    .write_cmd  = i2c_write_cmd,   /* adds the 0x00 control byte, then the bytes */
    .write_data = i2c_write_data,  /* adds the 0x40 control byte, then the bytes */
    .is_busy    = i2c_busy,        /* NULL if the bus blocks until done         */
    .io_ctx     = &i2c,            /* passed to write_cmd / write_data          */
};

nx_ssd1306_t disp;
nx_ssd1306_init(&disp, &cfg, fb);

nx_ssd1306_set_pixel(&disp, 64, 32);   /* the middle pixel of a 128x64 panel */

/* Non-blocking: sends one page per call until the frame is on the bus. */
while (nx_ssd1306_process(&disp) == NX_SSD1306_BUSY) {
    /* yield to the main loop; poll nx_ssd1306_busy before touching fb again */
}
```

> **Note:** `nx_ssd1306_process` returning `NX_SSD1306_OK` means the frame is on
> the wire, not that the panel has latched it — the controller still needs a few
> display-clock cycles to paint the new rows, and with a DMA-backed bus the
> transfer can continue after the call. And because `process` is the only thing
> that sends a page, a caller that draws without pumping `process` will see a
> stale panel: dirty pages sit in the framebuffer until something pumps them.
