/**
 * @file    nx_uds_svc_session_example.c
 * @brief   Exercises the services every server needs: 0x10, 0x11, 0x3E, 0x27.
 *
 * One server, one table holding the four library handlers and nothing else, and a
 * carrier that records what was sent. What is checked is the part a product cannot
 * check for itself: that the session changes only once its answer is away, that a
 * reset does not happen when the answer never left, that the windows announced are
 * the ones enforced, and that running out of attempts cannot be escaped by asking
 * for a session.
 */
#include "nx_middleware_examples.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/middleware/nx_uds_svc_sec.h"
#include "src/middleware/nx_uds_svc_session.h"

#define LINK_ID     1u
#define P2_US       50000u      /* 50 ms  -> 50 counts of 1 ms   */
#define P2_STAR_US  5000000u    /* 5 s    -> 500 counts of 10 ms */
#define P4_US       20000000u
#define S3_US       5000000u

#define SEED_LEN    4u
#define KEY_LEN     4u

/* ---- the clock ---- */
static uint32_t g_now;
static uint32_t mock_now(void) { return g_now; }

/* ---- the carrier ---- */
static uint8_t  g_sent[64];
static uint32_t g_sent_len;
static bool     g_sent_any;
static bool     g_refuse;

static bool mock_out(void *user, uint8_t link, const uint8_t *rsp, uint32_t len,
                     uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;

    if (g_refuse) {
        return false;
    }
    assert(len <= sizeof(g_sent));
    memcpy(g_sent, rsp, len);
    g_sent_len = len;
    g_sent_any = true;
    return true;
}

static void wire_reset(void)
{
    g_sent_len = 0u;
    g_sent_any = false;
    memset(g_sent, 0, sizeof(g_sent));
}

static void show(const char *what)
{
    uint32_t i;

    printf("  %-24s", what);
    if (!g_sent_any) {
        printf("(nothing sent)\n");
        return;
    }
    printf("[%u]", (unsigned)g_sent_len);
    for (i = 0u; i < g_sent_len; i++) {
        printf(" %02X", g_sent[i]);
    }
    printf("\n");
}

/* ---- what the product does ---- */
static uint8_t  g_reset_done;      /* the reset type carried out, 0 for none */
static unsigned g_reset_count;
static uint8_t  g_granted_level;
static bool     g_refuse_session;  /* the product will not enter one right now */

static nx_uds_server_t g_srv;
static nx_uds_svc_sec_t    g_sec;

static void app_do_reset(void *user, uint8_t reset_type)
{
    (void)user;
    g_reset_done = reset_type;
    g_reset_count++;
}

static bool app_allow_session(void *user, uint8_t from, uint8_t to, uint8_t *nrc)
{
    (void)user; (void)from; (void)to;

    if (g_refuse_session) {
        *nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return false;
    }
    return true;
}

/* A seed that is not random, which is what makes it checkable here and what makes
 * it unfit for a product. */
static bool app_seed(void *user, uint8_t level, const uint8_t *record,
                     uint32_t record_len, uint8_t *seed, uint32_t seed_cap,
                     uint32_t *seed_len)
{
    (void)user; (void)record; (void)record_len;
    assert(seed_cap >= SEED_LEN);

    seed[0] = 0xA0u;
    seed[1] = 0xA1u;
    seed[2] = 0xA2u;
    seed[3] = (uint8_t)(0xB0u + level);
    *seed_len = SEED_LEN;
    return true;
}

/* The key is the seed with every byte complemented. */
static bool app_verify(void *user, uint8_t level, const uint8_t *seed,
                       uint32_t seed_len, const uint8_t *key, uint32_t key_len)
{
    uint32_t i;

    (void)user; (void)level;
    if (seed_len != key_len) {
        return false;
    }
    for (i = 0u; i < key_len; i++) {
        if (key[i] != (uint8_t)~seed[i]) {
            return false;
        }
    }
    return true;
}

static void app_granted(void *user, uint8_t level)
{
    (void)user;
    g_granted_level = level;
}

