/**
 * @file    nx_kth7112.h
 * @brief   KTH7112 16-bit magnetic angle encoder over three-wire SPI.
 *
 * The KTH7112 is a hall angle sensor that answers three-wire SPI (Mode 3) with
 * either a 16-bit absolute angle, a register value, or the echo of a register
 * write. This module owns the protocol: command bytes, frame shapes, the CRC-8
 * trailing read and register accesses, and the register unlock state machine.
 *
 * Design:
 *   - The caller supplies the SPI primitives (chip select, write, read) and a
 *     nanosecond delay as callbacks, and owns the port's mode and clock rate.
 *   - The CRC-8 the part appends to its reads is computed here, from a table the
 *     module carries.
 *   - One call is one chip-select frame, executed synchronously on the caller's
 *     context.
 *
 * Typical flow:
 *   1. nx_kth7112_init with the SPI callbacks and delay.
 *   2. nx_kth7112_read_angle as often as an angle is wanted.
 *   3. nx_kth7112_unlock, then nx_kth7112_write_reg8 / _write_reg16 for
 *      configuration, then nx_kth7112_write_mtp to make it survive power loss.
 *
 * Memory:
 *   Nothing is allocated. The handle is caller-owned and holds a copy of the
 *   config; the callback context that copy points at must outlive the handle.
 *
 * Thread safety:
 *   Not thread-safe. The register-lock state lives in the handle, so serializing
 *   access is the caller's job.
 */
#ifndef NX_KTH7112_H
#define NX_KTH7112_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Minimum interval between two MTP burns, in milliseconds.
 *
 * The module holds no time source, so it cannot enforce this; it publishes the
 * limit for the caller to space its burns with. MTP programming is irreversible
 * and must not be interrupted by power loss.
 */
#define NX_KTH7112_MTP_MIN_INTERVAL_MS 400u

/* ------------------------------------------------------------------ */
/* Register addresses                                                  */
/* ------------------------------------------------------------------ */

#define NX_KTH7112_REG_ZERO_L      0x00u  /**< ZERO[7:0]  - zero position, low byte  */
#define NX_KTH7112_REG_ZERO_H      0x01u  /**< ZERO[15:8] - zero position, high byte */
#define NX_KTH7112_REG_CTRL        0x02u  /**< RD / AUTO_ZERO_SET / Z_*              */
#define NX_KTH7112_REG_PPT_L       0x03u  /**< PPT[7:0]   - ABZ resolution, low byte  */
#define NX_KTH7112_REG_PPT_H       0x04u  /**< RESERVE[7:4], PPT[11:8]                */
#define NX_KTH7112_REG_ABZ         0x05u  /**< ABZ_START_T / START_MODE / ABZLIMIT_F  */
#define NX_KTH7112_REG_HYS_L       0x06u  /**< RAM_HYS[7:0]  - output hysteresis      */
#define NX_KTH7112_REG_HYS_H       0x07u  /**< RESERVE[7:5], RAM_HYS[14:8]            */
#define NX_KTH7112_REG_NPP         0x08u  /**< RESERVE[7:5], NPP[4:0] - UVW pole pairs */
#define NX_KTH7112_REG_PWM_L       0x09u  /**< PWM_F[7:0]   - PWM frequency, low byte  */
#define NX_KTH7112_REG_PWM_H       0x0Au  /**< PWM_F[15:8]  - PWM frequency, high byte */
#define NX_KTH7112_REG_FW          0x0Du  /**< FW[7:0] - filter depth                 */
#define NX_KTH7112_REG_IO_MUX      0x10u  /**< RESERVE[7:3], IO_MUX[2:0]              */
#define NX_KTH7112_REG_CAL         0x16u  /**< RESERVE, REG_CAL, ANLC_EN, RESERVE     */
#define NX_KTH7112_REG_ANLC_STATUS 0x72u  /**< RESERVE[7:4], ANLC_STATUS[1:0]         */

/**
 * @brief Field positions inside the register bank.
 *
 * Every multi-byte field is low byte first, at the lower address: ZERO[7:0] sits
 * at 0x00 and ZERO[15:8] at 0x01, PPT[7:0] at 0x03 and PPT[11:8] at 0x04. The
 * bit ranges below are the ones the register map draws. They are published for
 * the caller to pack and unpack with: the module moves whole bytes and leaves the
 * bit layout of a register to its user.
 */

