/**
 * @file    nx_uds_sec.c
 * @brief   Implementation of the 0x27 seed/key exchange.
 */
#include <string.h>

#include "nx_uds_sec.h"

/** @brief Length of the answer to a key: the identifier and the echo, nothing more. */
#define SEC_KEY_RSP_LEN 2u

/**
 * @brief  Whether a deadline has been reached, safely across the clock wrapping.
 *
 * @param  now      The time now.
 * @param  deadline The time in question.
 * @return true when now is at or past it.
 */
static bool sec_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/**
 * @brief  The level a sub-function belongs to.
 *
 * Both sub-functions of a pair name the same level, the odd one asking and the even
 * one answering, so the level follows from either.
 *
 * @param  sub Sub-function, with the suppression bit already removed.
 * @return The level, or 0 where the sub-function names none.
 */
static uint8_t sec_level_of(uint8_t sub)
{
    if (sub == 0u || sub > NX_UDS_SEC_KEY_SUB(NX_UDS_SEC_MAX_LEVEL)) {
        return 0u;
    }
    return (uint8_t)((sub + 1u) / 2u);
}

/**
 * @brief  Whether a sub-function is the one asking for a seed.
 * @param  sub Sub-function, with the suppression bit already removed.
 * @return true for the odd one of a pair.
 */
static bool sec_is_seed_request(uint8_t sub)
{
    return (sub & 1u) != 0u;
}

/**
 * @brief  The declaration of a level, or NULL where the product has no such level.
 *
 * @param  sec   Handle.
 * @param  level Level number.
 * @return The entry describing it.
 */
