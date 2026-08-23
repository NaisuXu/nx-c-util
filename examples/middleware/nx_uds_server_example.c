/**
 * @file    nx_uds_server_example.c
 * @brief   Usage example for nx_uds_server: a diagnostic server, with no carrier.
 *
 * The server is a pure upper layer: a request goes in as bytes plus how it was
 * addressed, and a response comes back through a callback. Nothing here speaks
 * CAN, ISO-TP or Modbus, which is the point - the same server code runs over any
 * of them, and over none of them at all, as here.
 *
 *      request bytes                                   response bytes
 *           |                                                ^
 *           | nx_uds_server_indicate()                       | out_fn callback
 *           v                                                |
 *        [ nx_uds_server ] --dispatch--> service table row ---+
 *           |                              (handler)
 *           | nx_uds_server_process()  drives timers, re-enters a slow handler,
 *           v                          offers the answer to the carrier
 *      nx_uds_server_confirm()         reports what became of it
 *
 * The example plays both the tester and the carrier: it feeds requests in, and
 * its out_fn records what the server tried to send so the assertions can inspect
 * it. A flag makes that carrier refuse, which is how the retry and link-failure
 * paths are exercised.
 *
 * Fourteen scenarios are exercised:
 *   1.  A service the table implements          -> its positive response
 *   2.  A service it does not                   -> 0x11 serviceNotSupported
 *   3.  A response identifier arriving          -> discarded, nothing sent
 *   4.  A request too short / too long          -> 0x13
 *   5.  A service out of the active session     -> 0x7F, before any other check
 *   6.  A locked service                        -> 0x33, telling nothing else
 *   7.  A sub-function nobody implements        -> 0x12
 *   8.  A sub-function out of session           -> 0x7E, from the per-sub masks
 *   9.  The suppression bit                     -> action taken, nothing sent
 *  10.  A functionally addressed refusal        -> the five codes stay unsent
 *  11.  A handler that needs several cycles     -> 0x78 then the real answer
 *  12.  A handler that never finishes           -> capped by count, then by time
 *  13.  A carrier that refuses, then accepts    -> the answer is offered again
 *  14.  Session and security lifecycle          -> S3 expiry and relocking
 *
 * All storage is static; the example self-checks with asserts and prints what the
 * server put on the wire.
 */
#include "nx_middleware_examples.h"
#include "src/middleware/nx_uds_server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The connection this server serves. An indication from any other is refused,
 * so one instance stays one conversation. */
#define LINK_ID 4u

/* Timings short enough to step through on the mock clock. */
#define P2_US       10000u
#define P2_STAR_US  20000u
#define P4_US       100000u
#define S3_US       50000u
#define MAX_PENDING 3u

/* Security levels this example's table asks for. */
#define SEC_LEVEL_PROGRAMMING 1u

/* Identifiers the example uses for its own test services. */
#define SID_ECHO      0x22u   /**< a service with no sub-function */
#define SID_SUBBED    0x31u   /**< a service with sub-functions */
#define SID_SLOW      0x2Eu   /**< a service that takes several cycles */
#define SID_STUCK     0x2Fu   /**< a service that never finishes */
#define SID_LOCKED    0x23u   /**< a service behind a security level */
#define SID_PROG_ONLY 0x28u   /**< a service only the programming session has */
#define SID_ABSENT    0x19u   /**< in no row, so the table refuses it */

/* ------------------------------------------------------------------ */
/* Mock plumbing: a clock the example steers, and a carrier it watches */
/* ------------------------------------------------------------------ */
static uint32_t g_clock_us;

static uint32_t mock_get_us(void)
{
    return g_clock_us;
}

/** @brief Advance the clock, as a main loop running for that long would. */
static void advance_us(uint32_t us)
{
    g_clock_us += us;
}

/**
 * @brief What the carrier saw. The example plays the carrier, so this is the wire.
 */
static struct {
    uint8_t  buf[64];   /**< The last response handed down */
    uint32_t len;       /**< Its length; 0 when nothing was handed down */
    uint8_t  link;      /**< Which connection it was for */
    uint8_t  ta_type;   /**< How the request it answers was addressed */
    unsigned count;     /**< How many responses have been handed down in all */
    bool     refuse;    /**< Whether the carrier is refusing to take anything */
} g_wire;

