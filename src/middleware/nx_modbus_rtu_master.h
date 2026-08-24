/**
 * @file    nx_modbus_rtu_master.h
 * @brief   Event-driven Modbus RTU master: queue -> wire -> subscription dispatch, in pure C.
 *
 * This is the link/dispatch layer on top of nx_modbus_rtu.h (frame structs + CRC).
 * It owns no business logic. Its whole job is:
 *   1. transmit the request frames that business modules push onto a shared request
 *      queue, one at a time, honoring the inter-frame silence;
 *   2. pull bytes from the wire, slice out complete response frames and validate
 *      their CRC;
 *   3. route each valid response to whatever business module(s) subscribed to the
 *      slave address it came from, as a zero-copy reference-counted message
 *      (nx_ref_msg) - one response can fan out to several subscribers.
 *
 * Subscription model: a business module owns the devices it talks to. It declares the
 * slave addresses it owns in the subscription table and receives their responses as
 * data on its own queue, optionally narrowed to a single function code.
 *
 * Request building: the @c nx_modbus_rtu_master_request_* helpers build a well-formed
 * request frame, stamp its CRC and queue it for sending. They take only the pool and
 * the request queue, so a business module needs no handle on the master.
 *
 * Timeouts belong to the business module. This module transmits and dispatches; it
 * keeps no record of which requests are outstanding. A module that needs to know a
 * response never came notes when it sent, and decides for itself when to retry or
 * give up: an empty response queue means the answer has not arrived, not that it
 * failed.
 *
 * Framing model: frames are sliced by length. The length of every response is known
 * from its own bytes (5 for an exception, 8 for a write confirmation, or
 * 3 + byte_count + 2 for a read), so RX needs no inter-character timer - which
 * matters on a busy bus where arrival timing cannot be trusted. Resynchronization
 * after an unclaimed address or a bad CRC is done by dropping one byte and retrying.
 * On TX, a 3.5-character silence (derived from @c baud_rate) is inserted as a gap
 * after each transmitted frame.
 *
 * I/O is injected (this module touches no hardware): a non-blocking @c read pulls
 * received bytes, a non-blocking @c write starts a transmission, @c is_busy reports
 * whether the interface is still occupied (so a shared, non-exclusive bus is only
 * driven when free), and an optional @c dir_tx drives the RS-485 direction (DE) pin.
 * The serial callbacks (@c read / @c write / @c is_busy) share one @c io_ctx; the DE
 * pin, often a separate GPIO, gets its own @c dir_ctx. Timing is supplied by the
 * caller through the @c get_us callback, read on demand to time the TX inter-frame
 * gap; being a single system-wide time source, it takes no context.
 *
 * Memory: the RX framing buffer is caller-provided (@c rx_buf / @c rx_size); all
 * request/response messages are allocated from a caller-provided tiered pool via
 * nx_ref_msg. The module allocates nothing on its own.
 */
#ifndef NX_MODBUS_RTU_MASTER_H
#define NX_MODBUS_RTU_MASTER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_modbus_rtu.h"                 /* sibling: frame structs + CRC */
#include "src/core/nx_queue.h"
#include "src/core/nx_tiered_mem_pool.h"
#include "src/core/nx_ref_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a request builder; tells apart the reasons a request was not queued.
 *
 * Only @c NX_MODBUS_RTU_MASTER_ERR_NOMEM and @c ..._ERR_FULL report a runtime resource
 * shortage worth logging: the request is dropped and never reaches the wire, so the
 * business module sees no response and retries on its own schedule.
 * @c ..._ERR_PARAM is a caller bug.
 */
typedef enum {
    NX_MODBUS_RTU_MASTER_OK = 0,        /**< Request built and queued for sending */
    NX_MODBUS_RTU_MASTER_ERR_PARAM,     /**< Invalid argument (NULL pointer, bad quantity) */
    NX_MODBUS_RTU_MASTER_ERR_NOMEM,     /**< Pool exhausted: no message could be allocated */
    NX_MODBUS_RTU_MASTER_ERR_FULL       /**< Request queue is full */
} nx_modbus_rtu_master_ret_t;

