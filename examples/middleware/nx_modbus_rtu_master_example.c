/**
 * @file    nx_modbus_rtu_master_example.c
 * @brief   Usage example for nx_modbus_rtu_master: queue -> wire -> subscription dispatch.
 *
 * Shows the intended architecture end to end, with no real hardware:
 *
 *   q_pump (a business) ----+                              +----> q_pump  (its responses)
 *        | request_* helper |                              |
 *   q_meter (a business) ---+--> request_queue --[master]--+----> q_meter (its responses)
 *                                     |            |
 *                                  write()      read()
 *                                     v            ^
 *                                 mock RS-485 endpoint (a scripted "bus")
 *
 * The two "business modules" here (pump, meter) each own one slave device: each
 * subscribes to the address it owns and receives that device's responses as data on its
 * own queue. Nothing tracks timeouts - a module that cares notes when it sent and
 * decides for itself, which is why none of the checks below involve a deadline.
 *
 * The example self-checks with asserts and prints the wire.
 */
#include "nx_middleware_examples.h"
#include "src/middleware/nx_modbus_rtu_master.h"
#include "src/middleware/nx_modbus_rtu_slave.h"   /* peer for the loopback test */

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The two slave addresses on the bus, one per business module. */
#define ADDR_PUMP  0x11u
#define ADDR_METER 0x22u

/* ------------------------------------------------------------------ */
/* Mock RS-485 endpoint: a captured TX buffer and a scripted RX stream */
/* ------------------------------------------------------------------ */
/* A typical application product owns a single bus and driver, so the endpoint is just
 * module-level state the callbacks reach directly - io_ctx stays NULL. */
static struct {
    const uint8_t *rx;       /* scripted bytes the "slaves" answered with */
    size_t         rx_len;
    size_t         rx_pos;
    uint8_t        tx[512];  /* everything the master transmitted, concatenated */
    size_t         tx_len;
    uint32_t       clock_us; /* mock free-running microsecond counter */
} g_io;

