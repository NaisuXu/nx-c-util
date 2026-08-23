/**
 * @file    nx_uds_tp_bind.h
 * @brief   Joins a diagnostic server to a transport that speaks nx_tp_sdu_t.
 *
 * A diagnostic server takes request bytes and hands back response bytes. A
 * transport publishes what it received, and what it finished sending, as reference
 * counted messages on a queue, and reads what it should send from another. This
 * module is the few dozen lines between the two.
 *
 * What carries the conversation is not named here and not known: the two queues and
 * the pool are all this module sees, so the same code joins the server to any
 * transport that fills an nx_tp_sdu_t. One instance per path.
 *
 * The mechanics it owns are the ones easy to get wrong by hand: a received message
 * is released as soon as the server has copied it, a response the transport cannot
 * take yet is left for the server to offer again rather than dropped, a request
 * arriving while one is already being answered keeps the session alive instead of
 * letting it lapse, and every response published is addressed to the one client
 * that is owed it rather than to the whole link.
 *
 * That last one is the difference between how a request arrived and how its answer
 * travels. A request may be addressed to every receiver at once, and the server is
 * told so, because it decides from that whether to answer at all. The answer itself
 * is owed to one client, so it goes out physically addressed however the request
 * came in, and a transport reading @c ta_type off a message it is asked to send
 * finds the addressing the answer needs rather than the addressing the question had.
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration of one binding.
 *
 * The two queues are named from this module's point of view, which is the opposite
 * of the transport's: what the transport publishes is what this reads.
 */
typedef struct {
    nx_uds_server_t *srv;         /**< The server being served. Required. */

    nx_queue_t *sdu_in;           /**< transport -> here: messages that arrived and
                                   outcomes of messages sent. The transport's own
                                   outbound queue. Required. */
    nx_queue_t *sdu_out;          /**< here -> transport: responses to send. The
                                   transport's own inbound queue. Required. */

    nx_tiered_mem_pool_t *pool;   /**< Where a response is allocated from. Required,
                                   and its own: a pool shared between two paths lets
                                   a flood on one starve the other. */

    uint8_t  link;                /**< Connection number written into every response
                                   published, which the transport reads back to know
                                   the message is its own. Match the transport's. */

    uint32_t max_sdu_len;         /**< Longest response this path will publish, 0 to
                                   publish whatever the server produces. A transport
                                   refusing an over-long message discovers the problem
                                   at the far end of a queue; naming the limit here
                                   discovers it where the response is built. */
} nx_uds_tp_bind_cfg_t;

/**
 * @brief What the binding has had to discard, for an application that reports it.
 *
 * Every counter here names a message that was lost rather than answered, so a path
 * whose counters climb is a path that is failing quietly.
 */
typedef struct {
    uint32_t busy;        /**< Requests dropped because one was already being
                           answered. The transport had already committed the message
                           by publishing it, so there is nowhere to put it back. */
    uint32_t refused;     /**< Requests the server would not take: a link number that
                           is not its own, or an identifier from the response range. */
    uint32_t no_memory;   /**< Responses that could not be allocated. */
    uint32_t queue_full;  /**< Responses the outbound queue would not take. These are
                           offered again, so one response may count several times. */
    uint32_t too_long;    /**< Responses longer than this path carries. A path that
                           counts any is misconfigured: the server's own
                           @c max_resp_apdu should have been capped at what the path
                           accepts, so the refusal happened while the response was
                           being built rather than after it was finished. */
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
 * @brief  Set up a binding and wire the server's output to it.
 *
 * Installs this module as the server's output path, so the server's own @c out_fn
 * and @c out_user are overwritten. Initialise the server first, then bind it.
 *
 * @param  bind Handle to initialise, must not be NULL.
 * @param  cfg  Configuration, copied. The queues and pool it names must outlive the
 *              handle; the struct itself need not.
 * @return true on success; false where anything required is missing.
 */
bool nx_uds_tp_bind_init(nx_uds_tp_bind_t *bind,
                         const nx_uds_tp_bind_cfg_t *cfg);

/**
 * @brief  Move one message between the transport and the server.
 *
 * Takes at most one message off the inbound queue, because a server answers one
 * request at a time and a second would be refused rather than queued. Call it once
 * per pass of the application's loop, alongside the transport's own pump and the
 * server's.
 *
 * The server is not pumped here: what to drive, and in what order, is the
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
