/**
 * @file    nx_can_isotp_example.c
 * @brief   Usage example for nx_can_isotp: segmented transport over CAN.
 *
 * Shows the intended architecture end to end, with no real hardware. Everything
 * crossing a module boundary is an nx_ref_msg on a caller-owned queue:
 *
 *      CAN driver RX                                        upper layer
 *           |                                                    ^
 *           | can_rx_queue                                       | sdu_tx_queue
 *           v                                                    |
 *        [ nx_can_isotp ] --reassemble--> SDU ---------------->---+
 *           |     ^
 *  can_tx_q |     | sdu_rx_queue
 *           v     |
 *      CAN driver TX <--segment-- SDU <-- nx_can_isotp_send(...)
 *
 * Every queue is named from the module's point of view: it reads sdu_rx_queue
 * and can_rx_queue, and writes sdu_tx_queue and can_tx_queue.
 *
 * nx_can_isotp_process() is the only thing that moves: it drains can_rx_queue,
 * advances both directions, and fills can_tx_queue. The example plays the peer,
 * pushing the frames the module should receive and inspecting the ones it emits.
 *
 * The IDs are a physical pair plus a functional request ID, as ISO 15765-2
 * addressing requires: the module receives on PHYS_RX_ID and answers on
 * PHYS_TX_ID - including the flow control it sends while receiving.
 *
 * Twelve scenarios are exercised:
 *   1. Receive a single frame                  -> one SDU handed up, no FC
 *   2. Receive a segmented message (FF + CFs)  -> FC on the transmit ID carrying
 *                                                 the configured BS/STmin, and a
 *                                                 fresh FC when a block runs out
 *   3. Receive more than rx_max_len announced  -> refused with FC.OVERFLOW
 *   4. Receive on the functional ID            -> single frames only
 *   5. Send a short message                    -> one SF on the wire
 *   6. Send a long message (FF, FC, CFs)       -> SN order, STmin pacing, and
 *                                                 one-frame-per-process pacing
 *   7. A peer that keeps saying WAIT           -> held, then abandoned
 *   8. A second instance configured to confirm -> each transmission and each
 *                                                 failed reception reported by
 *                                                 kind and result
 *   9. A length no allocation can express      -> refused on send, and refused
 *                                                 with FC.OVERFLOW on receive
 *  10. A transmit queue that fills up, then    -> the frame is offered again and
 *      drains                                      the conversation carries on
 *  11. A transmit queue that never drains      -> N_As and N_Ar expire and each
 *                                                 side reports TIMEOUT_A
 *  12. A functionally addressed send            -> one SF on the functional ID,
 *                                                 no flow control involved
 *
 * Padding is on throughout, so every emitted frame is a full 8 bytes with 0xCC
 * in whatever tail the protocol data does not fill.
 *
 * All storage is static; the example self-checks with asserts and prints the wire.
 */
#include "nx_middleware_examples.h"
#include "src/middleware/nx_can_isotp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Diagnostic IDs for this example: the tester addresses this instance on
 * PHYS_RX_ID and it answers on PHYS_TX_ID; FUNC_RX_ID is the broadcast request
 * ID every ECU on the bus listens to, and FUNC_TX_ID is the same request ID as
 * seen from the tester's side - a network has at most one functional sender, so
 * only the instance that plays the tester configures it. The module never
 * interprets the layout. */
#define PHYS_RX_ID 0x7E0u
#define PHYS_TX_ID 0x7E8u
#define FUNC_RX_ID 0x7DFu
#define FUNC_TX_ID 0x7DFu

/* Copied into every SDU the module publishes, so one upper layer can serve
 * several instances from one queue. */
#define LINK_ID 3u

/* Flow control this instance advertises while receiving: two consecutive frames
 * per block, 10 ms of separation between them, and a cap on how long a message
 * may be - the SDU tier holds 64 bytes, so accepting more would be a promise the
 * pool cannot keep. */
#define RX_BLOCK_SIZE 2u
#define RX_STMIN      0x0Au
#define RX_MAX_LEN    64u

/* Network layer timeouts, short enough to step through on the mock clock. */
#define N_BS_US    50000u
#define N_CR_US    50000u
#define N_WFT_MAX  2u

/* This example fills every emitted frame out to 8 bytes, as many diagnostic
 * networks require, and writes 0xCC into the unused tail. */
#define PAD_BYTE 0xCCu

/* A second pair of IDs, for the instance that checks the length ceiling. */
#define CEIL_RX_ID 0x710u
#define CEIL_TX_ID 0x718u

/* A third instance, for the two scenarios that block the link. Its transmit
 * queue holds a single frame, so one unclaimed frame is enough to fill it. */
#define LINK_RX_ID 0x720u
#define LINK_TX_ID 0x728u

/* Long enough to see a retry cross it, short enough to reach in one step. */
#define N_AS_US 30000u
#define N_AR_US 30000u

/* Wire constants the example needs because it plays the peer: it hand-builds the
 * frames the module receives and checks the first byte of the ones it emits. The
 * frame type occupies the high nibble, and a flow control frame carries its flow
 * status in the low nibble of that same byte. */
#define PEER_PCI_SF 0x00u
#define PEER_PCI_FF 0x10u
#define PEER_PCI_CF 0x20u
#define PEER_PCI_FC 0x30u

#define PEER_FS_CTS   0x00u
#define PEER_FS_WAIT  0x01u
#define PEER_FS_OVFLW 0x02u

/* ------------------------------------------------------------------ */
/* Mock plumbing: a pool, four queues, and a clock the example steers  */
/* ------------------------------------------------------------------ */
/* A typical application owns one bus, so this is module-level state the
 * callbacks reach directly - get_us takes no context. */
static uint32_t g_clock_us;

static uint32_t mock_get_us(void)
{
    return g_clock_us;
}

/* The pool sizes two block classes: CAN frames and SDUs. A block must hold the
 * nx_ref_msg header as well as the payload, since the two are one allocation. */
#define FRAME_BLOCK (sizeof(nx_ref_msg_t) + sizeof(nx_can_msg_t) + NX_CAN_MAX_CLASSIC_LEN)
#define SDU_BLOCK   (sizeof(nx_ref_msg_t) + sizeof(nx_can_isotp_sdu_t) + 64u)

static uint8_t  g_pool_mem[8192];
static nx_tiered_mem_pool_t g_pool;

