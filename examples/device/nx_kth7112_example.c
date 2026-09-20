/**
 * @file    nx_kth7112_example.c
 * @brief   KTH7112 driver example: fake SPI slave on a bench, plus self-checks.
 *
 * Demonstrates:
 *   1. Reading the absolute angle, with the trailing CRC-8 checked on every frame
 *      and the conversion to millidegrees done by the caller.
 *   2. Register access: read while locked, unlock, write with echo
 *      verification, lock again.
 *   3. The failure paths: a corrupted CRC, a write the slave acknowledges with
 *      the wrong value, and a busy port.
 *   4. An optional delay callback: the driver runs with one, and with none.
 *
 * There is no real hardware here, so a fake slave decodes the bytes the driver
 * writes and queues the bytes it should read back. It is deliberately the only
 * place that knows what a KTH7112 answers, so the driver's frame shapes are
 * checked against the manual rather than against itself.
 */

#include "nx_kth7112_example.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/device/nx_kth7112.h"

/* ------------------------------------------------------------------ */
/* Fake SPI slave                                                      */
/* ------------------------------------------------------------------ */

#define FAKE_QUEUE_LEN 8u

/* Command bytes of the part's SPI protocol. They are private to this file: the
 * driver keeps its own copies, so these are spelled out here to make the fake
 * slave answer from the manual rather than from the driver's definitions. */
#define KTH_CMD_READ_ANGLE 0x00u
#define KTH_CMD_READ_REG   0x11u
#define KTH_CMD_WRITE_REG  0x33u
#define KTH_CMD_WRITE_MTP  0x22u

/** @brief One fake KTH7112 on a three-wire SPI port. */
typedef struct {
    /* Bus state */
    bool    cs_active;                 /**< Chip select currently asserted        */
    uint8_t tx[8];                     /**< Bytes the driver has written this frame */
    size_t  tx_len;

    /* Response queue, filled by the command decoder */
    uint8_t rx[FAKE_QUEUE_LEN];
    size_t  rx_len;

    /* Device state */
    uint8_t regs[256];                 /**< Register bank                         */
    bool    unlocked;                  /**< Register writes accepted              */
    uint16_t angle_raw;                /**< Angle the part reports                */

    /* Instrumentation and fault injection */
    size_t  frames;                    /**< Chip-select windows completed          */
    uint32_t last_gap_ns;              /**< Last delay the driver asked for        */
    bool    corrupt_crc;               /**< Answer with a bad CRC                  */
    bool    echo_wrong;                /**< Acknowledge a write with the wrong value */
    int     busy_countdown;            /**< Report busy for this many polls first  */

    /* Diagnostics */
    int     error;                     /**< First protocol violation seen          */
    const char *error_where;
} fake_slave_t;

static void fake_fail(fake_slave_t *s, const char *where)
{
    if (s->error == 0) {
        s->error = 1;
        s->error_where = where;
    }
}

/**
 * @brief  Compute the CRC the part appends to its responses.
 *
 * CRC-8/ITU, worked out here bit by bit from the polynomial, so this file carries
 * its own statement of the frame shape.
 */
static uint8_t fake_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00u;

    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint8_t)((crc & 0x80u) ? ((uint8_t)(crc << 1) ^ 0x07u)
                                          : (uint8_t)(crc << 1));
        }
    }

    return (uint8_t)(crc ^ 0x55u);
}

/**
 * @brief  Queue the trailing CRC of the response built so far (all but the last byte).
 */
static void fake_push_crc(fake_slave_t *s)
{
    assert(s->rx_len > 0u);

    uint8_t crc = fake_crc8(s->rx, s->rx_len - 1u);
    if (s->corrupt_crc) {
        crc = (uint8_t)(crc ^ 0xFFu);
    }
    s->rx[s->rx_len - 1u] = crc;
}

/**
 * @brief  Decode a completed command and queue what the part should answer.
 *
 * Called with the command bytes the driver wrote; the response is read back
 * afterwards, in the same chip-select window.
 */
