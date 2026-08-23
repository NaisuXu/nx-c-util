/**
 * @file    nx_uds_svc.c
 * @brief   Implementation of the handlers for the always-needed services.
 */
#include "nx_uds_svc.h"

/* ------------------------------------------------------------------ */
/* Shared                                                            */
/* ------------------------------------------------------------------ */
/**
 * @brief  Whether the answer that reached the link was this service's own.
 *
 * A phase reporting what became of the answer says nothing about which answer it
 * was: a refusal decided after the handler had written a positive response is
 * confirmed through the same phase. Both are in the one response buffer, so the
 * response identifier still there tells them apart.
 *
 * What acts on a request only once it has been answered asks this first, so that a
 * refusal cannot be mistaken for the acceptance it replaced.
 *
 * @param  ctx Transaction as the handler sees it.
 * @return true when the response in the buffer is this service's positive one.
 */
static bool svc_answered_positively(const nx_uds_ctx_t *ctx)
{
    return ctx->out_len >= 2u
           && ctx->out[0] == NX_UDS_SID_TO_POS_RSP(ctx->sid);
}

/* ------------------------------------------------------------------ */
/* 0x3E TesterPresent                                                */
/* ------------------------------------------------------------------ */
nx_uds_disposition_t nx_uds_svc_tester_present(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;

    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }

    /* The response identifier is already in place; what is left is the echo. The
     * sub-function reaches the handler with the suppression bit removed, so
     * echoing it cannot pass that bit back to the client as data. */
    ctx->out[1]  = ctx->sub;
    ctx->out_len = 2u;
    return NX_UDS_DISPOSITION_DONE;
}

/* ------------------------------------------------------------------ */
/* 0x10 DiagnosticSessionControl                                      */
/* ------------------------------------------------------------------ */
/** @brief Length of the response: identifier, session, and the two windows. */
#define SVC_SESSION_RSP_LEN 6u

/** @brief Largest value either published window fits in. */
#define SVC_WINDOW_MAX      0xFFFFu

/**
 * @brief  A window in microseconds as a count of the unit it is published in.
 *
 * Rounded up. Rounding down would publish a window shorter than the one being
 * kept to, and a client that believed it would give up while the server was still
 * inside the time it had announced.
 *
 * @param  us   The window.
 * @param  unit Microseconds per count: 1000 for the ordinary window, 10000 for the
 *              one a pending notification extends it to.
 * @return The count, saturated at what the field holds.
 */
static uint16_t svc_window_counts(uint32_t us, uint32_t unit)
{
    uint32_t counts = (us + unit - 1u) / unit;

    if (counts > SVC_WINDOW_MAX) {
        counts = SVC_WINDOW_MAX;
    }
    return (uint16_t)counts;
}

nx_uds_disposition_t nx_uds_svc_session_control(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_svc_session_cfg_t *cfg = (nx_uds_svc_session_cfg_t *)user;
    uint32_t p2      = 0u;
    uint32_t p2_star = 0u;
    uint16_t counts;
    uint8_t  nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;

    if (cfg == NULL || cfg->srv == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    switch (ctx->phase) {
    case NX_UDS_PHASE_REQUEST:
        break;

    case NX_UDS_PHASE_CONFIRM:
    case NX_UDS_PHASE_SILENCE:
        /* The session changes now, not when the request was accepted: the answer
         * describes the server as it was, and is carried by whatever the session
         * being left had established. An answer that was withheld still marks the
         * point the change belongs at, but only where what was withheld was the
         * acceptance rather than a refusal. */
        if (svc_answered_positively(ctx)) {
            (void)nx_uds_server_set_session(cfg->srv, ctx->sub);
        }
        return NX_UDS_DISPOSITION_DONE;

    default:
        return NX_UDS_DISPOSITION_DONE;
    }

    /* A session the masks cannot name would be entered and then match no row. The
     * row's own list is what normally settles which sessions exist; a row naming
     * one from further up than a mask reaches is refused here rather than answered
     * positively and then not entered. */
    if (!NX_UDS_SESSION_IN_RANGE(ctx->sub)) {
        ctx->nrc = NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (cfg->allow_fn != NULL
        && !cfg->allow_fn(cfg->user, ctx->session, ctx->sub, &nrc)) {
        ctx->nrc = nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (ctx->out_cap < SVC_SESSION_RSP_LEN) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* What is published is what the server enforces, read from it so the two
     * cannot drift apart. */
    nx_uds_server_timing(cfg->srv, &p2, &p2_star);

    ctx->out[1] = ctx->sub;
    counts      = svc_window_counts(p2, NX_UDS_P2_RESOLUTION_US);
    ctx->out[2] = (uint8_t)(counts >> 8);
    ctx->out[3] = (uint8_t)counts;
    counts      = svc_window_counts(p2_star, NX_UDS_P2_STAR_RESOLUTION_US);
    ctx->out[4] = (uint8_t)(counts >> 8);
    ctx->out[5] = (uint8_t)counts;
    ctx->out_len = SVC_SESSION_RSP_LEN;
    return NX_UDS_DISPOSITION_DONE;
}

/* ------------------------------------------------------------------ */
/* 0x11 ECUReset                                                      */
/* ------------------------------------------------------------------ */
nx_uds_disposition_t nx_uds_svc_ecu_reset(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_svc_reset_cfg_t *cfg = (nx_uds_svc_reset_cfg_t *)user;
    uint32_t len = 2u;
    uint8_t  nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;

    if (cfg == NULL || cfg->do_fn == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    switch (ctx->phase) {
    case NX_UDS_PHASE_REQUEST:
        break;

    case NX_UDS_PHASE_CONFIRM:
    case NX_UDS_PHASE_SILENCE:
        /* The reset happens once the answer is behind it, or once it is settled
         * that the answer asked for was silence. Either way the client has what it
         * is going to get before the product acts on the request. */
        if (svc_answered_positively(ctx)) {
            cfg->do_fn(cfg->user, ctx->sub);
        }
        return NX_UDS_DISPOSITION_DONE;

    default:
        /* Including the answer having failed to reach the link, where resetting
         * would be a reboot the client never asked for and never heard accepted. */
        return NX_UDS_DISPOSITION_DONE;
    }

    if (cfg->allow_fn != NULL && !cfg->allow_fn(cfg->user, ctx->sub, &nrc)) {
        ctx->nrc = nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* One reset type answers with how long the power stays down; the rest answer
     * with the type alone. Sending that byte where it does not belong, or leaving
     * it out where it does, is a response a strict client rejects. */
    if (ctx->sub == (uint8_t)NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN) {
        len = 3u;
    }
    if (ctx->out_cap < len) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    ctx->out[1] = ctx->sub;
    if (len == 3u) {
        ctx->out[2] = cfg->power_down_time;
    }
    ctx->out_len = len;
    return NX_UDS_DISPOSITION_DONE;
}

