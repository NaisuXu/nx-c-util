/**
 * @file    nx_mfrc522.h
 * @brief   MFRC522 contactless reader IC driver (register and command layer).
 *
 * Platform-independent driver for the NXP MFRC522 13.56MHz RFID transceiver.
 * Supports ISO/IEC 14443A at up to 848 kbps. Hardware interface (SPI, I2C, or
 * UART) is abstracted via caller-provided read/write callbacks.
 *
 * This is the chip command layer: register access, FIFO operations, PCD commands
 * (Idle, Transceive, CalcCRC, etc.), and non-blocking transceive with timeout.
 * It does NOT implement ISO 14443A protocol logic — that belongs in the
 * middleware layer (nx_iso14443a_pcd).
 *
 * ## Hardware Interface Support
 *
 * The driver abstracts register access via `read_reg(addr)` and `write_reg(addr, val)`
 * callbacks. All three MFRC522 interfaces can be supported:
 *
 * **SPI (most common):**
 * - Read:  Send `[addr<<1 | 0x80]`, receive `[value]`
 * - Write: Send `[addr<<1]`, send `[value]`
 *
 * **I2C:**
 * - Read:  Write device_addr + register_addr, read value
 * - Write: Write device_addr + register_addr + value
 *
 * **UART:**
 * - Read:  Send `[0x55, (addr<<1)|0x01]`, receive `[value]`
 * - Write: Send `[0x55, addr<<1, value]`
 *
 * The callbacks encapsulate the protocol framing, so the driver itself is
 * interface-agnostic.
 *
 * ## Blocking vs Non-Blocking Mode
 *
 * Each interface supports an optional `is_busy` callback:
 * - **NULL** (blocking mode): read/write callbacks block until the operation completes.
 * - **Non-NULL** (non-blocking mode): read/write return immediately after queueing
 *   the operation (e.g., DMA started); caller must poll `is_busy()` before the next
 *   operation. The driver only busy-waits during init and calc_crc (both are rare,
 *   one-shot operations). Register reads/writes in transceive_start/process never block.
 *
 * ## Typical Usage
 *
 *   1. Provide cfg (SPI/I2C/UART read/write, reset, get_us callbacks).
 *   2. nx_mfrc522_init() — soft reset, configure RF, turn on antenna.
 *   3. nx_mfrc522_transceive_start() — load TX data, start command.
 *   4. Poll nx_mfrc522_process() in the main loop until state != BUSY.
 *   5. nx_mfrc522_transceive_result() — retrieve RX data.
 *
 * The transceive operation is non-blocking: start returns immediately, and
 * process() advances the state machine (polling IRQ flags and timeout).
 */
#ifndef NX_MFRC522_H
#define NX_MFRC522_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Register Addresses
 * ======================================================================== */

/* Command and status */
#define NX_MFRC522_REG_COMMAND       0x01  /**< Starts/stops command execution */
#define NX_MFRC522_REG_COMIEN        0x02  /**< Interrupt enable register */
#define NX_MFRC522_REG_DIVIEN        0x03  /**< Interrupt enable for divider */
#define NX_MFRC522_REG_COMIRQ        0x04  /**< Interrupt request bits */
#define NX_MFRC522_REG_DIVIRQ        0x05  /**< Interrupt request for divider */
#define NX_MFRC522_REG_ERROR         0x06  /**< Error flags */
#define NX_MFRC522_REG_STATUS1       0x07  /**< Communication status */
#define NX_MFRC522_REG_STATUS2       0x08  /**< Receiver and transmitter status */
#define NX_MFRC522_REG_FIFO_DATA     0x09  /**< FIFO buffer I/O */
#define NX_MFRC522_REG_FIFO_LEVEL    0x0A  /**< FIFO depth and flush control */

/* Control registers */
#define NX_MFRC522_REG_CONTROL       0x0C  /**< Miscellaneous control */
#define NX_MFRC522_REG_BIT_FRAMING   0x0D  /**< Bit-oriented frame adjustments */
#define NX_MFRC522_REG_COLL          0x0E  /**< Collision detection */

