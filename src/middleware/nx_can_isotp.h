/**
 * @file    nx_can_isotp.h
 * @brief   ISO 15765-2 (DoCAN / ISO-TP) transport-layer module, in pure C.
 *
 * A queue-to-queue ISO-TP layer: it takes CAN frames from the application's CAN
 * driver queue (via nx_ref_msg wrapping nx_can_msg_t), reassembles complete
 * messages and hands them to an upper layer, takes the upper layer's messages
 * and segments them back into CAN frames. The module owns no bus and no hardware:
 * every incoming and outgoing artifact is an nx_ref_msg exchanged over
 * caller-provided queues. What the driver does with the frames it dequeues is the
 * driver's business.
 *
 * Addressing is "Normal" style, expressed with concrete CAN IDs rather than bit
 * fields. A conversation needs both directions, so the physical pair is
 * configured together: @c phys_rx_id is what this instance accepts and
 * @c phys_tx_id is what it answers under - including the flow control it must
 * send while receiving a segmented message. An optional @c func_rx_id adds
 * functional (1:N) reception, which is single-frame only, as a shared request ID
 * cannot carry per-receiver flow control; responses to it still go out on
 * @c phys_tx_id. An optional @c func_tx_id adds functional transmission, also
 * single-frame only and off by default, since a network has at most one
 * functional sender. The module never touches the SA/TA layout inside an ID,
 * which keeps one instance valid for any physical assignment (UDS's
 * 0x18DA..xx, a vendor scheme, even an 11-bit ID).
 *
 * One instance = one CAN channel. Multiple physical channels each get their own
 * instance. Reception carries one segmented conversation at a time (the physical
 * one); functional single frames are complete on arrival and are delivered
 * alongside it without disturbing it. Transmission is single as well - one bus,
 * frames leave in order - so send requests are queued and driven one at a time.
 *
 * The module is both ends of a transport at once - there is no client/server
 * split in ISO-TP. Configure the ID pair and let the upper layer answer, and it
 * behaves as a server (ECU under diagnostics); call nx_can_isotp_send() with a
 * request, and it behaves as a client (a tester issuing UDS requests). The same
 * instance does both.
 *
 * Timing is supplied by the caller through the @c get_us callback, read on
 * demand inside process(). The network layer timeouts (N_As, N_Ar, N_Bs, N_Cr),
 * the flow control this instance advertises while receiving (@c rx_block_size,
 * @c rx_stmin) and the separation this instance keeps while transmitting are all
 * configurable, and a zero-initialized cfg lands on documented defaults.
 *
 * A frame is handed to the link by publishing it to @c can_tx_queue, so a full
 * queue is what "the link would not take it" means here. Such a frame is offered
 * again on later process() calls for as long as @c n_as_us (transmitting) or
 * @c n_ar_us (receiving) allows, since a transmit queue that momentarily filled
 * up normally drains within milliseconds; only past that does the conversation
 * end with a timeout. The instant at which a frame truly leaves the bus is the
 * driver's to observe, so these two timeouts are measured from the moment the
 * frame was offered to the queue, which runs a little ahead of the transmission
 * itself and therefore expires a little late rather than early.
 *
 * Reception is bounded by @c rx_max_len: a first frame announcing more than that
 * is refused with an overflow flow control frame before anything is allocated, so
 * what the instance accepts is a property of its configuration rather than of how
 * full the pool happens to be. A first frame announcing more than
 * @c NX_CAN_ISOTP_MAX_MSG_LEN is refused the same way whatever @c rx_max_len says,
 * so a length taken off the wire can never exceed what one allocation expresses.
 *
 * Memory: everything is allocated from a caller-provided tiered pool via
 * nx_ref_msg - reassembled messages handed up, and CAN frames pushed down. A
 * segmented reception allocates one message-sized block from the pool at first
 * frame time and reassembles straight into it, so a finished message is handed up
 * without a copy. The module allocates nothing on its own.
 *
 * All storage is caller-provided and caller-owned; nothing is freed or allocated
 * by the module except through the pool.
 */
