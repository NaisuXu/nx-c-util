/**
 * @file    nx_mp6546_example.c
 * @brief   MP6546 register-port example and error-path checks.
 *
 * The simulated port receives numeric 16-bit registers, not I2C bytes. It
 * checks register semantics, address forwarding, pending duty commands and
 * failures both before and after a write takes effect. It does not validate
 * the undocumented I2C frame format, real motor behavior or electrical timing.
 * No chip is powered, no motor is started and no OTP is programmed.
 */
#include "nx_mp6546_example.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "src/device/nx_mp6546.h"

typedef struct {
    bool write;
    uint8_t reg;
    uint16_t value;
} port_event_t;

typedef struct {
    uint16_t regs[32];
    uint16_t effective_duty[3];
    unsigned duty_commits;
    uint8_t expected_addr;
    bool standby;
    port_event_t events[64];
    size_t event_count;
    size_t reads;
    size_t writes;
    size_t fail_read;       /* One-based callback number; zero disables. */
    size_t fail_write;
    bool fail_after_apply;
} fake_port_t;

static void fake_record(fake_port_t *s, bool write, uint8_t reg, uint16_t value)
{
    assert(s->event_count < sizeof s->events / sizeof s->events[0]);
    port_event_t *event = &s->events[s->event_count++];
    event->write = write;
    event->reg = reg;
    event->value = value;
}

static bool fake_read(void *ctx, uint8_t addr7, uint8_t reg, uint16_t *value)
{
    fake_port_t *s = (fake_port_t *)ctx;
    assert(addr7 == s->expected_addr && reg < 32u && value != NULL);
    ++s->reads;
    fake_record(s, false, reg, s->regs[reg]);
    if (s->fail_read == s->reads) {
        *value = 0xDEADu; /* A failed port can modify its receive buffer. */
        return false;
    }
    *value = s->regs[reg];
    return true;
}

static bool fake_write(void *ctx, uint8_t addr7, uint8_t reg, uint16_t value)
{
    fake_port_t *s = (fake_port_t *)ctx;
    assert(addr7 == s->expected_addr && reg < 32u);
    ++s->writes;
    fake_record(s, true, reg, value);
    bool fail = s->fail_write == s->writes;
    if (fail && !s->fail_after_apply) {
        return false;
    }

    s->regs[reg] = value;
    if (reg == 0x09u && (s->regs[0x00u] & 0x0010u) != 0u) {
        /* Only duty mode has the documented three-register commit rule. */
        s->effective_duty[0] = s->regs[0x07u];
        s->effective_duty[1] = s->regs[0x08u];
        s->effective_duty[2] = s->regs[0x09u];
        ++s->duty_commits;
    } else if (reg == 0x0Fu) {
        s->standby = false;
    } else if (reg == 0x10u && (s->regs[0x00u] & 0x2000u) != 0u) {
        s->standby = true;
    }
    return !fail;
}

static void clear_trace(fake_port_t *s)
{
    s->event_count = 0u;
    s->reads = 0u;
    s->writes = 0u;
    s->fail_read = 0u;
    s->fail_write = 0u;
    s->fail_after_apply = false;
}

static void bench_init(fake_port_t *s, nx_mp6546_t *dev, uint8_t addr7)
{
    memset(s, 0, sizeof *s);
    s->expected_addr = addr7;
    s->regs[0x00u] = 0xE003u;
    s->regs[0x01u] = 0x5151u;
    s->regs[0x02u] = 0x0007u;
    s->regs[0x03u] = 0x0100u;
    s->regs[0x0Au] = 0x0800u; /* Default includes a reserved bit. */
    nx_mp6546_cfg_t cfg = {fake_read, fake_write, s, addr7};
    assert(nx_mp6546_init(dev, &cfg));
    assert(s->event_count == 0u);
    /* cfg is local: subsequent accesses also test configuration copying. */
}