/** @brief Forget what the carrier saw, so the next assertion starts clean. */
static void wire_clear(void)
{
    g_wire.len   = 0u;
    g_wire.count = 0u;
}

/**
 * @brief  The server's output callback: what a binding shim would implement.
 *
 * A real one allocates a message and publishes it to the transport's queue, and
 * reports whether the queue took it. This one records the bytes and reports
 * whatever the example has asked it to.
 */
static bool mock_out(void *user, uint8_t link, const uint8_t *rsp, uint32_t len,
                     uint8_t ta_type)
{
    (void)user;
    if (g_wire.refuse) {
        return false;
    }
    assert(len <= sizeof(g_wire.buf));
    memcpy(g_wire.buf, rsp, len);
    g_wire.len     = len;
    g_wire.link    = link;
    g_wire.ta_type = ta_type;
    g_wire.count++;
    return true;
}

/** @brief Session changes the server reported, for the lifecycle scenario. */
static struct {
    uint8_t  from, to;
    unsigned count;
} g_session_log;

static void mock_session_change(void *user, uint8_t from, uint8_t to)
{
    (void)user;
    g_session_log.from = from;
    g_session_log.to   = to;
    g_session_log.count++;
}

/** @brief Print one response as it would appear on the wire. */
static void print_rsp(const char *tag)
{
    printf("  %-22s", tag);
    if (g_wire.len == 0u) {
        printf(" (nothing sent)\n");
        return;
    }
    printf(" [%u]", (unsigned)g_wire.len);
    for (uint32_t i = 0; i < g_wire.len; i++) {
        printf(" %02X", g_wire.buf[i]);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* The services this example's table implements                        */
/* ------------------------------------------------------------------ */
/** @brief What each handler records about the phases it was called in. */
static struct {
    unsigned request, resume, response, confirm, link_error, silence, abort;
} g_phases;

static void phase_note(nx_uds_phase_t phase)
{
    switch (phase) {
    case NX_UDS_PHASE_REQUEST:    g_phases.request++;    break;
    case NX_UDS_PHASE_RESUME:     g_phases.resume++;     break;
    case NX_UDS_PHASE_RESPONSE:   g_phases.response++;   break;
    case NX_UDS_PHASE_CONFIRM:    g_phases.confirm++;    break;
    case NX_UDS_PHASE_LINK_ERROR: g_phases.link_error++; break;
    case NX_UDS_PHASE_SILENCE:    g_phases.silence++;    break;
    case NX_UDS_PHASE_ABORT:      g_phases.abort++;      break;
    default: break;
    }
}

/**
 * @brief  A service that answers at once: it returns the request's data back.
 *
 * The layer has already put the positive response identifier in out[0] and set
 * out_len to 1, so a handler appends its own data from there.
 */
static nx_uds_disposition_t handle_echo(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;
    phase_note(ctx->phase);
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    uint32_t n = ctx->req_len - 1u;              /* everything after the SID */
    if (ctx->out_len + n > ctx->out_cap) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    memcpy(ctx->out + ctx->out_len, ctx->req + 1, n);
    ctx->out_len += n;
    return NX_UDS_DISPOSITION_DONE;
}

/** @brief A service with sub-functions: it echoes the stripped sub-function back. */
static nx_uds_disposition_t handle_subbed(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;
    phase_note(ctx->phase);
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    /* The sub-function the layer hands over has the suppression bit removed, so a
     * handler never has to know the bit exists. */
    ctx->out[ctx->out_len++] = ctx->sub;
    return NX_UDS_DISPOSITION_DONE;
}

/** @brief How many cycles the slow service still needs, counted down. */
static unsigned g_slow_left;

/**
 * @brief  A service whose work spans several cycles.
 *
 * It says it is not finished until its own counter runs out. The layer keeps the
 * transaction, tells the client the answer is coming when a window elapses, and
 * re-enters the handler on every process().
 */
static nx_uds_disposition_t handle_slow(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;
    phase_note(ctx->phase);
    if (ctx->phase != NX_UDS_PHASE_REQUEST && ctx->phase != NX_UDS_PHASE_RESUME) {
        return NX_UDS_DISPOSITION_DONE;
    }
    if (g_slow_left > 0u) {
        g_slow_left--;
        return NX_UDS_DISPOSITION_PENDING;
    }
    ctx->out[ctx->out_len++] = 0xA5u;            /* the answer, at last */
    return NX_UDS_DISPOSITION_DONE;
}

/** @brief A service that never finishes, so the layer has to give up on it. */
static nx_uds_disposition_t handle_stuck(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;
    phase_note(ctx->phase);
    return NX_UDS_DISPOSITION_PENDING;
}

/* ------------------------------------------------------------------ */
/* The service table: the whole of what this server supports           */
/* ------------------------------------------------------------------ */
/* Sub-functions the subbed service accepts, and the sessions each is available
 * in. The masks are what make one sub-function unavailable while the service
 * itself is reachable, which is the only way the layer can tell a sub-function
 * that is out of session from one that does not exist. */
static const uint8_t  g_subbed_subs[]  = {0x01u, 0x02u};
static const uint32_t g_subbed_masks[] = {
    NX_UDS_SESSION_MASK_ALL,            /* 0x01: always */
    NX_UDS_SESSION_MASK_PROGRAMMING     /* 0x02: only while programming */
};

static const nx_uds_service_t g_services[] = {
    {   /* answers at once, no sub-function, any length from 1 byte up */
        .sid = SID_ECHO, .session_mask = NX_UDS_SESSION_MASK_ALL,
        .min_len = 1u, .max_len = 8u, .handler = handle_echo,
    },
    {   /* carries a sub-function, so the layer strips the suppression bit */
        .sid = SID_SUBBED, .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs = g_subbed_subs, .subs_count = 2u,
        .sub_session_masks = g_subbed_masks,
        .min_len = 2u, .max_len = 2u, .handler = handle_subbed,
    },
    {   /* takes several cycles; capped by count sooner than by time */
        .sid = SID_SLOW, .session_mask = NX_UDS_SESSION_MASK_ALL,
        .min_len = 1u, .handler = handle_slow,
    },
    {   /* never finishes, and says so at most MAX_PENDING times */
        .sid = SID_STUCK, .session_mask = NX_UDS_SESSION_MASK_ALL,
        .min_len = 1u, .max_pending = MAX_PENDING, .handler = handle_stuck,
    },
    {   /* needs a security level nothing has unlocked yet */
        .sid = SID_LOCKED, .session_mask = NX_UDS_SESSION_MASK_ALL,
        .sec_level = SEC_LEVEL_PROGRAMMING,
        .min_len = 1u, .handler = handle_echo,
    },
    {   /* exists, but only while the programming session is active */
        .sid = SID_PROG_ONLY, .session_mask = NX_UDS_SESSION_MASK_PROGRAMMING,
        .min_len = 1u, .handler = handle_echo,
    },
};

#define SERVICE_COUNT (sizeof(g_services) / sizeof(g_services[0]))

/* The two buffers the server needs. The request is copied into the first, because
 * a transaction outlives the call that started it: a handler that takes several
 * cycles reads its request on each of them, and whatever delivered those bytes is
 * free to reuse its own storage as soon as indicate() returns. */
static uint8_t g_req_buf[64];
static uint8_t g_out_buf[32];

static nx_uds_server_t g_srv;

/** @brief Hand the server a physically addressed request. */
static nx_uds_server_ret_t request(const uint8_t *req, uint32_t len)
{
    return nx_uds_server_indicate(&g_srv, req, len, NX_TP_TA_PHYSICAL, LINK_ID);
}

/** @brief Hand the server a functionally addressed one. */
static nx_uds_server_ret_t request_func(const uint8_t *req, uint32_t len)
{
    return nx_uds_server_indicate(&g_srv, req, len, NX_TP_TA_FUNCTIONAL, LINK_ID);
}

/**
 * @brief  Run one request to completion: hand it over, drive it, confirm it.
 *
 * The shape a main loop has: process() moves the transaction and offers the
 * answer, and the carrier's report is what ends it.
 */
static void run_to_completion(void)
{
    nx_uds_server_process(&g_srv);
    if (g_wire.len != 0u) {
        (void)nx_uds_server_confirm(&g_srv, LINK_ID, NX_TP_N_OK);
    }
}

/* ------------------------------------------------------------------ */
/* 1. A service the table implements                                   */
/* ------------------------------------------------------------------ */
static void demo_positive_response(void)
{
    printf("1. a service the table implements\n");

    const uint8_t req[] = {SID_ECHO, 0xF1u, 0x90u};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);

    /* Nothing is sent from inside indicate(): the answer leaves from process(). */
    assert(g_wire.len == 0u);
    run_to_completion();

    print_rsp("positive ->");
    /* The positive response identifier is the request's with one bit added. */
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_ECHO));
    assert(g_wire.len == 3u);
    assert(g_wire.buf[1] == 0xF1u && g_wire.buf[2] == 0x90u);
    assert(g_wire.link == LINK_ID);
    /* The transaction ended when the carrier reported the answer had gone out. */
    assert(!nx_uds_server_is_busy(&g_srv));
    assert(g_phases.confirm == 1u);
}