#ifndef NX_CAN_ISOTP_H
#define NX_CAN_ISOTP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "src/core/nx_queue.h"
#include "src/core/nx_tiered_mem_pool.h"
#include "src/core/nx_ref_msg.h"
#include "nx_can_bus.h"                 /* sibling: nx_can_msg_t frame type */
#include "nx_tp_sdu.h"                  /* sibling: nx_tp_sdu_t message type */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Payload bytes per frame, one value per expressible frame size.
 *
 * 8 bytes is the only size a classic CAN frame carries. The larger sizes are
 * the ones a data length code expresses, so a frame of that size is exactly
 * full; a size between two of them cannot be put on the wire.
 */
#define NX_CAN_ISOTP_FRAME_8  8u
#define NX_CAN_ISOTP_FRAME_12 12u
#define NX_CAN_ISOTP_FRAME_16 16u
#define NX_CAN_ISOTP_FRAME_20 20u
#define NX_CAN_ISOTP_FRAME_24 24u
#define NX_CAN_ISOTP_FRAME_32 32u
#define NX_CAN_ISOTP_FRAME_48 48u
#define NX_CAN_ISOTP_FRAME_64 64u

/**
 * @brief Return codes for module operations.
 */
typedef enum {
    NX_CAN_ISOTP_OK = 0,      /**< Operation succeeded */
    NX_CAN_ISOTP_ERR_PARAM,   /**< Invalid argument (NULL pointer, bad config, bad length) */
    NX_CAN_ISOTP_ERR_NOMEM,   /**< Pool exhausted: a frame or message could not be allocated */
    NX_CAN_ISOTP_ERR_FULL,    /**< A target queue is full (message or frame could not be enqueued) */
    NX_CAN_ISOTP_ERR_STATE,   /**< Wrong state to accept the operation */
    NX_CAN_ISOTP_ERR_LENGTH   /**< A message length is unsupported */
} nx_can_isotp_ret_t;

/**
 * @brief The module's unit of exchange with the upper layer.
 *
 * A received message reports how it was addressed in @c ta_type, so the upper
 * layer can tell a physically addressed request from a functionally addressed
 * one and answer accordingly, and carries the instance's configured @c link so
 * one upper layer can serve several instances from one queue. A message being
 * sent needs neither: it goes out on the configured @c phys_tx_id.
 */
typedef nx_tp_sdu_t nx_can_isotp_sdu_t;

/**
 * @brief  What one message costs in the pool besides its payload, in bytes.
 *
 * A message is a single pooled block holding the reference-message header, the
 * SDU header and the payload, so a payload of @c n bytes occupies
 * @c NX_CAN_ISOTP_MSG_OVERHEAD + @c n.
 */
#define NX_CAN_ISOTP_MSG_OVERHEAD     (sizeof(nx_ref_msg_t) + sizeof(nx_can_isotp_sdu_t))

/**
 * @brief  Longest message this module handles, in bytes.
 *
 * A length is expressed in 32 bits on the wire and the message must also fit one
 * allocation, so the ceiling is whichever of those two limits is lower, less the
 * per-message overhead. It works out just under 4 GiB wherever @c size_t is at
 * least 32 bits wide; what a given instance can actually accept is smaller still,
 * being whatever its pool and @c rx_max_len allow.
 *
 * nx_can_isotp_send() refuses a longer request with @c NX_CAN_ISOTP_ERR_LENGTH,
 * and a first frame announcing more is refused with an overflow flow control
 * frame before anything is allocated.
 */
#define NX_CAN_ISOTP_MAX_MSG_LEN     ((SIZE_MAX < 0xFFFFFFFFu ? (size_t)SIZE_MAX : (size_t)0xFFFFFFFFu) - NX_CAN_ISOTP_MSG_OVERHEAD)

