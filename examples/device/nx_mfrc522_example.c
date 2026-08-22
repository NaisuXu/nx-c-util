/**
 * @file    nx_mfrc522_example.c
 * @brief   MFRC522 driver usage examples.
 *
 * These examples demonstrate the low-level MFRC522 driver API: register access,
 * initialization, CRC calculation, and the non-blocking transceive state machine.
 * They use a mock SPI interface (simulated register file) so they run without
 * real hardware.
 *
 * Higher-level examples (ISO 14443A protocol, Mifare card operations) belong in
 * the middleware examples once those modules are implemented.
 */
#include "src/device/nx_mfrc522.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================
 * Mock Hardware (simulated MFRC522 via SPI)
 * ======================================================================== */

static uint8_t g_mock_regs[64];  /* 64 registers (0x00–0x3F) */
static uint32_t g_mock_time_us = 0;

static bool mock_spi_transfer(void *ctx, const uint8_t *tx_data, uint8_t *rx_data, size_t len)
{
    (void)ctx;
    if (len < 2) {
        return false;
    }

    uint8_t addr_byte = tx_data[0];
    bool is_read = (addr_byte & 0x80) != 0;
    uint8_t addr = (addr_byte >> 1) & 0x3F;

    if (is_read) {
        /* Read operation: first byte is address, subsequent bytes are data */
        if (rx_data) {
            rx_data[0] = 0;  /* Address byte response is ignored */
            for (size_t i = 1; i < len; i++) {
                rx_data[i] = (addr < 64) ? g_mock_regs[addr] : 0;
            }
        }
    } else {
        /* Write operation: first byte is address, subsequent bytes are data */
        for (size_t i = 1; i < len; i++) {
            uint8_t value = tx_data[i];
            if (addr >= 64) {
                continue;
            }

            /* Simulate write-1-to-clear for interrupt registers */
            if (addr == NX_MFRC522_REG_COMIRQ || addr == NX_MFRC522_REG_DIVIRQ) {
                g_mock_regs[addr] &= ~value;
                continue;
            }

            g_mock_regs[addr] = value;

            /* Simulate immediate command effects */
            if (addr == NX_MFRC522_REG_COMMAND) {
                if (value == NX_MFRC522_CMD_SOFT_RESET) {
                    memset(g_mock_regs, 0, sizeof(g_mock_regs));
                    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
                } else if (value == NX_MFRC522_CMD_CALC_CRC) {
                    g_mock_regs[NX_MFRC522_REG_DIVIRQ] |= 0x04;
                    g_mock_regs[NX_MFRC522_REG_CRC_RESULT_L] = 0x34;
                    g_mock_regs[NX_MFRC522_REG_CRC_RESULT_H] = 0x12;
                } else if (value == NX_MFRC522_CMD_IDLE) {
                    g_mock_regs[NX_MFRC522_REG_COMIRQ] = 0;
                }
            }

            /* Simulate FIFO flush */
            if (addr == NX_MFRC522_REG_FIFO_LEVEL && (value & 0x80)) {
                g_mock_regs[NX_MFRC522_REG_FIFO_LEVEL] = 0;
            }
        }
    }

    return true;
}


static void mock_reset(void *ctx)
{
    (void)ctx;
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
}

static uint32_t mock_get_us(void)
{
    g_mock_time_us += 2;
    return g_mock_time_us;
}

/* ========================================================================
 * Example 1: Initialization and Version Check
 * ======================================================================== */