/* Timer registers */
#define NX_MFRC522_REG_TMODE         0x2A  /**< Timer mode */
#define NX_MFRC522_REG_TPRESCALER    0x2B  /**< Timer prescaler */
#define NX_MFRC522_REG_TRELOAD_H     0x2C  /**< Timer reload high byte */
#define NX_MFRC522_REG_TRELOAD_L     0x2D  /**< Timer reload low byte */

/* RF configuration */
#define NX_MFRC522_REG_TX_CONTROL    0x14  /**< Antenna driver enable */
#define NX_MFRC522_REG_TX_ASK        0x15  /**< Modulation settings */
#define NX_MFRC522_REG_MODE          0x11  /**< General mode settings */
#define NX_MFRC522_REG_RF_CFG        0x26  /**< Receiver gain */

/* CRC */
#define NX_MFRC522_REG_CRC_RESULT_H  0x21  /**< CRC result high byte */
#define NX_MFRC522_REG_CRC_RESULT_L  0x22  /**< CRC result low byte */

/* Version */
#define NX_MFRC522_REG_VERSION       0x37  /**< Chip version */

/* ========================================================================
 * Commands (written to CommandReg)
 * ======================================================================== */

#define NX_MFRC522_CMD_IDLE          0x00  /**< No action; cancel current command */
#define NX_MFRC522_CMD_MEM           0x01  /**< Store 25 bytes into internal buffer */
#define NX_MFRC522_CMD_GENERATE_RANDOM_ID 0x02  /**< Generate 10-byte random ID */
#define NX_MFRC522_CMD_CALC_CRC      0x03  /**< CRC coprocessor */
#define NX_MFRC522_CMD_TRANSMIT      0x04  /**< Transmit FIFO data */
#define NX_MFRC522_CMD_NO_CMD_CHANGE 0x07  /**< No command change */
#define NX_MFRC522_CMD_RECEIVE       0x08  /**< Activate receiver */
#define NX_MFRC522_CMD_TRANSCEIVE    0x0C  /**< Transmit FIFO data, then activate receiver */
#define NX_MFRC522_CMD_MF_AUTHENT    0x0E  /**< Mifare authentication */
#define NX_MFRC522_CMD_SOFT_RESET    0x0F  /**< Reset the chip */

/* ========================================================================
 * Bit Masks
 * ======================================================================== */

/* ComIrqReg / ComIEnReg bits */
#define NX_MFRC522_IRQ_TIMER         0x01  /**< Timer interrupt */
#define NX_MFRC522_IRQ_ERR           0x02  /**< Error interrupt */
#define NX_MFRC522_IRQ_LO_ALERT      0x04  /**< Low alert (FIFO) */
#define NX_MFRC522_IRQ_HI_ALERT      0x08  /**< High alert (FIFO) */
#define NX_MFRC522_IRQ_IDLE          0x10  /**< Command terminated (idle) */
#define NX_MFRC522_IRQ_RX            0x20  /**< Receiver finished */
#define NX_MFRC522_IRQ_TX            0x40  /**< Transmitter finished */

/* ErrorReg bits */
#define NX_MFRC522_ERR_BIT_PROTOCOL      0x01  /**< Protocol error */
#define NX_MFRC522_ERR_BIT_PARITY        0x02  /**< Parity error */
#define NX_MFRC522_ERR_BIT_CRC           0x04  /**< CRC error */
#define NX_MFRC522_ERR_BIT_COLL          0x08  /**< Collision detected */
#define NX_MFRC522_ERR_BIT_BUFFER_OVFL   0x10  /**< FIFO buffer overflow */
#define NX_MFRC522_ERR_BIT_TEMP          0x40  /**< Temperature error */
#define NX_MFRC522_ERR_BIT_WR            0x80  /**< Data write error */

/* FIFOLevelReg bits */
#define NX_MFRC522_FIFO_LEVEL_MASK   0x7F  /**< Number of bytes in FIFO */
#define NX_MFRC522_FIFO_FLUSH        0x80  /**< Flush FIFO buffer */

/* Status1Reg bits */
#define NX_MFRC522_STATUS1_CRC_OK    0x40  /**< CRC coprocessor done, result OK */

