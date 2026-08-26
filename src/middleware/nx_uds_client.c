/**
 * @file    nx_uds_client.c
 * @brief   Implementation of the ISO 14229 diagnostic client.
 *
 * See nx_uds_client.h for the design. A transaction moves through five states:
 *
 *   IDLE      nothing running; a request() may start something
 *   WAIT_SEND a request is armed and being offered to the send path
 *   WAIT_RSP  the send path took it; waiting for the first response
 *   WAIT_P2STAR the server said its answer is still coming; waiting out an
 *               extension
 *   CANCELED  the caller dropped the transaction; it is reported and cleared
 *
 * The request is assembled once, into the caller's buffer, and offered from
 * process() until the send path takes it. Nothing here allocates and nothing here
 * blocks.
 */
#include "nx_uds_client.h"

#include <string.h>

/** Transaction state machine positions. */
enum {
    TXN_IDLE = 0,   /**< No transaction */
    TXN_WAIT_SEND,  /**< Request armed; offering it to the send path */
    TXN_WAIT_RSP,   /**< Send path took it; awaiting the first response */
    TXN_WAIT_P2STAR,/**< 0x78 received; awaiting during an extension */
    TXN_CANCELED    /**< Caller dropped it; report it at the next process() */
};

/** @brief Read the clock the configuration supplies. */
static uint32_t uds_now(const nx_uds_client_t *clt)
{
    return clt->cfg.get_us();
}

/**
 * @brief  Whether a deadline has passed.
 *
 * The clock is free-running and wraps, so the comparison is on the signed
 * difference: that stays correct across a wrap for as long as the interval being
 * measured is under half the counter's range.
 */
static bool uds_expired(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* ------------------------------------------------------------------ */
/* Resolving a transaction                                              */
/* ------------------------------------------------------------------ */
/**
 * @brief  End the transaction and report how it ended.
 *
 * Only the transaction is cleared: the session and the effective wait windows
 * outlive it, because they belong to the conversation rather than to one request
 * in it.
 */
static void uds_txn_resolve(nx_uds_client_t *clt, nx_uds_client_result_t result)
{
    memset(&clt->run.txn, 0, sizeof(clt->run.txn));
    clt->run.txn.state = TXN_IDLE;

    if (clt->cfg.result_fn != NULL) {
        clt->cfg.result_fn(clt->cfg.result_user, clt, result);
    }
}

/**
 * @brief  Whether a response answers the transaction in flight.
 *
 * A positive response answers a request identifier by adding one bit, so the two
 * share all but that bit: the positive response to the service in flight is the
 * identifier with the added bit set. A negative response names the service in its
 * second byte, and the same comparison applies to it - because a negative response
 * to a different service is not this transaction's answer.
 */
static bool uds_answers_sid(const nx_uds_client_t *clt, const uint8_t *rsp)
{
    uint8_t sid = clt->run.txn.sid;

    if (NX_UDS_IS_NEG_RSP(rsp[0])) {
        return rsp[1] == sid;
    }
    return NX_UDS_SID_TO_POS_RSP(sid) == rsp[0];
}

/**
 * @brief  Buy another window: the server said the answer is still coming.
 *
 * The notification is a negative response whose reason is the code meaning
 * "received correctly, answer still coming". It is not the answer, but it is why
 * one is still owed, so the transaction is extended rather than resolved.
 *
 * @return true if the transaction resolved (it ran out of extensions); false if it
 *         is still waiting out the new window.
 */
static bool uds_txn_pend(nx_uds_client_t *clt)
{
    nx_uds_client_txn_t *txn = &clt->run.txn;

    if (txn->pend_count >= clt->cfg.max_pending) {
        /* A server that never finishes would keep extending the window forever,
         * so the count is what finally stops it. */
        clt->run.resp_len = 0u;
        uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_TIMEOUT);
        return true;
    }
    txn->pend_count++;
    /* Having said the answer is coming, the server owes one. A request that asked
     * for silence gave up its right to it the moment it was told to wait, so from
     * here on a silent window is a timeout, not the quiet the request asked for. */
    txn->suppress_pos = false;
    txn->state        = TXN_WAIT_P2STAR;
    txn->deadline     = uds_now(clt) + clt->run.p2_star_us;
    return false;
}

/**
 * @brief  Interpret a response that has arrived for the transaction in flight.
 *
 * A response either resolves the transaction - with a result, or as a protocol
 * error where the frame is not one this state allows - or extends it, when the
 * server says the answer is still coming.
 *
 * @return true if the transaction resolved; false if it is still waiting.
 */