/* ------------------------------------------------------------------ */
/* 2. A service the table does not implement                           */
/* ------------------------------------------------------------------ */
static void demo_service_not_supported(void)
{
    printf("2. a service the table does not implement\n");

    const uint8_t req[] = {SID_ABSENT, 0x02u};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();

    print_rsp("0x11 ->");
    /* Three bytes: the code marking a refusal, what is being refused, and why. */
    assert(g_wire.len == NX_UDS_NEG_RSP_LEN);
    assert(g_wire.buf[0] == NX_UDS_NEG_RSP_SID);
    assert(g_wire.buf[1] == SID_ABSENT);
    assert(g_wire.buf[2] == NX_UDS_NRC_SERVICE_NOT_SUPPORTED);
    assert(!nx_uds_server_is_busy(&g_srv));
}

/* ------------------------------------------------------------------ */
/* 3. A response identifier arriving as a request                      */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that an answer coming back around is not treated as a request.
 *
 * Two servers sharing a functional address would otherwise answer each other's
 * refusals indefinitely, since each would read the other's as a request for a
 * service it does not implement.
 */
static void demo_response_sid_ignored(void)
{
    printf("3. a response identifier arriving as a request\n");

    const uint8_t rsp_sid[] = {NX_UDS_NEG_RSP_SID, SID_ECHO,
                               NX_UDS_NRC_SERVICE_NOT_SUPPORTED};
    wire_clear();
    assert(request(rsp_sid, sizeof(rsp_sid)) == NX_UDS_SERVER_ERR_PARAM);
    nx_uds_server_process(&g_srv);

    print_rsp("refusal echoed ->");
    assert(g_wire.len == 0u);
    assert(!nx_uds_server_is_busy(&g_srv));

    /* A positive response identifier is refused the same way. */
    const uint8_t pos_sid[] = {NX_UDS_SID_TO_POS_RSP(SID_ECHO), 0x00u};
    assert(request(pos_sid, sizeof(pos_sid)) == NX_UDS_SERVER_ERR_PARAM);
    assert(g_wire.len == 0u);
    printf("  neither is answered; a shared functional address stays quiet\n");
}