/* ---- the table ---- */
static nx_uds_svc_session_cfg_t g_session_cfg = {
    .srv      = &g_srv,
    .allow_fn = app_allow_session,
    .user     = NULL
};

static nx_uds_svc_session_reset_cfg_t g_reset_cfg = {
    .do_fn           = app_do_reset,
    .allow_fn        = NULL,
    .user            = NULL,
    .power_down_time = 12u
};

static const nx_uds_svc_sec_level_t g_levels[] = {
    { 1u, SEED_LEN, KEY_LEN }
};

/* Every session the server is asked to enter. */
static const uint8_t g_session_subs[] = {
    NX_UDS_SESSION_DEFAULT, NX_UDS_SESSION_PROGRAMMING, NX_UDS_SESSION_EXTENDED,
    /* A value from the range a product numbers its own sessions in, listed here to
     * show what happens to one the masks cannot reach. */
    0x40u
};

/* Only the reset types the product performs. */
static const uint8_t g_reset_subs[] = {
    NX_UDS_RESET_HARD, NX_UDS_RESET_SOFT,
    NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN
};

static const uint8_t g_tp_subs[] = { NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB };

/* Both sub-functions of the one level offered. */
static const uint8_t g_sec_subs[] = {
    NX_UDS_SVC_SEC_SEED_SUB(1u), NX_UDS_SVC_SEC_KEY_SUB(1u)
};

static const nx_uds_service_t g_services[] = {
    {   /* 0x10: reachable from every session, since a server that cannot be asked
         * to leave a session can only be power-cycled out of it. */
        .sid = NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler = nx_uds_svc_session_control,
        .user = &g_session_cfg,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .sec_level = 0u,
        .subs = g_session_subs, .subs_count = 4u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 2u
    },
    {   /* 0x11: a reset is not something the default session offers. */
        .sid = NX_UDS_SID_ECU_RESET,
        .handler = nx_uds_svc_session_ecu_reset,
        .user = &g_reset_cfg,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_NON_DEFAULT,
        .sec_level = 0u,
        .subs = g_reset_subs, .subs_count = 3u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 2u
    },
    {   /* 0x3E: every session, and left unanswered when broadcast so that a link
         * full of servers does not answer at once. */
        .sid = NX_UDS_SID_TESTER_PRESENT,
        .handler = nx_uds_svc_session_tester_present,
        .user = NULL,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .sec_level = 0u,
        .subs = g_tp_subs, .subs_count = 1u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 2u
    },
    {   /* 0x27: not from the default session, which is how a request to unlock
         * from it is refused as out of session rather than by this module. */
        .sid = NX_UDS_SID_SECURITY_ACCESS,
        .handler = nx_uds_svc_security_access,
        .user = &g_sec,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_NON_DEFAULT,
        .sec_level = 0u,
        .subs = g_sec_subs, .subs_count = 2u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 2u + KEY_LEN
    }
};

/* ---- driving it ---- */
static uint8_t g_req_buf[16];
static uint8_t g_out_buf[32];

static void setup(void)
{
    nx_uds_server_cfg_t cfg;
    nx_uds_svc_sec_cfg_t    scfg;
    static uint8_t      seed_store[SEED_LEN];

    g_now = 1000u;
    g_refuse = false;
    g_refuse_session = false;
    g_reset_done = 0u;
    g_reset_count = 0u;
    g_granted_level = 0u;
    wire_reset();

    memset(&scfg, 0, sizeof(scfg));
    scfg.srv           = &g_srv;
    scfg.levels        = g_levels;
    scfg.levels_count  = 1u;
    scfg.seed_fn       = app_seed;
    scfg.verify_fn     = app_verify;
    scfg.granted_fn    = app_granted;
    scfg.seed_buf      = seed_store;
    scfg.seed_buf_size = sizeof(seed_store);
    scfg.max_attempts  = 2u;
    scfg.delay_us      = 100000u;    /* 100 ms, so the example need not wait */
    assert(nx_uds_svc_sec_init(&g_sec, &scfg));

    memset(&cfg, 0, sizeof(cfg));
    cfg.services       = g_services;
    cfg.services_count = (uint16_t)(sizeof(g_services) / sizeof(g_services[0]));
    cfg.out_fn         = mock_out;
    cfg.req_buf        = g_req_buf;
    cfg.req_buf_size   = sizeof(g_req_buf);
    cfg.out_buf        = g_out_buf;
    cfg.out_buf_size   = sizeof(g_out_buf);
    cfg.link           = LINK_ID;
    cfg.get_us         = mock_now;
    cfg.p2_us          = P2_US;
    cfg.p2_star_us     = P2_STAR_US;
    cfg.p4_us          = P4_US;
    cfg.s3_us          = S3_US;
    assert(nx_uds_server_init(&g_srv, &cfg));
}

