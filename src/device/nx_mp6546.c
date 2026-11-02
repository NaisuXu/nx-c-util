/**
 * @file    nx_mp6546.c
 * @brief   MP6546 register semantics, voltage commands and status decoding.
 */
#include "nx_mp6546.h"

#include <stddef.h>

static bool mp_address_valid(uint8_t addr7)
{
    return addr7 >= NX_MP6546_ADDR_LOW && addr7 <= NX_MP6546_ADDR_HIGH;
}

static bool mp_valid(const nx_mp6546_t *dev)
{
    return dev != NULL && dev->cfg.read_reg16 != NULL &&
           dev->cfg.write_reg16 != NULL && mp_address_valid(dev->cfg.addr7);
}

static bool mp_readable(uint8_t reg)
{
    return reg <= NX_MP6546_REG_CONTROL2 ||
           (reg >= NX_MP6546_REG_ANGLE_TEST && reg <= NX_MP6546_REG_ALIGN_MOD) ||
           reg == NX_MP6546_REG_ANGLE || reg == NX_MP6546_REG_FAULT_FLAG ||
           reg == NX_MP6546_REG_SPI_CMD ||
           (reg >= NX_MP6546_REG_INL_FIRST && reg <= NX_MP6546_REG_INL_LAST);
}

static bool mp_writable(uint8_t reg)
{
    return (mp_readable(reg) && reg != NX_MP6546_REG_ANGLE &&
            reg != NX_MP6546_REG_FAULT_FLAG) ||
           reg == NX_MP6546_REG_NOR_MOD || reg == NX_MP6546_REG_STB_MOD;
}

/* Only fields whose ordinary read-modify-write semantics are documented.
 * Raw write_reg16 remains available for SPI commands and expert use. */
static uint16_t mp_update_mask(uint8_t reg)
{
    switch (reg) {
    case NX_MP6546_REG_CONTROL1:
    case NX_MP6546_REG_ADC_BIAS:
    case NX_MP6546_REG_THETA_BIAS:
        return 0xFFFFu;
    case NX_MP6546_REG_CONTROL2:
        return NX_MP6546_SPI_BAUD_MASK | NX_MP6546_TRIM_ENP_MASK;
    case NX_MP6546_REG_ANGLE_TEST:
        return 0x0FFFu;
    case NX_MP6546_REG_D_AXIS_VOLTAGE:
    case NX_MP6546_REG_Q_AXIS_VOLTAGE:
        return 0x03FFu;
    case NX_MP6546_REG_ALIGN_MOD:
        return NX_MP6546_ALN_MD_MASK;
    default:
        return reg >= NX_MP6546_REG_INL_FIRST && reg <= NX_MP6546_REG_INL_LAST
                   ? 0xFFFFu : 0u;
    }
}

static bool mp_voltage_valid(int16_t value)
{
    return value >= NX_MP6546_VOLTAGE_MIN && value <= NX_MP6546_VOLTAGE_MAX;
}

/* Converting a negative signed integer to uint16_t is defined modulo 2^16;
 * masking then produces the chip's 10-bit two's-complement representation. */
static uint16_t mp_voltage_word(int16_t value)
{
    return (uint16_t)((uint16_t)value & 0x03FFu);
}

bool nx_mp6546_init(nx_mp6546_t *dev, const nx_mp6546_cfg_t *cfg)
{
    if (dev == NULL || cfg == NULL || cfg->read_reg16 == NULL ||
        cfg->write_reg16 == NULL || !mp_address_valid(cfg->addr7)) {
        return false;
    }
    dev->cfg = *cfg;
    return true;
}

