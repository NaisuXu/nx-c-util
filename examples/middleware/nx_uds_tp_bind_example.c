/**
 * @file    nx_uds_tp_bind_example.c
 * @brief   A diagnostic server answering over a real segmenting transport.
 *
 * Every other UDS example hands the server its requests directly and catches the
 * answers in a callback. This one puts a transport under it, so a request is
 * segmented into frames, carried, reassembled, answered, and the answer segmented
 * back - and the binding is what joins the two.
 *
 *   tester                                                          ECU
 *   [ nx_can_isotp ] --frames--> the bus --frames--> [ nx_can_isotp ]
 *                                                          |    ^
 *                                                  sdu_tx  |    | sdu_rx
 *                                                          v    |
 *                                                  [ nx_uds_tp_bind ]
 *                                                          |    ^
 *                                              indicate()  |    | out_fn
 *                                                          v    |
 *                                                   [ nx_uds_server ]
 *
 * The bus is two loops of wire: whatever one instance puts on its transmit queue is
 * fed to the other's receive queue. Both ends are the same module, configured with
 * the identifier pair swapped, so the frames crossing between them are the frames a
 * real bus would carry.
 *
 * What is being checked is what only a real transport can show:
 *   1. A request and its answer, both short enough for one frame each
 *   2. A flash sequence - unlock, open, blocks, finish - over segmented frames
 *   3. A functionally addressed request answered to one client, not to the link
 *   4. A response the transport cannot take yet, offered again and then sent
 *   5. Counters that stay at zero on a path that is working
 *
 * All storage is static and the example self-checks with asserts.
 */
#include "nx_middleware_examples.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/middleware/nx_can_isotp.h"
#include "src/middleware/nx_uds_svc_sec.h"
#include "src/middleware/nx_uds_svc_session.h"
#include "src/middleware/nx_uds_tp_bind.h"
#include "src/middleware/nx_uds_svc_transfer.h"

/* The identifier pair, as a diagnostic network assigns it: the tester asks on the
 * request identifier and the ECU answers on the response one. Each instance
 * receives on what the other transmits, which is the whole of the wiring. */
#define REQ_ID   0x7E0u
#define RSP_ID   0x7E8u
#define FUNC_ID  0x7DFu

/* One connection, numbered by the application. The transport stamps it into
 * everything it publishes and the server refuses anything carrying another. */
#define LINK_ID  1u

/* Frames hold eight bytes, so anything longer than seven travels segmented and the
 * example sees real flow control. */
#define FRAME_LEN  NX_CAN_ISOTP_FRAME_8

/* What one message may be. The server is capped at the same number, so a response
 * too long for the path is refused while it is being built rather than after. */
#define MAX_SDU  128u

/* Server timing. Wide enough that nothing here expires by accident. */
#define P2_US       50000u
#define P2_STAR_US  5000000u
#define P4_US       20000000u
#define S3_US       5000000u

/* The region being flashed, and the memory behind it. */
#define MEM_BASE  0x8000u
#define MEM_SIZE  64u

/* The largest block this product takes, counting the whole message. Ten bytes of
 * payload, so a 32-byte region arrives in four blocks. */
#define XFER_BLOCK_LEN  12u

/* Security: one level, four bytes each way. */
#define SEC_LEVEL    1u
#define SEC_SEED_LEN 4u
#define SEC_KEY_LEN  4u

/* ------------------------------------------------------------------ */
/* The clock                                                          */
/* ------------------------------------------------------------------ */
static uint32_t g_now_us;

static uint32_t mock_now(void)
{
    return g_now_us;
}

/* ------------------------------------------------------------------ */
/* The pool and the queues                                            */
/* ------------------------------------------------------------------ */
/* A block holds the reference-message header as well as the payload, since the two
 * are one allocation. */
#define FRAME_BLOCK (sizeof(nx_ref_msg_t) + sizeof(nx_can_msg_t) + 8u)
#define SDU_BLOCK   (sizeof(nx_ref_msg_t) + sizeof(nx_tp_sdu_t) + MAX_SDU)

static uint8_t g_pool_mem[16384];
static nx_tiered_mem_pool_t g_pool;

/* Each side has its own four queues. The names are from the transport's point of
 * view: it reads the rx queues and writes the tx ones. */
static nx_ref_msg_t *g_ecu_sdu_rx_buf[4];
static nx_ref_msg_t *g_ecu_sdu_tx_buf[8];
static nx_ref_msg_t *g_ecu_can_rx_buf[16];
static nx_ref_msg_t *g_ecu_can_tx_buf[16];
static nx_queue_t g_ecu_sdu_rx_q, g_ecu_sdu_tx_q, g_ecu_can_rx_q, g_ecu_can_tx_q;

static nx_ref_msg_t *g_tst_sdu_rx_buf[4];
static nx_ref_msg_t *g_tst_sdu_tx_buf[8];
static nx_ref_msg_t *g_tst_can_rx_buf[16];
static nx_ref_msg_t *g_tst_can_tx_buf[16];
static nx_queue_t g_tst_sdu_rx_q, g_tst_sdu_tx_q, g_tst_can_rx_q, g_tst_can_tx_q;