/**
 * @brief Reassembly state of the segmented (physical) reception.
 *
 * @note  Internal state; do not access directly.
 */
typedef struct {
    uint8_t       state;     /**< Reception state machine position */
    uint8_t       sn;        /**< Next consecutive-frame sequence number expected */
    uint8_t       block_left;/**< Frames left before the next flow control is due */
    uint32_t      total;     /**< Payload length announced by the first frame */
    uint32_t      filled;    /**< Payload bytes collected so far */
    nx_ref_msg_t *acc;       /**< Message being reassembled; NULL when idle */
    uint32_t      deadline;  /**< get_us() by which whatever the state is waiting
                              for - the peer's next frame, or room on the link for
                              a flow control frame - must have happened */
} nx_can_isotp_rx_t;

/**
 * @brief State of the single in-flight transmission.
 *
 * @note  Internal state; do not access directly.
 */
typedef struct {
    uint8_t       state;     /**< Transmit state machine position */
    uint8_t       sn;        /**< Next consecutive-frame sequence number to send */
    uint8_t       bs;        /**< Frames left in the current flow-control block */
    uint8_t       wft;       /**< Consecutive FC.WAIT frames seen so far */
    uint8_t       ta_type;   /**< Addressing of the message being sent */
    uint32_t      sent;      /**< Payload bytes already placed into frames */
    uint32_t      total;     /**< Payload length of the message being sent */
    int32_t       stmin_us;  /**< Resolved minimum frame spacing (us); 0 = none */
    uint32_t      last_us;   /**< get_us() when the last frame was emitted */
    uint32_t      deadline;  /**< get_us() by which whatever the state is waiting
                              for - the peer's flow control, or room on the link
                              for a frame - must have happened */
    uint32_t      tx_id;     /**< CAN ID the message is emitted under */
    nx_ref_msg_t *sdu;       /**< Message being sent; NULL when idle */
} nx_can_isotp_tx_t;

/**
 * @brief  Configuration (copied into the instance by init).
 *
 * All buffers and queues are caller-owned and must outlive the instance. Every
 * timing field treats 0 as "use the documented default", so a zero-initialized
 * cfg needs only the addressing and the queues filled in.
 */
