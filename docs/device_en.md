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

## nx_ktm5220 — KTM5220 magnetic angle encoder over SPI

This driver implements the KTM5220 v0.5 (2026-05-14) four-wire SPI protocol.
Configure the port for Mode 3, MSB first, at up to 20 MHz. The driver uses
caller-owned storage and integer arithmetic, with the same callback approach as
`nx_kth7112`.

- **Angle and alarms** — `nx_ktm5220_read_sample` reads one coherent 21-bit
  angle and three status bits with CRC validation. Raw values span
  `0 .. 0x1FFFFF`; `nx_ktm5220_raw_to_mdeg` converts them to millidegrees.
  Status masks identify overspeed or memory CRC failure, ADC amplitude alarms,
  and undervoltage. A valid sample returns `NX_KTM5220_OK` even with alarms;
  inspect `sample.status`. The angle-only `nx_ktm5220_read_angle` instead returns
  `NX_KTM5220_ERR_STATUS` when an alarm is present. Outputs remain unchanged on
  errors.
- **Register access and protection** — `read_reg8`/`write_reg8` and
  `read_reg16`/`write_reg16` access the published register map. Sixteen-bit fields
  are low byte first, use separate frames, and reject address wrap.
  `nx_ktm5220_unlock` writes `0xAA` to `0x1A` and requires CRC-checked readback of
  `0x01`; writes otherwise return `NX_KTM5220_ERR_LOCKED`. There is no documented
  software lock command. Reinitialize after a sensor reset and unlock again
  before configuration. Writes have no echo; use a subsequent read when the
  application needs verification.
- **Zero and calibration** — `get_zero`/`set_zero` access the 21-bit zero offset;
  `zero_here` triggers the self-clearing capture of the current position.
  `set_zero` and `update_reg8` preserve unrelated register bits. `set_calibration`
  controls `CAL_REG_SET`; `read_calibration_status` reads the two-bit status.
  The application controls motor speed and polling timeouts. Multi-frame
  operations are not atomic and failed writes can leave a partial update.
- **Synchronous full-duplex transfer** — `cs(ctx, true)` drives CSN low and
  `false` drives it high. Each CSN window calls
  `transfer(ctx, tx, rx, len)` once for the complete frame, directly mapping to
  a hardware SPI full-duplex transfer. Both buffers are non-NULL and separate;
  the callback must finish transmitting and receiving all `len` bytes before
  returning. The driver supplies zero dummy bytes while receiving the response
  and ignores MISO during the command, when the chip's output is high impedance.
  Complete frame lengths are six bytes for angle reads, four for register reads,
  and three for register writes or MTP programming. The optional
  `is_busy` refuses a frame immediately when the SPI port is busy. The optional
  `delay_ns` requests 13 ns before CSN rises, meeting the specified
  last-clock-to-CS hold time of at least 12.5 ns. Without this callback the port
  must guarantee that hold time. `nx_ktm5220_busy` reports only the port state,
  not completion of calibration or MTP programming. Serialize complete API
  calls when sharing a handle or SPI bus.

```c
#include "nx_ktm5220.h"

/* spi_cs and spi_transfer are synchronous application callbacks. */
bool encoder_init(nx_ktm5220_t *enc, void *spi_ctx)
{
    const nx_ktm5220_cfg_t cfg = {
        .cs         = spi_cs,
        .transfer   = spi_transfer, /* full-duplex transfer of the whole frame */
        .is_busy    = NULL,
        .delay_ns   = delay_ns,  /* optional if the port guarantees TH */
        .io_ctx     = spi_ctx,
        .crc_invert = false,     /* matches the default SPI_CRC setting */
    };
    return nx_ktm5220_init(enc, &cfg);
}

bool encoder_read_mdeg(nx_ktm5220_t *enc, uint32_t *mdeg)
{
    nx_ktm5220_sample_t sample;
    if (mdeg == NULL ||
        nx_ktm5220_read_sample(enc, &sample) != NX_KTM5220_OK) {
        return false;
    }
    if (sample.status != 0u) {
        return false;            /* application handles the individual alarms */
    }
    *mdeg = nx_ktm5220_raw_to_mdeg(sample.raw);
    return true;
}

nx_ktm5220_ret_t encoder_set_offset(nx_ktm5220_t *enc, uint32_t raw)
{
    nx_ktm5220_ret_t ret = nx_ktm5220_unlock(enc);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    return nx_ktm5220_set_zero(enc, raw);  /* RAM only; no MTP command */
}
```