/* Queue storage: each holds nx_ref_msg_t pointers. */
static nx_ref_msg_t *g_sdu_tx_q_buf[8];
static nx_ref_msg_t *g_sdu_rx_q_buf[4];
static nx_ref_msg_t *g_can_rx_q_buf[16];
static nx_ref_msg_t *g_can_tx_q_buf[16];
static nx_queue_t g_sdu_tx_q, g_sdu_rx_q, g_can_rx_q, g_can_tx_q;

static nx_can_isotp_t g_iso;

/**
 * @brief  Push one CAN frame onto the receive queue, as a driver's ISR would.
 *
 * The frame is allocated from the same pool the module uses, so a frame that is
 * consumed and released leaves the pool exactly as it found it.
 */
static void feed_frame(uint32_t id, const uint8_t *payload, size_t len)
{
    assert(len <= NX_CAN_MAX_CLASSIC_LEN);

    nx_ref_msg_t *m = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + len);
    assert(m != NULL);

    nx_can_msg_t *f = (nx_can_msg_t *)nx_ref_msg_data(m);
    f->id             = id;
    f->flags.raw      = 0u;
    f->timestamp      = 0u;
    f->flags.bits.dlc = nx_can_len_to_dlc((uint32_t)len);
    memcpy(f->data, payload, len);

    assert(nx_ref_msg_publish(m, &g_can_rx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(m);          /* the queue holds the only reference now */
}

/**
 * @brief  Pop one frame the module emitted, or NULL when the wire is quiet.
 *
 * The caller owns the returned reference and releases it when done, exactly as
 * a real CAN driver would after transmitting.
 */
static nx_ref_msg_t *pop_can_frame(void)
{
    nx_ref_msg_t *m = NULL;
    if (nx_queue_pop(&g_can_tx_q, &m) != NX_QUEUE_OK) {
        return NULL;
    }
    return m;
}

/** @brief Print one emitted frame as it would appear on the bus. */
static void print_frame(const char *tag, const nx_can_msg_t *f)
{
    size_t len = nx_can_dlc_to_len(f->flags.bits.dlc);
    printf("  %-8s %03X [%zu] ", tag, (unsigned)f->id, len);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", f->data[i]);
    }
    printf("\n");
}

/**
 * @brief  Pop the flow control frame the module emitted and check it against the
 *         configuration it is supposed to advertise.
 */
static void expect_flow_control(void)
{
    nx_ref_msg_t *m = pop_can_frame();
    assert(m != NULL);
    const nx_can_msg_t *fc = (const nx_can_msg_t *)nx_ref_msg_data(m);
    print_frame("FC ->", fc);

    /* Flow control belongs to this instance's transmit ID, not to the ID the
     * message arrived on - that one belongs to the peer. */
    assert(fc->id == PHYS_TX_ID);
    assert(fc->data[0] == (PEER_PCI_FC | PEER_FS_CTS));
    assert(fc->data[1] == RX_BLOCK_SIZE);
    assert(fc->data[2] == RX_STMIN);
    /* Three header bytes are all a flow control frame carries, so with padding
     * on it is filled out to a full 8-byte frame. */
    assert(nx_can_dlc_to_len(fc->flags.bits.dlc) == 8u);
    for (size_t i = 3u; i < 8u; i++) {
        assert(fc->data[i] == PAD_BYTE);
    }
    nx_ref_msg_release(m);
}

/* ------------------------------------------------------------------ */
/* 1. Receive a single frame                                          */
/* ------------------------------------------------------------------ */
static void demo_receive_single_frame(void)
{
    printf("1. receive a single frame\n");

    /* SF, 2 bytes: a UDS "read DTC information" style request. */
    const uint8_t sf[] = {0x02u, 0x19u, 0x02u};
    feed_frame(PHYS_RX_ID, sf, sizeof(sf));
    nx_can_isotp_process(&g_iso);

    /* A single frame is complete on arrival, so it needs no flow control. */
    assert(pop_can_frame() == NULL);

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_sdu_tx_q, &m) == NX_QUEUE_OK);

    const nx_can_isotp_sdu_t *sdu = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(sdu->ta_type == NX_TP_TA_PHYSICAL);
    assert(sdu->kind == NX_TP_SDU_INDICATION);
    assert(sdu->result == NX_TP_N_OK);
    assert(sdu->link == LINK_ID);
    assert(sdu->len == 2u);
    assert(sdu->data[0] == 0x19u && sdu->data[1] == 0x02u);
    printf("  got %u bytes on %03X: %02X %02X\n",
           (unsigned)sdu->len, (unsigned)PHYS_RX_ID, sdu->data[0], sdu->data[1]);

    nx_ref_msg_release(m);
    assert(nx_queue_is_empty(&g_sdu_tx_q));
}

/* ------------------------------------------------------------------ */
/* 2. Receive a segmented message                                     */
/* ------------------------------------------------------------------ */
static void demo_receive_segmented(void)
{
    printf("2. receive a segmented message (FF + CF)\n");

    /* 27 bytes announced: 6 ride in the FF, the rest follow in three CFs. */
    uint8_t expect[27];
    for (size_t i = 0; i < sizeof(expect); i++) {
        expect[i] = (uint8_t)(0x50u + i);
    }

    uint8_t ff[8] = {0x10u, (uint8_t)sizeof(expect)};   /* FF, FF_DL = 27 */
    memcpy(ff + 2, expect, 6);
    feed_frame(PHYS_RX_ID, ff, sizeof(ff));
    nx_can_isotp_process(&g_iso);

    /* The receiver answers an FF with the flow control it advertises. */
    expect_flow_control();

    /* Consecutive frames carry SN 1, 2, ... in the header's low nibble. */
    size_t   done = 6u;
    unsigned sn   = 1u;
    unsigned fcs  = 0u;
    while (done < sizeof(expect)) {
        uint8_t cf[8];
        size_t  chunk = sizeof(expect) - done;
        if (chunk > 7u) {
            chunk = 7u;
        }
        cf[0] = (uint8_t)(PEER_PCI_CF | (sn & 0x0Fu));
        memcpy(cf + 1, expect + done, chunk);
        feed_frame(PHYS_RX_ID, cf, chunk + 1u);
        nx_can_isotp_process(&g_iso);

        done += chunk;
        sn++;

        if (done < sizeof(expect)) {
            assert(nx_queue_is_empty(&g_sdu_tx_q));    /* still short of FF_DL */
            /* A block that has run out earns a fresh flow control frame. */
            if (((sn - 1u) % RX_BLOCK_SIZE) == 0u) {
                expect_flow_control();
                fcs++;
            } else {
                assert(pop_can_frame() == NULL);
            }
        }
    }
    printf("  %u further flow control frame(s) issued mid-message\n", fcs);

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *sdu = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(sdu->ta_type == NX_TP_TA_PHYSICAL);
    assert(sdu->kind == NX_TP_SDU_INDICATION);
    assert(sdu->len == sizeof(expect));
    assert(memcmp(sdu->data, expect, sizeof(expect)) == 0);
    printf("  reassembled %u bytes\n", (unsigned)sdu->len);
    nx_ref_msg_release(m);

    /* The finished conversation emits nothing further. */
    assert(pop_can_frame() == NULL);
}