typedef struct {
    /* ---- frame format and geometry ---- */
    uint8_t  max_frame_len;   /**< Payload bytes per CAN frame. Must be one of
                               8, 12, 16, 20, 24, 32, 48 or 64. Anything above 8
                               requires @c fd_frames. */
    bool     ext_id;          /**< true: this instance speaks in 29-bit identifiers.
                               Every frame it emits carries one, and a received
                               frame is considered only when its identifier is of
                               the same width, so the two identifier spaces stay
                               separate. */
    bool     fd_frames;       /**< true: emit CAN FD frames. Required once
                               @c max_frame_len exceeds 8, and also usable at 8,
                               which puts a classic-sized payload in an FD frame. */
    bool     brs;             /**< true: request the bit-rate switch on emitted
                               frames, so their data phase runs at the faster
                               rate. Requires @c fd_frames. */
    bool     pad_frames;      /**< true: raise every emitted frame to 8 data bytes,
                               filling the tail with @c pad_byte. false: emit the
                               exact length the frame carries. Frames longer than 8
                               bytes are filled to the next expressible length
                               either way, since no data length code sits between
                               them. */
    uint8_t  pad_byte;        /**< Filler written into an emitted frame's unused
                               tail. */

    /* ---- addressing: the channel, the physical pair, plus optional
     *      functional reception ---- */
    uint8_t  ch;              /**< CAN channel this instance serves, 0..15. Stamped
                               into every emitted frame, so a driver spanning
                               several buses reads off it which one to transmit on,
                               and matched on every received frame. Left 0 it names
                               channel 0, which is what a single-bus driver that
                               never fills the field reports. */
    uint32_t phys_rx_id;      /**< CAN ID this instance receives physically addressed
                               messages on. Required. */
    uint32_t phys_tx_id;      /**< CAN ID this instance transmits under, and sends flow
                               control on while receiving. Required, and must differ
                               from @c phys_rx_id. */
    uint32_t func_rx_id;      /**< CAN ID for functionally addressed (1:N) reception,
                               single-frame only; 0 disables it. Responses still go
                               out on @c phys_tx_id. */
    uint32_t func_tx_id;      /**< CAN ID for functionally addressed (1:N) transmission,
                               single-frame only; 0 disables it. There is at most
                               one functional sender on a network, so only a tester
                               instance configures this. */

    /* ---- upper-layer interface (queues) ---- */
    nx_tiered_mem_pool_t *pool;     /**< Pool for frames and SDUs */
    /* Every queue is named from this module's point of view: rx is what the
     * module reads, tx is what it writes. */
    nx_queue_t *sdu_rx_queue;       /**< upper -> module: send requests as nx_ref_msg(SDU) */
    nx_queue_t *sdu_tx_queue;       /**< module -> upper: received messages and send
                                     confirmations as nx_ref_msg(SDU) */
    nx_queue_t *can_rx_queue;       /**< CAN -> module: received frames as nx_ref_msg(nx_can_msg_t) */
    nx_queue_t *can_tx_queue;       /**< module -> CAN: segmented frames as nx_ref_msg(nx_can_msg_t) */

    /* ---- identification and reporting ---- */
    uint8_t  link;                  /**< Connection number copied into every SDU this
                                     instance publishes, letting one upper layer serve
                                     several instances from one queue. */
    bool     confirm_tx;            /**< true: publish a confirmation SDU when a
                                     transmission ends, carrying its outcome. false:
                                     transmissions end silently. */

    /* ---- timing ---- */
    uint32_t (*get_us)(void);       /**< Free-running microsecond counter (wrap-safe) */
    uint32_t n_as_us;               /**< Time a frame may spend waiting to be handed to
                                     the link while transmitting, in us. 0 = 1000 ms. */
    uint32_t n_ar_us;               /**< Time a flow control frame may spend waiting to be
                                     handed to the link while receiving, in us.
                                     0 = 1000 ms. */
    uint32_t n_bs_us;               /**< Timeout awaiting a peer's flow control while
                                     transmitting, in us. 0 = 1000 ms. */
    uint32_t n_cr_us;               /**< Timeout awaiting a peer's consecutive frame while
                                     receiving, in us. 0 = 1000 ms. */
    uint8_t  n_wft_max;             /**< Consecutive FC.WAIT frames tolerated before the
                                     transmission is abandoned. 0 = 4. */

    /* ---- flow control this instance advertises while receiving ---- */
    uint32_t rx_max_len;            /**< Longest message this instance accepts, in
                                     bytes. A first frame announcing more is refused
                                     with an overflow flow control frame and nothing
                                     is allocated for it. 0 = accept whatever the
                                     pool can hold, still capped by
                                     NX_CAN_ISOTP_MAX_MSG_LEN. */
    uint8_t  rx_block_size;         /**< Consecutive frames the peer may send between two
                                     flow control frames. 0 = the whole message at once. */
    uint8_t  rx_stmin;              /**< Minimum separation the peer must keep, encoded as
                                     on the wire: 0x00 = none, 0x01..0x7F = that many ms,
                                     0xF1..0xF9 = 100..900 us. */

    /* ---- pacing ---- */
    uint8_t  tx_frames_per_process; /**< Max frames to transmit in one process() call.
                                     0 = no limit (drive all flow control allows). */
} nx_can_isotp_cfg_t;

/**
 * @brief Instance of the ISO-TP transporter (one island per instance).
 *
 * Declare one in static storage and hand it to nx_can_isotp_init.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed
 *        to nx_can_isotp_init.
 */
