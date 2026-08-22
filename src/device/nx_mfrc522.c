/**
 * @file    nx_mfrc522.c
 * @brief   MFRC522 driver implementation.
 */
#include "src/device/nx_mfrc522.h"
#include <string.h>

/* ========================================================================
 * Low-Level Interface Helpers
 * ======================================================================== */

/**
 * @brief Wait for interface ready (only used in blocking contexts: init, calc_crc).
 */
static void wait_if_busy(const nx_mfrc522_t *m)
{
    if (m->run.is_busy_fn != NULL) {
        while (m->run.is_busy_fn(m->cfg.io_ctx)) {
            /* Busy-wait in blocking contexts only.
             * In real hardware, this polls the interface status.
             * Some implementations may need a small delay here to avoid
             * spinning too fast (e.g., a few microseconds). */
        }
    }
}

/**
 * @brief Read one register via SPI.
 */
static uint8_t spi_read_reg(const nx_mfrc522_t *m, uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t)((addr << 1) | 0x80), 0x00};
    uint8_t rx[2] = {0, 0};

    if (m->cfg.iface.spi.cs_control != NULL) {
        m->cfg.iface.spi.cs_control(m->cfg.io_ctx, true);
    }
    (void)m->cfg.iface.spi.transfer(m->cfg.io_ctx, tx, rx, 2);
    if (m->cfg.iface.spi.cs_control != NULL) {
        m->cfg.iface.spi.cs_control(m->cfg.io_ctx, false);
    }

    return rx[1];
}

/**
 * @brief Write one register via SPI.
 */
static void spi_write_reg(const nx_mfrc522_t *m, uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t)(addr << 1), val};

    if (m->cfg.iface.spi.cs_control != NULL) {
        m->cfg.iface.spi.cs_control(m->cfg.io_ctx, true);
    }
    (void)m->cfg.iface.spi.transfer(m->cfg.io_ctx, tx, NULL, 2);
    if (m->cfg.iface.spi.cs_control != NULL) {
        m->cfg.iface.spi.cs_control(m->cfg.io_ctx, false);
    }
}

/**
 * @brief Read one register via I2C.
 */
static uint8_t i2c_read_reg(const nx_mfrc522_t *m, uint8_t addr)
{
    uint8_t val = 0;
    (void)m->cfg.iface.i2c.read(m->cfg.io_ctx, addr, &val, 1);
    return val;
}

/**
 * @brief Write one register via I2C.
 */
static void i2c_write_reg(const nx_mfrc522_t *m, uint8_t addr, uint8_t val)
{
    uint8_t data[2] = {addr, val};
    (void)m->cfg.iface.i2c.write(m->cfg.io_ctx, data, 2);
}

/**
 * @brief Read one register via UART.
 */
static uint8_t uart_read_reg(const nx_mfrc522_t *m, uint8_t addr)
{
    uint8_t cmd[2] = {0x55, (uint8_t)((addr << 1) | 0x01)};
    uint8_t val = 0;

    (void)m->cfg.iface.uart.write(m->cfg.io_ctx, cmd, 2);

    /* Poll for response (non-blocking read with timeout) */
    uint32_t start = m->cfg.get_us();
    while (m->cfg.iface.uart.read(m->cfg.io_ctx, &val, 1) == 0) {
        if ((m->cfg.get_us() - start) > 10000) {  /* 10ms timeout */
            break;
        }
    }

    return val;
}

/**
 * @brief Write one register via UART.
 */
static void uart_write_reg(const nx_mfrc522_t *m, uint8_t addr, uint8_t val)
{
    uint8_t cmd[3] = {0x55, (uint8_t)(addr << 1), val};
    (void)m->cfg.iface.uart.write(m->cfg.io_ctx, cmd, 3);
}

/* ========================================================================
 * Register Helpers (Interface-Agnostic)
 * ======================================================================== */

