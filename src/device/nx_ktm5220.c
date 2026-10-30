/**
 * @file    nx_ktm5220.c
 * @brief   KTM5220 SPI framing, CRC, configuration and zero/calibration access.
 */
#include "nx_ktm5220.h"

#define KTM_CMD_READ_REG    0x30u
#define KTM_CMD_WRITE_REG   0x60u
#define KTM_CS_HOLD_NS      13u  /* TH >= 12.5 ns, datasheet table 21. */

/* MSB-first polynomial 0x07, four bits per lookup. */
static const uint8_t KTM_CRC8_NIBBLE[16] = {
    0x00u, 0x07u, 0x0Eu, 0x09u, 0x1Cu, 0x1Bu, 0x12u, 0x15u,
    0x38u, 0x3Fu, 0x36u, 0x31u, 0x24u, 0x23u, 0x2Au, 0x2Du
};

static uint8_t ktm_crc8(const nx_ktm5220_t *dev, const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= data[i];
        crc = (uint8_t)((crc << 4) ^ KTM_CRC8_NIBBLE[crc >> 4]);
        crc = (uint8_t)((crc << 4) ^ KTM_CRC8_NIBBLE[crc >> 4]);
    }
    return dev->cfg.crc_invert ? (uint8_t)(crc ^ 0xFFu) : crc;
}

static bool ktm_frame_end(nx_ktm5220_t *dev)
{
    if (dev->cfg.delay_ns != NULL) {
        dev->cfg.delay_ns(KTM_CS_HOLD_NS);
    }
    return dev->cfg.cs(dev->cfg.io_ctx, false);
}

/* One full-duplex call clocks the complete command/response frame. Even an
 * unsuccessful CS assertion may have changed the pin, so try to release it. */
static nx_ktm5220_ret_t ktm_transact(nx_ktm5220_t *dev,
                                    const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (dev->cfg.is_busy != NULL && dev->cfg.is_busy(dev->cfg.io_ctx)) {
        return NX_KTM5220_ERR_IO;
    }
    if (!dev->cfg.cs(dev->cfg.io_ctx, true)) {
        (void)ktm_frame_end(dev);
        return NX_KTM5220_ERR_IO;
    }

    bool ok = dev->cfg.transfer(dev->cfg.io_ctx, tx, rx, len);
    if (!ktm_frame_end(dev)) {
        ok = false;
    }
    return ok ? NX_KTM5220_OK : NX_KTM5220_ERR_IO;
}

/* The first two bytes pack CMD[3:0], ADDR[7:0], IDLE[3:0]. */
static nx_ktm5220_ret_t ktm_write_raw(nx_ktm5220_t *dev, uint8_t addr, uint8_t val)
{
    const uint8_t tx[3] = {
        (uint8_t)(KTM_CMD_WRITE_REG | (addr >> 4)),
        (uint8_t)(addr << 4), val
    };
    uint8_t rx[3]; /* MISO is high impedance throughout a register write. */
    return ktm_transact(dev, tx, rx, sizeof tx);
}

static nx_ktm5220_ret_t ktm_writable(const nx_ktm5220_t *dev, uint8_t addr)
{
    if (dev == NULL || addr == NX_KTM5220_REG_WRITE_UNLOCK) {
        return NX_KTM5220_ERR_PARAM;
    }
    return dev->unlocked ? NX_KTM5220_OK : NX_KTM5220_ERR_LOCKED;
}

bool nx_ktm5220_init(nx_ktm5220_t *dev, const nx_ktm5220_cfg_t *cfg)
{
    if (dev == NULL || cfg == NULL || cfg->cs == NULL || cfg->transfer == NULL) {
        return false;
    }
    dev->cfg = *cfg;
    dev->unlocked = false;
    return true;
}

