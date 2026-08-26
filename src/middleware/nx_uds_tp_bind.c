/**
 * @file    nx_uds_tp_bind.c
 * @brief   Implementation of the endpoint-to-transport binding.
 */
#include <string.h>

#include "nx_uds_tp_bind.h"

/* ------------------------------------------------------------------ */
/* Endpoint -> transport                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief  Publish a finished frame to the transport.
 *
 * What the frame is depends on which end this binding carries: a server's response
 * or a client's request. Both are published as an indication to the transport's
 * inbound queue, and both leave through the same mechanics - a transport that
 * cannot take one yet is told so, and the same frame is offered again on later
 * pumps until the endpoint runs out of time. That is what makes a momentarily full
 * queue a delay rather than a lost frame, and it is why this must never report
 * success it did not have.
 *
 * The one difference is how the frame is addressed on its way out. A server's
 * response goes to the one client that is owed it, so however the request came in
 * it is published physically addressed. A client's request is itself the question,
 * so how it travels is however the transaction asked for it to travel.
 *
 * @param  user    The binding.
 * @param  link    Connection the frame is owed on.
 * @param  frame   Frame bytes, valid only for this call.
 * @param  len     How many.
 * @param  ta_type How the frame is addressed, which the frame itself decides for a
 *                 response and inherits from the request for a request.
 * @return true when the transport has taken it.
 */
static bool bind_out(void *user, uint8_t link, const uint8_t *frame, uint32_t len,
                     uint8_t ta_type)
{
    nx_uds_tp_bind_t *bind = (nx_uds_tp_bind_t *)user;
    nx_ref_msg_t *m;
    nx_tp_sdu_t  *sdu;
    uint8_t       sdu_ta;

    (void)link;
    /* The addressing the frame travels with is not always the addressing it was
     * built with. A server answers a functionally addressed request with a
     * physically addressed response, because the answer is owed to one client; a
     * client's request is addressed however the transaction asked for it, so the
     * caller's addressing is what the transport must see. */
    if (bind->cfg.side == NX_UDS_TP_SERVER) {
        sdu_ta = (uint8_t)NX_TP_TA_PHYSICAL;
    } else {
        sdu_ta = ta_type;
    }

    /* A frame longer than this path carries would be discovered by the transport
     * at the far end of a queue, where nothing can be done about it. Refusing it here
     * is not a recovery - the same frame will be offered again and refused again
     * until the transaction gives up - but it is the truthful answer, and reporting it
     * taken would leave the endpoint waiting for an outcome that cannot arrive.
     *
     * A path that counts these is configured wrongly: cap the endpoint's own limits
     * at what the path carries and it cannot happen. */
    if (bind->cfg.max_sdu_len != 0u && len > bind->cfg.max_sdu_len) {
        bind->run.stats.too_long++;
        return false;
    }

    m = nx_ref_msg_alloc(bind->cfg.pool, sizeof(nx_tp_sdu_t) + len);
    if (m == NULL) {
        bind->run.stats.no_memory++;
        return false;   /* offer it again: the pool may free up */
    }

    sdu = (nx_tp_sdu_t *)nx_ref_msg_data(m);
    sdu->len     = len;
    sdu->link    = bind->cfg.link;
    sdu->kind    = (uint8_t)NX_TP_SDU_INDICATION;
    sdu->ta_type = sdu_ta;
    sdu->result  = (uint8_t)NX_TP_N_OK;
    memcpy(sdu->data, frame, len);

    if (nx_ref_msg_publish(m, bind->cfg.sdu_out) != NX_REF_MSG_OK) {
        /* The queue is full. Releasing returns the block, so nothing leaks, and the
         * endpoint offers the same frame again. */
        nx_ref_msg_release(m);
        bind->run.stats.queue_full++;
        return false;
    }
    /* Published, so the queue holds its own reference; this one is done with. */
    nx_ref_msg_release(m);
    return true;
}

/* ------------------------------------------------------------------ */
/* Transport -> endpoint                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief  Hand one message that arrived to the endpoint.
 *
 * A message the endpoint cannot take is gone: the transport gave it up by publishing
 * it, and an endpoint that took it would abandon whatever it is already doing. What
 * is left is to count it, as busy where the endpoint was already mid-transaction and
 * as refused where the message was not for the endpoint to begin with.
 *
 * @param  bind Handle.
 * @param  sdu  The message.
 */
