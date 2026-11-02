/**
 * @file    nx_mp6546.h
 * @brief   MP6546 three-phase motor driver through 16-bit register callbacks.
 *
 * Based on MP6546 Rev. 1.0 (2023-03-20). The chip implements voltage-mode
 * FOC, not a current, speed or position controller. No memory is allocated,
 * no floating point is used, and no platform headers are required.
 *
 * The public manual omits the complete I2C wire protocol and byte order.
 * The port must supply verified, synchronous register transactions; this
 * module implements register semantics without guessing the wire format.
 *
 * The caller owns the handle and serializes complete API calls, including
 * read-modify-write and multi-register operations. A failed write may have
 * reached the chip. There is no retry, rollback, automatic shutdown, OTP
 * programming, or fault-clear command. The application controls EN, timing,
 * calibration and recovery. All APIs other than init require successful init.
 */
#ifndef NX_MP6546_H
#define NX_MP6546_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_MP6546_ADDR_LOW              0x07u
#define NX_MP6546_ADDR_MID              0x08u
#define NX_MP6546_ADDR_HIGH             0x09u
#define NX_MP6546_I2C_MAX_HZ            1000000u
#define NX_MP6546_ANGLE_BITS            15u
#define NX_MP6546_ANGLE_MAX             0x7FFFu
#define NX_MP6546_TEST_ANGLE_MAX        0x0FFFu
#define NX_MP6546_VOLTAGE_MIN           (-512)
#define NX_MP6546_VOLTAGE_MAX           511

/* Each address names ONE 16-bit register, not two adjacent byte registers.
 * Ud is 08h per the register map and p12; the p17 heading says 09h in error. */
#define NX_MP6546_REG_CONTROL1          0x00u
#define NX_MP6546_REG_ADC_BIAS          0x01u
#define NX_MP6546_REG_THETA_BIAS        0x02u
#define NX_MP6546_REG_CONTROL2          0x03u
#define NX_MP6546_REG_ANGLE_TEST        0x07u
#define NX_MP6546_REG_D_AXIS_VOLTAGE    0x08u
#define NX_MP6546_REG_Q_AXIS_VOLTAGE    0x09u
#define NX_MP6546_REG_ALIGN_MOD         0x0Au
#define NX_MP6546_REG_ANGLE             0x0Cu
#define NX_MP6546_REG_FAULT_FLAG        0x0Du
#define NX_MP6546_REG_NOR_MOD           0x0Fu
#define NX_MP6546_REG_STB_MOD           0x10u
#define NX_MP6546_REG_SPI_CMD           0x11u
#define NX_MP6546_REG_INL_FIRST         0x13u
#define NX_MP6546_REG_INL_LAST          0x1Eu

/* Positioned masks for CONTROL1 (00h). No frequency enum is provided for
 * FLT_N_SVM: the manual conflicts on its width and frequency encodings. */
#define NX_MP6546_OCP_EN_MASK           0x8000u
#define NX_MP6546_SCP_EN_MASK           0x4000u
#define NX_MP6546_STDBY_EN_MASK         0x2000u
#define NX_MP6546_ADDR_MASK             0x1800u
#define NX_MP6546_ADDR_POS              11u
#define NX_MP6546_OCP_SEL_MASK          0x0400u
#define NX_MP6546_AMOD_MASK             0x0200u
#define NX_MP6546_THETA_DIR_MASK        0x0100u
#define NX_MP6546_OVP_EN_MASK           0x0080u
#define NX_MP6546_FLT_N_SVM_MASK        0x0060u
#define NX_MP6546_DRV_MOD_MASK          0x0010u
#define NX_MP6546_SPI_3_MASK            0x0008u
#define NX_MP6546_FLT_N_MASK            0x0007u

#define NX_MP6546_VA_BIAS_MASK          0xFF00u
#define NX_MP6546_VA_BIAS_POS           8u
#define NX_MP6546_VB_BIAS_MASK          0x00FFu
#define NX_MP6546_THETA_BIAS_MASK       0xFFF0u
#define NX_MP6546_THETA_BIAS_POS        4u
#define NX_MP6546_POLE_PAIR_MASK        0x000Fu
#define NX_MP6546_SPI_BAUD_MASK         0xE000u
#define NX_MP6546_SPI_BAUD_POS          13u
#define NX_MP6546_TRIM_ENP_MASK         0x0100u
#define NX_MP6546_ALN_MD_MASK           0x0400u
#define NX_MP6546_SAMPLE_FAULT_MASK     0x8000u