static size_t mock_read(void *ctx, uint8_t *dst, size_t max)
{
    (void)ctx;
    if (g_io.rx == NULL) {
        return 0u;               /* nothing scripted: an idle bus */
    }
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

/* A transmitter that never accepts anything: stands in for a UART whose TX path is
 * unavailable (DMA already armed, peripheral in an error state). */
static bool mock_write_fail(void *ctx, const uint8_t *src, size_t len)
{
    (void)ctx; (void)src; (void)len;
    return false;
}

/* A non-blocking transmitter: accepts the bytes, then reports busy until the test says
 * the frame has left the wire. Stands in for an interrupt- or DMA-driven UART, where
 * write() only starts the transfer. */
static bool     g_tx_inflight;
static unsigned g_dir_raise_count;

static bool mock_write_nb(void *ctx, const uint8_t *src, size_t len)
{
    (void)ctx;
    if (g_io.tx_len + len > sizeof(g_io.tx)) {
        return false;
    }
    memcpy(g_io.tx + g_io.tx_len, src, len);
    g_io.tx_len += len;
    g_tx_inflight = true;      /* still on the wire until the test clears it */
    return true;
}

static bool mock_is_busy(void *ctx)
{
    (void)ctx;
    return g_tx_inflight;
}

/* RS-485 direction pin: records the level so a test can check the bus was released. */
static bool g_de_asserted;

static void mock_dir_tx(void *ctx, bool enable)
{
    (void)ctx;
    g_de_asserted = enable;
}

/* Counts the rising edges, so a test can tell how many frames were started. */
static void mock_dir_tx_counting(void *ctx, bool enable)
{
    (void)ctx;
    if (enable) {
        g_dir_raise_count++;
    }
    g_de_asserted = enable;
}

/* Mock microsecond clock. A single system-wide time source takes no context. */
static uint32_t mock_get_us(void)
{
    return g_io.clock_us;
}

/* Free blocks in the smallest tier, for the leak checks below. */
static size_t after_free_probe(const nx_tiered_mem_pool_t *pool)
{
    nx_tiered_level_stat_t st;
    assert(nx_tiered_mem_pool_get_tier_stat(pool, 0, &st) == NX_TIERED_OK);
    return st.free_count;
}

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                   */
/* ------------------------------------------------------------------ */

/* Drive the master until it has nothing left to do: TX drained and RX consumed. A real
 * application just calls process() from its main loop; this bounded loop stands in for
 * "some number of iterations later", advancing the mock clock so the inter-frame gap
 * expires like it would on a real bus. */
static void pump_master(nx_modbus_rtu_master_t *m, unsigned iterations)
{
    for (unsigned i = 0; i < iterations; i++) {
        nx_modbus_rtu_master_process(m);
        g_io.clock_us += 500u;    /* > the ~335us gap at 115200 baud */
    }
}

/* Build the 8-byte fixed request the master is expected to have produced, so a test can
 * compare the captured wire bytes against an independently built frame. */
static size_t expect_fixed(uint8_t *out, uint8_t addr, uint8_t cmd, uint16_t a, uint16_t b)
{
    out[0] = addr;
    out[1] = cmd;
    out[2] = (uint8_t)((a) >> 8);
    out[3] = (uint8_t)((a) & 0xFFu);
    out[4] = (uint8_t)((b) >> 8);
    out[5] = (uint8_t)((b) & 0xFFu);
    nx_modbus_rtu_set_crc(out, 8u);
    return 8u;
}

/* Build a read response (01..04) as a slave would answer it. */
static size_t build_read_rsp(uint8_t *out, uint8_t addr, uint8_t cmd,
                             const uint8_t *data, uint8_t bc)
{
    out[0] = addr;
    out[1] = cmd;
    out[2] = bc;
    memcpy(&out[3], data, bc);
    nx_modbus_rtu_set_crc(out, (size_t)bc + 5u);
    return (size_t)bc + 5u;
}

/* Build a write confirmation (05/06/0F/10), which echoes the request's address+value. */
static size_t build_write_rsp(uint8_t *out, uint8_t addr, uint8_t cmd, uint16_t a, uint16_t b)
{
    out[0] = addr;
    out[1] = cmd;
    out[2] = (uint8_t)((a) >> 8);
    out[3] = (uint8_t)((a) & 0xFFu);
    out[4] = (uint8_t)((b) >> 8);
    out[5] = (uint8_t)((b) & 0xFFu);
    nx_modbus_rtu_set_crc(out, 8u);
    return 8u;
}

/* Build an exception response: the original code with 0x80 set, plus a code byte. */
static size_t build_exc_rsp(uint8_t *out, uint8_t addr, uint8_t cmd, uint8_t exc)
{
    out[0] = addr;
    out[1] = (uint8_t)(cmd | NX_MODBUS_RTU_EXCEPTION_FLAG);
    out[2] = exc;
    nx_modbus_rtu_set_crc(out, 5u);
    return 5u;
}

/* Pop one response off a subscriber queue, or NULL if it is empty. The caller owns the
 * reference and must release it - exactly what a business module does. */
static nx_ref_msg_t *pop_rsp(nx_queue_t *q)
{
    nx_ref_msg_t *msg = NULL;
    if (nx_queue_pop(q, &msg) != NX_QUEUE_OK) {
        return NULL;
    }
    return msg;
}

/* ------------------------------------------------------------------ */
/* Loopback endpoint: two one-way byte pipes between master and slave  */
/* ------------------------------------------------------------------ */
/* Each pipe is a byte FIFO: what one side writes, the other side reads. */
static struct {
    uint8_t buf[512];
    size_t  len;
    size_t  pos;
} g_m2s, g_s2m;   /* master->slave, slave->master */

static size_t pipe_read(uint8_t *dst, size_t max, void *pipe)
{
    struct { uint8_t buf[512]; size_t len; size_t pos; } *pp = pipe;
    size_t avail = pp->len - pp->pos;
    size_t n = (avail < max) ? avail : max;
    memcpy(dst, pp->buf + pp->pos, n);
    pp->pos += n;
    return n;
}

static bool pipe_write(const uint8_t *src, size_t len, void *pipe)
{
    struct { uint8_t buf[512]; size_t len; size_t pos; } *pp = pipe;
    if (pp->len + len > sizeof(pp->buf)) {
        return false;
    }
    memcpy(pp->buf + pp->len, src, len);
    pp->len += len;
    return true;
}

/* The master's end: reads what the slave sent, writes toward the slave. */
static size_t lb_master_read(void *ctx, uint8_t *dst, size_t max)
{
    (void)ctx; return pipe_read(dst, max, &g_s2m);
}
static bool lb_master_write(void *ctx, const uint8_t *src, size_t len)
{
    (void)ctx; return pipe_write(src, len, &g_m2s);
}

/* The slave's end: the mirror image. */
static size_t lb_slave_read(void *ctx, uint8_t *dst, size_t max)
{
    (void)ctx; return pipe_read(dst, max, &g_m2s);
}
static bool lb_slave_write(void *ctx, const uint8_t *src, size_t len)
{
    (void)ctx; return pipe_write(src, len, &g_s2m);
}

/* ------------------------------------------------------------------ */
/* Static storage                                                     */
/* ------------------------------------------------------------------ */
#define POOL_BLK  64
#define POOL_NBLK 16
/* +256: headroom for the pool's own metadata (tier table + bitmaps), which the pool
 * carves from this same buffer alongside the blocks. */
static _Alignas(max_align_t) uint8_t g_pool_mem[POOL_BLK * POOL_NBLK + 256];

static nx_ref_msg_t *g_reqq_buf[8];
static nx_ref_msg_t *g_pumpq_buf[4];
static nx_ref_msg_t *g_meterq_buf[4];

static uint8_t g_rx_buf[256];

int nx_modbus_rtu_master_example_run(void)
{
    printf("########## nx_modbus_rtu_master examples ##########\n");

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

    nx_queue_t request_queue, q_pump, q_meter;
    nx_ref_msg_queue_init(&request_queue, g_reqq_buf,   8);
    nx_ref_msg_queue_init(&q_pump,        g_pumpq_buf,  4);
    nx_ref_msg_queue_init(&q_meter,       g_meterq_buf, 4);

    /* ---- subscription table: two business modules, one device each ----
     * Each entry: the slave address it owns, an optional function-code filter (0 = any
     * code from that address), and the queue that receives matching responses. */
    const nx_modbus_rtu_master_sub_t sub_table[] = {
        { ADDR_PUMP,  0u, &q_pump  },   /* every response from the pump */
        { ADDR_METER, 0u, &q_meter },   /* every response from the meter */
    };

    /* ---- master ---- */
    memset(&g_io, 0, sizeof(g_io));
    nx_modbus_rtu_master_t m;
    nx_modbus_rtu_master_cfg_t cfg = {
        .baud_rate     = 115200,          /* derives the ~335us TX inter-frame gap */
        .pool          = &pool,
        .rx_buf        = g_rx_buf,
        .rx_size       = sizeof(g_rx_buf),
        .subs          = sub_table,
        .subs_count    = 2u,
        .request_queue = &request_queue,
        .read          = mock_read,
        .write         = mock_write,
        .get_us        = mock_get_us,     /* is_busy NULL => write is blocking */
    };
    assert(nx_modbus_rtu_master_init(&m, &cfg));

    /* ---- 1. a request helper builds the frame and the master puts it on the wire ---- */
    {
        assert(nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue,
                                                      ADDR_PUMP, 0x0000u, 2u)
               == NX_MODBUS_RTU_MASTER_OK);

        pump_master(&m, 4);

        uint8_t want[8];
        size_t  want_len = expect_fixed(want, ADDR_PUMP, NX_MODBUS_FC_READ_HOLDING_REGS,
                                        0x0000u, 2u);
        assert(g_io.tx_len == want_len);
        assert(memcmp(g_io.tx, want, want_len) == 0);

        printf("  OK: read-holding-regs request reached the wire byte-for-byte\n");
    }

    /* ---- 2. the answer comes back and lands on the owning module's queue ---- */
    {
        static uint8_t script[16];
        const uint8_t  regs[4] = { 0x00, 0x2A, 0x01, 0x00 };   /* 42, 256 */
        size_t         n = build_read_rsp(script, ADDR_PUMP,
                                         NX_MODBUS_FC_READ_HOLDING_REGS, regs, 4u);
        g_io.rx     = script;
        g_io.rx_len = n;
        g_io.rx_pos = 0u;

        pump_master(&m, 4);

        nx_ref_msg_t *msg = pop_rsp(&q_pump);
        assert(msg != NULL);                          /* the pump's own response */
        assert(pop_rsp(&q_meter) == NULL);            /* not the meter's */

        size_t         dlen = 0u;
        const uint8_t *data = nx_modbus_rtu_master_rsp_data(nx_ref_msg_data(msg),
                                                            nx_ref_msg_len(msg), &dlen);
        assert(data != NULL && dlen == 4u);
        assert((uint16_t)(((uint16_t)data[0] << 8) | data[1]) == 42u);
        assert((uint16_t)(((uint16_t)data[2] << 8) | data[3]) == 256u);
        assert(!nx_modbus_rtu_master_rsp_is_exception(nx_ref_msg_data(msg),
                                                      nx_ref_msg_len(msg), NULL));
        nx_ref_msg_release(msg);

        printf("  OK: read response dispatched to its owner, payload 42/256 readable\n");
    }

    /* ---- 3. a write confirmation is dispatched the same way ---- */
    {
        assert(nx_modbus_rtu_master_write_single_reg(&pool, &request_queue,
                                                     ADDR_PUMP, 0x0002u, 60u)
               == NX_MODBUS_RTU_MASTER_OK);
        g_io.tx_len = 0u;
        pump_master(&m, 4);

        uint8_t want[8];
        expect_fixed(want, ADDR_PUMP, NX_MODBUS_FC_WRITE_SINGLE_REG, 0x0002u, 60u);
        assert(g_io.tx_len == 8u && memcmp(g_io.tx, want, 8u) == 0);

        static uint8_t script[8];
        size_t n = build_write_rsp(script, ADDR_PUMP, NX_MODBUS_FC_WRITE_SINGLE_REG,
                                   0x0002u, 60u);
        g_io.rx = script; g_io.rx_len = n; g_io.rx_pos = 0u;
        pump_master(&m, 4);

        nx_ref_msg_t *msg = pop_rsp(&q_pump);
        assert(msg != NULL);
        /* A write confirmation carries no read payload. */
        assert(nx_modbus_rtu_master_rsp_data(nx_ref_msg_data(msg),
                                             nx_ref_msg_len(msg), NULL) == NULL);
        nx_ref_msg_release(msg);

        printf("  OK: write confirmation dispatched; it carries no read payload\n");
    }

    /* ---- 4. an exception response reaches the module that asked ----
     * A refusal must be delivered, not swallowed: the module that sent the request is
     * the one that needs to hear it was rejected. */
    {
        static uint8_t script[5];
        size_t n = build_exc_rsp(script, ADDR_METER, NX_MODBUS_FC_READ_INPUT_REGS,
                                 NX_MODBUS_EXC_ILLEGAL_DATA_ADDR);
        g_io.rx = script; g_io.rx_len = n; g_io.rx_pos = 0u;
        pump_master(&m, 4);

        nx_ref_msg_t *msg = pop_rsp(&q_meter);
        assert(msg != NULL);

        uint8_t exc = 0u;
        assert(nx_modbus_rtu_master_rsp_is_exception(nx_ref_msg_data(msg),
                                                     nx_ref_msg_len(msg), &exc));
        assert(exc == NX_MODBUS_EXC_ILLEGAL_DATA_ADDR);
        /* An exception is not a read response, however much its code resembles one. */
        assert(nx_modbus_rtu_master_rsp_data(nx_ref_msg_data(msg),
                                             nx_ref_msg_len(msg), NULL) == NULL);
        nx_ref_msg_release(msg);

        printf("  OK: exception 0x02 delivered to the module that asked\n");
    }

    /* ---- 5. a response from an unsubscribed address is discarded ----
     * A master answers nothing, so an unclaimed frame leaves no trace: no queue entry,
     * and no pool block held. */
    {
        size_t before = after_free_probe(&pool);

        static uint8_t script[8];
        size_t n = build_write_rsp(script, 0x33u /* nobody owns this */,
                                   NX_MODBUS_FC_WRITE_SINGLE_REG, 0x0001u, 1u);
        g_io.rx = script; g_io.rx_len = n; g_io.rx_pos = 0u;
        pump_master(&m, 4);

        assert(pop_rsp(&q_pump) == NULL);
        assert(pop_rsp(&q_meter) == NULL);
        assert(after_free_probe(&pool) == before);   /* nothing was allocated */

        printf("  OK: response from an unsubscribed address discarded silently\n");
    }

    /* ---- 6. a bad CRC is resynchronized past, and the good frame behind it survives ----
     * Framing is by length, so a corrupt frame costs one dropped byte and the stream
     * keeps making progress rather than stalling. */
    {
        static uint8_t script[16];
        size_t n = build_write_rsp(script, ADDR_PUMP, NX_MODBUS_FC_WRITE_SINGLE_REG,
                                   0x0002u, 60u);
        script[7] ^= 0xFFu;              /* corrupt the CRC high byte */
        size_t n2 = build_write_rsp(script + n, ADDR_PUMP,
                                    NX_MODBUS_FC_WRITE_SINGLE_REG, 0x0003u, 61u);
        g_io.rx = script; g_io.rx_len = n + n2; g_io.rx_pos = 0u;
        pump_master(&m, 8);

        /* Exactly one response survived: the intact second frame. */
        nx_ref_msg_t *msg = pop_rsp(&q_pump);
        assert(msg != NULL);
        const uint8_t *f = (const uint8_t *)nx_ref_msg_data(msg);
        assert((uint16_t)(((uint16_t)f[2] << 8) | f[3]) == 0x0003u);
        nx_ref_msg_release(msg);
        assert(pop_rsp(&q_pump) == NULL);

        printf("  OK: bad CRC resynced past; the frame behind it was still delivered\n");
    }

    /* ---- 7. one response fans out to every subscription that claims it ----
     * Two modules can share a device by splitting it by function code; a third claim
     * with no filter hears everything from it. The frame is copied once and published
     * to each, so all three see the same bytes. */
    {
        nx_queue_t q_a, q_b, q_all;
        static nx_ref_msg_t *qa_buf[4], *qb_buf[4], *qall_buf[4];
        nx_ref_msg_queue_init(&q_a,   qa_buf,   4);
        nx_ref_msg_queue_init(&q_b,   qb_buf,   4);
        nx_ref_msg_queue_init(&q_all, qall_buf, 4);

        const nx_modbus_rtu_master_sub_t shared[] = {
            { ADDR_PUMP, NX_MODBUS_FC_READ_HOLDING_REGS, &q_a   },
            { ADDR_PUMP, NX_MODBUS_FC_READ_INPUT_REGS,   &q_b   },
            { ADDR_PUMP, 0u,                             &q_all },
        };
        nx_modbus_rtu_master_t m2;
        nx_modbus_rtu_master_cfg_t c2 = cfg;
        c2.subs       = shared;
        c2.subs_count = 3u;
        assert(nx_modbus_rtu_master_init(&m2, &c2));

        static uint8_t script[16];
        const uint8_t  regs[2] = { 0x12, 0x34 };
        size_t n = build_read_rsp(script, ADDR_PUMP, NX_MODBUS_FC_READ_HOLDING_REGS,
                                  regs, 2u);
        memset(&g_io, 0, sizeof(g_io));
        g_io.rx = script; g_io.rx_len = n;
        pump_master(&m2, 4);

        nx_ref_msg_t *ma = pop_rsp(&q_a);
        nx_ref_msg_t *mall = pop_rsp(&q_all);
        assert(ma != NULL);                 /* matched by function code */
        assert(mall != NULL);               /* matched by the wildcard */
        assert(pop_rsp(&q_b) == NULL);      /* wrong function code: not claimed */
        assert(ma == mall);                 /* one copy, two references */
        nx_ref_msg_release(ma);
        nx_ref_msg_release(mall);

        nx_modbus_rtu_master_deinit(&m2);
        printf("  OK: one response fanned out to both claimants, zero-copy\n");
    }

    /* ---- 8. a write() that refuses the bytes releases the bus at once ----
     * Nothing reached the wire, so there is no transmission to wait out. The direction
     * pin must come back down in the same iteration: holding it asserted keeps this node
     * driving an RS-485 segment it has nothing to send on, which blocks every other
     * node. The frame is dropped and its block returned. */
    {
        size_t before = after_free_probe(&pool);

        nx_modbus_rtu_master_t m3;
        nx_modbus_rtu_master_cfg_t c3 = cfg;
        c3.write  = mock_write_fail;
        c3.dir_tx = mock_dir_tx;
        g_de_asserted = false;
        assert(nx_modbus_rtu_master_init(&m3, &c3));

        memset(&g_io, 0, sizeof(g_io));
        assert(nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue,
                                                      ADDR_PUMP, 0x0000u, 1u)
               == NX_MODBUS_RTU_MASTER_OK);

        /* One call is the whole story: tx picks the frame up and the write refuses it.
         * The check lands here, before any further iteration could tidy up after. */
        nx_modbus_rtu_master_process(&m3);

        assert(!g_de_asserted);                      /* bus released in that iteration */
        assert(g_io.tx_len == 0u);                   /* the mock accepted nothing */
        assert(nx_queue_is_empty(&request_queue));
        assert(after_free_probe(&pool) == before);   /* block handed back */

        nx_modbus_rtu_master_deinit(&m3);
        printf("  OK: a refused write released the bus and returned the frame's block\n");
    }

    /* ---- 9. the builders refuse what the protocol does not allow ----
     * A quantity out of range would produce a frame the slave rejects, costing a round
     * trip to learn what is knowable here; a broadcast read would make every slave on
     * the bus answer at once. Both are refused before anything is allocated. */
    {
        size_t before = after_free_probe(&pool);

        /* 03 tops out at 125 registers. */
        assert(nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue,
                                                      ADDR_PUMP, 0u, 126u)
               == NX_MODBUS_RTU_MASTER_ERR_PARAM);
        /* Zero quantity is not a request. */
        assert(nx_modbus_rtu_master_read_coils(&pool, &request_queue, ADDR_PUMP, 0u, 0u)
               == NX_MODBUS_RTU_MASTER_ERR_PARAM);
        /* A read addressed to the broadcast address. */
        assert(nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue,
                                                      NX_MODBUS_RTU_ADDR_BROADCAST, 0u, 1u)
               == NX_MODBUS_RTU_MASTER_ERR_PARAM);
        /* 0F: byte_count must be exactly ceil(qty/8) - 2 bytes for 9 coils, not 1. */
        const uint8_t one_byte[1] = { 0x01 };
        assert(nx_modbus_rtu_master_write_multiple_coils(&pool, &request_queue, ADDR_PUMP,
                                                         0u, 9u, one_byte, 1u)
               == NX_MODBUS_RTU_MASTER_ERR_PARAM);
        /* 10: byte_count must be exactly qty * 2. */
        const uint8_t three[3] = { 0, 1, 2 };
        assert(nx_modbus_rtu_master_write_multiple_regs(&pool, &request_queue, ADDR_PUMP,
                                                        0u, 2u, three, 3u)
               == NX_MODBUS_RTU_MASTER_ERR_PARAM);

        assert(nx_queue_is_empty(&request_queue));
        assert(after_free_probe(&pool) == before);   /* nothing was allocated */

        printf("  OK: out-of-range quantities, broadcast reads and bad byte counts refused\n");
    }

    /* ---- 10. a broadcast write is sent and answered by nobody ----
     * Address 0 is legal for a write and no slave replies, so the wire carries the frame
     * and no response ever arrives. Nothing about that is an error. */
    {
        nx_modbus_rtu_master_t m4;
        assert(nx_modbus_rtu_master_init(&m4, &cfg));
        memset(&g_io, 0, sizeof(g_io));

        assert(nx_modbus_rtu_master_write_single_reg(&pool, &request_queue,
                                                     NX_MODBUS_RTU_ADDR_BROADCAST,
                                                     0x0005u, 7u)
               == NX_MODBUS_RTU_MASTER_OK);
        pump_master(&m4, 4);

        uint8_t want[8];
        expect_fixed(want, NX_MODBUS_RTU_ADDR_BROADCAST, NX_MODBUS_FC_WRITE_SINGLE_REG,
                     0x0005u, 7u);
        assert(g_io.tx_len == 8u && memcmp(g_io.tx, want, 8u) == 0);
        assert(pop_rsp(&q_pump) == NULL);
        assert(pop_rsp(&q_meter) == NULL);

        nx_modbus_rtu_master_deinit(&m4);
        printf("  OK: broadcast write transmitted; no response, and none expected\n");
    }

    /* ---- 11. multi-register write: the variable-layout frame is built correctly ---- */
    {
        nx_modbus_rtu_master_t m5;
        assert(nx_modbus_rtu_master_init(&m5, &cfg));
        memset(&g_io, 0, sizeof(g_io));

        const uint8_t regs[4] = { 0x00, 0x0A, 0x00, 0x14 };   /* 10, 20 */
        assert(nx_modbus_rtu_master_write_multiple_regs(&pool, &request_queue, ADDR_METER,
                                                        0x0100u, 2u, regs, sizeof(regs))
               == NX_MODBUS_RTU_MASTER_OK);
        pump_master(&m5, 4);

        /* addr + cmd + start(2) + qty(2) + byte_count(1) + data(4) + crc(2) = 13 */
        uint8_t want[13];
        want[0] = ADDR_METER;
        want[1] = NX_MODBUS_FC_WRITE_MULTIPLE_REGS;
        want[2] = (uint8_t)((0x0100u) >> 8);
    want[3] = (uint8_t)((0x0100u) & 0xFFu);
        want[4] = (uint8_t)((2u) >> 8);
    want[5] = (uint8_t)((2u) & 0xFFu);
        want[6] = 4u;
        memcpy(&want[7], regs, 4u);
        nx_modbus_rtu_set_crc(want, sizeof(want));

        assert(g_io.tx_len == sizeof(want));
        assert(memcmp(g_io.tx, want, sizeof(want)) == 0);

        nx_modbus_rtu_master_deinit(&m5);
        printf("  OK: write-multiple-regs frame built with the right byte count\n");
    }

    /* ---- 12. init rejects a configuration that cannot work ---- */
    {
        nx_modbus_rtu_master_t bad;
        nx_modbus_rtu_master_cfg_t c = cfg;

        c = cfg; c.pool = NULL;
        assert(!nx_modbus_rtu_master_init(&bad, &c));
        c = cfg; c.request_queue = NULL;
        assert(!nx_modbus_rtu_master_init(&bad, &c));
        c = cfg; c.read = NULL;
        assert(!nx_modbus_rtu_master_init(&bad, &c));
        c = cfg; c.rx_size = 4u;                 /* smaller than the shortest response */
        assert(!nx_modbus_rtu_master_init(&bad, &c));

        /* A subscription with no queue would swallow every response it owns. */
        const nx_modbus_rtu_master_sub_t no_queue[] = { { ADDR_PUMP, 0u, NULL } };
        c = cfg; c.subs = no_queue; c.subs_count = 1u;
        assert(!nx_modbus_rtu_master_init(&bad, &c));

        /* No response ever carries the broadcast address, so such a claim is dead. */
        const nx_modbus_rtu_master_sub_t bcast[] = { { 0u, 0u, &q_pump } };
        c = cfg; c.subs = bcast; c.subs_count = 1u;
        assert(!nx_modbus_rtu_master_init(&bad, &c));

        printf("  OK: init refused every unworkable configuration\n");
    }

    /* ---- 13. a frame that arrives one byte per iteration is still assembled ----
     * A real UART hands over whatever has landed, so a response routinely spans several
     * process() calls. Framing must hold its partial frame across them and dispatch only
     * once the whole thing is in - never on a prefix. This is the path that "wait for
     * more bytes" exists for, and the one a bulk-delivery mock never exercises. */
    {
        static uint8_t script[16];
        const uint8_t  regs[4] = { 0x00, 0x2A, 0x01, 0x00 };
        size_t n = build_read_rsp(script, ADDR_PUMP, NX_MODBUS_FC_READ_HOLDING_REGS,
                                  regs, 4u);
        assert(n == 9u);

        memset(&g_io, 0, sizeof(g_io));
        g_io.rx = script; g_io.rx_len = 0u;    /* reveal one more byte per iteration */

        for (size_t i = 0; i < n; i++) {
            g_io.rx_len = i + 1u;
            nx_modbus_rtu_master_process(&m);
            if (i + 1u < n) {
                /* Nothing may be dispatched off an incomplete frame. */
                assert(nx_queue_is_empty(&q_pump));
            }
        }

        nx_ref_msg_t *msg = pop_rsp(&q_pump);
        assert(msg != NULL);                   /* delivered exactly when it completed */
        assert(nx_ref_msg_len(msg) == n);
        size_t         dlen = 0u;
        const uint8_t *data = nx_modbus_rtu_master_rsp_data(nx_ref_msg_data(msg),
                                                            nx_ref_msg_len(msg), &dlen);
        assert(data != NULL && dlen == 4u);
        assert((uint16_t)(((uint16_t)data[0] << 8) | data[1]) == 42u);
        nx_ref_msg_release(msg);
        assert(pop_rsp(&q_pump) == NULL);      /* and only once */

        printf("  OK: frame arriving one byte per iteration assembled, dispatched once\n");
    }

    /* ---- 14. end to end against a real slave ----
     * Master and slave, wired to each other through two byte pipes, with a business
     * module answering on the slave side. Nothing here hand-builds a frame: the request
     * is built by the master's helper and parsed by the slave's framing, the response is
     * built by the slave's reply helper and parsed by the master's. A wrong CRC
     * polynomial or a misread field cannot pass this, because the two sides would have
     * to be wrong in exactly the same way. */
    {
        /* ---- the slave and its business module ---- */
        nx_queue_t s_respq, s_inbox;
        static nx_ref_msg_t *s_respq_buf[8], *s_inbox_buf[4];
        nx_ref_msg_queue_init(&s_respq, s_respq_buf, 8);
        nx_ref_msg_queue_init(&s_inbox, s_inbox_buf, 4);

        const nx_modbus_rtu_slave_sub_t s_subs[] = {
            { NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000u, 0x00FFu, &s_inbox },
        };
        static uint8_t s_rx_buf[256];

        nx_modbus_rtu_slave_t sl;
        nx_modbus_rtu_slave_cfg_t s_cfg = {
            .slave_addr     = ADDR_PUMP,
            .baud_rate      = 115200u,
            .pool           = &pool,
            .rx_buf         = s_rx_buf,
            .rx_size        = sizeof(s_rx_buf),
            .subs           = s_subs,
            .subs_count     = 1u,
            .response_queue = &s_respq,
            .read           = lb_slave_read,
            .write          = lb_slave_write,
            .get_us         = mock_get_us,
        };
        assert(nx_modbus_rtu_slave_init(&sl, &s_cfg));

        /* ---- the master, pointed at the same pair of pipes ---- */
        nx_modbus_rtu_master_t ml;
        nx_modbus_rtu_master_cfg_t m_cfg = cfg;
        m_cfg.read  = lb_master_read;
        m_cfg.write = lb_master_write;
        assert(nx_modbus_rtu_master_init(&ml, &m_cfg));

        memset(&g_m2s, 0, sizeof(g_m2s));
        memset(&g_s2m, 0, sizeof(g_s2m));
        memset(&g_io, 0, sizeof(g_io));

        /* Ask for 3 holding registers from 0x0010. */
        assert(nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue,
                                                      ADDR_PUMP, 0x0010u, 3u)
               == NX_MODBUS_RTU_MASTER_OK);

        /* Run both sides together, as a product with two devices on one bus would. The
         * slave's business module answers from its inbox in between. */
        for (unsigned i = 0; i < 12u; i++) {
            nx_modbus_rtu_master_process(&ml);
            nx_modbus_rtu_slave_process(&sl);

            nx_ref_msg_t *req = NULL;
            while (nx_queue_pop(&s_inbox, &req) == NX_QUEUE_OK) {
                const nx_modbus_rtu_req_fix_t *q =
                    (const nx_modbus_rtu_req_fix_t *)nx_ref_msg_data(req);
                uint16_t start = (uint16_t)(((uint16_t)q->addr_h << 8) | q->addr_l);
                uint16_t qty   = (uint16_t)(((uint16_t)q->qty_h  << 8) | q->qty_l);

                /* The slave saw exactly what the master asked for. */
                assert(start == 0x0010u && qty == 3u);
                assert(q->cmd == NX_MODBUS_FC_READ_HOLDING_REGS);

                /* Answer with each register echoing its own address. */
                uint8_t data[6];
                for (uint16_t k = 0; k < qty; k++) {
                    uint16_t val = (uint16_t)(start + k);
                    data[k * 2]     = (uint8_t)(val >> 8);
                    data[k * 2 + 1] = (uint8_t)(val & 0xFFu);
                }
                assert(nx_modbus_rtu_slave_reply_read(&pool, &s_respq,
                                                      (const nx_modbus_rtu_header_t *)q,
                                                      data, (size_t)qty * 2u)
                       == NX_MODBUS_RTU_SLAVE_OK);
                nx_ref_msg_release(req);
            }
            g_io.clock_us += 500u;
        }

        /* The master parsed the slave's own response. */
        nx_ref_msg_t *rsp = pop_rsp(&q_pump);
        assert(rsp != NULL);
        size_t         dlen = 0u;
        const uint8_t *data = nx_modbus_rtu_master_rsp_data(nx_ref_msg_data(rsp),
                                                            nx_ref_msg_len(rsp), &dlen);
        assert(data != NULL && dlen == 6u);
        assert((uint16_t)(((uint16_t)data[0] << 8) | data[1]) == 0x0010u);
        assert((uint16_t)(((uint16_t)data[2] << 8) | data[3]) == 0x0011u);
        assert((uint16_t)(((uint16_t)data[4] << 8) | data[5]) == 0x0012u);
        nx_ref_msg_release(rsp);

        nx_modbus_rtu_master_deinit(&ml);
        nx_modbus_rtu_slave_deinit(&sl);
        printf("  OK: round trip against a real slave, both sides parsing each other\n");
    }

    /* ---- 15. a non-blocking interface is not written to while it is busy ----
     * With is_busy supplied, write() only starts the transfer. Until the frame has left
     * the wire the master must neither call write() again nor drop the direction pin, or
     * a second frame would be interleaved into the first on a shared segment. Once the
     * frame is out, the 3.5-character gap must be waited out before the next one starts. */
    {
        nx_queue_t nb_q;
        static nx_ref_msg_t *nb_q_buf[4];
        nx_ref_msg_queue_init(&nb_q, nb_q_buf, 4);

        nx_modbus_rtu_master_t mn;
        nx_modbus_rtu_master_cfg_t cn = cfg;
        cn.request_queue = &nb_q;
        cn.write         = mock_write_nb;
        cn.is_busy       = mock_is_busy;
        cn.dir_tx        = mock_dir_tx_counting;
        assert(nx_modbus_rtu_master_init(&mn, &cn));

        memset(&g_io, 0, sizeof(g_io));
        g_tx_inflight     = false;
        g_de_asserted     = false;
        g_dir_raise_count = 0u;

        /* Two requests queued back to back; they must go out one at a time. */
        assert(nx_modbus_rtu_master_write_single_reg(&pool, &nb_q, ADDR_PUMP, 0x0001u, 11u)
               == NX_MODBUS_RTU_MASTER_OK);
        assert(nx_modbus_rtu_master_write_single_reg(&pool, &nb_q, ADDR_PUMP, 0x0002u, 22u)
               == NX_MODBUS_RTU_MASTER_OK);

        /* First iteration starts frame one. */
        nx_modbus_rtu_master_process(&mn);
        assert(g_io.tx_len == 8u);          /* exactly one frame on the wire */
        assert(g_de_asserted);              /* still driving: the frame has not left */
        assert(g_dir_raise_count == 1u);

        /* While busy, further iterations must add nothing and keep the pin asserted. */
        for (unsigned i = 0; i < 5u; i++) {
            nx_modbus_rtu_master_process(&mn);
            g_io.clock_us += 500u;
        }
        assert(g_io.tx_len == 8u);          /* no second frame slipped in */
        assert(g_de_asserted);
        assert(g_dir_raise_count == 1u);

        /* The frame leaves the wire; the master releases the bus and enters the gap. */
        g_tx_inflight = false;
        nx_modbus_rtu_master_process(&mn);
        assert(!g_de_asserted);             /* bus released once the frame was out */
        assert(g_io.tx_len == 8u);          /* the gap has not expired yet */

        /* The gap expires, and the frame after it starts on the following iteration:
         * each call advances the transmit path by one state, so leaving the gap and
         * starting the next frame are two separate steps. That bounds how much one
         * process() call can do - it never chains several frames onto the wire at once. */
        g_io.clock_us += 500u;
        nx_modbus_rtu_master_process(&mn);      /* gap expires */
        assert(g_io.tx_len == 8u);              /* nothing new yet */
        nx_modbus_rtu_master_process(&mn);      /* now the second frame starts */
        assert(g_io.tx_len == 16u);
        assert(g_dir_raise_count == 2u);

        /* Drain it so the instance is left idle. */
        g_tx_inflight = false;
        g_io.clock_us += 500u;
        nx_modbus_rtu_master_process(&mn);

        nx_modbus_rtu_master_deinit(&mn);
        printf("  OK: busy interface never double-written; gap honored before the next frame\n");
    }

    /* ---- 16. a refusal reaches the module that asked, past a function-code filter ----
     * An exception carries the original code with 0x80 set. A subscription filtered to
     * that code must still match it, or the one module that needs to hear "your request
     * was rejected" is the only one that never would. */
    {
        nx_queue_t q_rd, q_wr;
        static nx_ref_msg_t *rd_buf[4], *wr_buf[4];
        nx_ref_msg_queue_init(&q_rd, rd_buf, 4);
        nx_ref_msg_queue_init(&q_wr, wr_buf, 4);

        /* Two modules split one device by function code. */
        const nx_modbus_rtu_master_sub_t split[] = {
            { ADDR_PUMP, NX_MODBUS_FC_READ_HOLDING_REGS, &q_rd },
            { ADDR_PUMP, NX_MODBUS_FC_WRITE_SINGLE_REG,  &q_wr },
        };
        nx_modbus_rtu_master_t mx;
        nx_modbus_rtu_master_cfg_t cx = cfg;
        cx.subs = split; cx.subs_count = 2u;
        assert(nx_modbus_rtu_master_init(&mx, &cx));

        /* The pump refuses a write with 0x03: cmd on the wire is 0x86. */
        static uint8_t script[5];
        size_t n = build_exc_rsp(script, ADDR_PUMP, NX_MODBUS_FC_WRITE_SINGLE_REG,
                                 NX_MODBUS_EXC_ILLEGAL_DATA_VALUE);
        assert(script[1] == 0x86u);
        memset(&g_io, 0, sizeof(g_io));
        g_io.rx = script; g_io.rx_len = n;
        pump_master(&mx, 4);

        nx_ref_msg_t *msg = pop_rsp(&q_wr);
        assert(msg != NULL);                 /* the writer hears its own refusal */
        assert(pop_rsp(&q_rd) == NULL);      /* the reader is not told about it */

        uint8_t exc = 0u;
        assert(nx_modbus_rtu_master_rsp_is_exception(nx_ref_msg_data(msg),
                                                     nx_ref_msg_len(msg), &exc));
        assert(exc == NX_MODBUS_EXC_ILLEGAL_DATA_VALUE);
        nx_ref_msg_release(msg);

        nx_modbus_rtu_master_deinit(&mx);
        printf("  OK: exception matched the filter for the code it answers\n");
    }

    nx_modbus_rtu_master_deinit(&m);
    return 0;
}


