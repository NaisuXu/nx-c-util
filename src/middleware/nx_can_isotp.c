/**
 * @file    nx_can_isotp.c
 * @brief   ISO 15765-2 (DoCAN / ISO-TP) transport-layer, queue-to-queue, pure C.
 *
 * See nx_can_isotp.h for the design overview. This translation unit is a core
 * implementation with no dynamic memory and no hardware access: it moves
 * nx_ref_msg objects between the caller's queues and tracks pacing/guard timers
 * against the @c get_us clock.
 */
#include "nx_can_isotp.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Defaults and helpers                                               */
/* ------------------------------------------------------------------ */

/* Protocol control information: the frame type occupies the high nibble of the
 * first byte of every ISO 15765-2 frame. */
/** @brief First protocol control byte value of a single frame (SF). */
#define NX_CAN_ISOTP_PCI_SF 0x00u
/** @brief First protocol control byte value of a first frame (FF). */
#define NX_CAN_ISOTP_PCI_FF 0x10u
/** @brief First protocol control byte value of a consecutive frame (CF). */
#define NX_CAN_ISOTP_PCI_CF 0x20u
/** @brief First protocol control byte value of a flow control frame (FC). */
#define NX_CAN_ISOTP_PCI_FC 0x30u

/** @brief Largest payload expressible with a 12-bit FF_DL. Above this the
 *         32-bit length-extension escape carries the true length. */
#define NX_CAN_ISOTP_FFDL_12   4095u

/**
 * @brief  Largest single-frame payload expressible in the header's low nibble.
 *
 * A longer payload still fits one frame on 64-byte geometry, where the nibble is
 * zero and a second header byte carries the length.
 */
#define NX_CAN_ISOTP_SF_DL_4 7u

/* Flow status: the low nibble of a flow control frame's first byte. */
/** @brief Flow control flag value: continue sending. */
#define NX_CAN_ISOTP_FS_CTS   0x00u
/** @brief Flow control flag value: wait (sender holds). */
#define NX_CAN_ISOTP_FS_WAIT  0x01u
/** @brief Flow control flag value: receiver overflow, abort. */
#define NX_CAN_ISOTP_FS_OVFLW 0x02u

/* Values init() substitutes for a cfg field left at zero. */
/** @brief Default N_As / N_Ar / N_Bs / N_Cr timeout in microseconds (1000 ms). */
#define NX_CAN_ISOTP_DEFAULT_TIMEOUT_US 1000000u

/** @brief Default number of consecutive FC.WAIT frames tolerated. */
#define NX_CAN_ISOTP_DEFAULT_WFT_MAX 4u

/* Reception states. FC_WAIT holds a reassembly whose flow control the transmit
 * queue would not take, so the frame is offered again until it goes out or N_Ar
 * expires. */
enum { NX_ISOTP_RX_IDLE = 0, NX_ISOTP_RX_ASSEM, NX_ISOTP_RX_FC_WAIT };

/* Transmit states. WAIT_LINK holds a frame the transmit queue would not take,
 * for the same reason and under N_As. */
enum {
    NX_ISOTP_TX_IDLE = 0,
    NX_ISOTP_TX_WAIT_FC,
    NX_ISOTP_TX_SEND,
    NX_ISOTP_TX_WAIT_LINK
};

/** @brief Current time from the injected clock. */
static uint32_t istp_now(nx_can_isotp_t *iso)
{
    return iso->cfg.get_us();
}

/**
 * @brief  Decode a wire STmin byte into microseconds.
 *
 * 0x01..0x7F are milliseconds; 0xF1..0xF9 are 100..900 us. Everything else is
 * reserved and treated as "no separation required", as the standard directs.
 */
static int32_t istp_stmin_to_us(uint8_t st)
{
    if (st >= 0x01u && st <= 0x7Fu) {
        return (int32_t)st * 1000;
    }
    if (st >= 0xF1u && st <= 0xF9u) {
        return (int32_t)(st - 0xF0u) * 100;
    }
    return 0;
}

/* ======================================================================== */
/* Frame emission                                                           */
/* ======================================================================== */

/**
 * @brief  Build and enqueue one CAN frame whose payload is the @p plen bytes of
 *         @p src after a @p pci_hdr-byte protocol header in @p pci.
 *
 * Clamps the payload to what the frame geometry allows, so the final, short,
 * frame of a message carries exactly the remaining bytes.
 *
 * @return true if the frame was accepted by the driver queue.
 */