typedef struct nx_can_isotp {
    nx_can_isotp_cfg_t cfg;                  /**< Copied configuration */
    struct {
        nx_can_isotp_rx_t rx;                /**< Segmented reception */
        nx_can_isotp_tx_t tx;                /**< Single transmit conversation */
    } run;                                   /**< Internal runtime state */
} nx_can_isotp_t;

/**
 * @brief  Initialize an instance from a configuration.
 *
 * @param  iso Instance to initialize, must not be NULL.
 * @param  cfg Configuration, must not be NULL; @c max_frame_len in
 *             {8,12,16,20,24,32,48,64}, with @c fd_frames set for anything
 *             above 8; @c brs only with @c fd_frames; @c ch at most
 *             @c NX_CAN_MAX_CH;
 *             @c phys_rx_id and @c phys_tx_id required and distinct;
 *             @c func_rx_id, when non-zero, distinct from both;
 *             @c func_tx_id, when non-zero, distinct from all three others;
 *             @c pool, @c sdu_rx_queue, @c sdu_tx_queue, @c can_rx_queue and
 *             @c can_tx_queue required.
 *
 * @return true on success; false on any invalid argument.
 */
bool nx_can_isotp_init(nx_can_isotp_t *iso, const nx_can_isotp_cfg_t *cfg);

/**
 * @brief  Drive the transporter once; call periodically from the main loop.
 *
 * Each call drains @c can_rx_queue and advances reception with the frames found
 * there, emitting flow control as reassembly requires; expires any conversation
 * whose peer has gone quiet past its timeout, or whose own frame could not be
 * handed to the link within @c n_as_us or @c n_ar_us; then pulls from
 * @c sdu_rx_queue and emits up to @c tx_frames_per_process frames of the current
 * transmission, respecting the peer's block size and separation time.
 *
 * A frame @c can_tx_queue refused is offered again on the next call, so a
 * transmit queue that fills up briefly costs the conversation nothing but the
 * time it takes to drain.
 *
 * Frames whose ID matches none of the configured receive IDs are released and
 * otherwise ignored, so several instances may share one @c can_rx_queue only if
 * their ID sets are disjoint - each frame is consumed by whichever instance
 * dequeues it first.
 *
 * @param  iso Instance, must not be NULL.
 */
void nx_can_isotp_process(nx_can_isotp_t *iso);

/**
 * @brief  Send a message (a helper over @c sdu_rx_queue).
 *
 * Builds an SDU in the pool and publishes it to @c sdu_rx_queue, exactly as the
 * upper layer would for a direct queue push. The message goes out on
 * @c phys_tx_id when process() reaches it; a @p ta_type of
 * NX_TP_TA_FUNCTIONAL goes out on @c func_tx_id instead, and must be short
 * enough for a single frame, since a shared request ID cannot carry per-receiver
 * flow control.
 *
 * @param  iso     Instance, must not be NULL.
 * @param  data    Payload, must not be NULL.
 * @param  len     Payload length in bytes; must be > 0 and at most
 *                 NX_CAN_ISOTP_MAX_MSG_LEN.
 * @param  ta_type NX_TP_TA_PHYSICAL (default) or NX_TP_TA_FUNCTIONAL.
 *
 * @return NX_CAN_ISOTP_OK if queued; ERR_PARAM on bad arguments, on a functional
 *         request that does not fit one frame, or when @p ta_type is not one of
 *         the two defined values; ERR_LENGTH if @p len exceeds
 *         NX_CAN_ISOTP_MAX_MSG_LEN; ERR_NOMEM if the pool is exhausted;
 *         ERR_FULL if @c sdu_rx_queue is full.
 */
nx_can_isotp_ret_t nx_can_isotp_send(nx_can_isotp_t *iso, const uint8_t *data,
                                     size_t len, nx_tp_ta_type_t ta_type);

#ifdef __cplusplus
}
#endif

#endif /* NX_CAN_ISOTP_H */
