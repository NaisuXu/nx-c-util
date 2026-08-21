/**
 * @file    nx_modbus_rtu_slave_example.c
 * @brief   Usage example for nx_modbus_rtu_slave: subscription-based dispatch.
 *
 * Shows the intended architecture end to end, with no real hardware:
 *
 *   master bytes --read()--> [slave] --slice/CRC/addr--> dispatch by (func,range)
 *        |                                                    |
 *        |                              +---------------------+---------------------+
 *        |                              v                                           v
 *        |                        q_valve (a business)                        q_sys (a business)
 *        |                              |  build response                           |  build response
 *        |                              +------------> response_queue <-------------+
 *        |                                                    |
 *        +<---------------------- write() <----[slave]--------+  (also emits exceptions)
 *
 * The two "business modules" here (valve, sys) both consume Modbus reads but own
 * different address ranges: each subscribes to the ranges it owns and receives
 * matching requests as data on its own queue.
 *
 * Eight requests are fed in to exercise every path:
 *   1. read holding regs @0x0000 x2   -> valve
 *   2. read input regs   @0x8000 x4   -> sys
 *   3. write single reg  @0x0001, addressed to broadcast (0) -> dropped, silent
 *   4. read coils        @0x0000 x8   -> no subscriber for func 0x01 -> exc 0x01
 *   5. read holding regs @0x0020 x1   -> func owned, addr out of range -> exc 0x02
 *   6. write single reg  @0x0002 = 4000 -> well-formed, valve rejects it -> exc 0x03
 *   7. write single reg  @0x0002 = 60   -> accepted -> write confirmation echoed
 *   8. read holding regs @0xFFFF x2   -> span runs past 0xFFFF -> exc 0x02
 *
 * All storage is static; the example self-checks with asserts and prints the wire.
 */
#include "nx_middleware_examples.h"
#include "src/middleware/nx_modbus_rtu_slave.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* This slave's address on the bus. */
#define SLAVE_ADDR 0x11u

/* ------------------------------------------------------------------ */
/* Mock RS-485 endpoint: a scripted RX stream and a captured TX buffer */
/* ------------------------------------------------------------------ */
/* A typical application product owns a single bus and driver, so the endpoint is
 * just module-level state the callbacks reach directly - io_ctx stays NULL. (io_ctx
 * is there for the less common cases: several slave instances sharing one build, or
 * a driver that must be handed its own context.) */
static struct {
    const uint8_t *rx;       /* scripted bytes the "master" sent */
    size_t         rx_len;
    size_t         rx_pos;
    uint8_t        tx[512];  /* everything the slave transmitted, concatenated */
    size_t         tx_len;
    uint32_t       clock_us; /* mock free-running microsecond counter (get_us reads it) */
} g_io;

static size_t mock_read(void *ctx, uint8_t *dst, size_t max)
{
    (void)ctx;               /* single module-owned endpoint: no context needed */
    size_t avail = g_io.rx_len - g_io.rx_pos;
    size_t n = (avail < max) ? avail : max;
    memcpy(dst, g_io.rx + g_io.rx_pos, n);
    g_io.rx_pos += n;
    return n;
}

static bool mock_write(void *ctx, const uint8_t *src, size_t len)
{
    (void)ctx;
    if (g_io.tx_len + len > sizeof(g_io.tx)) {
        return false;
    }
    memcpy(g_io.tx + g_io.tx_len, src, len);
    g_io.tx_len += len;
    return true;   /* blocking write: complete on return, so is_busy is NULL */
}

/* Mock microsecond clock; the drive loop advances g_io.clock_us between calls.
 * A single system-wide time source takes no context. */
static uint32_t mock_get_us(void)
{
    return g_io.clock_us;
}

/* ------------------------------------------------------------------ */
/* Request frame builder (fixed-layout 01..06)                        */
/* ------------------------------------------------------------------ */
static size_t build_fixed_req(uint8_t *buf, uint8_t addr, uint8_t cmd,
                              uint16_t a, uint16_t q)
{
    buf[0] = addr;
    buf[1] = cmd;
    buf[2] = (uint8_t)(a >> 8);
    buf[3] = (uint8_t)(a & 0xFFu);
    buf[4] = (uint8_t)(q >> 8);
    buf[5] = (uint8_t)(q & 0xFFu);
    nx_modbus_rtu_set_crc(buf, 8u);   /* fills buf[6..7] */
    return 8u;
}

/* ------------------------------------------------------------------ */
/* A "business module": drain its inbox and answer each request       */
/* ------------------------------------------------------------------ */
/* Each module only ever gets requests for the ranges it subscribed to, and the slave
 * has already settled structural legality, so it can trust func/addr/quantity and
 * focus on meaning. All three answers go out through the reply_* helpers, which need
 * only the pool and the response queue - no slave handle. Their return value names the
 * reason a reply was not queued, which is what a real module would log. */