/* ------------------------------------------------------------------ */
/* 4. A request whose length the service does not accept                */
/* ------------------------------------------------------------------ */
static void demo_bad_length(void)
{
    printf("4. a request whose length the service does not accept\n");

    /* The subbed service is exactly two bytes: one short has no sub-function. */
    const uint8_t too_short[] = {SID_SUBBED};
    wire_clear();
    assert(request(too_short, sizeof(too_short)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("too short ->");
    assert(g_wire.buf[2] == NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT);

    /* The echo service accepts up to 8 bytes; a ninth is one too many. */
    const uint8_t too_long[] = {SID_ECHO, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    wire_clear();
    assert(request(too_long, sizeof(too_long)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("too long ->");
    assert(g_wire.buf[1] == SID_ECHO);
    assert(g_wire.buf[2] == NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT);
}

/* ------------------------------------------------------------------ */
/* 5. A service the active session does not have                        */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that being out of session is answered before anything else is.
 *
 * The refusal is the same whether the request was well formed or not, so a client
 * probing a service it cannot reach learns only that it cannot reach it.
 */
static void demo_wrong_session(void)
{
    printf("5. a service the active session does not have\n");

    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);

    const uint8_t req[] = {SID_PROG_ONLY, 0x03u};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x7F ->");
    assert(g_wire.buf[2] == NX_UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);

    /* In the session that does have it, the same request is answered. */
    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_PROGRAMMING)
           == NX_UDS_SERVER_OK);
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("in session ->");
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_PROG_ONLY));

    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_DEFAULT)
           == NX_UDS_SERVER_OK);
}