/**
 * @brief One subscription: a business module's claim on a slave address.
 *
 * A received response matches this entry when its slave address equals @c slave_addr
 * and, if @c func is non-zero, its function code equals @c func. On a match, the
 * response frame is published (zero-copy) to @c queue. Several entries may match one
 * response - it is delivered to each of their queues.
 *
 * @c func is a filter, not a requirement: 0 (not a valid function code) accepts every
 * response from that address, which is what a module owning a whole device wants. Set
 * it when two modules share one device and split it by function code. An exception
 * response carries the original code OR'ed with 0x80, and matches a @c func filter for
 * the code it answers, so the module that asked also hears the refusal.
 *
 * A full @c queue drops that copy of the response. A response no subscription claims is
 * discarded: a master answers nothing, so there is no exception to emit and no reply to
 * withhold. Sizing each queue to the burst a module can fall behind by is what keeps a
 * claimed response from being dropped.
 */
typedef struct {
    uint8_t     slave_addr;  /**< Slave address owned, 1..247 */
    uint8_t     func;        /**< Function code filter; 0 = any code from that address */
    nx_queue_t *queue;       /**< Subscriber's response queue (an nx_ref_msg queue) */
} nx_modbus_rtu_master_sub_t;

/**
 * @brief Master configuration (copied into the instance by init).
 *
 * The subscription table, the RX buffer, the pool and the request queue are all
 * caller-owned and must outlive the master.
 */
typedef struct {
    uint32_t  baud_rate;    /**< Line baud; used to derive the TX inter-frame gap
                                 (3.5 chars). 0 = no inter-frame gap is enforced. */

    nx_tiered_mem_pool_t *pool;      /**< Pool for response ref-messages */

    uint8_t  *rx_buf;       /**< Caller-provided RX framing buffer */
    size_t    rx_size;      /**< Size of @c rx_buf; must be >= sizeof(nx_modbus_rtu_rsp_exc_t) + 1 */

    const nx_modbus_rtu_master_sub_t *subs;  /**< Subscription table (caller-owned) */
    size_t                            subs_count;

    nx_queue_t *request_queue;    /**< Shared request queue (an nx_ref_msg queue): the
                                 master transmits whatever is pushed here, in order. */

    /* ---- injected, non-blocking I/O ---- */

    /** Pull up to @p max received bytes into @p dst; return the count copied. */
    size_t (*read)(void *ctx, uint8_t *dst, size_t max);
    /** Start transmitting @p len bytes from @p src (non-blocking). Return true if the
     *  bytes were accepted for sending. A false return drops that request frame and
     *  releases the direction pin right away, rather than waiting out a transmission
     *  that never started; the business module sees no response and retries. */
    bool   (*write)(void *ctx, const uint8_t *src, size_t len);
    /** Return true while the interface is busy transmitting: from the moment a
     *  @c write starts until the frame has fully left the wire. Gates both the end
     *  of a transmission and, for a shared/non-exclusive bus, the start of the next
     *  one (nothing is written while busy). May be NULL, in which case @c write is
     *  treated as blocking and completing immediately. */
    bool   (*is_busy)(void *ctx);
    /** Drive the RS-485 direction pin: true = transmit, false = receive. May be NULL. */
    void   (*dir_tx)(void *ctx, bool enable);
    /** Return a free-running microsecond counter (wrap-around safe). Used only to
     *  time the TX inter-frame gap; if NULL, no gap is enforced. A single system-wide
     *  time source, so it takes no context. */
    uint32_t (*get_us)(void);

    void   *io_ctx;         /**< Context for the serial callbacks (read / write / is_busy) */
    void   *dir_ctx;        /**< Context for @c dir_tx (the DE pin, often a separate GPIO) */
} nx_modbus_rtu_master_cfg_t;