static bool istp_emit_frame(nx_can_isotp_t *iso, uint32_t id,
                            const uint8_t *src, size_t plen,
                            const uint8_t *pci, size_t pci_hdr)
{
    size_t cap   = (size_t)iso->cfg.max_frame_len - pci_hdr;
    size_t chunk = (plen < cap) ? plen : cap;

    /* A data length code only expresses certain sizes, so the frame carries at
     * least what is written and possibly a filled tail. With pad_frames, a short
     * frame is raised to 8 bytes as well. Size the block to what the code
     * expresses, not to the payload, so a consumer reading
     * nx_can_dlc_to_len() bytes stays inside the allocation. */
    size_t used = pci_hdr + chunk;
    size_t want = used;
    if (iso->cfg.pad_frames && want < NX_CAN_MAX_CLASSIC_LEN) {
        want = NX_CAN_MAX_CLASSIC_LEN;
    }
    uint8_t dlc  = nx_can_len_to_dlc((uint32_t)want);
    size_t  wire = (size_t)nx_can_dlc_to_len(dlc);

    nx_ref_msg_t *f = nx_ref_msg_alloc(iso->cfg.pool,
                                       sizeof(nx_can_msg_t) + wire);
    if (f == NULL) {
        return false;
    }
    nx_can_msg_t *cm = (nx_can_msg_t *)nx_ref_msg_data(f);
    cm->id            = id;
    cm->flags.raw     = 0u;
    cm->timestamp     = 0u;
    cm->flags.bits.ch     = iso->cfg.ch;
    cm->flags.bits.is_ext = iso->cfg.ext_id;
    cm->flags.bits.is_fd  = iso->cfg.fd_frames;
    cm->flags.bits.brs    = iso->cfg.brs;
    uint8_t *d = cm->data;
    memcpy(d, pci, pci_hdr);
    if (chunk > 0u) {
        memcpy(d + pci_hdr, src, chunk);
    }
    if (wire > used) {
        memset(d + used, iso->cfg.pad_byte, wire - used);
    }
    cm->flags.bits.dlc = dlc;
    nx_ref_msg_shrink(f, sizeof(nx_can_msg_t) + wire);

    nx_ref_msg_ret_t r = nx_ref_msg_publish(f, iso->cfg.can_tx_queue);
    if (r != NX_REF_MSG_OK) {
        nx_ref_msg_release(f);      /* queue full: frame dropped, pool balanced */
        return false;
    }
    nx_ref_msg_release(f);          /* drop producer reference; queue owns it */
    return true;
}

/**
 * @brief  Emit a single frame (SF).
 *
 * Up to 7 bytes the length rides in the low nibble of the one-byte header. Above
 * that (reachable only on 64-byte geometry) the nibble is zero and a second
 * header byte carries the true length.
 */
static bool istp_emit_sf(nx_can_isotp_t *iso, uint32_t id, const uint8_t *src,
                         size_t plen)
{
    if (plen <= NX_CAN_ISOTP_SF_DL_4) {
        uint8_t pci = (uint8_t)(NX_CAN_ISOTP_PCI_SF | (uint8_t)plen);
        return istp_emit_frame(iso, id, src, plen, &pci, 1u);
    }
    uint8_t pci[2];
    pci[0] = NX_CAN_ISOTP_PCI_SF;      /* nibble 0: length escapes to byte 1 */
    pci[1] = (uint8_t)plen;
    return istp_emit_frame(iso, id, src, plen, pci, 2u);
}

/** @brief Largest payload a single frame carries in this instance's geometry. */
static size_t istp_sf_capacity(const nx_can_isotp_t *iso)
{
    return (iso->cfg.max_frame_len <= NX_CAN_ISOTP_FRAME_8)
               ? (size_t)NX_CAN_ISOTP_SF_DL_4
               : (size_t)iso->cfg.max_frame_len - 2u;
}

static bool istp_emit_cf(nx_can_isotp_t *iso, uint32_t id, const uint8_t *src,
                         size_t plen, unsigned sn)
{
    uint8_t pci = (uint8_t)(NX_CAN_ISOTP_PCI_CF | (uint8_t)(sn & 0x0Fu));
    return istp_emit_frame(iso, id, src, plen, &pci, 1u);
}

/**
 * @brief  Emit the first frame (FF) of a message, using a 12-bit or (above the
 *         12-bit bound) 32-bit length field. Puts the first payload chunk into
 *         the frame and reports how many bytes it has carried.
 */
static size_t istp_emit_ff(nx_can_isotp_t *iso, uint32_t id, const uint8_t *src,
                           uint32_t total)
{
    uint8_t pci[6];
    size_t  hdr;
    if (total > NX_CAN_ISOTP_FFDL_12) {
        /* Escape form: the 12-bit field reads zero and the true length follows
         * as a 32-bit big-endian value. */
        pci[0] = NX_CAN_ISOTP_PCI_FF;
        pci[1] = 0x00u;
        pci[2] = (uint8_t)(total >> 24);
        pci[3] = (uint8_t)(total >> 16);
        pci[4] = (uint8_t)(total >> 8);
        pci[5] = (uint8_t)(total);
        hdr    = 6u;
    } else {
        pci[0] = (uint8_t)(NX_CAN_ISOTP_PCI_FF | (uint8_t)(total >> 8));
        pci[1] = (uint8_t)(total);
        hdr    = 2u;
    }
    if (!istp_emit_frame(iso, id, src, total, pci, hdr)) {
        return 0u;
    }
    size_t cap = (size_t)iso->cfg.max_frame_len - hdr;
    return ((size_t)total < cap) ? (size_t)total : cap;  /* bytes loaded into the FF */
}

/**
 * @brief  Emit a flow control frame on the physical transmit ID.
 *
 * Flow control belongs to the receiving half of a conversation, and it must go
 * out on this instance's own transmit ID - never on the ID the request arrived
 * on, which belongs to the peer.
 *
 * @return true if the frame was accepted by the driver queue.
 */
static bool istp_emit_fc(nx_can_isotp_t *iso, uint8_t fs, uint8_t bs,
                         uint8_t stmin)
{
    uint8_t pci[3];
    pci[0] = (uint8_t)(NX_CAN_ISOTP_PCI_FC | (uint8_t)(fs & 0x0Fu));
    pci[1] = bs;
    pci[2] = stmin;
    /* A full flow control frame is header only, so it goes out through the same
     * builder with no payload. */
    return istp_emit_frame(iso, iso->cfg.phys_tx_id, NULL, 0u, pci, 3u);
}

