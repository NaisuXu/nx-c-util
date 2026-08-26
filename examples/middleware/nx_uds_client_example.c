/**
 * @file    nx_uds_client_example.c
 * @brief   Usage example for nx_uds_client: the diagnostic tester (test tool side).
 *
 * The client is the peer of nx_uds_server, and this example is that file's mirror:
 * where the server example watches what the server puts on the wire and feeds it
 * requests, this one watches what the client puts on the wire and feeds it answers,
 * because the tester is the side that asks the questions.
 *
 *      request bytes                                 response bytes
 *           |                                                ^
 *           | nx_uds_client_request()                          | nx_uds_client_indicate()
 *           v                                                |
 *        [ nx_uds_client ] --send_fn-->    mock ECU       +-----+
 *           |                            (scripted answer)    |
 *           | nx_uds_client_process()  drives the timers      |
 *           v                            /-- one request -->  /
 *      nx_uds_client_confirm()   reports what became of it
 *
 * The example plays both the carrier and the ECU: its send_fn records what the
 * client tried to send so the assertions can inspect it, and its ECU hands back a
 * chosen answer (or nothing, or a refusal) once the request has actually left. A
 * flag makes the carrier refuse, and a call to nx_uds_client_confirm() with a
 * failed outcome exercises the link-failure path.
 *
 * Ten scenarios are exercised, each a complete transaction that self-checks:
 *   1.  The ECU answers                     -> NX_UDS_CLIENT_RESULT_OK
 *   2.  The ECU refuses                     -> NX_UDS_CLIENT_RESULT_NEGATIVE
 *   3.  The ECU delays, then answers        -> OK, after a 0x78 responsePending
 *   4.  The link took the request, then lost it -> NX_UDS_CLIENT_RESULT_TIMEOUT
 *   5.  The link refuses, then takes it     -> the same request is offered again
 *   6.  The link refuses past its deadline  -> NX_UDS_CLIENT_RESULT_TIMEOUT
 *   7.  A request that asks for silence     -> NO_RESPONSE after a quiet window
 *   8.  A 0x10 answer publishes the windows -> the client adopts them
 *   9.  A response for another service      -> NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR
 *  10.  A transaction canceled mid-wait     -> NX_UDS_CLIENT_RESULT_CANCELED
 *
 * All storage is static; the example is driven by a mock clock and prints what the
 * client sent and how each transaction ended.
 */
#include "nx_middleware_examples.h"
#include "src/middleware/nx_uds_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Timings short enough to step through on the mock clock, and deliberately
 * distinct from the values a 0x10 answer publishes (see scenario 8) so adoption can
 * be told apart from configuration. */
#define P2_US          100000u
#define P2_STAR_US     500000u
#define SEND_TIMEOUT_US 100000u
#define SEND_DEFAULT_TICK    1000u /* how far each process() call advances the clock */

/* Identifiers the example uses for its own test traffic. */
#define SID_READ     0x22u   /**< a data request: the client asks, the ECU answers */
#define SID_SESSION  0x10u   /**< a session request: its answer publishes timing */
#define SID_ABSENT   0x19u   /**< in no script; the ECU refuses it */
#define DID_TEST     0xF190u /**< a made-up data identifier */

#define LINK_ID      2u      /**< the one conversation this client speaks on */

/* ------------------------------------------------------------------ */
/* Mock plumbing: a clock the example steers, a carrier it watches     */
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
 * @brief What the carrier saw. A real send path copies the request and queues it;
 * this one records the bytes so the assertions can read them back.
 */
static struct {
    uint8_t  buf[16]; /**< the last request handed down */
    uint32_t len;     /**< its length; 0 when nothing was handed down */
    uint8_t  link;    /**< which connection it was for */
    uint8_t  ta_type; /**< how it was addressed */
    unsigned count;   /**< how many requests have been handed down in all */
    bool     refuse;  /**< whether the carrier refuses the request */
} g_wire;

/** @brief Forget what the carrier saw, and put it back to a taking carrier. */
static void wire_clear(void)
{
    g_wire.len     = 0u;
    g_wire.count   = 0u;
    g_wire.refuse  = false;
}