/**
 * @brief Master instance.
 *
 * @note  @c run is internal state; do not access it. Treat the whole object as
 *        opaque once passed to nx_modbus_rtu_master_init.
 */
typedef struct {
    nx_modbus_rtu_master_cfg_t cfg;   /**< Copied configuration */
    struct {
        size_t        rx_len;        /**< Valid bytes currently held in rx_buf */
        uint32_t      gap_us;        /**< Resolved TX inter-frame gap */
        uint8_t       tx_state;      /**< TX state machine (internal enum) */
        uint32_t      gap_start_us;  /**< get_us() timestamp when the last frame finished */
        nx_ref_msg_t *tx_cur;        /**< Message currently being transmitted */
    } run;
} nx_modbus_rtu_master_t;

/**
 * @brief  Initialize a master instance from a configuration.
 *
 * Validates the configuration and copies it in. The RX buffer must be able to hold at
 * least the shortest response plus one byte of slack for overflow detection
 * (>= sizeof(nx_modbus_rtu_rsp_exc_t) + 1).
 *
 * Every subscription must carry a @c queue: one without it would own a slave address
 * and then swallow every response from it, so it is rejected here rather than at
 * runtime. Subscribed addresses must be unicast (1..247); no response ever carries the
 * broadcast address, so a subscription to it could never match.
 *
 * To re-initialize an instance that has already been running, call
 * nx_modbus_rtu_master_deinit() first: a frame caught mid-transmit holds a pool block
 * that only deinit can give back.
 *
 * @param  m   Instance to initialize, must not be NULL. Its previous contents are
 *             never read, so it need not be zero-initialized.
 * @param  cfg Configuration, must not be NULL; @c pool, @c rx_buf, @c request_queue,
 *             @c read and @c write are required.
 *
 * @return true on success; false on any invalid argument (NULL required field,
 *         rx_size too small, subs NULL with subs_count > 0, a subscription with a
 *         NULL queue, or a subscribed address outside 1..247).
 */
bool nx_modbus_rtu_master_init(nx_modbus_rtu_master_t *m, const nx_modbus_rtu_master_cfg_t *cfg);

/**
 * @brief  Release what the instance still holds and park it idle.
 *
 * Gives back the pool block of a frame that was mid-transmit, deasserts the direction
 * pin, and drops any partially received response. The request queue is left alone: the
 * messages still in it belong to whoever pushed them.
 *
 * Call this before re-initializing a running instance, and when taking it out of
 * service. Safe to call on an instance that is already idle; not safe to call on one
 * that was never initialized, since there is nothing to release.
 *
 * @param  m Instance; NULL is ignored.
 */
void nx_modbus_rtu_master_deinit(nx_modbus_rtu_master_t *m);

/**
 * @brief  Drive the master once; call this periodically from the main loop.
 *
 * Each call: pulls available RX bytes and dispatches every complete valid response to
 * its subscribers, then advances the TX state machine, transmitting at most what the
 * flow permits and honoring the inter-frame gap. Time, when needed for the gap, is read
 * from the @c get_us callback.
 *
 * @param  m Instance, must not be NULL.
 */
void nx_modbus_rtu_master_process(nx_modbus_rtu_master_t *m);

/* ------------------------------------------------------------------ */
/* Request builders                                                   */
/* ------------------------------------------------------------------ */
/* Each builder validates the frame-internal rules the Modbus spec fixes for the
 * function code (the quantity range, and for the multi-writes the byte count implied
 * by it), fills the frame, stamps its CRC and queues it for sending. They take only
 * the pool and the request queue, so a business module needs no handle on the master.
 *
 * A quantity outside its protocol range is refused with ERR_PARAM rather than sent:
 * the frame would be well-formed on the wire and rejected by the slave, costing a
 * round trip to learn what is knowable here.
 *
 * Broadcast (address 0) is accepted by the write builders and refused by the read
 * builders: a broadcast write is answered by nobody by design, while a broadcast read
 * would ask every slave on the bus to answer at once. */

