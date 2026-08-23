/**
 * @file    nx_uds_transfer_example.c
 * @brief   Exercises the memory transfer services: 0x34, 0x35, 0x36, 0x37.
 *
 * One server, a table of the four transfer handlers, and a small array standing in
 * for the memory being written. What is checked is what a product cannot check for
 * itself: that a block arriving twice is written once, that a block out of turn does
 * not lose the transfer, that the length announced is one the link can carry, and
 * that the counter wraps where it should.
 */
#include "nx_middleware_examples.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/middleware/nx_uds_transfer.h"

#define LINK_ID     1u
#define P2_US       50000u
#define P2_STAR_US  5000000u
#define P4_US       20000000u
#define S3_US       5000000u

/* The link carries this much, so a block is this long including its two bytes of
 * overhead. */
#define REQ_APDU    18u
#define RSP_APDU    18u

#define MEM_SIZE    64u
#define MEM_BASE    0x8000u

/* ---- the clock ---- */
static uint32_t g_now;
static uint32_t mock_now(void) { return g_now; }

/* ---- the carrier ---- */
static uint8_t  g_sent[64];
static uint32_t g_sent_len;
static bool     g_sent_any;

static bool mock_out(void *user, uint8_t link, const uint8_t *rsp, uint32_t len,
                     uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;
    assert(len <= sizeof(g_sent));
    memcpy(g_sent, rsp, len);
    g_sent_len = len;
    g_sent_any = true;
    return true;
}