/* ------------------------------------------------------------------ */
/* 3. Refuse a message longer than the configured limit               */
/* ------------------------------------------------------------------ */
static void demo_receive_too_long(void)
{
    printf("3. refuse a message longer than rx_max_len\n");

    /* A first frame announcing 300 bytes, well past the 64 this instance
     * accepts. The decision is made on the announcement alone. */
    const uint16_t announced = 300u;
    uint8_t ff[8] = {(uint8_t)(PEER_PCI_FF | (announced >> 8)),
                     (uint8_t)announced};
    memset(ff + 2, 0x77u, 6);
    feed_frame(PHYS_RX_ID, ff, sizeof(ff));
    nx_can_isotp_process(&g_iso);

    /* The peer is told to stop rather than left waiting for its own timeout. */
    nx_ref_msg_t *m = pop_can_frame();
    assert(m != NULL);
    const nx_can_msg_t *fc = (const nx_can_msg_t *)nx_ref_msg_data(m);
    print_frame("FC ->", fc);
    assert(fc->id == PHYS_TX_ID);
    assert(fc->data[0] == (PEER_PCI_FC | PEER_FS_OVFLW));
    nx_ref_msg_release(m);

    /* Nothing was allocated for it and no reception is in progress, so a
     * consecutive frame that follows finds no conversation to join. */
    const uint8_t cf[] = {PEER_PCI_CF | 0x01u, 0x11u, 0x22u};
    feed_frame(PHYS_RX_ID, cf, sizeof(cf));
    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);
    assert(nx_queue_is_empty(&g_sdu_tx_q));
    printf("  %u bytes announced, %u accepted: refused with OVERFLOW\n",
           (unsigned)announced, (unsigned)RX_MAX_LEN);
}

/* ------------------------------------------------------------------ */
/* 4. Receive on the functional ID                                    */
/* ------------------------------------------------------------------ */
static void demo_receive_functional(void)
{
    printf("4. receive on the functional (1:N) ID\n");

    /* A functionally addressed request reaches every ECU at once. */
    const uint8_t sf[] = {0x02u, 0x10u, 0x03u};
    feed_frame(FUNC_RX_ID, sf, sizeof(sf));

    /* A first frame on the same ID is not accepted: several receivers would
     * answer flow control on one ID and collide. */
    const uint8_t ff[] = {0x10u, 0x14u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u};
    feed_frame(FUNC_RX_ID, ff, sizeof(ff));

    /* A frame for somebody else's ID pair is dropped without a trace. */
    const uint8_t other[] = {0x02u, 0x3Eu, 0x00u};
    feed_frame(0x7E1u, other, sizeof(other));

    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);       /* no flow control on any of them */

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *sdu = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    /* Addressing type, not the raw ID, is how the upper layer tells them apart. */
    assert(sdu->ta_type == NX_TP_TA_FUNCTIONAL);
    assert(sdu->kind == NX_TP_SDU_INDICATION);
    assert(sdu->len == 2u);
    printf("  got %u bytes, functionally addressed\n", (unsigned)sdu->len);
    nx_ref_msg_release(m);

    assert(nx_queue_is_empty(&g_sdu_tx_q));    /* the FF and the stray frame are gone */
}

/* ------------------------------------------------------------------ */
/* 5. Send a short message                                            */
/* ------------------------------------------------------------------ */
static void demo_send_single_frame(void)
{
    printf("5. send a short message\n");

    const uint8_t rsp[] = {0x59u, 0x02u, 0xFFu};
    assert(nx_can_isotp_send(&g_iso, rsp, sizeof(rsp), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);

    /* Nothing leaves the module until it is driven. */
    assert(pop_can_frame() == NULL);
    nx_can_isotp_process(&g_iso);

    nx_ref_msg_t *m = pop_can_frame();
    assert(m != NULL);
    const nx_can_msg_t *f = (const nx_can_msg_t *)nx_ref_msg_data(m);
    print_frame("SF ->", f);
    assert(f->id == PHYS_TX_ID);
    assert(f->data[0] == (PEER_PCI_SF | sizeof(rsp)));
    assert(memcmp(f->data + 1, rsp, sizeof(rsp)) == 0);
    /* Padding is on, so a 4-byte frame is filled out to 8. */
    assert(nx_can_dlc_to_len(f->flags.bits.dlc) == 8u);
    for (size_t i = 1u + sizeof(rsp); i < 8u; i++) {
        assert(f->data[i] == PAD_BYTE);
    }
    nx_ref_msg_release(m);

    /* A single frame completes the send outright: the module is idle again. */
    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);
}