/* Register 0x02: RD, AUTO_ZERO_SET, Z_PHASE[1:0], Z_EDGE, Z_WID[2:0] */
#define NX_KTH7112_CTRL_RD_POS           7u     /**< Rotation direction; 1 = CW adds angle */
#define NX_KTH7112_CTRL_AUTO_ZERO_POS    6u     /**< 1 = capture the current angle as zero  */
#define NX_KTH7112_CTRL_Z_PHASE_POS      4u     /**< Z pulse phase, 2 bits                  */
#define NX_KTH7112_CTRL_Z_PHASE_MASK     0x3u
#define NX_KTH7112_CTRL_Z_EDGE_POS       3u     /**< 1 = falling edge marks the zero point  */
#define NX_KTH7112_CTRL_Z_WID_POS        0u     /**< Z pulse width, 3 bits                  */
#define NX_KTH7112_CTRL_Z_WID_MASK       0x7u

/* Register 0x05: ABZ_START_T[3:0], ABZ_START_MODE, ABZLIMIT_F[2:0] */
#define NX_KTH7112_ABZ_START_T_POS       4u     /**< ABZ output delay, 4 bits               */
#define NX_KTH7112_ABZ_START_T_MASK      0xFu
#define NX_KTH7112_ABZ_START_MODE_POS    3u     /**< 1 = absolute start, 0 = incremental    */
#define NX_KTH7112_ABZ_LIMIT_F_POS       0u     /**< ABZ output bandwidth, 3 bits           */
#define NX_KTH7112_ABZ_LIMIT_F_MASK      0x7u

/* Register 0x10: RESERVE[7:3], IO_MUX[2:0] */
#define NX_KTH7112_IO_MUX_POS            0u     /**< Output interface select, 3 bits        */
#define NX_KTH7112_IO_MUX_MASK           0x7u

/* Register 0x16: RESERVE[7:5], REG_CAL, ANLC_EN, RESERVE[2:0] */
#define NX_KTH7112_CAL_REG_CAL_POS       4u     /**< 1 starts an ANLC run                   */
#define NX_KTH7112_CAL_ANLC_EN_POS       3u     /**< 1 applies calibration to the output    */

/* Register 0x72: RESERVE[7:4], ANLC_STATUS[1:0], RESERVE[1:0] */
#define NX_KTH7112_ANLC_STATUS_POS       4u     /**< Calibration state, 2 bits              */
#define NX_KTH7112_ANLC_STATUS_MASK      0x3u

/** @brief Values ANLC_STATUS[1:0] takes while a calibration runs. */
#define NX_KTH7112_ANLC_IDLE     0u  /**< No calibration has started              */
#define NX_KTH7112_ANLC_RUNNING  1u  /**< Calibration in progress                 */
#define NX_KTH7112_ANLC_FAILED   2u  /**< Calibration failed                      */
#define NX_KTH7112_ANLC_DONE     3u  /**< Calibration complete                    */

/** @brief IO_MUX settings, meaningful while the MODE pin is held low. */
#define NX_KTH7112_IO_MUX_UVW  1u  /**< UVW commutation outputs        */
#define NX_KTH7112_IO_MUX_SSI  2u  /**< Two-wire SSI output            */
#define NX_KTH7112_IO_MUX_ABZ  4u  /**< ABZ incremental output         */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Return codes. NX_KTH7112_OK is 0.
 */
typedef enum {
    NX_KTH7112_OK = 0,      /**< Frame completed and verified                 */
    NX_KTH7112_ERR_PARAM,   /**< NULL handle or output pointer                */
    NX_KTH7112_ERR_IO,      /**< A callback reported failure, or is missing   */
    NX_KTH7112_ERR_CRC,     /**< Received CRC did not match                   */
    NX_KTH7112_ERR_LOCKED   /**< Register write while writes are locked out   */
} nx_kth7112_ret_t;

