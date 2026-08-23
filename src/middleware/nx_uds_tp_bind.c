/**
 * @file    nx_uds_tp_bind.c
 * @brief   Implementation of the server-to-transport binding.
 */
#include <string.h>

#include "nx_uds_tp_bind.h"

/* ------------------------------------------------------------------ */
/* Server -> transport                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief  Publish a finished response to the transport.
 *
 * Returning false tells the server the response has not gone anywhere, and the same
 * response is offered again on later pumps until the transaction runs out of time.
 * That is what makes a momentarily full queue a delay rather than a lost answer, and
 * it is why this must never report success it did not have.
 *
 * @param  user    The binding.
 * @param  link    Connection the request arrived on.
 * @param  rsp     Response bytes, valid only for this call.
 * @param  len     How many.
 * @param  ta_type How the request was addressed, which is not how the response is
 *                 sent; see below.
 * @return true when the transport has taken it.
 */
static bool bind_out(void *user, uint8_t link, const uint8_t *rsp, uint32_t len,
                     uint8_t ta_type)
{
    nx_uds_tp_bind_t *bind = (nx_uds_tp_bind_t *)user;
    nx_ref_msg_t *m;
    nx_tp_sdu_t  *sdu;

    (void)link;
    /* How the request was addressed has already done its work inside the server,
     * where it decided whether to answer at all. It says nothing about how the
     * answer travels: a response goes to the one client that is owed it, so it is
     * addressed to that client and never to the whole link. */
    (void)ta_type;

    /* A response longer than this path carries would be discovered by the transport
     * at the far end of a queue, where nothing can be done about it. Refusing it here
     * is not a recovery - the same response will be offered again and refused again
     * until the transaction gives up - but it is the truthful answer, and reporting it
     * taken would leave the server waiting for an outcome that cannot arrive.
     *
     * A path that counts these is configured wrongly: cap the server's own
     * max_resp_apdu at what the path carries and it cannot happen. */
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
    sdu->ta_type = (uint8_t)NX_TP_TA_PHYSICAL;
    sdu->result  = (uint8_t)NX_TP_N_OK;
    memcpy(sdu->data, rsp, len);

    if (nx_ref_msg_publish(m, bind->cfg.sdu_out) != NX_REF_MSG_OK) {
        /* The queue is full. Releasing returns the block, so nothing leaks, and the
         * server offers the same response again. */
        nx_ref_msg_release(m);
        bind->run.stats.queue_full++;
        return false;
    }
    /* Published, so the queue holds its own reference; this one is done with. */
    nx_ref_msg_release(m);
    return true;
}

/* ------------------------------------------------------------------ */
/* Transport -> server                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief  Hand one message that arrived to the server.
 *
 * @param  bind Handle.
 * @param  sdu  The message.
 */
static void bind_deliver(nx_uds_tp_bind_t *bind, const nx_tp_sdu_t *sdu)
{
    nx_uds_server_ret_t r;

    r = nx_uds_server_indicate(bind->cfg.srv, sdu->data, sdu->len, sdu->ta_type,
                               sdu->link);
    switch (r) {
    case NX_UDS_SERVER_OK:
        break;

    case NX_UDS_SERVER_ERR_BUSY:
        /* One is already being answered, and this one is gone: the transport gave it
         * up by publishing it, and a server that took it would abandon the answer it
         * is in the middle of. What can still be done is to count the client as
         * having spoken, so a long transaction is not overtaken by the quiet timer
         * while its own client keeps asking after it. */
        nx_uds_server_touch(bind->cfg.srv);
        bind->run.stats.busy++;
        break;

    default:
        /* Not this server's: another link, or an identifier that belongs to an
         * answer rather than a request. */
        bind->run.stats.refused++;
        break;
    }
}

void nx_uds_tp_bind_process(nx_uds_tp_bind_t *bind)
{
    nx_ref_msg_t *m = NULL;
    const nx_tp_sdu_t *sdu;

    if (bind == NULL) {
        return;
    }
    /* One message per pass. A server answers one request at a time, so draining the
     * queue would only produce a run of refusals. */
    if (nx_queue_pop(bind->cfg.sdu_in, &m) != NX_QUEUE_OK) {
        return;
    }
    sdu = (const nx_tp_sdu_t *)nx_ref_msg_data(m);

    if (sdu->kind == (uint8_t)NX_TP_SDU_CONFIRM) {
        /* What became of a response that was sent. The server is waiting for exactly
         * this before it considers the transaction over. */
        (void)nx_uds_server_confirm(bind->cfg.srv, sdu->link, sdu->result);
    } else {
        bind_deliver(bind, sdu);
    }

    /* Done with it. The server copied whatever it needed to keep, so the block goes
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
    if (cfg->srv == NULL || cfg->sdu_in == NULL || cfg->sdu_out == NULL
        || cfg->pool == NULL) {
        return false;
    }

    memset(bind, 0, sizeof(*bind));
    bind->cfg = *cfg;

    /* The server's answers come here from now on. */
    return nx_uds_server_set_output(cfg->srv, bind_out, bind) == NX_UDS_SERVER_OK;
}

void nx_uds_tp_bind_get_stats(const nx_uds_tp_bind_t *bind,
                              nx_uds_tp_bind_stats_t *stats)
{
    if (bind == NULL || stats == NULL) {
        return;
    }
    *stats = bind->run.stats;
}

