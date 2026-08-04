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
 * Four requests are fed in to exercise every path:
 *   1. read holding regs @0x0000 x2   -> valve
 *   2. read input regs   @0x8000 x4   -> sys
 *   3. read coils        @0x0000 x8   -> no subscriber for func 0x01 -> exc 0x01
 *   4. read holding regs @0x0020 x1   -> func owned, addr out of range -> exc 0x02
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

/* Mock microsecond clock; the drive loop advances g_io.clock_us between calls. */
static uint32_t mock_get_us(void *ctx)
{
    (void)ctx;
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
/* A "business module": drain its inbox, answer each read with data   */
/* ------------------------------------------------------------------ */
/* Each module only ever gets requests for the ranges it subscribed to, so it can
 * trust func/addr and focus on producing data. It builds a read response and
 * pushes it onto the shared response queue; the slave transmits it. */
static void business_answer_reads(const char *name, nx_queue_t *inbox,
                                  nx_queue_t *response_queue, nx_tiered_mem_pool_t *pool)
{
    nx_ref_msg_t *req = NULL;
    while (nx_queue_pop(inbox, &req) == NX_QUEUE_OK) {
        const uint8_t *f = (const uint8_t *)nx_ref_msg_data(req);
        uint8_t  cmd   = f[1];
        uint16_t start = (uint16_t)((f[2] << 8) | f[3]);
        uint16_t qty   = (uint16_t)((f[4] << 8) | f[5]);
        printf("  [%s] request func=0x%02X start=0x%04X qty=%u\n", name, cmd, start, qty);

        /* Build a read response: addr, cmd, byte_count, data[qty*2], crc. */
        uint8_t  byte_count = (uint8_t)(qty * 2u);
        size_t   rsp_len    = 3u + byte_count + 2u;   /* header(3) + data + crc(2) */
        nx_ref_msg_t *rsp   = nx_ref_msg_alloc(pool, rsp_len);
        assert(rsp != NULL);
        nx_modbus_rtu_rsp_var_t *r = (nx_modbus_rtu_rsp_var_t *)nx_ref_msg_data(rsp);
        r->addr       = SLAVE_ADDR;
        r->cmd        = cmd;
        r->byte_count = byte_count;
        for (uint16_t i = 0; i < qty; i++) {
            uint16_t val = (uint16_t)(start + i);      /* dummy: echo the address */
            r->payload[i * 2]     = (uint8_t)(val >> 8);
            r->payload[i * 2 + 1] = (uint8_t)(val & 0xFFu);
        }
        nx_modbus_rtu_set_crc((uint8_t *)r, rsp_len);

        nx_ref_msg_publish(rsp, response_queue);
        nx_ref_msg_release(rsp);      /* producer done; response_queue holds the reference */

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
static _Alignas(max_align_t) uint8_t g_pool_mem[POOL_BLK * POOL_NBLK];

static nx_ref_msg_t *g_respq_buf[8];
static nx_ref_msg_t *g_valveq_buf[4];
static nx_ref_msg_t *g_sysq_buf[4];

static uint8_t g_rx_buf[256];

int nx_modbus_rtu_slave_example_run(void)
{
    printf("########## nx_modbus_rtu_slave examples ##########\n");

    /* ---- pool + queues ---- */
    nx_tiered_mem_pool_t pool;
    nx_tiered_mem_pool_cfg_t pool_cfg = {
        .memory      = g_pool_mem,
        .memory_size = sizeof(g_pool_mem),
        .tiers       = { { POOL_BLK, POOL_NBLK } },
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
    };
    if (!nx_modbus_rtu_slave_init(&slave, &cfg)) {
        printf("slave init failed\n");
        return 1;
    }

    /* ---- scripted master traffic: 4 back-to-back requests ---- */
    uint8_t stream[64];
    size_t  sn = 0;
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 2);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_INPUT_REGS,   0x8000, 4);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_COILS,        0x0000, 8);
    sn += build_fixed_req(stream + sn, SLAVE_ADDR, NX_MODBUS_FC_READ_HOLDING_REGS, 0x0020, 1);
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
        business_answer_reads("valve", &q_valve, &response_queue, &pool);
        business_answer_reads("sys",   &q_sys,   &response_queue, &pool);
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
        { NX_MODBUS_FC_READ_COILS        | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_FUNCTION,  5 },
        { NX_MODBUS_FC_READ_HOLDING_REGS | NX_MODBUS_RTU_EXCEPTION_FLAG, NX_MODBUS_EXC_ILLEGAL_DATA_ADDR, 5 },
        { NX_MODBUS_FC_READ_HOLDING_REGS,                               4 /* byte_count */,             3 + 4 + 2 },
        { NX_MODBUS_FC_READ_INPUT_REGS,                                 8 /* byte_count */,             3 + 8 + 2 },
    };
    size_t off = 0;
    for (size_t k = 0; k < sizeof(expect) / sizeof(expect[0]); k++) {
        const uint8_t *f = g_io.tx + off;
        assert(off + expect[k].len <= g_io.tx_len);
        assert(f[1] == expect[k].cmd);
        assert(f[2] == expect[k].info);                       /* exc code or byte_count */
        assert(nx_modbus_rtu_check_crc(f, expect[k].len));    /* CRC valid */
        off += expect[k].len;
    }
    assert(off == g_io.tx_len);   /* nothing extra, nothing missing */
    printf("  OK: 2 exceptions + 2 data responses, in order, every CRC valid\n");

    return 0;
}