/* ------------------------------------------------------------------ */
/* The scripted ECU                                                    */
/* ------------------------------------------------------------------ */
/**
 * @brief What the ECU hands back for the just-sent request.
 *
 * Set @c buf to a ready-made response, or to NULL to keep quiet (a carrier that is
 * fine but an ECU that is not answering). Handed back once, on the process() call
 * after the request has left - a scripted response is armed here, and it is
 * delivered by handoff_ecu() once the client is actually waiting for an answer.
 */
static struct {
    const uint8_t *buf;  /**< the response bytes, or NULL for silence */
    uint32_t       len;  /**< its length in bytes */
    unsigned       sent; /**< how many times it has been handed back */
} g_ecu;

/** @brief Whether a scripted response is waiting to be handed over. */
static struct {
    bool pending;
} g_handoff;

/** @brief Script the ECU with a response to hand back on the next request. */
static void ecu_script(const uint8_t *rsp, uint32_t len)
{
    g_ecu.buf  = rsp;
    g_ecu.len  = len;
    g_ecu.sent = 0u;
    g_handoff.pending = (rsp != NULL);
}

/** @brief Make the ECU answer nothing. */
static void ecu_silent(void)
{
    ecu_script(NULL, 0u);
}

/* ------------------------------------------------------------------ */
/* The send path                                                       */
/* ------------------------------------------------------------------ */
/**
 * @brief  The client's send callback: what a binding shim would implement.
 *
 * A real one builds a transport packet and queues it, and reports whether the
 * transport took it. This one records the bytes the client asked to send, so the
 * assertions can inspect exactly what went on the wire; the response is handled by
 * the ECU script above rather than here, since a response can only be delivered
 * while the client is waiting for one.
 */
static bool mock_send(void *user, uint8_t link, const uint8_t *req, uint32_t len,
                      uint8_t ta_type)
{
    (void)user;
    if (g_wire.refuse) {
        return false;
    }
    assert(len <= sizeof(g_wire.buf));
    memcpy(g_wire.buf, req, len);
    g_wire.len     = len;
    g_wire.link    = link;
    g_wire.ta_type = ta_type;
    g_wire.count++;
    return true;
}

/* ------------------------------------------------------------------ */
/* The step driver                                                     */
/* ------------------------------------------------------------------ */
static nx_uds_client_t g_clt;

/** @brief The outcome of the last finished transaction. */
static nx_uds_client_result_t g_last_result;

/** @brief The response length the client reported for the last result. */
static uint32_t g_last_resp_len;

/** @brief Count of result callbacks that have fired in all. */
static unsigned g_result_count;

/**
 * @brief  How a transaction ended, recorded for the assertion after it resolves.
 *
 * A real application acts here: it reads the response buffer (via the client's
 * response buffer and nx_uds_client_resp_len()) and decides what to do next. The
 * buffers belong to the application, so instead of keeping the whole client this
 * example keeps just the outcome and the length.
 */
static void report_result(void *user, nx_uds_client_t *clt,
                          nx_uds_client_result_t result)
{
    (void)user;
    g_last_result   = result;
    g_last_resp_len = nx_uds_client_resp_len(clt);
    g_result_count++;
}

/**
 * @brief  Run one process() call, advancing the clock a little.
 *
 * The clock must advance for the mock to be able to time out a window; a fixed
 * advance keeps every scenario honest about what enough time does.
 */
static void tick(void)
{
    advance_us(SEND_DEFAULT_TICK);
    (void)nx_uds_client_process(&g_clt);
}

/**
 * @brief  Hand the ECU's scripted answer over, if one is waiting.
 * @return Whether an answer was delivered.
 */
static bool handoff_ecu(void)
{
    if (!g_handoff.pending) {
        return false;
    }
    g_handoff.pending = false;
    (void)nx_uds_client_indicate(&g_clt, g_ecu.buf, g_ecu.len,
                                 NX_TP_TA_PHYSICAL, LINK_ID);
    return true;
}

/**
 * @brief  Drive a transaction to resolution: tick and deliver answers until idle.
 * @return The outcome the last result callback reported.
 *
 * The ECU's answer, if scripted, is delivered on the tick after the request leaves
 * so that the client is genuinely waiting when it arrives; a scripted silent ECU is
 * left to time out.
 */