static nx_can_isotp_t g_ecu_tp;
static nx_can_isotp_t g_tst_tp;

static nx_uds_server_t    g_srv;
static nx_uds_tp_bind_t   g_bind;
static nx_uds_svc_sec_t       g_sec;
static nx_uds_svc_transfer_t g_xfer;
static nx_uds_svc_session_cfg_t g_session_cfg;

/* ------------------------------------------------------------------ */
/* The bus                                                            */
/* ------------------------------------------------------------------ */
static unsigned g_frames_carried;

/* When set, frames leaving the ECU are held here instead of being delivered, which
 * is a link that has stopped taking them. */
static bool g_bus_blocked;

/**
 * @brief  Move every frame one instance emitted to the other's receive queue.
 *
 * A real driver transmits what it dequeues and its peer's controller receives it;
 * here the two steps are the same step. The reference travels with the frame, so
 * nothing is copied and nothing is released: the queue that receives it holds it.
 *
 * @param  from Queue the frames were emitted to.
 * @param  to   Queue they arrive on.
 * @return How many were carried.
 */
static unsigned carry(nx_queue_t *from, nx_queue_t *to)
{
    nx_ref_msg_t *m = NULL;
    unsigned n = 0u;

    while (nx_queue_pop(from, &m) == NX_QUEUE_OK) {
        assert(nx_ref_msg_publish(m, to) == NX_REF_MSG_OK);
        nx_ref_msg_release(m);   /* the receiving queue holds the reference now */
        n++;
        g_frames_carried++;
    }
    return n;
}

/**
 * @brief  One pass of everything: both transports, the binding, and the server.
 *
 * The order is the one an application would use - carry what the wire has, let each
 * side act on it, then carry what that produced - and the loop below runs it enough
 * times for a segmented exchange to complete.
 */
static void turn(void)
{
    carry(&g_tst_can_tx_q, &g_ecu_can_rx_q);
    if (!g_bus_blocked) {
        carry(&g_ecu_can_tx_q, &g_tst_can_rx_q);
    }

    nx_can_isotp_process(&g_ecu_tp);
    nx_uds_tp_bind_process(&g_bind);
    nx_uds_server_process(&g_srv);
    nx_can_isotp_process(&g_tst_tp);
}

/**
 * @brief  Run the loop until the tester has a message, or until time is up.
 *
 * Advancing the clock is what lets the separation time between frames elapse, so a
 * segmented message needs both the passes and the microseconds.
 *
 * @param  out Where to store the reference to the message; caller releases it.
 * @return true when one arrived.
 */