nx_mp6546_ret_t nx_mp6546_read_reg16(nx_mp6546_t *dev, uint8_t reg, uint16_t *value)
{
    if (!mp_valid(dev) || value == NULL || !mp_readable(reg)) {
        return NX_MP6546_ERR_PARAM;
    }
    uint16_t word = 0u;
    if (!dev->cfg.read_reg16(dev->cfg.io_ctx, dev->cfg.addr7, reg, &word)) {
        return NX_MP6546_ERR_IO;
    }
    *value = word;
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_write_reg16(nx_mp6546_t *dev, uint8_t reg, uint16_t value)
{
    if (!mp_valid(dev) || !mp_writable(reg)) {
        return NX_MP6546_ERR_PARAM;
    }
    return dev->cfg.write_reg16(dev->cfg.io_ctx, dev->cfg.addr7, reg, value)
               ? NX_MP6546_OK : NX_MP6546_ERR_IO;
}

nx_mp6546_ret_t nx_mp6546_update_reg16(nx_mp6546_t *dev, uint8_t reg,
                                     uint16_t mask, uint16_t value)
{
    const uint16_t allowed = mp_update_mask(reg);
    if (!mp_valid(dev) || allowed == 0u || (mask & (uint16_t)~allowed) != 0u) {
        return NX_MP6546_ERR_PARAM;
    }
    if (mask == 0u) {
        return NX_MP6546_OK;
    }
    uint16_t old;
    nx_mp6546_ret_t ret = nx_mp6546_read_reg16(dev, reg, &old);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    const uint16_t word = (uint16_t)((old & (uint16_t)~mask) | (value & mask));
    return nx_mp6546_write_reg16(dev, reg, word);
}

nx_mp6546_ret_t nx_mp6546_read_sample(nx_mp6546_t *dev, nx_mp6546_sample_t *out)
{
    if (out == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    uint16_t word;
    nx_mp6546_ret_t ret = nx_mp6546_read_reg16(dev, NX_MP6546_REG_ANGLE, &word);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    out->angle_raw = (uint16_t)(word & NX_MP6546_ANGLE_MAX);
    out->fault = (word & NX_MP6546_SAMPLE_FAULT_MASK) != 0u;
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_read_angle(nx_mp6546_t *dev, uint16_t *out_raw)
{
    if (out_raw == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    nx_mp6546_sample_t sample;
    nx_mp6546_ret_t ret = nx_mp6546_read_sample(dev, &sample);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    if (sample.fault) {
        return NX_MP6546_ERR_STATUS;
    }
    *out_raw = sample.angle_raw;
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_read_faults(nx_mp6546_t *dev, uint16_t *out_flags)
{
    if (out_flags == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    uint16_t word;
    nx_mp6546_ret_t ret = nx_mp6546_read_reg16(dev, NX_MP6546_REG_FAULT_FLAG, &word);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    *out_flags = (uint16_t)(word & NX_MP6546_FAULT_MASK);
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_set_dq(nx_mp6546_t *dev, int16_t ud, int16_t uq)
{
    if (!mp_valid(dev) || !mp_voltage_valid(ud) || !mp_voltage_valid(uq)) {
        return NX_MP6546_ERR_PARAM;
    }
    nx_mp6546_ret_t ret = nx_mp6546_write_reg16(
        dev, NX_MP6546_REG_D_AXIS_VOLTAGE, mp_voltage_word(ud));
    return ret == NX_MP6546_OK ? nx_mp6546_set_uq(dev, uq) : ret;
}

nx_mp6546_ret_t nx_mp6546_set_uq(nx_mp6546_t *dev, int16_t uq)
{
    if (!mp_voltage_valid(uq)) {
        return NX_MP6546_ERR_PARAM;
    }
    return nx_mp6546_write_reg16(dev, NX_MP6546_REG_Q_AXIS_VOLTAGE,
                                mp_voltage_word(uq));
}

nx_mp6546_ret_t nx_mp6546_set_pwm(nx_mp6546_t *dev,
                                const nx_mp6546_phase_t phases[3])
{
    if (!mp_valid(dev) || phases == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    uint16_t words[3];
    for (unsigned i = 0u; i < 3u; ++i) {
        if (phases[i].duty_permille > 1000u) {
            return NX_MP6546_ERR_PARAM;
        }
        const uint32_t steps = ((uint32_t)phases[i].duty_permille * 255u + 500u) / 1000u;
        words[i] = phases[i].enabled ? (uint16_t)(0x100u | (255u - steps)) : 0u;
    }
    for (unsigned i = 0u; i < 3u; ++i) {
        nx_mp6546_ret_t ret = nx_mp6546_write_reg16(
            dev, (uint8_t)(NX_MP6546_REG_ANGLE_TEST + i), words[i]);
        if (ret != NX_MP6546_OK) {
            return ret;
        }
    }
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_voltage_to_raw(int32_t voltage_mv,
                                       uint32_t bus_voltage_mv, int16_t *out_raw)
{
    if (bus_voltage_mv == 0u || out_raw == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    /* 512*sqrt(3) ~= 886.810013. Even INT32_MIN times this numerator and
     * UINT32_MAX times the denominator fit in int64_t, including rounding. */
    const int64_t num = (int64_t)voltage_mv * 886810013LL;
    const int64_t den = (int64_t)bus_voltage_mv * 1000000LL;
    const int64_t rounded = (num >= 0 ? num + den / 2 : num - den / 2) / den;
    if (rounded < NX_MP6546_VOLTAGE_MIN || rounded > NX_MP6546_VOLTAGE_MAX) {
        return NX_MP6546_ERR_PARAM;
    }
    *out_raw = (int16_t)rounded;
    return NX_MP6546_OK;
}

nx_mp6546_ret_t nx_mp6546_set_sensor_mode(nx_mp6546_t *dev,
                                        nx_mp6546_sensor_mode_t mode)
{
    uint16_t value;
    switch (mode) {
    case NX_MP6546_SENSOR_SPI:         value = 0u; break;
    case NX_MP6546_SENSOR_SSC:         value = NX_MP6546_SPI_3_MASK; break;
    case NX_MP6546_SENSOR_LINEAR_HALL: value = NX_MP6546_AMOD_MASK; break;
    default: return NX_MP6546_ERR_PARAM;
    }
    return nx_mp6546_update_reg16(dev, NX_MP6546_REG_CONTROL1,
                                 NX_MP6546_AMOD_MASK | NX_MP6546_SPI_3_MASK, value);
}

nx_mp6546_ret_t nx_mp6546_set_drive_mode(nx_mp6546_t *dev,
                                       nx_mp6546_drive_mode_t mode)
{
    if (mode != NX_MP6546_DRIVE_VECTOR && mode != NX_MP6546_DRIVE_DUTY) {
        return NX_MP6546_ERR_PARAM;
    }
    return nx_mp6546_update_reg16(dev, NX_MP6546_REG_CONTROL1,
                                 NX_MP6546_DRV_MOD_MASK,
                                 mode == NX_MP6546_DRIVE_DUTY ? NX_MP6546_DRV_MOD_MASK : 0u);
}

nx_mp6546_ret_t nx_mp6546_set_angle_source(nx_mp6546_t *dev,
                                         nx_mp6546_angle_source_t source)
{
    if (source != NX_MP6546_ANGLE_SENSOR && source != NX_MP6546_ANGLE_TEST) {
        return NX_MP6546_ERR_PARAM;
    }
    return nx_mp6546_update_reg16(dev, NX_MP6546_REG_ALIGN_MOD,
                                 NX_MP6546_ALN_MD_MASK,
                                 source == NX_MP6546_ANGLE_TEST ? NX_MP6546_ALN_MD_MASK : 0u);
}

nx_mp6546_ret_t nx_mp6546_set_motor_angle(nx_mp6546_t *dev, uint8_t pole_pairs,
                                        uint16_t bias, bool reverse)
{
    if (!mp_valid(dev) || pole_pairs == 0u || pole_pairs > 15u ||
        bias > NX_MP6546_TEST_ANGLE_MAX) {
        return NX_MP6546_ERR_PARAM;
    }
    const uint16_t word = (uint16_t)((bias << NX_MP6546_THETA_BIAS_POS) | pole_pairs);
    nx_mp6546_ret_t ret = nx_mp6546_write_reg16(dev, NX_MP6546_REG_THETA_BIAS, word);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    return nx_mp6546_update_reg16(dev, NX_MP6546_REG_CONTROL1,
                                 NX_MP6546_THETA_DIR_MASK,
                                 reverse ? NX_MP6546_THETA_DIR_MASK : 0u);
}

nx_mp6546_ret_t nx_mp6546_set_test_angle(nx_mp6546_t *dev, uint16_t raw)
{
    if (raw > NX_MP6546_TEST_ANGLE_MAX) {
        return NX_MP6546_ERR_PARAM;
    }
    return nx_mp6546_update_reg16(dev, NX_MP6546_REG_ANGLE_TEST, 0x0FFFu, raw);
}

nx_mp6546_ret_t nx_mp6546_enter_normal(nx_mp6546_t *dev)
{
    return nx_mp6546_write_reg16(dev, NX_MP6546_REG_NOR_MOD, 0u);
}

nx_mp6546_ret_t nx_mp6546_enter_standby(nx_mp6546_t *dev)
{
    nx_mp6546_ret_t ret = nx_mp6546_update_reg16(dev, NX_MP6546_REG_CONTROL1,
                                              NX_MP6546_STDBY_EN_MASK,
                                              NX_MP6546_STDBY_EN_MASK);
    return ret == NX_MP6546_OK
               ? nx_mp6546_write_reg16(dev, NX_MP6546_REG_STB_MOD, 0u) : ret;
}