static void expect_event(const fake_port_t *s, size_t index, bool write,
                         uint8_t reg)
{
    assert(index < s->event_count);
    assert(s->events[index].write == write && s->events[index].reg == reg);
}

static void expect_config(const nx_mp6546_t *dev, const nx_mp6546_cfg_t *cfg)
{
    assert(dev->cfg.read_reg16 == cfg->read_reg16);
    assert(dev->cfg.write_reg16 == cfg->write_reg16);
    assert(dev->cfg.io_ctx == cfg->io_ctx && dev->cfg.addr7 == cfg->addr7);
}

static void check_init_and_addresses(void)
{
    const uint8_t addresses[] = {0x07u, 0x08u, 0x09u};
    for (size_t i = 0u; i < sizeof addresses / sizeof addresses[0]; ++i) {
        fake_port_t s;
        nx_mp6546_t dev;
        bench_init(&s, &dev, addresses[i]);
        uint16_t value = 0u;
        assert(nx_mp6546_read_reg16(&dev, 0x00u, &value) == NX_MP6546_OK);
        assert(value == 0xE003u && s.reads == 1u);
        assert(nx_mp6546_write_reg16(&dev, 0x01u, 0xB2B2u) == NX_MP6546_OK);
        assert(s.regs[0x01u] == 0xB2B2u && s.writes == 1u);

        nx_mp6546_cfg_t cfg = dev.cfg;
        nx_mp6546_cfg_t before = dev.cfg;
        cfg.addr7 = 0x0Eu; /* Accidentally shifted 07h is rejected. */
        assert(!nx_mp6546_init(&dev, &cfg));
        expect_config(&dev, &before);
        cfg = dev.cfg;
        cfg.read_reg16 = NULL;
        assert(!nx_mp6546_init(&dev, &cfg));
        cfg = dev.cfg;
        cfg.write_reg16 = NULL;
        assert(!nx_mp6546_init(&dev, &cfg));
        assert(!nx_mp6546_init(NULL, &dev.cfg));
        assert(!nx_mp6546_init(&dev, NULL));
        expect_config(&dev, &before);
        assert(s.event_count == 2u);
        /* A context-free hardware port is allowed by the public contract. */
        cfg = dev.cfg;
        cfg.io_ctx = NULL;
        assert(nx_mp6546_init(&dev, &cfg));
        assert(s.event_count == 2u);
    }
}

static void check_samples_and_faults(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x08u);
    s.regs[0x0Cu] = 0xC000u;
    nx_mp6546_sample_t sample = {0u, false};
    assert(nx_mp6546_read_sample(&dev, &sample) == NX_MP6546_OK);
    assert(sample.angle_raw == 0x4000u && sample.fault && s.reads == 1u);
    assert(nx_mp6546_raw_to_mdeg(sample.angle_raw) == 180000u);
    uint16_t angle = 123u;
    assert(nx_mp6546_read_angle(&dev, &angle) == NX_MP6546_ERR_STATUS);
    assert(angle == 123u);
    s.regs[0x0Cu] = 0x7FFFu;
    assert(nx_mp6546_read_angle(&dev, &angle) == NX_MP6546_OK);
    assert(angle == 0x7FFFu);
    assert(nx_mp6546_raw_to_mdeg(0u) == 0u);
    assert(nx_mp6546_raw_to_mdeg(angle) == 359989u);

    s.regs[0x0Du] = 0xFBA5u; /* Reserved bits and INL must be excluded. */
    uint16_t faults = 0u;
    assert(nx_mp6546_read_faults(&dev, &faults) == NX_MP6546_OK);
    assert(faults == 0x0B00u);
    clear_trace(&s);
    s.fail_read = 1u;
    sample.angle_raw = 543u;
    sample.fault = true;
    assert(nx_mp6546_read_sample(&dev, &sample) == NX_MP6546_ERR_IO);
    assert(sample.angle_raw == 543u && sample.fault);
    s.fail_read = 2u;
    angle = 321u;
    assert(nx_mp6546_read_angle(&dev, &angle) == NX_MP6546_ERR_IO);
    assert(angle == 321u);
    s.fail_read = 3u;
    faults = 0x1111u;
    assert(nx_mp6546_read_faults(&dev, &faults) == NX_MP6546_ERR_IO);
    assert(faults == 0x1111u && s.writes == 0u);
    printf("nx_mp6546: mechanical half-turn = 180000 mdeg; FAULT kept separate\n");
}