static void example_init_and_version(void)
{
    fprintf(stderr, "=== Example 1: Initialization and Version ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;  /* Simulate MFRC522 v2.0 */
    g_mock_time_us = 0;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,  /* cs_control is optional */
                .is_busy = NULL,     /* Blocking mode */
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };

    bool init_ok = nx_mfrc522_init(&mfrc, &cfg);
    assert(init_ok && "Init should succeed with mock chip");

    uint8_t version = nx_mfrc522_get_version(&mfrc);
    fprintf(stderr, "  Chip version: 0x%02X (expected 0x92)\n", version);
    assert(version == 0x92);

    /* Check that antenna is turned on (TxControlReg bits 0 and 1 set) */
    uint8_t tx_ctrl = g_mock_regs[NX_MFRC522_REG_TX_CONTROL];
    fprintf(stderr, "  TxControl after init: 0x%02X (antenna should be on: 0x03)\n", tx_ctrl);
    assert((tx_ctrl & 0x03) == 0x03 && "Antenna should be on after init");

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 2: CRC Calculation
 * ======================================================================== */

static void example_crc_calculation(void)
{
    fprintf(stderr, "=== Example 2: CRC Calculation ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,
                .is_busy = NULL,
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };
    nx_mfrc522_init(&mfrc, &cfg);

    /* Calculate CRC of a short payload */
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = 0;
    nx_mfrc522_ret_t ret = nx_mfrc522_calc_crc(&mfrc, data, sizeof(data), &crc);

    fprintf(stderr, "  CRC of {01 02 03 04}: 0x%04X\n", crc);
    fprintf(stderr, "  Result: %s\n", (ret == NX_MFRC522_OK) ? "OK" : "ERROR");
    assert(ret == NX_MFRC522_OK && "CRC calculation should succeed");
    assert(crc == 0x1234 && "Mock CRC should be 0x1234");

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 3: Non-Blocking Transceive (Simulated Success)
 * ======================================================================== */

static void example_transceive_success(void)
{
    fprintf(stderr, "=== Example 3: Non-Blocking Transceive (Success) ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,
                .is_busy = NULL,
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };
    nx_mfrc522_init(&mfrc, &cfg);

    /* Start a transceive: send a short command, expect a response */
    uint8_t tx_data[] = {0x26};  /* REQA command (Request Type A) */
    uint8_t rx_buf[16];
    size_t rx_len = 0;

    nx_mfrc522_ret_t ret = nx_mfrc522_transceive_start(&mfrc, tx_data, sizeof(tx_data),
                                                         rx_buf, sizeof(rx_buf), 10000u);
    assert(ret == NX_MFRC522_OK && "Transceive start should succeed");
    fprintf(stderr, "  Transceive started, state=%d\n", nx_mfrc522_get_state(&mfrc));

    /* Simulate TX completion: set TxIRq flag */
    g_mock_regs[NX_MFRC522_REG_COMIRQ] |= NX_MFRC522_IRQ_TX;
    fprintf(stderr, "  Set TX IRQ, COMIRQ=0x%02X, ERROR=0x%02X\n",
            g_mock_regs[NX_MFRC522_REG_COMIRQ], g_mock_regs[NX_MFRC522_REG_ERROR]);
    fprintf(stderr, "  Calling process...\n");
    nx_mfrc522_process(&mfrc);
    fprintf(stderr, "  After process: state=%d (expected RX_BUSY=%d)\n",
            nx_mfrc522_get_state(&mfrc), NX_MFRC522_STATE_RX_BUSY);

    if (nx_mfrc522_get_state(&mfrc) != NX_MFRC522_STATE_RX_BUSY) {
        fprintf(stderr, "  ERROR: State is not RX_BUSY. Checking result...\n");
        size_t tmp_len;
        nx_mfrc522_ret_t tmp_ret = nx_mfrc522_transceive_result(&mfrc, &tmp_len);
        fprintf(stderr, "  Result: %d\n", tmp_ret);
    }
    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_RX_BUSY && "Should be waiting for RX");
    fprintf(stderr, "  TX done, waiting for RX\n");

    /* Simulate RX completion: put 2 bytes in FIFO and set RxIRq */
    g_mock_regs[NX_MFRC522_REG_FIFO_LEVEL] = 2;  /* 2 bytes received */
    g_mock_regs[NX_MFRC522_REG_FIFO_DATA] = 0x04;  /* Mock ATQA byte 1 */
    /* Reading FIFO_DATA advances the pointer, so we fake it by pre-loading */
    /* (In real hardware, each read of FIFO_DATA returns the next byte; our mock is simplified) */
    g_mock_regs[NX_MFRC522_REG_COMIRQ] |= NX_MFRC522_IRQ_RX;
    nx_mfrc522_process(&mfrc);

    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_DONE && "Should be done");
    ret = nx_mfrc522_transceive_result(&mfrc, &rx_len);
    fprintf(stderr, "  RX done: %zu bytes received, result=%d\n", rx_len, ret);
    assert(ret == NX_MFRC522_OK && "Result should be OK");
    /* Note: our mock FIFO is simplified, so rx_len depends on mock behavior */

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 4: Non-Blocking Transceive (Timeout)
 * ======================================================================== */

static void example_transceive_timeout(void)
{
    fprintf(stderr, "=== Example 4: Non-Blocking Transceive (Timeout) ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,
                .is_busy = NULL,
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };
    nx_mfrc522_init(&mfrc, &cfg);

    /* Start a transceive with a short timeout */
    uint8_t tx_data[] = {0x26};
    uint8_t rx_buf[16];
    size_t rx_len = 0;

    nx_mfrc522_ret_t ret = nx_mfrc522_transceive_start(&mfrc, tx_data, sizeof(tx_data),
                                                         rx_buf, sizeof(rx_buf), 5000u);
    assert(ret == NX_MFRC522_OK);
    fprintf(stderr, "  Transceive started with 5ms timeout\n");

    /* Simulate TX done */
    g_mock_regs[NX_MFRC522_REG_COMIRQ] |= NX_MFRC522_IRQ_TX;
    nx_mfrc522_process(&mfrc);
    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_RX_BUSY);

    /* Advance time past the timeout without receiving anything */
    /* Since get_us auto-advances by 2us per call, we need to call it enough times
     * to exceed the 5ms (5000us) timeout. But process() will call get_us multiple
     * times. Let's manually advance time by a large amount. */
    g_mock_time_us += 10000;  /* 10ms, exceeds 5ms timeout */
    nx_mfrc522_process(&mfrc);

    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_ERROR && "Should timeout");
    ret = nx_mfrc522_transceive_result(&mfrc, &rx_len);
    fprintf(stderr, "  Result after timeout: %d (expected TIMEOUT=%d)\n", ret, NX_MFRC522_ERR_TIMEOUT);
    assert(ret == NX_MFRC522_ERR_TIMEOUT && "Should return timeout error");

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 5: Antenna On/Off
 * ======================================================================== */