static nx_uds_client_result_t run_until_idle(void)
{
    unsigned guard = 0u;

    while (nx_uds_client_is_busy(&g_clt)) {
        tick();
        handoff_ecu();
        if (++guard > 1000u) {
            break; /* a scenario armed a transaction that never resolves */
        }
    }
    return g_last_result;
}

/** @brief Print a request as it appeared on the wire. */
static void print_req(const char *tag)
{
    printf("  %-28s", tag);
    if (g_wire.len == 0u) {
        printf("(nothing sent)\n");
        return;
    }
    printf("len %u: ", (unsigned)g_wire.len);
    for (uint32_t i = 0u; i < g_wire.len; i++) {
        printf("%02X ", g_wire.buf[i]);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* 1. The ECU answers                                                   */
/* ------------------------------------------------------------------ */
static void demo_answers(void)
{
    printf("1. a request the ECU answers\n");

    /* A request for data identifier 0xF190, assembled by the client. */
    static const uint8_t did[] = {0xF1u, 0x90u};
    static const uint8_t rsp[] = {NX_UDS_SID_TO_POS_RSP(SID_READ),
                                  0xF1u, 0x90u, 0xAAu, 0xBBu};
    ecu_script(rsp, sizeof(rsp));
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, did, sizeof(did),
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    assert(nx_uds_client_is_busy(&g_clt));
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_OK);

    /* The request the send path saw: the identifier, the sub-function byte, then
     * the data bytes. */
    print_req("request ->");
    assert(g_wire.count == 1u);
    assert(g_wire.len == 4u);
    assert(g_wire.buf[0] == SID_READ && g_wire.buf[1] == 0x00u
           && g_wire.buf[2] == ((uint8_t)(DID_TEST >> 8))
           && g_wire.buf[3] == ((uint8_t)(DID_TEST & 0xFFu)));
    assert(g_wire.link == LINK_ID);
    assert(g_wire.ta_type == NX_TP_TA_PHYSICAL);

    /* The response is kept whole, and exactly one result callback fired. */
    assert(g_last_resp_len == sizeof(rsp));
    assert(g_result_count == 1u);
    assert(!nx_uds_client_is_busy(&g_clt));
    printf("  the answer came back whole, %u bytes in the response buffer\n",
           (unsigned)g_last_resp_len);
}

/* ------------------------------------------------------------------ */
/* 2. The ECU refuses                                                   */
/* ------------------------------------------------------------------ */
static void demo_refusal(void)
{
    printf("2. a request the ECU refuses\n");

    static const uint8_t rsp[] = {NX_UDS_NEG_RSP_SID, SID_READ,
                                  NX_UDS_NRC_SERVICE_NOT_SUPPORTED};
    ecu_script(rsp, sizeof(rsp));
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_NEGATIVE);

    print_req("refused (0x11) ->");
    /* The refusal is kept for the application to read: the identifier and the
     * reason the ECU gave. */
    assert(g_last_resp_len == sizeof(rsp));
    assert(g_result_count == 1u);
    printf("  the refusal came back, reason 0x%02X\n",
           (unsigned)g_clt.cfg.rsp_buf[2]);
}

/* ------------------------------------------------------------------ */
/* 3. The ECU delays, then answers                                      */
/* ------------------------------------------------------------------ */
static void demo_delayed_answer(void)
{
    printf("3. the ECU delays, then answers\n");

    static const uint8_t pending[] = {NX_UDS_NEG_RSP_SID, SID_READ,
                                      NX_UDS_NRC_RESPONSE_PENDING};
    static const uint8_t answer[] = {NX_UDS_SID_TO_POS_RSP(SID_READ),
                                     0x01u, 0x02u, 0x03u, 0x04u};

    /* First a 0x78 responsePending: a refusal whose reason says the answer is still
     * coming, so the transaction is extended, not ended. */
    ecu_script(pending, sizeof(pending));
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* The pending notification reaches a client that is waiting. */
    tick();
    assert(handoff_ecu());
    assert(nx_uds_client_is_busy(&g_clt)); /* extended, not resolved */
    /* No result callback yet; the answer is still owed. */
    assert(g_result_count == 0u);

    /* Now the real answer, scripted as the second thing the ECU says. */
    ecu_script(answer, sizeof(answer));
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_OK);

    assert(g_last_resp_len == sizeof(answer));
    assert(g_result_count == 1u);
    printf("  extended once, then the answer came back on the second ask\n");
}

