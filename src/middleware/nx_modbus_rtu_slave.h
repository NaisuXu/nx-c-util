/**
 * @file    nx_modbus_rtu_slave.h
 * @brief   Event-driven Modbus RTU slave: frame -> subscription dispatch, in pure C.
 *
 * This is the link/dispatch layer on top of nx_modbus_rtu.h (frame structs + CRC).
 * It owns no business logic. Its whole job is:
 *   1. pull bytes from the wire, slice out complete RTU frames, validate CRC and
 *      the slave address (unicast always, broadcast only if @c accept_broadcast);
 *   2. route each valid request to whatever business module(s) subscribed to it,
 *      by (function code + address range), as a zero-copy reference-counted message
 *      (nx_ref_msg) - one request can fan out to several subscribers;
 *   3. transmit responses that business modules push back onto a shared response
 *      queue.
 *
 * Subscription model: a single function code (e.g. read holding registers) may be
 * owned by several independent business modules over different address ranges. Each
 * module declares the ranges it owns in the subscription table and receives matching
 * requests as data on its own queue.
 *
 * Framing model: frames are sliced by length. The length of every supported frame
 * is known from its function code (fixed 8 bytes for 01..06, or 9 + byte_count for
 * 0F/10), so RX needs no inter-character timer - which matters on a busy bus where
 * arrival timing cannot be trusted. Resynchronization after a bad address or CRC is
 * done by dropping one byte and retrying. On TX, a 3.5-character silence (derived
 * from @c baud_rate) is inserted as a gap after each transmitted frame.
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
#ifndef NX_MODBUS_RTU_SLAVE_H
#define NX_MODBUS_RTU_SLAVE_H

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
 * @brief One subscription: a business module's claim on a function code + range.
 *
 * A received request matches this entry when its function code equals @c func and
 * the whole register span it touches lies inside [@c addr_min, @c addr_max]
 * (inclusive). On a match, the request frame is published (zero-copy) to @c queue.
 * Several entries may match one request - it is delivered to each of their queues.
 *
 * The span a request touches:
 *   - 01/02/03/04, 0F/10: [start_addr, start_addr + quantity - 1];
 *   - 05/06 (single write): just the single data address.
 * The slave settles structural legality before dispatch - function support (0x01),
 * quantity range and byte_count consistency (0x03), and address containment (0x02).
 * A matched request is therefore well-formed; whether a value is operationally
 * acceptable for a given register is the owning business module's call, which may
 * emit its own exception response.
 */
typedef struct {
    uint8_t     func;      /**< Function code owned, e.g. NX_MODBUS_FC_READ_HOLDING_REGS */
    uint16_t    addr_min;  /**< Inclusive low bound of the owned address range */
    uint16_t    addr_max;  /**< Inclusive high bound of the owned address range */
    nx_queue_t *queue;     /**< Subscriber's request queue (an nx_ref_msg queue) */
} nx_modbus_rtu_slave_sub_t;

/**
 * @brief Slave configuration (copied into the instance by init).
 *
 * The subscription table, the RX buffer, the pool and the response queue are all
 * caller-owned and must outlive the slave.
 */
typedef struct {
    uint8_t   slave_addr;   /**< This slave's unicast address, 1..247 */

    uint32_t  baud_rate;    /**< Line baud; used to derive the TX inter-frame gap
                                 (3.5 chars). 0 = no inter-frame gap is enforced. */

    bool      accept_broadcast;  /**< true = also accept frames addressed to the
                                 broadcast address (0); such a request is dispatched
                                 to its subscribers but never answered. false
                                 (default) drops them, handling only unicast. */

    nx_tiered_mem_pool_t *pool;      /**< Pool for request/response ref-messages */

    uint8_t  *rx_buf;       /**< Caller-provided RX framing buffer */
    size_t    rx_size;      /**< Size of @c rx_buf; must be >= sizeof(nx_modbus_rtu_req_fix_t) + 1 */

    const nx_modbus_rtu_slave_sub_t *subs;  /**< Subscription table (caller-owned) */
    size_t                           subs_count;

    nx_queue_t *response_queue;   /**< Shared response queue (an nx_ref_msg queue):
                                 the slave transmits whatever is pushed here, and also
                                 pushes its own exception responses here. */

    /* ---- injected, non-blocking I/O ---- */

    /** Pull up to @p max received bytes into @p dst; return the count copied. */
    size_t (*read)(void *ctx, uint8_t *dst, size_t max);
    /** Start transmitting @p len bytes from @p src (non-blocking). */
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
} nx_modbus_rtu_slave_cfg_t;