static void example_antenna_control(void)
{
    fprintf(stderr, "=== Example 5: Antenna Control ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,
                .is_busy = NULL,
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };
    nx_mfrc522_init(&mfrc, &cfg);

    /* Antenna should be on after init */
    uint8_t tx_ctrl = g_mock_regs[NX_MFRC522_REG_TX_CONTROL];
    fprintf(stderr, "  After init: TxControl=0x%02X (antenna on)\n", tx_ctrl);
    assert((tx_ctrl & 0x03) == 0x03);

    /* Turn antenna off */
    nx_mfrc522_antenna_off(&mfrc);
    tx_ctrl = g_mock_regs[NX_MFRC522_REG_TX_CONTROL];
    fprintf(stderr, "  After antenna_off: TxControl=0x%02X (antenna off)\n", tx_ctrl);
    assert((tx_ctrl & 0x03) == 0x00);

    /* Turn antenna back on */
    nx_mfrc522_antenna_on(&mfrc);
    tx_ctrl = g_mock_regs[NX_MFRC522_REG_TX_CONTROL];
    fprintf(stderr, "  After antenna_on: TxControl=0x%02X (antenna on)\n", tx_ctrl);
    assert((tx_ctrl & 0x03) == 0x03);

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 6: Non-Blocking Interface Mode
 * ======================================================================== */

static bool g_mock_spi_busy = false;
static uint32_t g_mock_busy_count = 0;

static bool mock_spi_transfer_nonblocking(void *ctx, const uint8_t *tx_data, uint8_t *rx_data, size_t len)
{
    /* Simulate async transfer: queue operation, set busy flag */
    bool result = mock_spi_transfer(ctx, tx_data, rx_data, len);

    /* Simulate busy for next 10 is_busy() calls (simulates ~100us transfer time) */
    g_mock_busy_count = 10;
    g_mock_spi_busy = true;

    return result;
}

static bool mock_spi_is_busy(void *ctx)
{
    (void)ctx;
    if (g_mock_spi_busy) {
        if (g_mock_busy_count > 0) {
            g_mock_busy_count--;
            if (g_mock_busy_count == 0) {
                g_mock_spi_busy = false;
            }
        }
    }
    return g_mock_spi_busy;
}

static void example_nonblocking_interface(void)
{
    fprintf(stderr, "=== Example 6: Non-Blocking Interface Mode ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;
    g_mock_spi_busy = false;

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer_nonblocking,
                .cs_control = NULL,
                .is_busy = mock_spi_is_busy,  /* Non-blocking mode */
            }
        },
        .reset  = mock_reset,
        .get_us = mock_get_us,
        .io_ctx = NULL,
    };

    bool init_ok = nx_mfrc522_init(&mfrc, &cfg);
    assert(init_ok && "Init should succeed");
    fprintf(stderr, "  Init completed (with busy-wait during init)\n");

    /* CRC calculation in non-blocking mode */
    uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = 0;
    nx_mfrc522_ret_t ret = nx_mfrc522_calc_crc(&mfrc, test_data, sizeof(test_data), &crc);
    assert(ret == NX_MFRC522_OK && "CRC should succeed");
    fprintf(stderr, "  CRC calculation completed (with busy-wait)\n");

    /* Transceive remains non-blocking as before */
    uint8_t tx_data[] = {0x26};
    uint8_t rx_buf[64];
    ret = nx_mfrc522_transceive_start(&mfrc, tx_data, sizeof(tx_data), rx_buf, sizeof(rx_buf), 5000);
    assert(ret == NX_MFRC522_OK);
    fprintf(stderr, "  Transceive started (non-blocking)\n");

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Example 7: IRQ Pin Optimization
 * ======================================================================== */

