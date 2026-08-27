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
  number, 0..`NX_CAN_MAX_CH`: like `dir`, it is only meaningful in a tool/adapter
  context, naming which CAN interface the frame belongs to.
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
- **Wire order** — 16-bit fields (address, quantity, register values) are carried
  most-significant byte first; reassemble them how the code below does. The trailing
  CRC is little-endian (low byte first). For the variable-length frames the CRC is not
  a named field — `nx_modbus_rtu_req_var_crc` / `rsp_var_crc` return a pointer to it
  after the payload.
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
req->addr_h = (uint8_t)(0x0000u >> 8);   /* starting address */
req->addr_l = (uint8_t)(0x0000u & 0xFFu);
req->qty_h  = (uint8_t)(10u >> 8);       /* quantity         */
req->qty_l  = (uint8_t)(10u & 0xFFu);
nx_modbus_rtu_set_crc(buf, sizeof(buf));        /* fill crc_l / crc_h */

/* on a received frame, verify the CRC then read a 16-bit field */
if (nx_modbus_rtu_check_crc(buf, sizeof(buf))) {
    uint16_t qty = (uint16_t)(((uint16_t)req->qty_h << 8) | req->qty_l);   /* 10 */
    (void)qty;
}
```

> **Note:** casting a byte buffer to a frame struct relies on the all-`uint8_t`
> layout above; the same layout is why there is no packing pragma. Multi-byte
> fields are carried most-significant byte first — don't
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
  several subscribers at once. A full subscriber queue drops that copy; if *every*
  matching subscriber refuses it, the request is answered `0x06` (slave device busy)
  so the master knows to retry rather than being met with silence. A partial delivery
  sends no exception — the request did take effect somewhere.
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
  takes no context as a system-wide time source. A `write` that returns false has not
  taken the bytes, so there is nothing to wait out: the frame is dropped and the
  direction pin comes back down in the same iteration, leaving the segment free for
  other nodes.
- **Releasing an instance** — `nx_modbus_rtu_slave_deinit()` hands back the pool block
  of a frame caught mid-transmit, deasserts the direction pin, and parks the state
  machine idle. Call it before re-initializing an instance that has been running, and
  when taking one out of service; the response queue is left alone, since the messages
  in it belong to whoever pushed them.
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



### nx_modbus_rtu_master — event-driven RTU master: queue → wire → subscription dispatch

The link/dispatch layer on top of `nx_modbus_rtu` (frame structs + CRC). It transmits the
request frames business modules push onto a shared queue, slices and validates the
responses that come back, and routes each one to whatever business module owns the slave
it came from — carrying no business logic of its own. It touches no hardware: all I/O is
injected as non-blocking callbacks, and it is driven from the main loop by a single
`process()` call.

- **Subscription dispatch by slave address** — a business module owns the devices it
  talks to, so each entry in the subscription table claims a `slave_addr` and receives
  that device's responses (zero-copy, as a reference-counted `nx_ref_msg`) on its own
  queue. An optional `func` filter narrows an entry to one function code when two modules
  split a device; `func = 0` takes everything from that address. One response can reach
  several subscribers at once. A response no subscription claims is discarded — a master
  answers nothing, so there is nothing to emit in its place.
- **Timeouts belong to the business module** — this module transmits and dispatches; it
  keeps no record of which requests are outstanding. A module that needs to know an answer
  never came notes when it sent and decides for itself when to retry or give up. An empty
  response queue means the answer has not arrived, not that anything failed.
- **Request builders** — one per function code (`nx_modbus_rtu_master_read_holding_regs`,
  `..._write_multiple_regs`, and so on) builds a well-formed frame, stamps its CRC and
  queues it. They take only the pool and the request queue, so a business module needs no
  handle on the master. Each refuses what the protocol does not allow — a quantity outside
  its range, a byte count that disagrees with it, a broadcast read — rather than spending
  a round trip to be told the same thing.
- **Length-based framing** — every response's length follows from its own bytes (5 for an
  exception, 8 for a write confirmation, `3 + byte_count + 2` for a read), so RX needs no
  inter-character (T3.5) timer — robust on a busy bus where arrival timing cannot be
  trusted. A partial frame is held across calls and dispatched only once complete. Resync
  after an unsupported code or a bad CRC drops one byte and retries. On TX a 3.5-character
  gap (derived from `baud_rate`) follows each frame.
- **One step per call** — each `process()` advances the transmit path by a single state, so
  no call chains several frames onto the wire. With `is_busy` supplied, a frame in flight
  blocks both the next `write()` and the release of the direction pin, which is what keeps
  a shared RS-485 segment from being driven by two frames at once.
- **Response inspection** — a subscriber pops a whole ADU with its CRC already checked.
  `nx_modbus_rtu_master_rsp_is_exception()` tells a refusal from an answer and yields the
  exception code; `nx_modbus_rtu_master_rsp_data()` finds a read response's payload and
  its length, pointing into the message rather than copying it.
- **Releasing an instance** — `nx_modbus_rtu_master_deinit()` hands back the pool block of
  a frame caught mid-transmit, drops the direction pin and discards a partial response. The
  request queue is left alone: what is still in it belongs to whoever pushed it.

```c
#include "nx_modbus_rtu_master.h"

