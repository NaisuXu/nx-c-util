/**
 * @file    nx_ktm5220.h
 * @brief   KTM5220 21-bit magnetic angle encoder over four-wire SPI.
 *
 * Implements the KTM5220 v0.5 (2026-05-14) protocol. The caller configures
 * MSB-first SPI Mode 3 (CPOL=1, CPHA=1), at no more than 20 MHz, and supplies
 * synchronous chip-select and full-duplex transfer callbacks. No memory is
 * allocated.
 * Initialize after power-up, then read samples; unlock before configuration.
 * Configuration/zero writes need write_mtp for persistence. Calibration may
 * cause the chip to save automatically; see set_calibration below.
 *
 * The handle owns a copy of the configuration. Its io_ctx must outlive it.
 * Not thread-safe: serialize complete API calls, including multi-frame calls.
 * Reinitialize after a chip reset to discard the cached unlock state.
 */
#ifndef NX_KTM5220_H
#define NX_KTM5220_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_KTM5220_ANGLE_BITS           21u
#define NX_KTM5220_ANGLE_COUNTS         0x200000u
#define NX_KTM5220_ANGLE_MAX            0x1FFFFFu
#define NX_KTM5220_SPI_MAX_HZ           20000000u
/** Wait at least this long after a burn command (>2.5 s), then power-cycle. */
#define NX_KTM5220_MTP_PROGRAM_TIME_MS  2501u

/* Register addresses published in datasheet section 10.2. Multi-byte fields
 * store their low byte at the lower address. Unlisted addresses are reserved. */
#define NX_KTM5220_REG_PRODUCT_ID        0x0Cu
#define NX_KTM5220_REG_ZERO_SET          0x12u
#define NX_KTM5220_REG_DIFF_AVG_L        0x13u
#define NX_KTM5220_REG_DIFF_AVG_M        0x14u
#define NX_KTM5220_REG_DIFF_AVG_H        0x15u
#define NX_KTM5220_REG_ANLC_STATUS       0x16u
#define NX_KTM5220_REG_CAL_REG_SET       0x17u
#define NX_KTM5220_REG_WRITE_UNLOCK      0x1Au
#define NX_KTM5220_REG_ZERO_L            0x4Bu
#define NX_KTM5220_REG_ZERO_M            0x4Cu
#define NX_KTM5220_REG_ZERO_H            0x4Du
#define NX_KTM5220_REG_ROT_DIR           0x51u
#define NX_KTM5220_REG_PWM_LNR           0x52u
#define NX_KTM5220_REG_FILTER            0x54u
#define NX_KTM5220_REG_MAG_TH            0x55u
#define NX_KTM5220_REG_HYST              0x56u
#define NX_KTM5220_REG_ABZ_CONFIG        0x57u
#define NX_KTM5220_REG_ABZ_CTRL          0x58u
#define NX_KTM5220_REG_ABZ_PPT_L         0x59u
#define NX_KTM5220_REG_ABZ_PPT_H         0x5Au
#define NX_KTM5220_REG_UVW_CONFIG        0x5Bu
#define NX_KTM5220_REG_DEVICE_ID_0       0x68u
#define NX_KTM5220_REG_DEVICE_ID_1       0x69u
#define NX_KTM5220_REG_DEVICE_ID_2       0x6Au
#define NX_KTM5220_REG_DEVICE_ID_3       0x6Bu
#define NX_KTM5220_REG_GOLDEN_BASE       0x80u
#define NX_KTM5220_GOLDEN_COUNT          64u

/* Masks are positioned within the register, suitable for update_reg8. */
#define NX_KTM5220_ZERO_SET_MASK         0x01u
#define NX_KTM5220_ANLC_STATUS_MASK      0x03u
#define NX_KTM5220_CAL_REG_SET_MASK      0x01u
#define NX_KTM5220_ZERO_H_MASK           0x1Fu
#define NX_KTM5220_ROT_DIR_MASK          0x80u

#define NX_KTM5220_LNR2_UPD_MASK         0x80u
#define NX_KTM5220_LNR1_UPD_MASK         0x40u
#define NX_KTM5220_LNR2_EN_MASK          0x20u
#define NX_KTM5220_LNR1_EN_MASK          0x10u
#define NX_KTM5220_PWM_POL_MASK          0x08u
#define NX_KTM5220_PWM_SRC_MASK          0x04u
#define NX_KTM5220_PWM_FQ_MASK           0x02u
#define NX_KTM5220_PWM_EN_MASK           0x01u

