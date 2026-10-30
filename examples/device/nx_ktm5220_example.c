/**
 * @file    nx_ktm5220_example.c
 * @brief   KTM5220 fake-SPI examples and protocol/error-path checks.
 *
 * The fake decodes command nibbles independently of the driver and computes
 * CRC bit by bit. It models the v0.5 response-data CRC interpretation; actual
 * hardware must still confirm the manual's unspecified CRC coverage and timing.
 * No real EEPROM is programmed and no motor or time source is needed.
 */
#include "nx_ktm5220_example.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/device/nx_ktm5220.h"

typedef struct {
    uint8_t regs[256];
    uint32_t angle;
    uint8_t status;
    bool unlocked;
    bool crc_invert;

    bool selected;
    uint8_t tx[6];
    size_t tx_len;
    uint8_t rx[4];
    size_t rx_len;
    bool transferred;
    bool frame_failed;
    size_t frames;
    size_t select_calls;
    size_t release_calls;
    size_t delay_calls;
    size_t burns;

    bool busy;
    bool reject_unlock;
    bool corrupt_crc;
    bool corrupt_status;
    bool bad_mtp_ack;
    bool fail_select;
    bool fail_transfer;
    bool fail_release;
    bool corrupt_addr_enabled;
    uint8_t corrupt_addr;
} fake_slave_t;

/* delay_ns has no context argument, so the bench tracks the selected slave. */
static fake_slave_t *selected_slave;

static uint8_t fake_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (uint8_t)((crc & 0x80u) ? ((unsigned)crc << 1) ^ 0x07u
                                          : ((unsigned)crc << 1));
        }
    }
    return crc;
}

static void fake_append_crc(fake_slave_t *s, size_t data_len, bool corrupt)
{
    s->rx[data_len] = (uint8_t)(fake_crc8(s->rx, data_len)
                              ^ (s->crc_invert ? 0xFFu : 0u)
                              ^ (corrupt ? 0x01u : 0u));
    s->rx_len = data_len + 1u;
}

static void fake_decode(fake_slave_t *s)
{
    assert(s->tx_len >= 2u);
    uint8_t cmd = (uint8_t)(s->tx[0] >> 4);
    uint8_t addr = (uint8_t)(((s->tx[0] & 0x0Fu) << 4) | (s->tx[1] >> 4));
    assert((s->tx[1] & 0x0Fu) == 0u); /* four Idle bits */

    if (cmd == 3u) {
        assert(s->tx_len == 4u);
        assert(s->tx[2] == 0u && s->tx[3] == 0u);
        s->rx[0] = s->regs[addr];
        fake_append_crc(s, 1u, s->corrupt_crc ||
                        (s->corrupt_addr_enabled && addr == s->corrupt_addr));
    } else if (cmd == 6u) {
        assert(s->tx_len == 3u);
        if (addr == 0x1Au && s->tx[2] == 0xAAu && !s->reject_unlock) {
            s->unlocked = true;
            s->regs[addr] = 0x01u;
        } else if (s->unlocked && addr != 0x1Au) {
            s->regs[addr] = s->tx[2];
        }
        /* MISO is undefined throughout writes; the driver must ignore it. */
        s->rx_len = 0u;
    } else if (cmd == 10u) {
        assert(s->tx_len == 6u && addr == 0u);
        for (size_t i = 2u; i < s->tx_len; ++i) {
            assert(s->tx[i] == 0u); /* response clocks must shift MOSI=0 */
        }
        uint32_t packed = (s->angle << 3) | s->status;
        s->rx[0] = (uint8_t)(packed >> 16);
        s->rx[1] = (uint8_t)(packed >> 8);
        s->rx[2] = (uint8_t)packed;
        fake_append_crc(s, 3u, s->corrupt_crc);
        if (s->corrupt_status) {
            s->rx[2] ^= 1u; /* alter status after CRC: status must be covered */
        }
    } else {
        assert(cmd == 12u && s->tx_len == 3u && addr == 0u);
        assert(s->tx[2] == 0u);
        assert(s->unlocked);
        ++s->burns;
        s->rx[0] = s->bad_mtp_ack ? 0x54u : 0x55u;
        s->rx_len = 1u;
    }
}

static bool fake_cs(void *ctx, bool assert_level)
{
    fake_slave_t *s = (fake_slave_t *)ctx;
    if (assert_level) {
        assert(!s->selected && selected_slave == NULL);
        ++s->select_calls;
        s->selected = true;
        selected_slave = s;
        s->tx_len = 0u;
        s->rx_len = 0u;
        s->transferred = false;
        s->frame_failed = s->fail_select;
        return !s->fail_select;
    }

    ++s->release_calls;
    assert(s->selected && selected_slave == s);
    if (!s->frame_failed) {
        assert(s->tx_len > 0u && s->transferred);
    }
    s->selected = false;
    selected_slave = NULL;
    ++s->frames;
    return !s->fail_release;
}