/**
 * @brief  Emit the flow control that continues a reception under the cfg, and
 *         place the reception in the state that outcome calls for.
 *
 * A frame the transmit queue refused leaves the reception in FC_WAIT under N_Ar,
 * where process() offers it again: the peer is waiting on this permission, so
 * losing it would stall the conversation until the peer's own timer gave up.
 * Every parameter of the frame comes from the cfg, so a retry rebuilds exactly
 * the same frame.
 */
static void istp_emit_cts(nx_can_isotp_t *iso)
{
    nx_can_isotp_rx_t *rx = &iso->run.rx;
    if (!istp_emit_fc(iso, NX_CAN_ISOTP_FS_CTS,
                      iso->cfg.rx_block_size, iso->cfg.rx_stmin)) {
        if (rx->state != NX_ISOTP_RX_FC_WAIT) {
            rx->state    = NX_ISOTP_RX_FC_WAIT;
            rx->deadline = istp_now(iso) + iso->cfg.n_ar_us;
        }
        return;                        /* an outstanding N_Ar keeps running */
    }
    rx->block_left = iso->cfg.rx_block_size;   /* 0 = no block limit */
    rx->state      = NX_ISOTP_RX_ASSEM;
    rx->deadline   = istp_now(iso) + iso->cfg.n_cr_us;
}

/**
 * @brief  Fill the transport-neutral header of an SDU this instance publishes.
 */
static void istp_sdu_head(const nx_can_isotp_t *iso, nx_tp_sdu_t *s,
                          uint32_t len, uint8_t kind, uint8_t ta_type,
                          uint8_t result)
{
    s->len     = len;
    s->link    = iso->cfg.link;
    s->kind    = kind;
    s->ta_type = ta_type;
    s->result  = result;
}

/**
 * @brief  Report the outcome of a transmission to the upper layer.
 *
 * A confirmation carries no payload: it names which transmission ended and how.
 * Silent when the instance is not configured to confirm, and best-effort - a
 * report that cannot be allocated or queued is dropped rather than retried,
 * since the transmission itself is already over.
 */
static void istp_tx_confirm(nx_can_isotp_t *iso, uint8_t result)
{
    if (!iso->cfg.confirm_tx) {
        return;
    }
    nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool, sizeof(nx_tp_sdu_t));
    if (m == NULL) {
        return;
    }
    istp_sdu_head(iso, (nx_tp_sdu_t *)nx_ref_msg_data(m), 0u,
                  NX_TP_SDU_CONFIRM, NX_TP_TA_PHYSICAL, result);
    (void)nx_ref_msg_publish(m, iso->cfg.sdu_tx_queue);
    nx_ref_msg_release(m);
}

/* ======================================================================== */
/* Receive side: reassembly                                                 */
/* ======================================================================== */

/**
 * @brief  Report a reception that ended without delivering its message.
 *
 * The partially reassembled block is released, so nothing of a failed reception
 * reaches the upper layer except the reason it failed.
 */
static void istp_rx_abort(nx_can_isotp_t *iso, uint8_t result);

static void istp_rx_reset(nx_can_isotp_t *iso)
{
    nx_can_isotp_rx_t *rx = &iso->run.rx;
    if (rx->acc != NULL) {
        nx_ref_msg_release(rx->acc);   /* abort; freed at refcount 0 */
        rx->acc = NULL;
    }
    rx->state      = NX_ISOTP_RX_IDLE;
    rx->sn         = 0u;
    rx->total      = 0u;
    rx->filled     = 0u;
    rx->block_left = 0u;
}

/**
 * @brief  End a reception in progress and report why it did not complete.
 *
 * Reports only when a reception was actually under way, so an event that finds
 * the receiver idle - a fresh single frame, a first frame starting a new
 * conversation - passes through without inventing a failure.
 */
static void istp_rx_abort(nx_can_isotp_t *iso, uint8_t result)
{
    bool was_active = (iso->run.rx.state != NX_ISOTP_RX_IDLE);

    istp_rx_reset(iso);

    if (was_active && iso->cfg.confirm_tx) {
        nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool, sizeof(nx_tp_sdu_t));
        if (m == NULL) {
            return;
        }
        istp_sdu_head(iso, (nx_tp_sdu_t *)nx_ref_msg_data(m), 0u,
                      NX_TP_SDU_INDICATION, NX_TP_TA_PHYSICAL, result);
        (void)nx_ref_msg_publish(m, iso->cfg.sdu_tx_queue);
        nx_ref_msg_release(m);
    }
}

static void istp_rx_finish(nx_can_isotp_t *iso)
{
    nx_can_isotp_rx_t *rx = &iso->run.rx;

    /* Detach the message before resetting: the reset path releases whatever acc
     * still points at, which would drop the reference the queue is holding and
     * hand the block back while a consumer can still reach it. Publishing takes
     * the queue's reference, so releasing the producer's one here is what leaves
     * the message owned by the queue alone. A message no queue accepted reaches
     * refcount 0 and is returned, as every other emission path does. */
    nx_ref_msg_t *m = rx->acc;
    rx->acc = NULL;
    istp_rx_reset(iso);

    (void)nx_ref_msg_publish(m, iso->cfg.sdu_tx_queue);   /* full queue: dropped */
    nx_ref_msg_release(m);
}