static void business_serve(const char *name, nx_queue_t *inbox,
                           nx_queue_t *response_queue, nx_tiered_mem_pool_t *pool)
{
    nx_ref_msg_t *req = NULL;
    while (nx_queue_pop(inbox, &req) == NX_QUEUE_OK) {
        /* Every dispatched request is at least a fixed-layout frame, and the frame
         * structs map 1:1 onto the wire bytes - so parse it in place by casting. */
        const nx_modbus_rtu_req_fix_t *q = (const nx_modbus_rtu_req_fix_t *)nx_ref_msg_data(req);
        uint8_t  cmd   = q->cmd;
        uint16_t start = (uint16_t)((q->addr_h << 8) | q->addr_l);
        uint16_t qty   = (uint16_t)((q->qty_h  << 8) | q->qty_l);
        printf("  [%s] request func=0x%02X start=0x%04X qty=%u\n", name, cmd, start, qty);

        nx_modbus_rtu_slave_ret_t ret;

        if (cmd == NX_MODBUS_FC_WRITE_SINGLE_REG) {
            /* Semantic range check: a valve position is a percentage. The frame is
             * well-formed either way - only this module knows 4000 is meaningless. */
            if (qty > 100u) {
                printf("  [%s] value %u out of range -> exception 0x%02X\n",
                       name, qty, NX_MODBUS_EXC_ILLEGAL_DATA_VALUE);
                ret = nx_modbus_rtu_slave_reply_exception(pool, response_queue,
                                                          (const nx_modbus_rtu_header_t *)q,
                                                          NX_MODBUS_EXC_ILLEGAL_DATA_VALUE);
            } else {
                /* Accepted: the write confirmation is the request echoed back. */
                ret = nx_modbus_rtu_slave_reply_write(pool, response_queue, q);
            }
        } else {
            /* A read: gather the values (dummy here - each register echoes its address)
             * and hand them over; the helper wraps them in a response frame. */
            uint8_t data[32];
            assert(qty * 2u <= sizeof(data));
            for (uint16_t i = 0; i < qty; i++) {
                uint16_t val = (uint16_t)(start + i);
                data[i * 2]     = (uint8_t)(val >> 8);
                data[i * 2 + 1] = (uint8_t)(val & 0xFFu);
            }
            ret = nx_modbus_rtu_slave_reply_read(pool, response_queue,
                                                 (const nx_modbus_rtu_header_t *)q,
                                                 data, (size_t)qty * 2u);
        }

        /* A real module would log the resource shortages (NOMEM / FULL) here; every
         * reply in this example is expected to make it onto the queue. */
        assert(ret == NX_MODBUS_RTU_SLAVE_OK);
        (void)ret;

        nx_ref_msg_release(req);      /* done with the request */
    }
}

/* ------------------------------------------------------------------ */
/* Decode/print whatever the slave put on the wire                    */
/* ------------------------------------------------------------------ */
static void print_tx(void)
{
    size_t i = 0;
    int    n = 0;
    while (i + 5u <= g_io.tx_len) {         /* smallest frame (exception) is 5 bytes */
        const uint8_t *f   = g_io.tx + i;
        uint8_t        cmd = f[1];
        size_t         len;

        if (cmd & NX_MODBUS_RTU_EXCEPTION_FLAG) {
            len = 5u;                        /* addr,cmd,exc,crc,crc */
            printf("  frame %d: EXCEPTION func=0x%02X code=0x%02X\n",
                   ++n, (unsigned)(cmd & 0x7Fu), f[2]);
        } else if (cmd == NX_MODBUS_FC_WRITE_SINGLE_COIL ||
                   cmd == NX_MODBUS_FC_WRITE_SINGLE_REG ||
                   cmd == NX_MODBUS_FC_WRITE_MULTIPLE_COILS ||
                   cmd == NX_MODBUS_FC_WRITE_MULTIPLE_REGS) {
            len = 8u;                        /* write confirmation: echoes the request */
            printf("  frame %d: WRITE OK  func=0x%02X addr=0x%04X value=%u\n",
                   ++n, cmd, (unsigned)((f[2] << 8) | f[3]),
                   (unsigned)((f[4] << 8) | f[5]));
        } else {
            len = 3u + f[2] + 2u;            /* read response: header + data + crc */
            printf("  frame %d: RESPONSE func=0x%02X byte_count=%u\n",
                   ++n, cmd, f[2]);
        }
        if (i + len > g_io.tx_len) {
            break;
        }
        i += len;
    }
}