static inline uint8_t read_reg(const nx_mfrc522_t *m, uint8_t addr)
{
    return m->run.read_reg_fn(m, addr);
}

static inline void write_reg(const nx_mfrc522_t *m, uint8_t addr, uint8_t val)
{
    m->run.write_reg_fn(m, addr, val);
}

static inline void set_reg_bits(const nx_mfrc522_t *m, uint8_t addr, uint8_t mask)
{
    uint8_t val = read_reg(m, addr);
    write_reg(m, addr, val | mask);
}

static inline void clear_reg_bits(const nx_mfrc522_t *m, uint8_t addr, uint8_t mask)
{
    uint8_t val = read_reg(m, addr);
    write_reg(m, addr, val & ~mask);
}

/* ========================================================================
 * FIFO Operations
 * ======================================================================== */

static void fifo_flush(const nx_mfrc522_t *m)
{
    write_reg(m, NX_MFRC522_REG_FIFO_LEVEL, NX_MFRC522_FIFO_FLUSH);
}

static void fifo_write(const nx_mfrc522_t *m, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        write_reg(m, NX_MFRC522_REG_FIFO_DATA, data[i]);
    }
}

static size_t fifo_read(const nx_mfrc522_t *m, uint8_t *buf, size_t max_len)
{
    uint8_t level = read_reg(m, NX_MFRC522_REG_FIFO_LEVEL) & NX_MFRC522_FIFO_LEVEL_MASK;
    size_t to_read = (level < max_len) ? level : max_len;
    for (size_t i = 0; i < to_read; i++) {
        buf[i] = read_reg(m, NX_MFRC522_REG_FIFO_DATA);
    }
    return to_read;
}

/* ========================================================================
 * Command Execution
 * ======================================================================== */

