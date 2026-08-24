/**
 * @file    nx_modbus_rtu_master.c
 * @brief   Implementation of the event-driven Modbus RTU master.
 *
 * See nx_modbus_rtu_master.h for the design. Two halves run in each process() call:
 *   - master_rx: pull bytes, slice length-known responses, check CRC, then dispatch
 *     each frame to the subscribers that own the address it came from.
 *   - master_tx: a small state machine that drains the shared request queue, one frame
 *     at a time, honoring the 3.5-character inter-frame gap.
 */
#include "nx_modbus_rtu_master.h"

#include <string.h>

/** Shortest response ADU: an exception response. */
#define MODBUS_RTU_RSP_MIN 5u

/** Length of a write confirmation (05/06/0F/10): addr + cmd + 2 + 2 + crc(2). */
#define MODBUS_RTU_RSP_WRITE_LEN 8u

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
 * @brief Total ADU length for a response, from its own bytes.
 *
 * Every response's length follows from what has already arrived: the exception flag
 * makes it 5, a read response carries a byte count at offset 2, and the write
 * confirmations are a fixed 8. This is what lets RX slice by length with no
 * inter-character timer.
 *
 * @param  cmd   Function code as received (exception flag still set, if any).
 * @param  p     Frame start.
 * @param  avail Bytes available at @p p.
 * @return length in bytes; 0 for an unsupported code; SIZE_MAX if more bytes are
 *         needed before the length can be determined.
 */