/* read_faults returns these positioned 0Dh bits, excluding INL_COMP[7:0]. */
#define NX_MP6546_FAULT_SCP             0x0800u
#define NX_MP6546_FAULT_TSD             0x0400u
#define NX_MP6546_FAULT_OVP             0x0200u
#define NX_MP6546_FAULT_OCP             0x0100u
#define NX_MP6546_FAULT_MASK            0x0F00u

typedef enum {
    NX_MP6546_OK = 0,
    NX_MP6546_ERR_PARAM,
    NX_MP6546_ERR_IO,
    NX_MP6546_ERR_STATUS
} nx_mp6546_ret_t;

/** Required synchronous port callbacks.
 * addr7 is unshifted, without the R/W bit. value has the numeric bit[15:0]
 * layout from the manual regardless of CPU endianness. The port owns byte
 * order, START/repeated START/STOP, ACK, bus release, and bounded timeouts.
 * Even DMA-backed ports must finish all wire activity before returning.
 * Callback false reports a port failure, including a busy bus; a write may
 * nevertheless have taken effect. Success does not imply persistent storage.
 * cfg is copied; io_ctx must outlive the handle. NULL io_ctx is permitted.
 */
typedef struct {
    bool (*read_reg16)(void *ctx, uint8_t addr7, uint8_t reg, uint16_t *value);
    bool (*write_reg16)(void *ctx, uint8_t addr7, uint8_t reg, uint16_t value);
    void *io_ctx;
    uint8_t addr7;
} nx_mp6546_cfg_t;

typedef struct {
    nx_mp6546_cfg_t cfg;
} nx_mp6546_t;

typedef struct {
    uint16_t angle_raw; /**< 15-bit mechanical angle; resolution is not accuracy. */
    bool fault;         /**< From the SAME register read as angle_raw. */
} nx_mp6546_sample_t;

typedef enum {
    NX_MP6546_SENSOR_SPI = 0,
    NX_MP6546_SENSOR_SSC,
    NX_MP6546_SENSOR_LINEAR_HALL
} nx_mp6546_sensor_mode_t;

typedef enum {
    NX_MP6546_DRIVE_VECTOR = 0,
    NX_MP6546_DRIVE_DUTY
} nx_mp6546_drive_mode_t;

typedef enum {
    NX_MP6546_ANGLE_SENSOR = 0,
    NX_MP6546_ANGLE_TEST
} nx_mp6546_angle_source_t;

typedef struct {
    bool enabled;
    uint16_t duty_permille; /**< 0..1000; rounded to the hardware's 8-bit steps. */
} nx_mp6546_phase_t;

/** Validate/copy cfg, without I/O, reset, probing, EN changes or motor start.
 * Only 07h/08h/09h addresses are accepted. On failure dev is unchanged. */
bool nx_mp6546_init(nx_mp6546_t *dev, const nx_mp6546_cfg_t *cfg);

/** Raw accesses reject undocumented addresses, RO writes and command reads.
 * Read outputs are unchanged on error. Raw writes are an expert escape hatch:
 * preserve reserved bits and manage mode/address/command side effects yourself.
 * After changing the hardware address, reinitialize with its verified address;
 * the driver does not probe or rebind automatically, even on write success.
 * No explicit OTP programming sequence is issued by any API.
 */
nx_mp6546_ret_t nx_mp6546_read_reg16(nx_mp6546_t *dev, uint8_t reg, uint16_t *value);
nx_mp6546_ret_t nx_mp6546_write_reg16(nx_mp6546_t *dev, uint8_t reg, uint16_t value);

/** new = (old & ~mask) | (value & mask), preserving all other bits.
 * Only ordinary writable fields are accepted: 00h/01h/02h, SPI_BAUD and
 * TRIM_ENP in 03h, low 12 bits of 07h, low 10 of 08h/09h, ALN_MD in 0Ah,
 * and INL data. Command/RO/production-code/reserved fields are rejected.
 * Zero mask performs no I/O. A failed read never causes a write; a failed
 * write may have taken effect. Serialize the whole read-modify-write.
 */
nx_mp6546_ret_t nx_mp6546_update_reg16(nx_mp6546_t *dev, uint8_t reg,
                                     uint16_t mask, uint16_t value);

/** One 0Ch read. OK also when fault=true; output unchanged on I/O error.
 * Reads the chip's angle register only: no external sensor transaction is
 * triggered and no timestamp/data-freshness guarantee is available. In
 * SPI/SSC mode the application must select angle reading (SPI_CMD=0) after
 * sensor-register commands, as described on p11 of the manual. */
nx_mp6546_ret_t nx_mp6546_read_sample(nx_mp6546_t *dev, nx_mp6546_sample_t *out);

/** ERR_STATUS on FAULT; angle output unchanged on EVERY error. */
nx_mp6546_ret_t nx_mp6546_read_angle(nx_mp6546_t *dev, uint16_t *out_raw);