static void exec_command(const nx_mfrc522_t *m, uint8_t cmd)
{
    write_reg(m, NX_MFRC522_REG_COMMAND, cmd);
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

bool nx_mfrc522_init(nx_mfrc522_t *m, const nx_mfrc522_cfg_t *cfg)
{
    if (m == NULL || cfg == NULL || cfg->get_us == NULL) {
        return false;
    }

    /* Validate interface-specific callbacks (is_busy and cs_control are optional) */
    switch (cfg->if_type) {
    case NX_MFRC522_IF_SPI:
        if (cfg->iface.spi.transfer == NULL) {
            return false;
        }
        break;
    case NX_MFRC522_IF_I2C:
        if (cfg->iface.i2c.write == NULL || cfg->iface.i2c.read == NULL) {
            return false;
        }
        break;
    case NX_MFRC522_IF_UART:
        if (cfg->iface.uart.write == NULL || cfg->iface.uart.read == NULL) {
            return false;
        }
        break;
    default:
        return false;
    }

    memcpy(&m->cfg, cfg, sizeof(nx_mfrc522_cfg_t));
    m->run.state = NX_MFRC522_STATE_IDLE;

    /* Set up unified function pointers based on interface type */
    switch (m->cfg.if_type) {
    case NX_MFRC522_IF_SPI:
        m->run.read_reg_fn = (uint8_t (*)(const void *, uint8_t))spi_read_reg;
        m->run.write_reg_fn = (void (*)(const void *, uint8_t, uint8_t))spi_write_reg;
        m->run.is_busy_fn = m->cfg.iface.spi.is_busy;
        break;
    case NX_MFRC522_IF_I2C:
        m->run.read_reg_fn = (uint8_t (*)(const void *, uint8_t))i2c_read_reg;
        m->run.write_reg_fn = (void (*)(const void *, uint8_t, uint8_t))i2c_write_reg;
        m->run.is_busy_fn = m->cfg.iface.i2c.is_busy;
        break;
    case NX_MFRC522_IF_UART:
        m->run.read_reg_fn = (uint8_t (*)(const void *, uint8_t))uart_read_reg;
        m->run.write_reg_fn = (void (*)(const void *, uint8_t, uint8_t))uart_write_reg;
        m->run.is_busy_fn = m->cfg.iface.uart.is_busy;
        break;
    default:
        return false;  /* Already checked above, but for safety */
    }

    /* Hardware reset if available */
    if (m->cfg.reset != NULL) {
        m->cfg.reset(m->cfg.io_ctx);
    }

    /* Wait for interface ready before starting initialization sequence */
    wait_if_busy(m);

    /* Soft reset */
    exec_command(m, NX_MFRC522_CMD_SOFT_RESET);
    wait_if_busy(m);

    /* Wait for reset to complete (typ. 37.74us @ 13.56MHz, allow margin) */
    uint32_t start = m->cfg.get_us();
    while ((m->cfg.get_us() - start) < 100u) {
        /* busy wait */
    }

    /* Check version to confirm chip is responding */
    uint8_t ver = nx_mfrc522_get_version(m);
    if (ver != 0x92 && ver != 0x91 && ver != 0x88) {
        /* Expected: 0x92 (v2.0), 0x91 (v1.0), 0x88 (Chinese clone) */
        return false;
    }

    /* Timer: Tauto = 1, prescaler = 0x00A9 (169), reload = 0x03E8 (1000)
     * Timeout = (TPrescaler * 2 + 1) * (TReload + 1) / 13.56MHz
     *         = (169*2+1) * 1001 / 13.56MHz ≈ 25ms */
    write_reg(m, NX_MFRC522_REG_TMODE, 0x80);         /* Tauto=1, prescaler high bit=0 */
    write_reg(m, NX_MFRC522_REG_TPRESCALER, 0xA9);    /* Prescaler low 8 bits = 169 */
    write_reg(m, NX_MFRC522_REG_TRELOAD_H, 0x03);     /* Reload high = 3 */
    write_reg(m, NX_MFRC522_REG_TRELOAD_L, 0xE8);     /* Reload low = 232 (total 1000) */

    /* TxASK: force 100% ASK modulation */
    write_reg(m, NX_MFRC522_REG_TX_ASK, 0x40);

    /* Mode: CRCPreset = 0x6363 (ISO 14443A) */
    write_reg(m, NX_MFRC522_REG_MODE, 0x3D);

    /* Antenna gain: RxGain = max (48dB) */
    write_reg(m, NX_MFRC522_REG_RF_CFG, 0x70);

    /* Turn on antenna */
    nx_mfrc522_antenna_on(m);

    return true;
}

/* ========================================================================
 * Antenna Control
 * ======================================================================== */

void nx_mfrc522_antenna_on(nx_mfrc522_t *m)
{
    uint8_t val = read_reg(m, NX_MFRC522_REG_TX_CONTROL);
    if ((val & 0x03) != 0x03) {
        set_reg_bits(m, NX_MFRC522_REG_TX_CONTROL,
                     NX_MFRC522_TX_CONTROL_TX1_RF_EN | NX_MFRC522_TX_CONTROL_TX2_RF_EN);
    }
}

void nx_mfrc522_antenna_off(nx_mfrc522_t *m)
{
    clear_reg_bits(m, NX_MFRC522_REG_TX_CONTROL,
                   NX_MFRC522_TX_CONTROL_TX1_RF_EN | NX_MFRC522_TX_CONTROL_TX2_RF_EN);
}

/* ========================================================================
 * Version
 * ======================================================================== */

uint8_t nx_mfrc522_get_version(const nx_mfrc522_t *m)
{
    return read_reg(m, NX_MFRC522_REG_VERSION);
}

/* ========================================================================
 * CRC Calculation
 * ======================================================================== */

nx_mfrc522_ret_t nx_mfrc522_calc_crc(nx_mfrc522_t *m,
                                      const uint8_t *data,
                                      size_t         len,
                                      uint16_t      *crc_out)
{
    if (m == NULL || data == NULL || crc_out == NULL) {
        return NX_MFRC522_ERR_INVALID;
    }

    /* Wait for interface ready (blocking context) */
    wait_if_busy(m);

    /* Stop any active command */
    exec_command(m, NX_MFRC522_CMD_IDLE);
    wait_if_busy(m);

    /* Clear CRCIRq flag */
    clear_reg_bits(m, NX_MFRC522_REG_DIVIRQ, 0x04);
    wait_if_busy(m);

    /* Flush FIFO and write data */
    fifo_flush(m);
    wait_if_busy(m);
    fifo_write(m, data, len);
    wait_if_busy(m);

    /* Start CRC calculation */
    exec_command(m, NX_MFRC522_CMD_CALC_CRC);
    wait_if_busy(m);

    /* Poll for completion (CRCIRq bit set in DivIrqReg) */
    uint32_t start = m->cfg.get_us();
    while (true) {
        wait_if_busy(m);
        uint8_t irq = read_reg(m, NX_MFRC522_REG_DIVIRQ);
        wait_if_busy(m);
        if (irq & 0x04) {
            /* CRC done */
            break;
        }
        if ((m->cfg.get_us() - start) > 5000u) {
            /* Timeout (5ms is way more than needed, typical <100us) */
            return NX_MFRC522_ERR_TIMEOUT;
        }
    }

    /* Read result: LSB first, MSB second */
    wait_if_busy(m);
    uint8_t low  = read_reg(m, NX_MFRC522_REG_CRC_RESULT_L);
    wait_if_busy(m);
    uint8_t high = read_reg(m, NX_MFRC522_REG_CRC_RESULT_H);
    wait_if_busy(m);
    *crc_out = (uint16_t)low | ((uint16_t)high << 8);

    return NX_MFRC522_OK;
}

/* ========================================================================
 * Transceive (Non-Blocking)
 * ======================================================================== */

nx_mfrc522_ret_t nx_mfrc522_transceive_start(nx_mfrc522_t *m,
                                               const uint8_t *tx_data,
                                               size_t         tx_len,
                                               uint8_t       *rx_buf,
                                               size_t         rx_max,
                                               uint32_t       timeout_us)
{
    if (m == NULL || tx_data == NULL || (rx_max > 0 && rx_buf == NULL)) {
        return NX_MFRC522_ERR_INVALID;
    }
    if (m->run.state != NX_MFRC522_STATE_IDLE) {
        return NX_MFRC522_ERR_INVALID;  /* operation already in progress */
    }
    if (tx_len > NX_MFRC522_FIFO_SIZE) {
        return NX_MFRC522_ERR_INVALID;
    }

    /* Stop any active command */
    exec_command(m, NX_MFRC522_CMD_IDLE);

    /* Clear all IRQ flags */
    write_reg(m, NX_MFRC522_REG_COMIRQ, 0x7F);

    /* Flush FIFO */
    fifo_flush(m);

    /* Write TX data to FIFO */
    fifo_write(m, tx_data, tx_len);

    /* Start Transceive command */
    exec_command(m, NX_MFRC522_CMD_TRANSCEIVE);

    /* Set StartSend bit to begin transmission */
    set_reg_bits(m, NX_MFRC522_REG_BIT_FRAMING, 0x80);

    /* Arm state machine */
    m->run.state       = NX_MFRC522_STATE_TX_BUSY;
    m->run.result      = NX_MFRC522_OK;
    m->run.deadline_us = m->cfg.get_us() + timeout_us;
    m->run.rx_buf      = rx_buf;
    m->run.rx_max      = rx_max;
    m->run.rx_len      = 0;

    return NX_MFRC522_OK;
}

void nx_mfrc522_process(nx_mfrc522_t *m)
{
    if (m == NULL) {
        return;
    }

    if (m->run.state != NX_MFRC522_STATE_TX_BUSY && m->run.state != NX_MFRC522_STATE_RX_BUSY) {
        return;  /* nothing to do */
    }

    /* Quick exit if IRQ pin is available and not asserted (optimization) */
    if (m->cfg.read_irq != NULL && !m->cfg.read_irq(m->cfg.io_ctx)) {
        /* IRQ pin high = no interrupt pending, skip register read */
        return;
    }

    /* Check timeout */
    uint32_t now = m->cfg.get_us();
    if ((now - m->run.deadline_us) < 0x80000000u) {
        /* Deadline passed (unsigned wrap-safe comparison) */
        m->run.state  = NX_MFRC522_STATE_ERROR;
        m->run.result = NX_MFRC522_ERR_TIMEOUT;
        exec_command(m, NX_MFRC522_CMD_IDLE);
        return;
    }

    /* Read IRQ flags */
    uint8_t irq = read_reg(m, NX_MFRC522_REG_COMIRQ);

    /* Check for errors */
    if (irq & NX_MFRC522_IRQ_ERR) {
        uint8_t err = read_reg(m, NX_MFRC522_REG_ERROR);
        m->run.state = NX_MFRC522_STATE_ERROR;

        if (err & NX_MFRC522_ERR_BIT_COLL) {
            m->run.result = NX_MFRC522_ERR_COLLISION;
        } else if (err & NX_MFRC522_ERR_BIT_CRC) {
            m->run.result = NX_MFRC522_ERR_CRC;
        } else if (err & NX_MFRC522_ERR_BIT_PROTOCOL) {
            m->run.result = NX_MFRC522_ERR_PROTOCOL;
        } else if (err & NX_MFRC522_ERR_BIT_PARITY) {
            m->run.result = NX_MFRC522_ERR_PARITY;
        } else if (err & NX_MFRC522_ERR_BIT_BUFFER_OVFL) {
            m->run.result = NX_MFRC522_ERR_BUFFER_OVFL;
        } else {
            m->run.result = NX_MFRC522_ERR_INTERNAL;
        }

        exec_command(m, NX_MFRC522_CMD_IDLE);
        return;
    }

    /* Check for RX completion */
    if (irq & NX_MFRC522_IRQ_RX) {
        /* Transmission finished, data received */
        m->run.rx_len = fifo_read(m, m->run.rx_buf, m->run.rx_max);
        m->run.state  = NX_MFRC522_STATE_DONE;
        m->run.result = NX_MFRC522_OK;
        exec_command(m, NX_MFRC522_CMD_IDLE);
        return;
    }

    /* Check for idle (no response received, but transmission done + timer expired) */
    if (irq & NX_MFRC522_IRQ_IDLE) {
        /* Command terminated without receiving data (e.g., no card response) */
        m->run.state  = NX_MFRC522_STATE_ERROR;
        m->run.result = NX_MFRC522_ERR_TIMEOUT;
        exec_command(m, NX_MFRC522_CMD_IDLE);
        return;
    }

    /* Transition from TX_BUSY to RX_BUSY once transmission completes */
    if (m->run.state == NX_MFRC522_STATE_TX_BUSY && (irq & NX_MFRC522_IRQ_TX)) {
        m->run.state = NX_MFRC522_STATE_RX_BUSY;
    }

    /* Still busy, check again next time */
}

nx_mfrc522_ret_t nx_mfrc522_transceive_result(const nx_mfrc522_t *m, size_t *rx_len)
{
    if (m == NULL) {
        return NX_MFRC522_ERR_INVALID;
    }
    if (m->run.state != NX_MFRC522_STATE_DONE && m->run.state != NX_MFRC522_STATE_ERROR) {
        return NX_MFRC522_ERR_INVALID;  /* operation still in progress or not started */
    }

    if (rx_len != NULL) {
        *rx_len = (m->run.state == NX_MFRC522_STATE_DONE) ? m->run.rx_len : 0;
    }

    return m->run.result;
}

nx_mfrc522_state_t nx_mfrc522_get_state(const nx_mfrc522_t *m)
{
    return (m != NULL) ? m->run.state : NX_MFRC522_STATE_IDLE;
}