CRC uses polynomial `0x07`, initial value `0x00`, and no reflection.
`cfg.crc_invert = false` selects xorout `0x00`; `true` selects `0xFF` and must
match the chip's `SPI_CRC` configuration. The supplied v0.5 manual does not
publish that setting's register location or explicitly define CRC coverage.
The driver checks response data only: one register byte, or all three packed
angle/status bytes. Confirm this interpretation on hardware during bring-up.

`nx_ktm5220_write_mtp` sends the programming command and verifies a `0x55`
acknowledgement; success does not mean programming has finished. Maintain stable
power, wait at least `NX_KTM5220_MTP_PROGRAM_TIME_MS` (2501 ms), then power-cycle
before further operations. Apply that wait even after an I/O error if the command
may have reached the chip; do not immediately retry. No driver call waits for or
tracks programming completion. The v0.5 manual also conflicts on whether
register-triggered calibration saves automatically (sections 9.2.1 and 9.2.3).
`set_calibration` only sends the trigger, but the chip may itself program MTP;
confirm that behavior before using calibration in the application.

The runnable fake-SPI example is in `examples/device/nx_ktm5220_example.c` and
runs as part of `nx_device_examples`.

## nx_mp6546 — MP6546 three-phase motor driver

This module implements the register semantics in MP6546 Rev. 1.0
(2023-03-20), using caller-owned storage and integer arithmetic. The chip
generates three-phase outputs from d/q voltage commands and a rotor angle.
Current, speed and position control, alignment, voltage/current limits, EN,
timing and recovery belong to the application.

The public manual does not specify the complete I2C transactions or byte order.
The required synchronous `read_reg16` and `write_reg16` callbacks must therefore
come from a verified platform port. This module does not supply an I2C wire
backend or infer frames from the register map. Each register address identifies
**one 16-bit register**, rather than two adjacent byte registers. Callback
values use the manual's numeric bit[15:0] layout regardless of CPU endianness;
the port handles byte order, START/repeated START/STOP, ACK, bus release and
bounded timeouts. DMA transfers must finish before returning. `addr7` is
unshifted and excludes the R/W bit; supported addresses are `0x07`, `0x08` and
`0x09`. The configuration is copied, while `io_ctx` must outlive the handle.

- **Initialization and configuration** — `nx_mp6546_init` validates and copies
  the configuration without I/O, probing, resetting, changing EN or starting
  the motor. It does not establish hardware readiness. Sensor, drive and angle
  source setters perform read-modify-write and preserve unrelated and reserved
  bits. `set_motor_angle` sets 1–15 pole pairs, a 12-bit mechanical-angle bias
  and direction; it does not perform alignment or determine the bias sign.
  This operation can fail after updating only part of the configuration.
  `set_test_angle` sets a 12-bit fixed angle, which is used only after explicitly
  selecting `NX_MP6546_ANGLE_TEST` in vector mode.
- **Samples and faults** — `read_sample` obtains the 15-bit mechanical angle
  and aggregate fault bit from the same `0x0C` read. It returns `NX_MP6546_OK`
  even when `sample.fault` is true. `read_angle` instead returns
  `NX_MP6546_ERR_STATUS` for that fault and leaves the angle output unchanged.
  `read_faults` reads `0x0D` and returns the positioned `NX_MP6546_FAULT_*`
  bits without the INL compensation byte; this is a separate observation,
  not a simultaneous sample. Read and conversion outputs remain unchanged on
  every error. `raw_to_mdeg` converts a valid `0..0x7FFF` angle to
  `0..359989` millidegrees; output width does not specify sensor accuracy.
- **Vector commands and units** — `set_dq` accepts signed raw codes
  `-512..511`, encodes 10-bit two's complement, and writes Ud (`0x08`) before
  Uq (`0x09`). `set_uq` writes only Uq. Upper reserved command-word bits are
  sent as zero. These calls require an already configured vector mode and do
  not check or select it. `voltage_to_raw` calculates
  `round(512 * sqrt(3) * voltage_mv / bus_voltage_mv)` using integer arithmetic.
  A zero bus voltage or an out-of-range rounded result is rejected, without
  clipping. Supply the measured bus voltage and limit the total d/q vector,
  slew, current and stall duration in the application.
- **PWM commands** — With duty mode already selected, `set_pwm` validates all
  three phases before I/O, including a disabled phase's `duty_permille`, which
  must be `0..1000`. Enabled duties are rounded to the hardware's 8-bit steps:
  the lower byte runs in reverse, so 0‰ encodes as `0x1FF` and 1000‰ as
  `0x100`. A disabled channel encodes as zero and differs from an enabled 0‰
  channel. A/B/C are written to `0x07/0x08/0x09` in order; the manual describes
  `0x09` as the group commit. An error can leave pending or effective values.
  After application recovery, resubmit the entire group.