static const nx_uds_sec_level_t *sec_find_level(const nx_uds_sec_t *sec,
                                               uint8_t level)
{
    uint8_t i;

    for (i = 0u; i < sec->cfg.levels_count; i++) {
        if (sec->cfg.levels[i].level == level) {
            return &sec->cfg.levels[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Setting up                                                        */
/* ------------------------------------------------------------------ */
bool nx_uds_sec_init(nx_uds_sec_t *sec, const nx_uds_sec_cfg_t *cfg)
{
    uint8_t i;
    uint8_t j;

    if (sec == NULL || cfg == NULL) {
        return false;
    }
    if (cfg->srv == NULL || cfg->levels == NULL || cfg->levels_count == 0u
        || cfg->seed_fn == NULL || cfg->verify_fn == NULL
        || cfg->seed_buf == NULL || cfg->seed_buf_size == 0u) {
        return false;
    }

    for (i = 0u; i < cfg->levels_count; i++) {
        const nx_uds_sec_level_t *lv = &cfg->levels[i];

        if (lv->level == 0u || lv->level > NX_UDS_SEC_MAX_LEVEL) {
            return false;    /* no pair of sub-functions names it */
        }
        if (lv->seed_len == 0u || lv->key_len == 0u) {
            return false;
        }
        /* A seed longer than there is room for would be truncated on its way to
         * the store and judged against a different seed than was sent. */
        if (lv->seed_len > cfg->seed_buf_size) {
            return false;
        }
        for (j = 0u; j < i; j++) {
            if (cfg->levels[j].level == lv->level) {
                return false;   /* two declarations, one of them unreachable */
            }
        }
    }

    memset(sec, 0, sizeof(*sec));
    sec->cfg = *cfg;
    if (sec->cfg.max_attempts == 0u) {
        sec->cfg.max_attempts = NX_UDS_SEC_DEFAULT_ATTEMPTS;
    }
    if (sec->cfg.delay_us == 0u) {
        sec->cfg.delay_us = NX_UDS_SEC_DEFAULT_DELAY_US;
    }
    return true;
}

void nx_uds_sec_get_lockout(const nx_uds_sec_t *sec, uint8_t *attempts,
                            bool *waiting, uint32_t *remaining)
{
    if (sec == NULL) {
        return;
    }
    if (attempts != NULL) {
        *attempts = sec->run.attempts;
    }
    if (waiting != NULL) {
        *waiting = sec->run.waiting;
    }
    if (remaining != NULL) {
        uint32_t left = 0u;

        if (sec->run.waiting) {
            uint32_t now = nx_uds_server_now(sec->cfg.srv);

            if (!sec_reached(now, sec->run.wait_until)) {
                left = sec->run.wait_until - now;
            }
        }
        *remaining = left;
    }
}

bool nx_uds_sec_set_lockout(nx_uds_sec_t *sec, uint8_t attempts, bool waiting,
                            uint32_t remaining)
{
    if (sec == NULL) {
        return false;
    }
    sec->run.attempts = attempts;
    sec->run.waiting  = waiting;
    if (waiting) {
        sec->run.wait_until = nx_uds_server_now(sec->cfg.srv) + remaining;
    } else {
        sec->run.wait_until = 0u;
    }
    /* Whatever seed was outstanding when the state was stored is not in this
     * store, so nothing is outstanding now. */
    sec->run.seed_level = 0u;
    sec->run.seed_len   = 0u;
    return true;
}

/* ------------------------------------------------------------------ */
/* Asking for a seed                                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief  Answer the odd sub-function of a pair.
 *
 * @param  sec   Handle.
 * @param  ctx   Transaction.
 * @param  lv    The level's declaration.
 * @param  level Its number.
 * @return What to do with the request.
 */
static nx_uds_disposition_t sec_do_seed(nx_uds_sec_t *sec, nx_uds_ctx_t *ctx,
                                        const nx_uds_sec_level_t *lv,
                                        uint8_t level)
{
    uint32_t seed_len = 0u;

    if (ctx->out_cap < 2u + lv->seed_len) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* A seed the client asked not to be told is a seed it cannot compute against,
     * so none is issued and none is remembered. A key arriving afterwards is out of
     * sequence, which is what it is: there is no exchange under way. */
    if (ctx->suppress_pos) {
        sec->run.seed_level = 0u;
        sec->run.seed_len   = 0u;
        ctx->out[1]  = ctx->sub;
        ctx->out_len = 2u;
        return NX_UDS_DISPOSITION_DONE;
    }

    if (ctx->sec_level == level) {
        /* Already in. The answer is a seed of zeros, as long as the level's seed
         * always is, which tells the client there is nothing to compute without
         * changing the shape of what it parses. Nothing is outstanding, so a key
         * offered against it is out of sequence rather than wrong. */
        sec->run.seed_level = 0u;
        sec->run.seed_len   = 0u;
        memset(&ctx->out[2], 0, lv->seed_len);
        ctx->out[1]  = ctx->sub;
        ctx->out_len = 2u + lv->seed_len;
        return NX_UDS_DISPOSITION_DONE;
    }

    if (!sec->cfg.seed_fn(sec->cfg.user, level, ctx->req + 2u, ctx->req_len - 2u,
                          sec->cfg.seed_buf, sec->cfg.seed_buf_size, &seed_len)) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    /* The declared length is what the answer promises and what a client reads off
     * it, so a seed of another length is not answerable. */
    if (seed_len != lv->seed_len) {
        sec->run.seed_level = 0u;
        sec->run.seed_len   = 0u;
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    sec->run.seed_level = level;
    sec->run.seed_len   = seed_len;
    memcpy(&ctx->out[2], sec->cfg.seed_buf, seed_len);
    ctx->out[1]  = ctx->sub;
    ctx->out_len = 2u + seed_len;
    return NX_UDS_DISPOSITION_DONE;
}

/* ------------------------------------------------------------------ */
/* Presenting a key                                                  */
/* ------------------------------------------------------------------ */
/**
 * @brief  Answer the even sub-function of a pair.
 *
 * @param  sec   Handle.
 * @param  ctx   Transaction.
 * @param  lv    The level's declaration.
 * @param  level Its number.
 * @return What to do with the request.
 */
static nx_uds_disposition_t sec_do_key(nx_uds_sec_t *sec, nx_uds_ctx_t *ctx,
                                       const nx_uds_sec_level_t *lv,
                                       uint8_t level)
{
    /* A key means something only against a seed this level is waiting on. Offered
     * with nothing outstanding, or against the seed of another level, it is not a
     * wrong key but a request at the wrong point, and is not counted: the count
     * exists to stop guessing, and nothing was guessed. */
    if (sec->run.seed_level != level) {
        ctx->nrc = NX_UDS_NRC_REQUEST_SEQUENCE_ERROR;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    /* The level's key is exactly this long. A shorter one judged as-is would let a
     * client narrow the key a byte at a time. */
    if (ctx->req_len != 2u + lv->key_len) {
        ctx->nrc = NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (ctx->out_cap < SEC_KEY_RSP_LEN) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (!sec->cfg.verify_fn(sec->cfg.user, level, sec->cfg.seed_buf,
                            sec->run.seed_len, ctx->req + 2u, lv->key_len)) {
        /* Spend the seed. A key judged against it has had its one chance, and a
         * second attempt starts by asking for a new one. */
        sec->run.seed_level = 0u;
        sec->run.seed_len   = 0u;

        if (sec->run.attempts < 0xFFu) {
            sec->run.attempts++;
        }
        if (sec->run.attempts >= sec->cfg.max_attempts) {
            /* The attempt that reaches the limit is the one told so, and starts the
             * wait. Every request during it hears about the wait instead. */
            sec->run.waiting    = true;
            sec->run.wait_until = nx_uds_server_now(sec->cfg.srv)
                                  + sec->cfg.delay_us;
            ctx->nrc = NX_UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS;
        } else {
            ctx->nrc = NX_UDS_NRC_INVALID_KEY;
        }
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* Right. Spend the seed so the same key cannot be presented again, and clear the
     * count: a client that got in is not one that was guessing. */
    sec->run.seed_level = 0u;
    sec->run.seed_len   = 0u;
    sec->run.attempts   = 0u;

    (void)nx_uds_server_set_sec_level(sec->cfg.srv, level);

    ctx->out[1]  = ctx->sub;
    ctx->out_len = SEC_KEY_RSP_LEN;

    if (sec->cfg.granted_fn != NULL) {
        sec->cfg.granted_fn(sec->cfg.user, level);
    }
    return NX_UDS_DISPOSITION_DONE;
}

/* ------------------------------------------------------------------ */
/* The service                                                       */
/* ------------------------------------------------------------------ */
nx_uds_disposition_t nx_uds_svc_security_access(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_sec_t *sec = (nx_uds_sec_t *)user;
    const nx_uds_sec_level_t *lv;
    uint8_t level;

    if (sec == NULL || sec->cfg.srv == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    /* Nothing here outlives the answer: the unlock is recorded as the answer is
     * assembled, and the seed store is the module's own. */
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }

    /* The wait is answered before anything else is looked at, so that a client
     * serving it learns nothing about which levels exist or what they expect. */
    if (sec->run.waiting) {
        if (!sec_reached(nx_uds_server_now(sec->cfg.srv), sec->run.wait_until)) {
            ctx->nrc = NX_UDS_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED;
            return NX_UDS_DISPOSITION_NEGATIVE;
        }
        /* Served. The count goes back to nothing, which is what makes the wait the
         * price of guessing rather than the end of it. */
        sec->run.waiting  = false;
        sec->run.attempts = 0u;
    }

    level = sec_level_of(ctx->sub);
    lv    = (level != 0u) ? sec_find_level(sec, level) : NULL;
    if (lv == NULL) {
        /* A sub-function naming a level the product does not offer. The row's own
         * list normally catches this first; a row listing more than the exchange
         * declares reaches here. */
        ctx->nrc = NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (sec_is_seed_request(ctx->sub)) {
        return sec_do_seed(sec, ctx, lv, level);
    }
    return sec_do_key(sec, ctx, lv, level);
}