static void check_register_guards_and_rmw(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x07u);
    assert(nx_mp6546_update_reg16(&dev, 0x0Au, 0x0400u, 0xFFFFu) == NX_MP6546_OK);
    assert(s.regs[0x0Au] == 0x0C00u); /* Preserve reserved default bit 11. */
    expect_event(&s, 0u, false, 0x0Au);
    expect_event(&s, 1u, true, 0x0Au);
    s.regs[0x03u] = 0x1A5Au;
    assert(nx_mp6546_update_reg16(&dev, 0x03u, 0xE100u, 0x6100u) == NX_MP6546_OK);
    assert(s.regs[0x03u] == 0x7B5Au); /* Preserve PCODE and reserved bits. */
    clear_trace(&s);
    assert(nx_mp6546_update_reg16(&dev, 0x0Au, 0u, 0xFFFFu) == NX_MP6546_OK);
    assert(s.event_count == 0u);
    const uint8_t bad_reads[] = {0x04u, 0x0Bu, 0x0Fu, 0x10u, 0x12u, 0x1Fu, 0xFFu};
    uint16_t value = 0x1234u;
    for (size_t i = 0u; i < sizeof bad_reads / sizeof bad_reads[0]; ++i) {
        assert(nx_mp6546_read_reg16(&dev, bad_reads[i], &value) == NX_MP6546_ERR_PARAM);
        assert(value == 0x1234u);
    }
    assert(nx_mp6546_write_reg16(&dev, 0x0Cu, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_write_reg16(&dev, 0x0Du, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_write_reg16(&dev, 0x04u, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x0Cu, 1u, 1u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x0Fu, 1u, 1u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x11u, 1u, 1u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x0Au, 0x0800u, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x03u, 0x00FFu, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x07u, 0x1000u, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(&dev, 0x08u, 0x0400u, 0u) == NX_MP6546_ERR_PARAM);
    assert(s.event_count == 0u);

    s.fail_read = 1u;
    assert(nx_mp6546_update_reg16(&dev, 0x00u, 0x0100u, 0u) == NX_MP6546_ERR_IO);
    assert(s.reads == 1u && s.writes == 0u && s.regs[0x00u] == 0xE003u);
    clear_trace(&s);
    s.fail_write = 1u;
    s.fail_after_apply = true;
    assert(nx_mp6546_update_reg16(&dev, 0x00u, 0x0100u, 0x0100u) == NX_MP6546_ERR_IO);
    assert(s.regs[0x00u] == 0xE103u && s.event_count == 2u);
}

static void check_dq_and_partial_writes(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x09u);
    assert(nx_mp6546_set_dq(&dev, -512, 511) == NX_MP6546_OK);
    assert(s.regs[0x08u] == 0x0200u && s.regs[0x09u] == 0x01FFu);
    assert(s.event_count == 2u && s.reads == 0u);
    expect_event(&s, 0u, true, 0x08u);
    expect_event(&s, 1u, true, 0x09u);
    assert(nx_mp6546_set_dq(&dev, 1, -1) == NX_MP6546_OK);
    assert(s.regs[0x08u] == 0x0001u && s.regs[0x09u] == 0x03FFu);
    assert(nx_mp6546_set_uq(&dev, -74) == NX_MP6546_OK);
    assert(s.regs[0x09u] == 0x03B6u && s.regs[0x08u] == 1u);
    clear_trace(&s);
    assert(nx_mp6546_set_dq(&dev, 0, 512) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_dq(&dev, -513, 0) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_uq(&dev, 512) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_uq(&dev, INT16_MIN) == NX_MP6546_ERR_PARAM);
    assert(s.event_count == 0u); /* Validate the whole pair before writing Ud. */

    s.regs[0x08u] = 7u;
    s.regs[0x09u] = 8u;
    s.fail_write = 2u;
    assert(nx_mp6546_set_dq(&dev, 20, -20) == NX_MP6546_ERR_IO);
    assert(s.regs[0x08u] == 20u && s.regs[0x09u] == 8u);
    assert(s.event_count == 2u); /* No rollback/retry or hidden EN handling. */
    clear_trace(&s);
    s.fail_write = 2u;
    s.fail_after_apply = true;
    assert(nx_mp6546_set_dq(&dev, 30, -30) == NX_MP6546_ERR_IO);
    assert(s.regs[0x08u] == 30u && s.regs[0x09u] == 0x03E2u);
    assert(s.event_count == 2u); /* Even ERR_IO can leave BOTH values applied. */
    clear_trace(&s);
    s.fail_write = 1u;
    s.fail_after_apply = true;
    assert(nx_mp6546_set_dq(&dev, 40, 40) == NX_MP6546_ERR_IO);
    assert(s.regs[0x08u] == 40u && s.regs[0x09u] == 0x03E2u);
    assert(s.writes == 1u); /* Do not continue after an ambiguous Ud result. */
    printf("nx_mp6546: signed D/Q and partial-write failures checked\n");
}

static void check_pwm_commit_and_failures(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x07u);
    s.regs[0x00u] |= 0x0010u; /* Application already selected duty mode. */
    nx_mp6546_phase_t phases[3] = {{true, 1000u}, {true, 0u}, {false, 777u}};
    assert(nx_mp6546_set_pwm(&dev, phases) == NX_MP6546_OK);
    assert(s.reads == 0u && s.writes == 3u && s.duty_commits == 1u);
    expect_event(&s, 0u, true, 0x07u);
    expect_event(&s, 1u, true, 0x08u);
    expect_event(&s, 2u, true, 0x09u);
    assert(s.effective_duty[0] == 0x0100u);
    assert(s.effective_duty[1] == 0x01FFu);
    assert(s.effective_duty[2] == 0u);
    clear_trace(&s);
    phases[2].duty_permille = 1001u;
    assert(nx_mp6546_set_pwm(&dev, phases) == NX_MP6546_ERR_PARAM);
    assert(s.event_count == 0u); /* Disabled phases are validated too. */
    phases[0].duty_permille = 0u;
    phases[1].duty_permille = 1000u;
    phases[2].enabled = true;
    phases[2].duty_permille = 500u;
    s.fail_write = 2u;
    assert(nx_mp6546_set_pwm(&dev, phases) == NX_MP6546_ERR_IO);
    assert(s.event_count == 2u && s.duty_commits == 1u);
    assert(s.regs[0x07u] == 0x01FFu); /* New A is pending, not committed. */
    assert(s.effective_duty[0] == 0x0100u && s.effective_duty[1] == 0x01FFu);
    clear_trace(&s);
    assert(nx_mp6546_set_pwm(&dev, phases) == NX_MP6546_OK);
    assert(s.event_count == 3u && s.duty_commits == 2u);
    assert(s.effective_duty[0] == 0x01FFu && s.effective_duty[1] == 0x0100u);
    assert(s.effective_duty[2] >= 0x017Fu && s.effective_duty[2] <= 0x0180u);
    clear_trace(&s);
    phases[0].duty_permille = 1000u;
    s.fail_write = 3u;
    s.fail_after_apply = true;
    assert(nx_mp6546_set_pwm(&dev, phases) == NX_MP6546_ERR_IO);
    assert(s.duty_commits == 3u && s.effective_duty[0] == 0x0100u);
    assert(s.event_count == 3u); /* Reported failure does not imply no commit. */
    printf("nx_mp6546: duty 1000/0/disabled -> 100h/1FFh/000h; 09h commits\n");
}