static bool fake_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    fake_slave_t *s = (fake_slave_t *)ctx;
    assert(s->selected && tx != NULL && rx != NULL && !s->transferred);
    assert((uintptr_t)tx + len <= (uintptr_t)rx ||
           (uintptr_t)rx + len <= (uintptr_t)tx);
    assert(len >= 3u && len <= sizeof s->tx && s->tx_len == 0u);
    s->transferred = true;
    memcpy(s->tx, tx, len);
    s->tx_len = len;
    if (s->fail_transfer) {
        /* A failed DMA transfer may already have changed part of RX. Public
         * output parameters must still remain untouched. */
        memset(rx, 0xCC, len / 2u);
        s->frame_failed = true;
        return false;
    }
    fake_decode(s);
    /* Nonzero command-phase garbage catches a driver that parses response
     * bytes from offset zero or includes the command RX bytes in its CRC. */
    memset(rx, 0xA5, len);
    if (s->rx_len > 0u) {
        assert(len == 2u + s->rx_len);
        memcpy(rx + 2u, s->rx, s->rx_len);
    }
    return true;
}

static bool fake_busy(void *ctx)
{
    return ((fake_slave_t *)ctx)->busy;
}

static void fake_delay_ns(uint32_t ns)
{
    assert(selected_slave != NULL && selected_slave->selected);
    assert(ns >= 13u);
    ++selected_slave->delay_calls;
}

static void bench_init(fake_slave_t *s, nx_ktm5220_t *dev, bool crc_invert,
                       bool optional_callbacks)
{
    memset(s, 0, sizeof *s);
    assert(selected_slave == NULL);
    s->crc_invert = crc_invert;
    nx_ktm5220_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cs = fake_cs;
    cfg.transfer = fake_transfer;
    cfg.is_busy = optional_callbacks ? fake_busy : NULL;
    cfg.delay_ns = optional_callbacks ? fake_delay_ns : NULL;
    cfg.io_ctx = s;
    cfg.crc_invert = crc_invert;
    assert(nx_ktm5220_init(dev, &cfg));
    /* cfg is local: all following accesses exercise the copied config. */
}

static void check_angles(bool crc_invert, bool optional_callbacks)
{
    fake_slave_t s;
    nx_ktm5220_t dev;
    bench_init(&s, &dev, crc_invert, optional_callbacks);
    const uint32_t angles[] = {0u, 1u, 7u, 8u, 0x100001u, 0x1FFFFFu};
    for (size_t i = 0u; i < sizeof angles / sizeof angles[0]; ++i) {
        s.angle = angles[i];
        uint32_t raw = 0xDEADBEEFu;
        assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_OK);
        assert(raw == angles[i]);
    }
    assert(nx_ktm5220_raw_to_mdeg(0u) == 0u);
    assert(nx_ktm5220_raw_to_mdeg(0x100000u) == 180000u);
    assert(nx_ktm5220_raw_to_mdeg(0x1FFFFFu) == 359999u);

    for (uint8_t alarm = 1u; alarm <= 4u; alarm = (uint8_t)(alarm << 1)) {
        s.status = alarm;
        nx_ktm5220_sample_t sample = {0u, 0u};
        assert(nx_ktm5220_read_sample(&dev, &sample) == NX_KTM5220_OK);
        assert(sample.raw == s.angle && sample.status == alarm);
        uint32_t raw = 0x12345678u;
        assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_ERR_STATUS);
        assert(raw == 0x12345678u);
    }
    s.status = 0u;
    s.corrupt_crc = true;
    nx_ktm5220_sample_t sample = {0x12345678u, 0xA5u};
    assert(nx_ktm5220_read_sample(&dev, &sample) == NX_KTM5220_ERR_CRC);
    assert(sample.raw == 0x12345678u && sample.status == 0xA5u);
    s.corrupt_crc = false;
    s.corrupt_status = true;
    assert(nx_ktm5220_read_sample(&dev, &sample) == NX_KTM5220_ERR_CRC);
    assert(sample.raw == 0x12345678u && sample.status == 0xA5u);
    s.corrupt_status = false;
    dev.cfg.crc_invert = !crc_invert;
    assert(nx_ktm5220_read_sample(&dev, &sample) == NX_KTM5220_ERR_CRC);
    assert(!s.selected && !nx_ktm5220_busy(&dev));
    assert(s.delay_calls == (optional_callbacks ? s.frames : 0u));
}