/**
 * @brief  Deliver a complete single-frame message to the upper layer.
 *
 * @param  hdr Header bytes ahead of the payload (1, or 2 for the escape form).
 */
static void istp_rx_single(nx_can_isotp_t *iso, uint8_t ta_type,
                           const uint8_t *d, size_t plen, size_t hdr)
{
    nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool,
                                       sizeof(nx_can_isotp_sdu_t) + plen);
    if (m == NULL) {
        return;
    }
    nx_can_isotp_sdu_t *out = (nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    istp_sdu_head(iso, out, (uint32_t)plen, NX_TP_SDU_INDICATION, ta_type,
                  NX_TP_N_OK);
    memcpy(out->data, d + hdr, plen);
    if (nx_ref_msg_publish(m, iso->cfg.sdu_tx_queue) != NX_REF_MSG_OK) {
        /* upper layer too slow: drop the message */
    }
    nx_ref_msg_release(m);
}

/**
 * @brief  Decode a single frame's announced length, or return false if the frame
 *         does not carry a valid one.
 *
 * The two header forms are each valid over one range of frame lengths, and an
 * announced length must fit in what the frame actually carries. A frame outside
 * those bounds is refused rather than trimmed: a trimmed message would reach the
 * upper layer looking complete while missing its tail.
 */
static bool istp_sf_len(const uint8_t *d, size_t dlen, size_t *out_plen,
                        size_t *out_hdr)
{
    size_t plen = d[0] & 0x0Fu;
    size_t hdr  = 1u;
    if (plen == 0u) {
        /* Escape form: the length lives in the second header byte, and it only
         * applies to a frame longer than the low nibble could ever describe. */
        if (dlen <= NX_CAN_MAX_CLASSIC_LEN || dlen < 2u) {
            return false;
        }
        plen = d[1];
        hdr  = 2u;
        if (plen <= NX_CAN_ISOTP_SF_DL_4) {
            return false;      /* short enough for the nibble form; not this one */
        }
    } else if (dlen > NX_CAN_MAX_CLASSIC_LEN) {
        return false;          /* nibble form does not describe a long frame */
    }
    if (plen == 0u || plen > dlen - hdr) {
        return false;          /* nothing announced, or more than is carried */
    }
    *out_plen = plen;
    *out_hdr  = hdr;
    return true;
}

/**
 * @brief  Advance the physical reception with one frame addressed to it.
 */
static void istp_rx_phys_frame(nx_can_isotp_t *iso, const nx_can_msg_t *frame)
{
    nx_can_isotp_rx_t *rx = &iso->run.rx;
    const uint8_t *d    = frame->data;
    size_t         dlen = nx_can_dlc_to_len(frame->flags.bits.dlc);
    uint8_t        pci_kind = d[0] & 0xF0u;

    if (pci_kind == NX_CAN_ISOTP_PCI_SF) {
        /* A new single frame supersedes any in-progress conversation, which is
         * then reported as interrupted rather than dropped silently. */
        istp_rx_abort(iso, NX_TP_N_UNEXP_PDU);
        size_t plen, hdr;
        if (istp_sf_len(d, dlen, &plen, &hdr)) {
            istp_rx_single(iso, NX_TP_TA_PHYSICAL, d, plen, hdr);
        }
        return;
    }

    if (pci_kind == NX_CAN_ISOTP_PCI_FF) {
        istp_rx_abort(iso, NX_TP_N_UNEXP_PDU);
        if (dlen < 2u) {
            return;
        }
        uint32_t total = ((uint32_t)(d[0] & 0x0Fu) << 8) | d[1];
        size_t   hdr   = 2u;
        if (total == 0u) {
            /* Escape form: the true length lives in bytes 2..5. */
            if (dlen < 6u) {
                return;
            }
            total = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                    ((uint32_t)d[4] << 8) | d[5];
            hdr = 6u;
        }
        if (total == 0u) {
            return;                       /* malformed: no length announced */
        }
        if (total > NX_CAN_ISOTP_MAX_MSG_LEN ||
            (iso->cfg.rx_max_len != 0u && total > iso->cfg.rx_max_len)) {
            /* More than one allocation can express, or more than this instance
             * accepts: refuse it outright, so the peer learns now instead of
             * waiting out its own timeout. Bounding total here is what keeps the
             * addition below from wrapping on a target whose size_t is no wider
             * than the announced length. */
            (void)istp_emit_fc(iso, NX_CAN_ISOTP_FS_OVFLW, 0u, 0u);
            istp_rx_abort(iso, NX_TP_N_BUFFER_OVFLW);
            return;
        }
        nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool,
                                           sizeof(nx_can_isotp_sdu_t) + total);
        if (m == NULL) {
            /* No room to reassemble: tell the peer rather than time out. */
            (void)istp_emit_fc(iso, NX_CAN_ISOTP_FS_OVFLW, 0u, 0u);
            istp_rx_abort(iso, NX_TP_N_BUFFER_OVFLW);
            return;
        }
        rx->acc      = m;
        rx->state    = NX_ISOTP_RX_ASSEM;
        rx->sn       = 1u;                /* the FF is SN 0; the first CF is SN 1 */
        rx->total    = total;
        rx->deadline = istp_now(iso) + iso->cfg.n_cr_us;

        nx_can_isotp_sdu_t *out = (nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
        istp_sdu_head(iso, out, total, NX_TP_SDU_INDICATION,
                      NX_TP_TA_PHYSICAL, NX_TP_N_OK);
        size_t have = (dlen > hdr) ? (dlen - hdr) : 0u;
        if (have > rx->total) {
            have = rx->total;
        }
        if (have > 0u) {
            memcpy(out->data, d + hdr, have);
        }
        rx->filled = (uint32_t)have;
        if (rx->filled >= rx->total) {
            istp_rx_finish(iso);          /* everything arrived in the FF */
            return;
        }
        istp_emit_cts(iso);
        return;
    }

    if (pci_kind == NX_CAN_ISOTP_PCI_CF) {
        if (rx->state != NX_ISOTP_RX_ASSEM) {
            return;                       /* spurious CF: ignored */
        }
        if ((d[0] & 0x0Fu) != rx->sn) {
            istp_rx_abort(iso, NX_TP_N_WRONG_SN);   /* sequence lost */
            return;
        }
        size_t have = dlen - 1u;
        size_t room = (size_t)rx->total - rx->filled;
        if (have > room) {
            have = room;
        }
        nx_can_isotp_sdu_t *out = (nx_can_isotp_sdu_t *)nx_ref_msg_data(rx->acc);
        if (have > 0u) {
            memcpy(out->data + rx->filled, d + 1u, have);
        }
        rx->filled  += (uint32_t)have;
        rx->sn       = (uint8_t)((rx->sn + 1u) & 0x0Fu);
        rx->deadline = istp_now(iso) + iso->cfg.n_cr_us;
        if (rx->filled >= rx->total) {
            istp_rx_finish(iso);
            return;
        }
        /* A block that has run out is refreshed with new flow control. */
        if (rx->block_left > 0u && --rx->block_left == 0u) {
            istp_emit_cts(iso);
        }
        return;
    }

    /* pci_kind == FC: flow control belongs to the transmit path. */
}