static bool wait_for_response(nx_ref_msg_t **out)
{
    unsigned i;

    for (i = 0u; i < 400u; i++) {
        turn();
        if (nx_queue_pop(&g_tst_sdu_tx_q, out) == NX_QUEUE_OK) {
            const nx_tp_sdu_t *s = (const nx_tp_sdu_t *)nx_ref_msg_data(*out);

            /* The tester asked for no confirmations, so anything on this queue is
             * something that arrived. */
            assert(s->kind == (uint8_t)NX_TP_SDU_INDICATION);
            return true;
        }
        g_now_us += 1000u;   /* 1 ms per pass */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Asking, from the tester's side                                     */
/* ------------------------------------------------------------------ */
static uint8_t  g_rsp[MAX_SDU];
static uint32_t g_rsp_len;

/**
 * @brief  Send a request and keep the answer.
 *
 * @param  req     Request bytes.
 * @param  len     How many.
 * @param  ta_type How to address it.
 * @return true when an answer came back.
 */
static bool ask_as(const uint8_t *req, uint32_t len, nx_tp_ta_type_t ta_type)
{
    nx_ref_msg_t *m = NULL;

    g_rsp_len = 0u;
    assert(nx_can_isotp_send(&g_tst_tp, req, len, ta_type) == NX_CAN_ISOTP_OK);

    if (!wait_for_response(&m)) {
        return false;
    }
    {
        const nx_tp_sdu_t *s = (const nx_tp_sdu_t *)nx_ref_msg_data(m);

        assert(s->len <= sizeof(g_rsp));
        memcpy(g_rsp, s->data, s->len);
        g_rsp_len = s->len;
        /* However the request was addressed, the answer is for this client alone. */
        assert(s->ta_type == (uint8_t)NX_TP_TA_PHYSICAL);
        assert(s->link == LINK_ID);
    }
    nx_ref_msg_release(m);
    return true;
}

/** @brief Send a physically addressed request and keep the answer. */
static bool ask(const uint8_t *req, uint32_t len)
{
    return ask_as(req, len, NX_TP_TA_PHYSICAL);
}

/** @brief Print what came back. */
static void show(const char *what)
{
    uint32_t i;
    uint32_t n = (g_rsp_len > 12u) ? 12u : g_rsp_len;

    printf("  %-24s", what);
    if (g_rsp_len == 0u) {
        printf("(nothing)\n");
        return;
    }
    printf("[%u]", (unsigned)g_rsp_len);
    for (i = 0u; i < n; i++) {
        printf(" %02X", g_rsp[i]);
    }
    if (n < g_rsp_len) {
        printf(" ...");
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* What the product does                                              */
/* ------------------------------------------------------------------ */
static uint8_t  g_mem[MEM_SIZE];
static unsigned g_writes;
static unsigned g_closes;
static uint8_t  g_reset_asked;
static bool     g_unlocked;

/** @brief The seed this level issues. A product computes one; this counts. */
static bool app_seed(void *user, uint8_t level, const uint8_t *record,
                     uint32_t record_len, uint8_t *seed, uint32_t seed_cap,
                     uint32_t *seed_len)
{
    uint32_t i;

    (void)user; (void)level; (void)record; (void)record_len;
    assert(seed_cap >= SEC_SEED_LEN);
    for (i = 0u; i < SEC_SEED_LEN; i++) {
        seed[i] = (uint8_t)(0xA0u + i);
    }
    *seed_len = SEC_SEED_LEN;
    return true;
}

/** @brief The key that seed expects. Every byte inverted, which is not a scheme. */
static bool app_verify(void *user, uint8_t level, const uint8_t *seed,
                       uint32_t seed_len, const uint8_t *key, uint32_t key_len)
{
    uint32_t i;

    (void)user; (void)level;
    if (key_len != seed_len) {
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
    (void)user; (void)level;
    g_unlocked = true;
}

static void app_reset(void *user, uint8_t reset_type)
{
    (void)user;
    /* Reaching here means the answer was handed over and accepted. A product
     * restarts; this records that it was asked to. */
    g_reset_asked = reset_type;
}

static bool app_open(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t addr,
                     nx_uds_svc_transfer_addr_t size, uint8_t format, uint32_t *block_len,
                     uint8_t *nrc)
{
    (void)user; (void)dir; (void)block_len;

    if (format != 0x00u) {
        *nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
        return false;
    }
    if (addr < MEM_BASE || size == 0u || addr + size > MEM_BASE + MEM_SIZE) {
        *nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
        return false;
    }
    return true;
}

static bool app_write(void *user, nx_uds_svc_transfer_addr_t addr, const uint8_t *data,
                      uint32_t len, uint8_t *nrc)
{
    (void)user; (void)nrc;

    assert(addr >= MEM_BASE && addr + len <= MEM_BASE + MEM_SIZE);
    memcpy(&g_mem[addr - MEM_BASE], data, len);
    g_writes++;
    return true;
}

static bool app_read(void *user, nx_uds_svc_transfer_addr_t addr, uint8_t *out, uint32_t len,
                     uint8_t *nrc)
{
    (void)user; (void)nrc;

    assert(addr >= MEM_BASE && addr + len <= MEM_BASE + MEM_SIZE);
    memcpy(out, &g_mem[addr - MEM_BASE], len);
    return true;
}

static bool app_close(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t done,
                      nx_uds_svc_transfer_addr_t size, const uint8_t *record,
                      uint32_t record_len, uint8_t *out, uint32_t out_cap,
                      uint32_t *out_len, uint8_t *nrc)
{
    (void)user; (void)dir; (void)done; (void)size; (void)record;
    (void)record_len; (void)out; (void)out_cap; (void)nrc;

    g_closes++;
    *out_len = 0u;   /* nothing to add after the identifier */
    return true;
}

/* ------------------------------------------------------------------ */
/* The service table                                                  */
/* ------------------------------------------------------------------ */
/* 0x10 accepts the three sessions this product has; 0x27 offers the one level in
 * every session but the default, which is how a request to unlock from the default
 * session is refused as out of session. */
static const uint8_t g_session_subs[] = {
    NX_UDS_SESSION_DEFAULT, NX_UDS_SESSION_PROGRAMMING, NX_UDS_SESSION_EXTENDED
};
static const uint8_t g_reset_subs[] = { NX_UDS_RESET_HARD, NX_UDS_RESET_SOFT };
static const uint8_t g_sec_subs[] = {
    NX_UDS_SVC_SEC_SEED_SUB(SEC_LEVEL), NX_UDS_SVC_SEC_KEY_SUB(SEC_LEVEL)
};
static const uint8_t g_tp_subs[] = { NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB };

static const nx_uds_svc_sec_level_t g_sec_levels[] = {
    { SEC_LEVEL, SEC_SEED_LEN, SEC_KEY_LEN }
};
static uint8_t g_seed_buf[SEC_SEED_LEN];

static nx_uds_svc_session_reset_cfg_t g_reset_cfg = {
    .do_fn = app_reset, .allow_fn = NULL, .user = NULL, .power_down_time = 0u
};

static const nx_uds_service_t g_services[] = {
    {   .sid = NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler = nx_uds_svc_session_control, .user = &g_session_cfg,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION | NX_UDS_SVC_ANSWER_FUNCTIONAL,
        .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = g_session_subs,
        .subs_count = (uint8_t)(sizeof(g_session_subs) / sizeof(g_session_subs[0])),
        .sub_session_masks = NULL, .min_len = 2u, .max_len = 2u },

    {   .sid = NX_UDS_SID_ECU_RESET,
        .handler = nx_uds_svc_session_ecu_reset, .user = &g_reset_cfg,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = g_reset_subs,
        .subs_count = (uint8_t)(sizeof(g_reset_subs) / sizeof(g_reset_subs[0])),
        .sub_session_masks = NULL, .min_len = 2u, .max_len = 2u },

    {   /* Answered functionally as well, since a tester keeps a whole network alive
         * with one broadcast rather than one request per ECU. */
        .sid = NX_UDS_SID_TESTER_PRESENT,
        .handler = nx_uds_svc_session_tester_present, .user = NULL,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION | NX_UDS_SVC_ANSWER_FUNCTIONAL,
        .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = g_tp_subs,
        .subs_count = (uint8_t)(sizeof(g_tp_subs) / sizeof(g_tp_subs[0])),
        .sub_session_masks = NULL, .min_len = 2u, .max_len = 2u },

    {   .sid = NX_UDS_SID_SECURITY_ACCESS,
        .handler = nx_uds_svc_security_access, .user = &g_sec,
        .flags = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_NON_DEFAULT, .sec_level = 0u,
        .subs = g_sec_subs,
        .subs_count = (uint8_t)(sizeof(g_sec_subs) / sizeof(g_sec_subs[0])),
        .sub_session_masks = NULL, .min_len = 2u, .max_len = 34u },

    /* The transfer services need the level unlocked and the programming session,
     * which is what a flash sequence has to earn before it can start. */
    {   .sid = NX_UDS_SID_REQUEST_DOWNLOAD,
        .handler = nx_uds_svc_transfer_request_download, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_PROGRAMMING,
        .sec_level = SEC_LEVEL,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 5u, .max_len = 33u },

    {   .sid = NX_UDS_SID_REQUEST_UPLOAD,
        .handler = nx_uds_svc_transfer_request_upload, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_PROGRAMMING,
        .sec_level = SEC_LEVEL,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 5u, .max_len = 33u },

    {   /* No sub-function flag: the byte after the identifier is a counter. */
        .sid = NX_UDS_SID_TRANSFER_DATA,
        .handler = nx_uds_svc_transfer_data, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_PROGRAMMING,
        .sec_level = SEC_LEVEL,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 0u },

    {   .sid = NX_UDS_SID_REQUEST_TRANSFER_EXIT,
        .handler = nx_uds_svc_transfer_exit, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_PROGRAMMING,
        .sec_level = SEC_LEVEL,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 1u, .max_len = 0u }
};

static uint8_t g_req_buf[MAX_SDU];
static uint8_t g_out_buf[MAX_SDU];

/* ------------------------------------------------------------------ */
/* Building the whole stack                                           */
/* ------------------------------------------------------------------ */
/**
 * @brief  Bring up pool, queues, both transports, the server and the binding.
 *
 * The order matters in one place: the server is initialised before the binding,
 * because the binding is what installs itself as the server's output.
 */
static void setup(void)
{
    static const nx_tiered_level_cfg_t tiers[] = {
        { FRAME_BLOCK, 48 },
        { SDU_BLOCK,   10 },
    };
    nx_tiered_mem_pool_cfg_t pool_cfg;
    nx_can_isotp_cfg_t       tp_cfg;
    nx_uds_server_cfg_t      srv_cfg;
    nx_uds_svc_sec_cfg_t      sec_cfg;
    nx_uds_svc_transfer_cfg_t  xfer_cfg;
    nx_uds_tp_bind_cfg_t     bind_cfg;

    g_now_us         = 1000u;
    g_frames_carried = 0u;
    g_bus_blocked    = false;
    g_writes         = 0u;
    g_closes         = 0u;
    g_reset_asked    = 0u;
    g_unlocked       = false;
    g_rsp_len        = 0u;
    memset(g_mem, 0, sizeof(g_mem));

    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.memory      = g_pool_mem;
    pool_cfg.memory_size = sizeof(g_pool_mem);
    pool_cfg.tiers       = tiers;
    pool_cfg.tier_count  = sizeof(tiers) / sizeof(tiers[0]);
    assert(nx_tiered_mem_pool_init(&g_pool, &pool_cfg, NULL) == NX_TIERED_OK);

#define Q_INIT(q, buf) \
    assert(nx_ref_msg_queue_init((q), (buf), sizeof(buf) / sizeof((buf)[0])) \
           == NX_QUEUE_OK)
    Q_INIT(&g_ecu_sdu_rx_q, g_ecu_sdu_rx_buf);
    Q_INIT(&g_ecu_sdu_tx_q, g_ecu_sdu_tx_buf);
    Q_INIT(&g_ecu_can_rx_q, g_ecu_can_rx_buf);
    Q_INIT(&g_ecu_can_tx_q, g_ecu_can_tx_buf);
    Q_INIT(&g_tst_sdu_rx_q, g_tst_sdu_rx_buf);
    Q_INIT(&g_tst_sdu_tx_q, g_tst_sdu_tx_buf);
    Q_INIT(&g_tst_can_rx_q, g_tst_can_rx_buf);
    Q_INIT(&g_tst_can_tx_q, g_tst_can_tx_buf);
#undef Q_INIT

    /* The ECU: receives requests on REQ_ID, answers on RSP_ID, and listens on the
     * functional identifier as well. Confirmations are on, because the server needs
     * to be told what became of every answer it sent. */
    memset(&tp_cfg, 0, sizeof(tp_cfg));
    tp_cfg.max_frame_len = FRAME_LEN;
    tp_cfg.phys_rx_id    = REQ_ID;
    tp_cfg.phys_tx_id    = RSP_ID;
    tp_cfg.func_rx_id    = FUNC_ID;
    tp_cfg.pool          = &g_pool;
    tp_cfg.sdu_rx_queue  = &g_ecu_sdu_rx_q;
    tp_cfg.sdu_tx_queue  = &g_ecu_sdu_tx_q;
    tp_cfg.can_rx_queue  = &g_ecu_can_rx_q;
    tp_cfg.can_tx_queue  = &g_ecu_can_tx_q;
    tp_cfg.link          = LINK_ID;
    tp_cfg.confirm_tx    = true;
    tp_cfg.get_us        = mock_now;
    tp_cfg.rx_max_len    = MAX_SDU;
    assert(nx_can_isotp_init(&g_ecu_tp, &tp_cfg));

    /* The tester: the same pair the other way round, and the functional identifier
     * as something it transmits rather than listens to. It takes no confirmations,
     * so everything on its outbound queue is a message that arrived. */
    memset(&tp_cfg, 0, sizeof(tp_cfg));
    tp_cfg.max_frame_len = FRAME_LEN;
    tp_cfg.phys_rx_id    = RSP_ID;
    tp_cfg.phys_tx_id    = REQ_ID;
    tp_cfg.func_tx_id    = FUNC_ID;
    tp_cfg.pool          = &g_pool;
    tp_cfg.sdu_rx_queue  = &g_tst_sdu_rx_q;
    tp_cfg.sdu_tx_queue  = &g_tst_sdu_tx_q;
    tp_cfg.can_rx_queue  = &g_tst_can_rx_q;
    tp_cfg.can_tx_queue  = &g_tst_can_tx_q;
    tp_cfg.link          = LINK_ID;
    tp_cfg.get_us        = mock_now;
    tp_cfg.rx_max_len    = MAX_SDU;
    assert(nx_can_isotp_init(&g_tst_tp, &tp_cfg));

    /* The server. Its output is left unnamed: the binding installs itself as the
     * output once it has the server's address, which is why the two are set up in
     * this order rather than needing each other's addresses at once. */
    memset(&srv_cfg, 0, sizeof(srv_cfg));
    srv_cfg.services       = g_services;
    srv_cfg.services_count = (uint16_t)(sizeof(g_services) / sizeof(g_services[0]));
    srv_cfg.req_buf        = g_req_buf;
    srv_cfg.req_buf_size   = sizeof(g_req_buf);
    srv_cfg.out_buf        = g_out_buf;
    srv_cfg.out_buf_size   = sizeof(g_out_buf);
    srv_cfg.get_us         = mock_now;
    srv_cfg.link           = LINK_ID;
    /* Capped at what the path carries, which is what keeps the binding's too_long
     * counter at zero: an answer that would not fit is refused while it is being
     * built, with 0x14, rather than after it is finished. */
    srv_cfg.max_req_apdu   = MAX_SDU;
    srv_cfg.max_resp_apdu  = MAX_SDU;
    srv_cfg.p2_us          = P2_US;
    srv_cfg.p2_star_us     = P2_STAR_US;
    srv_cfg.p4_us          = P4_US;
    srv_cfg.s3_us          = S3_US;
    srv_cfg.out_fn         = NULL;
    assert(nx_uds_server_init(&g_srv, &srv_cfg));

    g_session_cfg.srv      = &g_srv;
    g_session_cfg.allow_fn = NULL;
    g_session_cfg.user     = NULL;

    memset(&sec_cfg, 0, sizeof(sec_cfg));
    sec_cfg.srv           = &g_srv;
    sec_cfg.levels        = g_sec_levels;
    sec_cfg.levels_count  = (uint8_t)(sizeof(g_sec_levels) / sizeof(g_sec_levels[0]));
    sec_cfg.seed_fn       = app_seed;
    sec_cfg.verify_fn     = app_verify;
    sec_cfg.granted_fn    = app_granted;
    sec_cfg.seed_buf      = g_seed_buf;
    sec_cfg.seed_buf_size = sizeof(g_seed_buf);
    assert(nx_uds_svc_sec_init(&g_sec, &sec_cfg));

    memset(&xfer_cfg, 0, sizeof(xfer_cfg));
    xfer_cfg.srv      = &g_srv;
    xfer_cfg.open_fn  = app_open;
    xfer_cfg.write_fn = app_write;
    xfer_cfg.read_fn  = app_read;
    xfer_cfg.close_fn = app_close;
    /* This product's flash write window is narrower than the link, which is the
     * ordinary case and the reason a client is told a block length at all: the
     * region then takes several blocks and the counter has to carry across them. */
    xfer_cfg.max_block_len = XFER_BLOCK_LEN;
    assert(nx_uds_svc_transfer_init(&g_xfer, &xfer_cfg));

    /* The binding, last: it wires the server's answers to the ECU transport. The
     * queues are named from the binding's point of view, which is the opposite of
     * the transport's - what the transport publishes is what the binding reads. */
    memset(&bind_cfg, 0, sizeof(bind_cfg));
    bind_cfg.srv         = &g_srv;
    bind_cfg.sdu_in      = &g_ecu_sdu_tx_q;
    bind_cfg.sdu_out     = &g_ecu_sdu_rx_q;
    bind_cfg.pool        = &g_pool;
    bind_cfg.link        = LINK_ID;
    bind_cfg.max_sdu_len = MAX_SDU;
    assert(nx_uds_tp_bind_init(&g_bind, &bind_cfg));
}

/** @brief Every counter the binding keeps, which on a working path is all zero. */
static void expect_no_losses(void)
{
    nx_uds_tp_bind_stats_t st;

    nx_uds_tp_bind_get_stats(&g_bind, &st);
    assert(st.busy == 0u);
    assert(st.refused == 0u);
    assert(st.no_memory == 0u);
    assert(st.too_long == 0u);
}

/* ------------------------------------------------------------------ */
/* 1. One frame each way                                              */
/* ------------------------------------------------------------------ */
static void demo_single_frame(void)
{
    static const uint8_t enter_extended[] = { 0x10u, NX_UDS_SESSION_EXTENDED };
    static const uint8_t tester_present[] = { 0x3Eu, NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB };

    printf("1. a request and its answer, one frame each\n");
    setup();

    assert(ask(enter_extended, sizeof(enter_extended)));
    show("10 03 ->");
    /* The session echoed back, then the two windows the server holds itself to:
     * P2 in milliseconds and P2* in tens of milliseconds, both big-endian. */
    assert(g_rsp_len == 6u);
    assert(g_rsp[0] == 0x50u && g_rsp[1] == NX_UDS_SESSION_EXTENDED);
    assert(((uint32_t)g_rsp[2] << 8 | g_rsp[3]) == P2_US / 1000u);
    assert(((uint32_t)g_rsp[4] << 8 | g_rsp[5]) == P2_STAR_US / 10000u);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    printf("  %-24sP2 %u ms, P2* %u ms\n", "windows ->",
           (unsigned)((uint32_t)g_rsp[2] << 8 | g_rsp[3]),
           (unsigned)(((uint32_t)g_rsp[4] << 8 | g_rsp[5]) * 10u));

    /* Six bytes of answer, so both directions were single frames: two frames on the
     * wire in total. */
    printf("  %-24s%u frames\n", "on the wire ->", g_frames_carried);
    assert(g_frames_carried == 2u);

    g_frames_carried = 0u;
    assert(ask(tester_present, sizeof(tester_present)));
    show("3E 00 ->");
    assert(g_rsp_len == 2u && g_rsp[0] == 0x7Eu);
    assert(g_frames_carried == 2u);
    expect_no_losses();
}

/* ------------------------------------------------------------------ */
/* 2. A flash sequence, segmented                                     */
/* ------------------------------------------------------------------ */
/** @brief Unlock the one level: ask for the seed, answer with the key. */
static void unlock(void)
{
    static const uint8_t seed_req[] = { 0x27u, NX_UDS_SVC_SEC_SEED_SUB(SEC_LEVEL) };
    uint8_t key_req[2u + SEC_KEY_LEN];
    uint8_t seed[SEC_SEED_LEN];
    uint32_t i;

    assert(ask(seed_req, sizeof(seed_req)));
    show("27 01 ->");
    assert(g_rsp_len == 2u + SEC_SEED_LEN);
    assert(g_rsp[0] == 0x67u && g_rsp[1] == NX_UDS_SVC_SEC_SEED_SUB(SEC_LEVEL));
    memcpy(seed, &g_rsp[2], SEC_SEED_LEN);

    key_req[0] = 0x27u;
    key_req[1] = NX_UDS_SVC_SEC_KEY_SUB(SEC_LEVEL);
    for (i = 0u; i < SEC_KEY_LEN; i++) {
        key_req[2u + i] = (uint8_t)~seed[i];
    }
    assert(ask(key_req, sizeof(key_req)));
    show("27 02 ->");
    assert(g_rsp_len == 2u && g_rsp[0] == 0x67u);
    assert(nx_uds_server_sec_level(&g_srv) == SEC_LEVEL);
    assert(g_unlocked);
}

static void demo_flash(void)
{
    static const uint8_t enter_prog[] = { 0x10u, NX_UDS_SESSION_PROGRAMMING };
    /* 32 bytes at MEM_BASE, both fields two bytes wide. The width byte carries the
     * length's width above and the address's below. */
    static const uint8_t open_dl[] = {
        0x34u, 0x00u, 0x22u, 0x80u, 0x00u, 0x00u, 0x20u
    };
    static const uint8_t exit_req[] = { 0x37u };
    uint32_t block_len;
    uint32_t room;
    uint32_t done;
    uint8_t  bsc;

    printf("2. a flash sequence over segmented frames\n");
    setup();

    assert(ask(enter_prog, sizeof(enter_prog)));
    assert(g_rsp[0] == 0x50u);
    unlock();

    /* Before the transfer opens, the request is refused for want of the level - so
     * ordering the sequence this way round is not a formality. */
    assert(ask(open_dl, sizeof(open_dl)));
    show("34 ->");
    /* The announcement is one byte wide here, because what the link carries fits in
     * one: the width is in the high half of the byte before the number. */
    assert(g_rsp_len == 3u && g_rsp[0] == 0x74u && g_rsp[1] == 0x10u);
    block_len = g_rsp[2];
    room      = nx_uds_svc_transfer_payload_room(block_len);
    printf("  %-24s%u bytes, %u of payload\n", "block ->",
           (unsigned)block_len, (unsigned)room);
    /* What the product named, not what the link would have allowed: the
     * announcement is the lower of the two, because announcing more would
     * announce a block that is then refused on arrival. */
    assert(block_len == XFER_BLOCK_LEN);
    assert(room == XFER_BLOCK_LEN - NX_UDS_SVC_TRANSFER_BLOCK_OVERHEAD);

    /* 32 bytes in blocks of whatever the announcement allows. Each block is longer
     * than a frame, so every one of them travels segmented and is paced by the
     * receiver's flow control. */
    done = 0u;
    bsc  = NX_UDS_SVC_TRANSFER_FIRST_BSC;
    while (done < 32u) {
        uint8_t req[2u + MAX_SDU];
        uint32_t n = (32u - done < room) ? (32u - done) : room;
        uint32_t i;

        req[0] = 0x36u;
        req[1] = bsc;
        for (i = 0u; i < n; i++) {
            req[2u + i] = (uint8_t)(done + i);
        }
        assert(ask(req, 2u + n));
        assert(g_rsp_len == 2u && g_rsp[0] == 0x76u && g_rsp[1] == bsc);
        done += n;
        bsc++;
    }
    show("last 36 ->");
    printf("  %-24s%u bytes in %u blocks\n", "written ->",
           (unsigned)done, g_writes);
    assert(done == 32u);
    /* 32 bytes in blocks of ten: three full ones and a short last one. */
    assert(g_writes == 4u);
    /* The counter ran 0x01..0x04, and the last answer echoed where it stopped. */
    assert(g_rsp[1] == 0x04u);

    assert(ask(exit_req, sizeof(exit_req)));
    show("37 ->");
    assert(g_rsp_len == 1u && g_rsp[0] == 0x77u);
    assert(g_closes == 1u);

    /* The bytes that went in are the bytes that are there. */
    {
        uint32_t i;

        for (i = 0u; i < 32u; i++) {
            assert(g_mem[i] == (uint8_t)i);
        }
    }
    printf("  %-24s32 bytes match\n", "memory ->");
    printf("  %-24s%u frames for the sequence\n", "on the wire ->",
           g_frames_carried);
    expect_no_losses();
}

/* ------------------------------------------------------------------ */
/* 3. A functional request, answered to one client                    */
/* ------------------------------------------------------------------ */
static void demo_functional(void)
{
    static const uint8_t tester_present[] = { 0x3Eu, NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB };
    static const uint8_t suppressed[] = {
        0x3Eu, NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB | NX_UDS_SUPPRESS_POS_RSP_BIT
    };
    static const uint8_t unknown_svc[] = { 0x22u, 0xF1u, 0x90u };
    nx_ref_msg_t *m = NULL;

    printf("3. a functionally addressed request\n");
    setup();

    /* Sent to every receiver on the link at once, and the answer comes back to this
     * client alone: ask_as checks that the message it gets is physically addressed,
     * which is the transport having sent it on the response identifier rather than
     * on the shared request one. */
    assert(ask_as(tester_present, sizeof(tester_present), NX_TP_TA_FUNCTIONAL));
    show("3E 00 (func) ->");
    assert(g_rsp_len == 2u && g_rsp[0] == 0x7Eu);
    printf("  %-24sphysical, on %03X\n", "answered ->", (unsigned)RSP_ID);

    /* The same request asking for silence. Nothing should come back at all, and the
     * loop below runs long enough for an answer to have arrived if one were coming. */
    assert(nx_can_isotp_send(&g_tst_tp, suppressed, sizeof(suppressed),
                             NX_TP_TA_FUNCTIONAL) == NX_CAN_ISOTP_OK);
    {
        unsigned i;

        for (i = 0u; i < 50u; i++) {
            turn();
            g_now_us += 1000u;
        }
    }
    assert(nx_queue_pop(&g_tst_sdu_tx_q, &m) != NX_QUEUE_OK);
    printf("  %-24snothing sent\n", "3E 80 (func) ->");

    /* A service this table does not carry. Addressed to one receiver it earns a
     * refusal; addressed to the whole link it earns silence, because every ECU on
     * the network would otherwise answer at once. */
    assert(ask(unknown_svc, sizeof(unknown_svc)));
    show("22 (phys) ->");
    assert(g_rsp_len == 3u && g_rsp[0] == 0x7Fu && g_rsp[2] == 0x11u);

    assert(nx_can_isotp_send(&g_tst_tp, unknown_svc, sizeof(unknown_svc),
                             NX_TP_TA_FUNCTIONAL) == NX_CAN_ISOTP_OK);
    {
        unsigned i;

        for (i = 0u; i < 50u; i++) {
            turn();
            g_now_us += 1000u;
        }
    }
    assert(nx_queue_pop(&g_tst_sdu_tx_q, &m) != NX_QUEUE_OK);
    printf("  %-24snothing sent\n", "22 (func) ->");
    expect_no_losses();
}

/* ------------------------------------------------------------------ */
/* 4. A link that stops taking frames, then starts again              */
/* ------------------------------------------------------------------ */
static void demo_back_pressure(void)
{
    static const uint8_t enter_extended[] = { 0x10u, NX_UDS_SESSION_EXTENDED };
    nx_uds_tp_bind_stats_t st;
    unsigned i;

    printf("4. a response the link cannot take yet\n");
    setup();

    /* The bus stops carrying what the ECU emits, so the answer is built, published,
     * and then sits in frames that go nowhere. */
    g_bus_blocked = true;
    assert(nx_can_isotp_send(&g_tst_tp, enter_extended, sizeof(enter_extended),
                             NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);
    for (i = 0u; i < 20u; i++) {
        turn();
        g_now_us += 1000u;
    }
    assert(g_rsp_len == 0u);
    printf("  %-24sheld, nothing lost\n", "blocked ->");

    /* Nothing was discarded while it was blocked: the answer is waiting rather than
     * gone, and the session has not changed hands. */
    nx_uds_tp_bind_get_stats(&g_bind, &st);
    assert(st.no_memory == 0u && st.too_long == 0u && st.busy == 0u);

    /* The link drains. The frames already emitted are carried and the answer
     * arrives, without the request having been sent again. */
    g_bus_blocked = false;
    {
        nx_ref_msg_t *m = NULL;

        assert(wait_for_response(&m));
        {
            const nx_tp_sdu_t *s = (const nx_tp_sdu_t *)nx_ref_msg_data(m);

            assert(s->len <= sizeof(g_rsp));
            memcpy(g_rsp, s->data, s->len);
            g_rsp_len = s->len;
        }
        nx_ref_msg_release(m);
    }
    show("drained ->");
    assert(g_rsp_len == 6u && g_rsp[0] == 0x50u);
    assert(nx_uds_server_session(&g_srv) == NX_UDS_SESSION_EXTENDED);
    printf("  %-24sthe answer arrived late, not twice\n", "recovered ->");
    expect_no_losses();
}

/* ------------------------------------------------------------------ */
/* 5. An upload, which is the answers carrying the payload            */
/* ------------------------------------------------------------------ */
static void demo_upload(void)
{
    static const uint8_t enter_prog[] = { 0x10u, NX_UDS_SESSION_PROGRAMMING };
    /* 40 bytes at MEM_BASE, so the answers are long enough to be segmented. */
    static const uint8_t open_ul[] = {
        0x35u, 0x00u, 0x22u, 0x80u, 0x00u, 0x00u, 0x28u
    };
    uint32_t i;
    uint32_t got;
    uint8_t  bsc;

    printf("5. an upload: the payload travels in the answers\n");
    setup();
    for (i = 0u; i < MEM_SIZE; i++) {
        g_mem[i] = (uint8_t)(0x50u + i);
    }

    assert(ask(enter_prog, sizeof(enter_prog)));
    unlock();

    assert(ask(open_ul, sizeof(open_ul)));
    show("35 ->");
    assert(g_rsp_len == 3u && g_rsp[0] == 0x75u);

    /* The request is the counter alone; the block comes back in the answer, which
     * is longer than a frame and so travels segmented in the other direction. */
    got = 0u;
    bsc = NX_UDS_SVC_TRANSFER_FIRST_BSC;
    while (got < 40u) {
        uint8_t req[2];
        uint32_t n;
        uint32_t j;

        req[0] = 0x36u;
        req[1] = bsc;
        assert(ask(req, sizeof(req)));
        assert(g_rsp_len > NX_UDS_SVC_TRANSFER_BLOCK_OVERHEAD);
        assert(g_rsp[0] == 0x76u && g_rsp[1] == bsc);

        n = g_rsp_len - NX_UDS_SVC_TRANSFER_BLOCK_OVERHEAD;
        for (j = 0u; j < n; j++) {
            assert(g_rsp[NX_UDS_SVC_TRANSFER_BLOCK_OVERHEAD + j]
                   == (uint8_t)(0x50u + got + j));
        }
        got += n;
        bsc++;
    }
    show("last 36 ->");
    /* Forty bytes in blocks of ten, every byte the one that was in memory. */
    assert(got == 40u);
    assert(bsc == NX_UDS_SVC_TRANSFER_FIRST_BSC + 4u);
    printf("  %-24s%u bytes read back in %u blocks\n", "payload ->",
           (unsigned)got, (unsigned)(bsc - NX_UDS_SVC_TRANSFER_FIRST_BSC));
    expect_no_losses();
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
int nx_uds_tp_bind_example_run(void)
{
    printf("=== nx_uds_tp_bind example ===\n");

    demo_single_frame();
    demo_flash();
    demo_functional();
    demo_back_pressure();
    demo_upload();

    printf("nx_uds_tp_bind example passed\n\n");
    return 0;
}