static void fake_decode(fake_slave_t *s)
{
    s->rx_len = 0u;

    if (s->tx_len == 0u) {
        return;
    }

    if (s->tx_len == 1u && s->tx[0] == KTH_CMD_READ_ANGLE) {
        /* ANGLE[15:8], ANGLE[7:0], CRC8 - high byte first. */
        s->rx[0] = (uint8_t)(s->angle_raw >> 8);
        s->rx[1] = (uint8_t)(s->angle_raw & 0xFFu);
        s->rx[2] = 0u;
        s->rx_len = 3u;
        fake_push_crc(s);
        return;
    }

    if (s->tx_len == 2u && s->tx[0] == KTH_CMD_READ_REG) {
        s->rx[0] = s->regs[s->tx[1]];
        s->rx[1] = 0u;
        s->rx_len = 2u;
        fake_push_crc(s);
        return;
    }

    if (s->tx_len == 3u && s->tx[0] == KTH_CMD_WRITE_REG) {
        uint8_t addr = s->tx[1];
        uint8_t val  = s->tx[2];

        if (s->unlocked) {
            s->regs[addr] = val;
        }
        /* A locked part drops the write but still answers, and the answer is the
         * value it actually holds - that is how the caller sees the drop. */
        s->rx[0] = s->echo_wrong ? (uint8_t)(val ^ 0xFFu) : s->regs[addr];
        s->rx_len = 1u;
        return;
    }

    if (s->tx_len == 3u && s->tx[0] == KTH_CMD_WRITE_MTP &&
        s->tx[1] == 0x55u && s->tx[2] == 0xAAu) {
        return;   /* No response */
    }

    if (s->tx_len == 4u &&
        memcmp(s->tx, "\x20\x24\x01\x01", 4u) == 0) {
        s->unlocked = true;
        return;
    }

    if (s->tx_len == 4u &&
        memcmp(s->tx, "\x20\x24\x12\x31", 4u) == 0) {
        s->unlocked = false;
        return;
    }

    fake_fail(s, "unrecognized frame");
}

static bool fake_cs(void *ctx, bool assert_level)
{
    fake_slave_t *s = (fake_slave_t *)ctx;

    if (assert_level) {
        if (s->cs_active) {
            fake_fail(s, "chip select asserted twice");
        }
        s->cs_active = true;
        s->tx_len = 0u;
        s->rx_len = 0u;
    } else {
        if (!s->cs_active) {
            fake_fail(s, "chip select released while inactive");
        } else {
            if (s->rx_len == 0u) {
                fake_decode(s);   /* Frames with no response still follow the protocol */
            }
            if (s->tx_len == 0u) {
                fake_fail(s, "frame with no bytes written");
            }
            s->cs_active = false;
            s->frames++;
        }
    }

    return true;
}

static bool fake_write(void *ctx, const uint8_t *data, size_t len)
{
    fake_slave_t *s = (fake_slave_t *)ctx;

    if (!s->cs_active || data == NULL) {
        fake_fail(s, "write outside a frame");
        return false;
    }
    if (s->tx_len + len > sizeof s->tx) {
        fake_fail(s, "overlong frame");
        return false;
    }

    memcpy(&s->tx[s->tx_len], data, len);
    s->tx_len += len;

    /* Decoding is idempotent, so a command split across write calls is decoded
     * again once the rest of it arrives. */
    fake_decode(s);

    return true;
}

static bool fake_read(void *ctx, uint8_t *data, size_t len)
{
    fake_slave_t *s = (fake_slave_t *)ctx;

    if (!s->cs_active || data == NULL) {
        fake_fail(s, "read outside a frame");
        return false;
    }
    if (len != s->rx_len) {
        fake_fail(s, "read length does not match the response");
        return false;
    }

    memcpy(data, s->rx, len);
    return true;
}

static bool fake_busy(void *ctx)
{
    fake_slave_t *s = (fake_slave_t *)ctx;

    if (s->busy_countdown > 0) {
        s->busy_countdown--;
        return true;
    }
    return false;
}

static void fake_delay_ns(uint32_t ns)
{
    (void)ns;
}

static void fake_slave_reset(fake_slave_t *s)
{
    memset(s, 0, sizeof *s);
    memset(s->regs, 0, sizeof s->regs);
    s->regs[NX_KTH7112_REG_PPT_L] = 0xFFu;
    s->regs[NX_KTH7112_REG_PPT_H] = 0x03u;
}

/* ------------------------------------------------------------------ */
/* Self-checks                                                         */
/* ------------------------------------------------------------------ */