/* ------------------------------------------------------------------ */
/* 6. A service behind a security level                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that a locked service says nothing but that it is locked.
 *
 * Security is judged before the length and the sub-function, so a client that has
 * not unlocked a service is not told how long its request should have been or
 * which sub-functions it has.
 */
static void demo_locked_service(void)
{
    printf("6. a service behind a security level\n");

    assert(nx_uds_server_sec_level(&g_srv) == 0u);

    const uint8_t req[] = {SID_LOCKED, 0x11u};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x33 ->");
    assert(g_wire.buf[2] == NX_UDS_NRC_SECURITY_ACCESS_DENIED);

    /* Unlocked, the same request reaches the handler. */
    assert(nx_uds_server_set_sec_level(&g_srv, SEC_LEVEL_PROGRAMMING)
           == NX_UDS_SERVER_OK);
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("unlocked ->");
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_LOCKED));

    assert(nx_uds_server_set_sec_level(&g_srv, 0u) == NX_UDS_SERVER_OK);
}

/* ------------------------------------------------------------------ */
/* 7. A sub-function the service does not have                          */
/* ------------------------------------------------------------------ */
static void demo_bad_sub_function(void)
{
    printf("7. a sub-function the service does not have\n");

    const uint8_t req[] = {SID_SUBBED, 0x7Fu};   /* not among {0x01, 0x02} */
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x12 ->");
    assert(g_wire.buf[2] == NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

/* ------------------------------------------------------------------ */
/* 8. A sub-function the active session does not have                   */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show the two sub-function refusals kept apart.
 *
 * A sub-function that exists but is out of session is a different answer from one
 * that does not exist, and telling them apart needs a session mask per
 * sub-function - the service itself is reachable in both cases.
 */
static void demo_sub_function_wrong_session(void)
{
    printf("8. a sub-function the active session does not have\n");

    /* 0x02 is programming-only, and the default session is active. */
    const uint8_t req[] = {SID_SUBBED, 0x02u};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x7E ->");
    assert(g_wire.buf[2] ==
           NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION);

    /* 0x01 is available in every session, so it answers here. */
    const uint8_t always[] = {SID_SUBBED, 0x01u};
    wire_clear();
    assert(request(always, sizeof(always)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x01 ->");
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_SUBBED));
    assert(g_wire.buf[1] == 0x01u);

    /* And in the session it belongs to, 0x02 answers too. */
    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_PROGRAMMING)
           == NX_UDS_SERVER_OK);
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x02 in session ->");
    assert(g_wire.buf[1] == 0x02u);
    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_DEFAULT)
           == NX_UDS_SERVER_OK);
}

/* ------------------------------------------------------------------ */
/* 9. A request that asks for no positive response                      */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show the suppression bit stripped before anything compares it.
 *
 * The bit rides in the top of the sub-function byte. Comparing the raw byte
 * against the accepted sub-functions is the classic mistake: the value with the
 * bit set is in no table, so a request asking for silence would be refused for a
 * sub-function that does not exist instead of being carried out quietly.
 */
static void demo_suppress_positive(void)
{
    printf("9. a request that asks for no positive response\n");

    const uint8_t req[] = {SID_SUBBED,
                           (uint8_t)(0x01u | NX_UDS_SUPPRESS_POS_RSP_BIT)};
    wire_clear();
    unsigned before = g_phases.request;
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);
    nx_uds_server_process(&g_srv);

    print_rsp("suppressed ->");
    /* The handler ran - the request was carried out - and nothing was sent. */
    assert(g_phases.request == before + 1u);
    assert(g_wire.len == 0u);
    /* No answer means no confirmation to wait for, so the transaction is over. */
    assert(!nx_uds_server_is_busy(&g_srv));
    printf("  the service ran; the answer was withheld, not refused\n");
}