/* ------------------------------------------------------------------ */
/* 6. Send a segmented message                                        */
/* ------------------------------------------------------------------ */
static void demo_send_segmented(void)
{
    printf("6. send a segmented message (FF + FC + CF)\n");

    /* 18 bytes: 6 in the FF, then 7 and a short final 5. */
    uint8_t payload[18];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xA0u + i);
    }
    assert(nx_can_isotp_send(&g_iso, payload, sizeof(payload), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);

    /* First process() emits the FF and then waits: the peer has not yet agreed
     * to receive, so no CF may follow. */
    nx_can_isotp_process(&g_iso);
    nx_ref_msg_t *m = pop_can_frame();
    assert(m != NULL);
    const nx_can_msg_t *ff = (const nx_can_msg_t *)nx_ref_msg_data(m);
    print_frame("FF ->", ff);
    assert(ff->id == PHYS_TX_ID);
    assert(nx_can_dlc_to_len(ff->flags.bits.dlc) == 8u);
    assert(ff->data[0] == (PEER_PCI_FF | (sizeof(payload) >> 8)));
    assert(ff->data[1] == (uint8_t)sizeof(payload));
    assert(memcmp(ff->data + 2, payload, 6) == 0);
    nx_ref_msg_release(m);

    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);       /* held: no flow control yet */

    /* The peer clears the sender to continue, asking for 1 ms between frames.
     * Its flow control arrives on the ID this instance receives on. */
    const uint8_t fc[] = {PEER_PCI_FC | PEER_FS_CTS,
                          0x00u,   /* BS = 0: no block limit */
                          0x01u};  /* STmin = 1 ms */
    feed_frame(PHYS_RX_ID, fc, sizeof(fc));

    /* tx_frames_per_process is 1, so each call emits at most one CF - and only
     * once STmin has elapsed on the mock clock. */
    size_t sent = 6u;
    unsigned expect_sn = 1u;
    while (sent < sizeof(payload)) {
        nx_can_isotp_process(&g_iso);
        nx_ref_msg_t *cm = pop_can_frame();
        if (cm == NULL) {
            g_clock_us += 250u;            /* STmin not elapsed: let time pass */
            continue;
        }
        const nx_can_msg_t *cf = (const nx_can_msg_t *)nx_ref_msg_data(cm);
        print_frame("CF ->", cf);
        assert(cf->id == PHYS_TX_ID);
        assert(cf->data[0] == (PEER_PCI_CF | (expect_sn & 0x0Fu)));

        /* Every frame is a full 8 bytes here, so what is payload and what is
         * filler is known from how much of the message is left, not from the
         * frame length. */
        size_t chunk = sizeof(payload) - sent;
        if (chunk > 7u) {
            chunk = 7u;
        }
        assert(nx_can_dlc_to_len(cf->flags.bits.dlc) == 8u);
        assert(memcmp(cf->data + 1, payload + sent, chunk) == 0);
        for (size_t i = 1u + chunk; i < 8u; i++) {
            assert(cf->data[i] == PAD_BYTE);
        }
        sent += chunk;
        expect_sn++;
        nx_ref_msg_release(cm);

        /* One frame per call is the configured pacing. */
        assert(pop_can_frame() == NULL);
    }
    assert(sent == sizeof(payload));
    printf("  sent %zu bytes in %u frames, tail filled with %02X\n",
           sent, expect_sn, PAD_BYTE);

    /* The conversation is over; further calls emit nothing. */
    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);
}