static bool g_mock_irq_pin = true;  /* true = low/asserted, false = high/idle */

static bool mock_read_irq(void *ctx)
{
    (void)ctx;
    /* IRQ pin reflects ComIrqReg state: asserted (low/true) when any interrupt bit is set */
    return g_mock_irq_pin;
}

static void example_irq_optimization(void)
{
    fprintf(stderr, "=== Example 7: IRQ Pin Optimization ===\n");

    /* Reset mock state */
    memset(g_mock_regs, 0, sizeof(g_mock_regs));
    g_mock_regs[NX_MFRC522_REG_VERSION] = 0x92;
    g_mock_time_us = 0;
    g_mock_irq_pin = false;  /* IRQ idle initially */

    nx_mfrc522_t mfrc;
    nx_mfrc522_cfg_t cfg = {
        .if_type = NX_MFRC522_IF_SPI,
        .iface = {
            .spi = {
                .transfer = mock_spi_transfer,
                .cs_control = NULL,
                .is_busy = NULL,
            }
        },
        .reset   = mock_reset,
        .read_irq = mock_read_irq,  /* IRQ pin connected */
        .get_us  = mock_get_us,
        .io_ctx  = NULL,
    };
    nx_mfrc522_init(&mfrc, &cfg);

    /* Start a transceive */
    uint8_t tx_data[] = {0x26};
    uint8_t rx_buf[64];
    nx_mfrc522_transceive_start(&mfrc, tx_data, sizeof(tx_data), rx_buf, sizeof(rx_buf), 5000);

    /* process() should return immediately when IRQ pin is not asserted */
    g_mock_irq_pin = false;  /* IRQ idle */
    fprintf(stderr, "  IRQ pin idle (high), calling process...\n");
    nx_mfrc522_process(&mfrc);
    fprintf(stderr, "  process() returned quickly without SPI read\n");
    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_TX_BUSY && "Should still be busy");

    /* Now assert IRQ and set TX done */
    g_mock_irq_pin = true;  /* IRQ asserted (low) */
    g_mock_regs[NX_MFRC522_REG_COMIRQ] = NX_MFRC522_IRQ_TX;
    fprintf(stderr, "  IRQ pin asserted (low), calling process...\n");
    nx_mfrc522_process(&mfrc);
    fprintf(stderr, "  process() read registers and transitioned to RX_BUSY\n");
    assert(nx_mfrc522_get_state(&mfrc) == NX_MFRC522_STATE_RX_BUSY);

    fprintf(stderr, "  passed\n\n");
}

/* ========================================================================
 * Main Entry
 * ======================================================================== */

int nx_mfrc522_example_run(void)
{
    example_init_and_version();
    example_crc_calculation();
    example_transceive_success();
    example_transceive_timeout();
    example_antenna_control();
    example_nonblocking_interface();
    example_irq_optimization();

    fprintf(stderr, "nx_mfrc522: all checks passed\n");
    return 0;
}