/* ------------------------------------------------------------------ */
/* 10. A functionally addressed request that earns a refusal            */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show which refusals go unsent when a request reached every server.
 *
 * A request addressed to everybody is answered only by the servers it concerns, so
 * a refusal that amounts to "this was not for me" stays unsent - otherwise every
 * server on the link would answer at once. A refusal about the request's own
 * merits is still sent, because that is this server's own answer.
 */
static void demo_functional_suppression(void)
{
    printf("10. a functionally addressed request that earns a refusal\n");

    /* Not implemented here: this server is not the one being addressed. */
    const uint8_t absent[] = {SID_ABSENT, 0x01u};
    wire_clear();
    assert(request_func(absent, sizeof(absent)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x11 broadcast ->");
    assert(g_wire.len == 0u);

    /* A length that is wrong is wrong for every server, so it is answered. */
    const uint8_t short_req[] = {SID_SUBBED};
    wire_clear();
    assert(request_func(short_req, sizeof(short_req)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("0x13 broadcast ->");
    assert(g_wire.len == NX_UDS_NEG_RSP_LEN);
    assert(g_wire.buf[2] == NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT);
    assert(g_wire.ta_type == (uint8_t)NX_TP_TA_FUNCTIONAL);

    /* A positive answer to a broadcast is withheld unless the row asks for it,
     * since one request would otherwise draw an answer from every server. */
    const uint8_t echo[] = {SID_ECHO, 0x77u};
    wire_clear();
    assert(request_func(echo, sizeof(echo)) == NX_UDS_SERVER_OK);
    run_to_completion();
    print_rsp("positive broadcast ->");
    assert(g_wire.len == 0u);
    printf("  \"not for me\" stays quiet; \"this is malformed\" answers\n");
}

/* ------------------------------------------------------------------ */
/* 11. A handler that needs several cycles                              */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show the notification that keeps a slow answer alive.
 *
 * A handler that has not finished within the window an answer is allowed to take
 * would leave the client to time out. The layer says the answer is still coming,
 * which buys another window, and keeps re-entering the handler until it is done.
 */
static void demo_pending_then_answer(void)
{
    printf("11. a handler that needs several cycles\n");

    /* Enough cycles that the handler is still working when the window elapses:
     * one is spent by indicate() and one by each process() before it. */
    g_slow_left = 4u;
    const uint8_t req[] = {SID_SLOW};
    wire_clear();
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);

    /* Inside the first window nothing is said: the handler may yet finish. */
    nx_uds_server_process(&g_srv);
    assert(g_wire.len == 0u);
    assert(nx_uds_server_is_busy(&g_srv));

    /* Past it, the client is told the answer is coming. */
    advance_us(P2_US);
    nx_uds_server_process(&g_srv);
    print_rsp("0x78 ->");
    assert(g_wire.len == NX_UDS_NEG_RSP_LEN);
    assert(g_wire.buf[0] == NX_UDS_NEG_RSP_SID);
    assert(g_wire.buf[1] == SID_SLOW);
    assert(g_wire.buf[2] == NX_UDS_NRC_RESPONSE_PENDING);
    assert(nx_uds_server_is_busy(&g_srv));

    /* The handler finishes on the next cycle, and the real answer follows. */
    wire_clear();
    nx_uds_server_process(&g_srv);
    if (g_wire.len == 0u) {
        nx_uds_server_process(&g_srv);
    }
    print_rsp("answer ->");
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_SLOW));
    assert(g_wire.buf[1] == 0xA5u);
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, NX_TP_N_OK);
    assert(!nx_uds_server_is_busy(&g_srv));
    printf("  the wait was announced once, then the answer arrived\n");
}

/* ------------------------------------------------------------------ */
/* 12. A handler that never finishes                                    */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show a transaction given up on, and the handler told about it.
 *
 * Saying the answer is coming cannot go on forever: a handler that says it every
 * short interval would fill the link with notifications for as long as the whole
 * transaction is allowed to last. Two limits bound it - how often it may be said,
 * and how long the transaction may run - and whichever is reached first ends it.
 */