/* ------------------------------------------------------------------ */
/* 7. A peer that keeps asking to wait                                */
/* ------------------------------------------------------------------ */
static void demo_send_wait_limit(void)
{
    printf("7. peer holds the sender with FC.WAIT\n");

    uint8_t payload[20];
    memset(payload, 0x5Au, sizeof(payload));
    assert(nx_can_isotp_send(&g_iso, payload, sizeof(payload), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);

    nx_can_isotp_process(&g_iso);
    nx_ref_msg_t *m = pop_can_frame();
    assert(m != NULL);
    print_frame("FF ->", (const nx_can_msg_t *)nx_ref_msg_data(m));
    nx_ref_msg_release(m);

    const uint8_t wait[] = {PEER_PCI_FC | PEER_FS_WAIT, 0u, 0u};

    /* Each WAIT buys the peer another N_Bs window, so the transmission survives
     * well past the timeout it started with. */
    for (unsigned i = 0; i < N_WFT_MAX; i++) {
        g_clock_us += N_BS_US - 10000u;
        feed_frame(PHYS_RX_ID, wait, sizeof(wait));
        nx_can_isotp_process(&g_iso);
        assert(pop_can_frame() == NULL);       /* held, but still alive */
    }
    printf("  survived %u WAIT frames over %u us\n",
           (unsigned)N_WFT_MAX, (unsigned)((N_BS_US - 10000u) * N_WFT_MAX));

    /* One WAIT too many, and the transmission is abandoned. */
    g_clock_us += N_BS_US - 10000u;
    feed_frame(PHYS_RX_ID, wait, sizeof(wait));
    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);

    /* A clear-to-send now finds no transmission to clear. */
    const uint8_t cts[] = {PEER_PCI_FC | PEER_FS_CTS, 0u, 0u};
    feed_frame(PHYS_RX_ID, cts, sizeof(cts));
    nx_can_isotp_process(&g_iso);
    assert(pop_can_frame() == NULL);
    printf("  abandoned after %u; the module is idle again\n",
           (unsigned)(N_WFT_MAX + 1u));

    /* Idle means ready: the next message goes out normally. */
    const uint8_t rsp[] = {0x7Fu, 0x10u, 0x78u};
    assert(nx_can_isotp_send(&g_iso, rsp, sizeof(rsp), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);
    nx_can_isotp_process(&g_iso);
    nx_ref_msg_t *sfm = pop_can_frame();
    assert(sfm != NULL);
    const nx_can_msg_t *sf = (const nx_can_msg_t *)nx_ref_msg_data(sfm);
    print_frame("SF ->", sf);
    assert(sf->id == PHYS_TX_ID);
    assert(sf->data[0] == (PEER_PCI_SF | sizeof(rsp)));
    nx_ref_msg_release(sfm);
}

/* ------------------------------------------------------------------ */
/* 8. Outcome reporting                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief  A second instance, configured to confirm, reports how each
 *         transmission and reception ended.
 *
 * Confirmations share the queue that carries received messages, so the upper
 * layer reads @c kind to tell "a message arrived" from "a message I sent has
 * finished". This instance has its own IDs and queues, leaving the first one
 * untouched.
 */
static void demo_confirmations(void)
{
    printf("8. report how a transmission ended\n");

    static nx_ref_msg_t *sdu_tx_buf[8], *sdu_rx_buf[4];
    static nx_ref_msg_t *can_rx_buf[16], *can_tx_buf[16];
    static nx_queue_t    sdu_tx_q, sdu_rx_q, can_rx_q, can_tx_q;
    static nx_can_isotp_t iso;

    assert(nx_ref_msg_queue_init(&sdu_tx_q, sdu_tx_buf, 8) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&sdu_rx_q, sdu_rx_buf, 4) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&can_rx_q, can_rx_buf, 16) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&can_tx_q, can_tx_buf, 16) == NX_QUEUE_OK);

    const nx_can_isotp_cfg_t cfg = {
        .max_frame_len = NX_CAN_ISOTP_FRAME_8,
        .phys_rx_id    = 0x700u,
        .phys_tx_id    = 0x708u,
        .pool          = &g_pool,
        .sdu_rx_queue  = &sdu_rx_q,
        .sdu_tx_queue  = &sdu_tx_q,
        .link          = 9u,
        .confirm_tx    = true,          /* report every outcome */
        .can_rx_queue  = &can_rx_q,
        .can_tx_queue  = &can_tx_q,
        .get_us        = mock_get_us,
        .n_bs_us       = N_BS_US,
        .n_cr_us       = N_CR_US,
        .rx_max_len    = RX_MAX_LEN,
    };
    assert(nx_can_isotp_init(&iso, &cfg));

    /* Drop every frame this instance emits: the wire is not what is under test. */
    #define DRAIN_LOCAL() do {                                       \
        nx_ref_msg_t *f;                                             \
        while (nx_queue_pop(&can_tx_q, &f) == NX_QUEUE_OK) {         \
            nx_ref_msg_release(f);                                   \
        }                                                            \
    } while (0)

    /* Pop one SDU and check what it reports. */
    #define EXPECT_SDU(k, r) do {                                             \
        nx_ref_msg_t *sm = NULL;                                              \
        assert(nx_queue_pop(&sdu_tx_q, &sm) == NX_QUEUE_OK);                  \
        const nx_tp_sdu_t *sd = (const nx_tp_sdu_t *)nx_ref_msg_data(sm);     \
        assert(sd->kind == (k));                                              \
        assert(sd->result == (r));                                            \
        assert(sd->link == 9u);                                               \
        nx_ref_msg_release(sm);                                               \
    } while (0)

    /* --- a single frame goes out and is confirmed --- */
    const uint8_t sf[] = {0x50u, 0x03u};
    assert(nx_can_isotp_send(&iso, sf, sizeof(sf), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);
    nx_can_isotp_process(&iso);
    DRAIN_LOCAL();
    EXPECT_SDU(NX_TP_SDU_CONFIRM, NX_TP_N_OK);
    printf("  single frame sent      -> CONFIRM / N_OK\n");

    /* --- flow control carrying a reserved status ends the transmission --- */
    uint8_t big[20];
    memset(big, 0x33u, sizeof(big));
    assert(nx_can_isotp_send(&iso, big, sizeof(big), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);
    nx_can_isotp_process(&iso);            /* FF out, now awaiting flow control */
    DRAIN_LOCAL();

    const uint8_t bad_fs[] = {PEER_PCI_FC | 0x03u, 0u, 0u};   /* 3 is reserved */
    nx_ref_msg_t *fm = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + 8u);
    assert(fm != NULL);
    nx_can_msg_t *fc = (nx_can_msg_t *)nx_ref_msg_data(fm);
    fc->id = 0x700u;
    fc->flags.raw = 0u;
    fc->timestamp = 0u;
    fc->flags.bits.dlc = nx_can_len_to_dlc(8u);
    memset(fc->data, 0u, 8u);
    memcpy(fc->data, bad_fs, sizeof(bad_fs));
    assert(nx_ref_msg_publish(fm, &can_rx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(fm);

    nx_can_isotp_process(&iso);
    DRAIN_LOCAL();
    EXPECT_SDU(NX_TP_SDU_CONFIRM, NX_TP_N_INVALID_FS);
    printf("  reserved flow status   -> CONFIRM / N_INVALID_FS\n");

    /* --- a reception whose peer goes quiet is reported too --- */
    nx_ref_msg_t *ffm = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + 8u);
    assert(ffm != NULL);
    nx_can_msg_t *ff = (nx_can_msg_t *)nx_ref_msg_data(ffm);
    ff->id = 0x700u;
    ff->flags.raw = 0u;
    ff->timestamp = 0u;
    ff->flags.bits.dlc = nx_can_len_to_dlc(8u);
    ff->data[0] = PEER_PCI_FF;
    ff->data[1] = 20u;                     /* announce 20, deliver 6 */
    memset(ff->data + 2, 0x44u, 6u);
    assert(nx_ref_msg_publish(ffm, &can_rx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(ffm);

    nx_can_isotp_process(&iso);            /* FF taken, flow control sent */
    DRAIN_LOCAL();
    g_clock_us += N_CR_US + 1u;            /* no consecutive frame ever arrives */
    nx_can_isotp_process(&iso);
    DRAIN_LOCAL();
    EXPECT_SDU(NX_TP_SDU_INDICATION, NX_TP_N_TIMEOUT_CR);
    printf("  peer stopped mid-message -> INDICATION / N_TIMEOUT_Cr\n");

    assert(nx_queue_is_empty(&sdu_tx_q));

    #undef DRAIN_LOCAL
    #undef EXPECT_SDU
}

/* ------------------------------------------------------------------ */
/* 9. The length ceiling                                              */
/* ------------------------------------------------------------------ */
/**
 * @brief  A length no allocation can express is refused in both directions.
 *
 * NX_CAN_ISOTP_MAX_MSG_LEN is the longest message this module handles: what a
 * length can say on the wire, less what one pooled block spends on its headers.
 * A send request above it comes back as NX_CAN_ISOTP_ERR_LENGTH, and a first
 * frame announcing more is answered with an overflow flow control frame before
 * anything is taken from the pool.
 *
 * This instance leaves rx_max_len at 0 - accept whatever the pool can hold - so
 * what refuses the first frame here is the module's own ceiling.
 */
static void demo_length_ceiling(void)
{
    printf("9. refuse a length no allocation can express\n");

    static nx_ref_msg_t  *sdu_tx_buf[4], *sdu_rx_buf[4];
    static nx_ref_msg_t  *can_rx_buf[8], *can_tx_buf[8];
    static nx_queue_t     sdu_tx_q, sdu_rx_q, can_rx_q, can_tx_q;
    static nx_can_isotp_t iso;

    assert(nx_ref_msg_queue_init(&sdu_tx_q, sdu_tx_buf, 4) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&sdu_rx_q, sdu_rx_buf, 4) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&can_rx_q, can_rx_buf, 8) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&can_tx_q, can_tx_buf, 8) == NX_QUEUE_OK);

    const nx_can_isotp_cfg_t cfg = {
        .max_frame_len = NX_CAN_ISOTP_FRAME_8,
        .phys_rx_id    = CEIL_RX_ID,
        .phys_tx_id    = CEIL_TX_ID,
        .pool          = &g_pool,
        .sdu_rx_queue  = &sdu_rx_q,
        .sdu_tx_queue  = &sdu_tx_q,
        .link          = 5u,
        .can_rx_queue  = &can_rx_q,
        .can_tx_queue  = &can_tx_q,
        .get_us        = mock_get_us,
        .n_bs_us       = N_BS_US,
        .n_cr_us       = N_CR_US,
        .rx_max_len    = 0u,            /* no ceiling of its own */
    };
    assert(nx_can_isotp_init(&iso, &cfg));

    /* --- a send request above the ceiling never reaches the pool --- */
    const uint8_t one = 0x00u;
    assert(nx_can_isotp_send(&iso, &one, NX_CAN_ISOTP_MAX_MSG_LEN + 1u,
                             NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_ERR_LENGTH);
    assert(nx_queue_is_empty(&sdu_rx_q));
    printf("  send(MAX_MSG_LEN + 1)    -> ERR_LENGTH\n");

    /* --- neither does a first frame announcing more than that --- */
    nx_ref_msg_t *ffm = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + 8u);
    assert(ffm != NULL);
    nx_can_msg_t *ff = (nx_can_msg_t *)nx_ref_msg_data(ffm);
    ff->id             = CEIL_RX_ID;
    ff->flags.raw      = 0u;
    ff->timestamp      = 0u;
    ff->flags.bits.dlc = nx_can_len_to_dlc(8u);
    /* Escape form: the 12-bit length reads zero, so the true length follows in
     * the next four bytes - here the largest one a wire length can express. */
    ff->data[0] = PEER_PCI_FF;
    ff->data[1] = 0x00u;
    ff->data[2] = 0xFFu;
    ff->data[3] = 0xFFu;
    ff->data[4] = 0xFFu;
    ff->data[5] = 0xFFu;
    ff->data[6] = 0x00u;
    ff->data[7] = 0x00u;
    assert(nx_ref_msg_publish(ffm, &can_rx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(ffm);

    nx_can_isotp_process(&iso);

    nx_ref_msg_t *fcm = NULL;
    assert(nx_queue_pop(&can_tx_q, &fcm) == NX_QUEUE_OK);
    const nx_can_msg_t *fc = (const nx_can_msg_t *)nx_ref_msg_data(fcm);
    assert(fc->id == CEIL_TX_ID);
    assert(fc->data[0] == (PEER_PCI_FC | PEER_FS_OVFLW));
    nx_ref_msg_release(fcm);
    assert(nx_queue_is_empty(&can_tx_q));
    printf("  FF announcing 0xFFFFFFFF -> FC.OVERFLOW, nothing reassembled\n");

    /* The reception never started, so there is nothing to report upwards. */
    assert(nx_queue_is_empty(&sdu_tx_q));
}

/* ------------------------------------------------------------------ */
/* 10. A transmit queue that fills up, then drains                    */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that a frame the link refuses is offered again, not lost.
 *
 * Both halves of a conversation have to put frames on the link: the transmit
 * side its own, the receive side the flow control that lets the peer continue.
 * Here the transmit queue is plugged with a frame nobody claims, so the next
 * emission has nowhere to go; unplugging it lets the held frame through and the
 * conversation finishes normally.
 */

/* Storage for the blocked-link instance, shared by scenarios 10 and 11. */
static nx_ref_msg_t  *g_lk_sdu_tx_buf[4], *g_lk_sdu_rx_buf[4];
static nx_ref_msg_t  *g_lk_can_rx_buf[8], *g_lk_can_tx_buf[1];
static nx_queue_t     g_lk_sdu_tx_q, g_lk_sdu_rx_q, g_lk_can_rx_q, g_lk_can_tx_q;
static nx_can_isotp_t g_lk_iso;

/** @brief Build the instance whose transmit queue holds exactly one frame. */
static void link_instance_init(void)
{
    assert(nx_ref_msg_queue_init(&g_lk_sdu_tx_q, g_lk_sdu_tx_buf, 4) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_lk_sdu_rx_q, g_lk_sdu_rx_buf, 4) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_lk_can_rx_q, g_lk_can_rx_buf, 8) == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_lk_can_tx_q, g_lk_can_tx_buf, 1) == NX_QUEUE_OK);

    const nx_can_isotp_cfg_t cfg = {
        .max_frame_len = NX_CAN_ISOTP_FRAME_8,
        .phys_rx_id    = LINK_RX_ID,
        .phys_tx_id    = LINK_TX_ID,
        .pool          = &g_pool,
        .sdu_rx_queue  = &g_lk_sdu_rx_q,
        .sdu_tx_queue  = &g_lk_sdu_tx_q,
        .link          = 7u,
        .confirm_tx    = true,          /* so a timeout is visible upstairs */
        .can_rx_queue  = &g_lk_can_rx_q,
        .can_tx_queue  = &g_lk_can_tx_q,
        .get_us        = mock_get_us,
        .n_as_us       = N_AS_US,
        .n_ar_us       = N_AR_US,
        .n_bs_us       = N_BS_US,
        .n_cr_us       = N_CR_US,
        .rx_max_len    = 64u,
        .rx_block_size = 2u,
    };
    assert(nx_can_isotp_init(&g_lk_iso, &cfg));
}