/** One 0Dh read, masked to FAULT_MASK. Not simultaneous with read_sample.
 * No clearing is attempted; the manual does not define flag-clear semantics. */
nx_mp6546_ret_t nx_mp6546_read_faults(nx_mp6546_t *dev, uint16_t *out_flags);

/** Convert a valid 15-bit raw angle to 0..359989 millidegrees. */
static inline uint32_t nx_mp6546_raw_to_mdeg(uint16_t raw)
{
    return (uint32_t)(((uint64_t)raw * 360000u) >> NX_MP6546_ANGLE_BITS);
}

/** Encode -512..511 as 10-bit two's complement, writing Ud(08h) then Uq(09h).
 * Upper reserved bits of these command words are sent as zero. Vector mode
 * must already be selected; this call neither checks nor changes the mode.
 * The writes are NOT guaranteed atomic. On any I/O error output may be partly
 * updated/unknown. No retry or rollback is attempted; EN remains with the app.
 * The application limits voltage-vector magnitude, slew, current and stall.
 */
nx_mp6546_ret_t nx_mp6546_set_dq(nx_mp6546_t *dev, int16_t ud, int16_t uq);

/** Write only Uq (09h), same encoding and prerequisites as set_dq. */
nx_mp6546_ret_t nx_mp6546_set_uq(nx_mp6546_t *dev, int16_t uq);

/** Write A/B/C to 07h/08h/09h in duty mode (selected by the application).
 * Validate ALL phases before I/O, even a disabled phase's duty. A disabled
 * channel encodes as zero; enabled 0/1000 permille encode as 1FFh/100h.
 * Writing 09h commits the group per p12-13. An error may leave pending or
 * effective values; resubmit the WHOLE group after application recovery.
 */
nx_mp6546_ret_t nx_mp6546_set_pwm(nx_mp6546_t *dev,
                                const nx_mp6546_phase_t phases[3]);

/** Pure integer conversion: round(512*sqrt(3)*voltage_mv/bus_voltage_mv).
 * Uses factor 886.810013 and 64-bit intermediates. Rejects zero bus voltage
 * and a rounded result outside -512..511; no clipping. Output unchanged on
 * error. Caller supplies measured bus voltage and enforces vector limits.
 */
nx_mp6546_ret_t nx_mp6546_voltage_to_raw(int32_t voltage_mv,
                                       uint32_t bus_voltage_mv, int16_t *out_raw);

/** Sensor/drive/source setters preserve unrelated and reserved bits. They
 * do NOT sequence a safe mode transition or initialize shared 07h..09h
 * contents. Change modes only under application control with suitable EN,
 * timing and output commands; EN low completely shuts down the chip, so
 * configuring an unpowered/disabled chip is not assumed to work. Selecting
 * SPI/SSC changes only AMOD/SPI_3, not SPI_BAUD or SPI_CMD; see read_sample.
 */
nx_mp6546_ret_t nx_mp6546_set_sensor_mode(nx_mp6546_t *dev,
                                        nx_mp6546_sensor_mode_t mode);
nx_mp6546_ret_t nx_mp6546_set_drive_mode(nx_mp6546_t *dev,
                                       nx_mp6546_drive_mode_t mode);
nx_mp6546_ret_t nx_mp6546_set_angle_source(nx_mp6546_t *dev,
                                         nx_mp6546_angle_source_t source);

/** Set 1..15 pole pairs and 12-bit MECHANICAL bias in 02h, then direction
 * in 00h. No alignment or bias-sign inference. Multiple accesses: failure
 * can leave partial configuration. Preserve CONTROL1's other bits. */
nx_mp6546_ret_t nx_mp6546_set_motor_angle(nx_mp6546_t *dev, uint8_t pole_pairs,
                                        uint16_t bias, bool reverse);

/** Set 12-bit ANGLE_T, preserving 07h's upper bits. Vector mode only;
 * fixed angle is used only after explicitly selecting ANGLE_TEST source. */
nx_mp6546_ret_t nx_mp6546_set_test_angle(nx_mp6546_t *dev, uint16_t raw);

/** Issue work command 0Fh. Does not reset, clear faults or wait for readiness. */
nx_mp6546_ret_t nx_mp6546_enter_normal(nx_mp6546_t *dev);

/** Set STDBY_EN, then issue 10h. LDO behavior follows CONTROL2.TRIM_ENP.
 * No EN change or delay. SCL/SCK falling edges can wake the chip (p13), so
 * traffic on a shared bus may end standby; this is not a shutdown guarantee. */
nx_mp6546_ret_t nx_mp6546_enter_standby(nx_mp6546_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* NX_MP6546_H */