/* two business modules, one device each */
const nx_modbus_rtu_master_sub_t subs[] = {
    { 0x11u, 0u, &q_pump  },      /* every response from 0x11 */
    { 0x22u, 0u, &q_meter },      /* every response from 0x22 */
};

nx_modbus_rtu_master_t     master;
nx_modbus_rtu_master_cfg_t cfg = {
    .baud_rate     = 115200u,        /* derives the 3.5-char TX gap */
    .pool          = &pool,          /* tiered pool for messages */
    .rx_buf        = rx_buf,
    .rx_size       = sizeof(rx_buf),
    .subs          = subs,
    .subs_count    = 2u,
    .request_queue = &request_queue, /* what modules push here gets sent */
    .read          = uart_read,      /* injected non-blocking I/O */
    .write         = uart_write,
    .get_us        = board_micros,   /* is_busy NULL => write is blocking */
};
nx_modbus_rtu_master_init(&master, &cfg);

/* a business module asks for 10 registers; the reply lands on its own queue */
nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue, 0x11u, 0x0000u, 10u);

for (;;) {
    nx_modbus_rtu_master_process(&master);        /* TX pump + RX dispatch */
    pump_business(&q_pump, &request_queue, &pool);   /* drain inbox, ask again */
}
```

> **Note:** RTU frames carry no transaction id, so a response is matched to a request by
> slave address and function code alone. Two outstanding requests to the same slave with
> the same function code come back as two responses nothing in the frames can tell apart —
> a module that needs strict pairing should keep one request per slave in flight. The same
> is true of a late response to a request already given up on: it arrives looking exactly
> like an answer to the current one.


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
- **Outcome reporting** — with `confirm_tx` set, a transmission that ends and a
  reception that fails each publish an SDU naming the reason: a timeout waiting
  for flow control or a consecutive frame, a sequence number out of order, a flow
  status that is not defined, a peer that asked to wait too many times, or a
  message too long to receive. `kind` separates these from messages that arrived.
- **One instance, one bus** — `ch` is the CAN channel the instance serves. It is
  stamped into every emitted frame, so a driver spanning several buses reads off
  the frame which one to transmit on, and it is matched on every received frame,
  so a misrouted one is passed over. A driver holding one bus per module leaves it
  0 at both ends and the match always holds.
- **Configured frame format** — `ext_id`, `fd_frames` and `brs` are what every
  emitted frame carries: a 29-bit identifier, a CAN FD frame, and the bit-rate
  switch that runs its data phase at the faster rate. `ext_id` also decides which
  received frames are this instance's traffic, since an 11-bit and a 29-bit
  identifier of the same number name two different addresses.
- **Frame geometry** — `max_frame_len` is the payload each frame carries, and it
  must be one of the eight sizes a data length code expresses: 8, 12, 16, 20, 24,
  32, 48 or 64. Anything above 8 needs `fd_frames`, and `brs` needs it too. A
  wider geometry puts more into a single frame and more into each consecutive
  frame, so a message that would need segmenting at 8 bytes may travel whole.
- **Frame padding** — with `pad_frames` set, every emitted frame is raised to
  eight data bytes and the unused tail is filled with `pad_byte`, as the
  diagnostic networks that expect a fixed frame length require. Left clear, each
  frame carries exactly the bytes it holds. Above eight bytes the tail is filled
  either way, since a frame has to land on an expressible size.
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
    .max_frame_len = NX_CAN_ISOTP_FRAME_8,  /* 8/12/16/20/24/32/48/64 */
    .ch            = 0u,                    /* the CAN channel this one serves */
    .ext_id        = false,                 /* true = 29-bit identifiers */
    .fd_frames     = false,                 /* true = CAN FD; required above 8 */
    .brs           = false,                 /* true = bit-rate switch; needs FD */
    .pad_frames    = true,                  /* fill every frame out to 8 bytes */
    .pad_byte      = 0xCCu,                 /* what goes in the unused tail */
    .phys_rx_id    = 0x7E0u,                /* received physically addressed */
    .phys_tx_id    = 0x7E8u,                /* transmitted, and flow control */
    .func_rx_id    = 0x7DFu,                /* 1:N requests; 0 disables */
    .func_tx_id    = 0u,                    /* a tester fills its broadcast ID */
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
> whose `ch` or identifier width does not match the configuration, or whose ID
> matches neither receive ID. So a driver that leaves `ch` or `is_ext` clear on
> every received frame makes an instance configured otherwise drop all of its
> traffic; fill both flags in the driver's receive path, or leave `ch` and
> `ext_id` at the values the driver actually reports. Matching on `ch` sorts
> frames that reach the right instance; it does not make one `can_rx_queue`
> shareable — every frame popped is released whether it matched or not, so
> whichever instance runs first takes it and the others never see it. Give each
> instance its own receive queue, or demultiplex in the driver. Anything popped from
> `sdu_tx_queue` is a reference the consumer owns and must `nx_ref_msg_release()`
> once handled; forgetting to is a pool leak, not a use-after-free, because the
> block is only returned when the count reaches zero. Frames pushed to
> `can_tx_queue` are likewise the driver's to release after transmitting.

### nx_uds — ISO 14229 vocabulary

The enums, masks and structures the diagnostic modules share: service and response
identifiers, negative response codes, session types and the bitmask that names a set
of them, the phases a service handler is called for, and the handler's own contract.
Header-only, with no state and nothing to initialise.

- **Service identifiers and their responses** — `nx_uds_sid_t` names the services,
  `NX_UDS_SID_TO_POS_RSP()` and `NX_UDS_POS_RSP_TO_SID()` convert between a request
  identifier and the positive response that answers it, and `NX_UDS_NEG_RSP_SID`
  with `NX_UDS_NEG_RSP_LEN` describe the three-byte refusal.
- **Response codes as an enum** — `nx_uds_nrc_t` covers the codes a server emits,
  with `NX_UDS_NRC_NONE` naming the absence of one so a zeroed field means nothing
  to report rather than code 0x00.
- **Sessions as a bitmask** — a session's bit is its own value, so a service row
  names the sessions it is available in with one `uint32_t`. `NX_UDS_SESSION_BIT()`
  builds one, `NX_UDS_SESSION_MASK_ALL` and `NX_UDS_SESSION_MASK_NON_DEFAULT` cover
  the common sets, and `NX_UDS_SESSION_MAX` bounds what a mask reaches.
- **The suppression bit** — `NX_UDS_SUPPRESS_POS_RSP_BIT`,
  `NX_UDS_SUPPRESSES_POS_RSP()` and `NX_UDS_SUB_FUNCTION()` read and strip bit 7 of
  a sub-function byte, which asks for a positive response to go unsent.
- **The handler contract** — `nx_uds_ctx_t` is a transaction as its handler sees it:
  the request and its length, the sub-function with the suppression bit already
  removed, the session and unlocked level it arrived in, a response buffer with the
  response identifier already written, and a place to keep something across the
  phases of one transaction. `nx_uds_phase_t` names why the handler is being called
  and `nx_uds_disposition_t` what it decided.
- **The service table row** — `nx_uds_service_t` describes one service as data: its
  identifier, its handler, the sessions and security level it needs, the
  sub-functions it has and optionally the sessions each of those is available in, and
  the length window its requests fall in.

The SIDs are grouped by the ISO 14229-1 functional unit they belong to. Which
module carries the handler for each is data, not code: the small stateless set lives
in `nx_uds_svc_session`, the security-access handshake carries its own state machine
in `nx_uds_svc_sec`, and the transfer sequence shares one in `nx_uds_svc_transfer`.
The rest have no dedicated module — the caller binds them with a `nx_uds_service_t`
row and a handler of its own.

| SID | Service | Functional unit (ISO 14229-1:2020) | Module |
|---|---|---|---|
| 0x10 | DiagnosticSessionControl | 10 Diagnostic and communication management | `nx_uds_svc_session` |
| 0x11 | ECUReset | 10 Diagnostic and communication management | `nx_uds_svc_session` |
| 0x27 | SecurityAccess | 10 Diagnostic and communication management | `nx_uds_svc_sec` |
| 0x28 | CommunicationControl | 10 Diagnostic and communication management | — |
| 0x3E | TesterPresent | 10 Diagnostic and communication management | `nx_uds_svc_session` |
| 0x83 | AccessTimingParameter | 10 Diagnostic and communication management | — |
| 0x84 | SecuredDataTransmission | 10 Diagnostic and communication management | — |
| 0x85 | ControlDTCSetting | 10 Diagnostic and communication management | — |
| 0x86 | ResponseOnEvent | 10 Diagnostic and communication management | — |
| 0x87 | LinkControl | 10 Diagnostic and communication management | — |
| 0x22 | ReadDataByIdentifier | 11 Data transmission | — |
| 0x23 | ReadMemoryByAddress | 11 Data transmission | — |
| 0x24 | ReadScalingDataByIdentifier | 11 Data transmission | — |
| 0x2A | ReadDataByPeriodicIdentifier | 11 Data transmission | — |
| 0x2C | DynamicallyDefineDataIdentifier | 11 Data transmission | — |
| 0x2E | WriteDataByIdentifier | 11 Data transmission | — |
| 0x3D | WriteMemoryByAddress | 11 Data transmission | — |
| 0x14 | ClearDiagnosticInformation | 12 Stored data transmission | — |
| 0x19 | ReadDTCInformation | 12 Stored data transmission | — |
| 0x2F | InputOutputControlByIdentifier | 13 Input/output and routine control | — |
| 0x31 | RoutineControl | 13 Input/output and routine control | — |
| 0x34 | RequestDownload | 15 Upload and download | `nx_uds_svc_transfer` |
| 0x35 | RequestUpload | 15 Upload and download | `nx_uds_svc_transfer` |
| 0x36 | TransferData | 15 Upload and download | `nx_uds_svc_transfer` |
| 0x37 | RequestTransferExit | 15 Upload and download | `nx_uds_svc_transfer` |
| 0x38 | RequestFileTransfer | 15 Upload and download | — |

```c
#include "nx_uds.h"