- **Register access** — Raw accesses reject unpublished addresses, RO writes
  and command reads. Raw writes leave reserved-bit and side-effect management
  to the caller. `update_reg16` uses
  `(old & ~mask) | (value & mask)`, accepts only documented ordinary writable
  fields, rejects command/RO/production-code/reserved fields, and does no I/O
  for a zero mask. A read failure never causes a write. Changing the hardware
  address does not update the handle automatically: verify the new address
  externally and reinitialize the handle with it.

Serialize complete API calls, including read-modify-write and multi-register
operations. D/Q and multi-register configuration writes are not guaranteed
atomic. Any failed write may already have reached the chip, leaving a partial
or unknown hardware state. The module does not retry, roll back, or change EN
on an error. The application must decide when to stop and recover.

Registers `0x07..0x09` are shared between vector and duty modes. Mode setters do
not reinitialize their contents or sequence a safe transition; stale values
can take on a different meaning after a mode change. The application must
control transitions, readiness and output commands. EN low completely shuts
down the chip, so configuration while EN is low is not assumed to work.
`enter_normal` issues the work command without resetting or clearing faults.
`enter_standby` sets `STDBY_EN` and issues the standby command; LDO behavior
follows `TRIM_ENP`. SCL/SCK falling edges can wake the chip, including traffic
on a shared bus, so standby is not a shutdown guarantee.

In SPI angle mode, bring-up must also select sensor angle reading as described
on p11: explicitly write zero to `NX_MP6546_REG_SPI_CMD` (`0x11`) and check
the result. `read_sample` only reads `0x0C`; it does not implicitly change
the sensor command or configure its protocol.

The following application fragment binds an already verified port. Before
calling `motor_step`, the application must have explicitly powered and
configured the chip for vector mode, calibrated pole pairs/direction/offset,
selected the sensor angle, completed alignment and established Ud = 0. The
caller supplies a suitably limited Uq command, checks the returned result and
handles any fault or I/O error through its own EN/recovery policy.

```c
#include "nx_mp6546.h"
#include <stddef.h>

/* Application port: complete verified register transactions synchronously. */
extern bool port_read_reg16(void *ctx, uint8_t addr7,
                            uint8_t reg, uint16_t *value);
extern bool port_write_reg16(void *ctx, uint8_t addr7,
                             uint8_t reg, uint16_t value);

bool motor_bind(nx_mp6546_t *motor, void *port_ctx)
{
    const nx_mp6546_cfg_t cfg = {
        .read_reg16  = port_read_reg16,
        .write_reg16 = port_write_reg16,
        .io_ctx      = port_ctx,
        .addr7       = NX_MP6546_ADDR_LOW,
    };
    return nx_mp6546_init(motor, &cfg); /* Software binding only. */
}

nx_mp6546_ret_t motor_step(nx_mp6546_t *motor, int32_t uq_mv,
                           uint32_t bus_mv, uint32_t *angle_mdeg)
{
    if (angle_mdeg == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    nx_mp6546_sample_t sample;
    nx_mp6546_ret_t ret = nx_mp6546_read_sample(motor, &sample);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    if (sample.fault) {
        return NX_MP6546_ERR_STATUS;
    }
    int16_t uq;
    ret = nx_mp6546_voltage_to_raw(uq_mv, bus_mv, &uq);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    ret = nx_mp6546_set_uq(motor, uq);
    if (ret != NX_MP6546_OK) {
        return ret; /* Output may have changed; caller handles recovery. */
    }
    *angle_mdeg = nx_mp6546_raw_to_mdeg(sample.angle_raw);
    return NX_MP6546_OK;
}
```

No API programs OTP, clears fault flags or sends broadcast commands. The manual
does not publish the required programming, flag-clear or complete broadcast
protocols; successful register writes do not imply persistent storage. The
manual also conflicts on ADC resolution (12 bits on p1, 10 on p5), thermal
shutdown thresholds (p5 versus p13), and the width/frequency encodings of
`FLT_N_SVM` (p12 versus p15–16). Only its positioned mask is exposed, without
a frequency enum. Ud uses `0x08` per p12 and the register map; the p17 heading
incorrectly lists `0x09`. Resolve these points with MPS and verify the platform
transactions, control sequencing and protection behavior on real hardware;
host-side register simulation cannot establish hardware compatibility.