/** @brief Occupy the single transmit slot with a frame the example owns. */
static nx_ref_msg_t *plug_link(void)
{
    nx_ref_msg_t *m = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + 8u);
    assert(m != NULL);
    nx_can_msg_t *f = (nx_can_msg_t *)nx_ref_msg_data(m);
    f->id             = 0x001u;
    f->flags.raw      = 0u;
    f->timestamp      = 0u;
    f->flags.bits.dlc = nx_can_len_to_dlc(8u);
    memset(f->data, 0, 8u);
    assert(nx_ref_msg_publish(m, &g_lk_can_tx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(m);
    assert(nx_queue_is_full(&g_lk_can_tx_q));
    return m;
}

/** @brief Free the transmit slot again, as a driver draining the queue would. */
static void unplug_link(void)
{
    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_lk_can_tx_q, &m) == NX_QUEUE_OK);
    nx_ref_msg_release(m);
}

/** @brief Push one frame at the blocked-link instance. */
static void feed_link_frame(const uint8_t *payload, size_t len)
{
    nx_ref_msg_t *m = nx_ref_msg_alloc(&g_pool, sizeof(nx_can_msg_t) + len);
    assert(m != NULL);
    nx_can_msg_t *f = (nx_can_msg_t *)nx_ref_msg_data(m);
    f->id             = LINK_RX_ID;
    f->flags.raw      = 0u;
    f->timestamp      = 0u;
    f->flags.bits.dlc = nx_can_len_to_dlc((uint32_t)len);
    memcpy(f->data, payload, len);
    assert(nx_ref_msg_publish(m, &g_lk_can_rx_q) == NX_REF_MSG_OK);
    nx_ref_msg_release(m);
}