static bool uds_digest(nx_uds_client_t *clt, const uint8_t *rsp, uint32_t len)
{
    nx_uds_client_txn_t *txn = &clt->run.txn;

    if (len < 1u) {
        clt->run.resp_len = 0u;
        uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
        return true;
    }

    if (NX_UDS_IS_NEG_RSP(rsp[0])) {
        /* A refusal is three bytes: the code that marks it, the service being
         * refused, and the reason. Less than that cannot name either, so it is
         * not a refusal this transaction can reconcile. */
        if (len < NX_UDS_NEG_RSP_LEN) {
            clt->run.resp_len = 0u;
            uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
            return true;
        }
        /* A 0x78 responsePending is an ordinary refusal whose reason is the code
         * meaning "answer still coming". It is not the answer, so the transaction
         * is extended rather than resolved. */
        if (len == NX_UDS_NEG_RSP_LEN &&
            rsp[2] == (uint8_t)NX_UDS_NRC_RESPONSE_PENDING) {
            if (!uds_answers_sid(clt, rsp)) {
                clt->run.resp_len = 0u;
                uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
                return true;
            }
            return uds_txn_pend(clt);
        }
        /* A refusal that answers the request: the request was understood and
         * declined, and the response is kept for the application to read. */
        if (!uds_answers_sid(clt, rsp)) {
            clt->run.resp_len = 0u;
            uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
            return true;
        }
        if (len > clt->cfg.rsp_buf_size) {
            clt->run.resp_len = 0u;
            uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
            return true;
        }
        memcpy(clt->cfg.rsp_buf, rsp, len);
        clt->run.resp_len = len;
        uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_NEGATIVE);
        return true;
    }

    /* A positive response for the service in flight is the answer. It is copied
     * whole: a positive response longer than the buffer is one this client cannot
     * hold, and is reported as such rather than truncated. */
    if (uds_answers_sid(clt, rsp)) {
        if (len > clt->cfg.rsp_buf_size) {
            clt->run.resp_len = 0u;
            uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
            return true;
        }
        /* A session-control answer re-arms the conversation: the session it
         * names becomes this client's, and the wait windows it publishes - unless
         * the client is told to keep its own - become the ones it waits with. */
        if (txn->sid == (uint8_t)NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL &&
            len >= 2u) {
            clt->run.session = rsp[1];
            if (!clt->cfg.fixed_timing && len >= 6u) {
                uint32_t p2  = ((uint32_t)rsp[2] << 8) | rsp[3];
                uint32_t p2s = ((uint32_t)rsp[4] << 8) | rsp[5];
                clt->run.p2_us     = p2 * NX_UDS_P2_RESOLUTION_US;
                clt->run.p2_star_us = p2s * NX_UDS_P2_STAR_RESOLUTION_US;
            }
        }
        memcpy(clt->cfg.rsp_buf, rsp, len);
        clt->run.resp_len = len;
        uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_OK);
        return true;
    }

    /* A frame that answers nothing this transaction asked: a response for
     * another service, or one this client cannot reconcile. */
    clt->run.resp_len = 0u;
    uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);
    return true;
}

/* ------------------------------------------------------------------ */
/* Arming a transaction                                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief  Arm a transaction whose request is already in the request buffer.
 *
 * The request outlives this call, so keeping it in the caller's buffer is what
 * makes a transaction safe: it can be re-offered, and the service identifier and
 * suppression intent are read off it for as long as it runs.
 */