/* TxControlReg bits */
#define NX_MFRC522_TX_CONTROL_TX1_RF_EN 0x01  /**< Antenna driver 1 enable */
#define NX_MFRC522_TX_CONTROL_TX2_RF_EN 0x02  /**< Antenna driver 2 enable */

/* ========================================================================
 * Constants
 * ======================================================================== */

#define NX_MFRC522_FIFO_SIZE         64u   /**< Internal FIFO buffer size */

/* ========================================================================
 * Return Codes
 * ======================================================================== */

/**
 * @brief MFRC522 operation result codes.
 */
typedef enum {
    NX_MFRC522_OK = 0,           /**< Success */
    NX_MFRC522_ERR_TIMEOUT,      /**< Operation timed out */
    NX_MFRC522_ERR_COLLISION,    /**< Collision detected */
    NX_MFRC522_ERR_CRC,          /**< CRC mismatch */
    NX_MFRC522_ERR_PROTOCOL,     /**< Protocol error */
    NX_MFRC522_ERR_PARITY,       /**< Parity error */
    NX_MFRC522_ERR_BUFFER_OVFL,  /**< FIFO overflow */
    NX_MFRC522_ERR_INTERNAL,     /**< Internal error (temperature, write fault) */
    NX_MFRC522_ERR_INVALID,      /**< Invalid parameter */
} nx_mfrc522_ret_t;

/**
 * @brief MFRC522 transceive state.
 */
typedef enum {
    NX_MFRC522_STATE_IDLE = 0,   /**< No operation in progress */
    NX_MFRC522_STATE_TX_BUSY,    /**< Transmitting */
    NX_MFRC522_STATE_RX_BUSY,    /**< Receiving */
    NX_MFRC522_STATE_DONE,       /**< Operation completed successfully */
    NX_MFRC522_STATE_ERROR,      /**< Operation failed */
} nx_mfrc522_state_t;

/* ========================================================================
 * Hardware Interface Abstraction
 * ======================================================================== */

/**
 * @brief MFRC522 interface type.
 */
typedef enum {
    NX_MFRC522_IF_SPI,   /**< SPI interface */
    NX_MFRC522_IF_I2C,   /**< I2C interface */
    NX_MFRC522_IF_UART,  /**< UART interface */
} nx_mfrc522_if_type_t;

/**
 * @brief SPI interface callbacks.
 *
 * transfer: full-duplex SPI transfer. Must block until the write is accepted
 * (buffered or DMA queued), though transmission may continue in the background.
 * Returns true on success, false on error.
 *
 * cs_control: drive chip select. true = assert (pull low), false = deassert (pull high).
 * May be NULL if CS is managed by hardware or fixed for a single slave.
 *
 * is_busy: optional; returns true while the previous transfer is still in progress.
 * NULL means transfers complete when transfer() returns (blocking mode).
 * Non-NULL means the interface is non-blocking: caller must check is_busy() before
 * starting a new transfer, and poll is_busy() after transfer() returns.
 */
typedef struct {
    bool (*transfer)(void *ctx, const uint8_t *tx_data, uint8_t *rx_data, size_t len);
    void (*cs_control)(void *ctx, bool assert);  /* Optional; may be NULL */
    bool (*is_busy)(void *ctx);                  /* Optional; may be NULL */
} nx_mfrc522_if_spi_t;

/**
 * @brief I2C interface callbacks.
 *
 * write: initiate I2C write. Must block until the write is accepted (buffered or
 * DMA queued), though transmission may continue in the background. Returns true
 * on success, false on error.
 *
 * read: initiate I2C read from a register address. Must block until the read is
 * accepted, though the actual transfer may continue in the background. Returns
 * true on success, false on error.
 *
 * is_busy: optional; returns true while the previous operation is still in progress.
 * NULL means operations complete when write/read returns (blocking mode).
 * Non-NULL means the interface is non-blocking: caller must check is_busy() before
 * starting a new operation, and poll is_busy() after write/read returns.
 */
typedef struct {
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    bool (*read)(void *ctx, uint8_t reg_addr, uint8_t *data, size_t len);
    bool (*is_busy)(void *ctx);  /* Optional; may be NULL */
    uint8_t device_addr;
} nx_mfrc522_if_i2c_t;

