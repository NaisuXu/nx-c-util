/**
 * @file    nx_modbus_rtu_slave.c
 * @brief   Implementation of the event-driven Modbus RTU slave.
 *
 * See nx_modbus_rtu_slave.h for the design. Two halves run in each process() call:
 *   - slave_rx: pull bytes, slice length-known frames, CRC + address check, then
 *     dispatch each frame to its subscribers (or emit an exception).
 *   - slave_tx: a small state machine that drains the shared response queue, one
 *     frame at a time, honoring the 3.5-character inter-frame gap.
 */
#include "nx_modbus_rtu_slave.h"

#include <string.h>

/** Shortest request ADU: fixed-layout request for function codes 01..06. */
#define MODBUS_RTU_ADU_MIN 8u

/** TX state machine states. */
enum {
    TX_IDLE = 0,   /**< Nothing in flight; may start the next queued frame */
    TX_SENDING,    /**< A frame is on the wire; waiting for the interface to go idle */
    TX_GAP         /**< Frame sent; waiting out the inter-frame silence */
};

/* ------------------------------------------------------------------ */
/* Frame geometry                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Total ADU length for a request, from its function code.
 * @return length in bytes; 0 for an unsupported code; SIZE_MAX if more bytes are
 *         needed before the length can be determined (variable frame header).
 */
static size_t frame_len(uint8_t cmd, const uint8_t *p, size_t avail)
{
    switch (cmd) {
    case NX_MODBUS_FC_READ_COILS:
    case NX_MODBUS_FC_READ_DISCRETE_INPUTS:
    case NX_MODBUS_FC_READ_HOLDING_REGS:
    case NX_MODBUS_FC_READ_INPUT_REGS:
    case NX_MODBUS_FC_WRITE_SINGLE_COIL:
    case NX_MODBUS_FC_WRITE_SINGLE_REG:
        return MODBUS_RTU_ADU_MIN;                 /* addr+cmd + 2 + 2 + crc(2) */
    case NX_MODBUS_FC_WRITE_MULTIPLE_COILS:
    case NX_MODBUS_FC_WRITE_MULTIPLE_REGS:
        if (avail < 7u) {
            return SIZE_MAX;                        /* need byte_count at offset 6 */
        }
        /* addr+cmd + start(2) + qty(2) + byte_count(1) + data(bc) + crc(2) */
        return (size_t)p[6] + 9u;
    default:
        return 0u;                                 /* unsupported */
    }
}

/**
 * @brief The register span a request touches, as an inclusive [lo, hi].
 */
static void request_span(uint8_t cmd, const uint8_t *f, uint16_t *lo, uint16_t *hi)
{
    uint16_t a = (uint16_t)(((uint16_t)f[2] << 8) | f[3]);   /* address field */

    if (cmd == NX_MODBUS_FC_WRITE_SINGLE_COIL || cmd == NX_MODBUS_FC_WRITE_SINGLE_REG) {
        *lo = a;
        *hi = a;                                             /* single address */
    } else {
        uint16_t q = (uint16_t)(((uint16_t)f[4] << 8) | f[5]);  /* quantity */
        *lo = a;
        *hi = (q == 0u) ? a : (uint16_t)(a + q - 1u);
    }
}

/**
 * @brief Structural well-formedness check on a request's quantity / byte_count.
 *
 * These are frame-internal rules the Modbus spec fixes for every device: a quantity
 * that must fall in a protocol range, and (for the multi-writes) a byte_count that
 * must equal a function of that quantity. They depend only on the frame, not on the
 * device's register map, so the slave can settle them itself and reject a violation
 * directly with ILLEGAL_DATA_VALUE. Whether a well-formed value is operationally
 * acceptable for a given register stays with the owning business module.
 *
 * @return 0 if well-formed; otherwise the exception code to return (0x03).
 */