static void uds_txn_arm(nx_uds_client_t *clt, uint8_t sid, bool suppress_pos,
                        uint8_t ta_type, uint32_t len)
{
    nx_uds_client_txn_t *txn = &clt->run.txn;
    uint32_t now = uds_now(clt);

    clt->run.resp_len = 0u;
    memset(txn, 0, sizeof(*txn));
    txn->state         = TXN_WAIT_SEND;
    txn->sid           = sid;
    txn->suppress_pos  = suppress_pos;
    txn->ta_type       = ta_type;
    txn->link          = clt->cfg.link;
    txn->req_len       = len;
    txn->started       = now;
    txn->send_deadline = now + clt->cfg.send_timeout_us;
    txn->deadline      = now + clt->run.p2_us;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
nx_uds_client_ret_t nx_uds_client_init(nx_uds_client_t *clt,
                                       const nx_uds_client_cfg_t *cfg)
{
    if (clt == NULL || cfg == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (cfg->req_buf == NULL || cfg->req_buf_size < 1u) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    /* Every response the client interprets is at least a negative response (a 0x78
     * responsePending included), so a buffer that cannot hold one cannot be talked
     * to. */
    if (cfg->rsp_buf == NULL || cfg->rsp_buf_size < NX_UDS_NEG_RSP_LEN) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (cfg->get_us == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }

    clt->cfg = *cfg;
    /* Resolve "0 means default" once, so the hot paths read a plain value. */
    if (clt->cfg.p2_us == 0u) {
        clt->cfg.p2_us = NX_UDS_CLIENT_DEFAULT_P2_US;
    }
    if (clt->cfg.p2_star_us == 0u) {
        clt->cfg.p2_star_us = NX_UDS_CLIENT_DEFAULT_P2_STAR_US;
    }
    if (clt->cfg.send_timeout_us == 0u) {
        clt->cfg.send_timeout_us = NX_UDS_CLIENT_DEFAULT_SEND_TIMEOUT_US;
    }
    if (clt->cfg.max_pending == 0u) {
        clt->cfg.max_pending = NX_UDS_CLIENT_DEFAULT_MAX_PENDING;
    }

    memset(&clt->run, 0, sizeof(clt->run));
    clt->run.session    = (uint8_t)NX_UDS_SESSION_DEFAULT;
    clt->run.p2_us      = clt->cfg.p2_us;
    clt->run.p2_star_us = clt->cfg.p2_star_us;
    clt->run.txn.state  = TXN_IDLE;
    return NX_UDS_CLIENT_OK;
}

nx_uds_client_ret_t nx_uds_client_request(nx_uds_client_t *clt, uint8_t sid,
                                          uint8_t subfunc, const uint8_t *data,
                                          size_t len, nx_tp_ta_type_t ta_type)
{
    if (clt == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (len > 0u && data == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (clt->run.txn.state != TXN_IDLE) {
        return NX_UDS_CLIENT_ERR_BUSY;
    }
    /* A request the caller's buffer cannot hold is not a transaction: the request
     * has to be kept for as long as the transaction runs, so a length past the
     * buffer is refused here rather than truncated later. The two bytes beyond
     * the payload are the service identifier and the sub-function byte. */
    if (len > clt->cfg.req_buf_size || clt->cfg.req_buf_size - len < 2u) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }

    clt->cfg.req_buf[0] = sid;
    clt->cfg.req_buf[1] = subfunc;
    if (len > 0u) {
        memcpy(&clt->cfg.req_buf[2], data, len);
    }

    /* The request for silence rides in the top bit of the sub-function byte, and
     * it is read into the transaction here so that a request that asks for no
     * positive response is interpreted as one that asked for it - that is what
     * decides that a silent window is the expected outcome rather than a timeout. */
    uds_txn_arm(clt, sid, NX_UDS_SUPPRESSES_POS_RSP(subfunc), ta_type,
                (uint32_t)len + 2u);
    return NX_UDS_CLIENT_OK;
}

nx_uds_client_ret_t nx_uds_client_request_raw(nx_uds_client_t *clt,
                                              const uint8_t *req, size_t len,
                                              nx_tp_ta_type_t ta_type)
{
    if (clt == NULL || req == NULL || len == 0u) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (clt->run.txn.state != TXN_IDLE) {
        return NX_UDS_CLIENT_ERR_BUSY;
    }
    if (len > clt->cfg.req_buf_size) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    memcpy(clt->cfg.req_buf, req, len);

    /* The client did not build the frame, so it does not know whether a
     * sub-function byte carried a request for silence; the request is treated as
     * one that is owed a positive response. */
    uds_txn_arm(clt, req[0], false, ta_type, (uint32_t)len);
    return NX_UDS_CLIENT_OK;
}

nx_uds_client_ret_t nx_uds_client_process(nx_uds_client_t *clt)
{
    if (clt == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }

    nx_uds_client_txn_t *txn = &clt->run.txn;
    uint32_t now = uds_now(clt);

    if (txn->state == TXN_IDLE) {
        return NX_UDS_CLIENT_OK;
    }

    /* A canceled transaction is reported here, at a clean point in the loop,
     * rather than inside cancel(): an application may cancel from anywhere. */
    if (txn->state == TXN_CANCELED) {
        uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_CANCELED);
        return NX_UDS_CLIENT_OK;
    }

    if (txn->state == TXN_WAIT_SEND) {
        /* Nothing is carrying this conversation yet, which is not distinguishable
         * from a carrier that will not take anything: either way the request did
         * not go. */
        bool taken = (clt->cfg.send_fn != NULL)
                     && clt->cfg.send_fn(clt->cfg.send_user, txn->link,
                                         clt->cfg.req_buf, txn->req_len,
                                         txn->ta_type);
        if (taken) {
            /* Handed over. The wait for the response starts now, with the
             * ordinary window. */
            txn->state    = TXN_WAIT_RSP;
            txn->deadline = now + clt->run.p2_us;
        } else if (uds_expired(now, txn->send_deadline)) {
            /* The carrier never took it and the time it was allowed to think
             * about it is gone. The request never left, so this is a failure to
             * get it out rather than a response that did not come. */
            uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_TIMEOUT);
        }
        return NX_UDS_CLIENT_OK;
    }

    if (txn->state == TXN_WAIT_RSP || txn->state == TXN_WAIT_P2STAR) {
        /* A window that runs out is the one way these states advance without a
         * response. Whether the silence is an outcome or a failure depends on
         * whether an answer was owed: a request that asked for no positive
         * response is answered by silence, and anything else is a timeout. */
        if (uds_expired(now, txn->deadline)) {
            if (txn->suppress_pos) {
                uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_NO_RESPONSE);
            } else {
                uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_TIMEOUT);
            }
        }
        return NX_UDS_CLIENT_OK;
    }

    return NX_UDS_CLIENT_OK;
}