static void bind_deliver(nx_uds_tp_bind_t *bind, const nx_tp_sdu_t *sdu)
{
    if (bind->cfg.side == NX_UDS_TP_SERVER) {
        nx_uds_server_ret_t r =
            nx_uds_server_indicate(bind->cfg.srv, sdu->data, sdu->len, sdu->ta_type,
                                   sdu->link);
        switch (r) {
        case NX_UDS_SERVER_OK:
            break;
        case NX_UDS_SERVER_ERR_BUSY:
            /* One is already being answered. Count the client as having spoken, so a
             * long transaction is not overtaken by the quiet timer while its own
             * client keeps asking after it. */
            nx_uds_server_touch(bind->cfg.srv);
            bind->run.stats.busy++;
            break;
        default:
            /* Not this server's: another link, or an identifier that belongs to an
             * answer rather than a request. */
            bind->run.stats.refused++;
            break;
        }
    } else {
        nx_uds_client_ret_t r =
            nx_uds_client_indicate(bind->cfg.clt, sdu->data, sdu->len, sdu->ta_type,
                                   sdu->link);
        switch (r) {
        case NX_UDS_CLIENT_OK:
            break;
        case NX_UDS_CLIENT_ERR_BUSY:
        case NX_UDS_CLIENT_ERR_STATE:
            /* A response arrived while the client was not waiting for one: another
             * transaction in flight, a request still being offered, or one that has
             * already resolved. Nothing is waiting to hear it, so it is counted the
             * way a request that overtakes a server one is. */
            bind->run.stats.busy++;
            break;
        default:
            /* Not this client's: another link, or a length it cannot read. */
            bind->run.stats.refused++;
            break;
        }
    }
}

void nx_uds_tp_bind_process(nx_uds_tp_bind_t *bind)
{
    nx_ref_msg_t *m = NULL;
    const nx_tp_sdu_t *sdu;

    if (bind == NULL) {
        return;
    }
    /* One message per pass. A server answers one request at a time and a client asks
     * one question at a time, so draining the queue would only produce a run of
     * refusals. */
    if (nx_queue_pop(bind->cfg.sdu_in, &m) != NX_QUEUE_OK) {
        return;
    }
    sdu = (const nx_tp_sdu_t *)nx_ref_msg_data(m);

    if (sdu->kind == (uint8_t)NX_TP_SDU_CONFIRM) {
        /* What became of a frame that was sent. The endpoint is waiting for exactly
         * this before it considers the transaction over. */
        if (bind->cfg.side == NX_UDS_TP_SERVER) {
            (void)nx_uds_server_confirm(bind->cfg.srv, sdu->link, sdu->result);
        } else {
            nx_uds_client_confirm(bind->cfg.clt, sdu->link, sdu->result);
        }
    } else {
        bind_deliver(bind, sdu);
    }

    /* Done with it. The endpoint copied whatever it needed to keep, so the block goes
     * back now rather than being held for the length of a transaction. */
    nx_ref_msg_release(m);
}

/* ------------------------------------------------------------------ */
/* Setting up                                                        */
/* ------------------------------------------------------------------ */
bool nx_uds_tp_bind_init(nx_uds_tp_bind_t *bind,
                         const nx_uds_tp_bind_cfg_t *cfg)
{
    if (bind == NULL || cfg == NULL) {
        return false;
    }
    if (cfg->sdu_in == NULL || cfg->sdu_out == NULL || cfg->pool == NULL) {
        return false;
    }
    /* The endpoint this binding serves is chosen by the side, and each side names its
     * own: a configuration that wants one and provides the other is a mis-wiring that
     * would dispatch through a null or a wrong pointer. */
    if (cfg->side == NX_UDS_TP_SERVER) {
        if (cfg->srv == NULL) {
            return false;
        }
    } else if (cfg->side == NX_UDS_TP_CLIENT) {
        if (cfg->clt == NULL) {
            return false;
        }
    } else {
        return false;
    }

    memset(bind, 0, sizeof(*bind));
    bind->cfg = *cfg;

    /* The endpoint's frames come here from now on. */
    if (bind->cfg.side == NX_UDS_TP_SERVER) {
        return nx_uds_server_set_output(cfg->srv, bind_out, bind) == NX_UDS_SERVER_OK;
    }
    return nx_uds_client_set_send(cfg->clt, bind_out, bind) == NX_UDS_CLIENT_OK;
}

void nx_uds_tp_bind_get_stats(const nx_uds_tp_bind_t *bind,
                              nx_uds_tp_bind_stats_t *stats)
{
    if (bind == NULL || stats == NULL) {
        return;
    }
    *stats = bind->run.stats;
}
