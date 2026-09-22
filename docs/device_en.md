# Device Modules

## nx_ws2812 — WS2812(B) RGB LED strip driver

This driver encodes pixel data for WS2812/WS2812B addressable RGB LEDs. The
caller manages the hardware peripheral (SPI, UART, or timer with DMA) and
provides a write callback; the module produces the byte stream for that
peripheral. It does not allocate memory.

- **Configurable bit-expansion encoding** — the WS2812 protocol uses an
  approximately 1.25 µs bit period with a ±150 ns high-time tolerance. Each
  WS2812 data bit is expanded into a configurable 8-bit peripheral pattern whose
  run of high bits sets the pulse width. The `bit0_pattern` and `bit1_pattern`
  settings adapt the encoder to supported peripheral clocks and bit orders.
  Reference patterns are `0xC0` / `0xF8` for MSB-first SPI at 2.4–3.6 MHz and
  `0x03` / `0x1F` for LSB-first UART at 2.4–3.2 Mbaud (8N1, inverted TX).
- **Caller-owned buffers** — both buffer sizes are provided by macros suitable
  for static array declarations. `NX_WS2812_PIXEL_BUF_SIZE` reserves 3 bytes per
  LED in GRB order for color state. `NX_WS2812_TX_BUF_SIZE` reserves 24 bytes per
  LED plus the reset bytes for the encoded transfer; `nx_ws2812_update` rebuilds
  this buffer on every call, and it may otherwise be treated as scratch storage.
- **Busy-aware updates** — if the peripheral is still sending the previous
  frame, `nx_ws2812_update` returns `false` immediately without modifying the
  transfer buffer, so a later retry cannot corrupt in-flight data. The optional
  `is_busy` callback enables this check and is also exposed through
  `nx_ws2812_busy`, allowing the caller to poll without spinning inside the
  driver. This model is well suited to DMA-backed transfers. When `is_busy` is
  NULL, the transfer is assumed complete when `write` returns; whether that call
  blocks depends on the callback implementation.
- **Lossless global brightness** — because WS2812 devices have no brightness
  register, `nx_ws2812_update` applies the brightness setting as a scale factor
  to each channel while encoding. It never writes the scaled values back to the
  pixel buffer, so repeated dimming and brightening does not accumulate rounding
  error.
- **Pixel operations** — `set_pixel`, `fill`, and `set_all` set colors;
  `get_pixel` reads them back; and `clear` sets every pixel to black. `push` and
  `push_tail` shift the strip in either direction, insert a new color at the
  vacated end, and discard the pixel shifted past the opposite end. These
  operations support effects such as marquees, comet trails, and VU meters.
- **Shared I/O context** — the `write` and `is_busy` callbacks operate on the
  same peripheral, so both receive the same `io_ctx` as their first argument.
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

> **Note:** A `true` return from `nx_ws2812_update` means that the peripheral
> accepted the write; it does not mean that the LEDs have latched the frame. With
> DMA, transmission continues in the background, and `nx_ws2812_busy` reports
> when it has finished. Select `bit0_pattern`, `bit1_pattern`, and `reset_bytes`
> for the actual peripheral clock. The high-time tolerance is only ±150 ns, and
> the reset interval must hold the line low long enough to latch the frame: more
> than 50 µs for WS2812 devices and more than 280 µs for some WS2812B revisions.

## nx_kth7112 — KTH7112 magnetic angle encoder over SPI

This driver implements the KTH7112 three-wire SPI Mode 3 protocol, including
command framing, read-response CRC validation, and register locking. The caller
manages the SPI port and provides chip-select, write, read, and optional timing
callbacks. The module does not allocate memory or use floating-point arithmetic.