static void check_registers(void)
{
    fake_slave_t s;
    nx_ktm5220_t dev;
    bench_init(&s, &dev, false, true);
    s.regs[0x59] = 0x34u;
    s.regs[0x5A] = 0x12u;
    s.regs[0xFF] = 0xA5u;
    uint8_t byte = 0u;
    uint16_t word = 0u;
    assert(nx_ktm5220_read_reg8(&dev, 0xFFu, &byte) == NX_KTM5220_OK);
    assert(byte == 0xA5u);
    assert(nx_ktm5220_read_reg16(&dev, NX_KTM5220_REG_ABZ_PPT_L, &word) == NX_KTM5220_OK);
    assert(word == 0x1234u);

    size_t frames = s.frames;
    assert(nx_ktm5220_write_reg8(&dev, 0x54u, 0x55u) == NX_KTM5220_ERR_LOCKED);
    assert(nx_ktm5220_set_zero(&dev, 0x123456u) == NX_KTM5220_ERR_LOCKED);
    assert(nx_ktm5220_zero_here(&dev) == NX_KTM5220_ERR_LOCKED);
    assert(nx_ktm5220_set_calibration(&dev, true) == NX_KTM5220_ERR_LOCKED);
    assert(nx_ktm5220_write_mtp(&dev) == NX_KTM5220_ERR_LOCKED);
    assert(s.frames == frames);

    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_OK);
    assert(dev.unlocked && s.unlocked);
    assert(s.tx[0] == 0x31u && s.tx[1] == 0xA0u); /* unlock readback */
    assert(nx_ktm5220_write_reg8(&dev, 0x54u, 0x67u) == NX_KTM5220_OK);
    assert(s.regs[0x54] == 0x67u && s.tx[0] == 0x65u && s.tx[1] == 0x40u);
    assert(nx_ktm5220_write_reg16(&dev, 0xFEu, 0xBEEFu) == NX_KTM5220_OK);
    assert(s.regs[0xFE] == 0xEFu && s.regs[0xFF] == 0xBEu);
    frames = s.frames;
    assert(nx_ktm5220_read_reg16(&dev, 0xFFu, &word) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_reg16(&dev, 0xFFu, 0u) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_reg8(&dev, 0x1Au, 0xAAu) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_reg16(&dev, 0x19u, 0u) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_reg16(&dev, 0x1Au, 0u) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_update_reg8(&dev, 0x1Au, 1u, 1u) == NX_KTM5220_ERR_PARAM);
    assert(s.frames == frames);

    s.corrupt_addr_enabled = true;
    s.corrupt_addr = 0x5Au;
    word = 0xCAFEu;
    assert(nx_ktm5220_read_reg16(&dev, 0x59u, &word) == NX_KTM5220_ERR_CRC);
    assert(word == 0xCAFEu);
    s.corrupt_crc = true;
    byte = 0xA5u;
    assert(nx_ktm5220_read_reg8(&dev, 0x54u, &byte) == NX_KTM5220_ERR_CRC);
    assert(byte == 0xA5u);
    s.corrupt_crc = false;
    s.corrupt_addr_enabled = false;

    s.regs[0x51] = 0x6Bu;
    assert(nx_ktm5220_update_reg8(&dev, 0x51u, 0x80u, 0xFFu) == NX_KTM5220_OK);
    assert(s.regs[0x51] == 0xEBu); /* only masked bits are used from val */
    s.regs[0x4D] = 0xE0u;
    assert(nx_ktm5220_set_zero(&dev, 0x123456u) == NX_KTM5220_OK);
    assert(s.regs[0x4B] == 0x56u && s.regs[0x4C] == 0x34u && s.regs[0x4D] == 0xF2u);
    uint32_t zero = 0u;
    assert(nx_ktm5220_get_zero(&dev, &zero) == NX_KTM5220_OK && zero == 0x123456u);
    s.corrupt_addr_enabled = true;
    s.corrupt_addr = 0x4Du;
    zero = 0xDEADBEEFu;
    assert(nx_ktm5220_get_zero(&dev, &zero) == NX_KTM5220_ERR_CRC && zero == 0xDEADBEEFu);
    s.corrupt_addr_enabled = false;
    frames = s.frames;
    assert(nx_ktm5220_set_zero(&dev, 0x200000u) == NX_KTM5220_ERR_PARAM);
    assert(s.frames == frames);

    s.regs[0x12] = 0xA4u;
    assert(nx_ktm5220_zero_here(&dev) == NX_KTM5220_OK);
    assert(s.regs[0x12] == 0xA5u);
    s.regs[0x17] = 0xB0u;
    assert(nx_ktm5220_set_calibration(&dev, true) == NX_KTM5220_OK);
    assert(s.regs[0x17] == 0xB1u);
    assert(nx_ktm5220_set_calibration(&dev, false) == NX_KTM5220_OK);
    assert(s.regs[0x17] == 0xB0u);
    s.regs[0x16] = 0xF3u;
    assert(nx_ktm5220_read_calibration_status(&dev, &byte) == NX_KTM5220_OK);
    assert(byte == NX_KTM5220_ANLC_DONE);
    assert(s.burns == 0u); /* configuration/trigger APIs send no MTP command */

    assert(nx_ktm5220_write_mtp(&dev) == NX_KTM5220_OK);
    assert(s.burns == 1u && s.rx_len == 1u && s.tx[0] == 0xC0u && s.tx[1] == 0u);
    /* A real device must now keep power for >2.5 s and power-cycle. The fake
     * has no EEPROM timing, so it can immediately exercise a bad response. */
    s.bad_mtp_ack = true;
    assert(nx_ktm5220_write_mtp(&dev) == NX_KTM5220_ERR_IO);
    assert(!s.selected);
}

