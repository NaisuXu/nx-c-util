/**
 * @file    nx_uds_tp_bind.h
 * @brief   Joins a diagnostic endpoint - a server that answers, or a client that
 *          asks - to a transport that speaks nx_tp_sdu_t.
 *
 * A diagnostic endpoint takes bytes and hands back bytes. A transport publishes what
 * it received, and what it finished sending, as reference counted messages on a
 * queue, and reads what it should send from another. This module is the few dozen
 * lines between the two.
 *
 * What carries the conversation is not named here and not known: the two queues and
 * the pool are all this module sees, so the same code joins an endpoint to any
 * transport that fills an nx_tp_sdu_t. One instance per path.
 *
 * The mechanics it owns are the ones easy to get wrong by hand: a message that
 * arrived is released as soon as the endpoint has copied it, an outbound frame the
 * transport cannot take yet is left for the endpoint to offer again rather than
 * dropped, and every outbound frame is published addressed the way it must travel
 * rather than the way it arrived. An inbound frame that the endpoint is not waiting
 * for is counted and dropped the same way on either side.
 *
 * The one difference between the two sides is how a frame is addressed on its way
 * out, and the side is where the binding reads it from. On the server side, a
 * request may arrive addressed to every receiver at once, and the server is told so
 * because it decides from that whether to answer at all - but the answer is owed to
 * one client, so it is published physically addressed whatever the question was. On
 * the client side, a request is itself the question, so how it travels is how the
 * caller asked for it to travel, and the transport reading @c ta_type off a message
 * it is asked to send finds the addressing the request needs.
 */
#ifndef NX_UDS_TP_BIND_H
#define NX_UDS_TP_BIND_H

#include <stdbool.h>
#include <stdint.h>

#include "src/core/nx_queue.h"
#include "src/core/nx_ref_msg.h"
#include "src/core/nx_tiered_mem_pool.h"
#include "nx_tp_sdu.h"
#include "nx_uds_server.h"
#include "nx_uds_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which end of the conversation a binding carries.
 */
typedef enum {
    NX_UDS_TP_SERVER = 0, /**< Answers requests: serves a server, publishes responses */
    NX_UDS_TP_CLIENT      /**< Asks questions: serves a client, publishes requests */
} nx_uds_tp_side_t;

/**
 * @brief Configuration of one binding.
 *
 * The two queues are named from this module's point of view, which is the opposite
 * of the transport's: what the transport publishes is what this reads.
 */
typedef struct {
    nx_uds_tp_side_t   side;         /**< Which end this binding carries. Required. */

    nx_uds_server_t   *srv;          /**< The server being served. Required when
                                      @c side is NX_UDS_TP_SERVER. */
    nx_uds_client_t   *clt;          /**< The client being served. Required when
                                      @c side is NX_UDS_TP_CLIENT. */

    nx_queue_t *sdu_in;              /**< transport -> here: messages that arrived and
                                      outcomes of messages sent. The transport's own
                                      outbound queue. Required. */
    nx_queue_t *sdu_out;             /**< here -> transport: responses or requests to
                                      send. The transport's own inbound queue.
                                      Required. */

    nx_tiered_mem_pool_t *pool;      /**< Where an outbound frame is allocated from.
                                      Required, and its own: a pool shared between two
                                      paths lets a flood on one starve the other. */

    uint8_t  link;                   /**< Connection number written into every frame
                                      published, which the transport reads back to know
                                      the message is its own. Match the transport's. */

    uint32_t max_sdu_len;            /**< Longest frame this path will publish, 0 to
                                      publish whatever the endpoint produces. A
                                      transport refusing an over-long message
                                      discovers the problem at the far end of a queue;
                                      naming the limit here discovers it where the
                                      frame is built. */
} nx_uds_tp_bind_cfg_t;

/**
 * @brief What the binding has had to discard, for an application that reports it.
 *
 * Every counter here names a message that was lost rather than answered, so a path
 * whose counters climb is a path that is failing quietly.
 */
typedef struct {
    uint32_t busy;        /**< Inbound messages dropped because the endpoint was
                           already mid-transaction. The transport had already
                           committed the message by publishing it, so there is
                           nowhere to put it back. */
    uint32_t refused;     /**< Messages the endpoint would not take: a link number that
                           is not its own, or an identifier from the response range. */
    uint32_t no_memory;   /**< Outbound frames that could not be allocated. */
    uint32_t queue_full;  /**< Outbound frames the outbound queue would not take. These
                           are offered again, so one frame may count several times. */
    uint32_t too_long;    /**< Outbound frames longer than this path carries. A path that
                           counts any is misconfigured: the endpoint's own limits
                           should have been capped at what the path accepts. */
} nx_uds_tp_bind_stats_t;

/**
 * @brief One binding.
 *
 * Declare one in static storage per path and hand it to nx_uds_tp_bind_init.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed to
 *        nx_uds_tp_bind_init.
 */
typedef struct {
    nx_uds_tp_bind_cfg_t cfg;        /**< Copied configuration */
    struct {
        nx_uds_tp_bind_stats_t stats; /**< What has been discarded */
    } run;                           /**< Internal runtime state */
} nx_uds_tp_bind_t;

/**
 * @brief  Set up a binding and wire the endpoint's outbound path to it.
 *
 * Installs this module as the endpoint's output path, so the endpoint's own output
 * callback is overwritten. Initialise the endpoint first, then bind it: the binding
 * needs the endpoint's address to attach itself, and a server or client that has not
 * been initialised has nowhere for the callback to point.
 *
 * @param  bind Handle to initialise, must not be NULL.
 * @param  cfg  Configuration, copied. The queues and pool it names must outlive the
 *              handle; the struct itself need not.
 * @return true on success; false where anything required is missing, including a
 *         server a client configuration would want and a client a server
 *         configuration would want.
 */
bool nx_uds_tp_bind_init(nx_uds_tp_bind_t *bind,
                         const nx_uds_tp_bind_cfg_t *cfg);

/**
 * @brief  Move one message between the transport and the endpoint.
 *
 * Takes at most one message off the inbound queue, because a server answers one
 * request at a time and a client asks one question at a time, and a second would be
 * refused rather than queued. Call it once per pass of the application's loop,
 * alongside the transport's own pump and the endpoint's.
 *
 * The endpoint is not pumped here: what to drive, and in what order, is the
 * application's to arrange.
 *
 * @param  bind Handle; NULL does nothing.
 */
void nx_uds_tp_bind_process(nx_uds_tp_bind_t *bind);

/**
 * @brief  What this binding has discarded.
 *
 * @param  bind  Handle, must not be NULL.
 * @param  stats Where to copy the counters. Must not be NULL.
 */
void nx_uds_tp_bind_get_stats(const nx_uds_tp_bind_t *bind,
                              nx_uds_tp_bind_stats_t *stats);
#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_TP_BIND_H */