- **Synchronous frame-level API** — `nx_kth7112_read_angle` returns the raw
  16-bit angle code. `nx_kth7112_read_reg8`, `nx_kth7112_write_reg8`,
  `nx_kth7112_read_reg16`, and `nx_kth7112_write_reg16` access the register bank.
  Each call synchronously executes the required chip-select frame or frames in
  the caller's context. `nx_kth7112_raw_to_mdeg` converts a raw code to
  millidegrees without floating-point arithmetic.
- **CRC-8/ITU validation on every read** — the device appends a CRC byte to each
  read response. The CRC uses polynomial `0x07`, initial value `0x00`, no input
  or output reflection, and xorout `0x55`; its check value for `"123456789"` is
  `0xA1`. The driver calculates it with an internal 256-entry table and validates
  it before returning data. A mismatch returns `NX_KTH7112_ERR_CRC` and leaves
  the caller's output variable unchanged.
- **Verified register writes** — the device echoes the accepted value in the
  same frame, and the driver compares that value with the byte it sent. A
  mismatch returns `NX_KTH7112_ERR_IO`.
- **Software-tracked lock state** — the device powers up with register writes
  locked and silently ignores writes while locked. The driver also starts in the
  locked state and returns `NX_KTH7112_ERR_LOCKED` without accessing the bus when
  a write is attempted. `nx_kth7112_unlock` may be called repeatedly; call it
  again after reinitializing the driver or whenever the hardware lock state may
  have changed.
- **Low byte at the low address** — multi-byte fields store their low byte at
  the lower address. For example, `NX_KTH7112_REG_ZERO_L` addresses `ZERO[7:0]`
  and `NX_KTH7112_REG_ZERO_H` addresses `ZERO[15:8]`.
  `nx_kth7112_read_reg16` and `nx_kth7112_write_reg16` take the low-byte address
  and access both registers in separate frames, each with its own CRC check or
  echo verification.
- **Optional port-state callbacks** — `is_busy` and `delay_ns` may be NULL. A
  NULL `is_busy` means that the port is assumed ready, which suits blocking
  transfers. A NULL `delay_ns` omits the driver's inter-frame delay, so the port
  must satisfy that timing requirement itself.
- **Not thread-safe** — the lock state lives in the handle, so serialize access from
  multiple contexts yourself.

```c
#include "nx_kth7112.h"

static const nx_kth7112_cfg_t cfg = {
    .cs         = spi_cs,        /* required: asserts and releases chip select */
    .write      = spi_write,     /* required: shifts bytes out                 */
    .read       = spi_read,      /* required: shifts bytes in                  */
    .is_busy    = spi_busy,      /* optional: NULL if the port is always ready */
    .delay_ns   = delay_ns,      /* optional: NULL if the port already paces   */
    .io_ctx     = &spi,
};

nx_kth7112_t enc;
nx_kth7112_init(&enc, &cfg);

uint16_t raw;
if (nx_kth7112_read_angle(&enc, &raw) == NX_KTH7112_OK) {
    /* raw == angle * 65536 / 360 */
}

/* The part powers up locked, so unlock once before any register write. */
nx_kth7112_unlock(&enc);
nx_kth7112_write_reg8(&enc, NX_KTH7112_REG_FW, 0x44u);   /* verify echo */
nx_kth7112_write_mtp(&enc);                              /* make it survive power-down */
nx_kth7112_lock(&enc);                                   /* refuse further writes */
```

> **Note:** `nx_kth7112_write_mtp` programs the register bank into nonvolatile
> memory, and the operation is irreversible. The application must leave more
> than `NX_KTH7112_MTP_MIN_INTERVAL_MS` (400 ms) between programming operations
> and maintain stable power while programming. The SPI port must also keep the
> high phase of the 24th clock in a register-write frame longer than 100 ns and
> leave more than 150 ns between frames. Configure the SPI clock and final-clock
> behavior to meet the first requirement. The driver requests a 150 ns delay
> through `delay_ns` after each frame; the callback or the port must ensure that
> the actual gap exceeds 150 ns. Near the device's 10 Mbps limit, verify both
> timings explicitly.