/**
 * @brief  Hand over a request and let it run to wherever it stops.
 *
 * A request that is answered leaves the answer with the carrier and waits to be
 * told it arrived, so this reports the answer whether it was confirmed or not; a
 * caller that wants the confirmation does it itself.
 */
static void submit(const uint8_t *req, uint32_t len, uint8_t ta_type)
{
    wire_reset();
    (void)nx_uds_server_indicate(&g_srv, req, len, ta_type, LINK_ID);
    /* Nothing leaves from inside indicate(): it accepts the request and runs the
     * handler, and the answer is offered to the carrier from process(). */
    nx_uds_server_process(&g_srv);
}

/** @brief Tell the server the answer arrived. */
static void confirmed(void)
{
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, (uint8_t)NX_TP_N_OK);
}

/* ------------------------------------------------------------------ */
/* 0x10 DiagnosticSessionControl                                      */
/* ------------------------------------------------------------------ */
static void demo_session(void)
{
    static const uint8_t to_prog[] = { 0x10u, NX_UDS_SESSION_PROGRAMMING };
    static const uint8_t to_ext[]  = { 0x10u, NX_UDS_SESSION_EXTENDED };
    static const uint8_t to_ext_quiet[] = {
        0x10u, NX_UDS_SESSION_EXTENDED | NX_UDS_SUPPRESS_POS_RSP_BIT
    };
    static const uint8_t to_nowhere[] = { 0x10u, 0x7Fu };

    printf("1. entering a session\n");
    setup();
    submit(to_prog, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("answer ->");
    /* The windows announced are the ones the server keeps: 50 ms as 50 counts of
     * 1 ms, 5 s as 500 counts of 10 ms. */
    assert(g_sent_len == 6u);
    assert(g_sent[0] == 0x50u && g_sent[1] == NX_UDS_SESSION_PROGRAMMING);
    assert(((uint32_t)g_sent[2] << 8 | g_sent[3]) == P2_US / 1000u);
    assert(((uint32_t)g_sent[4] << 8 | g_sent[5]) == P2_STAR_US / 10000u);

    /* Not yet: the answer is a message of the session being left, and is still
     * with the carrier. */
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);
    printf("  %-24sstill %u\n", "before confirm ->",
           (unsigned)nx_uds_server_session(&g_srv));
    confirmed();
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_PROGRAMMING);
    printf("  %-24snow %u\n", "after confirm ->",
           (unsigned)nx_uds_server_session(&g_srv));

    printf("2. an answer that never left\n");
    g_refuse = true;
    submit(to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("nothing away ->");
    /* A carrier that will not take the answer is offered it again on every pump,
     * so what ends the attempt is the transaction running out of time. */
    g_now += P4_US + 1u;
    nx_uds_server_process(&g_srv);
    g_refuse = false;
    assert(!nx_uds_server_is_busy(&g_srv));
    /* The session asked for was never entered, since nothing told the client it
     * had been. Waiting the transaction out outlasted the quiet timer too, so what
     * is left is the default session rather than the one being spoken in. */
    assert(nx_uds_server_session(&g_srv) != NX_UDS_SESSION_EXTENDED);
    printf("  %-24s%u, not extended\n", "session ->",
           (unsigned)nx_uds_server_session(&g_srv));

    printf("3. a session the product will not enter, and one it does not have\n");
    setup();
    g_refuse_session = true;
    submit(to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x22 ->");
    assert(g_sent_len == 3u && g_sent[0] == 0x7Fu && g_sent[2] == 0x22u);
    confirmed();
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);

    g_refuse_session = false;
    submit(to_nowhere, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x12 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x12u);
    confirmed();

    /* A session numbered past what a mask reaches is refused rather than
     * answered and then not entered, which would leave the client believing
     * it had a session the server never took. */
    {
        static const uint8_t to_own[] = { 0x10u, 0x40u };

        submit(to_own, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
        show("out of reach ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x12u);
        confirmed();
        assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);
    }

    printf("4. a session change the client asked not to hear about\n");
    submit(to_ext_quiet, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("silent ->");
    assert(!g_sent_any);
    /* Silence was what was asked for, not a refusal, so the change stands. */
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    printf("  %-24s%u\n", "session ->",
           (unsigned)nx_uds_server_session(&g_srv));
}

/* ------------------------------------------------------------------ */
/* 0x11 ECUReset                                                      */
/* ------------------------------------------------------------------ */
static void demo_reset(void)
{
    static const uint8_t hard[]  = { 0x11u, NX_UDS_RESET_HARD };
    static const uint8_t soft[]  = { 0x11u, NX_UDS_RESET_SOFT };
    static const uint8_t rapid[] = {
        0x11u, NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN
    };
    static const uint8_t key_off[] = { 0x11u, NX_UDS_RESET_KEY_OFF_ON };
    static const uint8_t quiet[] = {
        0x11u, NX_UDS_RESET_HARD | NX_UDS_SUPPRESS_POS_RSP_BIT
    };
    static const uint8_t to_ext[] = { 0x10u, NX_UDS_SESSION_EXTENDED };

    printf("5. a reset happens after its answer, not before\n");
    setup();
    submit(to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();

    submit(hard, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("answer ->");
    assert(g_sent_len == 2u && g_sent[0] == 0x51u
           && g_sent[1] == NX_UDS_RESET_HARD);
    assert(g_reset_count == 0u);
    printf("  %-24snot yet\n", "before confirm ->");
    confirmed();
    assert(g_reset_count == 1u && g_reset_done == NX_UDS_RESET_HARD);
    printf("  %-24scarried out\n", "after confirm ->");

    printf("6. one reset type answers with the power-down time\n");
    submit(rapid, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("three bytes ->");
    assert(g_sent_len == 3u && g_sent[1] == 0x04u && g_sent[2] == 12u);
    confirmed();
    submit(soft, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("two bytes ->");
    assert(g_sent_len == 2u);
    confirmed();

    printf("7. a reset type the product does not perform\n");
    submit(key_off, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x12 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x12u);
    confirmed();
    assert(g_reset_count == 3u);   /* hard, rapid, soft; nothing from this one */

    printf("8. an answer that never left means no reset\n");
    g_refuse = true;
    submit(hard, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("nothing away ->");
    /* A carrier that will not take the answer is offered it again on every pump,
     * so what ends the attempt is the transaction running out of time. */
    g_now += P4_US + 1u;
    nx_uds_server_process(&g_srv);
    g_refuse = false;
    assert(!nx_uds_server_is_busy(&g_srv));
    assert(g_reset_count == 3u);
    printf("  %-24sstill %u resets\n", "count ->", g_reset_count);

    printf("9. a reset the client asked not to hear about still happens\n");
    /* Waiting out the last transaction also outlasted the quiet timer, so the
     * session it needs is entered again. */
    submit(to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    submit(quiet, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("silent ->");
    assert(!g_sent_any);
    assert(g_reset_count == 4u && g_reset_done == NX_UDS_RESET_HARD);
    printf("  %-24scarried out\n", "count ->");
}

/* ------------------------------------------------------------------ */
/* 0x3E TesterPresent                                                 */
/* ------------------------------------------------------------------ */
static void demo_tester_present(void)
{
    static const uint8_t tp[]       = { 0x3Eu, 0x00u };
    static const uint8_t tp_quiet[] = { 0x3Eu, 0x80u };
    static const uint8_t tp_wrong[] = { 0x3Eu, 0x01u };
    static const uint8_t to_ext[]   = { 0x10u, NX_UDS_SESSION_EXTENDED };

    printf("10. keeping the session\n");
    setup();
    submit(to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();

    submit(tp, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("answer ->");
    assert(g_sent_len == 2u && g_sent[0] == 0x7Eu && g_sent[1] == 0x00u);
    confirmed();

    printf("11. the form that asks for no answer\n");
    submit(tp_quiet, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("silent ->");
    assert(!g_sent_any);
    /* The point of the request is that it arrived, and the server counts any
     * request it accepts as the client still being there. */
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);

    /* Nearly all the way to the quiet limit, then this request, then the rest of
     * the way: the session survives what would otherwise have ended it. */
    g_now += S3_US - 1000u;
    submit(tp_quiet, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    g_now += S3_US - 1000u;
    nx_uds_server_process(&g_srv);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    printf("  %-24skept the session\n", "quiet form ->");

    printf("12. a sub-function 0x3E does not have\n");
    submit(tp_wrong, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x12 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x12u);
    confirmed();

    printf("13. broadcast, and what a whole link would answer\n");
    submit(tp, 2u, (uint8_t)NX_TP_TA_FUNCTIONAL);
    show("nothing ->");
    /* Every server on the link received it, and a link where each answered would
     * be a link of collisions. The session is kept all the same. */
    assert(!g_sent_any);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
}

/* ------------------------------------------------------------------ */
/* 0x27 SecurityAccess                                                */
/* ------------------------------------------------------------------ */
static const uint8_t g_ask_seed[] = { 0x27u, 0x01u };
static const uint8_t g_to_ext[]   = { 0x10u, NX_UDS_SESSION_EXTENDED };

/** @brief Present a key derived from the seed just answered, or a wrong one. */
static void present_key(bool correct)
{
    uint8_t req[2u + KEY_LEN];
    uint32_t i;

    req[0] = 0x27u;
    req[1] = 0x02u;
    for (i = 0u; i < KEY_LEN; i++) {
        req[2u + i] = (uint8_t)~g_sent[2u + i];
    }
    if (!correct) {
        req[2] = (uint8_t)(req[2] + 1u);
    }
    submit(req, sizeof(req), (uint8_t)NX_TP_TA_PHYSICAL);
}

static void demo_security(void)
{
    printf("14. unlocking a level\n");
    setup();
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();

    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("seed ->");
    assert(g_sent_len == 2u + SEED_LEN && g_sent[0] == 0x67u
           && g_sent[1] == 0x01u);
    confirmed();

    present_key(true);
    show("unlocked ->");
    assert(g_sent_len == 2u && g_sent[0] == 0x67u && g_sent[1] == 0x02u);
    assert(nx_uds_server_sec_level(&g_srv) == 1u);
    assert(g_granted_level == 1u);
    confirmed();

    printf("15. a level already unlocked answers a seed of zeros\n");
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("zeros ->");
    assert(g_sent_len == 2u + SEED_LEN);
    assert(g_sent[2] == 0u && g_sent[3] == 0u && g_sent[4] == 0u
           && g_sent[5] == 0u);
    confirmed();
    /* Nothing is outstanding against it, so a key is a request at the wrong
     * point rather than a wrong key. */
    present_key(true);
    show("0x24 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x24u);
    confirmed();

    printf("16. a key with no seed behind it\n");
    setup();
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    {
        static const uint8_t key[] = { 0x27u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u };

        submit(key, sizeof(key), (uint8_t)NX_TP_TA_PHYSICAL);
        show("0x24 ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x24u);
        confirmed();
    }

    printf("17. a seed is spent once a key has been judged against it\n");
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(false);
    show("0x35 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x35u);
    confirmed();
    /* The same seed cannot be tried again, right key or not. */
    present_key(true);
    show("0x24 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x24u);
    confirmed();
    assert(nx_uds_server_sec_level(&g_srv) == 0u);
}

/** @brief Ask for a seed and answer it wrongly, one counted attempt. */
static void one_wrong_attempt(void)
{
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(false);
    confirmed();
}

static void demo_lockout(void)
{
    printf("18. running out of attempts\n");
    setup();
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();

    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(false);
    show("first wrong ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x35u);
    confirmed();

    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(false);
    show("second wrong ->");
    /* The attempt that reaches the limit is the one told so. */
    assert(g_sent_len == 3u && g_sent[2] == 0x36u);
    confirmed();

    /* Nothing of the service is available now, not even asking for a seed, and no
     * callback is consulted to find that out. */
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x37 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x37u);
    confirmed();

    printf("19. asking for a session is not a way out of the wait\n");
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("session granted ->");
    assert(g_sent_len == 6u);
    confirmed();
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("still 0x37 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x37u);
    confirmed();

    printf("20. the wait ends and the count goes back to nothing\n");
    g_now += 100000u;
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("seed again ->");
    assert(g_sent_len == 2u + SEED_LEN && g_sent[0] == 0x67u);
    confirmed();
    /* Two more wrong keys are needed to lock it again, which is what the count
     * having been cleared means. */
    one_wrong_attempt();
    assert(g_sent[2] == 0x35u);
    one_wrong_attempt();
    assert(g_sent[2] == 0x36u);
    printf("  %-24stwo attempts again\n", "count ->");

    printf("21. carrying the wait across a restart\n");
    {
        uint8_t  attempts = 0u;
        bool     waiting  = false;
        uint32_t remaining = 0u;

        nx_uds_svc_sec_get_lockout(&g_sec, &attempts, &waiting, &remaining);
        assert(attempts == 2u && waiting && remaining > 0u);
        printf("  %-24s%u attempts, %u us left\n", "stored ->",
               (unsigned)attempts, (unsigned)remaining);

        /* What a product does with those three values is store them; what it does
         * on the way back up is hand them over again. */
        setup();
        submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
        confirmed();
        submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
        show("fresh start ->");
        assert(g_sent[0] == 0x67u);
        confirmed();

        assert(nx_uds_svc_sec_set_lockout(&g_sec, attempts, waiting, remaining));
        submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
        show("restored ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x37u);
        confirmed();
    }

    printf("22. unlocking from the default session\n");
    setup();
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    show("0x7F ->");
    /* Refused by the row, before this module is reached. */
    assert(g_sent_len == 3u && g_sent[2] == 0x7Fu);
    confirmed();
}

/* ------------------------------------------------------------------ */
/* The two together                                                   */
/* ------------------------------------------------------------------ */
static void demo_relock(void)
{
    printf("23. a session relocks what was unlocked\n");
    setup();
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(true);
    confirmed();
    assert(nx_uds_server_sec_level(&g_srv) == 1u);
    printf("  %-24slevel %u\n", "unlocked ->",
           (unsigned)nx_uds_server_sec_level(&g_srv));

    /* The session asked for is the one already active, and it relocks all the
     * same: a request to enter a session is answered by a server with nothing
     * unlocked, whichever session that was. */
    submit(g_to_ext, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    assert(nx_uds_server_sec_level(&g_srv) == 0u);
    printf("  %-24ssame session, level %u\n", "re-entered ->",
           (unsigned)nx_uds_server_sec_level(&g_srv));

    printf("24. going quiet drops the session and relocks\n");
    submit(g_ask_seed, 2u, (uint8_t)NX_TP_TA_PHYSICAL);
    confirmed();
    present_key(true);
    confirmed();
    assert(nx_uds_server_sec_level(&g_srv) == 1u);

    g_now += S3_US + 1u;
    wire_reset();
    nx_uds_server_process(&g_srv);
    assert(!g_sent_any);      /* nothing announces it; the client notices by asking */
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);
    assert(nx_uds_server_sec_level(&g_srv) == 0u);
    printf("  %-24ssession %u, level %u\n", "after the quiet ->",
           (unsigned)nx_uds_server_session(&g_srv),
           (unsigned)nx_uds_server_sec_level(&g_srv));
}

int nx_uds_svc_session_example_run(void)
{
    printf("=== nx_uds_svc_session example ===\n");

    demo_session();
    demo_reset();
    demo_tester_present();
    demo_security();
    demo_lockout();
    demo_relock();

    printf("nx_uds_svc_session example passed\n\n");
    return 0;
}