/* One service, described as data: read a data identifier, available in every
 * session, needing no unlock, with no sub-function and a fixed request length. */
static nx_uds_disposition_t read_did(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;

    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    /* out[0] already holds 0x62; append the identifier asked for and one byte. */
    ctx->out[1]  = ctx->req[1];
    ctx->out[2]  = ctx->req[2];
    ctx->out[3]  = 0x5Au;
    ctx->out_len = 4u;
    return NX_UDS_DISPOSITION_DONE;
}

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_READ_DATA_BY_IDENTIFIER,
        .handler      = read_did,
        .user         = NULL,
        .flags        = 0u,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .sec_level    = 0u,
        .min_len      = 3u,
        .max_len      = 3u
    }
};
```

> **Note:** a session's bit is the session's own value, so a mask reaches
> `NX_UDS_SESSION_MAX` and no further. The sessions ISO 14229 defines are all well
> inside that, but the manufacturer and supplier ranges run to 0x7E and cannot be
> named in a mask — `NX_UDS_SESSION_BIT()` yields no bit for them rather than
> shifting by the width of the type, so a row listing one is a row no session
> matches. A mask of 0 names no session at all and is refused at init, so a
> zero-initialised row cannot be silently dead.

### nx_uds_server — ISO 14229 diagnostic server (ECU side)

The ECU side of an ISO 14229 conversation: it takes a request A_PDU, finds the
service that implements it, and produces the response A_PDU. What carried the
request is not known to this module — a request enters through
`nx_uds_server_indicate()` as plain bytes plus how it was addressed, and a
response leaves through a callback the application wires to whatever it is
speaking over. Each instance serves one conversation.

What the server owns is what every service shares. The set of services is the
application's, held as a table of `nx_uds_service_t` rows; a service is added by
writing a handler and adding a row, and nothing in this module is edited. On top
of that dispatch the server owns the session and the S3 timer that drops it, the
response timing ISO 14229-2 prescribes, the pending notification that extends a
slow transaction, and the negative responses that belong to no service.

- **One transaction at a time** — a request is accepted, run to its response, and
  only then is the next one taken. `nx_uds_server_indicate()` reports `ERR_BUSY`
  rather than starting a second, and a handler that needs several cycles keeps the
  transaction until it is finished.
- **Response timing driven from the config** — the server holds itself to P2, P2*
  and P4, all in microseconds, emits the pending notification that keeps the
  client waiting past P2, and gives the transaction up (with the configured
  `p4_nrc`) on whichever of P4 or `max_pending` is reached first.
- **The session as a resource** — the server tracks the active session and the
  unlocked security level, drops a non-default session after `s3_us` of quiet, and
  restarts that timer on every request it accepts.
- **Caller-provided buffers only** — the request is copied into `req_buf` for as
  long as its transaction runs, and the response is assembled in `out_buf`; a
  handler that must span several cycles reads the one buffer and writes the other.
  Nothing is allocated.
- **A request that matches no service still gets an answer** — it starts a
  transaction that produces a negative response, so the refusal is sent the same
  way a positive one is.

The server exposes the pieces a service needs to speak to it: `_session` and
`_sec_level` report the state a request arrived in, `_set_session` and
`_set_sec_level` change it, `_touch` restarts the quiet timer, `_timing`, `_now`
and `_apdu_limits` publish what the server holds itself to, and `_is_busy` asks
whether a transaction is running.

```c
#include "nx_uds.h"
#include "nx_uds_server.h"