/* ======================================================================== */
/* Transmit side                                                            */
/* ======================================================================== */

/**
 * @brief  End the in-flight transmission and report its outcome.
 *
 * Reports only when a transmission was actually in flight, so an idle instance
 * never emits a confirmation.
 */
static void istp_tx_abort(nx_can_isotp_t *iso, uint8_t result)
{
    nx_can_isotp_tx_t *tx = &iso->run.tx;
    bool was_active = (tx->sdu != NULL);

    if (tx->sdu != NULL) {
        nx_ref_msg_release(tx->sdu);
        tx->sdu = NULL;
    }
    tx->state = NX_ISOTP_TX_IDLE;
    tx->wft   = 0u;

    if (was_active) {
        istp_tx_confirm(iso, result);
    }
}

/**
 * @brief  Apply a peer's flow-control frame to the in-flight send.
 */
static void istp_tx_apply_fc(nx_can_isotp_t *iso, const nx_can_msg_t *frame)
{
    nx_can_isotp_tx_t *tx = &iso->run.tx;
    const uint8_t *d    = frame->data;
    size_t         dlen = nx_can_dlc_to_len(frame->flags.bits.dlc);
    if (tx->state != NX_ISOTP_TX_WAIT_FC || dlen < 3u) {
        return;                    /* no send awaiting FC, or malformed frame */
    }
    uint8_t fs = d[0] & 0x0Fu;
    if (fs == NX_CAN_ISOTP_FS_OVFLW) {
        istp_tx_abort(iso, NX_TP_N_BUFFER_OVFLW);
        return;
    }
    if (fs == NX_CAN_ISOTP_FS_WAIT) {
        /* Hold, and give the peer another timeout window - but only so many
         * times, or a peer stuck on WAIT would stall the send forever. */
        if (++tx->wft > iso->cfg.n_wft_max) {
            istp_tx_abort(iso, NX_TP_N_WFT_OVRN);
            return;
        }
        tx->deadline = istp_now(iso) + iso->cfg.n_bs_us;
        return;
    }
    if (fs != NX_CAN_ISOTP_FS_CTS) {
        /* A flow status outside the defined set leaves the transmission with no
         * instruction to follow, so it ends rather than waiting out N_Bs. */
        istp_tx_abort(iso, NX_TP_N_INVALID_FS);
        return;
    }
    tx->wft      = 0u;
    tx->bs       = d[1];
    tx->stmin_us = istp_stmin_to_us(d[2]);
    tx->state    = NX_ISOTP_TX_SEND;
}

/**
 * @brief  Hold the frame the link would not take, under N_As.
 *
 * The emission path releases a frame the queue refused, so nothing is retained
 * but the intent to try again: none of @c sent, @c sn or @c last_us moved, so the
 * next attempt rebuilds exactly the same frame. An outstanding deadline is left
 * running - the window belongs to the frame, not to the attempt.
 */
static void istp_tx_hold(nx_can_isotp_t *iso)
{
    nx_can_isotp_tx_t *tx = &iso->run.tx;
    if (tx->state != NX_ISOTP_TX_WAIT_LINK) {
        tx->state    = NX_ISOTP_TX_WAIT_LINK;
        tx->deadline = istp_now(iso) + iso->cfg.n_as_us;
    }
}

/**
 * @brief  Take the transmission from its first frame to awaiting flow control.
 */