/**
 * @brief Slave instance.
 *
 * @note  @c run is internal state; do not access it. Treat the whole object as
 *        opaque once passed to nx_modbus_rtu_slave_init.
 */
typedef struct {
    nx_modbus_rtu_slave_cfg_t cfg;   /**< Copied configuration */
    struct {
        size_t        rx_len;        /**< Valid bytes currently held in rx_buf */
        uint32_t      gap_us;        /**< Resolved TX inter-frame gap */
        uint8_t       tx_state;      /**< TX state machine (internal enum) */
        uint32_t      gap_start_us;  /**< get_us() timestamp when the last frame finished */
        nx_ref_msg_t *tx_cur;        /**< Message currently being transmitted */
    } run;
} nx_modbus_rtu_slave_t;

/**
 * @brief  Initialize a slave instance from a configuration.
 *
 * Validates the configuration and copies it in. The RX buffer must be able to hold
 * at least the shortest request plus one byte of slack for overflow detection
 * (>= sizeof(nx_modbus_rtu_req_fix_t) + 1).
 *
 * @param  s   Instance to initialize, must not be NULL.
 * @param  cfg Configuration, must not be NULL; @c pool, @c rx_buf, @c response_queue,
 *             @c read and @c write are required.
 *
 * @return true on success; false on any invalid argument (NULL required field,
 *         address out of 1..247, rx_size too small, subs NULL with subs_count > 0).
 */
bool nx_modbus_rtu_slave_init(nx_modbus_rtu_slave_t *s, const nx_modbus_rtu_slave_cfg_t *cfg);

/**
 * @brief  Drive the slave once; call this periodically from the main loop.
 *
 * Each call: pulls available RX bytes and dispatches every complete valid frame to
 * its subscribers (or emits an exception), then advances the TX state machine,
 * transmitting at most the flow permits and honoring the inter-frame gap. Time, when
 * needed for the gap, is read from the @c get_us callback.
 *
 * @param  s Instance, must not be NULL.
 */
void nx_modbus_rtu_slave_process(nx_modbus_rtu_slave_t *s);

/**
 * @brief  Build an exception response for a received request and queue it for sending.
 *
 * For subscriber use: a business module that finds a request unacceptable answers it
 * with this. The reply's address and function code are taken from @p request.
 *
 * @param  pool           Pool the response is allocated from, must not be NULL.
 * @param  response_queue Queue the slave transmits from, must not be NULL.
 * @param  request        Header of the request being answered, must not be NULL. Only
 *                        @c addr and @c cmd are read, so any request frame can be cast
 *                        to this type.
 * @param  exception_code Exception code to report; one of nx_modbus_exc_t.
 *
 * @return true if the response was queued; false if nothing was queued (broadcast
 *         request, NULL argument, pool exhausted, or queue full), in which case the
 *         master times out. A caller has no repair to make in any of these cases.
 */
bool nx_modbus_rtu_slave_reply_exception(nx_tiered_mem_pool_t         *pool,
                                         nx_queue_t                   *response_queue,
                                         const nx_modbus_rtu_header_t *request,
                                         uint8_t                       exception_code);

#ifdef __cplusplus
}
#endif

#endif /* NX_MODBUS_RTU_SLAVE_H */