nx_ktm5220_ret_t nx_ktm5220_read_sample(nx_ktm5220_t *dev,
                                      nx_ktm5220_sample_t *out)
{
    if (dev == NULL || out == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    const uint8_t tx[6] = { 0xA0u, 0x00u, 0u, 0u, 0u, 0u };
    uint8_t rx[6] = { 0u };
    nx_ktm5220_ret_t ret = ktm_transact(dev, tx, rx, sizeof tx);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    if (ktm_crc8(dev, &rx[2], 3u) != rx[5]) {
        return NX_KTM5220_ERR_CRC;
    }
    uint32_t packed = ((uint32_t)rx[2] << 16) |
                      ((uint32_t)rx[3] << 8) | (uint32_t)rx[4];
    out->raw = packed >> 3;
    out->status = (uint8_t)(packed & 0x07u);
    return NX_KTM5220_OK;
}

nx_ktm5220_ret_t nx_ktm5220_read_angle(nx_ktm5220_t *dev, uint32_t *out_raw)
{
    if (dev == NULL || out_raw == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    nx_ktm5220_sample_t sample;
    nx_ktm5220_ret_t ret = nx_ktm5220_read_sample(dev, &sample);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    if (sample.status != 0u) {
        return NX_KTM5220_ERR_STATUS;
    }
    *out_raw = sample.raw;
    return NX_KTM5220_OK;
}

nx_ktm5220_ret_t nx_ktm5220_read_reg8(nx_ktm5220_t *dev, uint8_t addr, uint8_t *val)
{
    if (dev == NULL || val == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    const uint8_t tx[4] = {
        (uint8_t)(KTM_CMD_READ_REG | (addr >> 4)), (uint8_t)(addr << 4), 0u, 0u
    };
    uint8_t rx[4] = { 0u };
    nx_ktm5220_ret_t ret = ktm_transact(dev, tx, rx, sizeof tx);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    if (ktm_crc8(dev, &rx[2], 1u) != rx[3]) {
        return NX_KTM5220_ERR_CRC;
    }
    *val = rx[2];
    return NX_KTM5220_OK;
}

nx_ktm5220_ret_t nx_ktm5220_write_reg8(nx_ktm5220_t *dev, uint8_t addr, uint8_t val)
{
    nx_ktm5220_ret_t ret = ktm_writable(dev, addr);
    return ret == NX_KTM5220_OK ? ktm_write_raw(dev, addr, val) : ret;
}

nx_ktm5220_ret_t nx_ktm5220_read_reg16(nx_ktm5220_t *dev, uint8_t addr, uint16_t *val)
{
    if (dev == NULL || val == NULL || addr == 0xFFu) {
        return NX_KTM5220_ERR_PARAM;
    }
    uint8_t low = 0u;
    uint8_t high = 0u;
    nx_ktm5220_ret_t ret = nx_ktm5220_read_reg8(dev, addr, &low);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    ret = nx_ktm5220_read_reg8(dev, (uint8_t)(addr + 1u), &high);
    if (ret == NX_KTM5220_OK) {
        *val = (uint16_t)((uint16_t)low | ((uint16_t)high << 8));
    }
    return ret;
}

nx_ktm5220_ret_t nx_ktm5220_write_reg16(nx_ktm5220_t *dev, uint8_t addr, uint16_t val)
{
    /* Validate both addresses before sending either byte. */
    if (dev == NULL || addr == 0xFFu ||
        addr == NX_KTM5220_REG_WRITE_UNLOCK ||
        (uint8_t)(addr + 1u) == NX_KTM5220_REG_WRITE_UNLOCK) {
        return NX_KTM5220_ERR_PARAM;
    }
    nx_ktm5220_ret_t ret = nx_ktm5220_write_reg8(dev, addr, (uint8_t)val);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    return nx_ktm5220_write_reg8(dev, (uint8_t)(addr + 1u), (uint8_t)(val >> 8));
}

nx_ktm5220_ret_t nx_ktm5220_update_reg8(nx_ktm5220_t *dev, uint8_t addr,
                                      uint8_t mask, uint8_t val)
{
    nx_ktm5220_ret_t ret = ktm_writable(dev, addr);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    if (mask == 0u) {
        return NX_KTM5220_OK;
    }
    uint8_t old = 0u;
    ret = nx_ktm5220_read_reg8(dev, addr, &old);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    return ktm_write_raw(dev, addr, (uint8_t)((old & (uint8_t)~mask) | (val & mask)));
}

nx_ktm5220_ret_t nx_ktm5220_unlock(nx_ktm5220_t *dev)
{
    if (dev == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    dev->unlocked = false;
    nx_ktm5220_ret_t ret = ktm_write_raw(dev, NX_KTM5220_REG_WRITE_UNLOCK, 0xAAu);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    uint8_t state = 0u;
    ret = nx_ktm5220_read_reg8(dev, NX_KTM5220_REG_WRITE_UNLOCK, &state);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    if (state != 0x01u) {
        return NX_KTM5220_ERR_IO;
    }
    dev->unlocked = true;
    return NX_KTM5220_OK;
}

nx_ktm5220_ret_t nx_ktm5220_get_zero(nx_ktm5220_t *dev, uint32_t *out_raw)
{
    if (dev == NULL || out_raw == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    uint16_t low = 0u;
    uint8_t high = 0u;
    nx_ktm5220_ret_t ret = nx_ktm5220_read_reg16(dev, NX_KTM5220_REG_ZERO_L, &low);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    ret = nx_ktm5220_read_reg8(dev, NX_KTM5220_REG_ZERO_H, &high);
    if (ret == NX_KTM5220_OK) {
        *out_raw = (uint32_t)low | ((uint32_t)(high & NX_KTM5220_ZERO_H_MASK) << 16);
    }
    return ret;
}

nx_ktm5220_ret_t nx_ktm5220_set_zero(nx_ktm5220_t *dev, uint32_t raw)
{
    if (dev == NULL || raw > NX_KTM5220_ANGLE_MAX) {
        return NX_KTM5220_ERR_PARAM;
    }
    if (!dev->unlocked) {
        return NX_KTM5220_ERR_LOCKED;
    }
    /* Read reserved bits before changing any byte of the zero offset. */
    uint8_t high = 0u;
    nx_ktm5220_ret_t ret = nx_ktm5220_read_reg8(dev, NX_KTM5220_REG_ZERO_H, &high);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    ret = nx_ktm5220_write_reg16(dev, NX_KTM5220_REG_ZERO_L, (uint16_t)raw);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    high = (uint8_t)((high & (uint8_t)~NX_KTM5220_ZERO_H_MASK) | (uint8_t)(raw >> 16));
    return nx_ktm5220_write_reg8(dev, NX_KTM5220_REG_ZERO_H, high);
}

nx_ktm5220_ret_t nx_ktm5220_zero_here(nx_ktm5220_t *dev)
{
    return nx_ktm5220_update_reg8(dev, NX_KTM5220_REG_ZERO_SET,
                                 NX_KTM5220_ZERO_SET_MASK, NX_KTM5220_ZERO_SET_MASK);
}

nx_ktm5220_ret_t nx_ktm5220_set_calibration(nx_ktm5220_t *dev, bool enable)
{
    return nx_ktm5220_update_reg8(dev, NX_KTM5220_REG_CAL_REG_SET,
                                 NX_KTM5220_CAL_REG_SET_MASK,
                                 enable ? NX_KTM5220_CAL_REG_SET_MASK : 0u);
}

nx_ktm5220_ret_t nx_ktm5220_read_calibration_status(nx_ktm5220_t *dev, uint8_t *status)
{
    if (dev == NULL || status == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    uint8_t value = 0u;
    nx_ktm5220_ret_t ret = nx_ktm5220_read_reg8(dev, NX_KTM5220_REG_ANLC_STATUS, &value);
    if (ret == NX_KTM5220_OK) {
        *status = (uint8_t)(value & NX_KTM5220_ANLC_STATUS_MASK);
    }
    return ret;
}

nx_ktm5220_ret_t nx_ktm5220_write_mtp(nx_ktm5220_t *dev)
{
    if (dev == NULL) {
        return NX_KTM5220_ERR_PARAM;
    }
    if (!dev->unlocked) {
        return NX_KTM5220_ERR_LOCKED;
    }
    const uint8_t tx[3] = { 0xC0u, 0x00u, 0u };
    uint8_t rx[3] = { 0u };
    nx_ktm5220_ret_t ret = ktm_transact(dev, tx, rx, sizeof tx);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    return rx[2] == 0x55u ? NX_KTM5220_OK : NX_KTM5220_ERR_IO;
}

bool nx_ktm5220_busy(nx_ktm5220_t *dev)
{
    return dev != NULL && dev->cfg.is_busy != NULL && dev->cfg.is_busy(dev->cfg.io_ctx);
}