static void show(const char *what)
{
    uint32_t i;

    printf("  %-26s", what);
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

/* ---- the memory being written ---- */
static uint8_t  g_mem[MEM_SIZE];
static unsigned g_writes;          /* how many times a block was written */
static bool     g_write_fails;
static bool     g_close_fails;
static unsigned g_closes;

static nx_uds_server_t g_srv;
static nx_uds_xfer_t   g_xfer;

static bool app_write(void *user, nx_uds_addr_t addr, const uint8_t *data,
                      uint32_t len, uint8_t *nrc)
{
    (void)user;

    if (g_write_fails) {
        *nrc = NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE;
        return false;
    }
    assert(addr >= MEM_BASE && addr + len <= MEM_BASE + MEM_SIZE);
    memcpy(&g_mem[addr - MEM_BASE], data, len);
    g_writes++;
    return true;
}

static bool app_read(void *user, nx_uds_addr_t addr, uint8_t *out, uint32_t len,
                     uint8_t *nrc)
{
    (void)user; (void)nrc;
    assert(addr >= MEM_BASE && addr + len <= MEM_BASE + MEM_SIZE);
    memcpy(out, &g_mem[addr - MEM_BASE], len);
    return true;
}

static bool app_open(void *user, nx_uds_xfer_dir_t dir, nx_uds_addr_t addr,
                     nx_uds_addr_t size, uint8_t format, uint32_t *block_len,
                     uint8_t *nrc)
{
    (void)user; (void)dir; (void)block_len;

    /* This product stores what it is given, so anything asking to be uncompressed
     * or decrypted on the way in is out of range. */
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

static bool app_close(void *user, nx_uds_xfer_dir_t dir, nx_uds_addr_t done,
                      nx_uds_addr_t size, const uint8_t *record,
                      uint32_t record_len, uint8_t *out, uint32_t out_cap,
                      uint32_t *out_len, uint8_t *nrc)
{
    (void)user; (void)dir; (void)size; (void)record; (void)record_len;

    if (g_close_fails) {
        *nrc = NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE;
        return false;
    }
    g_closes++;
    /* Answer with how much was taken, which is this product's own choice of what to
     * put after the identifier. */
    if (out_cap >= 1u) {
        out[0]   = (uint8_t)done;
        *out_len = 1u;
    }
    return true;
}

/* ---- the table ---- */
static const nx_uds_service_t g_services[] = {
    {   .sid = NX_UDS_SID_REQUEST_DOWNLOAD,
        .handler = nx_uds_svc_request_download, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 5u, .max_len = 33u },
    {   .sid = NX_UDS_SID_REQUEST_UPLOAD,
        .handler = nx_uds_svc_request_upload, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 5u, .max_len = 33u },
    {   /* No sub-function flag: the byte after the identifier is a counter, and
         * reading its top bit as a request for silence would answer half of every
         * transfer with nothing. */
        .sid = NX_UDS_SID_TRANSFER_DATA,
        .handler = nx_uds_svc_transfer_data, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 2u, .max_len = 0u },
    {   .sid = NX_UDS_SID_REQUEST_TRANSFER_EXIT,
        .handler = nx_uds_svc_transfer_exit, .user = &g_xfer,
        .flags = 0u, .session_mask = NX_UDS_SESSION_MASK_ALL, .sec_level = 0u,
        .subs = NULL, .subs_count = 0u, .sub_session_masks = NULL,
        .min_len = 1u, .max_len = 0u }
};

static uint8_t g_req_buf[64];
static uint8_t g_out_buf[64];

static void setup(void)
{
    nx_uds_server_cfg_t cfg;
    nx_uds_xfer_cfg_t   xcfg;

    g_now = 1000u;
    g_writes = 0u;
    g_closes = 0u;
    g_write_fails = false;
    g_close_fails = false;
    memset(g_mem, 0, sizeof(g_mem));
    g_sent_len = 0u;
    g_sent_any = false;

    memset(&xcfg, 0, sizeof(xcfg));
    xcfg.srv      = &g_srv;
    xcfg.open_fn  = app_open;
    xcfg.write_fn = app_write;
    xcfg.read_fn  = app_read;
    xcfg.close_fn = app_close;
    assert(nx_uds_xfer_init(&g_xfer, &xcfg));

    memset(&cfg, 0, sizeof(cfg));
    cfg.services       = g_services;
    cfg.services_count = (uint16_t)(sizeof(g_services) / sizeof(g_services[0]));
    cfg.out_fn         = mock_out;
    cfg.req_buf        = g_req_buf;
    cfg.req_buf_size   = sizeof(g_req_buf);
    cfg.out_buf        = g_out_buf;
    cfg.out_buf_size   = sizeof(g_out_buf);
    cfg.get_us         = mock_now;
    cfg.link           = LINK_ID;
    cfg.max_req_apdu   = REQ_APDU;
    cfg.max_resp_apdu  = RSP_APDU;
    cfg.p2_us          = P2_US;
    cfg.p2_star_us     = P2_STAR_US;
    cfg.p4_us          = P4_US;
    cfg.s3_us          = S3_US;
    assert(nx_uds_server_init(&g_srv, &cfg));
}

/** @brief Hand over a request, let the answer reach the carrier, confirm it. */
static void submit(const uint8_t *req, uint32_t len)
{
    g_sent_len = 0u;
    g_sent_any = false;
    (void)nx_uds_server_indicate(&g_srv, req, len, (uint8_t)NX_TP_TA_PHYSICAL,
                                 LINK_ID);
    nx_uds_server_process(&g_srv);
    (void)nx_uds_server_confirm(&g_srv, LINK_ID, (uint8_t)NX_TP_N_OK);
}

/* A region of 32 bytes at MEM_BASE, addresses and lengths two bytes wide.
 * The width byte carries the length's width above and the address's below. */
static const uint8_t g_open_dl[] = {
    0x34u, 0x00u, 0x22u, 0x80u, 0x00u, 0x00u, 0x20u
};

/** @brief Send one block of a download, payload filled with a marker. */
static void send_block(uint8_t bsc, uint32_t payload, uint8_t marker)
{
    uint8_t req[2u + 32u];
    uint32_t i;

    assert(payload <= 32u);
    req[0] = 0x36u;
    req[1] = bsc;
    for (i = 0u; i < payload; i++) {
        req[2u + i] = (uint8_t)(marker + i);
    }
    submit(req, 2u + payload);
}

static void demo_download(void)
{
    printf("1. opening a download\n");
    setup();
    submit(g_open_dl, sizeof(g_open_dl));
    show("announced ->");
    /* The link carries 18 bytes, so that is the whole block including its two bytes
     * of overhead: the width byte says one byte follows, and it reads 18. */
    assert(g_sent_len == 3u);
    assert(g_sent[0] == 0x74u && g_sent[1] == 0x10u && g_sent[2] == REQ_APDU);
    printf("  %-26s%u bytes, %u of payload\n", "block ->",
           (unsigned)g_sent[2],
           (unsigned)nx_uds_xfer_payload_room(g_sent[2]));

    printf("2. carrying the blocks\n");
    send_block(0x01u, 16u, 0x10u);
    show("first ->");
    assert(g_sent_len == 2u && g_sent[0] == 0x76u && g_sent[1] == 0x01u);
    send_block(0x02u, 16u, 0x20u);
    show("second ->");
    assert(g_sent_len == 2u && g_sent[1] == 0x02u);
    assert(g_writes == 2u);
    /* The region is full: 32 bytes declared, 32 written. */
    {
        nx_uds_addr_t done = 0u;
        nx_uds_addr_t size = 0u;

        assert(nx_uds_xfer_progress(&g_xfer, &done, &size)
               == NX_UDS_XFER_DOWNLOAD);
        assert(done == 32u && size == 32u);
        printf("  %-26s%u of %u bytes\n", "progress ->", (unsigned)done,
               (unsigned)size);
    }
    assert(g_mem[0] == 0x10u && g_mem[15] == 0x1Fu);
    assert(g_mem[16] == 0x20u && g_mem[31] == 0x2Fu);

    printf("3. finishing\n");
    {
        static const uint8_t exit_req[] = { 0x37u };

        submit(exit_req, sizeof(exit_req));
        show("done ->");
        assert(g_sent_len == 2u && g_sent[0] == 0x77u && g_sent[1] == 32u);
        assert(g_closes == 1u);
        assert(nx_uds_xfer_progress(&g_xfer, NULL, NULL) == NX_UDS_XFER_NONE);
    }
}

static void demo_repeat(void)
{
    printf("4. a block that arrives twice is written once\n");
    setup();
    submit(g_open_dl, sizeof(g_open_dl));
    send_block(0x01u, 16u, 0x10u);
    assert(g_writes == 1u);

    /* The client did not hear the answer and sends the same block again. */
    send_block(0x01u, 16u, 0x10u);
    show("answered again ->");
    assert(g_sent_len == 2u && g_sent[0] == 0x76u && g_sent[1] == 0x01u);
    assert(g_writes == 1u);
    printf("  %-26sstill %u write\n", "writes ->", g_writes);
    {
        nx_uds_addr_t done = 0u;

        (void)nx_uds_xfer_progress(&g_xfer, &done, NULL);
        assert(done == 16u);
        printf("  %-26s%u bytes, not %u\n", "cursor ->", (unsigned)done, 32u);
    }

    /* And the transfer carries on from where it was. */
    send_block(0x02u, 16u, 0x20u);
    show("next ->");
    assert(g_sent[1] == 0x02u && g_writes == 2u);
    assert(g_mem[16] == 0x20u);

    printf("5. a block out of turn does not lose the transfer\n");
    setup();
    submit(g_open_dl, sizeof(g_open_dl));
    send_block(0x01u, 16u, 0x10u);

    /* Counter 4 when 2 was due: neither the next nor the last. */
    send_block(0x04u, 16u, 0x40u);
    show("0x73 ->");
    assert(g_sent_len == 3u && g_sent[0] == 0x7Fu && g_sent[1] == 0x36u
           && g_sent[2] == 0x73u);
    assert(g_writes == 1u);

    /* The transfer is still open, so the correct next block succeeds. */
    send_block(0x02u, 16u, 0x20u);
    show("recovered ->");
    assert(g_sent_len == 2u && g_sent[1] == 0x02u);
    assert(g_writes == 2u);
    printf("  %-26sthe transfer survived\n", "recovery ->");

    printf("6. a write the product refuses\n");
    setup();
    submit(g_open_dl, sizeof(g_open_dl));
    g_write_fails = true;
    send_block(0x01u, 16u, 0x10u);
    show("0x72 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x72u);
    /* Nothing advanced, so the same block may be sent again. */
    g_write_fails = false;
    send_block(0x01u, 16u, 0x10u);
    show("retried ->");
    assert(g_sent_len == 2u && g_sent[1] == 0x01u);
    assert(g_writes == 1u);
}

static void demo_refusals(void)
{
    printf("7. requests the services refuse\n");
    setup();
    {
        /* A block with no transfer open: the right service at the wrong time. */
        static const uint8_t block[] = { 0x36u, 0x01u, 0xAAu };
        static const uint8_t exit_req[] = { 0x37u };

        submit(block, sizeof(block));
        show("0x36 with none ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x24u);

        submit(exit_req, sizeof(exit_req));
        show("0x37 with none ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x24u);
    }
    {
        /* A width byte whose halves do not account for the request. */
        static const uint8_t bad_len[] = { 0x34u, 0x00u, 0x22u, 0x80u, 0x00u };

        submit(bad_len, sizeof(bad_len));
        show("0x13 ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x13u);
    }
    {
        /* A format this product does not apply, and a region outside its memory. */
        static const uint8_t compressed[] = {
            0x34u, 0x10u, 0x22u, 0x80u, 0x00u, 0x00u, 0x20u
        };
        static const uint8_t outside[] = {
            0x34u, 0x00u, 0x22u, 0x90u, 0x00u, 0x00u, 0x20u
        };

        submit(compressed, sizeof(compressed));
        show("0x31 format ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x31u);

        submit(outside, sizeof(outside));
        show("0x31 region ->");
        assert(g_sent_len == 3u && g_sent[2] == 0x31u);
    }

    printf("8. a second transfer while one is open\n");
    submit(g_open_dl, sizeof(g_open_dl));
    assert(g_sent[0] == 0x74u);
    submit(g_open_dl, sizeof(g_open_dl));
    show("0x22 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x22u);

    printf("9. a block larger than what was announced\n");
    setup();
    /* A product whose write window is narrower than the link says so, and the
     * announcement follows it. A block sized to the link rather than to the
     * announcement is then well formed and larger than what was promised. */
    {
        nx_uds_xfer_cfg_t xcfg;

        memset(&xcfg, 0, sizeof(xcfg));
        xcfg.srv           = &g_srv;
        xcfg.open_fn       = app_open;
        xcfg.write_fn      = app_write;
        xcfg.read_fn       = app_read;
        xcfg.close_fn      = app_close;
        xcfg.max_block_len = 10u;         /* 8 bytes of payload */
        assert(nx_uds_xfer_init(&g_xfer, &xcfg));
    }
    submit(g_open_dl, sizeof(g_open_dl));
    show("announced ->");
    assert(g_sent_len == 3u && g_sent[2] == 10u);

    send_block(0x01u, 16u, 0x10u);
    show("0x31 ->");
    assert(g_sent_len == 3u && g_sent[0] == 0x7Fu && g_sent[2] == 0x31u);
    assert(g_writes == 0u);

    /* A block of the size announced is taken. */
    send_block(0x01u, 8u, 0x10u);
    show("as announced ->");
    assert(g_sent_len == 2u && g_sent[1] == 0x01u);
    assert(g_writes == 1u);

    printf("10. writing past the region declared\n");
    setup();
    submit(g_open_dl, sizeof(g_open_dl));
    send_block(0x01u, 16u, 0x10u);
    send_block(0x02u, 16u, 0x20u);
    assert(g_writes == 2u);
    /* The 32 bytes declared are all written; there is nowhere for a third block. */
    send_block(0x03u, 16u, 0x30u);
    show("0x31 ->");
    assert(g_sent_len == 3u && g_sent[2] == 0x31u);
    assert(g_writes == 2u);
}

static void demo_upload(void)
{
    /* A region of 20 bytes, so the last block is a short one. */
    static const uint8_t open_ul[] = {
        0x35u, 0x00u, 0x22u, 0x80u, 0x00u, 0x00u, 0x14u
    };
    static const uint8_t ask[] = { 0x36u, 0x01u };
    static const uint8_t ask2[] = { 0x36u, 0x02u };
    uint32_t i;

    printf("11. an upload sends the bytes in its answers\n");
    setup();
    for (i = 0u; i < MEM_SIZE; i++) {
        g_mem[i] = (uint8_t)(0x50u + i);
    }
    submit(open_ul, sizeof(open_ul));
    show("announced ->");
    assert(g_sent_len == 3u && g_sent[0] == 0x75u && g_sent[2] == RSP_APDU);

    submit(ask, sizeof(ask));
    show("first ->");
    /* A whole block: two bytes of overhead and sixteen of payload. */
    assert(g_sent_len == 18u && g_sent[0] == 0x76u && g_sent[1] == 0x01u);
    assert(g_sent[2] == 0x50u && g_sent[17] == 0x5Fu);

    submit(ask2, sizeof(ask2));
    show("last, short ->");
    /* Four bytes left of the twenty declared. */
    assert(g_sent_len == 6u && g_sent[1] == 0x02u);
    assert(g_sent[2] == 0x60u && g_sent[5] == 0x63u);

    printf("12. an upload block that arrives twice reads the same bytes\n");
    setup();
    for (i = 0u; i < MEM_SIZE; i++) {
        g_mem[i] = (uint8_t)(0x50u + i);
    }
    submit(open_ul, sizeof(open_ul));
    submit(ask, sizeof(ask));
    {
        uint8_t first[64];
        uint32_t first_len = g_sent_len;

        memcpy(first, g_sent, g_sent_len);
        submit(ask, sizeof(ask));
        show("same again ->");
        assert(g_sent_len == first_len);
        assert(memcmp(first, g_sent, first_len) == 0);
        printf("  %-26sidentical\n", "bytes ->");
    }
}

static void demo_wrap(void)
{
    /* One byte per block, so the counter is what runs out rather than the region. */
    static const uint8_t open_small[] = {
        0x34u, 0x00u, 0x22u, 0x80u, 0x00u, 0x01u, 0x04u   /* 260 bytes */
    };
    unsigned i;
    uint8_t bsc;

    printf("13. the counter wraps 0xFF to 0x00\n");
    setup();
    /* The region is larger than the memory behind it, so this transfer is opened
     * without the product's opinion: what is being watched is the counter. */
    {
        nx_uds_xfer_cfg_t xcfg;

        memset(&xcfg, 0, sizeof(xcfg));
        xcfg.srv      = &g_srv;
        xcfg.open_fn  = NULL;
        xcfg.write_fn = app_write;
        xcfg.read_fn  = app_read;
        assert(nx_uds_xfer_init(&g_xfer, &xcfg));
    }
    submit(open_small, sizeof(open_small));
    assert(g_sent[0] == 0x74u);

    /* 0x01 through 0xFF, then 0x00, then 0x01 again. One byte each, and the memory
     * behind it is written in a circle so the addresses stay in range. */
    bsc = 0x01u;
    for (i = 0u; i < 256u; i++) {
        uint8_t req[3];

        req[0] = 0x36u;
        req[1] = bsc;
        req[2] = (uint8_t)i;
        g_xfer.run.done = 0u;         /* keep the writes inside the array */
        submit(req, 3u);
        assert(g_sent_len == 2u);
        assert(g_sent[1] == bsc);
        bsc = (uint8_t)(bsc + 1u);
    }
    /* 256 blocks starting at 0x01 ends having just committed 0x00. */
    printf("  %-26s0x01..0xFF then 0x00, %u blocks\n", "counter ->", 256u);
    assert(g_xfer.run.bsc_last == 0x00u);
    assert(g_xfer.run.bsc_next == 0x01u);
}

int nx_uds_transfer_example_run(void)
{
    printf("=== nx_uds_transfer example ===\n");

    demo_download();
    demo_repeat();
    demo_refusals();
    demo_upload();
    demo_wrap();

    printf("nx_uds_transfer example passed\n\n");
    return 0;
}