static void check_configuration_and_commands(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x08u);
    s.regs[0x00u] = 0xE1AFu;
    assert(nx_mp6546_set_sensor_mode(&dev, NX_MP6546_SENSOR_LINEAR_HALL) == NX_MP6546_OK);
    assert((s.regs[0x00u] & 0x0200u) != 0u);
    assert(nx_mp6546_set_sensor_mode(&dev, NX_MP6546_SENSOR_SSC) == NX_MP6546_OK);
    assert((s.regs[0x00u] & 0x0208u) == 0x0008u);
    assert(nx_mp6546_set_sensor_mode(&dev, NX_MP6546_SENSOR_SPI) == NX_MP6546_OK);
    assert(s.regs[0x00u] == 0xE1A7u);
    assert(nx_mp6546_set_drive_mode(&dev, NX_MP6546_DRIVE_DUTY) == NX_MP6546_OK);
    assert(s.regs[0x00u] == 0xE1B7u);
    assert(nx_mp6546_set_drive_mode(&dev, NX_MP6546_DRIVE_VECTOR) == NX_MP6546_OK);
    assert(s.regs[0x00u] == 0xE1A7u);
    assert(nx_mp6546_set_angle_source(&dev, NX_MP6546_ANGLE_TEST) == NX_MP6546_OK);
    assert(s.regs[0x0Au] == 0x0C00u);
    assert(nx_mp6546_set_angle_source(&dev, NX_MP6546_ANGLE_SENSOR) == NX_MP6546_OK);
    assert(s.regs[0x0Au] == 0x0800u);
    s.regs[0x07u] = 0xA000u;
    assert(nx_mp6546_set_test_angle(&dev, 0x0ABCu) == NX_MP6546_OK);
    assert(s.regs[0x07u] == 0xAABCu);
    assert(nx_mp6546_set_motor_angle(&dev, 7u, 0x0123u, false) == NX_MP6546_OK);
    assert(s.regs[0x02u] == 0x1237u && s.regs[0x00u] == 0xE0A7u);

    clear_trace(&s);
    assert(nx_mp6546_set_motor_angle(&dev, 0u, 0u, false) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_motor_angle(&dev, 16u, 0u, false) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_motor_angle(&dev, 7u, 0x1000u, false) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_test_angle(&dev, 0x1000u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_sensor_mode(&dev, (nx_mp6546_sensor_mode_t)99) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_drive_mode(&dev, (nx_mp6546_drive_mode_t)99) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_angle_source(&dev, (nx_mp6546_angle_source_t)99) == NX_MP6546_ERR_PARAM);
    assert(s.event_count == 0u);
    s.fail_write = 2u;
    assert(nx_mp6546_set_motor_angle(&dev, 15u, 0x0FFFu, true) == NX_MP6546_ERR_IO);
    assert(s.regs[0x02u] == 0xFFFFu && s.regs[0x00u] == 0xE0A7u);
    assert(s.writes == 2u); /* New pole/bias can precede failed direction. */

    clear_trace(&s);
    s.regs[0x00u] = 0xC003u;
    assert(nx_mp6546_enter_standby(&dev) == NX_MP6546_OK);
    assert(s.standby && s.regs[0x00u] == 0xE003u && s.writes == 2u);
    expect_event(&s, 0u, false, 0x00u);
    expect_event(&s, 1u, true, 0x00u);
    expect_event(&s, 2u, true, 0x10u);
    clear_trace(&s);
    assert(nx_mp6546_enter_normal(&dev) == NX_MP6546_OK);
    assert(!s.standby && s.event_count == 1u);
    expect_event(&s, 0u, true, 0x0Fu);
    clear_trace(&s);
    s.fail_read = 1u;
    assert(nx_mp6546_enter_standby(&dev) == NX_MP6546_ERR_IO);
    assert(!s.standby && s.writes == 0u);
}