/* ------------------------------------------------------------------ */
/* 4. The link took the request, then lost it                           */
/* ------------------------------------------------------------------ */
static void demo_link_fault(void)
{
    printf("4. the link took the request, then lost it\n");

    ecu_silent();
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* Let the request leave (it was accepted), then hear that it never arrived. */
    tick();
    assert(g_wire.count == 1u);
    nx_uds_client_confirm(&g_clt, LINK_ID, (uint8_t)NX_TP_N_UNEXP_PDU);
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_TIMEOUT);

    assert(g_result_count == 1u);
    assert(g_last_resp_len == 0u);
    printf("  the request went out, but the link reported it lost -> timeout\n");
}

/* ------------------------------------------------------------------ */
/* 5. The link refuses, then takes it                                   */
/* ------------------------------------------------------------------ */
static void demo_retry(void)
{
    printf("5. the link refuses, then takes it\n");

    static const uint8_t rsp[] = {NX_UDS_SID_TO_POS_RSP(SID_READ), 0x00u};
    ecu_script(rsp, sizeof(rsp));
    wire_clear();
    g_wire.refuse = true;
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* First offer: the carrier will not take it, so the request stays pending. */
    tick();
    assert(g_wire.count == 0u);
    assert(nx_uds_client_is_busy(&g_clt));
    assert(g_result_count == 0u);

    /* The carrier gets out of the way; the very same request goes out. */
    g_wire.refuse = false;
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_OK);

    assert(g_wire.count == 1u);
    assert(g_last_resp_len == sizeof(rsp));
    printf("  the same request was re-offered until the carrier took it\n");
}

/* ------------------------------------------------------------------ */
/* 6. The link refuses past its deadline                                */
/* ------------------------------------------------------------------ */
static void demo_refuse_timeout(void)
{
    printf("6. the link refuses past its deadline\n");

    ecu_silent();
    wire_clear();
    g_wire.refuse = true;
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* The send path keeps refusing well beyond send_timeout_us; the request never
     * left, so this is a failure to get it out rather than a missing answer. */
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_TIMEOUT);

    assert(g_wire.count == 0u);
    assert(g_result_count == 1u);
    assert(g_last_resp_len == 0u);
    printf("  the carrier never took it within send_timeout_us\n");
}

/* ------------------------------------------------------------------ */
/* 7. A request that asks for silence                                   */
/* ------------------------------------------------------------------ */
static void demo_suppress_pos(void)
{
    printf("7. a request that asks for silence\n");

    /* The suppression bit is the top bit of the sub-function byte. A request that
     * sets it asks for no positive response; silence is the expected outcome. */
    ecu_silent();
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, NX_UDS_SUPPRESS_POS_RSP_BIT,
                                 NULL, 0u, NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* The bit rides in the frame too, so the carrier sees the request as
     * intended. */
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_NO_RESPONSE);

    assert(g_wire.count == 1u);
    assert((g_wire.buf[1] & NX_UDS_SUPPRESS_POS_RSP_BIT) != 0u);
    assert(g_result_count == 1u);
    assert(g_last_resp_len == 0u);
    printf("  the window ran out in silence, which is what the request asked for\n");
}