static size_t response_len(uint8_t cmd, const uint8_t *p, size_t avail)
{
    if ((cmd & NX_MODBUS_RTU_EXCEPTION_FLAG) != 0u) {
        return sizeof(nx_modbus_rtu_rsp_exc_t);     /* addr + cmd + code + crc(2) */
    }

    switch (cmd) {
    case NX_MODBUS_FC_READ_COILS:
    case NX_MODBUS_FC_READ_DISCRETE_INPUTS:
    case NX_MODBUS_FC_READ_HOLDING_REGS:
    case NX_MODBUS_FC_READ_INPUT_REGS:
        if (avail < 3u) {
            return SIZE_MAX;                        /* need byte_count at offset 2 */
        }
        /* addr + cmd + byte_count(1) + data(bc) + crc(2) */
        return (size_t)p[2] + 5u;
    case NX_MODBUS_FC_WRITE_SINGLE_COIL:
    case NX_MODBUS_FC_WRITE_SINGLE_REG:
    case NX_MODBUS_FC_WRITE_MULTIPLE_COILS:
    case NX_MODBUS_FC_WRITE_MULTIPLE_REGS:
        return MODBUS_RTU_RSP_WRITE_LEN;
    default:
        return 0u;                                  /* unsupported */
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Whether a subscription claims this response.
 *
 * The address must match. The function code filter, when set, matches the plain code
 * and the exception form of it alike, so the module that asked also hears the refusal.
 */
static bool sub_claims(const nx_modbus_rtu_master_sub_t *sub, uint8_t addr, uint8_t cmd)
{
    if (sub->slave_addr != addr) {
        return false;
    }
    if (sub->func == 0u) {
        return true;                                /* any code from that address */
    }
    return (uint8_t)(cmd & (uint8_t)~NX_MODBUS_RTU_EXCEPTION_FLAG) == sub->func;
}

/**
 * @brief Route one CRC-checked response frame to the subscribers that own it.
 *
 * Copies the ADU once, on the first match, and publishes the same object to every
 * claiming subscription. A response nobody claims is discarded: a master answers
 * nothing, so there is no exception to emit.
 */
static void dispatch(nx_modbus_rtu_master_t *m, const uint8_t *frame, size_t flen)
{
    const uint8_t addr = frame[0];
    const uint8_t cmd  = frame[1];

    nx_ref_msg_t *msg = NULL;

    for (size_t i = 0; i < m->cfg.subs_count; i++) {
        const nx_modbus_rtu_master_sub_t *sub = &m->cfg.subs[i];
        if (!sub_claims(sub, addr, cmd)) {
            continue;
        }
        if (msg == NULL) {
            msg = nx_ref_msg_alloc(m->cfg.pool, flen);
            if (msg == NULL) {
                return;             /* pool exhausted: the response is lost */
            }
            memcpy(nx_ref_msg_data(msg), frame, flen);
        }
        (void)nx_ref_msg_publish(msg, sub->queue);   /* a full queue drops this copy */
    }

    if (msg != NULL) {
        nx_ref_msg_release(msg);    /* drop producer ref; subscribers own it now */
    }
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

static void master_rx(nx_modbus_rtu_master_t *m)
{
    /* 1. Pull whatever is available into the free tail of rx_buf. */
    if (m->run.rx_len < m->cfg.rx_size) {
        size_t got = m->cfg.read(m->cfg.io_ctx,
                                 m->cfg.rx_buf + m->run.rx_len,
                                 m->cfg.rx_size - m->run.rx_len);
        m->run.rx_len += got;
    }

    /* 2. Slice out complete frames from the front. */
    size_t front = 0;
    while ((m->run.rx_len - front) >= MODBUS_RTU_RSP_MIN) {
        uint8_t *p    = m->cfg.rx_buf + front;
        size_t   have = m->run.rx_len - front;

        size_t flen = response_len(p[1], p, have);
        if (flen == 0u) {
            front++;                  /* unsupported code: resync */
            continue;
        }
        if (flen == SIZE_MAX) {
            break;                    /* read header incomplete: wait for more */
        }
        if (flen > m->cfg.rx_size) {
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

        dispatch(m, p, flen);
        front += flen;
    }

    /* 3. Compact any unconsumed tail back to the start. */
    if (front > 0u) {
        if (front < m->run.rx_len) {
            memmove(m->cfg.rx_buf, m->cfg.rx_buf + front, m->run.rx_len - front);
        }
        m->run.rx_len -= front;
    }

    /* 4. Overflow guard: a full buffer that yielded no frame cannot be the start
     *    of any frame that fits - drop one byte so RX keeps making progress. */
    if (m->run.rx_len == m->cfg.rx_size) {
        memmove(m->cfg.rx_buf, m->cfg.rx_buf + 1, m->run.rx_len - 1u);
        m->run.rx_len--;
    }
}

/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

static void master_tx(nx_modbus_rtu_master_t *m)
{
    switch (m->run.tx_state) {
    case TX_IDLE: {
        if (nx_queue_is_empty(m->cfg.request_queue)) {
            break;
        }
        /* Shared/non-exclusive bus: don't start a frame while the interface is busy. */
        if (m->cfg.is_busy != NULL && m->cfg.is_busy(m->cfg.io_ctx)) {
            break;
        }
        nx_ref_msg_t *msg = NULL;
        if (nx_queue_pop(m->cfg.request_queue, &msg) != NX_QUEUE_OK || msg == NULL) {
            break;
        }
        m->run.tx_cur = msg;
        if (m->cfg.dir_tx != NULL) {
            m->cfg.dir_tx(m->cfg.dir_ctx, true);   /* drive the bus */
        }
        if (!m->cfg.write(m->cfg.io_ctx, nx_ref_msg_data(msg), nx_ref_msg_len(msg))) {
            /* The bytes were never accepted, so there is nothing to wait for: drop the
             * frame and release the bus instead of timing the send of a frame that is
             * not on the wire. No response will come; the business module retries. */
            if (m->cfg.dir_tx != NULL) {
                m->cfg.dir_tx(m->cfg.dir_ctx, false);
            }
            nx_ref_msg_release(msg);
            m->run.tx_cur   = NULL;
            m->run.tx_state = TX_IDLE;
            break;
        }
        m->run.tx_state = TX_SENDING;
        break;
    }

    case TX_SENDING:
        /* A NULL is_busy means write() was blocking: treat as already done. */
        if (m->cfg.is_busy == NULL || !m->cfg.is_busy(m->cfg.io_ctx)) {
            if (m->cfg.dir_tx != NULL) {
                m->cfg.dir_tx(m->cfg.dir_ctx, false);  /* release the bus */
            }
            nx_ref_msg_release(m->run.tx_cur);
            m->run.tx_cur = NULL;
            /* Enter the inter-frame gap only if we have a clock and a gap to wait. */
            if (m->cfg.get_us != NULL && m->run.gap_us != 0u) {
                m->run.gap_start_us = m->cfg.get_us();
                m->run.tx_state     = TX_GAP;
            } else {
                m->run.tx_state = TX_IDLE;
            }
        }
        break;

    case TX_GAP:
        /* Only reached when get_us != NULL (guarded above), so the call is safe. */
        if ((uint32_t)(m->cfg.get_us() - m->run.gap_start_us) >= m->run.gap_us) {
            m->run.tx_state = TX_IDLE;
        }
        break;

    default:
        m->run.tx_state = TX_IDLE;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API: lifecycle                                              */
/* ------------------------------------------------------------------ */

bool nx_modbus_rtu_master_init(nx_modbus_rtu_master_t *m, const nx_modbus_rtu_master_cfg_t *cfg)
{
    if (m == NULL || cfg == NULL) {
        return false;
    }
    if (cfg->pool == NULL || cfg->rx_buf == NULL || cfg->request_queue == NULL) {
        return false;
    }
    if (cfg->read == NULL || cfg->write == NULL) {
        return false;
    }
    if (cfg->subs == NULL && cfg->subs_count != 0u) {
        return false;
    }
    for (size_t i = 0; i < cfg->subs_count; i++) {
        /* A subscription without a queue would silently swallow every response it owns. */
        if (cfg->subs[i].queue == NULL) {
            return false;
        }
        /* No response ever carries the broadcast address, so such a claim is dead. */
        if (cfg->subs[i].slave_addr < NX_MODBUS_RTU_ADDR_MIN ||
            cfg->subs[i].slave_addr > NX_MODBUS_RTU_ADDR_MAX) {
            return false;
        }
    }
    if (cfg->rx_size < sizeof(nx_modbus_rtu_rsp_exc_t) + 1u) {
        return false;
    }

    m->cfg              = *cfg;
    m->run.rx_len       = 0u;
    m->run.tx_state     = TX_IDLE;
    m->run.gap_start_us = 0u;
    m->run.tx_cur       = NULL;

    /* Resolve the TX inter-frame gap: 3.5 chars from baud (11 bits/char):
     * 3.5 * 11 * 1e6 / baud us. +1 rounds up. 0 baud => no gap. */
    if (cfg->baud_rate != 0u) {
        m->run.gap_us = (uint32_t)(38500000UL / cfg->baud_rate) + 1u;
    } else {
        m->run.gap_us = 0u;
    }

    return true;
}

void nx_modbus_rtu_master_deinit(nx_modbus_rtu_master_t *m)
{
    if (m == NULL) {
        return;
    }

    /* A frame caught mid-transmit still holds a pool block; give it back. */
    if (m->run.tx_cur != NULL) {
        nx_ref_msg_release(m->run.tx_cur);
        m->run.tx_cur = NULL;
    }
    if (m->cfg.dir_tx != NULL) {
        m->cfg.dir_tx(m->cfg.dir_ctx, false);   /* leave the bus released */
    }

    m->run.rx_len   = 0u;
    m->run.tx_state = TX_IDLE;
}

void nx_modbus_rtu_master_process(nx_modbus_rtu_master_t *m)
{
    if (m == NULL) {
        return;
    }
    master_rx(m);
    master_tx(m);
}

/* ------------------------------------------------------------------ */
/* Request helpers (shared by the public request builders)            */
/* ------------------------------------------------------------------ */

/**
 * @brief Stamp a built frame's CRC, queue it, and drop the producer reference.
 * @return NX_MODBUS_RTU_MASTER_OK if the request was queued, ERR_FULL if the queue was.
 */
static nx_modbus_rtu_master_ret_t request_send(nx_ref_msg_t *msg, nx_queue_t *request_queue,
                                               size_t len)
{
    nx_modbus_rtu_set_crc((uint8_t *)nx_ref_msg_data(msg), len);

    bool queued = (nx_ref_msg_publish(msg, request_queue) == NX_REF_MSG_OK);
    nx_ref_msg_release(msg);          /* drop the producer reference either way */

    return queued ? NX_MODBUS_RTU_MASTER_OK : NX_MODBUS_RTU_MASTER_ERR_FULL;
}

/**
 * @brief Build one of the six fixed-layout requests (01..06) and queue it.
 *
 * The fixed requests differ only in their function code and the meaning of the two
 * 16-bit fields, so they share one builder. Validation of those fields is the caller's,
 * since what is legal depends on the code.
 */
static nx_modbus_rtu_master_ret_t request_fixed(nx_tiered_mem_pool_t *pool,
                                                nx_queue_t           *request_queue,
                                                uint8_t               slave_addr,
                                                uint8_t               cmd,
                                                uint16_t              field_a,
                                                uint16_t              field_b)
{
    if (pool == NULL || request_queue == NULL || slave_addr > NX_MODBUS_RTU_ADDR_MAX) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }

    nx_ref_msg_t *msg = nx_ref_msg_alloc(pool, sizeof(nx_modbus_rtu_req_fix_t));
    if (msg == NULL) {
        return NX_MODBUS_RTU_MASTER_ERR_NOMEM;
    }

    nx_modbus_rtu_req_fix_t *r = (nx_modbus_rtu_req_fix_t *)nx_ref_msg_data(msg);
    r->addr = slave_addr;
    r->cmd  = cmd;
    /* 16-bit fields go on the wire high byte first. */
    r->addr_h = (uint8_t)(field_a >> 8);
    r->addr_l = (uint8_t)(field_a & 0xFFu);
    r->qty_h  = (uint8_t)(field_b >> 8);
    r->qty_l  = (uint8_t)(field_b & 0xFFu);

    return request_send(msg, request_queue, sizeof(*r));
}

/**
 * @brief Build one of the two variable-layout requests (0F/10) and queue it.
 *
 * @param  expect_bytes The byte count this quantity implies; @p payload_len must equal
 *                      it, which is the frame-internal rule the spec fixes for the code.
 */
static nx_modbus_rtu_master_ret_t request_var(nx_tiered_mem_pool_t *pool,
                                              nx_queue_t           *request_queue,
                                              uint8_t               slave_addr,
                                              uint8_t               cmd,
                                              uint16_t              start_addr,
                                              uint16_t              qty,
                                              const uint8_t        *payload,
                                              size_t                payload_len,
                                              size_t                expect_bytes)
{
    if (pool == NULL || request_queue == NULL || slave_addr > NX_MODBUS_RTU_ADDR_MAX) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }
    if (payload == NULL || payload_len != expect_bytes) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }

    /* addr + cmd + start(2) + qty(2) + byte_count(1) + data + crc(2) */
    const size_t req_len = sizeof(nx_modbus_rtu_req_var_t) + payload_len + 2u;

    nx_ref_msg_t *msg = nx_ref_msg_alloc(pool, req_len);
    if (msg == NULL) {
        return NX_MODBUS_RTU_MASTER_ERR_NOMEM;
    }

    nx_modbus_rtu_req_var_t *r = (nx_modbus_rtu_req_var_t *)nx_ref_msg_data(msg);
    r->addr = slave_addr;
    r->cmd  = cmd;
    /* 16-bit fields go on the wire high byte first. */
    r->addr_h = (uint8_t)(start_addr >> 8);
    r->addr_l = (uint8_t)(start_addr & 0xFFu);
    r->qty_h  = (uint8_t)(qty >> 8);
    r->qty_l  = (uint8_t)(qty & 0xFFu);
    r->byte_count = (uint8_t)payload_len;
    memcpy(r->payload, payload, payload_len);

    return request_send(msg, request_queue, req_len);
}

/**
 * @brief Shared front half of the four read builders: refuse a broadcast, check quantity.
 *
 * A broadcast read would ask every slave on the bus to answer at once, so it is refused
 * here rather than sent. The quantity range is the protocol's for the code.
 */
static nx_modbus_rtu_master_ret_t read_allowed(uint8_t slave_addr, uint16_t qty, uint16_t qty_max)
{
    if (slave_addr < NX_MODBUS_RTU_ADDR_MIN || slave_addr > NX_MODBUS_RTU_ADDR_MAX) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }
    if (qty < 1u || qty > qty_max) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }
    return NX_MODBUS_RTU_MASTER_OK;
}

/* ------------------------------------------------------------------ */
/* Public API: request builders                                       */
/* ------------------------------------------------------------------ */

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_coils(nx_tiered_mem_pool_t *pool,
                                                           nx_queue_t           *request_queue,
                                                           uint8_t               slave_addr,
                                                           uint16_t              start_addr,
                                                           uint16_t              qty)
{
    nx_modbus_rtu_master_ret_t ret = read_allowed(slave_addr, qty, 2000u);
    if (ret != NX_MODBUS_RTU_MASTER_OK) {
        return ret;
    }
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_READ_COILS, start_addr, qty);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_discrete_inputs(nx_tiered_mem_pool_t *pool,
                                                                     nx_queue_t           *request_queue,
                                                                     uint8_t               slave_addr,
                                                                     uint16_t              start_addr,
                                                                     uint16_t              qty)
{
    nx_modbus_rtu_master_ret_t ret = read_allowed(slave_addr, qty, 2000u);
    if (ret != NX_MODBUS_RTU_MASTER_OK) {
        return ret;
    }
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_READ_DISCRETE_INPUTS, start_addr, qty);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_holding_regs(nx_tiered_mem_pool_t *pool,
                                                                  nx_queue_t           *request_queue,
                                                                  uint8_t               slave_addr,
                                                                  uint16_t              start_addr,
                                                                  uint16_t              qty)
{
    nx_modbus_rtu_master_ret_t ret = read_allowed(slave_addr, qty, 125u);
    if (ret != NX_MODBUS_RTU_MASTER_OK) {
        return ret;
    }
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_READ_HOLDING_REGS, start_addr, qty);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_read_input_regs(nx_tiered_mem_pool_t *pool,
                                                                nx_queue_t           *request_queue,
                                                                uint8_t               slave_addr,
                                                                uint16_t              start_addr,
                                                                uint16_t              qty)
{
    nx_modbus_rtu_master_ret_t ret = read_allowed(slave_addr, qty, 125u);
    if (ret != NX_MODBUS_RTU_MASTER_OK) {
        return ret;
    }
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_READ_INPUT_REGS, start_addr, qty);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_single_coil(nx_tiered_mem_pool_t *pool,
                                                                  nx_queue_t           *request_queue,
                                                                  uint8_t               slave_addr,
                                                                  uint16_t              coil_addr,
                                                                  bool                  on)
{
    /* The output value is only ever 0xFF00 (on) or 0x0000 (off). */
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_WRITE_SINGLE_COIL, coil_addr,
                         on ? 0xFF00u : 0x0000u);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_single_reg(nx_tiered_mem_pool_t *pool,
                                                                 nx_queue_t           *request_queue,
                                                                 uint8_t               slave_addr,
                                                                 uint16_t              reg_addr,
                                                                 uint16_t              value)
{
    return request_fixed(pool, request_queue, slave_addr,
                         NX_MODBUS_FC_WRITE_SINGLE_REG, reg_addr, value);
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_multiple_coils(nx_tiered_mem_pool_t *pool,
                                                                     nx_queue_t           *request_queue,
                                                                     uint8_t               slave_addr,
                                                                     uint16_t              start_addr,
                                                                     uint16_t              qty,
                                                                     const uint8_t        *bits,
                                                                     size_t                bits_len)
{
    if (qty < 1u || qty > 1968u) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }
    /* byte_count must carry exactly ceil(qty/8) bytes of coil bits. */
    return request_var(pool, request_queue, slave_addr, NX_MODBUS_FC_WRITE_MULTIPLE_COILS,
                       start_addr, qty, bits, bits_len, (size_t)((qty + 7u) / 8u));
}

nx_modbus_rtu_master_ret_t nx_modbus_rtu_master_write_multiple_regs(nx_tiered_mem_pool_t *pool,
                                                                    nx_queue_t           *request_queue,
                                                                    uint8_t               slave_addr,
                                                                    uint16_t              start_addr,
                                                                    uint16_t              qty,
                                                                    const uint8_t        *regs,
                                                                    size_t                regs_len)
{
    if (qty < 1u || qty > 123u) {
        return NX_MODBUS_RTU_MASTER_ERR_PARAM;
    }
    /* byte_count must carry exactly two bytes per register. */
    return request_var(pool, request_queue, slave_addr, NX_MODBUS_FC_WRITE_MULTIPLE_REGS,
                       start_addr, qty, regs, regs_len, (size_t)qty * 2u);
}

/* ------------------------------------------------------------------ */
/* Public API: response inspection                                    */
/* ------------------------------------------------------------------ */

bool nx_modbus_rtu_master_rsp_is_exception(const uint8_t *frame, size_t flen, uint8_t *exc)
{
    if (frame == NULL || flen < sizeof(nx_modbus_rtu_rsp_exc_t)) {
        return false;
    }
    if ((frame[1] & NX_MODBUS_RTU_EXCEPTION_FLAG) == 0u) {
        return false;
    }
    if (exc != NULL) {
        *exc = frame[2];
    }
    return true;
}

const uint8_t *nx_modbus_rtu_master_rsp_data(const uint8_t *frame, size_t flen, size_t *len)
{
    if (len != NULL) {
        *len = 0u;
    }
    if (frame == NULL || flen < MODBUS_RTU_RSP_MIN) {
        return NULL;
    }

    const uint8_t cmd = frame[1];
    if (cmd != NX_MODBUS_FC_READ_COILS && cmd != NX_MODBUS_FC_READ_DISCRETE_INPUTS &&
        cmd != NX_MODBUS_FC_READ_HOLDING_REGS && cmd != NX_MODBUS_FC_READ_INPUT_REGS) {
        return NULL;    /* an exception or a write confirmation carries no payload */
    }

    /* The byte count must agree with the frame it arrived in. */
    const size_t bc = (size_t)frame[2];
    if (bc == 0u || bc + 5u != flen) {
        return NULL;
    }

    if (len != NULL) {
        *len = bc;
    }
    return &frame[3];
}