#define NX_KTM5220_ABZ_LIMIT_POS         6u
#define NX_KTM5220_ABZ_LIMIT_MASK        0xC0u
#define NX_KTM5220_ABZ_PHASE_POS         4u
#define NX_KTM5220_ABZ_PHASE_MASK        0x30u
#define NX_KTM5220_Z_EDGE_MASK           0x08u
#define NX_KTM5220_Z_WIDTH_MASK          0x07u
#define NX_KTM5220_WEAK_MAG_MASK         0x10u
#define NX_KTM5220_ABZ_START_MASK        0x04u
#define NX_KTM5220_ABZ_DIR_MASK          0x02u
#define NX_KTM5220_ABZ_EN_MASK           0x01u
#define NX_KTM5220_UVW_PPT_POS           2u
#define NX_KTM5220_UVW_PPT_MASK          0xFCu
#define NX_KTM5220_IO_ORDER_MASK         0x02u
#define NX_KTM5220_UVW_EN_MASK           0x01u

/* Angle response alarm bits, all active high. */
#define NX_KTM5220_STATUS_SPEED_OR_MEMORY 0x01u
#define NX_KTM5220_STATUS_ADC_AMPLITUDE   0x02u
#define NX_KTM5220_STATUS_UNDERVOLTAGE    0x04u

#define NX_KTM5220_ANLC_IDLE     0u
#define NX_KTM5220_ANLC_RUNNING  1u
#define NX_KTM5220_ANLC_FAILED   2u
#define NX_KTM5220_ANLC_DONE     3u

typedef enum {
    NX_KTM5220_OK = 0,
    NX_KTM5220_ERR_PARAM,   /**< NULL argument, invalid range or unlock access */
    NX_KTM5220_ERR_IO,      /**< Busy port, callback failure or bad acknowledgement */
    NX_KTM5220_ERR_CRC,     /**< Response CRC mismatch */
    NX_KTM5220_ERR_LOCKED,  /**< A write requires a successful unlock first */
    NX_KTM5220_ERR_STATUS   /**< read_angle received a sensor alarm */
} nx_ktm5220_ret_t;

/**
 * @brief Synchronous transport configuration.
 *
 * cs(ctx, true) asserts CSN low; cs(ctx, false) releases it high.
 * transfer(ctx, tx, rx, len) simultaneously sends and receives len bytes,
 * exactly once per CSN window. Both buffers are non-NULL, distinct and valid
 * only until the callback returns. The callback must finish all wire activity
 * before returning, including when DMA is used; false reports a port failure.
 * It must leave CSN under the driver's control.
 *
 * The driver supplies the entire MOSI frame: command followed by zero dummy
 * bytes to clock out the response. The receive buffer includes the command
 * phase's ignored MISO bytes, then the response. Angle reads use 6 bytes,
 * register reads 4, and register writes/MTP commands 3. The driver attempts
 * to release CSN on every transfer failure; failure to release CSN is itself
 * reported as ERR_IO.
 *
 * is_busy is optional, checked once before each frame; NULL assumes ready.
 * delay_ns is optional and requests 13 ns before releasing CSN, satisfying
 * TH >= 12.5 ns (last SCK rising edge to CSN rising edge). With NULL, the port
 * must guarantee TH itself. No undocumented inter-frame delay is imposed.
 *
 * crc_invert must match the hardware SPI_CRC setting: false (power-up default)
 * uses CRC-8/poly 0x07/init 0x00/xorout 0x00; true uses xorout 0xFF. No bit
 * reflection is used. The v0.5 manual does not publish SPI_CRC's register
 * location or explicitly define CRC coverage; this driver checks response
 * data bytes (including status for angle samples), excluding the command.
 * Confirm coverage against hardware when bringing up a new device revision.
 */
typedef struct {
    bool (*cs)(void *ctx, bool assert);                        /**< Required */
    bool (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx,
                     size_t len);                             /**< Required */
    bool (*is_busy)(void *ctx);                                /**< Optional */
    void (*delay_ns)(uint32_t ns);                             /**< Optional */
    void *io_ctx;
    bool crc_invert;
} nx_ktm5220_cfg_t;

typedef struct {
    nx_ktm5220_cfg_t cfg;
    bool unlocked; /**< Cached; use unlock again if the chip may have reset. */
} nx_ktm5220_t;

typedef struct {
    uint32_t raw;   /**< 21-bit angle: 0 .. NX_KTM5220_ANGLE_MAX */
    uint8_t status; /**< Three NX_KTM5220_STATUS_* alarm bits */
} nx_ktm5220_sample_t;

/** Copy cfg, initially locked. Returns false for NULL/missing callbacks.
 * Does not communicate with or reset the chip. All other APIs require init. */
bool nx_ktm5220_init(nx_ktm5220_t *dev, const nx_ktm5220_cfg_t *cfg);

/** One coherent angle/status frame. Returns OK on valid CRC even if status
 * reports alarms; inspect out->status. Output is untouched on failure. */
nx_ktm5220_ret_t nx_ktm5220_read_sample(nx_ktm5220_t *dev,
                                      nx_ktm5220_sample_t *out);