static void check_voltage_conversion(void)
{
    int16_t raw = 123;
    assert(nx_mp6546_voltage_to_raw(1000, 12000u, &raw) == NX_MP6546_OK);
    assert(raw == 74);
    assert(nx_mp6546_voltage_to_raw(-1000, 12000u, &raw) == NX_MP6546_OK);
    assert(raw == -74);
    assert(nx_mp6546_voltage_to_raw(576, 1000u, &raw) == NX_MP6546_OK);
    assert(raw == 511);
    assert(nx_mp6546_voltage_to_raw(577, 1000u, &raw) == NX_MP6546_ERR_PARAM);
    assert(raw == 511); /* No clipping to the positive endpoint. */
    assert(nx_mp6546_voltage_to_raw(-577, 1000u, &raw) == NX_MP6546_OK);
    assert(raw == -512); /* Negative representation has one extra count. */
    assert(nx_mp6546_voltage_to_raw(-578, 1000u, &raw) == NX_MP6546_ERR_PARAM);
    assert(raw == -512);
    assert(nx_mp6546_voltage_to_raw(0, 12000u, &raw) == NX_MP6546_OK && raw == 0);
    assert(nx_mp6546_voltage_to_raw(0, 0u, &raw) == NX_MP6546_ERR_PARAM && raw == 0);
    assert(nx_mp6546_voltage_to_raw(1000, 12000u, NULL) == NX_MP6546_ERR_PARAM);
    raw = 123;
    assert(nx_mp6546_voltage_to_raw(INT32_MAX, 1u, &raw) == NX_MP6546_ERR_PARAM);
    assert(raw == 123);
    assert(nx_mp6546_voltage_to_raw(INT32_MIN, 1u, &raw) == NX_MP6546_ERR_PARAM);
    assert(raw == 123);
    assert(nx_mp6546_voltage_to_raw(INT32_MAX, UINT32_MAX, &raw) == NX_MP6546_OK);
    assert(raw == 443);
    assert(nx_mp6546_voltage_to_raw(INT32_MIN, UINT32_MAX, &raw) == NX_MP6546_OK);
    assert(raw == -443);
    assert(nx_mp6546_voltage_to_raw(1, UINT32_MAX, &raw) == NX_MP6546_OK && raw == 0);
    printf("nx_mp6546: +1 V / 12 V bus -> +74 raw; -1 V -> -74 raw\n");
}

static void check_null_parameters(void)
{
    fake_port_t s;
    nx_mp6546_t dev;
    bench_init(&s, &dev, 0x07u);
    assert(nx_mp6546_read_reg16(&dev, 0x00u, NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_read_sample(&dev, NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_read_angle(&dev, NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_read_faults(&dev, NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_pwm(&dev, NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_write_reg16(NULL, 0x00u, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_update_reg16(NULL, 0x00u, 1u, 0u) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_dq(NULL, 0, 0) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_set_uq(NULL, 0) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_enter_normal(NULL) == NX_MP6546_ERR_PARAM);
    assert(nx_mp6546_enter_standby(NULL) == NX_MP6546_ERR_PARAM);
    assert(s.event_count == 0u);
}

int nx_mp6546_example_run(void)
{
    check_init_and_addresses();
    check_samples_and_faults();
    check_register_guards_and_rmw();
    check_dq_and_partial_writes();
    check_pwm_commit_and_failures();
    check_configuration_and_commands();
    check_voltage_conversion();
    check_null_parameters();
    printf("nx_mp6546: all simulated-register checks passed (I2C wire format not tested)\n");
    return 0;
}