static bool send_response(void *user, uint8_t link, const uint8_t *rsp,
                          uint32_t len, uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;
    return can_send(myself, rsp, len);   /* queued, or false to retry later */
}

static nx_uds_disposition_t read_did(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;

    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    ctx->out[1] = ctx->req[1];   /* the identifier asked for */
    ctx->out[2] = ctx->req[2];
    ctx->out[3] = 0x5Au;         /* one byte of data */
    ctx->out_len = 4u;
    return NX_UDS_DISPOSITION_DONE;
}

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_READ_DATA_BY_IDENTIFIER,
        .handler      = read_did,
        .flags        = 0u,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .min_len      = 3u,
        .max_len      = 3u
    }
};

static uint32_t board_micros(void) { return timer_read_us(); }

static nx_uds_server_t srv;
static uint8_t req_buf[64];
static uint8_t out_buf[64];

static void server_setup(void)
{
    nx_uds_server_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.services       = services;
    cfg.services_count = 1u;
    cfg.out_fn         = send_response;
    cfg.req_buf        = req_buf;
    cfg.req_buf_size   = sizeof(req_buf);
    cfg.out_buf        = out_buf;
    cfg.out_buf_size   = sizeof(out_buf);
    cfg.max_req_apdu   = sizeof(req_buf);
    cfg.get_us         = board_micros;

    nx_uds_server_init(&srv, &cfg);
}
```

> **Note:** a request that arrives while a transaction is running is refused with
> `ERR_BUSY` and the running transaction is left strictly alone. The alternative —
> cancelling what is running to take the newcomer — loses the work in progress to
> any request at all, including a broadcast TesterPresent that was never addressed
> to this server in particular. The caller decides what to do with the refused
> request: answering it with 0x21 asks the client to send it again, and dropping
> it is also correct for one that was functionally addressed.

### nx_uds_svc_session — the always-needed service handlers

The three services a diagnostic server is expected to answer whatever else it
implements: 0x10 DiagnosticSessionControl, 0x11 ECUReset and 0x3E TesterPresent.
Each is an ordinary handler occupying an ordinary service table row, wired into a
table alongside the application's own services and reached the same way. Each
takes its own configuration struct through the row's `user` pointer, and nothing
here keeps state of its own.

- **0x10 SessionControl** — answers with the session echoed back and the response
  windows P2 and P2*, then enters the session once that answer has reached the
  client. The windows are read from the server rather than configured here, so
  what is announced is what is enforced. Entering a session relocks security,
  whichever session is entered.
- **0x11 ECUReset** — answers with the reset type echoed back, and 0x04
  additionally with the power-down time it was configured with, then performs the
  reset once that answer has reached the client. A reset carried out before its
  answer is sent looks to a client like a server that rebooted on its own.
- **0x3E TesterPresent** — echoes the sub-function and does nothing else; the
  server restarts its quiet timer on any request it accepts, and the service is
  what a client with nothing to ask sends to keep the session.

Each handler publishes what its row must declare, since a row that declares
something else is not corrected: a length bound the service does not expect, or a
sub-function list naming what the product cannot do, produces a server that
answers wrongly rather than one that refuses to start.

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_session.h"

static nx_uds_server_t srv;

static bool allow_session(void *user, uint8_t from, uint8_t to, uint8_t *nrc)
{
    (void)user; (void)from; (void)to; (void)nrc;
    return !driving_now();          /* refuse programming while driving */
}

static nx_uds_svc_session_cfg_t session_cfg = {
    .srv      = &srv,
    .allow_fn = allow_session,
};

static void do_reset(void *user, uint8_t reset_type)
{
    (void)user;
    if (reset_type == NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN) {
        power_down_requested = true;      /* 0x04 records, rather than resetting */
    } else {
        board_reset(reset_type);
    }
}

static nx_uds_svc_session_reset_cfg_t reset_cfg = {
    .do_fn           = do_reset,
    .power_down_time = 0xFEu,            /* no power-down time available */
};

static const uint8_t sessions[] = {
    NX_UDS_SESSION_DEFAULT, NX_UDS_SESSION_PROGRAMMING, NX_UDS_SESSION_EXTENDED,
};
static const uint8_t resets[] = {
    NX_UDS_RESET_HARD, NX_UDS_RESET_KEY_OFF_ON, NX_UDS_RESET_SOFT,
    NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN,
};
static const uint8_t tester_present_sub = NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB;

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler      = nx_uds_svc_session_control,
        .user         = &session_cfg,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = sessions,
        .subs_count   = 3u,
        .min_len      = 2u,
        .max_len      = 2u
    },
    {
        .sid          = NX_UDS_SID_ECU_RESET,
        .handler      = nx_uds_svc_session_ecu_reset,
        .user         = &reset_cfg,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = resets,
        .subs_count   = 4u,
        .min_len      = 2u,
        .max_len      = 2u
    },
    {
        .sid          = NX_UDS_SID_TESTER_PRESENT,
        .handler      = nx_uds_svc_session_tester_present,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = &tester_present_sub,
        .subs_count   = 1u,
        .min_len      = 2u,
        .max_len      = 2u
    },
};
```