/** Read angle only. Sensor alarms return ERR_STATUS; output is untouched on
 * every error. Use read_sample when the alarm bits are needed. */
nx_ktm5220_ret_t nx_ktm5220_read_angle(nx_ktm5220_t *dev, uint32_t *out_raw);

/** Convert a valid 21-bit raw code to 0 .. 359999 millidegrees. */
static inline uint32_t nx_ktm5220_raw_to_mdeg(uint32_t raw)
{
    return (uint32_t)(((uint64_t)raw * 360000u) >> NX_KTM5220_ANGLE_BITS);
}

/** CRC-checked register read, permitted while locked. Output unchanged on error. */
nx_ktm5220_ret_t nx_ktm5220_read_reg8(nx_ktm5220_t *dev, uint8_t addr, uint8_t *val);

/** Write one byte after unlock. The chip supplies no echo or CRC for writes;
 * OK means the frame was sent, not that the register accepted the value.
 * Preserve reserved bits with update_reg8. Direct writes to WRITE_UNLOCK
 * are rejected; use unlock instead. The driver sends no implicit MTP command;
 * register-triggered side effects (including calibration saves) follow the chip. */
nx_ktm5220_ret_t nx_ktm5220_write_reg8(nx_ktm5220_t *dev, uint8_t addr, uint8_t val);

/** Read two adjacent bytes, low byte first, using separate CRC-checked frames.
 * Rejects addr=0xFF; output unchanged on error. Not an atomic snapshot. */
nx_ktm5220_ret_t nx_ktm5220_read_reg16(nx_ktm5220_t *dev, uint8_t addr, uint16_t *val);

/** Write low byte then high byte in separate frames. Rejects address wrap and
 * either byte touching WRITE_UNLOCK. An error may leave a partial write. */
nx_ktm5220_ret_t nx_ktm5220_write_reg16(nx_ktm5220_t *dev, uint8_t addr, uint16_t val);

/** Read-modify-write: new = (old & ~mask) | (val & mask). Other bits survive.
 * Requires unlock. The two frames must be serialized by the caller. */
nx_ktm5220_ret_t nx_ktm5220_update_reg8(nx_ktm5220_t *dev, uint8_t addr,
                                      uint8_t mask, uint8_t val);

/** Write 0xAA to 0x1A and verify a CRC-checked readback of 0x01. On any error,
 * cached state becomes locked. Safe to repeat. The manual documents locking
 * only through power-down; no software lock command is exposed. */
nx_ktm5220_ret_t nx_ktm5220_unlock(nx_ktm5220_t *dev);

/** Read ZERO[20:0] from three registers. Output unchanged on error; not atomic. */
nx_ktm5220_ret_t nx_ktm5220_get_zero(nx_ktm5220_t *dev, uint32_t *out_raw);

/** Set 21-bit zero offset, preserving ZERO_H's reserved bits. Requires unlock.
 * Rejects raw > ANGLE_MAX. Multiple frames; an error may leave a partial write.
 * Does not capture the current angle or program MTP. */
nx_ktm5220_ret_t nx_ktm5220_set_zero(nx_ktm5220_t *dev, uint32_t raw);

/** Trigger ZERO_SET[0] to capture the current position as zero. Self-clearing;
 * returns after sending the trigger, without polling or programming MTP. */
nx_ktm5220_ret_t nx_ktm5220_zero_here(nx_ktm5220_t *dev);

/** Set/clear CAL_REG_SET[0] while preserving other bits. Requires unlock.
 * Caller manages motor speed, polling/timeouts and power. The manual conflicts
 * on automatic persistence for register-triggered calibration (9.2.1/9.2.3);
 * this API only sends the trigger and does not issue a separate MTP command. */
nx_ktm5220_ret_t nx_ktm5220_set_calibration(nx_ktm5220_t *dev, bool enable);

/** Read ANLC_STATUS[1:0], one of NX_KTM5220_ANLC_*. Output unchanged on error. */
nx_ktm5220_ret_t nx_ktm5220_read_calibration_status(nx_ktm5220_t *dev, uint8_t *status);

/** Send C0 00 and verify the single-byte 0x55 acknowledgement. Requires unlock.
 * OK confirms command acknowledgement, not completion. After any attempted burn
 * that reached the chip (even if the response is lost), maintain stable power,
 * wait >= MTP_PROGRAM_TIME_MS, then power-cycle before further operations.
 * The driver does not wait, track elapsed time, or program MTP automatically.
 */
nx_ktm5220_ret_t nx_ktm5220_write_mtp(nx_ktm5220_t *dev);

/** Query the SPI port, not calibration/MTP completion. NULL/no callback: false. */
bool nx_ktm5220_busy(nx_ktm5220_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* NX_KTM5220_H */