/**
 * @brief UART interface callbacks.
 *
 * write: initiate UART write. Must block until the write is accepted (buffered or
 * DMA queued), though transmission may continue in the background. Returns true
 * on success, false on error.
 *
 * read: pull received bytes into the buffer. Returns the number of bytes actually
 * read (may be 0 if nothing available, or less than requested). Non-blocking.
 *
 * is_busy: optional; returns true while the previous write is still transmitting.
 * NULL means writes complete when write() returns (blocking mode).
 * Non-NULL means the interface is non-blocking: caller must check is_busy() before
 * starting a new write, and poll is_busy() after write() returns.
 */
typedef struct {
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    size_t (*read)(void *ctx, uint8_t *data, size_t max);
    bool (*is_busy)(void *ctx);  /* Optional; may be NULL */
} nx_mfrc522_if_uart_t;

/**
 * @brief MFRC522 configuration (hardware abstraction).
 *
 * All fields must be set by the caller before passing to nx_mfrc522_init.
 */
typedef struct {
    /**
     * @brief Interface type.
     */
    nx_mfrc522_if_type_t if_type;

    /**
     * @brief Interface-specific callbacks (union).
     */
    union {
        nx_mfrc522_if_spi_t  spi;   /**< SPI interface */
        nx_mfrc522_if_i2c_t  i2c;   /**< I2C interface */
        nx_mfrc522_if_uart_t uart;  /**< UART interface */
    } iface;

    /**
     * @brief Hardware reset (toggle RST pin low for >1us, then high).
     * @param ctx    User context.
     *
     * May be NULL if reset is handled externally or not connected.
     */
    void (*reset)(void *ctx);

    /**
     * @brief Read IRQ pin state (optional, for optimization).
     * @param ctx User context.
     * @return true if IRQ is asserted (active low: true = pin is low = event pending).
     *
     * May be NULL if IRQ pin is not connected. When provided, process() checks
     * this GPIO before reading interrupt registers via SPI/I2C/UART, avoiding
     * unnecessary bus transactions when no event has occurred. This is much faster
     * than polling registers (GPIO read is ~nanoseconds, SPI read is ~microseconds).
     *
     * MFRC522 IRQ pin is active-low and open-drain: it pulls low when any enabled
     * interrupt fires, stays low until the interrupt flag is cleared by software.
     * Typically connected with a pull-up resistor.
     */
    bool (*read_irq)(void *ctx);

    /**
     * @brief Get current time in microseconds.
     * @return Monotonic timestamp in microseconds.
     *
     * Used for timeout. Must not wrap within the longest operation timeout
     * (typically a few hundred milliseconds). Typically maps to HAL_GetTick(),
     * micros(), or xTaskGetTickCount().
     */
    uint32_t (*get_us)(void);

    /**
     * @brief User context passed to all callbacks.
     */
    void *io_ctx;
} nx_mfrc522_cfg_t;

/**
 * @brief MFRC522 driver handle.
 */
typedef struct {
    nx_mfrc522_cfg_t cfg;   /**< Configuration (copied at init) */

    struct {
        /* Transceive state machine */
        nx_mfrc522_state_t state;       /**< Current transceive state */
        nx_mfrc522_ret_t   result;      /**< Result code when state is DONE or ERROR */
        uint32_t           deadline_us; /**< Timeout deadline (us) */
        uint8_t           *rx_buf;      /**< Receive buffer (caller-owned) */
        size_t             rx_max;      /**< Max bytes rx_buf can hold */
        size_t             rx_len;      /**< Actual bytes received */

        /* Unified function pointers (set once at init, avoid repeated switch) */
        uint8_t (*read_reg_fn)(const void *m, uint8_t addr);
        void (*write_reg_fn)(const void *m, uint8_t addr, uint8_t val);
        bool (*is_busy_fn)(void *ctx);  /**< NULL if blocking mode */
    } run;
} nx_mfrc522_t;

/* ========================================================================
 * API
 * ======================================================================== */

/**
 * @brief Initialize the MFRC522 chip.
 *
 * Performs soft reset, configures RF settings, and turns on the antenna.
 * Returns false if the chip is not responding (wrong version or hardware fault).
 *
 * @param m    Handle to initialize.
 * @param cfg  Configuration (callbacks and context). Copied into the handle.
 * @return true on success, false if the chip is not responding.
 */