static uint8_t request_value_exc(uint8_t cmd, const uint8_t *f)
{
    uint16_t qty = (uint16_t)(((uint16_t)f[4] << 8) | f[5]);

    switch (cmd) {
    case NX_MODBUS_FC_READ_COILS:
    case NX_MODBUS_FC_READ_DISCRETE_INPUTS:
        if (qty < 1u || qty > 2000u) {                 /* max 0x7D0 items */
            return NX_MODBUS_EXC_ILLEGAL_DATA_VALUE;
        }
        break;
    case NX_MODBUS_FC_READ_HOLDING_REGS:
    case NX_MODBUS_FC_READ_INPUT_REGS:
        if (qty < 1u || qty > 125u) {                  /* max 0x7D registers */
            return NX_MODBUS_EXC_ILLEGAL_DATA_VALUE;
        }
        break;
    case NX_MODBUS_FC_WRITE_MULTIPLE_COILS:
        /* byte_count (f[6]) must carry exactly ceil(qty/8) bytes of coil bits. */
        if (qty < 1u || qty > 1968u || f[6] != (uint8_t)((qty + 7u) / 8u)) {
            return NX_MODBUS_EXC_ILLEGAL_DATA_VALUE;
        }
        break;
    case NX_MODBUS_FC_WRITE_MULTIPLE_REGS:
        /* byte_count (f[6]) must carry exactly two bytes per register. */
        if (qty < 1u || qty > 123u || f[6] != (uint8_t)(qty * 2u)) {
            return NX_MODBUS_EXC_ILLEGAL_DATA_VALUE;
        }
        break;
    case NX_MODBUS_FC_WRITE_SINGLE_COIL:
        /* the output value (f[4..5]) is only ever 0x0000 (off) or 0xFF00 (on). */
        if (qty != 0x0000u && qty != 0xFF00u) {
            return NX_MODBUS_EXC_ILLEGAL_DATA_VALUE;
        }
        break;
    default:
        break;                                         /* 06: any register value is legal */
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Answer a request with an exception response; a broadcast is left unanswered.
 */
static void send_exception(nx_modbus_rtu_slave_t *s, const uint8_t *frame, uint8_t exc)
{
    (void)nx_modbus_rtu_slave_reply_exception(s->cfg.pool, s->cfg.response_queue,
                                              (const nx_modbus_rtu_header_t *)frame, exc);
}

/**
 * @brief Route one validated request frame to its subscribers.
 *
 * Resolves the request in Modbus exception order: function support (0x01), then
 * structural value legality (0x03), then address containment (0x02). A well-formed,
 * in-range request is fanned out (zero-copy) to every subscription that owns it. A
 * broadcast is never answered, whatever the outcome.
 */
static void dispatch(nx_modbus_rtu_slave_t *s, const uint8_t *frame, size_t flen)
{
    const uint8_t cmd = frame[1];

    /* 1. Function support: is this code claimed by any subscription? */
    bool func_ok = false;
    for (size_t i = 0; i < s->cfg.subs_count; i++) {
        if (s->cfg.subs[i].func == cmd) {
            func_ok = true;
            break;
        }
    }
    if (!func_ok) {
        send_exception(s, frame, NX_MODBUS_EXC_ILLEGAL_FUNCTION);
        return;
    }

    /* 2. Structural value legality (quantity range, byte_count vs quantity). */
    uint8_t exc = request_value_exc(cmd, frame);
    if (exc != 0u) {
        send_exception(s, frame, exc);
        return;
    }

    /* 3. Address containment: copy the ADU once, publish the same object to every
     *    subscription whose owned range contains the whole span it touches. */
    uint16_t lo, hi;
    request_span(cmd, frame, &lo, &hi);

    nx_ref_msg_t *msg = NULL;
    for (size_t i = 0; i < s->cfg.subs_count; i++) {
        const nx_modbus_rtu_slave_sub_t *sub = &s->cfg.subs[i];
        if (sub->func != cmd || lo < sub->addr_min || hi > sub->addr_max) {
            continue;
        }
        if (msg == NULL) {
            msg = nx_ref_msg_alloc(s->cfg.pool, flen);
            if (msg == NULL) {
                send_exception(s, frame, NX_MODBUS_EXC_SLAVE_DEVICE_FAILURE);
                return;
            }
            memcpy(nx_ref_msg_data(msg), frame, flen);
        }
        (void)nx_ref_msg_publish(msg, sub->queue);   /* full queue -> skipped */
    }

    if (msg != NULL) {
        nx_ref_msg_release(msg);      /* drop producer ref; subscribers own it now */
        return;
    }

    /* Function supported and frame well-formed, but no subscriber owns the span. */
    send_exception(s, frame, NX_MODBUS_EXC_ILLEGAL_DATA_ADDR);
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

static void slave_rx(nx_modbus_rtu_slave_t *s)
{
    /* 1. Pull whatever is available into the free tail of rx_buf. */
    if (s->run.rx_len < s->cfg.rx_size) {
        size_t got = s->cfg.read(s->cfg.io_ctx,
                                 s->cfg.rx_buf + s->run.rx_len,
                                 s->cfg.rx_size - s->run.rx_len);
        s->run.rx_len += got;
    }

    /* 2. Slice out complete frames from the front. */
    size_t front = 0;
    while ((s->run.rx_len - front) >= MODBUS_RTU_ADU_MIN) {
        uint8_t *p    = s->cfg.rx_buf + front;
        size_t   have = s->run.rx_len - front;

        /* Accept our unicast address; broadcast only when configured to answer it. */
        const bool is_bcast = (p[0] == NX_MODBUS_RTU_ADDR_BROADCAST);
        if (p[0] != s->cfg.slave_addr && !(is_bcast && s->cfg.accept_broadcast)) {
            front++;                  /* not for us: resync one byte */
            continue;
        }

        size_t flen = frame_len(p[1], p, have);
        if (flen == 0u) {
            front++;                  /* unsupported code: resync */
            continue;
        }
        if (flen == SIZE_MAX) {
            break;                    /* variable header incomplete: wait for more */
        }
        if (flen > s->cfg.rx_size) {
            front++;                  /* can never fit our buffer: resync */
            continue;
        }
        if (have < flen) {
            break;                    /* frame not fully arrived: wait for more */
        }

        if (!nx_modbus_rtu_check_crc(p, flen)) {
            front++;                  /* bad CRC: resync */
            continue;
        }

        dispatch(s, p, flen);
        front += flen;
    }

    /* 3. Compact any unconsumed tail back to the start. */
    if (front > 0u) {
        if (front < s->run.rx_len) {
            memmove(s->cfg.rx_buf, s->cfg.rx_buf + front, s->run.rx_len - front);
        }
        s->run.rx_len -= front;
    }

    /* 4. Overflow guard: a full buffer that yielded no frame cannot be the start
     *    of any frame that fits - drop one byte so RX keeps making progress. */
    if (s->run.rx_len == s->cfg.rx_size) {
        memmove(s->cfg.rx_buf, s->cfg.rx_buf + 1, s->run.rx_len - 1u);
        s->run.rx_len--;
    }
}

/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

static void slave_tx(nx_modbus_rtu_slave_t *s)
{
    switch (s->run.tx_state) {
    case TX_IDLE: {
        if (nx_queue_is_empty(s->cfg.response_queue)) {
            break;
        }
        /* Shared/non-exclusive bus: don't start a frame while the interface is busy. */
        if (s->cfg.is_busy != NULL && s->cfg.is_busy(s->cfg.io_ctx)) {
            break;
        }
        nx_ref_msg_t *msg = NULL;
        if (nx_queue_pop(s->cfg.response_queue, &msg) != NX_QUEUE_OK || msg == NULL) {
            break;
        }
        s->run.tx_cur = msg;
        if (s->cfg.dir_tx != NULL) {
            s->cfg.dir_tx(s->cfg.dir_ctx, true);   /* drive the bus */
        }
        (void)s->cfg.write(s->cfg.io_ctx, nx_ref_msg_data(msg), nx_ref_msg_len(msg));
        s->run.tx_state = TX_SENDING;
        break;
    }

    case TX_SENDING:
        /* A NULL is_busy means write() was blocking: treat as already done. */
        if (s->cfg.is_busy == NULL || !s->cfg.is_busy(s->cfg.io_ctx)) {
            if (s->cfg.dir_tx != NULL) {
                s->cfg.dir_tx(s->cfg.dir_ctx, false);  /* release the bus */
            }
            nx_ref_msg_release(s->run.tx_cur);
            s->run.tx_cur = NULL;
            /* Enter the inter-frame gap only if we have a clock and a gap to wait. */
            if (s->cfg.get_us != NULL && s->run.gap_us != 0u) {
                s->run.gap_start_us = s->cfg.get_us();
                s->run.tx_state     = TX_GAP;
            } else {
                s->run.tx_state = TX_IDLE;
            }
        }
        break;

    case TX_GAP:
        /* Only reached when get_us != NULL (guarded above), so the call is safe. */
        if ((uint32_t)(s->cfg.get_us() - s->run.gap_start_us) >= s->run.gap_us) {
            s->run.tx_state = TX_IDLE;
        }
        break;

    default:
        s->run.tx_state = TX_IDLE;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool nx_modbus_rtu_slave_init(nx_modbus_rtu_slave_t *s, const nx_modbus_rtu_slave_cfg_t *cfg)
{
    if (s == NULL || cfg == NULL) {
        return false;
    }
    if (cfg->pool == NULL || cfg->rx_buf == NULL || cfg->response_queue == NULL) {
        return false;
    }
    if (cfg->read == NULL || cfg->write == NULL) {
        return false;
    }
    if (cfg->subs == NULL && cfg->subs_count != 0u) {
        return false;
    }
    if (cfg->rx_size < sizeof(nx_modbus_rtu_req_fix_t) + 1u) {
        return false;
    }
    if (cfg->slave_addr < NX_MODBUS_RTU_ADDR_MIN || cfg->slave_addr > NX_MODBUS_RTU_ADDR_MAX) {
        return false;
    }

    s->cfg              = *cfg;
    s->run.rx_len       = 0u;
    s->run.tx_state     = TX_IDLE;
    s->run.gap_start_us = 0u;
    s->run.tx_cur       = NULL;

    /* Resolve the TX inter-frame gap: 3.5 chars from baud (11 bits/char):
     * 3.5 * 11 * 1e6 / baud us. +1 rounds up. 0 baud => no gap. */
    if (cfg->baud_rate != 0u) {
        s->run.gap_us = (uint32_t)(38500000UL / cfg->baud_rate) + 1u;
    } else {
        s->run.gap_us = 0u;
    }

    return true;
}

void nx_modbus_rtu_slave_process(nx_modbus_rtu_slave_t *s)
{
    if (s == NULL) {
        return;
    }
    slave_rx(s);
    slave_tx(s);
}

/* ------------------------------------------------------------------ */
/* Response helpers (shared by the three public reply_* builders)      */
/* ------------------------------------------------------------------ */

/**
 * @brief Whether a reply may be built at all: arguments present, and not a broadcast.
 *
 * Taking the address from the request means this check and the reply's address field
 * can never disagree.
 *
 * @return NX_MODBUS_RTU_SLAVE_OK when a reply may be built.
 */
static nx_modbus_rtu_slave_ret_t reply_allowed(const nx_tiered_mem_pool_t   *pool,
                                               const nx_queue_t             *response_queue,
                                               const nx_modbus_rtu_header_t *request)
{
    if (pool == NULL || response_queue == NULL || request == NULL) {
        return NX_MODBUS_RTU_SLAVE_ERR_PARAM;
    }
    if (request->addr == NX_MODBUS_RTU_ADDR_BROADCAST) {
        return NX_MODBUS_RTU_SLAVE_ERR_BROADCAST;
    }
    return NX_MODBUS_RTU_SLAVE_OK;
}

/**
 * @brief Stamp a built frame's CRC, queue it, and drop the producer reference.
 * @return NX_MODBUS_RTU_SLAVE_OK if the response was queued, ERR_FULL if the queue was.
 */
static nx_modbus_rtu_slave_ret_t reply_send(nx_ref_msg_t *msg, nx_queue_t *response_queue,
                                            size_t len)
{
    nx_modbus_rtu_set_crc((uint8_t *)nx_ref_msg_data(msg), len);

    bool queued = (nx_ref_msg_publish(msg, response_queue) == NX_REF_MSG_OK);
    nx_ref_msg_release(msg);          /* drop the producer reference either way */

    return queued ? NX_MODBUS_RTU_SLAVE_OK : NX_MODBUS_RTU_SLAVE_ERR_FULL;
}

nx_modbus_rtu_slave_ret_t nx_modbus_rtu_slave_reply_read(nx_tiered_mem_pool_t         *pool,
                                                         nx_queue_t                   *response_queue,
                                                         const nx_modbus_rtu_header_t *request,
                                                         const uint8_t                *data,
                                                         size_t                        len)
{
    nx_modbus_rtu_slave_ret_t ret = reply_allowed(pool, response_queue, request);
    if (ret != NX_MODBUS_RTU_SLAVE_OK) {
        return ret;
    }
    /* addr + cmd + byte_count (3) + data + crc (2) must fit one ADU. */
    if (data == NULL || len == 0u || len > (NX_MODBUS_RTU_MAX_ADU - 5u)) {
        return NX_MODBUS_RTU_SLAVE_ERR_PARAM;
    }

    const size_t rsp_len = sizeof(nx_modbus_rtu_rsp_var_t) + len + 2u;

    nx_ref_msg_t *msg = nx_ref_msg_alloc(pool, rsp_len);
    if (msg == NULL) {
        return NX_MODBUS_RTU_SLAVE_ERR_NOMEM;   /* master will time out */
    }

    nx_modbus_rtu_rsp_var_t *r = (nx_modbus_rtu_rsp_var_t *)nx_ref_msg_data(msg);
    r->addr       = request->addr;
    r->cmd        = request->cmd;
    r->byte_count = (uint8_t)len;
    memcpy(r->payload, data, len);

    return reply_send(msg, response_queue, rsp_len);
}

nx_modbus_rtu_slave_ret_t nx_modbus_rtu_slave_reply_write(nx_tiered_mem_pool_t          *pool,
                                                          nx_queue_t                    *response_queue,
                                                          const nx_modbus_rtu_req_fix_t *request)
{
    nx_modbus_rtu_slave_ret_t ret =
        reply_allowed(pool, response_queue, (const nx_modbus_rtu_header_t *)request);
    if (ret != NX_MODBUS_RTU_SLAVE_OK) {
        return ret;
    }

    nx_ref_msg_t *msg = nx_ref_msg_alloc(pool, sizeof(nx_modbus_rtu_rsp_fix_t));
    if (msg == NULL) {
        return NX_MODBUS_RTU_SLAVE_ERR_NOMEM;   /* master will time out */
    }

    /* The response echoes the request's first six bytes; only the CRC is recomputed. */
    nx_modbus_rtu_rsp_fix_t *r = (nx_modbus_rtu_rsp_fix_t *)nx_ref_msg_data(msg);
    r->addr   = request->addr;
    r->cmd    = request->cmd;
    r->addr_h = request->addr_h;
    r->addr_l = request->addr_l;
    r->data_h = request->qty_h;
    r->data_l = request->qty_l;

    return reply_send(msg, response_queue, sizeof(*r));
}

nx_modbus_rtu_slave_ret_t nx_modbus_rtu_slave_reply_exception(nx_tiered_mem_pool_t         *pool,
                                                              nx_queue_t                   *response_queue,
                                                              const nx_modbus_rtu_header_t *request,
                                                              uint8_t                       exception_code)
{
    nx_modbus_rtu_slave_ret_t ret = reply_allowed(pool, response_queue, request);
    if (ret != NX_MODBUS_RTU_SLAVE_OK) {
        return ret;
    }

    nx_ref_msg_t *msg = nx_ref_msg_alloc(pool, sizeof(nx_modbus_rtu_rsp_exc_t));
    if (msg == NULL) {
        return NX_MODBUS_RTU_SLAVE_ERR_NOMEM;   /* master will time out */
    }

    nx_modbus_rtu_rsp_exc_t *r = (nx_modbus_rtu_rsp_exc_t *)nx_ref_msg_data(msg);
    r->addr           = request->addr;
    r->cmd            = (uint8_t)(request->cmd | NX_MODBUS_RTU_EXCEPTION_FLAG);
    r->exception_code = exception_code;

    return reply_send(msg, response_queue, sizeof(*r));
}