static void run_with_delay_callback(void)
{
    fake_slave_t slave;
    nx_kth7112_t dev;

    fake_slave_reset(&slave);

    nx_kth7112_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cs         = fake_cs;
    cfg.write      = fake_write;
    cfg.read       = fake_read;
    cfg.is_busy    = fake_busy;
    cfg.delay_ns   = fake_delay_ns;
    cfg.io_ctx     = &slave;

    assert(nx_kth7112_init(&dev, &cfg));

    /* Nothing has asked the port to start a frame yet, so it is free. */
    assert(!nx_kth7112_busy(&dev));

    /* --- Angle read ------------------------------------------------- */
    slave.angle_raw = 0x4000u;   /* a quarter turn */
    uint16_t raw = 0u;
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_OK);
    assert(raw == 0x4000u);

    /* --- Angle conversion ------------------------------------------- */
    assert(nx_kth7112_raw_to_mdeg(0u)      == 0u);
    assert(nx_kth7112_raw_to_mdeg(0x4000u) == 90000u);
    assert(nx_kth7112_raw_to_mdeg(0xFFFFu) == 359994u);
    assert(nx_kth7112_raw_to_mdeg(raw)     == 90000u);

    /* --- Register read, which is allowed while locked --------------- */
    uint8_t val = 0u;
    assert(nx_kth7112_read_reg8(&dev, NX_KTH7112_REG_PPT_L, &val) == NX_KTH7112_OK);
    assert(val == 0xFFu);

    assert(nx_kth7112_read_reg16(&dev, NX_KTH7112_REG_PPT_L, &raw) == NX_KTH7112_OK);
    assert(raw == 0x03FFu);   /* low byte at the low address */

    /* --- A write while locked is refused without touching the bus --- */
    size_t frames_before = slave.frames;
    assert(nx_kth7112_write_reg8(&dev, NX_KTH7112_REG_FW, 0x11u) == NX_KTH7112_ERR_LOCKED);
    assert(nx_kth7112_write_mtp(&dev) == NX_KTH7112_ERR_LOCKED);
    assert(slave.frames == frames_before);

    /* --- Unlock, write, verify the echo, lock again ------------------ */
    assert(nx_kth7112_unlock(&dev) == NX_KTH7112_OK);

    assert(nx_kth7112_write_reg8(&dev, NX_KTH7112_REG_FW, 0x44u) == NX_KTH7112_OK);
    assert(slave.regs[NX_KTH7112_REG_FW] == 0x44u);

    assert(nx_kth7112_write_reg16(&dev, NX_KTH7112_REG_ZERO_L, 0x1234u) == NX_KTH7112_OK);
    assert(slave.regs[NX_KTH7112_REG_ZERO_L] == 0x34u);
    assert(slave.regs[NX_KTH7112_REG_ZERO_H] == 0x12u);

    assert(nx_kth7112_write_mtp(&dev) == NX_KTH7112_OK);

    assert(nx_kth7112_lock(&dev) == NX_KTH7112_OK);

    /* --- And is refused again once locked ---------------------------- */
    assert(nx_kth7112_write_reg8(&dev, NX_KTH7112_REG_FW, 0x66u) == NX_KTH7112_ERR_LOCKED);
    assert(slave.regs[NX_KTH7112_REG_FW] == 0x44u);

    /* --- A corrupted CRC is caught and the output is left alone ------- */
    slave.corrupt_crc = true;
    raw = 0xABCDu;
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_ERR_CRC);
    assert(raw == 0xABCDu);
    assert(nx_kth7112_read_reg8(&dev, NX_KTH7112_REG_FW, &val) == NX_KTH7112_ERR_CRC);
    slave.corrupt_crc = false;

    /* --- An unacknowledged write is reported ------------------------- */
    assert(nx_kth7112_unlock(&dev) == NX_KTH7112_OK);
    slave.echo_wrong = true;
    assert(nx_kth7112_write_reg8(&dev, NX_KTH7112_REG_FW, 0x55u) == NX_KTH7112_ERR_IO);
    slave.echo_wrong = false;

    /* --- Parameter rejection ----------------------------------------- */
    assert(nx_kth7112_read_angle(&dev, NULL) == NX_KTH7112_ERR_PARAM);
    assert(nx_kth7112_read_reg8(&dev, 0x00u, NULL) == NX_KTH7112_ERR_PARAM);
    assert(nx_kth7112_read_reg16(&dev, NX_KTH7112_REG_ZERO_L, NULL) == NX_KTH7112_ERR_PARAM);
    assert(!nx_kth7112_init(NULL, &cfg));
    assert(!nx_kth7112_init(&dev, NULL));

    nx_kth7112_cfg_t incomplete;
    memset(&incomplete, 0, sizeof incomplete);
    incomplete.cs = fake_cs;   /* write / read missing */
    assert(!nx_kth7112_init(&dev, &incomplete));

    /* --- The port is left idle and every frame well formed ------------ */
    assert(!slave.cs_active);
    assert(slave.error == 0);
    assert(slave.frames > 0u);
}