static void demo_pending_capped(void)
{
    printf("12. a handler that never finishes\n");

    const uint8_t req[] = {SID_STUCK};
    wire_clear();
    unsigned aborts = g_phases.abort;
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);

    /* Each elapsed window earns one notification, up to the row's limit. */
    unsigned pendings = 0u;
    for (unsigned i = 0; i < MAX_PENDING + 2u; i++) {
        advance_us(P2_STAR_US);
        wire_clear();
        nx_uds_server_process(&g_srv);
        if (g_wire.len != 0u &&
            g_wire.buf[2] == NX_UDS_NRC_RESPONSE_PENDING) {
            pendings++;
            continue;
        }
        break;
    }
    printf("  said \"still coming\" %u times (limit %u)\n", pendings, MAX_PENDING);
    assert(pendings <= MAX_PENDING);

    /* Then the transaction is given up on and the client told to ask again. */
    for (unsigned i = 0; i < 4u && g_wire.len == 0u; i++) {
        nx_uds_server_process(&g_srv);
    }
    print_rsp("gave up ->");
    assert(g_wire.buf[0] == NX_UDS_NEG_RSP_SID);
    assert(g_wire.buf[2] == NX_UDS_NRC_BUSY_REPEAT_REQUEST);
    /* The handler was told, so whatever it had started can be unwound. */
    assert(g_phases.abort == aborts + 1u);
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, NX_TP_N_OK);
    assert(!nx_uds_server_is_busy(&g_srv));
}

/* ------------------------------------------------------------------ */
/* 13. A carrier that refuses the answer, then takes it                 */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that an answer the carrier would not take is offered again.
 *
 * A transmit path that briefly fills up should cost the transaction the time it
 * takes to drain, not the answer.
 */
static void demo_carrier_backpressure(void)
{
    printf("13. a carrier that refuses the answer, then takes it\n");

    const uint8_t req[] = {SID_ECHO, 0x5Au};
    wire_clear();
    g_wire.refuse = true;
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_OK);

    /* Refused: nothing reaches the wire, and the answer is still owed. */
    nx_uds_server_process(&g_srv);
    nx_uds_server_process(&g_srv);
    assert(g_wire.len == 0u);
    assert(nx_uds_server_is_busy(&g_srv));
    printf("  carrier refusing: answer held, transaction alive\n");

    /* Draining lets the same answer through, unchanged. */
    g_wire.refuse = false;
    nx_uds_server_process(&g_srv);
    print_rsp("after drain ->");
    assert(g_wire.buf[0] == NX_UDS_SID_TO_POS_RSP(SID_ECHO));
    assert(g_wire.buf[1] == 0x5Au);
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, NX_TP_N_OK);
    assert(!nx_uds_server_is_busy(&g_srv));
}

/* ------------------------------------------------------------------ */
/* 14. Session and security lifecycle                                   */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show a session ending on its own, and what it takes with it.
 *
 * A conversation that goes quiet loses its session, and a level is unlocked only
 * for the session it was unlocked in - so entering any session locks the server,
 * including entering the one already active.
 */
static void demo_session_lifecycle(void)
{
    printf("14. session and security lifecycle\n");

    /* Entering a session and unlocking something in it. */
    g_session_log.count = 0u;
    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_EXTENDED)
           == NX_UDS_SERVER_OK);
    assert(nx_uds_server_set_sec_level(&g_srv, SEC_LEVEL_PROGRAMMING)
           == NX_UDS_SERVER_OK);
    assert(g_session_log.count == 1u);
    assert(g_session_log.to == NX_UDS_SESSION_EXTENDED);

    /* Asking again for the session already active relocks: the level was
     * unlocked for a session that has now been re-entered. */
    assert(nx_uds_server_set_session(&g_srv, NX_UDS_SESSION_EXTENDED)
           == NX_UDS_SERVER_OK);
    assert(nx_uds_server_sec_level(&g_srv) == 0u);
    printf("  re-entering the same session relocked it\n");

    /* A request keeps the session alive, so the quiet timer starts over. */
    assert(nx_uds_server_set_sec_level(&g_srv, SEC_LEVEL_PROGRAMMING)
           == NX_UDS_SERVER_OK);
    const uint8_t keepalive[] = {SID_ECHO, 0x01u};
    advance_us(S3_US - 1000u);
    wire_clear();
    assert(request(keepalive, sizeof(keepalive)) == NX_UDS_SERVER_OK);
    run_to_completion();
    advance_us(S3_US - 1000u);
    nx_uds_server_process(&g_srv);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    printf("  a request restarted the quiet timer\n");

    /* Left alone past it, the session ends and takes the unlocked level with it.
     * Nothing is sent: the client infers the drop from its own timer. */
    wire_clear();
    advance_us(S3_US + 1000u);
    nx_uds_server_process(&g_srv);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_DEFAULT);
    assert(nx_uds_server_sec_level(&g_srv) == 0u);
    assert(g_wire.len == 0u);
    printf("  going quiet dropped the session, silently, and relocked\n");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
