/**
 * @file    nx_uds_server.c
 * @brief   Implementation of the ISO 14229 diagnostic server.
 *
 * See nx_uds_server.h for the design. A transaction moves through four states:
 *
 *   IDLE     nothing running; an indication may start something
 *   RUNNING  the handler has not finished; it is re-entered from process()
 *   SENDING  an answer is assembled and being offered to the carrier
 *   AWAIT    the carrier took it; waiting to hear whether it went out
 *
 * The answer is assembled once, into the caller's buffer, and offered from
 * process() until the carrier takes it. Nothing here allocates and nothing here
 * blocks.
 */
#include "nx_uds_server.h"

#include <string.h>

/** Transaction state machine positions. */
enum {
    TXN_IDLE = 0,  /**< No transaction */
    TXN_RUNNING,   /**< Handler has not produced a final answer yet */
    TXN_SENDING,   /**< Answer assembled; offering it to the carrier */
    TXN_AWAIT      /**< Carrier took the answer; awaiting its outcome */
};

/** @brief Read the clock the configuration supplies. */
static uint32_t uds_now(const nx_uds_server_t *srv)
{
    return srv->cfg.get_us();
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
/* The service table                                                  */
/* ------------------------------------------------------------------ */
/**
 * @brief  Whether an identifier names a response rather than a request.
 *
 * Positive response identifiers are request identifiers with one bit added, and
 * the code marking a refusal sits in the same range, so those two spans of values
 * are answers. One arriving as a request is an answer that has come back around.
 */
static bool uds_is_response_sid(uint8_t sid)
{
    return (sid >= 0x40u && sid <= 0x7Fu) || (sid >= 0xC0u);
}

/**
 * @brief  Find the row implementing a service identifier.
 *
 * The first row carrying the identifier wins, so a table with a duplicate uses
 * the earlier one. The search is linear over a table an application writes by
 * hand, which is short.
 *
 * @return The row's index, or the row count when no row carries it.
 */
static uint16_t uds_find_row(const nx_uds_server_t *srv, uint8_t sid)
{
    uint16_t i;
    for (i = 0u; i < srv->cfg.services_count; i++) {
        if (srv->cfg.services[i].sid == sid) {
            return i;
        }
    }
    return srv->cfg.services_count;
}

/**
 * @brief  The row the transaction in flight belongs to, or NULL when it has none.
 *
 * A refusal decided before the table was consulted - or because the table had
 * nothing to consult - still needs a transaction to carry it, and that transaction
 * names no row. Everything that reads per-service configuration goes through here
 * so that case cannot index past the table.
 */
static const nx_uds_service_t *uds_txn_row(const nx_uds_server_t *srv)
{
    if (srv->run.txn.row >= srv->cfg.services_count) {
        return NULL;
    }
    return &srv->cfg.services[srv->run.txn.row];
}

/** @brief Whether a row is available in a session. */
static bool uds_row_in_session(const nx_uds_service_t *row, uint8_t session)
{
    return (row->session_mask & NX_UDS_SESSION_BIT(session)) != 0u;
}

/* ------------------------------------------------------------------ */
/* Session state                                                      */
/* ------------------------------------------------------------------ */
/**
 * @brief  Arm or disarm the quiet timer according to the active session.
 *
 * The timer exists to return an idle conversation to the default session, so it
 * runs in every other session and not in that one.
 */
static void uds_arm_s3(nx_uds_server_t *srv)
{
    if (srv->run.session == NX_UDS_SESSION_DEFAULT) {
        srv->run.s3_armed = false;
        return;
    }
    srv->run.s3_armed    = true;
    srv->run.s3_deadline = uds_now(srv) + srv->cfg.s3_us;
}

/**
 * @brief  Enter a session, relocking the server and reporting the change.
 *
 * A security level is unlocked for the session it was unlocked in, and entering a
 * session is entering a new one whichever session that is - so this locks the
 * server every time, including when the session entered is the one already
 * active. Asking again for the session you are in is a way to relock, and treating
 * it as nothing to do would leave a level unlocked that nothing had authorized in
 * the session now running.
 */
static void uds_enter_session(nx_uds_server_t *srv, uint8_t session)
{
    uint8_t from = srv->run.session;

    srv->run.session   = session;
    srv->run.sec_level = 0u;
    uds_arm_s3(srv);

    if (from != session && srv->cfg.session_fn != NULL) {
        srv->cfg.session_fn(srv->cfg.out_user, from, session);
    }
}

/* ------------------------------------------------------------------ */
/* Assembling a response                                              */
/* ------------------------------------------------------------------ */
/**
 * @brief  Write a negative response into the transaction's buffer.
 *
 * Three bytes: the code that marks a refusal, the service identifier being
 * refused, and the reason. The buffer is validated at init to hold at least
 * that, so a refusal can always be expressed however small the buffer is.
 */
static void uds_put_negative(nx_uds_server_t *srv, uint8_t sid, uint8_t nrc)
{
    uint8_t *out = srv->cfg.out_buf;

    out[0] = NX_UDS_NEG_RSP_SID;
    out[1] = sid;
    out[2] = nrc;
    srv->run.txn.ctx.out_len = NX_UDS_NEG_RSP_LEN;
}

/**
 * @brief  Whether a negative response to this request would go unsent.
 *
 * A functionally addressed request reaches every server on the link, so a server
 * that does not implement it must stay quiet rather than answer for the ones that
 * do; ISO 14229-1 names the codes this applies to. A refusal for a reason outside
 * that set - the request was understood and rejected on its merits - is still
 * sent, because it is this server's own answer rather than a comment on whether
 * the request was addressed to it.
 */
static bool uds_negative_suppressed(const nx_uds_server_t *srv, uint8_t ta_type,
                                    uint8_t nrc)
{
    if (ta_type != (uint8_t)NX_TP_TA_FUNCTIONAL) {
        return false;
    }
    /* A client that has been told to wait is waiting for an answer, so it gets
     * one even where the request's addressing would have kept the server quiet. */
    if (srv->run.txn.pending_sent) {
        return false;
    }
    switch (nrc) {
    case NX_UDS_NRC_SERVICE_NOT_SUPPORTED:
    case NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED:
    case NX_UDS_NRC_REQUEST_OUT_OF_RANGE:
    case NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION:
    case NX_UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* The transaction                                                    */
/* ------------------------------------------------------------------ */
/**
 * @brief  Call the running transaction's handler for a phase whose answer is not
 *         wanted.
 *
 * The reporting phases tell a handler what happened to its answer. Their return
 * value carries nothing, so it is discarded; a handler with nothing to do in them
 * does nothing.
 */
static void uds_notify(nx_uds_server_t *srv, nx_uds_phase_t phase)
{
    const nx_uds_service_t *row = uds_txn_row(srv);

    if (row == NULL) {
        return;              /* no handler was ever entered; none to report to */
    }
    srv->run.txn.ctx.phase = phase;
    (void)row->handler(&srv->run.txn.ctx, row->user);
}

/**
 * @brief  End the transaction, leaving the server able to take the next request.
 *
 * Only the transaction is cleared: the session, the security level and the quiet
 * timer outlive it, because they belong to the conversation rather than to one
 * request in it.
 */
static void uds_txn_end(nx_uds_server_t *srv)
{
    memset(&srv->run.txn, 0, sizeof(srv->run.txn));
    srv->run.txn.state = TXN_IDLE;
}

/**
 * @brief  Take a transaction that has an answer to the point of offering it.
 *
 * A response that will not be sent is not assembled into anything: the handler is
 * told so through SILENCE and the transaction ends, which is the same outcome as a
 * response the handler declined to give.
 */
static void uds_txn_answer_ready(nx_uds_server_t *srv, bool send)
{
    nx_uds_txn_t *txn = &srv->run.txn;

    if (!send) {
        uds_notify(srv, NX_UDS_PHASE_SILENCE);
        uds_txn_end(srv);
        return;
    }
    txn->answered = true;
    txn->state    = TXN_SENDING;
    uds_notify(srv, NX_UDS_PHASE_RESPONSE);
}

/**
 * @brief  Finish a transaction that a handler has refused.
 *
 * The refusal is expressed as a negative response, and whether that response
 * actually goes out depends on how the request was addressed.
 */
static void uds_txn_refuse(nx_uds_server_t *srv, uint8_t nrc)
{
    nx_uds_txn_t *txn = &srv->run.txn;

    uds_put_negative(srv, txn->ctx.sid, nrc);
    uds_txn_answer_ready(srv, !uds_negative_suppressed(srv, txn->ctx.ta_type, nrc));
}

/**
 * @brief  Apply what a handler returned from a phase that may produce an answer.
 *
 * A handler that is not finished keeps the transaction and is re-entered from the
 * next process(); one that has an answer takes the transaction to the offering
 * state; one that declines ends it. A positive answer is checked against the
 * capacity first, since a response longer than what can be conveyed has to be
 * reported as such rather than truncated.
 */
static void uds_apply_disposition(nx_uds_server_t *srv, nx_uds_disposition_t d)
{
    nx_uds_txn_t *txn = &srv->run.txn;

    switch (d) {
    case NX_UDS_DISPOSITION_PENDING:
        txn->state = TXN_RUNNING;
        return;

    case NX_UDS_DISPOSITION_NEGATIVE:
        uds_txn_refuse(srv, (txn->ctx.nrc != NX_UDS_NRC_NONE)
                                ? txn->ctx.nrc
                                : (uint8_t)NX_UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;

    case NX_UDS_DISPOSITION_NO_RESPONSE:
        uds_notify(srv, NX_UDS_PHASE_SILENCE);
        uds_txn_end(srv);
        return;

    case NX_UDS_DISPOSITION_DONE:
    default:
        break;
    }

    /* A positive answer. A handler that wrote past what the buffer holds has
     * already overrun it, so what is checkable here is what it reports having
     * written; the capacity it was given is the promise it was asked to keep. */
    if (txn->ctx.out_len > txn->ctx.out_cap) {
        uds_txn_refuse(srv, NX_UDS_NRC_RESPONSE_TOO_LONG);
        return;
    }
    if (txn->ctx.out_len == 0u) {
        /* Nothing was written, not even the response identifier the layer put
         * there: treat it as the handler having produced no answer at all. */
        uds_notify(srv, NX_UDS_PHASE_SILENCE);
        uds_txn_end(srv);
        return;
    }
    /* A positive response is what the suppression bit suppresses, and a
     * functionally addressed request is answered only by a service that says so. */
    bool send = !txn->ctx.suppress_pos;
    const nx_uds_service_t *row = uds_txn_row(srv);
    if (txn->ctx.ta_type == (uint8_t)NX_TP_TA_FUNCTIONAL &&
        (row == NULL || (row->flags & NX_UDS_SVC_ANSWER_FUNCTIONAL) == 0u)) {
        send = false;
    }
    uds_txn_answer_ready(srv, send);
}

/**
 * @brief  Enter the handler for a phase that may produce an answer, and apply
 *         what it returned.
 */
static void uds_run_handler(nx_uds_server_t *srv, nx_uds_phase_t phase)
{
    nx_uds_txn_t *txn = &srv->run.txn;
    const nx_uds_service_t *row = uds_txn_row(srv);

    if (row == NULL) {
        return;              /* nothing to dispatch into */
    }
    txn->ctx.phase = phase;
    txn->ctx.nrc   = NX_UDS_NRC_NONE;
    uds_apply_disposition(srv, row->handler(&txn->ctx, row->user));
}

/* ------------------------------------------------------------------ */
/* Keeping a slow transaction alive                                   */
/* ------------------------------------------------------------------ */
/**
 * @brief  Tell the client the answer is still coming, and buy another window.
 *
 * A handler that has not finished within the time an answer is allowed to take
 * would otherwise leave the client to time out. The notification is a negative
 * response carrying the code that means "received correctly, answer still
 * coming": it does not end the transaction, and the real answer follows it.
 *
 * The notification is built in the response buffer, which is where the eventual
 * answer will also be built - safe because the handler has not produced that
 * answer yet, and it will be rebuilt from scratch when it does.
 *
 * A notification the carrier would not take extends the window anyway and is
 * offered again on the next call, since a transmit path that is briefly full
 * should cost a window rather than the transaction. But the client is only
 * actually waiting if one of them arrived: when a whole window passes with none of
 * them accepted, the client is timing out on silence and the transaction has
 * failed on the link rather than pended.
 *
 * @return true if the carrier took it.
 */
static bool uds_emit_pending(nx_uds_server_t *srv)
{
    nx_uds_txn_t *txn = &srv->run.txn;
    uint8_t buf[NX_UDS_NEG_RSP_LEN];

    buf[0] = NX_UDS_NEG_RSP_SID;
    buf[1] = txn->ctx.sid;
    buf[2] = (uint8_t)NX_UDS_NRC_RESPONSE_PENDING;

    /* Nothing is carrying this conversation yet, which is not distinguishable from a
     * carrier that will not take anything: either way the notification did not go. */
    bool taken = (srv->cfg.out_fn != NULL)
                 && srv->cfg.out_fn(srv->cfg.out_user, txn->ctx.link, buf,
                                    NX_UDS_NEG_RSP_LEN, txn->ctx.ta_type);
    if (!taken) {
        /* Offered again on the next call. The client is waiting on the window that
         * has just run out, so the retrying is bounded by one more of them. */
        if (!txn->pend_stuck) {
            txn->pend_stuck   = true;
            txn->pend_give_up = uds_now(srv) + srv->cfg.p2_star_us;
        }
        return false;
    }
    txn->pend_stuck = false;

    /* Having said the answer is coming, the server owes one. A request that asked
     * for silence gave up that right the moment it was told to wait: the client is
     * now holding a transaction open for an answer, and withholding it would leave
     * that client waiting for its own timeout. The same applies to the refusals
     * that go unsent on a functionally addressed request. */
    txn->ctx.suppress_pos = false;
    txn->pending_sent     = true;

    if (txn->pend_count < 0xFFu) {
        txn->pend_count++;
    }
    txn->deadline = uds_now(srv) + srv->cfg.p2_star_us - srv->cfg.p2_adjust_us;
    return true;
}

/** @brief The whole-transaction limit this service is held to. */
static uint32_t uds_txn_p4(const nx_uds_server_t *srv)
{
    const nx_uds_service_t *row = uds_txn_row(srv);
    uint32_t p4 = (row != NULL) ? row->p4_us : 0u;
    return (p4 != 0u) ? p4 : srv->cfg.p4_us;
}

/** @brief How often this service may say its answer is still coming. */
static uint8_t uds_txn_max_pending(const nx_uds_server_t *srv)
{
    const nx_uds_service_t *row = uds_txn_row(srv);
    uint8_t n = (row != NULL) ? row->max_pending : 0u;
    return (n != 0u) ? n : srv->cfg.max_pending;
}

/** @brief What to answer a transaction that ran past its limit. */
static uint8_t uds_txn_p4_nrc(const nx_uds_server_t *srv)
{
    const nx_uds_service_t *row = uds_txn_row(srv);
    uint8_t nrc = (row != NULL) ? row->p4_nrc : (uint8_t)NX_UDS_NRC_NONE;
    if (nrc != NX_UDS_NRC_NONE) {
        return nrc;
    }
    return (srv->cfg.p4_nrc != NX_UDS_NRC_NONE)
               ? srv->cfg.p4_nrc
               : (uint8_t)NX_UDS_NRC_BUSY_REPEAT_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
void nx_uds_server_process(nx_uds_server_t *srv)
{
    if (srv == NULL) {
        return;
    }

    /* A conversation that has gone quiet loses its session, and with it whatever
     * it had unlocked. The transaction in flight is left alone: it was accepted
     * in the session that was active then, and dropping it mid-answer would
     * leave the client with no reply at all. */
    if (srv->run.s3_armed && uds_expired(uds_now(srv), srv->run.s3_deadline)) {
        uds_enter_session(srv, NX_UDS_SESSION_DEFAULT);
    }

    if (srv->run.txn.state == TXN_IDLE) {
        return;
    }

    /* Two limits end a transaction that will not finish: the time it is allowed
     * to take, and how often it may say it is not finished yet. The second is what
     * keeps a handler that pends on a short interval from filling the link with
     * notifications for the whole of the first limit. Either way the client is told
     * to ask again rather than left waiting, and the handler is told the
     * transaction was given up on so that whatever it started can be unwound.
     *
     * A transaction whose answer is already with the carrier is not cut off here:
     * the answer exists, and the only thing outstanding is the carrier's word that
     * it went out. */
    if (srv->run.txn.state == TXN_RUNNING &&
        (uds_expired(uds_now(srv), srv->run.txn.started + uds_txn_p4(srv)) ||
         srv->run.txn.pend_count >= uds_txn_max_pending(srv))) {
        uds_notify(srv, NX_UDS_PHASE_ABORT);
        uds_txn_refuse(srv, uds_txn_p4_nrc(srv));
        /* fall through: the refusal is offered to the carrier below */
    }

    if (srv->run.txn.state == TXN_RUNNING) {
        /* The answer is late. Say so once per window, then re-enter the handler:
         * a handler that finishes this time round answers immediately, and the
         * notification just sent costs nothing. */
        if (uds_expired(uds_now(srv), srv->run.txn.deadline)) {
            (void)uds_emit_pending(srv);
        }
        /* Nothing has reached the client for a whole window, so it is timing out
         * on silence: the transaction has failed on the link rather than pended. */
        if (srv->run.txn.pend_stuck &&
            uds_expired(uds_now(srv), srv->run.txn.pend_give_up)) {
            srv->run.txn.ctx.result = (uint8_t)NX_TP_N_ERROR;
            uds_notify(srv, NX_UDS_PHASE_LINK_ERROR);
            uds_txn_end(srv);
            return;
        }
        uds_run_handler(srv, NX_UDS_PHASE_RESUME);
    }

    if (srv->run.txn.state == TXN_SENDING) {
        nx_uds_txn_t *txn = &srv->run.txn;
        if (srv->cfg.out_fn != NULL
            && srv->cfg.out_fn(srv->cfg.out_user, txn->ctx.link, srv->cfg.out_buf,
                               txn->ctx.out_len, txn->ctx.ta_type)) {
            /* Handed over. The transaction is not finished: it ends when the
             * carrier reports what became of it, or when it stops reporting for
             * long enough that P4 gives up on it. */
            txn->state    = TXN_AWAIT;
            txn->deadline = uds_now(srv) + uds_txn_p4(srv);
        } else if (uds_expired(uds_now(srv), txn->started + uds_txn_p4(srv))) {
            /* The carrier never took it and the transaction is out of time.
             * Nothing can be sent, so the handler is told and it ends. */
            uds_notify(srv, NX_UDS_PHASE_ABORT);
            uds_txn_end(srv);
        }
        return;
    }

    if (srv->run.txn.state == TXN_AWAIT &&
        uds_expired(uds_now(srv), srv->run.txn.deadline)) {
        /* The carrier took the answer and never said what happened to it. The
         * answer may well have gone out, so this is reported as a link failure
         * rather than as a refusal, and nothing further is sent. */
        srv->run.txn.ctx.result = (uint8_t)NX_TP_N_ERROR;
        uds_notify(srv, NX_UDS_PHASE_LINK_ERROR);
        uds_txn_end(srv);
    }
}

bool nx_uds_server_init(nx_uds_server_t *srv, const nx_uds_server_cfg_t *cfg)
{
    if (srv == NULL || cfg == NULL) {
        return false;
    }
    if (cfg->services == NULL || cfg->services_count == 0u) {
        return false;
    }
    /* The table is checked once here rather than on every request, and what is
     * checked is what the dispatch path then relies on without re-testing. */
    for (uint16_t i = 0u; i < cfg->services_count; i++) {
        const nx_uds_service_t *row = &cfg->services[i];

        /* A row without a handler would be dispatched into and called through a
         * null pointer. */
        if (row->handler == NULL) {
            return false;
        }
        /* An identifier from the response ranges cannot be a service: the
         * positive response to it would not be an identifier at all. */
        if (uds_is_response_sid(row->sid)) {
            return false;
        }
        /* A row available in no session is a service that can never be reached,
         * which is more likely a mask left unset than an intent. */
        if (row->session_mask == 0u) {
            return false;
        }
        /* Every request carries an identifier, so no service can ask for less
         * than that byte. */
        if (row->min_len < 1u) {
            return false;
        }
        if (row->max_len != 0u && row->max_len < row->min_len) {
            return false;
        }
        /* The sub-function byte is read without a bounds test of its own, so a
         * row that has one must require it to be present. */
        if ((row->flags & NX_UDS_SVC_HAS_SUB_FUNCTION) != 0u && row->min_len < 2u) {
            return false;
        }
        /* Masks per sub-function are read at the same indices as the
         * sub-functions themselves, so one without the other has no meaning. */
        if (row->sub_session_masks != NULL &&
            (row->subs == NULL || row->subs_count == 0u)) {
            return false;
        }
    }
    if (cfg->get_us == NULL) {
        return false;
    }
    /* No out_fn is checked for here. A server whose answers are carried by something
     * that has to be given the server's address to attach itself cannot name that
     * something while it is still being initialised, so the output may be installed
     * afterwards with nx_uds_server_set_output. Until one is installed the server
     * holds every answer instead of sending it, which is the same state a carrier
     * that is not taking anything puts it in. */
    /* A transaction outlives the call that started it, so the request it is
     * answering has to be somewhere that lasts as long. */
    if (cfg->req_buf == NULL || cfg->req_buf_size < 1u) {
        return false;
    }
    /* Every refusal is three bytes, and a server that cannot express one has no
     * way to answer a request it does not implement. */
    if (cfg->out_buf == NULL || cfg->out_buf_size < NX_UDS_NEG_RSP_LEN) {
        return false;
    }
    /* A response capacity larger than the buffer would promise room that does not
     * exist - including to a handler reading out_cap to size its answer. */
    if (cfg->max_resp_apdu != 0u && cfg->max_resp_apdu > cfg->out_buf_size) {
        return false;
    }

    srv->cfg = *cfg;
    /* Resolve "0 means default" once, so the hot paths read a plain value. */
    if (srv->cfg.p2_us == 0u) {
        srv->cfg.p2_us = NX_UDS_SERVER_DEFAULT_P2_US;
    }
    if (srv->cfg.p2_star_us == 0u) {
        srv->cfg.p2_star_us = NX_UDS_SERVER_DEFAULT_P2_STAR_US;
    }
    if (srv->cfg.p4_us == 0u) {
        srv->cfg.p4_us = NX_UDS_SERVER_DEFAULT_P4_US;
    }
    if (srv->cfg.s3_us == 0u) {
        srv->cfg.s3_us = NX_UDS_SERVER_DEFAULT_S3_US;
    }
    if (srv->cfg.max_pending == 0u) {
        srv->cfg.max_pending = NX_UDS_SERVER_DEFAULT_MAX_PENDING;
    }
    /* The head start is subtracted from a window, so one at least as long as the
     * window would put the notification before the request it answers. */
    if (srv->cfg.p2_adjust_us >= srv->cfg.p2_us ||
        srv->cfg.p2_adjust_us >= srv->cfg.p2_star_us) {
        return false;
    }
    if (srv->cfg.max_resp_apdu == 0u) {
        srv->cfg.max_resp_apdu = srv->cfg.out_buf_size;
    }

    memset(&srv->run, 0, sizeof(srv->run));
    srv->run.session   = NX_UDS_SESSION_DEFAULT;
    srv->run.sec_level = 0u;
    srv->run.s3_armed  = false;      /* the default session has no quiet timer */
    srv->run.txn.state = TXN_IDLE;
    return true;
}

nx_uds_server_ret_t nx_uds_server_confirm(nx_uds_server_t *srv, uint8_t link,
                                          uint8_t result)
{
    if (srv == NULL) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    if (link != srv->cfg.link) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    /* Only a transaction whose answer is with the carrier has an outcome to
     * hear. A carrier that confirms twice, or confirms one that already timed
     * out, is told there was nothing to confirm rather than allowed to disturb
     * whatever is running now. */
    if (srv->run.txn.state != TXN_AWAIT) {
        return NX_UDS_SERVER_ERR_STATE;
    }

    srv->run.txn.ctx.result = result;
    if (result == (uint8_t)NX_TP_N_OK) {
        uds_notify(srv, NX_UDS_PHASE_CONFIRM);
    } else {
        uds_notify(srv, NX_UDS_PHASE_LINK_ERROR);
    }
    uds_txn_end(srv);
    return NX_UDS_SERVER_OK;
}

bool nx_uds_server_is_busy(const nx_uds_server_t *srv)
{
    return (srv != NULL) && (srv->run.txn.state != TXN_IDLE);
}

uint8_t nx_uds_server_session(const nx_uds_server_t *srv)
{
    return (srv != NULL) ? srv->run.session : (uint8_t)NX_UDS_SESSION_DEFAULT;
}

uint8_t nx_uds_server_sec_level(const nx_uds_server_t *srv)
{
    return (srv != NULL) ? srv->run.sec_level : 0u;
}

void nx_uds_server_timing(const nx_uds_server_t *srv, uint32_t *p2,
                          uint32_t *p2_star)
{
    if (srv == NULL) {
        return;
    }
    if (p2 != NULL) {
        *p2 = srv->cfg.p2_us;
    }
    if (p2_star != NULL) {
        *p2_star = srv->cfg.p2_star_us;
    }
}

uint32_t nx_uds_server_now(const nx_uds_server_t *srv)
{
    if (srv == NULL) {
        return 0u;
    }
    return uds_now(srv);
}

void nx_uds_server_apdu_limits(const nx_uds_server_t *srv, uint32_t *req,
                               uint32_t *rsp)
{
    if (srv == NULL) {
        return;
    }
    if (req != NULL) {
        *req = srv->cfg.max_req_apdu;
    }
    if (rsp != NULL) {
        *rsp = srv->cfg.max_resp_apdu;
    }
}

nx_uds_server_ret_t nx_uds_server_set_output(nx_uds_server_t *srv,
                                            nx_uds_output_fn fn, void *user)
{
    if (srv == NULL || fn == NULL) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    srv->cfg.out_fn   = fn;
    srv->cfg.out_user = user;
    return NX_UDS_SERVER_OK;
}

nx_uds_server_ret_t nx_uds_server_set_session(nx_uds_server_t *srv, uint8_t session)
{
    /* A session past what a mask reaches would be entered and then match no row,
     * leaving a server that answers nothing and cannot be asked to leave. */
    if (srv == NULL || !NX_UDS_SESSION_IN_RANGE(session)) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    uds_enter_session(srv, session);
    return NX_UDS_SERVER_OK;
}

nx_uds_server_ret_t nx_uds_server_set_sec_level(nx_uds_server_t *srv, uint8_t level)
{
    if (srv == NULL) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    srv->run.sec_level = level;
    return NX_UDS_SERVER_OK;
}

void nx_uds_server_touch(nx_uds_server_t *srv)
{
    if (srv == NULL) {
        return;
    }
    uds_arm_s3(srv);
}

/* ------------------------------------------------------------------ */
/* Accepting a request                                                */
/* ------------------------------------------------------------------ */
/**
 * @brief  The sessions a sub-function is available in.
 *
 * A service can be available in a session while one of its sub-functions is not,
 * so a row may carry a mask per sub-function. Without them every sub-function
 * follows the row.
 */
static uint32_t uds_sub_session_mask(const nx_uds_service_t *row, uint8_t idx)
{
    if (row->sub_session_masks == NULL) {
        return row->session_mask;
    }
    return row->sub_session_masks[idx];
}

/**
 * @brief  Find a sub-function among the ones a row lists.
 *
 * @return Its index, or @c subs_count when the row does not list it.
 */
static uint8_t uds_find_sub(const nx_uds_service_t *row, uint8_t sub)
{
    uint8_t i;
    for (i = 0u; i < row->subs_count; i++) {
        if (row->subs[i] == sub) {
            return i;
        }
    }
    return row->subs_count;
}

/**
 * @brief  Start a transaction for a request that has been accepted or refused.
 *
 * Every request that is going to be answered gets a transaction, a refusal
 * included, because the answer still has to be assembled and handed to the
 * carrier - and because a handler that has already been told about the request
 * must be told how it ended.
 */
static void uds_txn_begin(nx_uds_server_t *srv, uint16_t row, uint32_t len,
                          uint8_t ta_type, uint8_t link)
{
    nx_uds_txn_t *txn = &srv->run.txn;
    uint32_t cap = srv->cfg.out_buf_size;

    if (srv->cfg.max_resp_apdu < cap) {
        cap = srv->cfg.max_resp_apdu;
    }

    memset(txn, 0, sizeof(*txn));
    txn->row     = row;
    txn->started = uds_now(srv);
    txn->deadline = txn->started + srv->cfg.p2_us - srv->cfg.p2_adjust_us;

    txn->ctx.req      = srv->cfg.req_buf;
    txn->ctx.req_len  = len;
    txn->ctx.sid      = srv->cfg.req_buf[0];
    txn->ctx.ta_type  = ta_type;
    txn->ctx.link     = link;
    txn->ctx.session  = srv->run.session;
    txn->ctx.sec_level = srv->run.sec_level;
    txn->ctx.out      = srv->cfg.out_buf;
    txn->ctx.out_cap  = cap;
    txn->ctx.out_len  = 0u;
    txn->state        = TXN_RUNNING;
}

/**
 * @brief  Refuse a request outright: start a transaction only to carry the
 *         refusal, with no handler behind it.
 *
 * A refusal decided before a handler was ever entered has no handler to report to,
 * so the row it names is the one it was looked up in - which the reporting phases
 * then reach only if that row exists. Where no row was found, the transaction
 * carries no handler at all.
 */
static void uds_refuse_before_dispatch(nx_uds_server_t *srv, uint16_t row,
                                       uint32_t len, uint8_t ta_type, uint8_t link,
                                       uint8_t nrc)
{
    uds_txn_begin(srv, row, len, ta_type, link);
    /* No handler ran, so nothing is reported to one: the refusal is assembled and
     * the transaction goes straight to offering it. */
    uds_put_negative(srv, srv->run.txn.ctx.sid, nrc);
    if (uds_negative_suppressed(srv, ta_type, nrc)) {
        uds_txn_end(srv);
        return;
    }
    srv->run.txn.answered = true;
    srv->run.txn.state    = TXN_SENDING;
}

nx_uds_server_ret_t nx_uds_server_indicate(nx_uds_server_t *srv, const uint8_t *req,
                                           uint32_t len, uint8_t ta_type, uint8_t link)
{
    if (srv == NULL || req == NULL || len == 0u) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    if (link != srv->cfg.link) {
        return NX_UDS_SERVER_ERR_PARAM;
    }
    /* One transaction at a time, and the one that is running keeps its place. */
    if (srv->run.txn.state != TXN_IDLE) {
        return NX_UDS_SERVER_ERR_BUSY;
    }
    /* An answer that has come back around is not a request. Answering it is what
     * makes two servers on one functional address refuse each other forever. */
    if (!srv->cfg.accept_rsp_range_sid && uds_is_response_sid(req[0])) {
        return NX_UDS_SERVER_ERR_PARAM;
    }

    /* The request is kept for as long as the transaction runs, so it is copied:
     * whatever delivered it may reuse its storage the moment this returns. A
     * request too long to keep is one this server does not accept. */
    if (len > srv->cfg.req_buf_size ||
        (srv->cfg.max_req_apdu != 0u && len > srv->cfg.max_req_apdu)) {
        /* The identifier is still needed to name what is being refused, and it is
         * the one byte that is certainly there. */
        srv->cfg.req_buf[0] = req[0];
        uds_refuse_before_dispatch(srv, srv->cfg.services_count, 1u, ta_type, link,
                                   NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return NX_UDS_SERVER_OK;
    }
    memcpy(srv->cfg.req_buf, req, len);

    /* Accepting a request is what keeps the conversation alive, so the quiet
     * timer restarts here rather than when an answer is produced: a handler that
     * takes many cycles must not let the session lapse underneath it. */
    uds_arm_s3(srv);

    uint16_t row_idx = uds_find_row(srv, req[0]);
    if (row_idx >= srv->cfg.services_count) {
        uds_refuse_before_dispatch(srv, srv->cfg.services_count, len, ta_type, link,
                                   NX_UDS_NRC_SERVICE_NOT_SUPPORTED);
        return NX_UDS_SERVER_OK;
    }
    const nx_uds_service_t *row = &srv->cfg.services[row_idx];

    /* The service exists but not now. Asked before anything about the request's
     * own shape, so a service that is out of session answers the same way however
     * well or badly formed the request was. */
    if (!uds_row_in_session(row, srv->run.session)) {
        uds_refuse_before_dispatch(srv, row_idx, len, ta_type, link,
                                   NX_UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);
        return NX_UDS_SERVER_OK;
    }

    /* Security next, and before anything is said about sub-functions or lengths:
     * telling a client which sub-functions exist, or how long the request should
     * have been, is telling it about a service it has not unlocked. */
    if (row->sec_level != 0u && srv->run.sec_level < row->sec_level) {
        uds_refuse_before_dispatch(srv, row_idx, len, ta_type, link,
                                   NX_UDS_NRC_SECURITY_ACCESS_DENIED);
        return NX_UDS_SERVER_OK;
    }

    /* The length window, which must be checked before any byte past the
     * identifier is read. init has already established that a row carrying a
     * sub-function asks for at least two bytes, so passing this check is what
     * makes the sub-function byte safe to read below. */
    if (len < row->min_len || (row->max_len != 0u && len > row->max_len)) {
        uds_refuse_before_dispatch(srv, row_idx, len, ta_type, link,
                                   NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return NX_UDS_SERVER_OK;
    }

    uds_txn_begin(srv, row_idx, len, ta_type, link);
    nx_uds_txn_t *txn = &srv->run.txn;

    if ((row->flags & NX_UDS_SVC_HAS_SUB_FUNCTION) != 0u) {
        /* The request for silence rides in the top bit of the sub-function byte,
         * and it is stripped off before the value is compared against anything:
         * comparing the raw byte is what makes a request that asks for silence
         * look like a sub-function nobody implements. */
        txn->ctx.has_sub      = true;
        txn->ctx.suppress_pos = NX_UDS_SUPPRESSES_POS_RSP(srv->cfg.req_buf[1]);
        txn->ctx.sub          = NX_UDS_SUB_FUNCTION(srv->cfg.req_buf[1]);

        if (row->subs != NULL && row->subs_count != 0u) {
            uint8_t idx = uds_find_sub(row, txn->ctx.sub);
            if (idx >= row->subs_count) {
                uds_txn_refuse(srv, NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED);
                return NX_UDS_SERVER_OK;
            }
            /* The sub-function exists; whether it is available now is a separate
             * question, and asking it in this order is what keeps an unknown
             * sub-function from being reported as one that is merely out of
             * session. */
            if ((uds_sub_session_mask(row, idx) &
                 NX_UDS_SESSION_BIT(srv->run.session)) == 0u) {
                uds_txn_refuse(srv,
                    NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION);
                return NX_UDS_SERVER_OK;
            }
        }
    }

    /* Everything the table can decide has been decided. The response identifier
     * is written for the handler, so the one place that knows how a positive
     * response is named is here. */
    txn->ctx.out[0]  = NX_UDS_SID_TO_POS_RSP(txn->ctx.sid);
    txn->ctx.out_len = 1u;

    uds_run_handler(srv, NX_UDS_PHASE_REQUEST);
    return NX_UDS_SERVER_OK;
}