> **Note:** 0x10 and 0x11 act in the CONFIRM/SILENCE phase rather than when they
> produce the answer, so the change or the reset happens only once the client has
> been told the request was accepted. What acts on a request at that point asks
> whether the response in the buffer is this service's positive one, so a refusal
> cannot be mistaken for the acceptance it replaced. A request whose positive
> response was suppressed is still acted on (at the point the answer would have
> been sent); a request whose answer never reached the link is not.

### nx_uds_svc_sec — 0x27 seed/key exchange

The seed/key exchange that unlocks a security level. Each level is a pair of
sub-functions — an odd one asking for a seed, the even one after it presenting a
key — and a level unlocked is what a service row's `sec_level` names. The
algorithm is not here: the application produces the seed and judges the key
through two callbacks, and this module never sees a secret or invents a random
number. What it owns is the sequence and the guessing defence.

- **The pair is in the level** — level *n* is sub-functions `NX_UDS_SVC_SEC_SEED_SUB(n)`
  and `NX_UDS_SVC_SEC_KEY_SUB(n)`, so level 1 is 0x01/0x02, level 2 is 0x03/0x04, up
  to `NX_UDS_SVC_SEC_MAX_LEVEL`. A level absent from the levels list does not exist.
- **Fixed byte counts per level** — each level declares how long its seed and its
  key are. The seed answer is exactly that long whether the seed was computed or
  the level was already unlocked; a key must be exactly the declared length.