bool nx_mfrc522_init(nx_mfrc522_t *m, const nx_mfrc522_cfg_t *cfg);

/**
 * @brief Start a non-blocking transceive operation.
 *
 * Loads tx_data into the FIFO, starts the Transceive command, and arms the
 * state machine. Call nx_mfrc522_process() repeatedly until the state is no
 * longer BUSY. Then call nx_mfrc522_transceive_result() to retrieve received
 * data.
 *
 * @param m          Handle, must be in IDLE state.
 * @param tx_data    Data to transmit (copied into FIFO immediately).
 * @param tx_len     Number of bytes to transmit (max NX_MFRC522_FIFO_SIZE).
 * @param rx_buf     Buffer to receive response (caller-owned, must outlive the operation).
 * @param rx_max     Size of rx_buf.
 * @param timeout_us Operation timeout in microseconds.
 * @return NX_MFRC522_OK on successful start, NX_MFRC522_ERR_INVALID if
 *         parameters are invalid or state is not IDLE.
 */
nx_mfrc522_ret_t nx_mfrc522_transceive_start(nx_mfrc522_t *m,
                                               const uint8_t *tx_data,
                                               size_t         tx_len,
                                               uint8_t       *rx_buf,
                                               size_t         rx_max,
                                               uint32_t       timeout_us);

/**
 * @brief Advance the transceive state machine.
 *
 * Call this repeatedly in the main loop while the state is BUSY. It polls IRQ
 * flags and checks timeout. When finished, the state becomes DONE or ERROR.
 *
 * @param m Handle.
 */
void nx_mfrc522_process(nx_mfrc522_t *m);

/**
 * @brief Retrieve the result of a completed transceive operation.
 *
 * Call this after process() returns state DONE. The received data is already
 * in rx_buf (provided at start); this function only returns the length and
 * result code.
 *
 * @param m      Handle.
 * @param rx_len Pointer to receive the number of bytes received (may be NULL).
 * @return Result code (NX_MFRC522_OK, NX_MFRC522_ERR_TIMEOUT, etc.).
 *         Returns NX_MFRC522_ERR_INVALID if the operation is still in progress.
 */
nx_mfrc522_ret_t nx_mfrc522_transceive_result(const nx_mfrc522_t *m, size_t *rx_len);

/**
 * @brief Get the current transceive state.
 *
 * @param m Handle.
 * @return Current state (IDLE, TX_BUSY, RX_BUSY, DONE, ERROR).
 */
nx_mfrc522_state_t nx_mfrc522_get_state(const nx_mfrc522_t *m);

/**
 * @brief Read the chip version register.
 *
 * Expected value for MFRC522 is 0x92 or 0x91 (version 2.0 or 1.0).
 *
 * @param m Handle.
 * @return Version register value.
 */
uint8_t nx_mfrc522_get_version(const nx_mfrc522_t *m);

/**
 * @brief Calculate CRC using the chip's coprocessor.
 *
 * Blocking operation (polls until CRC is ready or timeout). Typically completes
 * in a few microseconds.
 *
 * @param m        Handle.
 * @param data     Data to calculate CRC over.
 * @param len      Number of bytes.
 * @param crc_out  Pointer to receive the 16-bit CRC (little-endian: [LSB, MSB]).
 * @return NX_MFRC522_OK on success, NX_MFRC522_ERR_TIMEOUT if CRC did not complete.
 */
nx_mfrc522_ret_t nx_mfrc522_calc_crc(nx_mfrc522_t *m,
                                      const uint8_t *data,
                                      size_t         len,
                                      uint16_t      *crc_out);

/**
 * @brief Turn the antenna on.
 *
 * @param m Handle.
 */
void nx_mfrc522_antenna_on(nx_mfrc522_t *m);

/**
 * @brief Turn the antenna off.
 *
 * @param m Handle.
 */
void nx_mfrc522_antenna_off(nx_mfrc522_t *m);

#ifdef __cplusplus
}
#endif

#endif /* NX_MFRC522_H */