static void istp_tx_ff_done(nx_can_isotp_t *iso, size_t loaded)
{
    nx_can_isotp_tx_t *tx = &iso->run.tx;
    tx->sent     = (uint32_t)loaded;
    tx->last_us  = istp_now(iso);
    tx->deadline = tx->last_us + iso->cfg.n_bs_us;
    tx->state    = NX_ISOTP_TX_WAIT_FC;
}

/**
 * @brief  Pull a send request and drive the transmit side, emitting at most
 *         @p max_frames frames. Returns the number emitted.
 */
static unsigned istp_request_tx(nx_can_isotp_t *iso, unsigned max_frames)
{
    nx_can_isotp_tx_t *tx = &iso->run.tx;
    unsigned emitted = 0u;

    /* Idle: pull the next send off the upper-layer queue and start it. */
    if (tx->state == NX_ISOTP_TX_IDLE) {
        if (nx_queue_pop(iso->cfg.sdu_rx_queue, &tx->sdu) != NX_QUEUE_OK) {
            return 0u;
        }
        const nx_can_isotp_sdu_t *s =
            (const nx_can_isotp_sdu_t *)nx_ref_msg_data(tx->sdu);
        tx->total   = s->len;
        tx->sent    = 0u;
        tx->sn      = 1u;                /* the FF is SN 0; the first CF is SN 1 */
        tx->wft     = 0u;
        tx->ta_type = s->ta_type;
        /* The addressing picks the ID the whole message goes out under. A
         * functional message is single-frame by construction, so nothing beyond
         * this point consults ta_type again. */
        tx->tx_id = (s->ta_type == NX_TP_TA_FUNCTIONAL)
                        ? iso->cfg.func_tx_id
                        : iso->cfg.phys_tx_id;
        if (tx->total <= istp_sf_capacity(iso)) {
            /* Single frame: one SF, done. */
            if (!istp_emit_sf(iso, tx->tx_id, s->data, tx->total)) {
                istp_tx_hold(iso);     /* offer it again while N_As allows */
                return 0u;
            }
            istp_tx_abort(iso, NX_TP_N_OK);
            return 1u;
        }
        /* Multi-frame: FF then wait for flow control. */
        size_t loaded = istp_emit_ff(iso, tx->tx_id, s->data, tx->total);
        if (loaded == 0u) {
            istp_tx_hold(iso);
            return 0u;
        }
        istp_tx_ff_done(iso, loaded);
        return 1u;
    }

    /* Holding a frame the link refused: offer the same one again. Which frame it
     * is follows from how far the message has got, exactly as it did the first
     * time round. */
    if (tx->state == NX_ISOTP_TX_WAIT_LINK) {
        const nx_can_isotp_sdu_t *s =
            (const nx_can_isotp_sdu_t *)nx_ref_msg_data(tx->sdu);
        if (tx->sent == 0u) {
            if (tx->total <= istp_sf_capacity(iso)) {
                if (!istp_emit_sf(iso, tx->tx_id, s->data, tx->total)) {
                    return 0u;
                }
                istp_tx_abort(iso, NX_TP_N_OK);
                return 1u;
            }
            size_t loaded = istp_emit_ff(iso, tx->tx_id, s->data, tx->total);
            if (loaded == 0u) {
                return 0u;
            }
            istp_tx_ff_done(iso, loaded);
            return 1u;
        }
        /* Part way through: the frame owed is the next consecutive one. */
        size_t chunk = (size_t)(tx->total - tx->sent);
        if (chunk > (size_t)iso->cfg.max_frame_len - 1u) {
            chunk = (size_t)iso->cfg.max_frame_len - 1u;
        }
        if (!istp_emit_cf(iso, tx->tx_id, s->data + tx->sent, chunk, tx->sn)) {
            return 0u;
        }
        tx->sent   += (uint32_t)chunk;
        tx->sn      = (uint8_t)((tx->sn + 1u) & 0x0Fu);
        tx->last_us = istp_now(iso);
        tx->state   = NX_ISOTP_TX_SEND;
        emitted     = 1u;
        if (tx->sent >= tx->total) {
            istp_tx_abort(iso, NX_TP_N_OK);
            return emitted;
        }
        if (tx->bs > 0u && --tx->bs == 0u) {
            tx->state    = NX_ISOTP_TX_WAIT_FC;   /* block full: need fresh FC */
            tx->deadline = tx->last_us + iso->cfg.n_bs_us;
            return emitted;
        }
    }

    /* In SEND state: emit CFs up to the per-process budget and the FC block. */
    if (tx->state != NX_ISOTP_TX_SEND) {
        return emitted;
    }
    const nx_can_isotp_sdu_t *s =
        (const nx_can_isotp_sdu_t *)nx_ref_msg_data(tx->sdu);
    uint32_t now = istp_now(iso);
    while (emitted < max_frames && tx->sent < tx->total) {
        if (tx->stmin_us > 0 &&
            (int32_t)(now - tx->last_us) < tx->stmin_us) {
            break;                        /* STmin not yet elapsed */
        }
        size_t chunk = (size_t)(tx->total - tx->sent);
        if (chunk > (size_t)iso->cfg.max_frame_len - 1u) {
            chunk = (size_t)iso->cfg.max_frame_len - 1u;
        }
        if (!istp_emit_cf(iso, tx->tx_id, s->data + tx->sent, chunk, tx->sn)) {
            istp_tx_hold(iso);   /* offer the same frame again while N_As allows */
            return emitted;
        }
        tx->sent   += (uint32_t)chunk;
        tx->sn      = (uint8_t)((tx->sn + 1u) & 0x0Fu);
        tx->last_us = now;
        emitted++;
        now = istp_now(iso);
        if (tx->bs > 0u && --tx->bs == 0u && tx->sent < tx->total) {
            tx->state    = NX_ISOTP_TX_WAIT_FC;   /* block full: need fresh FC */
            tx->deadline = now + iso->cfg.n_bs_us;
            break;
        }
    }
    if (tx->sent >= tx->total) {
        istp_tx_abort(iso, NX_TP_N_OK);      /* every byte is on the link */
    }
    return emitted;
}