- **A seed is spent once judged** — the same key cannot be presented twice, and a
  wrong key spends the seed so a second attempt starts by asking for a new one.
- **The waiting period** — a wrong key counts as an attempt; when the count reaches
  `max_attempts` the module starts a period of `delay_us` in which every 0x27
  request is refused with 0x37 without a callback being consulted. The count and
  the period outlast both a session change and a relock, and the pair
  `nx_uds_svc_sec_get_lockout()` / `nx_uds_svc_sec_set_lockout()` stores them somewhere
  that survives a power cycle.

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_sec.h"

static nx_uds_server_t srv;

static bool make_seed(void *user, uint8_t level, const uint8_t *record,
                      uint32_t record_len, uint8_t *seed, uint32_t seed_cap,
                      uint32_t *seed_len)
{
    (void)user; (void)level; (void)record; (void)record_len;
    (void)seed_cap;
    seed[0] = random_byte();
    *seed_len = 1u;
    return true;
}

static bool judge_key(void *user, uint8_t level, const uint8_t *seed,
                      uint32_t seed_len, const uint8_t *key, uint32_t key_len)
{
    (void)user;
    return level == 1u && seed_len == 1u && key_len == 1u
           && key[0] == (uint8_t)(seed[0] ^ 0x5Au);   /* just an example */
}

static const nx_uds_svc_sec_level_t levels[] = {
    { .level = 1u, .seed_len = 1u, .key_len = 1u },
};

static uint8_t seed_buf[4];

static nx_uds_svc_sec_t sec;
static void sec_setup(void)
{
    nx_uds_svc_sec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.srv          = &srv;
    cfg.levels       = levels;
    cfg.levels_count = 1u;
    cfg.seed_fn      = make_seed;
    cfg.verify_fn    = judge_key;
    cfg.seed_buf     = seed_buf;
    cfg.seed_buf_size = sizeof(seed_buf);

    nx_uds_svc_sec_init(&sec, &cfg);
}
```

> **Note:** a wrong key is counted as an attempt only when the request it arrived
> in was the right shape. A key presented with no seed outstanding is answered with
> 0x24 (request out of sequence) and not counted, and a seed a client asked not to
> be told is not issued — there is nothing to count against. The count is across
> every level together, so a client working through levels one at a time is one
> client, and reaching the limit is what starts the waiting period.

### nx_uds_svc_transfer — moving a block of memory

The memory transfer services: 0x34 RequestDownload, 0x35 RequestUpload, 0x36
TransferData and 0x37 RequestTransferExit. Four handlers occupying four ordinary
service table rows, plus the state one transfer needs — which direction it runs
in, the region it covers, how far it has got, and which block is expected next.
The memory is not here: the application reads and writes it through two
callbacks, and this module never touches an address itself.

- **Open, carry, close** — a transfer is opened by 0x34 (the client writes memory)
  or 0x35 (the client reads it), carried by any number of 0x36 exchanges, and
  closed by 0x37. One runs at a time, and an open finished only when the whole
  declared region has moved.
- **The block length is announced** — the open answers with the length of one
  block, counted as the whole message rather than its payload alone, and drawn
  from what the server can carry. `nx_uds_svc_transfer_payload_room()` subtracts the two
  bytes of overhead the counter and identifier occupy.
- **A block arriving twice is not carried twice** — the same counter as the last
  committed block is answered again, from the same place at the same length, so a
  lost answer is recoverable rather than a doubled write. A counter that is
  neither the next nor the last is refused, and the transfer is left open for the
  client to try again.
- **The last block of an upload is short** — the server reads whatever is left of
  the declared region, and a download that would write past the end the client
  itself declared is refused.
- **Finishing is the application's say** — 0x37 calls the close callback before its
  answer is assembled, which is where a written image gets verified or a partition
  marked valid. It leaves the session, the unlocked level and the quiet timer
  alone: a client commonly runs several transfers in one session.

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_transfer.h"

static nx_uds_server_t srv;

static bool open_transfer(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t addr,
                          nx_uds_svc_transfer_addr_t size, uint8_t format, uint32_t *block_len,
                          uint8_t *nrc)
{
    (void)user; (void)format; (void)block_len; (void)nrc;
    return (dir == NX_UDS_SVC_TRANSFER_DOWNLOAD) && addr == FLASH_BASE && size > 0u;
}

static bool write_block(void *user, nx_uds_svc_transfer_addr_t addr, const uint8_t *data,
                        uint32_t len, uint8_t *nrc)
{
    (void)user; (void)addr; (void)data; (void)len; (void)nrc;
    return true;    /* flash_write_buffered(addr, data, len); */
}

static bool read_block(void *user, nx_uds_svc_transfer_addr_t addr, uint8_t *out,
                       uint32_t len, uint8_t *nrc)
{
    (void)user; (void)addr; (void)out; (void)len; (void)nrc;
    return true;    /* flash_read(addr, out, len); */
}

static bool close_transfer(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t done,
                           nx_uds_svc_transfer_addr_t size, const uint8_t *record,
                           uint32_t record_len, uint8_t *out, uint32_t out_cap,
                           uint32_t *out_len, uint8_t *nrc)
{
    (void)user; (void)dir; (void)done; (void)size; (void)record;
    (void)record_len; (void)out; (void)out_cap; (void)out_len; (void)nrc;
    return flash_checksum_ok();
}

static nx_uds_svc_transfer_t xfer;
static void xfer_setup(void)
{
    nx_uds_svc_transfer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.srv           = &srv;
    cfg.open_fn       = open_transfer;
    cfg.write_fn      = write_block;
    cfg.read_fn       = read_block;
    cfg.close_fn      = close_transfer;
    cfg.max_block_len = FLASH_WRITE_UNIT;

    nx_uds_svc_transfer_init(&xfer, &cfg);
}

/* Four rows carry the transfer; each has the same handle as user. */
static const nx_uds_service_t services[] = {
    { .sid = NX_UDS_SID_REQUEST_DOWNLOAD,  .handler = nx_uds_svc_transfer_request_download,
      .user = &xfer, .min_len = 5u, .max_len = 33u },
    { .sid = NX_UDS_SID_REQUEST_UPLOAD,    .handler = nx_uds_svc_transfer_request_upload,
      .user = &xfer, .min_len = 5u, .max_len = 33u },
    { .sid = NX_UDS_SID_TRANSFER_DATA,     .handler = nx_uds_svc_transfer_data,
      .user = &xfer, .min_len = 2u, .max_len = 0u },
    { .sid = NX_UDS_SID_REQUEST_TRANSFER_EXIT,
      .handler = nx_uds_svc_transfer_exit, .user = &xfer,
      .min_len = 1u, .max_len = 0u },
};
```