static void check_unlock_failures(void)
{
    fake_slave_t s;
    nx_ktm5220_t dev;
    bench_init(&s, &dev, false, true);
    s.reject_unlock = true;
    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_ERR_IO && !dev.unlocked);
    s.reject_unlock = false;
    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_OK);
    s.corrupt_crc = true;
    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_ERR_CRC && !dev.unlocked);
    size_t frames = s.frames;
    assert(nx_ktm5220_write_reg8(&dev, 0x54u, 0u) == NX_KTM5220_ERR_LOCKED);
    assert(s.frames == frames);
    s.corrupt_crc = false;
    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_OK);
    s.fail_transfer = true;
    assert(nx_ktm5220_unlock(&dev) == NX_KTM5220_ERR_IO && !dev.unlocked);
    assert(!s.selected);
}

static void check_transport_failures(void)
{
    fake_slave_t s;
    nx_ktm5220_t dev;
    bench_init(&s, &dev, false, true);
    uint32_t raw = 0xDEADBEEFu;
    s.busy = true;
    assert(nx_ktm5220_busy(&dev));
    assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_ERR_IO);
    assert(s.select_calls == 0u && raw == 0xDEADBEEFu);
    s.busy = false;

    s.fail_select = true;
    assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_ERR_IO);
    assert(!s.selected && s.release_calls == 1u && raw == 0xDEADBEEFu);
    s.fail_select = false;
    s.fail_transfer = true;
    assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_ERR_IO);
    assert(!s.selected && s.release_calls == 2u && raw == 0xDEADBEEFu);
    s.fail_transfer = false;
    s.fail_release = true;
    assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_ERR_IO);
    assert(!s.selected && s.release_calls == 3u && raw == 0xDEADBEEFu);
    s.fail_release = false;
    assert(nx_ktm5220_read_angle(&dev, &raw) == NX_KTM5220_OK);
    assert(raw == 0u && s.select_calls == s.release_calls);
}

static void check_parameters(void)
{
    fake_slave_t s;
    nx_ktm5220_t dev;
    bench_init(&s, &dev, false, false);
    nx_ktm5220_cfg_t cfg = dev.cfg;
    assert(!nx_ktm5220_init(NULL, &cfg));
    assert(!nx_ktm5220_init(&dev, NULL));
    cfg.cs = NULL;
    assert(!nx_ktm5220_init(&dev, &cfg));
    cfg = dev.cfg;
    cfg.transfer = NULL;
    assert(!nx_ktm5220_init(&dev, &cfg));
    assert(nx_ktm5220_read_sample(&dev, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_read_angle(&dev, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_read_reg8(&dev, 0u, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_read_reg16(&dev, 0u, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_get_zero(&dev, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_read_calibration_status(&dev, NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_reg8(NULL, 0u, 0u) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_unlock(NULL) == NX_KTM5220_ERR_PARAM);
    assert(nx_ktm5220_write_mtp(NULL) == NX_KTM5220_ERR_PARAM);
    assert(!nx_ktm5220_busy(NULL) && !nx_ktm5220_busy(&dev));
    assert(s.frames == 0u);
}

int nx_ktm5220_example_run(void)
{
    assert(fake_crc8((const uint8_t *)"123456789", 9u) == 0xF4u);
    check_angles(false, true);
    check_angles(true, false);
    check_registers();
    check_unlock_failures();
    check_transport_failures();
    check_parameters();
    printf("nx_ktm5220: all checks passed\n");
    return 0;
}