/* ======================================================================== */
/* Frame intake                                                             */
/* ======================================================================== */

/**
 * @brief  Route one received frame by its ID and protocol control byte.
 */
static void istp_intake(nx_can_isotp_t *iso, const nx_can_msg_t *frame)
{
    size_t dlen = nx_can_dlc_to_len(frame->flags.bits.dlc);
    if (dlen == 0u) {
        return;                     /* no protocol control byte to act on */
    }

    /* The channel names the bus the frame came off, and an instance serves one
     * bus, so a frame from any other channel belongs to somebody else. */
    if (frame->flags.bits.ch != iso->cfg.ch) {
        return;
    }

    /* An 11-bit and a 29-bit identifier of the same numeric value name two
     * different addresses, so a frame from the other space is not this
     * instance's traffic however its number reads. */
    if ((bool)frame->flags.bits.is_ext != iso->cfg.ext_id) {
        return;
    }

    if (frame->id == iso->cfg.phys_rx_id) {
        if ((frame->data[0] & 0xF0u) == NX_CAN_ISOTP_PCI_FC) {
            istp_tx_apply_fc(iso, frame);   /* feed the in-flight send */
        } else {
            istp_rx_phys_frame(iso, frame);
        }
        return;
    }

    if (iso->cfg.func_rx_id != 0u && frame->id == iso->cfg.func_rx_id) {
        /* Functional addressing is 1:N, so a shared request ID cannot carry
         * per-receiver flow control: only single frames are accepted, and they
         * never disturb the physical reassembly in progress. */
        if ((frame->data[0] & 0xF0u) != NX_CAN_ISOTP_PCI_SF) {
            return;
        }
        size_t plen, hdr;
        if (istp_sf_len(frame->data, dlen, &plen, &hdr)) {
            istp_rx_single(iso, NX_TP_TA_FUNCTIONAL, frame->data, plen, hdr);
        }
        return;
    }

    /* No match: the frame is for another instance; ignore it. */
}