> **Note:** the byte after the 0x36 identifier is a block sequence counter, not a
> sub-function. The row must NOT declare `NX_UDS_SVC_HAS_SUB_FUNCTION`, since
> reading that byte's top bit as a request for silence would answer half of every
> transfer with nothing. The row's `max_len` is 0 — the real ceiling is the length
> that was announced, and a block over it is refused as out of range rather than
> as the wrong length.

### nx_uds_tp_bind — joining the server to a transport

The few dozen lines between a diagnostic server and a transport that speaks
`nx_tp_sdu_t`. A transport publishes what it received, and what it finished
sending, as reference-counted messages on a queue, and reads what it should send
from another; this module is what moves those messages in and out of the server.
It is transport-agnostic — the two queues and the pool are all it sees, so the
same code joins the server to any transport that fills an `nx_tp_sdu_t`. One
instance per path.

- **The mechanics that are easy to get wrong by hand** — a received message is
  released as soon as the server has copied it, a response the transport cannot
  take yet is left for the server to offer again rather than dropped, a request
  arriving while one is already being answered keeps the session alive instead of
  letting it lapse, and every response published is addressed to the one client
  that is owed it rather than to the whole link.
- **One step per pass** — `nx_uds_tp_bind_process()` takes at most one message off
  the inbound queue, because a server answers one request at a time and a second
  would be refused rather than queued. The server is not pumped here; what to
  drive, and in what order, is the application's to arrange.
- **A pool of its own** — the pool is per path, not shared, so a flood on one path
  cannot starve another.
- **What was lost is counted** — the stats report every message that was discarded
  rather than answered, so a path whose counters climb is failing quietly.

The binding installs itself as the server's output path, so the server's own
`out_fn` and `out_user` are overwritten. Initialise the server first, then bind it.

```c
#include "nx_queue.h"
#include "nx_ref_msg.h"
#include "nx_tiered_mem_pool.h"
#include "nx_tp_sdu.h"
#include "nx_uds_server.h"
#include "nx_uds_tp_bind.h"

static nx_uds_server_t srv;
static nx_tiered_mem_pool_t pool;
static nx_queue_t sdu_in_q, sdu_out_q;

static nx_uds_tp_bind_t bind;
static void bind_setup(void)
{
    nx_uds_tp_bind_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.srv         = &srv;
    cfg.sdu_in      = &sdu_in_q;   /* what the transport published */
    cfg.sdu_out     = &sdu_out_q;  /* what the transport reads to send */
    cfg.pool        = &pool;
    cfg.link        = 1u;
    cfg.max_sdu_len = 4096u;

    nx_uds_tp_bind_init(&bind, &cfg);
}
```