nx_uds_client_ret_t nx_uds_client_indicate(nx_uds_client_t *clt, const uint8_t *rsp,
                                           uint32_t len, uint8_t ta_type,
                                           uint8_t link)
{
    if (clt == NULL || rsp == NULL || len == 0u) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    if (link != clt->cfg.link) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    /* Only a transaction that is actually waiting for a response can accept one;
     * a response that arrives out of the blue has no state to disturb. */
    if (clt->run.txn.state != TXN_WAIT_RSP &&
        clt->run.txn.state != TXN_WAIT_P2STAR) {
        return NX_UDS_CLIENT_ERR_STATE;
    }
    /* The addressing is carried for completeness: how a response was addressed
     * says nothing about whether it answers this transaction, so it is not
     * arbitrated on. */
    (void)ta_type;

    (void)uds_digest(clt, rsp, len);
    return NX_UDS_CLIENT_OK;
}

void nx_uds_client_confirm(nx_uds_client_t *clt, uint8_t link, uint8_t result)
{
    if (clt == NULL) {
        return;
    }
    if (link != clt->cfg.link) {
        return;
    }
    nx_uds_client_txn_t *txn = &clt->run.txn;

    /* Only a transaction waiting for a response has an outcome pending. A carrier
     * that confirms twice, or confirms one that has already resolved, is told
     * nothing rather than allowed to disturb whatever is running now. */
    if (txn->state != TXN_WAIT_RSP && txn->state != TXN_WAIT_P2STAR) {
        return;
    }
    if (result == (uint8_t)NX_TP_N_OK) {
        /* The request went out; the client is already waiting for its answer. */
        return;
    }
    /* The request did not reach the link, and hearing that here is better than
     * after the response window runs out. */
    clt->run.resp_len = 0u;
    uds_txn_resolve(clt, NX_UDS_CLIENT_RESULT_TIMEOUT);
}

nx_uds_client_ret_t nx_uds_client_cancel(nx_uds_client_t *clt)
{
    if (clt == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    nx_uds_client_txn_t *txn = &clt->run.txn;

    if (txn->state == TXN_IDLE || txn->state == TXN_CANCELED) {
        return NX_UDS_CLIENT_ERR_STATE;
    }
    txn->state = TXN_CANCELED;
    return NX_UDS_CLIENT_OK;
}

bool nx_uds_client_is_busy(const nx_uds_client_t *clt)
{
    return (clt != NULL) && (clt->run.txn.state != TXN_IDLE);
}

uint8_t nx_uds_client_session(const nx_uds_client_t *clt)
{
    return (clt != NULL) ? clt->run.session : (uint8_t)NX_UDS_SESSION_DEFAULT;
}

void nx_uds_client_timing(const nx_uds_client_t *clt, uint32_t *p2,
                          uint32_t *p2_star)
{
    if (clt == NULL) {
        return;
    }
    if (p2 != NULL) {
        *p2 = clt->run.p2_us;
    }
    if (p2_star != NULL) {
        *p2_star = clt->run.p2_star_us;
    }
}

void nx_uds_client_set_timing(nx_uds_client_t *clt, uint32_t p2_us,
                              uint32_t p2_star_us)
{
    if (clt == NULL) {
        return;
    }
    clt->cfg.p2_us      = p2_us;
    clt->cfg.p2_star_us = p2_star_us;
    clt->run.p2_us      = p2_us;
    clt->run.p2_star_us = p2_star_us;
}

uint32_t nx_uds_client_resp_len(const nx_uds_client_t *clt)
{
    return (clt != NULL) ? clt->run.resp_len : 0u;
}

nx_uds_client_ret_t nx_uds_client_set_send(nx_uds_client_t *clt,
                                           nx_uds_client_send_fn fn, void *user)
{
    if (clt == NULL || fn == NULL) {
        return NX_UDS_CLIENT_ERR_PARAM;
    }
    clt->cfg.send_fn   = fn;
    clt->cfg.send_user = user;
    return NX_UDS_CLIENT_OK;
}