/**
 * @brief Transport configuration, supplied by the caller at init time.
 *
 * The three bus primitives are split rather than offered as one full-duplex
 * transfer, so a port that has separate transmit and receive paths maps onto them
 * directly. A frame is: chip select asserted, one or more writes, then one read
 * covering the response, then chip select released.
 *
 * Clock and phase are Mode 3 (CPOL = 1, CPHA = 1) and fixed by the part, so the
 * port is set up by the caller once, outside this module.
 *
 * cs: drives chip select. Called with true to start a frame and false to end it;
 * NULL is rejected by nx_kth7112_init. Receive data is only valid inside a frame,
 * so this must be called even when the port also manages the pin itself.
 *
 * write: shifts @p len bytes out. Must not also consume the response - the read
 * callback does that. Returns false on a port error.
 *
 * read: shifts @p len bytes in, generating the clocks those bytes need. Returns
 * false on a port error.
 *
 * is_busy: optional; returns true while the port cannot accept a new frame.
 * Checked once before chip select is asserted, and a busy port yields
 * NX_KTH7112_ERR_IO without the bus being touched, leaving the retry decision to
 * the caller. NULL means the port is assumed ready, which suits blocking
 * transfers.
 *
 * delay_ns: optional; waits at least @p ns nanoseconds, used for the gap the part
 * requires between two frames. NULL skips that wait, which is only correct when
 * the surrounding port already guarantees it - a bit-banged or DMA-backed pace, or
 * a transfer slow enough that the interval is met by construction.
 *
 * io_ctx: opaque context passed to every callback.
 */
typedef struct {
    bool (*cs)(void *ctx, bool assert);                           /**< Required */
    bool (*write)(void *ctx, const uint8_t *data, size_t len);    /**< Required */
    bool (*read)(void *ctx, uint8_t *data, size_t len);           /**< Required */
    bool (*is_busy)(void *ctx);                                   /**< Optional; may be NULL */
    void (*delay_ns)(uint32_t ns);                                /**< Optional; may be NULL */
    void *io_ctx;                                                 /**< Context for every callback */
} nx_kth7112_cfg_t;

/**
 * @brief Driver instance.
 *
 * Configuration is copied at init, so the struct it came from need not outlive
 * the handle; only the callback context it points at must.
 */
typedef struct {
    nx_kth7112_cfg_t cfg;       /**< Configuration (copied at init)          */
    struct {
        bool unlocked;          /**< Register writes permitted right now     */
    } run;                      /**< Runtime state                           */
} nx_kth7112_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialize a driver instance.
 *
 * The part powers up with registers locked, so @c run.unlocked starts false and
 * every register write returns NX_KTH7112_ERR_LOCKED until nx_kth7112_unlock
 * succeeds.
 *
 * @param  dev  Driver instance, must not be NULL.
 * @param  cfg  Configuration, must not be NULL; copied into @p dev. Its cs, write
 *              and read callbacks must all be non-NULL.
 *
 * @return true on success; false if @p dev or @p cfg is NULL, or a required
 *         callback is missing.
 */
bool nx_kth7112_init(nx_kth7112_t *dev, const nx_kth7112_cfg_t *cfg);

/**
 * @brief  Read the absolute angle as the part's raw 16-bit code.
 *
 * The whole 16-bit range is one mechanical revolution, so the code is the
 * angle's fraction of a turn: @p out_raw = angle * 65536 / 360. The value is only
 * written once the trailing CRC checks out.
 *
 * @param  dev      Driver instance.
 * @param  out_raw  Receives the angle, 0 .. 65535. Untouched on failure.
 *
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, NX_KTH7112_ERR_IO, or
 *         NX_KTH7112_ERR_CRC.
 */
nx_kth7112_ret_t nx_kth7112_read_angle(nx_kth7112_t *dev, uint16_t *out_raw);

/**
 * @brief  Convert a raw angle code to millidegrees.
 *
 * For callers that want degrees without floating point: @p raw * 360000 / 65536,
 * computed in 64-bit intermediates. The result spans 0 .. 359994, each step being
 * about 0.0055 degrees, so the resolution is the part's and not the unit's.
 *
 * @param  raw  Angle code, 0 .. 65535.
 * @return The angle in millidegrees, 0 .. 359994.
 */
static inline uint32_t nx_kth7112_raw_to_mdeg(uint16_t raw)
{
    /* Full scale is one revolution, so the product needs more than 32 bits:
     * 65535 * 360000 fits in 35. Subtracting the zero position is left out on
     * purpose - where zero sits is a register setting, not a property of the code. */
    return (uint32_t)(((uint64_t)raw * 360000u) >> 16);
}

/**
 * @brief  Read one register byte.
 *
 * Reading is allowed while the registers are locked.
 *
 * @param  dev   Driver instance.
 * @param  addr  Register address; see the NX_KTH7112_REG_* constants.
 * @param  val   Receives the register value. Untouched on failure.
 *
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, NX_KTH7112_ERR_IO, or
 *         NX_KTH7112_ERR_CRC.
 */
nx_kth7112_ret_t nx_kth7112_read_reg8(nx_kth7112_t *dev, uint8_t addr, uint8_t *val);