/** @brief Drain the CAN receive queue, routing every frame found there. */
static void istp_drain_can_rx(nx_can_isotp_t *iso)
{
    nx_ref_msg_t *m = NULL;
    while (nx_queue_pop(iso->cfg.can_rx_queue, &m) == NX_QUEUE_OK) {
        const nx_can_msg_t *frame = (const nx_can_msg_t *)nx_ref_msg_data(m);
        if (frame != NULL) {
            istp_intake(iso, frame);
        }
        nx_ref_msg_release(m);      /* consumed: drop this side's reference */
    }
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

/**
 * @brief  Whether a frame length is one a data length code expresses exactly.
 *
 * Rounding a length to a code and reading that code back returns the same
 * number only for the sizes that sit on the wire. A length between two of them
 * would leave a frame the geometry cannot fill, so it is not a usable setting.
 */
static bool istp_frame_len_valid(uint8_t len)
{
    if (len < NX_CAN_ISOTP_FRAME_8) {
        return false;
    }
    return nx_can_dlc_to_len(nx_can_len_to_dlc((uint32_t)len)) == (uint32_t)len;
}

bool nx_can_isotp_init(nx_can_isotp_t *iso, const nx_can_isotp_cfg_t *cfg)
{
    if (iso == NULL || cfg == NULL) {
        return false;
    }
    if (!istp_frame_len_valid(cfg->max_frame_len)) {
        return false;
    }
    /* Only an FD frame carries more than the classic eight bytes, and only an
     * FD frame has a data phase to switch the rate of. */
    if (cfg->max_frame_len > NX_CAN_ISOTP_FRAME_8 && !cfg->fd_frames) {
        return false;
    }
    if (cfg->brs && !cfg->fd_frames) {
        return false;
    }
    /* The channel travels in a 4-bit field, so a larger number would be stamped
     * onto frames as a different channel than the one asked for. */
    if (cfg->ch > NX_CAN_MAX_CH) {
        return false;
    }
    /* A conversation needs both directions, and they must be distinct: flow
     * control goes out on phys_tx_id while frames come in on phys_rx_id. */
    if (cfg->phys_rx_id == 0u || cfg->phys_tx_id == 0u ||
        cfg->phys_rx_id == cfg->phys_tx_id) {
        return false;
    }
    if (cfg->func_rx_id != 0u &&
        (cfg->func_rx_id == cfg->phys_rx_id ||
         cfg->func_rx_id == cfg->phys_tx_id)) {
        return false;
    }
    if (cfg->func_tx_id != 0u &&
        (cfg->func_tx_id == cfg->phys_rx_id ||
         cfg->func_tx_id == cfg->phys_tx_id ||
         cfg->func_tx_id == cfg->func_rx_id)) {
        return false;
    }
    if (cfg->pool == NULL || cfg->sdu_rx_queue == NULL ||
        cfg->sdu_tx_queue == NULL ||
        cfg->can_rx_queue == NULL || cfg->can_tx_queue == NULL) {
        return false;
    }
    if (cfg->get_us == NULL) {
        return false;
    }
    iso->cfg = *cfg;
    /* Resolve "0 means default" once, so the hot paths read a plain value. */
    if (iso->cfg.n_as_us == 0u) {
        iso->cfg.n_as_us = NX_CAN_ISOTP_DEFAULT_TIMEOUT_US;
    }
    if (iso->cfg.n_ar_us == 0u) {
        iso->cfg.n_ar_us = NX_CAN_ISOTP_DEFAULT_TIMEOUT_US;
    }
    if (iso->cfg.n_bs_us == 0u) {
        iso->cfg.n_bs_us = NX_CAN_ISOTP_DEFAULT_TIMEOUT_US;
    }
    if (iso->cfg.n_cr_us == 0u) {
        iso->cfg.n_cr_us = NX_CAN_ISOTP_DEFAULT_TIMEOUT_US;
    }
    if (iso->cfg.n_wft_max == 0u) {
        iso->cfg.n_wft_max = NX_CAN_ISOTP_DEFAULT_WFT_MAX;
    }
    memset(&iso->run, 0, sizeof(iso->run));
    return true;
}

void nx_can_isotp_process(nx_can_isotp_t *iso)
{
    if (iso == NULL) {
        return;
    }
    istp_drain_can_rx(iso);

    uint32_t now = istp_now(iso);
    /* Every guard names the one parameter it enforces. A conversation waiting on
     * the peer expires under N_Cr or N_Bs; one waiting on room in the transmit
     * queue expires under N_Ar or N_As. */
    if (iso->run.rx.state == NX_ISOTP_RX_ASSEM &&
        (int32_t)(now - iso->run.rx.deadline) >= 0) {
        /* The peer stopped sending consecutive frames. */
        istp_rx_abort(iso, NX_TP_N_TIMEOUT_CR);
    }
    if (iso->run.rx.state == NX_ISOTP_RX_FC_WAIT &&
        (int32_t)(now - iso->run.rx.deadline) >= 0) {
        /* The flow control the peer is waiting for never reached the link. */
        istp_rx_abort(iso, NX_TP_N_TIMEOUT_A);
    }
    if (iso->run.rx.state == NX_ISOTP_RX_FC_WAIT) {
        istp_emit_cts(iso);           /* offer the same flow control again */
    }
    if (iso->run.tx.state == NX_ISOTP_TX_WAIT_FC &&
        (int32_t)(now - iso->run.tx.deadline) >= 0) {
        istp_tx_abort(iso, NX_TP_N_TIMEOUT_BS);   /* no flow control in time */
    }
    if (iso->run.tx.state == NX_ISOTP_TX_WAIT_LINK &&
        (int32_t)(now - iso->run.tx.deadline) >= 0) {
        /* Frames have not been leaving: the transmit queue never drained. */
        istp_tx_abort(iso, NX_TP_N_TIMEOUT_A);
    }

    unsigned budget = iso->cfg.tx_frames_per_process;
    if (budget == 0u) {
        budget = 255u;                    /* 0 = no per-call limit */
    }
    istp_request_tx(iso, budget);
}

nx_can_isotp_ret_t nx_can_isotp_send(nx_can_isotp_t *iso, const uint8_t *data,
                                     size_t len, nx_tp_ta_type_t ta_type)
{
    if (iso == NULL || data == NULL || len == 0u) {
        return NX_CAN_ISOTP_ERR_PARAM;
    }
    if (ta_type == NX_TP_TA_FUNCTIONAL) {
        /* Functional addressing is 1:N, so there is no flow control to pace a
         * segmented message: only a single frame can go out, and the ID it uses
         * must be configured. */
        if (iso->cfg.func_tx_id == 0u ||
            len > istp_sf_capacity(iso)) {
            return NX_CAN_ISOTP_ERR_PARAM;
        }
    } else if (ta_type != NX_TP_TA_PHYSICAL) {
        return NX_CAN_ISOTP_ERR_PARAM;          /* addressing not defined */
    }
    /* Refusing here keeps a request that no allocation can express from being
     * truncated into a message that looks well-formed. */
    if (len > NX_CAN_ISOTP_MAX_MSG_LEN) {
        return NX_CAN_ISOTP_ERR_LENGTH;
    }
    nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool,
                                       sizeof(nx_can_isotp_sdu_t) + len);
    if (m == NULL) {
        return NX_CAN_ISOTP_ERR_NOMEM;
    }
    nx_can_isotp_sdu_t *s = (nx_can_isotp_sdu_t *)nx_ref_msg_data(m);
    istp_sdu_head(iso, s, (uint32_t)len, NX_TP_SDU_INDICATION,
                  (uint8_t)ta_type, NX_TP_N_OK);
    memcpy(s->data, data, len);
    nx_ref_msg_ret_t r = nx_ref_msg_publish(m, iso->cfg.sdu_rx_queue);
    if (r != NX_REF_MSG_OK) {
        nx_ref_msg_release(m);
        return NX_CAN_ISOTP_ERR_FULL;
    }
    nx_ref_msg_release(m);
    return NX_CAN_ISOTP_OK;
}