/* ------------------------------------------------------------------ */
/* Static storage                                                     */
/* ------------------------------------------------------------------ */
#define POOL_BLK  64
#define POOL_NBLK 16
/* +256: headroom for the pool's own metadata (tier table + bitmaps), which the
 * pool carves from this same buffer alongside the blocks. */
static _Alignas(max_align_t) uint8_t g_pool_mem[POOL_BLK * POOL_NBLK + 256];

static nx_ref_msg_t *g_respq_buf[8];
static nx_ref_msg_t *g_valveq_buf[4];
static nx_ref_msg_t *g_sysq_buf[4];

static uint8_t g_rx_buf[256];

int nx_modbus_rtu_slave_example_run(void)
{
    printf("########## nx_modbus_rtu_slave examples ##########\n");

    /* ---- pool + queues ---- */
    nx_tiered_mem_pool_t pool;
    static const nx_tiered_level_cfg_t pool_tiers[] = { { POOL_BLK, POOL_NBLK } };
    nx_tiered_mem_pool_cfg_t pool_cfg = {
        .memory      = g_pool_mem,
        .memory_size = sizeof(g_pool_mem),
        .tiers       = pool_tiers,
        .tier_count  = 1,
    };
    if (nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL) != NX_TIERED_OK) {
        printf("pool init failed\n");
        return 1;
    }

    nx_queue_t response_queue, q_valve, q_sys;
    nx_ref_msg_queue_init(&response_queue, g_respq_buf,  8);
    nx_ref_msg_queue_init(&q_valve,  g_valveq_buf, 4);
    nx_ref_msg_queue_init(&q_sys,    g_sysq_buf,   4);

    /* ---- subscription table: two business modules, different ranges ----
     * Each entry: which function code + inclusive address range it owns, and the
     * queue that receives matching requests. A single func (e.g. reads) can be
     * split across modules purely by range. */
    const nx_modbus_rtu_slave_sub_t sub_table[] = {
        { NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 0x000F, &q_valve },
        { NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0000, 0x000F, &q_valve },
        { NX_MODBUS_FC_READ_INPUT_REGS,   0x8000, 0x8007, &q_sys   },
    };

    /* ---- slave ---- */
    memset(&g_io, 0, sizeof(g_io));   /* fresh endpoint state */
    nx_modbus_rtu_slave_t slave;
    nx_modbus_rtu_slave_cfg_t cfg = {
        .slave_addr     = SLAVE_ADDR,
        .baud_rate      = 115200,   /* derives the ~335us TX inter-frame gap */
        .accept_broadcast = false,  /* default: address 0 is dropped, unicast only */
        .pool           = &pool,
        .rx_buf         = g_rx_buf,
        .rx_size        = sizeof(g_rx_buf),
        .subs           = sub_table,
        .subs_count     = sizeof(sub_table) / sizeof(sub_table[0]),
        .response_queue = &response_queue,
        .read           = mock_read,
        .write          = mock_write,
        .is_busy        = NULL,     /* blocking write: never busy afterward */
        .dir_tx         = NULL,     /* no DE pin in this mock */
        .get_us         = mock_get_us,
        .io_ctx         = NULL,     /* single module-owned endpoint; see g_io */
        .dir_ctx        = NULL,     /* no DE pin in this mock */
    };
    if (!nx_modbus_rtu_slave_init(&slave, &cfg)) {
        printf("slave init failed\n");
        return 1;
    }

    /* ---- scripted master traffic: 8 back-to-back requests ---- */
    uint8_t stream[96];   /* 8 requests x 8 bytes, with slack */
    size_t  sn = 0;
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 2);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_INPUT_REGS,   0x8000, 4);
    /* A broadcast write that a subscription *would* own: accept_broadcast is false,
     * so it is dropped at the address check - no dispatch, no response, no exception. */
    sn += build_fixed_req(stream + sn, NX_MODBUS_RTU_ADDR_BROADCAST,
                          NX_MODBUS_FC_WRITE_SINGLE_REG, 0x0001, 0x0000);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_COILS,        0x0000, 8);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_HOLDING_REGS, 0x0020, 1);
    /* Structurally fine (any 16-bit value is legal for func 0x06), so the slave
     * dispatches it; only the valve module knows 4000 is not a valid position. */
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0002, 4000);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0002, 60);
    /* A span that runs off the end of the address space: registers 0xFFFF and 0x10000.
     * The quantity is structurally legal (1..125), so it reaches the containment check,
     * where the end address must stay 0x10000 and not wrap to 0x0000 - which would land
     * inside the valve's 0x0000..0x000F range and be dispatched to a module that owns
     * neither register. */
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_HOLDING_REGS, 0xFFFF, 2);
    g_io.rx     = stream;
    g_io.rx_len = sn;

    /* ---- drive it like a main loop ----
     * Each iteration: process() pulls bytes, dispatches new requests, and pumps one
     * step of the TX state machine; then each business module answers whatever
     * landed in its inbox. Requests, exceptions and responses all flow through
     * naturally - there is no artificial "receive then transmit" split. */
    printf("Driving the slave; frames it puts on the wire:\n");
    for (int i = 0; i < 60; i++) {
        g_io.clock_us += 1000;   /* advance the mock clock past the ~335us gap */
        nx_modbus_rtu_slave_process(&slave);
        business_serve("valve", &q_valve, &response_queue, &pool);
        business_serve("sys",   &q_sys,   &response_queue, &pool);
        if (g_io.rx_pos == g_io.rx_len && nx_queue_is_empty(&response_queue) &&
            nx_queue_is_empty(&q_valve) && nx_queue_is_empty(&q_sys)) {
            /* a few more iterations let the final frame finish SENDING/GAP */
            for (int k = 0; k < 3; k++) {
                g_io.clock_us += 1000;
                nx_modbus_rtu_slave_process(&slave);
            }
            break;
        }
    }
    printf("\n");

    /* ---- verify what actually went on the wire ---- */
    printf("Wire trace:\n");
    print_tx();

    /* Walk every emitted frame: correct count, order, and a valid CRC each. */
    struct { uint8_t cmd; uint8_t info; size_t len; } expect[] = {
        { NX_MODBUS_FC_READ_COILS        | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_FUNCTION,   5 },
        { NX_MODBUS_FC_READ_HOLDING_REGS | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_DATA_ADDR,  5 },
        { NX_MODBUS_FC_READ_HOLDING_REGS | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_DATA_ADDR,  5 },   /* req 8: span crossed 0xFFFF */
        { NX_MODBUS_FC_READ_HOLDING_REGS,                               4 /* byte_count */,              3 + 4 + 2 },
        { NX_MODBUS_FC_WRITE_SINGLE_REG  | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_DATA_VALUE, 5 },
        { NX_MODBUS_FC_WRITE_SINGLE_REG,                                0x00 /* addr_h */,               8 },
        { NX_MODBUS_FC_READ_INPUT_REGS,                                 8 /* byte_count */,              3 + 8 + 2 },
    };
    size_t off = 0;
    size_t at[sizeof(expect) / sizeof(expect[0])];   /* where each frame started */
    for (size_t k = 0; k < sizeof(expect) / sizeof(expect[0]); k++) {
        const uint8_t *f = g_io.tx + off;
        at[k] = off;
        assert(off + expect[k].len <= g_io.tx_len);
        assert(f[1] == expect[k].cmd);
        assert(f[2] == expect[k].info);                       /* exc code or byte_count */
        assert(nx_modbus_rtu_check_crc(f, expect[k].len));    /* CRC valid */
        off += expect[k].len;
    }
    assert(off == g_io.tx_len);   /* nothing extra, nothing missing */
    printf("  OK: 4 exceptions + 2 data responses + 1 write confirmation,\n"
           "      in order, every CRC valid\n");
    /* The write confirmation must echo the accepted request byte for byte: same data
     * address (0x0002) and the same value (60) the master asked to write. */
    const uint8_t *wr = g_io.tx + at[5];   /* the write confirmation */
    assert(((wr[2] << 8) | wr[3]) == 0x0002);
    assert(((wr[4] << 8) | wr[5]) == 60);
    printf("  OK: the write confirmation echoes addr=0x0002 value=60\n");
    /* The last frame answers the span that runs past 0xFFFF. It exists only because the
     * end address is computed wide: truncated to 16 bits it would have been 0x0000,
     * landing inside the valve's range, and the request would have been dispatched to a
     * module owning neither register instead of refused. */
    const uint8_t *ovf = g_io.tx + at[2];   /* the answer to request 8 */
    assert(ovf[1] == (NX_MODBUS_FC_READ_HOLDING_REGS | NX_MODBUS_RTU_EXCEPTION_FLAG));
    assert(ovf[2] == NX_MODBUS_EXC_ILLEGAL_DATA_ADDR);
    printf("  OK: the span crossing 0xFFFF drew exception 0x%02X, not a dispatch\n",
           NX_MODBUS_EXC_ILLEGAL_DATA_ADDR);
    /* The broadcast frame produced none of the above: had it been accepted it would
     * have reached the valve queue, and had it been mistaken for unicast it would
     * have drawn a response. The exact-length match above proves neither happened. */
    printf("  OK: the broadcast frame was dropped silently (accept_broadcast = false)\n");

    return 0;
}