/**
 * @brief  Write one register byte.
 *
 * The part echoes the accepted value in the same frame, and the module compares
 * that echo against what was sent; a mismatch is reported as
 * NX_KTH7112_ERR_IO, since the write went unacknowledged or was corrupted.
 *
 * The value is held in volatile registers and lost at power-down. Use
 * nx_kth7112_write_mtp to commit the whole register bank.
 *
 * @param  dev   Driver instance.
 * @param  addr  Register address.
 * @param  val   Value to write.
 *
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, NX_KTH7112_ERR_LOCKED,
 *         NX_KTH7112_ERR_IO, or NX_KTH7112_ERR_CRC.
 */
nx_kth7112_ret_t nx_kth7112_write_reg8(nx_kth7112_t *dev, uint8_t addr, uint8_t val);

/**
 * @brief  Read a two-byte register field, low byte first.
 *
 * The multi-byte fields of this part are stored low byte at the lower address:
 * @p addr carries the low byte and @p addr + 1 the high byte. The address must
 * name the low byte, so NX_KTH7112_REG_ZERO_L reads the whole zero position and
 * NX_KTH7112_REG_ZERO_H reads only its high byte.
 *
 * The two bytes are fetched in separate frames, each with its own CRC, so a value
 * that changes between them can be mixed. Fields that move on their own - the
 * angle is the one that does - are read through nx_kth7112_read_angle instead,
 * which takes both bytes in a single frame.
 *
 * @param  dev   Driver instance.
 * @param  addr  Address of the field's low byte.
 * @param  val   Receives the value, @c low | (@c high @c << @c 8 ).
 *
 * @return As nx_kth7112_read_reg8.
 */
nx_kth7112_ret_t nx_kth7112_read_reg16(nx_kth7112_t *dev, uint8_t addr, uint16_t *val);

/**
 * @brief  Write a two-byte register field, low byte first.
 *
 * Counterpart of nx_kth7112_read_reg16: the low byte goes to @p addr and the high
 * byte to @p addr + 1, each echoed and verified.
 *
 * @param  dev   Driver instance.
 * @param  addr  Address of the field's low byte.
 * @param  val   Value to write.
 *
 * @return As nx_kth7112_write_reg8.
 */
nx_kth7112_ret_t nx_kth7112_write_reg16(nx_kth7112_t *dev, uint8_t addr, uint16_t val);

/**
 * @brief  Unlock the registers for writing.
 *
 * The part is locked at power-up and discards register writes silently while
 * locked, so this must succeed before any write. The module tracks that state
 * itself and refuses a write issued while locked without putting a frame on the
 * bus.
 *
 * @param  dev  Driver instance.
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, or NX_KTH7112_ERR_IO.
 *
 * @note   The part powers up locked and forgets the unlock only at power-down. A
 *         fresh driver instance therefore starts out locked even against a part
 *         that is still unlocked, and this call is what clears that; it is safe to
 *         repeat.
 */
nx_kth7112_ret_t nx_kth7112_unlock(nx_kth7112_t *dev);

/**
 * @brief  Lock the registers again, rejecting further writes.
 *
 * @param  dev  Driver instance.
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, or NX_KTH7112_ERR_IO.
 */
nx_kth7112_ret_t nx_kth7112_lock(nx_kth7112_t *dev);

/**
 * @brief  Burn the current register values into MTP.
 *
 * Makes the register bank survive power-down, and is irreversible. The part needs
 * more than NX_KTH7112_MTP_MIN_INTERVAL_MS between burns, which the module cannot
 * measure - space the calls yourself, and do not let power drop mid-burn. The
 * registers must be unlocked first.
 *
 * @param  dev  Driver instance.
 * @return NX_KTH7112_OK, NX_KTH7112_ERR_PARAM, NX_KTH7112_ERR_LOCKED, or
 *         NX_KTH7112_ERR_IO.
 */
nx_kth7112_ret_t nx_kth7112_write_mtp(nx_kth7112_t *dev);

/**
 * @brief  Report whether the port is currently unable to take a frame.
 *
 * Mirrors the optional is_busy callback, so a caller can find a good moment to
 * read the angle instead of taking NX_KTH7112_ERR_IO from a read that arrives too
 * early. Always false when no is_busy callback is configured.
 *
 * @param  dev  Driver instance; NULL reports false.
 * @return true if the port is busy.
 */
bool nx_kth7112_busy(nx_kth7112_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* NX_KTH7112_H */