static void demo_link_backpressure(void)
{
    printf("10. a transmit queue that fills up, then drains\n");

    link_instance_init();

    /* --- transmit side: the single frame cannot go out yet --- */
    plug_link();
    const uint8_t msg[3] = {0x22u, 0xF1u, 0x90u};
    assert(nx_can_isotp_send(&g_lk_iso, msg, sizeof(msg), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);

    nx_can_isotp_process(&g_lk_iso);
    /* Nothing was reported: the transmission is held, not finished. */
    assert(nx_queue_is_empty(&g_lk_sdu_tx_q));
    printf("  queue full at send time  -> held, no confirmation yet\n");

    /* --- the driver drains, and the same frame goes out --- */
    unplug_link();
    g_clock_us += 5000u;                /* well inside N_As */
    nx_can_isotp_process(&g_lk_iso);

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_lk_can_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_msg_t *sf = (const nx_can_msg_t *)nx_ref_msg_data(m);
    assert(sf->id == LINK_TX_ID);
    assert(sf->data[0] == (PEER_PCI_SF | 0x03u));
    assert(sf->data[1] == 0x22u && sf->data[2] == 0xF1u && sf->data[3] == 0x90u);
    print_frame("SF ->", sf);
    nx_ref_msg_release(m);

    /* And now the transmission reports that it completed. */
    assert(nx_queue_pop(&g_lk_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *cf = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(cf->kind == NX_TP_SDU_CONFIRM);
    assert(cf->result == NX_TP_N_OK);
    nx_ref_msg_release(m);
    printf("  queue drained            -> same frame emitted, confirm N_OK\n");

    /* --- receive side: the flow control cannot go out yet --- */
    plug_link();
    const uint8_t ff[8] = {PEER_PCI_FF, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u};
    feed_link_frame(ff, 8u);
    nx_can_isotp_process(&g_lk_iso);
    /* The reassembly is alive and waiting for room, so nothing failed upwards. */
    assert(nx_queue_is_empty(&g_lk_sdu_tx_q));
    printf("  FF arrives, queue full   -> reassembly held, FC owed\n");

    unplug_link();
    g_clock_us += 5000u;
    nx_can_isotp_process(&g_lk_iso);

    assert(nx_queue_pop(&g_lk_can_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_msg_t *fc = (const nx_can_msg_t *)nx_ref_msg_data(m);
    assert(fc->id == LINK_TX_ID);
    assert(fc->data[0] == (PEER_PCI_FC | PEER_FS_CTS));
    print_frame("FC ->", fc);
    nx_ref_msg_release(m);
    printf("  queue drained            -> the owed FC goes out\n");

    /* Finish the message so the instance is idle for the next scenario. */
    const uint8_t cf1[8] = {PEER_PCI_CF | 1u, 7u, 8u, 9u, 10u, 0u, 0u, 0u};
    feed_link_frame(cf1, 8u);
    nx_can_isotp_process(&g_lk_iso);

    assert(nx_queue_pop(&g_lk_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *in = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(in->kind == NX_TP_SDU_INDICATION);
    assert(in->result == NX_TP_N_OK);
    assert(in->len == 10u);
    nx_ref_msg_release(m);
    printf("  message completes        -> 10 bytes handed up\n");

    /* Whatever the module put on the link is the example's to release. */
    while (nx_queue_pop(&g_lk_can_tx_q, &m) == NX_QUEUE_OK) {
        nx_ref_msg_release(m);
    }
}

/* ------------------------------------------------------------------ */
/* 11. A transmit queue that never drains                             */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show that a link which stays blocked ends the conversation.
 *
 * A frame is offered again for as long as N_As (transmitting) or N_Ar
 * (receiving) allows. Past that the conversation ends and says why, so a driver
 * that has stopped consuming is reported as a local failure rather than left to
 * look like a silent peer.
 */
static void demo_link_timeout(void)
{
    printf("11. a transmit queue that never drains\n");

    /* --- transmit side: N_As expires --- */
    plug_link();
    const uint8_t msg[3] = {0x22u, 0xF1u, 0x90u};
    assert(nx_can_isotp_send(&g_lk_iso, msg, sizeof(msg), NX_TP_TA_PHYSICAL) == NX_CAN_ISOTP_OK);

    nx_can_isotp_process(&g_lk_iso);          /* refused: the frame is held */
    assert(nx_queue_is_empty(&g_lk_sdu_tx_q));

    g_clock_us += N_AS_US + 1u;               /* the window closes */
    nx_can_isotp_process(&g_lk_iso);

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_lk_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *cf = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(cf->kind == NX_TP_SDU_CONFIRM);
    assert(cf->result == NX_TP_N_TIMEOUT_A);
    nx_ref_msg_release(m);
    printf("  send, link never free    -> confirm N_TIMEOUT_A\n");

    /* --- receive side: N_Ar expires --- */
    const uint8_t ff[8] = {PEER_PCI_FF, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u};
    feed_link_frame(ff, 8u);
    nx_can_isotp_process(&g_lk_iso);          /* the FC cannot go out */
    assert(nx_queue_is_empty(&g_lk_sdu_tx_q));

    g_clock_us += N_AR_US + 1u;
    nx_can_isotp_process(&g_lk_iso);

    assert(nx_queue_pop(&g_lk_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *in = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(in->kind == NX_TP_SDU_INDICATION);
    /* The peer went on waiting for permission that never left this side, so the
     * fault is named as a local one and not as a quiet peer. */
    assert(in->result == NX_TP_N_TIMEOUT_A);
    nx_ref_msg_release(m);
    printf("  receive, link never free -> indication N_TIMEOUT_A\n");

    unplug_link();
    while (nx_queue_pop(&g_lk_can_tx_q, &m) == NX_QUEUE_OK) {
        nx_ref_msg_release(m);
    }
}

/* ------------------------------------------------------------------ */
/* 12. Send a functionally addressed single frame                      */
/* ------------------------------------------------------------------ */
/**
 * @brief  Show the tester side broadcasting one request to every ECU at once.
 *
 * Functional addressing is 1:N, so it is single-frame only: the SF goes out on
 * the functional request ID, and the module reports completion without any flow
 * control having been involved. A request too long for one frame is refused
 * before anything is queued.
 */
static void demo_send_functional(void)
{
    printf("12. send a functionally addressed single frame\n");

    /* The functional send needs the request ID configured as an outgoing one,
     * which this instance cannot also listen on: a tester that broadcasts and an
     * ECU that hears broadcasts are different roles on different instances. So a
     * small tester instance is set up here, listening on a spare physical ID. */
    nx_can_isotp_t tester;
    const nx_can_isotp_cfg_t tcfg = {
        .max_frame_len = NX_CAN_ISOTP_FRAME_8,
        .pad_frames    = true,
        .pad_byte      = PAD_BYTE,
        .phys_rx_id    = CEIL_RX_ID,   /* an ECU would answer on its own ID */
        .phys_tx_id    = CEIL_TX_ID,
        .func_tx_id    = FUNC_TX_ID,   /* the one functional sender on the bus */
        .pool          = &g_pool,
        .sdu_rx_queue  = &g_sdu_rx_q,
        .sdu_tx_queue  = &g_sdu_tx_q,
        .link          = LINK_ID,
        .confirm_tx    = true,   /* the scenario asserts the confirmation */
        .can_rx_queue  = &g_can_rx_q,
        .can_tx_queue  = &g_can_tx_q,
        .get_us        = mock_get_us,
    };
    assert(nx_can_isotp_init(&tester, &tcfg));

    const uint8_t req[] = {0x10u, 0x03u};      /* ECU reset, broadcast */
    assert(nx_can_isotp_send(&tester, req, sizeof(req), NX_TP_TA_FUNCTIONAL)
           == NX_CAN_ISOTP_OK);

    /* A functional message must fit one frame: no flow control exists to pace a
     * longer one, so this is refused outright. */
    const uint8_t big[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    assert(nx_can_isotp_send(&tester, big, sizeof(big), NX_TP_TA_FUNCTIONAL)
           == NX_CAN_ISOTP_ERR_PARAM);

    nx_can_isotp_process(&tester);

    nx_ref_msg_t *m = NULL;
    assert(nx_queue_pop(&g_can_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_msg_t *f = (const nx_can_msg_t *)nx_ref_msg_data(m);
    assert(f->id == FUNC_TX_ID);
    assert(f->data[0] == (PEER_PCI_SF | 0x02u));
    assert(f->data[1] == 0x10u && f->data[2] == 0x03u);
    print_frame("SF ->", f);
    nx_ref_msg_release(m);

    /* The single frame completed the transmission, so a confirmation follows. */
    assert(nx_queue_pop(&g_sdu_tx_q, &m) == NX_QUEUE_OK);
    const nx_can_isotp_sdu_t *cf = (const nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    assert(cf->kind == NX_TP_SDU_CONFIRM);
    assert(cf->result == NX_TP_N_OK);
    nx_ref_msg_release(m);
    assert(nx_queue_is_empty(&g_sdu_tx_q));
    printf("  broadcast request        -> one SF on 0x7DF, confirm N_OK\n");

    /* The tester's empty can_rx queue must be drained, or the idempotent init
     * check that follows would not see an empty queue. */
    while (nx_queue_pop(&g_can_rx_q, &m) == NX_QUEUE_OK) {
        nx_ref_msg_release(m);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
int nx_can_isotp_example_run(void)
{
    printf("=== nx_can_isotp example ===\n");

    /* One tier for CAN frames, one for the SDUs this example moves. */
    const nx_tiered_level_cfg_t tiers[] = {
        {FRAME_BLOCK, 24},
        {SDU_BLOCK,    8},
    };
    const nx_tiered_mem_pool_cfg_t pool_cfg = {
        .memory      = g_pool_mem,
        .memory_size = sizeof(g_pool_mem),
        .tiers       = tiers,
        .tier_count  = sizeof(tiers) / sizeof(tiers[0]),
    };
    assert(nx_tiered_mem_pool_init(&g_pool, &pool_cfg, NULL) == NX_TIERED_OK);

    assert(nx_ref_msg_queue_init(&g_sdu_tx_q, g_sdu_tx_q_buf,
                                 sizeof(g_sdu_tx_q_buf) / sizeof(g_sdu_tx_q_buf[0]))
           == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_sdu_rx_q, g_sdu_rx_q_buf,
                                 sizeof(g_sdu_rx_q_buf) / sizeof(g_sdu_rx_q_buf[0]))
           == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_can_rx_q, g_can_rx_q_buf,
                                 sizeof(g_can_rx_q_buf) / sizeof(g_can_rx_q_buf[0]))
           == NX_QUEUE_OK);
    assert(nx_ref_msg_queue_init(&g_can_tx_q, g_can_tx_q_buf,
                                 sizeof(g_can_tx_q_buf) / sizeof(g_can_tx_q_buf[0]))
           == NX_QUEUE_OK);

    const nx_can_isotp_cfg_t cfg = {
        .max_frame_len = NX_CAN_ISOTP_FRAME_8,
        .pad_frames    = true,
        .pad_byte      = PAD_BYTE,
        .phys_rx_id    = PHYS_RX_ID,
        .phys_tx_id    = PHYS_TX_ID,
        .func_rx_id    = FUNC_RX_ID,    /* listen for functional requests */
        .pool          = &g_pool,
        .sdu_rx_queue  = &g_sdu_rx_q,   /* upper -> module: send requests   */
        .sdu_tx_queue  = &g_sdu_tx_q,   /* module -> upper: what it received */
        .link          = LINK_ID,       /* stamped into every published SDU  */
        .can_rx_queue  = &g_can_rx_q,
        .can_tx_queue  = &g_can_tx_q,
        .get_us        = mock_get_us,
        .n_bs_us       = N_BS_US,
        .n_cr_us       = N_CR_US,
        .n_wft_max     = N_WFT_MAX,
        .rx_max_len    = RX_MAX_LEN,
        .rx_block_size = RX_BLOCK_SIZE,
        .rx_stmin      = RX_STMIN,
        .tx_frames_per_process = 1u,
    };
    assert(nx_can_isotp_init(&g_iso, &cfg));

    demo_receive_single_frame();
    demo_receive_segmented();
    demo_receive_too_long();
    demo_receive_functional();
    demo_send_single_frame();
    demo_send_segmented();
    demo_send_wait_limit();
    demo_confirmations();
    demo_length_ceiling();
    demo_link_backpressure();
    demo_link_timeout();
    demo_send_functional();

    /* The four IDs must be mutually distinct while non-zero: here func_tx_id
     * collides with phys_rx_id, which init must refuse. */
    nx_can_isotp_cfg_t bad = cfg;
    bad.func_tx_id = PHYS_RX_ID;
    assert(nx_can_isotp_init(&g_iso, &bad) == false);

    /* Every message and frame the example touched has been released, so the
     * pool must be back to fully free. */
    for (size_t i = 0; i < nx_tiered_mem_pool_tier_count(&g_pool); i++) {
        nx_tiered_level_stat_t st;
        assert(nx_tiered_mem_pool_get_tier_stat(&g_pool, i, &st) == NX_TIERED_OK);
        assert(st.free_count == st.block_count);
    }
    printf("all pool blocks returned; nx_can_isotp example passed\n\n");
    return 0;
}