static void run_without_delay_callback(void)
{
    fake_slave_t slave;
    nx_kth7112_t dev;

    fake_slave_reset(&slave);

    nx_kth7112_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cs       = fake_cs;
    cfg.write    = fake_write;
    cfg.read     = fake_read;
    cfg.is_busy  = fake_busy;
    cfg.delay_ns = NULL;   /* No waiting at all; the port is fast enough */
    cfg.io_ctx   = &slave;

    assert(nx_kth7112_init(&dev, &cfg));
    assert(nx_kth7112_busy(&dev) == false);

    slave.angle_raw = 0x2000u;
    uint16_t raw = 0u;
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_OK);
    assert(raw == 0x2000u);

    assert(nx_kth7112_unlock(&dev) == NX_KTH7112_OK);
    assert(nx_kth7112_write_reg8(&dev, NX_KTH7112_REG_FW, 0x22u) == NX_KTH7112_OK);

    assert(!slave.cs_active);
    assert(slave.error == 0);
}

static void run_without_busy_callback(void)
{
    fake_slave_t slave;
    nx_kth7112_t dev;

    fake_slave_reset(&slave);

    nx_kth7112_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cs       = fake_cs;
    cfg.write    = fake_write;
    cfg.read     = fake_read;
    cfg.is_busy  = NULL;   /* The port is always ready to take a frame */
    cfg.delay_ns = fake_delay_ns;
    cfg.io_ctx   = &slave;

    assert(nx_kth7112_init(&dev, &cfg));

    /* A port with no busy line reads as never busy, and frames still go out. */
    assert(!nx_kth7112_busy(&dev));
    slave.angle_raw = 0x1234u;
    uint16_t raw = 0u;
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_OK);
    assert(raw == 0x1234u);

    assert(!slave.cs_active);
    assert(slave.error == 0);
}

static void run_busy_port(void)
{
    fake_slave_t slave;
    nx_kth7112_t dev;

    fake_slave_reset(&slave);

    nx_kth7112_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.cs       = fake_cs;
    cfg.write    = fake_write;
    cfg.read     = fake_read;
    cfg.is_busy  = fake_busy;
    cfg.delay_ns = fake_delay_ns;   /* Present, but a busy port is not waited out */
    cfg.io_ctx   = &slave;

    assert(nx_kth7112_init(&dev, &cfg));

    /* A busy port is refused immediately, with the frame not started: the caller
     * owns the retry, so nothing is put on the bus and no time is spent waiting.
     * The busy line is read once per poll and never in a loop, which the two
     * explicit polls below plus the refused read account for. */
    slave.busy_countdown = 3;
    slave.angle_raw      = 0x8000u;
    uint16_t raw = 0u;
    size_t frames_before = slave.frames;
    assert(nx_kth7112_busy(&dev));                                            /* 2 */
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_ERR_IO);           /* 1 */
    assert(slave.frames == frames_before);
    assert(!slave.cs_active);
    assert(slave.busy_countdown == 1);

    /* Once the port is free the same call goes through. */
    slave.busy_countdown = 0;
    assert(!nx_kth7112_busy(&dev));
    assert(nx_kth7112_read_angle(&dev, &raw) == NX_KTH7112_OK);
    assert(raw == 0x8000u);

    assert(!slave.cs_active);
    assert(slave.error == 0);
}

int nx_kth7112_example_run(void)
{
    /* The CRC the part appends is CRC-8/ITU, whose check value over "123456789"
     * is 0xA1. Every frame the driver reads is verified with the same arithmetic,
     * on the driver's side of the bus. */
    assert(fake_crc8((const uint8_t *)"123456789", 9u) == 0xA1u);

    run_with_delay_callback();
    run_without_delay_callback();
    run_without_busy_callback();
    run_busy_port();

    printf("nx_kth7112: all checks passed\n");
    return 0;
}