/**
 * @brief  Build a "read coils" request (0x01) and queue it for sending.
 *
 * @param  pool          Pool the request is allocated from, must not be NULL.
 * @param  request_queue Queue the master transmits from, must not be NULL.
 * @param  slave_addr    Target slave, 1..247 (broadcast is not valid for a read).
 * @param  start_addr    Address of the first coil to read.
 * @param  qty           Number of coils, 1..2000.
 *
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued; otherwise the reason it
 *         was not (invalid argument, pool exhausted, queue full), in which case
 *         nothing reaches the wire and no response will arrive.
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_coils(nx_tiered_mem_pool_t *pool,
                                                           nx_queue_t           *request_queue,
                                                           uint8_t               slave_addr,
                                                           uint16_t              start_addr,
                                                           uint16_t              qty);

/**
 * @brief  Build a "read discrete inputs" request (0x02) and queue it for sending.
 *
 * Arguments and return values are as nx_modbus_rtu_master_read_coils; @p qty is
 * 1..2000 discrete inputs.
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_discrete_inputs(nx_tiered_mem_pool_t *pool,
                                                                     nx_queue_t           *request_queue,
                                                                     uint8_t               slave_addr,
                                                                     uint16_t              start_addr,
                                                                     uint16_t              qty);

/**
 * @brief  Build a "read holding registers" request (0x03) and queue it for sending.
 *
 * Arguments and return values are as nx_modbus_rtu_master_read_coils; @p qty is
 * 1..125 registers.
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_holding_regs(nx_tiered_mem_pool_t *pool,
                                                                  nx_queue_t           *request_queue,
                                                                  uint8_t               slave_addr,
                                                                  uint16_t              start_addr,
                                                                  uint16_t              qty);

/**
 * @brief  Build a "read input registers" request (0x04) and queue it for sending.
 *
 * Arguments and return values are as nx_modbus_rtu_master_read_coils; @p qty is
 * 1..125 registers.
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_input_regs(nx_tiered_mem_pool_t *pool,
                                                                nx_queue_t           *request_queue,
                                                                uint8_t               slave_addr,
                                                                uint16_t              start_addr,
                                                                uint16_t              qty);

/**
 * @brief  Build a "write single coil" request (0x05) and queue it for sending.
 *
 * @param  pool          Pool the request is allocated from, must not be NULL.
 * @param  request_queue Queue the master transmits from, must not be NULL.
 * @param  slave_addr    Target slave, 0..247 (0 = broadcast, answered by nobody).
 * @param  coil_addr     Address of the coil to write.
 * @param  on            true drives the coil on (wire value 0xFF00), false off (0x0000).
 *
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued; otherwise the reason it
 *         was not (invalid argument, pool exhausted, queue full).
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_single_coil(nx_tiered_mem_pool_t *pool,
                                                                  nx_queue_t           *request_queue,
                                                                  uint8_t               slave_addr,
                                                                  uint16_t              coil_addr,
                                                                  bool                  on);

/**
 * @brief  Build a "write single register" request (0x06) and queue it for sending.
 *
 * @param  pool          Pool the request is allocated from, must not be NULL.
 * @param  request_queue Queue the master transmits from, must not be NULL.
 * @param  slave_addr    Target slave, 0..247 (0 = broadcast, answered by nobody).
 * @param  reg_addr      Address of the register to write.
 * @param  value         Value to store; any 16-bit value is legal.
 *
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued; otherwise the reason it
 *         was not (invalid argument, pool exhausted, queue full).
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_single_reg(nx_tiered_mem_pool_t *pool,
                                                                 nx_queue_t           *request_queue,
                                                                 uint8_t               slave_addr,
                                                                 uint16_t              reg_addr,
                                                                 uint16_t              value);

/**
 * @brief  Build a "write multiple coils" request (0x0F) and queue it for sending.
 *
 * @param  pool          Pool the request is allocated from, must not be NULL.
 * @param  request_queue Queue the master transmits from, must not be NULL.
 * @param  slave_addr    Target slave, 0..247 (0 = broadcast, answered by nobody).
 * @param  start_addr    Address of the first coil to write.
 * @param  qty           Number of coils, 1..1968.
 * @param  bits          Coil states packed one bit per coil, LSB of the first byte
 *                       being @p start_addr, must not be NULL.
 * @param  bits_len      Length of @p bits; must equal ceil(@p qty / 8).
 *
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued; otherwise the reason it
 *         was not (invalid argument, pool exhausted, queue full).
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_multiple_coils(nx_tiered_mem_pool_t *pool,
                                                                     nx_queue_t           *request_queue,
                                                                     uint8_t               slave_addr,
                                                                     uint16_t              start_addr,
                                                                     uint16_t              qty,
                                                                     const uint8_t        *bits,
                                                                     size_t                bits_len);

/**
 * @brief  Build a "write multiple registers" request (0x10) and queue it for sending.
 *
 * @param  pool          Pool the request is allocated from, must not be NULL.
 * @param  request_queue Queue the master transmits from, must not be NULL.
 * @param  slave_addr    Target slave, 0..247 (0 = broadcast, answered by nobody).
 * @param  start_addr    Address of the first register to write.
 * @param  qty           Number of registers, 1..123.
 * @param  regs          Register values in wire order (big-endian pairs), must not be NULL.
 * @param  regs_len      Length of @p regs in bytes; must equal @p qty * 2.
 *
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued; otherwise the reason it
 *         was not (invalid argument, pool exhausted, queue full).
 */
nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_multiple_regs(nx_tiered_mem_pool_t *pool,
                                                                    nx_queue_t           *request_queue,
                                                                    uint8_t               slave_addr,
                                                                    uint16_t              start_addr,
                                                                    uint16_t              qty,
                                                                    const uint8_t        *regs,
                                                                    size_t                regs_len);

/* ------------------------------------------------------------------ */
/* Response inspection                                                */
/* ------------------------------------------------------------------ */
/* What a subscriber pops from its queue is a whole response ADU, CRC already verified.
 * These two helpers save it from counting offsets: one tells apart a refusal from an
 * answer, the other finds the read data. Both are pure reads over the frame. */

/**
 * @brief  Report whether a response is an exception, and which one.
 *
 * A slave refuses a request by answering with the function code OR'ed with 0x80 and a
 * single exception code byte. Check this before reading any data out of a response.
 *
 * @param  frame Response frame, must not be NULL.
 * @param  flen  Frame length in bytes, as reported by nx_ref_msg_len().
 * @param  exc   May be NULL; if non-NULL and the frame is an exception, receives the
 *               exception code (one of nx_modbus_exc_t).
 *
 * @return true if @p frame is an exception response; false if it is a normal response,
 *         or if the arguments are invalid.
 */
bool nx_modbus_rtu_master_rsp_is_exception(const uint8_t *frame, size_t flen, uint8_t *exc);

/**
 * @brief  Locate the data carried by a read response (0x01/0x02/0x03/0x04).
 *
 * The read responses carry their payload behind a byte count. This returns a pointer
 * into @p frame - no copy is made, and the data is valid for as long as the message the
 * subscriber holds. Register values are two-byte fields, most-significant byte first.
 *
 * @param  frame Response frame, must not be NULL.
 * @param  flen  Frame length in bytes, as reported by nx_ref_msg_len().
 * @param  len   May be NULL; if non-NULL, receives the payload length in bytes.
 *
 * @return Pointer to the payload; NULL if @p frame is not a read response (an
 *         exception or a write confirmation carries no payload), if its byte count
 *         disagrees with @p flen, or if the arguments are invalid. When NULL is
 *         returned, @p len is set to 0.
 */
const uint8_t *nx_modbus_rtu_master_rsp_data(const uint8_t *frame, size_t flen, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* NX_MODBUS_RTU_MASTER_H */