int nx_uds_server_example_run(void)
{
    printf("=== nx_uds_server example ===\n");

    const nx_uds_server_cfg_t cfg = {
        .services       = g_services,
        .services_count = (uint16_t)SERVICE_COUNT,
        .out_fn         = mock_out,          /* where a finished answer goes  */
        .session_fn     = mock_session_change,
        .req_buf        = g_req_buf,         /* the request is kept here      */
        .req_buf_size   = sizeof(g_req_buf),
        .out_buf        = g_out_buf,         /* the answer is assembled here  */
        .out_buf_size   = sizeof(g_out_buf),
        .max_req_apdu   = sizeof(g_req_buf), /* longer is refused with 0x13   */
        .max_resp_apdu  = sizeof(g_out_buf),
        .link           = LINK_ID,           /* the one conversation it serves */
        .get_us         = mock_get_us,
        .p2_us          = P2_US,
        .p2_star_us     = P2_STAR_US,
        .p4_us          = P4_US,
        .s3_us          = S3_US,
        .max_pending    = MAX_PENDING + 8u,  /* rows may ask for less */
    };
    assert(nx_uds_server_init(&g_srv, &cfg));

    demo_positive_response();
    demo_service_not_supported();
    demo_response_sid_ignored();
    demo_bad_length();
    demo_wrong_session();
    demo_locked_service();
    demo_bad_sub_function();
    demo_sub_function_wrong_session();
    demo_suppress_positive();
    demo_functional_suppression();
    demo_pending_then_answer();
    demo_pending_capped();
    demo_carrier_backpressure();
    demo_session_lifecycle();

    /* A request on a connection this instance does not serve is not its
     * business: one instance is one conversation. */
    const uint8_t req[] = {SID_ECHO, 0x00u};
    assert(nx_uds_server_indicate(&g_srv, req, sizeof(req), NX_TP_TA_PHYSICAL,
                                  LINK_ID + 1u) == NX_UDS_SERVER_ERR_PARAM);

    /* A second request while one is running is refused, and the running one is
     * left alone - a broadcast meant for somebody else must not tear down a
     * transfer in progress. */
    g_slow_left = 1u;
    const uint8_t slow[] = {SID_SLOW};
    assert(request(slow, sizeof(slow)) == NX_UDS_SERVER_OK);
    assert(nx_uds_server_is_busy(&g_srv));
    assert(request(req, sizeof(req)) == NX_UDS_SERVER_ERR_BUSY);
    assert(nx_uds_server_is_busy(&g_srv));
    for (unsigned i = 0; i < 4u; i++) {
        nx_uds_server_process(&g_srv);
    }
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, NX_TP_N_OK);
    assert(!nx_uds_server_is_busy(&g_srv));

    /* A configuration init must refuse: a row available in no session at all
     * describes a service nothing can ever reach. */
    static nx_uds_service_t bad_rows[1];
    bad_rows[0] = g_services[0];
    bad_rows[0].session_mask = 0u;
    nx_uds_server_cfg_t bad = cfg;
    bad.services = bad_rows;
    bad.services_count = 1u;
    assert(nx_uds_server_init(&g_srv, &bad) == false);

    /* And one whose row carries a sub-function it never requires the bytes for:
     * the sub-function byte is read without a bounds test of its own. */
    bad_rows[0] = g_services[1];
    bad_rows[0].min_len = 1u;
    assert(nx_uds_server_init(&g_srv, &bad) == false);

    printf("nx_uds_server example passed\n\n");
    return 0;
}