/* ------------------------------------------------------------------ */
/* 8. A 0x10 answer publishes the windows                               */
/* ------------------------------------------------------------------ */
static void demo_published_timing(void)
{
    printf("8. a 0x10 answer publishes the timing windows\n");

    /* A session request whose answer says P2=100 ms and P2*=5000 ms, in the
     * packed 16-bit forms the protocol uses (P2 in ms, P2* in units of 10 ms). */
    static const uint8_t rsp[] = {NX_UDS_SID_TO_POS_RSP(SID_SESSION),
                                  NX_UDS_SESSION_DEFAULT,
                                  0x00u, 0x64u,      /* P2  = 100 ms */
                                  0x13u, 0x88u};     /* P2* = 5000 * 10 ms */
    ecu_script(rsp, sizeof(rsp));
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_SESSION, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* The answer is fed to the client and, because the client is not running with
     * fixed_timing, the timing it publishes is adopted for the conversation. */
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_OK);

    uint32_t p2, p2_star;
    nx_uds_client_timing(&g_clt, &p2, &p2_star);
    assert(p2 == 100u * 1000u);
    assert(p2_star == 5000u * 10000u);
    /* The session control answer also names the session, which is now active. */
    assert(nx_uds_client_session(&g_clt) == NX_UDS_SESSION_DEFAULT);
    printf("  adopted P2=100ms and P2*=50000ms from the 0x10 answer\n");
    (void)p2;
    (void)p2_star;
}

/* ------------------------------------------------------------------ */
/* 9. A response for another service                                    */
/* ------------------------------------------------------------------ */
static void demo_wrong_service(void)
{
    printf("9. a response for another service\n");

    /* The client asked for a ReadDataByIdentifier, but the ECU answers a
     * session-control request: a positive response whose identifier is for SID_ABSENT,
     * which is not this transaction's. */
    static const uint8_t rsp[] = {NX_UDS_SID_TO_POS_RSP(SID_ABSENT), 0x00u};
    ecu_script(rsp, sizeof(rsp));
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR);

    /* A frame for another service is not an answer, so no response is kept. */
    assert(g_last_resp_len == 0u);
    assert(g_result_count == 1u);
    printf("  a response for another service is a protocol error\n");
}

/* ------------------------------------------------------------------ */
/* 10. A transaction canceled mid-wait                                  */
/* ------------------------------------------------------------------ */
static void demo_cancel(void)
{
    printf("10. a transaction canceled mid-wait\n");

    ecu_silent();
    wire_clear();
    g_result_count = 0u;

    assert(nx_uds_client_request(&g_clt, SID_READ, 0x00u, NULL, 0u,
                                 NX_TP_TA_PHYSICAL) == NX_UDS_CLIENT_OK);
    /* Let the request leave so the client is genuinely waiting for an answer. */
    tick();
    assert(g_wire.count == 1u);
    assert(nx_uds_client_cancel(&g_clt) == NX_UDS_CLIENT_OK);
    /* The cancellation is reported at the next clean pump rather than in cancel(),
     * so no result callback has fired yet. */
    assert(g_result_count == 0u);
    assert(run_until_idle() == NX_UDS_CLIENT_RESULT_CANCELED);

    assert(g_result_count == 1u);
    assert(!nx_uds_client_is_busy(&g_clt));
    printf("  the leftover wait was dropped, reported as canceled\n");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */
int nx_uds_client_example_run(void)
{
    static uint8_t req_buf[16];
    static uint8_t rsp_buf[64];
    nx_uds_client_cfg_t cfg = {0};

    printf("nx_uds_client examples\n");

    cfg.result_fn      = report_result;
    cfg.send_fn        = mock_send;
    cfg.req_buf        = req_buf;
    cfg.req_buf_size   = sizeof(req_buf);
    cfg.rsp_buf        = rsp_buf;
    cfg.rsp_buf_size   = sizeof(rsp_buf);
    cfg.link           = LINK_ID;
    cfg.get_us         = mock_get_us;
    cfg.p2_us          = P2_US;
    cfg.p2_star_us     = P2_STAR_US;
    cfg.send_timeout_us = SEND_TIMEOUT_US;

    assert(nx_uds_client_init(&g_clt, &cfg) == NX_UDS_CLIENT_OK);

    g_clock_us = 0u;
    g_last_result = NX_UDS_CLIENT_RESULT_TIMEOUT; /* so a failed start is visible */
    g_wire.refuse = false;

    demo_answers();
    demo_refusal();
    demo_delayed_answer();
    demo_link_fault();
    demo_retry();
    demo_refuse_timeout();
    demo_suppress_pos();
    demo_published_timing();
    demo_wrong_service();
    demo_cancel();

    printf("all nx_uds_client examples passed\n");
    return 0;
}