> **Note:** a response is always physically addressed, however the request came
> in. A transport reads the `ta_type` off a message it is asked to send to select
> the CAN identifier, so propagating the request's functional addressing would put
> the response on the address a broadcast arrived on — which on an ECU is 0, the
> address nobody listens to. The binding writes `NX_TP_TA_PHYSICAL` into every
> outbound response for exactly that reason.

### nx_uds_client — ISO 14229 diagnostic client (the test tool side)

The other half of an ISO 14229 conversation: it takes a request A_PDU and reports
the outcome — the response A_PDU that came back, or why none did. What carries the
request is not known to this module; a request leaves through a callback the
application wires to whatever it is speaking over, and a response enters through
`nx_uds_client_indicate()` as plain bytes plus how it was addressed. Each instance
runs one transaction at a time, so the client is a test tool that asks and waits
rather than a peer that streams.

A transaction is a question and the wait for its answer. The client offers the
request to the send path, waits P2 for a response, and if the server says the
answer is still coming — a negative response carrying 0x78 responsePending — waits
P2* and keeps going, up to a bounded number of extensions. The wait windows are the
server's to set: a 0x10 positive response publishes P2 and P2*, and the client
adopts them for the conversation unless it is configured with `fixed_timing`, in
which case it always uses its own values.

- **One transaction at a time** — a request is armed, run to its outcome, and only
  then is the next one taken. `nx_uds_client_request()` reports `ERR_BUSY` while a
  transaction is in flight.
- **The outcome is reported, not just the bytes** — the result callback names how
  the transaction ended: a positive response, a refusal, a protocol error, a
  timeout, a cancellation, or silence where the request had asked for no positive
  response at all (the only silence that is not a failure).
- **The send path answers back** — what the carrier does with the request is
  reported through `nx_uds_client_confirm()`, so the client hears immediately if a
  request never got onto the link instead of waiting out the response window. And
  where the carrier cannot take a request yet, the client offers the same one again
  on the next pass rather than dropping it — but only until `send_timeout_us`.
- **Timing the server sets** — the wait windows are the published values unless
  `fixed_timing` is set, so the client times out when the server says the response
  is late, not when the application guessed it would be.
- **Caller-provided buffers only** — the request in flight is kept in `req_buf` and
  the response that arrived is kept in `rsp_buf`, both caller-owned. Nothing is
  allocated.

The client exposes the state the application needs to drive a session:
`_is_busy` asks whether a conversation is running, `_session` reports the active
session, `_timing` reports the wait windows in use, and `_set_send` lets a binding
attach itself as the send path after the client is initialised.

```c
#include "nx_uds.h"
#include "nx_uds_client.h"

static bool send_request(void *user, uint8_t link, const uint8_t *req,
                         uint32_t len, uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;
    return can_send(myself, req, len);   /* queued, or false to retry later */
}

static void report(void *user, nx_uds_client_t *clt, nx_uds_client_result_t result)
{
    (void)user;
    const uint8_t *rsp = clt->cfg.rsp_buf;
    uint32_t len = nx_uds_client_resp_len(clt);
    if (result == NX_UDS_CLIENT_RESULT_NEGATIVE) {
        reason_code = rsp[2];            /* the NRC that refused the request */
    }
}

static uint32_t board_micros(void) { return timer_read_us(); }

static nx_uds_client_t clt;
static uint8_t req_buf[16];
static uint8_t rsp_buf[64];

static void client_setup(void)
{
    nx_uds_client_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.result_fn     = report;
    cfg.send_fn       = send_request;
    cfg.req_buf       = req_buf;
    cfg.req_buf_size  = sizeof(req_buf);
    cfg.rsp_buf       = rsp_buf;
    cfg.rsp_buf_size  = sizeof(rsp_buf);
    cfg.link          = 1u;
    cfg.get_us        = board_micros;

    nx_uds_client_init(&clt, &cfg);
}

static void ask_session(void)
{
    /* 0x10 0x00, physically addressed: what session is this ECU in? */
    nx_uds_client_request(&clt, NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x00u, NULL, 0u,
                          NX_TP_TA_PHYSICAL);
    while (nx_uds_client_is_busy(&clt)) {
        nx_uds_client_process(&clt);   /* every main-loop iteration */
    }
}
```

> **Note:** `nx_uds_client_request()` does not send anything. It arms the request
> and the first `nx_uds_client_process()` call offers it to the send path, so
> calling `request()` and immediately reading `rsp_buf` will always see zero bytes.
> The send path taking the request also does not mean the transaction is shipping —
> it is over only when the outcome fires, which for most signals is in the result
> callback, and a request that asks for silence resolves without ever placing a
> response in the buffer.
