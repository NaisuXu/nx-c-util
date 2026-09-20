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

## nx_kth7112 — KTH7112 magnetic angle encoder over SPI

A driver for the KTH7112 16-bit magnetic angle encoder, covering its three-wire SPI
protocol (Mode 3): command bytes, frame shapes, the CRC-8 the part appends to every
read, and the register-lock state machine. The caller owns the SPI port and supplies
chip-select, write, read and delay callbacks. No dynamic memory, no floating point.

- **Frame-level API** — `nx_kth7112_read_angle` returns the raw 16-bit code, and
  `nx_kth7112_read_reg8` / `_write_reg8` / `_read_reg16` / `_write_reg16` reach the
  register bank. One call is one chip-select frame, executed synchronously: a frame
  is a handful of bytes, far shorter than a control cycle, so there is nothing to
  spread across iterations. `nx_kth7112_raw_to_mdeg` converts a code to
  millidegrees for callers that want degrees without floating point.
- **CRC-8/ITU on every read, verified before the value is released** — the part
  appends polynomial `0x07` / init `0x00` / xorout `0x55`, computed here from the
  module's own 256-entry table. A mismatch returns `NX_KTH7112_ERR_CRC` and leaves
  the caller's output variable untouched, so a failed read can never be mistaken for
  a good one.
- **Register writes are acknowledged** — a write answers with the value the part
  accepted, in the same frame, and the driver compares that echo against what it
  sent. A mismatch is `NX_KTH7112_ERR_IO`: the write went unacknowledged rather than
  merely unauthorised.
- **The lock state is tracked, and a locked write never reaches the bus** — the part
  is locked at power-up and discards register writes silently. The driver tracks the
  state itself and returns `NX_KTH7112_ERR_LOCKED` from a write issued while locked
  without putting a frame on the wire. Unlocking may be repeated, so calling
  `nx_kth7112_unlock` again is how the state is re-established.
- **Low byte at the low address** — the multi-byte fields are stored low byte first,
  so `NX_KTH7112_REG_ZERO_L` addresses `ZERO[7:0]` and `NX_KTH7112_REG_ZERO_H` the
  high byte. `_read_reg16` / `_write_reg16` take the low byte's address and move both
  bytes, each with its own CRC and echo check.
- **Optional callbacks stay optional** — `is_busy` and `delay_ns` may each be NULL.
  A NULL `is_busy` means the port is assumed ready, which suits blocking transfers;
  a NULL `delay_ns` skips the inter-frame wait, which is correct only when the port
  already guarantees it.
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

> **Note:** `nx_kth7112_write_mtp` burns the register bank into non-volatile memory
> and is irreversible. The part needs more than `NX_KTH7112_MTP_MIN_INTERVAL_MS`
> (400 ms) between burns, which the driver cannot measure since it holds no time
> source — space the calls yourself and never let power drop mid-burn. Two further
> timing requirements belong to the port rather than the driver: the SCK high time of
> the 24th clock of a register write must exceed 100 ns, and two frames must be more
> than 150 ns apart. These are comfortable at a few MHz and marginal at the part's
> 10 Mbps ceiling, where the clock period is the 100 ns minimum itself, so check
> what your port actually runs — and that it does not stretch its final clock — then
> use `delay_ns` if either interval needs help.

