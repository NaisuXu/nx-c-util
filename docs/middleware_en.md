# Middleware Modules
### nx_can_bus — CAN / CAN FD frame structures and helpers

A header-only module with a generic in-memory representation of a CAN frame and
small, dependency-free helpers. Aimed at a tool/adapter sitting between a host
and the bus, so the frame carries not just data but also direction and error
context.

- **Classic CAN and CAN FD** — one `nx_can_msg_t` covers both; the payload is a
  flexible array member, so the caller sizes storage to the actual length (up to
  64 bytes). Frame attributes (`is_ext`, `is_remote`, `is_fd`, `brs`, `esi`,
  `dlc`) are packed into a bitfield that also exposes a `flags.raw` word for fast
  copy/compare.
- **Host/tool direction and channel** — `dir` (see `nx_can_dir_t`) distinguishes
  `TX` (host asks the tool to send), `RX` (received from the bus), and `TXR` (the
  tool's transmit-completion report for a prior `TX`). `ch` is a 4-bit channel
  number: like `dir`, it is only meaningful in a tool/adapter context, naming
  which CAN interface the frame belongs to.
- **Error / result reporting** — an `is_err` flag plus a 4-bit `err_code` (see
  `nx_can_err_t`) share one encoding across both directions: on an `RX` frame it
  names an error frame's cause, on a `TXR` report it names why the transmit
  failed (bit / stuff / form / ack / crc error, arbitration lost, bus-off,
  timeout, overrun, ...).
- **DLC helpers** — `nx_can_dlc_to_len` and `nx_can_len_to_dlc` convert between a
  4-bit DLC and the actual byte length, handling the CAN FD sizes (12/16/20/24/
  32/48/64) as well as classic 0..8.
- **Header-only** — every helper is `static inline`; just include the header,
  nothing to compile or link.

```c
#include "nx_can_bus.h"

/* a received CAN FD frame carrying 16 bytes */
uint8_t          buf[sizeof(nx_can_msg_t) + 16];
nx_can_msg_t    *msg = (nx_can_msg_t *)buf;
msg->id             = 0x123;
msg->flags.raw      = 0;                    /* clear all flags first */
msg->flags.bits.dir = NX_CAN_DIR_RX;
msg->flags.bits.is_fd = 1;
msg->flags.bits.dlc = nx_can_len_to_dlc(16);   /* -> DLC 10 */
/* ... fill msg->data[0..15] ... */

uint32_t len = nx_can_dlc_to_len(msg->flags.bits.dlc);   /* 16 */

/* the tool reports a failed transmit: arbitration was lost */
nx_can_msg_t txr;
txr.flags.raw           = 0;
txr.flags.bits.dir      = NX_CAN_DIR_TXR;
txr.flags.bits.is_err   = 1;
txr.flags.bits.err_code = NX_CAN_ERR_ARB_LOST;
```

> **Note:** `flags.bits` is an in-memory layout; bitfield ordering is
> compiler-defined. When moving frames across a wire or between toolchains,
> serialize `flags.raw` (or pack fields explicitly) rather than memcpy'ing the
> struct.


### nx_modbus_rtu — Modbus RTU frame structures and CRC

In-memory representations of the common Modbus RTU frames plus a table-driven
CRC-16/MODBUS. The frame structs map 1:1 onto the wire, and the CRC needs a
small lookup table, so this module has a `.c` file.

- **Frame structs map 1:1 onto the wire** — every struct is made of `uint8_t`
  fields only, so it has alignment 1 and no padding, and a received byte buffer
  can be cast directly to the matching type to parse it in place, with no packing
  pragma. Keeping every frame field a `uint8_t` is what guarantees this — a
  non-`uint8_t` field could introduce padding and break the 1:1 mapping.
- **Covers the common frames** — fixed- and variable-length requests and
  responses (`nx_modbus_rtu_req_fix_t` / `req_var_t` / `rsp_fix_t` / `rsp_var_t`)
  plus the exception response (`nx_modbus_rtu_rsp_exc_t`). Function codes and
  exception codes have their own enums (`nx_modbus_fc_t`, `nx_modbus_exc_t`),
  which are transport-independent and would be shared by a future TCP module.
- **Byte order helpers** — 16-bit fields (address, quantity, register values) are
  big-endian on the wire; use `nx_modbus_rtu_get_u16` / `set_u16` to convert. The
  trailing CRC is little-endian (low byte first). For the variable-length frames
  the CRC is not a named field — `nx_modbus_rtu_req_var_crc` / `rsp_var_crc`
  return a pointer to it after the payload.
- **Self-contained CRC** — `nx_modbus_rtu_crc16` computes CRC-16/MODBUS from a
  256-entry table (no dependency on `nx_crc`); `nx_modbus_rtu_set_crc` fills a
  frame's trailing CRC and `nx_modbus_rtu_check_crc` verifies a received one.
  Both frame helpers require a length of at least 5 (the shortest valid ADU is a
  5-byte exception response).
- **Header-mostly, dependency-free** — the frame structs and byte helpers are in
  the header; only the CRC lives in the `.c`. No dependency on the other modules.

```c
#include "nx_modbus_rtu.h"

/* build a "read holding registers" request: addr 1, start 0x0000, count 10 */
uint8_t buf[8];
nx_modbus_rtu_req_fix_t *req = (nx_modbus_rtu_req_fix_t *)buf;
req->addr = 1u;
req->cmd  = NX_MODBUS_FC_READ_HOLDING_REGS;
nx_modbus_rtu_set_u16(&req->addr_h, 0x0000u);   /* starting address */
nx_modbus_rtu_set_u16(&req->qty_h, 10u);        /* quantity         */
nx_modbus_rtu_set_crc(buf, sizeof(buf));        /* fill crc_l / crc_h */

/* on a received frame, verify the CRC then read a 16-bit field */
if (nx_modbus_rtu_check_crc(buf, sizeof(buf))) {
    uint16_t qty = nx_modbus_rtu_get_u16(&req->qty_h);   /* 10 */
    (void)qty;
}
```

> **Note:** casting a byte buffer to a frame struct relies on the all-`uint8_t`
> layout above; the same layout is why there is no packing pragma. Multi-byte
> fields still need `get_u16` / `set_u16` for the big-endian wire order — don't
> read them as native `uint16_t`.


### nx_modbus_rtu_slave — event-driven RTU slave: frame → subscription dispatch

The link/dispatch layer on top of `nx_modbus_rtu` (frame structs + CRC). It pulls
bytes from the wire, slices and validates complete RTU frames, and routes each
request to whatever business module subscribed to it — carrying no business logic
of its own. It touches no hardware: all I/O is injected as non-blocking callbacks,
and it is driven from the main loop by a single `process()` call.

- **Subscription dispatch** — each business module claims a `(function code +
  inclusive address range)` in the subscription table; a matching request is fanned
  out zero-copy (as a reference-counted `nx_ref_msg`) to that module's queue. One
  function code can be split across modules by range, and one request can reach
  several subscribers at once.
- **Structural validation, in exception order** — before dispatch the slave settles
  what the frame alone determines: function support (`0x01`), then quantity /
  byte_count / single-write value legality (`0x03`), then address containment
  (`0x02`). A dispatched request is therefore already well-formed; whether a value
  is *operationally* acceptable for a given register stays with the business module,
  which may push its own exception response.
- **Length-based framing** — every supported frame's length follows from its
  function code (8 bytes for `01..06`, `9 + byte_count` for `0F/10`), so RX needs no
  inter-character (T3.5) timer — robust on a busy bus where arrival timing cannot be
  trusted. Resync after a bad address or CRC drops one byte and retries. On TX a
  3.5-character gap (derived from `baud_rate`) follows each frame.
- **Injected non-blocking I/O** — `read` / `write` move bytes, `is_busy` reports
  whether the interface is still transmitting (so a shared, non-exclusive bus is
  driven only when free), optional `dir_tx` toggles the RS-485 direction (DE) pin,
  and `get_us` times the TX gap. A NULL `is_busy` treats `write` as blocking; a NULL
  `get_us` skips the gap. The serial callbacks (`read` / `write` / `is_busy`) share
  `io_ctx`, which may stay NULL when the driver is a single module-owned instance;
  `dir_tx` takes its own `dir_ctx` (the DE pin is often a separate GPIO), and `get_us`
  takes no context as a system-wide time source.
- **Reply helpers for business modules** — three builders cover every answer a module can
  give: `nx_modbus_rtu_slave_reply_read()` wraps the data it gathered behind a byte count,
  `nx_modbus_rtu_slave_reply_write()` builds the write confirmation that echoes the request,
  and `nx_modbus_rtu_slave_reply_exception()` reports a code. Each takes only the pool and
  the response queue, so a business module needs no slave handle; the reply's address and
  function code are taken from the request frame. All three return an
  `nx_modbus_rtu_slave_ret_t` that names why a reply was not queued — `ERR_NOMEM` and
  `ERR_FULL` are the resource shortages worth logging, `ERR_BROADCAST` is the normal
  outcome for a broadcast request, `ERR_PARAM` a caller bug.
- **Allocates nothing itself** — the RX framing buffer, the tiered pool behind every
  message, and the shared response queue are all caller-owned. Out of memory degrades
  gracefully: the response is dropped and the master simply times out.

```c
#include "nx_modbus_rtu_slave.h"

/* one business module owns holding registers 0x0000..0x000F */
const nx_modbus_rtu_slave_sub_t subs[] = {
    { NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 0x000F, &valve_q },
    { NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0000, 0x000F, &valve_q },
};

nx_modbus_rtu_slave_t     slave;
nx_modbus_rtu_slave_cfg_t cfg = {
    .slave_addr     = 0x11u,
    .baud_rate      = 115200u,         /* derives the 3.5-char TX gap */
    .pool           = &pool,           /* tiered pool for messages */
    .rx_buf         = rx_buf,
    .rx_size        = sizeof(rx_buf),
    .subs           = subs,
    .subs_count     = 2u,
    .response_queue = &response_queue, /* business replies + exceptions go here */
    .read           = uart_read,       /* injected non-blocking I/O */
    .write          = uart_write,
    .get_us         = board_micros,    /* is_busy NULL => write is blocking */
};
nx_modbus_rtu_slave_init(&slave, &cfg);

for (;;) {
    nx_modbus_rtu_slave_process(&slave);            /* RX dispatch + TX pump */
    valve_business(&valve_q, &response_queue, &pool);  /* drain inbox, push replies */
}
```

> **Note:** the slave validates *structure* (function `0x01`, value `0x03`, address
> `0x02`), not *meaning*. A business module still range-checks the actual values it
> is asked to write and may emit its own `0x03` exception onto the response queue.
> Broadcasts (address 0) are dropped by default, leaving only unicast requests
> handled; with `accept_broadcast = true` they are dispatched but never answered —
> no response, no exception.



### nx_tp_sdu — transport-layer service data unit

A structs-only header describing the object a diagnostic transport exchanges with
the layer above it: a complete message, plus the few facts about how it travelled
that the layer above needs in order to answer correctly. Those facts do not depend
on what carried the message, so they are described once here and every transport
fills the same structure.

- **One allocation per message** — the payload is the flexible-array part, so the
  header and the bytes it describes sit in a single pooled block and travel by
  pointer through the caller's queues.
- **Addressing** — `ta_type` records whether a message was addressed to a single
  receiver or to every receiver at once (see `nx_tp_ta_type_t`), which is what
  decides whether an answer is expected at all.
- **Connection tag** — `link` numbers the connection a message belongs to, as the
  application chooses to number them. A transport copies its configured value into
  everything it publishes and never interprets it, so one upper layer can serve
  several transports through a single queue and still tell them apart.
- **What it reports** — `kind` separates the two things that share an outbound
  queue: a message that arrived, and the outcome of a message that was sent (see
  `nx_tp_sdu_kind_t`).
- **Outcome** — `result` names how an operation ended (see `nx_tp_result_t`):
  completed, one of the timeouts, a sequence-number or flow-control error, a peer
  that asked to wait more times than are tolerated, or a message that did not fit
  the space available to receive it. The enumerators are listed in the priority
  order they are resolved in, so values may be compared.
- **32-bit length** — `len` counts payload bytes. How long a message may be is
  bounded by the configuration and the memory behind it, never by the width of
  this field.
- **Types only, no dependencies** — nothing to compile and nothing to link. A
  zero-initialized instance describes a physically addressed indication that
  succeeded, so only the fields that depart from that need setting.

```c
#include "nx_tp_sdu.h"

/* a request handed to a transport: it sends len bytes taken from data */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, sizeof(nx_tp_sdu_t) + 2u);
nx_tp_sdu_t  *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
s->len     = 2u;
s->data[0] = 0x22u;
s->data[1] = 0xF1u;

/* coming back the other way, one field decides how to read the rest */
const nx_tp_sdu_t *in = (const nx_tp_sdu_t *)nx_ref_msg_data(msg);
if (in->kind == NX_TP_SDU_INDICATION) {
    handle(in->link, in->ta_type, in->data, in->len);   /* a message arrived */
} else if (in->result != NX_TP_N_OK) {
    report(in->link, in->result);                       /* a send ended badly */
}
```

> **Note:** direction is not a field. Which way a message is going follows from the
> queue it travels on — a transport reads send requests from one queue and
> publishes what it received and what it finished sending to another — so `kind`
> is what tells an arrived message apart from a send that has ended, and `result`
> carries an outcome only on the latter.


### nx_can_isotp — ISO 15765-2 (DoCAN / ISO-TP) segmented transport

A queue-to-queue transport layer that carries messages larger than a single CAN
frame. It drains a queue of received frames, reassembles complete messages and
hands them to an upper layer; it takes the upper layer's messages, segments them
back into frames and paces them onto the CAN transmit queue. The module owns no
bus and touches no hardware — every artifact in and out is an `nx_ref_msg`
exchanged over caller-provided queues, and `nx_can_isotp_process()` is the only
thing that moves them.

- **Both roles from one instance** — configure the ID pair and let the upper
  layer answer, and it serves as an ECU under diagnostics; call
  `nx_can_isotp_send()` with a request, and it acts as a tester issuing them. The
  same instance does both at once.
- **Full protocol data units** — single frames (with the 2016 length escape for
  payloads above 7 bytes), first frames with a 12-bit length and a 32-bit escape
  above 4095, consecutive frames with 4-bit sequence numbers that wrap 0..15, and
  flow control carrying `CTS` / `WAIT` / `OVERFLOW`, a block size and an `STmin`.
- **Addressing as a pair** — `phys_rx_id` is what the instance accepts and
  `phys_tx_id` is what it answers under, including the flow control it emits
  while receiving a segmented message. An optional `func_rx_id` adds functional
  (1:N) reception, single-frame only because a shared request ID cannot carry
  per-receiver flow control; a received message reports in `ta_type` whether it
  was addressed to one receiver or to many, so the upper layer can tell them
  apart. An optional `func_tx_id` adds functional transmission — pass
  `NX_TP_TA_FUNCTIONAL` to `nx_can_isotp_send()` to broadcast a request — also
  single-frame only for the same reason, and off by default because a network has
  at most one functional sender, so only a tester instance configures it. IDs are
  concrete frame values, never bit fields, so one instance is valid for a UDS
  `0x18DA..xx` pair, a vendor scheme, or plain 11-bit IDs.
- **Configurable timing** — `n_as_us` bounds how long a frame may wait to be
  handed to the link while transmitting and `n_ar_us` the same while receiving,
  `n_bs_us` bounds the wait for a peer's flow control while transmitting,
  `n_cr_us` the wait for its next consecutive frame while receiving, and
  `n_wft_max` how many consecutive `WAIT` frames a peer may use to hold the
  sender before the transmission is abandoned. Each `WAIT` buys a fresh
  `n_bs_us` window. A zero field takes the documented default.
- **A busy link costs only the wait** — a frame is handed to the link by
  publishing it to `can_tx_queue`, so a queue with no room is what "the link
  would not take it" means. Such a frame is offered again on later
  `nx_can_isotp_process()` calls, since a transmit queue that briefly fills up
  normally drains within milliseconds; only once `n_as_us` or `n_ar_us` has
  passed does the conversation end and report `N_TIMEOUT_A`, naming the local
  link rather than a peer that has gone quiet. The instant a frame truly leaves
  the bus is the driver's to observe, so these two are measured from the moment
  the frame was offered to the queue, which runs a little ahead of the
  transmission itself and therefore expires a little late rather than early. The
  overflow flow control frame that refuses a message is the exception: the
  reception is over either way, so that frame is offered once and not held.
- **Flow control on your terms** — `rx_block_size` and `rx_stmin` are what this
  instance advertises while receiving, so a peer is told to pause every N frames
  and to keep a minimum separation; the module issues a fresh flow control frame
  as each block runs out.
- **Bounded reception** — `rx_max_len` is the longest message the instance
  accepts. A first frame announcing more is answered with an overflow flow
  control frame before anything is taken from the pool, so what arrives is
  governed by the configuration rather than by how full the pool happens to be.
  Leaving it 0 accepts whatever the pool can hold at the moment the first frame
  lands, still under the module's own ceiling.
- **One ceiling above every configuration** — `NX_CAN_ISOTP_MAX_MSG_LEN` is the
  longest message the module handles: what a length can say on the wire, less
  what one pooled block spends on its headers — 4294967255 bytes wherever
  `size_t` is at least 32 bits wide. `nx_can_isotp_send()` refuses a longer
  request with `NX_CAN_ISOTP_ERR_LENGTH`, and a first frame announcing more is
  answered with an overflow flow control frame before anything is allocated, so
  a length taken off the bus can never be reassembled into a block too small to
  hold it. What a given instance actually accepts is smaller still: whatever its
  pool and `rx_max_len` allow.
- **A message type that outlives CAN** — what crosses the upper boundary is an
  `nx_tp_sdu_t`: the payload plus how it was addressed, which connection it
  belongs to (`link`, copied from the configuration so one upper layer can serve
  several instances from one queue), what it reports (`kind`) and how it turned
  out (`result`). The length is a 32-bit count.
- **Outcome reporting** — with `confirm_tx` set, a transmission that ends and a
  reception that fails each publish an SDU naming the reason: a timeout waiting
  for flow control or a consecutive frame, a sequence number out of order, a flow
  status that is not defined, a peer that asked to wait too many times, or a
  message too long to receive. `kind` separates these from messages that arrived.
- **Frame padding** — with `pad_frames` set, every emitted frame is raised to
  eight data bytes and the unused tail is filled with `pad_byte`, as the
  diagnostic networks that expect a fixed frame length require. Left clear, each
  frame carries exactly the bytes it holds.
- **Paced, bounded transmission** — `tx_frames_per_process` caps how many frames
  leave per `process()` call, and `STmin` from the peer's flow control spaces
  consecutive frames against the injected clock.
- **Zero-copy reassembly** — one message-sized block is taken from the pool when
  the first frame arrives and the message is reassembled straight into it, so a
  finished message is handed up without a copy.

```c
static uint8_t pool_mem[8192];
static nx_tiered_mem_pool_t pool;
static nx_ref_msg_t *sdu_tx_buf[8], *sdu_rx_buf[4], *can_rx_buf[16], *can_tx_buf[16];
static nx_queue_t sdu_tx_q, sdu_rx_q, can_rx_q, can_tx_q;
static nx_can_isotp_t iso;

const nx_tiered_level_cfg_t tiers[] = {
    {sizeof(nx_ref_msg_t) + sizeof(nx_can_msg_t) + 8u,       32},  /* CAN frames */
    {sizeof(nx_ref_msg_t) + sizeof(nx_can_isotp_sdu_t) + 4096u, 4},/* messages */
};
const nx_tiered_mem_pool_cfg_t pool_cfg = {
    .memory = pool_mem, .memory_size = sizeof(pool_mem),
    .tiers = tiers, .tier_count = 2,
};
nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL);
nx_ref_msg_queue_init(&sdu_tx_q, sdu_tx_buf, 8);
nx_ref_msg_queue_init(&sdu_rx_q, sdu_rx_buf, 4);
nx_ref_msg_queue_init(&can_rx_q, can_rx_buf, 16);
nx_ref_msg_queue_init(&can_tx_q, can_tx_buf, 16);

const nx_can_isotp_cfg_t cfg = {
    .max_frame_len = NX_CAN_ISOTP_FRAME_8,  /* 8 = classic CAN, 64 = CAN FD */
    .pad_frames    = true,                  /* fill every frame out to 8 bytes */
    .pad_byte      = 0xCCu,                 /* what goes in the unused tail */
    .phys_rx_id    = 0x7E0u,                /* received physically addressed */
    .phys_tx_id    = 0x7E8u,                /* transmitted, and flow control */
    .func_rx_id    = 0x7DFu,                /* 1:N requests; 0 disables */
    .pool          = &pool,
    .sdu_rx_queue  = &sdu_rx_q,             /* upper -> module: to send */
    .sdu_tx_queue  = &sdu_tx_q,             /* module -> upper: received */
    .can_rx_queue  = &can_rx_q,             /* driver -> module: frames */
    .can_tx_queue  = &can_tx_q,             /* module -> driver: frames */
    .link          = 1u,                    /* stamped into every published SDU */
    .confirm_tx    = true,                  /* report how each send ended */
    .get_us        = board_micros,
    .n_as_us       = 1000000u,              /* 0 = 1000 ms */
    .n_ar_us       = 1000000u,              /* 0 = 1000 ms */
    .n_bs_us       = 1000000u,              /* 0 = 1000 ms */
    .n_cr_us       = 1000000u,              /* 0 = 1000 ms */
    .n_wft_max     = 4u,                    /* 0 = 4 */
    .rx_max_len    = 4096u,                 /* longer is refused with OVERFLOW */
    .rx_block_size = 8u,                    /* 0 = whole message at once */
    .rx_stmin      = 0x0Au,                 /* 10 ms between the peer's frames */
    .tx_frames_per_process = 1u,            /* 0 = emit all flow control allows */
};
nx_can_isotp_init(&iso, &cfg);

for (;;) {
    can_driver_fill(&can_rx_q);   /* driver pushes received frames as nx_ref_msg */
    nx_can_isotp_process(&iso);   /* reassemble, flow control, segment, pace */
    can_driver_drain(&can_tx_q);  /* driver pops frames and transmits them */

    nx_ref_msg_t *m;                          /* drain what the module published */
    while (nx_queue_pop(&sdu_tx_q, &m) == NX_QUEUE_OK) {
        const nx_can_isotp_sdu_t *sdu = nx_ref_msg_data(m);
        if (sdu->kind == NX_TP_SDU_INDICATION && sdu->result == NX_TP_N_OK) {
            uds_handle(&iso, sdu->ta_type, sdu->data, sdu->len);
        } else {
            uds_report(&iso, sdu->kind, sdu->result);   /* a send ended, or a receive failed */
        }
        nx_ref_msg_release(m);                /* consumer releases its reference */
    }
}
```

> **Note:** every queue is named from the module's point of view — it reads
> `sdu_rx_queue` and `can_rx_queue`, and writes `sdu_tx_queue` and
> `can_tx_queue`. So the queue the upper layer reads its messages from is the
> module's *transmit* queue, and the one it pushes send requests to is the
> module's *receive* queue.
>
> `process()` consumes every frame it finds on `can_rx_queue`, releasing the ones
> whose ID matches neither receive ID. Two instances may therefore share one
> `can_rx_queue` only if their ID sets are disjoint — whichever instance runs
> first takes the frame, matched or not. Give each instance its own receive
> queue, or filter in the driver, when that is not certain. Anything popped from
> `sdu_tx_queue` is a reference the consumer owns and must `nx_ref_msg_release()`
> once handled; forgetting to is a pool leak, not a use-after-free, because the
> block is only returned when the count reaches zero. Frames pushed to
> `can_tx_queue` are likewise the driver's to release after transmitting.
