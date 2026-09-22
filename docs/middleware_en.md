# Middleware Modules

## nx_can_bus — CAN / CAN FD frame structures and helpers

A header-only module that defines a generic in-memory CAN frame and a small set
of dependency-free helpers. It is intended for tools or adapters between a host
and a CAN bus, so each frame can include direction, channel, and error metadata
in addition to its payload.

- **Classic CAN and CAN FD** — one `nx_can_msg_t` covers both; the payload is a
  flexible array member, so the caller sizes storage to the actual length (up to
  64 bytes). Frame attributes (`is_ext`, `is_remote`, `is_fd`, `brs`, `esi`,
  `dlc`) are packed into a bitfield that also exposes a `flags.raw` word for fast
  copy/compare.
- **Host/tool direction and channel** — `dir` (see `nx_can_dir_t`) distinguishes
  `TX` (the host requests transmission), `RX` (the tool received a frame), and
  `TXR` (the tool reports completion of an earlier `TX`). `ch` is a 4-bit channel
  number in the range 0..`NX_CAN_MAX_CH`. Both fields are meaningful only in a
  tool or adapter that manages one or more CAN interfaces.
- **Error / result reporting** — an `is_err` flag plus a 4-bit `err_code` (see
  `nx_can_err_t`) share one encoding across both directions: on an `RX` frame it
  names an error frame's cause, on a `TXR` report it names why the transmit
  failed (bit / stuff / form / ack / crc error, arbitration lost, bus-off,
  timeout, overrun, ...).
- **DLC helpers** — `nx_can_dlc_to_len` and `nx_can_len_to_dlc` convert between a
  4-bit DLC and the actual byte length, handling the CAN FD sizes (12/16/20/24/
  32/48/64) as well as classic 0..8.
- **Header-only** — every helper is `static inline`; include the header without
  compiling or linking an additional source file.

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
> encode the individual fields into a defined external representation rather
> than copying the struct or its compiler-dependent `flags.raw` value.


## nx_modbus_rtu — Modbus RTU frame structures and CRC

This module provides in-memory representations of common Modbus RTU frames and
a table-driven CRC-16/MODBUS implementation. The frame structures map directly
to the wire format. The CRC implementation uses a lookup table and therefore
resides in a `.c` file.

- **Frame structs map 1:1 onto the wire** — every struct is made of `uint8_t`
  fields only, which gives the structures byte alignment without padding. A
  received byte buffer can therefore be cast to the corresponding frame type
  and parsed in place without packing pragmas. Adding a non-`uint8_t` field could
  introduce padding and would invalidate this direct mapping.
- **Covers the common frames** — fixed- and variable-length requests and
  responses (`nx_modbus_rtu_req_fix_t` / `req_var_t` / `rsp_fix_t` / `rsp_var_t`)
  plus the exception response (`nx_modbus_rtu_rsp_exc_t`). Function codes and
  exception codes have their own enums (`nx_modbus_fc_t`, `nx_modbus_exc_t`),
  which are transport-independent and would be shared by a future TCP module.
- **Wire order** — 16-bit fields (address, quantity, register values) are carried
  most-significant byte first and must be decoded explicitly, as shown below.
  The trailing CRC is little-endian (low byte first). In variable-length frames,
  the CRC has no named structure member; `nx_modbus_rtu_req_var_crc()` and
  `nx_modbus_rtu_rsp_var_crc()` return a pointer to it after the payload.
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


## nx_modbus_rtu_slave — event-driven RTU slave: frame → subscription dispatch

This module adds RTU framing and request dispatch to `nx_modbus_rtu`. It reads
bytes, identifies and validates complete frames, and routes each request to the
subscribed application modules. It contains no application-specific logic and
accesses hardware only through injected, non-blocking callbacks. Call
`nx_modbus_rtu_slave_process()` from the main loop to advance reception,
dispatch, and transmission.

- **Subscription dispatch** — each application module registers a function code
  and an inclusive address range. A matching request is enqueued without copying
  as a reference-counted `nx_ref_msg`. One function code can be split across
  modules by range, and one request can reach
  several subscribers at once. A full subscriber queue drops that copy; if *every*
  matching queue is full, the slave returns exception `0x06` (slave device busy).
  If at least one subscriber accepts the request, no exception is sent.
- **Structural validation, in exception order** — for a recognized request
  layout, the slave first checks whether a subscription supports the function
  (`0x01`), then validates quantity /
  byte_count / single-write value legality (`0x03`), then address containment
  (`0x02`). A dispatched request is therefore already well-formed; whether a value
  is operationally valid for a particular register remains the responsibility of
  the application module, which may enqueue an exception response.
- **Length-based framing** — every supported request length follows from its
  function code: 8 bytes for `0x01` through `0x06`, `9 + byte_count` for `0x0F`
  and `0x10`, and `13 + byte_count` for `0x17`. Reception therefore does not
  require an inter-character (T3.5) timer. After an invalid address, unknown
  function code, or bad CRC, the parser discards one byte and attempts to
  resynchronize. Because an unknown function code has no known frame length, it
  is discarded this way rather than answered with exception `0x01`. On
  transmission, a 3.5-character gap derived from `baud_rate` follows each frame.
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
- **Releasing an instance** — `nx_modbus_rtu_slave_deinit()` releases any pooled
  message retained during transmission, deasserts the direction pin, and returns
  the state machine to idle. Call it before reinitializing a running instance or
  removing one from service. It does not modify the response queue because queued
  messages retain queue-held references; the queue consumer must still dequeue
  and release them.
- **Reply helpers for application modules** — `nx_modbus_rtu_slave_reply_read()`
  builds a read response with a byte count,
  `nx_modbus_rtu_slave_reply_write()` builds a write confirmation, and
  `nx_modbus_rtu_slave_reply_exception()` builds an exception response. All three
  accept the memory pool, response queue, slave address, and function code
  directly. The write helper additionally accepts the 16-bit `addr` and `data`
  arguments in host byte order. For function codes `0x05` and `0x06`, `data` is
  the written value; for function codes `0x0F` and `0x10`, it is the written
  quantity. The
  helper encodes both `addr` and `data` in the RTU high-byte-first wire format.
  The application module therefore needs only the relevant decoded request
  fields, not a slave handle or the complete request frame. All helpers return an
  `nx_modbus_rtu_slave_ret_t` that names why a reply was not queued — `ERR_NOMEM` and
  `ERR_FULL` indicate resource exhaustion, `ERR_BROADCAST` is the expected result
  for a broadcast request, and `ERR_PARAM` indicates invalid arguments.
- **Caller-owned storage** — the RX framing buffer, tiered message pool, and shared
  response queue are all supplied by the caller; the module does not use the system
  heap. If the pool is exhausted, the response is dropped and the master eventually
  times out.

```c
#include "nx_modbus_rtu_slave.h"

/* one application module owns holding registers 0x0000..0x000F */
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
    .response_queue = &response_queue, /* application responses are enqueued here */
    .read           = uart_read,       /* injected non-blocking I/O */
    .write          = uart_write,
    .get_us         = board_micros,    /* is_busy NULL => write is blocking */
};
nx_modbus_rtu_slave_init(&slave, &cfg);

for (;;) {
    nx_modbus_rtu_slave_process(&slave);            /* RX dispatch + TX pump */
    valve_business(&valve_q, &response_queue, &pool);  /* dequeue requests, enqueue responses */
}
```

> **Note:** the slave validates *structure* (function `0x01`, value `0x03`, address
> `0x02`), not *meaning*. An application module still validates the values it
> is asked to write and may enqueue its own `0x03` exception response.
> Broadcasts (address 0) are dropped by default, leaving only unicast requests
> handled; with `accept_broadcast = true` they are dispatched but never answered —
> no response, no exception.



## nx_modbus_rtu_master — event-driven RTU master: queue → wire → subscription dispatch

This module adds request transmission and response dispatch to `nx_modbus_rtu`.
It dequeues requests from a shared queue, validates received responses, and
routes each response to the application module subscribed to its slave address.
It contains no application-specific logic and accesses hardware only through
injected, non-blocking callbacks. Call `nx_modbus_rtu_master_process()` from the
main loop to advance transmission and reception.

- **Subscription dispatch by slave address** — an application module owns the devices it
  talks to, so each entry in the subscription table claims a `slave_addr` and receives
  that device's responses (zero-copy, as a reference-counted `nx_ref_msg`) on its own
  queue. An optional `func` filter narrows an entry to one function code when two modules
  split a device; `func = 0` takes everything from that address. One response can reach
  several subscribers at once. A response that matches no subscription is
  discarded because a Modbus master does not reply to responses.
- **Timeouts belong to the application module** — the master transmits and
  dispatches frames but does not track outstanding requests. An application
  module must record when it sends a request and decide when to retry or stop
  waiting. An empty response queue does not by itself indicate failure.
- **Request builders** — one per function code (`nx_modbus_rtu_master_read_holding_regs`,
  `..._write_multiple_regs`, and so on) builds a well-formed frame, stamps its CRC and
  enqueues it. They require only the memory pool and request queue, so an
  application module does not need a master handle. Invalid requests, including
  out-of-range quantities, inconsistent byte counts, and broadcast reads, are
  rejected locally.
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
- **Response inspection** — a subscriber dequeues a complete ADU whose CRC has
  already been validated. `nx_modbus_rtu_master_rsp_is_exception()` detects an
  exception response and returns its exception code.
  `nx_modbus_rtu_master_rsp_data()` returns a pointer to the payload of a read
  response and its length without copying the data.
- **Releasing an instance** — `nx_modbus_rtu_master_deinit()` releases any pooled
  message retained during transmission, deasserts the direction pin, and discards
  a partial response. It does not modify the request queue because queued messages
  retain queue-held references; the queue consumer must still dequeue and release
  them.

```c
#include "nx_modbus_rtu_master.h"

/* two application modules, one device each */
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
    .request_queue = &request_queue, /* application modules enqueue requests here */
    .read          = uart_read,      /* injected non-blocking I/O */
    .write         = uart_write,
    .get_us        = board_micros,   /* is_busy NULL => write is blocking */
};
nx_modbus_rtu_master_init(&master, &cfg);

/* an application module requests 10 registers; the response enters its queue */
nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue, 0x11u, 0x0000u, 10u);

for (;;) {
    nx_modbus_rtu_master_process(&master);        /* TX pump + RX dispatch */
    pump_business(&q_pump, &request_queue, &pool);   /* dequeue responses, enqueue requests */
}
```

> **Note:** RTU frames contain no transaction identifier, so a response can be
> matched only by slave address and function code. Responses to two simultaneous
> requests with the same slave address and function code cannot be distinguished.
> An application that requires strict pairing should keep at most one request per
> slave in flight. A late response from an expired request is likewise
> indistinguishable from a response to the current request.


## nx_tp_sdu — transport-layer service data unit

This header defines the service data unit exchanged between a diagnostic
transport and its upper layer. An SDU contains a complete payload together with
transport-independent metadata such as addressing type, link identifier, message
kind, and result. Every compatible transport uses the same structure.

- **One allocation per message** — the flexible-array payload follows the SDU
  header in the same pooled block. Queues therefore transfer one message pointer
  without copying the payload.
- **Addressing** — `ta_type` records physical (one-to-one) or functional
  (one-to-many) addressing; see `nx_tp_ta_type_t`. The upper layer uses it to
  determine whether a response is allowed or expected.
- **Connection tag** — `link` is an application-defined transport-path identifier.
  The transport copies its configured value into each published SDU without
  interpreting it, allowing one upper layer to distinguish multiple paths that
  share a queue.
- **Message kind** — `kind` distinguishes a received-message indication from a
  transmission result on the same output queue; see `nx_tp_sdu_kind_t`.
- **Result** — `result` reports how an operation ended (see `nx_tp_result_t`):
  completed, one of the timeouts, a sequence-number or flow-control error, a peer
  that asked to wait more times than are tolerated, or a message that did not fit
  the space available to receive it. The enumerators are listed in the priority
  order they are resolved in, so values may be compared.
- **32-bit length** — `len` counts payload bytes. How long a message may be is
  bounded by the configuration and the memory behind it, never by the width of
  this field.
- **Types only** — no source file must be compiled or linked. A zero-initialized
  SDU represents a successful, physically addressed indication, so producers
  need to set only fields that differ from those defaults.

```c
#include "nx_tp_sdu.h"

/* a request handed to a transport: it sends len bytes taken from data */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, sizeof(nx_tp_sdu_t) + 2u);
nx_tp_sdu_t  *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
s->len     = 2u;
s->data[0] = 0x22u;
s->data[1] = 0xF1u;

/* kind determines whether this is received data or a transmission result */
const nx_tp_sdu_t *in = (const nx_tp_sdu_t *)nx_ref_msg_data(msg);
if (in->kind == NX_TP_SDU_INDICATION) {
    handle(in->link, in->ta_type, in->data, in->len);   /* a message arrived */
} else if (in->result != NX_TP_N_OK) {
    report(in->link, in->result);                       /* a send ended badly */
}
```

> **Note:** direction is implied by the queue and is not stored in the SDU. A
> transport dequeues send requests from one queue and enqueues received messages
> and transmission results to another. On the output queue, `kind` distinguishes
> a received-message indication from a transmission result; `result` applies to
> the latter.


## nx_can_isotp — ISO 15765-2 (DoCAN / ISO-TP) segmented transport

This queue-based transport carries protocol data units that may exceed one CAN
frame. It dequeues received CAN frames, reassembles complete SDUs, and enqueues
them for the upper layer. In the opposite direction, it segments outbound SDUs
and enqueues paced CAN frames for the driver. All input and output objects are
reference-counted `nx_ref_msg` instances stored in caller-provided queues. The
module does not access CAN hardware directly; call `nx_can_isotp_process()` to
advance both receive and transmit processing.

- **Tester and ECU operation** — one instance can receive diagnostic requests
  and send responses, or send requests with `nx_can_isotp_send()` and receive the
  corresponding responses. Both directions may be active on the same instance.
- **ISO-TP frame types** — the implementation supports single frames, including
  the 2016 length escape for payloads longer than 7 bytes; first frames with the
  12-bit length and the 32-bit escape for lengths above 4095; consecutive frames
  with 4-bit sequence numbers; and flow-control frames with `CTS`, `WAIT`, or
  `OVERFLOW`, block size, and `STmin` fields.
- **Physical and functional addressing** — `phys_rx_id` selects physically
  addressed frames received by the instance. `phys_tx_id` is used for transmitted
  physical frames and flow-control frames. Optional `func_rx_id` and `func_tx_id`
  values enable functional reception and transmission. Functional traffic is
  limited to single frames because a shared functional identifier cannot support
  per-receiver flow control. Received SDUs identify the addressing mode in
  `ta_type`. Identifiers are complete 11-bit or 29-bit CAN IDs; the module does
  not interpret identifier bit fields.
- **Protocol timing** — `n_as_us` and `n_ar_us` limit how long a locally generated
  frame may wait to enter `can_tx_queue` during transmission and reception,
  respectively. `n_bs_us` limits the wait for flow control, and `n_cr_us` limits
  the wait for the next consecutive frame. `n_wft_max` limits consecutive `WAIT`
  frames; each accepted `WAIT` restarts the N_Bs timer. A zero value selects the
  documented default for each setting.
- **Transmit-queue backpressure** — if `can_tx_queue` is full, the module retries
  the frame on later process calls until N_As or N_Ar expires. Expiration reports
  `N_TIMEOUT_A`. These timers start when enqueueing is first attempted, not when
  the driver puts the frame on the bus. An `OVERFLOW` flow-control frame is tried
  only once because reception has already been aborted.
- **Receive flow control** — `rx_block_size` and `rx_stmin` define the block size
  and minimum separation time advertised to the sender. The module emits another
  flow-control frame after each receive block.
- **Receive limits** — `rx_max_len` limits accepted SDU length. If a first frame
  announces a larger SDU, the module sends `OVERFLOW` before allocating memory.
  Setting `rx_max_len` to 0 uses the capacity available from the memory pool,
  subject to `NX_CAN_ISOTP_MAX_MSG_LEN`.
- **Absolute length limit** — `NX_CAN_ISOTP_MAX_MSG_LEN` is the smaller of the
  `size_t` limit and the 32-bit wire-format limit, minus the platform-dependent
  pooled-message headers. It is therefore just under 4 GiB when `size_t` is at
  least 32 bits. Longer outbound SDUs return `NX_CAN_ISOTP_ERR_LENGTH`; longer
  incoming SDUs are rejected with `OVERFLOW` before allocation. The practical
  limit is normally lower and depends on `rx_max_len` and the configured memory
  pool.
- **Result reporting** — when `confirm_tx` is enabled, completed transmissions
  and failed receptions produce result SDUs. `result` identifies timeout,
  sequence, flow-status, wait-count, or length failures, while `kind`
  distinguishes results from received data indications.
- **Channel and frame format** — `ch` identifies the CAN channel and is copied to
  transmitted frames and checked on received frames. `ext_id`, `fd_frames`, and
  `brs` configure 29-bit IDs, CAN FD, and bit-rate switching. `ext_id` also
  participates in receive matching because standard and extended IDs with the
  same numeric value are distinct.
- **Frame capacity and padding** — `max_frame_len` must be 8, 12, 16, 20, 24, 32,
  48, or 64 bytes. Values above 8 require CAN FD, as does `brs`. When
  `pad_frames` is enabled, shorter classic CAN frames are padded to 8 bytes with
  `pad_byte`. CAN FD frames are always padded to the next representable DLC size.
- **Pacing and work limits** — the peer's `STmin` controls spacing between
  consecutive frames. `tx_frames_per_process` limits the number of frames that
  one call to `nx_can_isotp_process()` may enqueue. A zero value selects the
  implementation's maximum per-call budget of 255 frames.
- **Direct reassembly** — on receipt of a first frame, the module allocates one
  message-sized block and reassembles the SDU directly into it. The completed SDU
  is then enqueued for the upper layer without another payload copy.

```c
static _Alignas(max_align_t) uint8_t pool_mem[24u * 1024u];
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
if (nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL) != NX_TIERED_OK) {
    handle_configuration_error();
}
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
    .tx_frames_per_process = 1u,            /* 0 = up to 255 frames per call */
};
nx_can_isotp_init(&iso, &cfg);

for (;;) {
    can_driver_fill(&can_rx_q);   /* driver enqueues received nx_ref_msg frames */
    nx_can_isotp_process(&iso);   /* reassemble, flow control, segment, pace */
    can_driver_drain(&can_tx_q);  /* driver dequeues and transmits frames */

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

> **Note:** queue names use the ISO-TP module's point of view. The module
> dequeues from `sdu_rx_queue` and `can_rx_queue`, and enqueues to
> `sdu_tx_queue` and `can_tx_queue`. The upper layer therefore submits outbound
> SDUs to `sdu_rx_queue` and receives completed SDUs from `sdu_tx_queue`.
>
> `nx_can_isotp_process()` consumes every frame it dequeues from `can_rx_queue`.
> It also releases frames that do not match `ch`, identifier format, or either
> configured receive ID. The driver must therefore set `ch` and `is_ext`
> consistently with the instance configuration. A receive queue cannot be shared
> directly by multiple ISO-TP instances because the first instance to dequeue a
> frame consumes it even if it does not match; use one queue per instance or
> demultiplex frames in the driver. The consumer owns each message dequeued from
> `sdu_tx_queue` and must call `nx_ref_msg_release()` after processing it. The CAN
> driver has the same responsibility for frames dequeued from `can_tx_queue`.

## nx_uds — ISO 14229 vocabulary

This header defines the enums, masks, and structures shared by the diagnostic
modules. These include service and response identifiers, negative response
codes, session types and masks, service-handler phases, and the handler contract.
The module is header-only and contains no runtime state.

- **Service identifiers and their responses** — `nx_uds_sid_t` names the services,
  `NX_UDS_SID_TO_POS_RSP()` and `NX_UDS_POS_RSP_TO_SID()` convert between a request
  identifier and the positive response that answers it, and `NX_UDS_NEG_RSP_SID`
  with `NX_UDS_NEG_RSP_LEN` describe the three-byte refusal.
- **Negative response codes** — `nx_uds_nrc_t` defines the codes emitted by a
  server. `NX_UDS_NRC_NONE` means that no negative response code is present, so a
  zero-initialized field does not represent NRC 0x00.
- **Sessions as a bitmask** — a session's bit is its own value, so a service row
  names the sessions it is available in with one `uint32_t`. `NX_UDS_SESSION_BIT()`
  builds one, `NX_UDS_SESSION_MASK_ALL` and `NX_UDS_SESSION_MASK_NON_DEFAULT` cover
  the common sets, and `NX_UDS_SESSION_MAX` bounds what a mask reaches.
- **The suppression bit** — `NX_UDS_SUPPRESS_POS_RSP_BIT`,
  `NX_UDS_SUPPRESSES_POS_RSP()` and `NX_UDS_SUB_FUNCTION()` read and strip bit 7 of
  a sub-function byte, which asks for a positive response to go unsent.
- **Handler context** — `nx_uds_ctx_t` gives a handler the request and its length,
  the sub-function with the suppression bit removed, the active session and
  security level, a response buffer whose response SID is already initialized,
  and storage that persists across phases of the same transaction.
  `nx_uds_phase_t` identifies the current phase, and `nx_uds_disposition_t`
  reports the handler's result.
- **The service table row** — `nx_uds_service_t` describes one service as data: its
  identifier, its handler, the sessions and security level it needs, the
  sub-functions it has and optionally the sessions each of those is available in, and
  the length window its requests fall in.

The following table groups SIDs by their ISO 14229-1 functional unit. Built-in
handlers are provided for core session services, security access, and memory
transfer. The application implements other services by adding an
`nx_uds_service_t` entry with its own handler.

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

> **Note:** a session mask uses the session value as the bit index and is limited
> by `NX_UDS_SESSION_MAX`. Manufacturer- and supplier-specific session values up
> to 0x7E cannot be represented by this mask. `NX_UDS_SESSION_BIT()` returns 0
> for such values instead of performing an invalid shift. A service entry with a
> zero session mask is rejected during initialization.

## nx_uds_server — ISO 14229 diagnostic server (ECU side)

This module implements the ECU side of ISO 14229. It accepts a request A_PDU,
dispatches it to the matching service handler, and produces a response A_PDU.
The transport is supplied by the application: requests enter through
`nx_uds_server_indicate()`, and responses leave through the configured output
callback. Each server instance processes one transaction at a time.

The application defines available services in an `nx_uds_service_t` table. A
service is added by implementing a handler and adding a table entry. The server
provides the shared protocol behavior around those handlers, including session
and security state, the S3 session timer, P2/P2*/P4 response timing,
response-pending handling, and protocol-level negative responses.

- **One transaction at a time** — the server finishes the current request before
  accepting another. `nx_uds_server_indicate()` returns `ERR_BUSY` while a
  transaction is active. A handler may remain pending across multiple process
  calls until it completes.
- **Configured response timing** — the server enforces P2, P2*, and P4 in
  microseconds. It sends NRC 0x78 (response pending) when processing extends
  beyond P2. The transaction ends with `p4_nrc` when either the P4 limit or
  `max_pending` is reached.
- **Session state** — the server tracks the active session and unlocked security
  level. After `s3_us` without an accepted request, it returns a non-default
  session to the default session. Every accepted request restarts the S3 timer.
- **Caller-provided buffers only** — the request is copied into `req_buf` for as
  long as its transaction runs, and the response is assembled in `out_buf`; a
  handler that must span several cycles reads the one buffer and writes the other.
  Nothing is allocated.
- **Unsupported requests** — a request that matches no service still starts a
  transaction and produces a negative response through the normal output path.

The server accessors expose the state needed by service modules:
`_session` and `_sec_level` return the active session and security level;
`_set_session` and `_set_sec_level` update them; `_touch` restarts the S3 timer;
`_timing`, `_now`, and `_apdu_limits` return timing and buffer limits; and
`_is_busy` reports whether a transaction is active.

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

> **Note:** a request received during an active transaction returns `ERR_BUSY`
> without affecting that transaction. The caller decides how to handle the
> rejected request. For a physically addressed request, it may return NRC 0x21
> (busy repeat request); a functionally addressed request may be discarded.

## nx_uds_svc_session — core session and reset services

This module provides handlers for 0x10 DiagnosticSessionControl, 0x11 ECUReset,
and 0x3E TesterPresent. Add each required handler to the application's service
table like any other `nx_uds_service_t` entry. Handler-specific configuration is
passed through the entry's `user` pointer; the module itself stores no additional
global state.

- **0x10 DiagnosticSessionControl** — the positive response echoes the requested
  session and includes the server's P2 and P2* values. The server changes session
  only after the response is confirmed or intentionally suppressed. Entering any
  session also clears the unlocked security level.
- **0x11 ECUReset** — the positive response echoes the reset type. Sub-function
  0x04 also returns the configured power-down time. The reset callback runs only
  after the response is confirmed or intentionally suppressed, ensuring that a
  normal positive response can be transmitted before the ECU resets.
- **0x3E TesterPresent** — the positive response echoes the sub-function. The
  request also refreshes the server's S3 timer through normal request acceptance.

Each handler defines constants for the flags and length bounds required by its
service-table entry. The server does not correct inconsistent entries, so the
application must configure the entry with the supported sub-functions and the
handler's required request length.

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
    .power_down_time = 0xFFu,            /* no power-down time available */
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

> **Note:** handlers for 0x10 and 0x11 perform their side effects during the
> CONFIRM or SILENCE phase, and only when the prepared response is their positive
> response. A negative response therefore cannot trigger a session change or
> reset. A request with a suppressed positive response still performs the side
> effect in the SILENCE phase. If transmission of a required response fails, the
> side effect is not performed.

## nx_uds_svc_sec — 0x27 seed/key exchange

This module implements the state machine for UDS SecurityAccess (0x27). Each
security level uses an odd sub-function to request a seed and the following even
sub-function to submit a key. A protected service selects the required unlocked
level through its `sec_level` field. The application supplies callbacks to
generate seeds and verify keys; the module does not define the security
algorithm or generate random data.

- **Sub-function pairs** — level *n* uses
  `NX_UDS_SVC_SEC_SEED_SUB(n)` and `NX_UDS_SVC_SEC_KEY_SUB(n)`. Level 1 therefore
  uses 0x01/0x02, level 2 uses 0x03/0x04, and so on through
  `NX_UDS_SVC_SEC_MAX_LEVEL`. Only levels present in the configured level table
  are supported.
- **Configured seed and key lengths** — each level specifies fixed seed and key
  lengths. A seed response always contains the configured number of seed bytes,
  including when the level is already unlocked. A submitted key must match the
  configured key length.
- **Single-use seeds** — a seed is consumed when a key is verified. After an
  incorrect key, the client must request a new seed before submitting another
  key.
- **Attempt delay** — each correctly formed but incorrect key increments the
  attempt count. When the count reaches `max_attempts`, all 0x27 requests return
  NRC 0x37 for `delay_us` without invoking the application callbacks. Session
  changes and security relocking do not clear this state.
  `nx_uds_svc_sec_get_lockout()` and `nx_uds_svc_sec_set_lockout()` allow the
  application to persist and restore it across power cycles.

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

> **Note:** only a correctly formed request containing an incorrect key increments
> the attempt count. A key submitted without an outstanding seed returns NRC 0x24
> (request sequence error) and is not counted. If the seed response is suppressed,
> no seed is issued. The attempt count is shared across all configured security
> levels, and reaching the limit starts the delay period.

## nx_uds_svc_transfer — memory upload and download services

This module implements 0x34 RequestDownload, 0x35 RequestUpload, 0x36
TransferData, and 0x37 RequestTransferExit. The four handlers occupy separate
service-table entries and share one `nx_uds_svc_transfer_t` state object. The
state records the transfer direction, memory region, completed length, and next
block sequence counter. The application performs actual memory access through
callbacks.

- **Transfer sequence** — 0x34 opens a download from client to server, and 0x35
  opens an upload from server to client. One or more 0x36 requests transfer data,
  and 0x37 closes the transfer. Only one transfer may be active at a time.
- **Negotiated block length** — the opening response reports the maximum complete
  TransferData message length, including the service identifier and block
  sequence counter. The value is limited by server capacity, `max_block_len`, and
  any smaller value selected by `open_fn`.
  `nx_uds_svc_transfer_payload_room()` converts that complete message length to
  available payload length by subtracting the two overhead bytes.
- **Duplicate block handling** — if the client repeats the most recently
  committed block sequence counter, the module returns the same logical result
  without writing a download block twice. Upload retries read the same address
  and length again. Any counter other than the expected or previous value is
  rejected without closing the transfer.
- **Region bounds** — the last upload block contains only the bytes remaining in
  the declared region. A download block that would extend past that region is
  rejected.
- **Transfer completion** — before building the 0x37 response, the module invokes
  `close_fn` when one is configured. The callback can verify a downloaded image,
  commit a partition, or add response data. The transfer handler does not change
  the UDS session or security level. As with every accepted request, however,
  accepting 0x37 refreshes the server's S3 timer.

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
    cfg.require_full_size = true;

    nx_uds_svc_transfer_init(&xfer, &cfg);
}

/* All four entries share the same transfer state through user. */
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

> **Note:** the byte after SID 0x36 is a block sequence counter, not a
> sub-function. The TransferData entry must not set
> `NX_UDS_SVC_HAS_SUB_FUNCTION`; otherwise bit 7 of the counter would be treated
> as the suppress-positive-response bit. Set the entry's `max_len` to 0 because
> the negotiated block length is enforced by the transfer handler. An oversized
> block is then rejected as out of range instead of as an invalid request length.

## nx_uds_tp_bind — binding a UDS endpoint to a transport

This adapter connects either an `nx_uds_server_t` or an `nx_uds_client_t` to a
transport that exchanges `nx_tp_sdu_t` messages. The `side` setting selects the
endpoint. The adapter dequeues received SDUs and transmission results from the
transport's inbound queue, passes them to that endpoint, and enqueues the
endpoint's responses or requests for the transport. It depends only on the SDU
queues and a memory pool, so it can be used with any compatible transport. Create
one binding for each transport path.

- **Message ownership and retry** — the binding releases an inbound message after
  the endpoint has copied its data. If the outbound queue is full, the endpoint
  retains its frame and retries later. On the server side, requests received while
  the server is busy still refresh the session timer, and every response is
  emitted with physical addressing. On the client side, outbound requests retain
  the addressing type selected by the transaction.
- **One inbound message per call** — `nx_uds_tp_bind_process()` dequeues at most
  one transport message because either endpoint accepts only one active
  transaction. It does not process the server or client state machine; the
  application controls the order in which the binding, endpoint, and transport
  are processed.
- **Per-path memory pool** — each binding uses its configured pool. Assigning a
  separate pool to each transport path prevents traffic on one path from
  exhausting another path's memory.
- **Failure statistics** — `busy` and `refused` count inbound messages that were
  dropped. `no_memory`, `queue_full`, and `too_long` count failed outbound
  publication attempts. Because the endpoint retries an outbound frame, one frame
  may increment an outbound counter more than once. The counters expose sustained
  queue, memory, or configuration pressure.

The binding installs its output callback with `nx_uds_server_set_output()` on the
server side or `nx_uds_client_set_send()` on the client side. Initialize the
selected endpoint before initializing the binding.

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

    cfg.side        = NX_UDS_TP_SERVER;
    cfg.srv         = &srv;
    cfg.sdu_in      = &sdu_in_q;   /* what the transport published */
    cfg.sdu_out     = &sdu_out_q;  /* what the transport reads to send */
    cfg.pool        = &pool;
    cfg.link        = 1u;
    cfg.max_sdu_len = 4096u;

    nx_uds_tp_bind_init(&bind, &cfg);
}
```

> **Note:** on the server side, UDS responses are physically addressed even when
> the request was functionally addressed. The transport uses `ta_type` to select
> the destination, so the binding sets every outbound response to
> `NX_TP_TA_PHYSICAL`. On the client side, the binding preserves the request's
> configured addressing type.

## nx_uds_client — ISO 14229 diagnostic client (tester side)

This module implements the tester side of ISO 14229. It builds a request A_PDU,
submits it through an application-provided send callback, and reports either the
received response A_PDU or the reason the transaction ended without one. The
transport passes received bytes and their addressing type to
`nx_uds_client_indicate()`. Each client instance processes one transaction at a
time.

After the send path accepts a request, the client waits up to P2 for a response.
NRC 0x78 (response pending) changes the timeout to P2* and may extend it up to the
configured pending-response limit. A positive 0x10 response can update the P2 and
P2* values used for subsequent transactions. When `fixed_timing` is enabled, the
client always uses its configured timing values instead.

- **One transaction at a time** — after a request is initialized, the client
  processes it to completion before accepting another.
  `nx_uds_client_request()` returns `ERR_BUSY` while a transaction is active.
- **Explicit transaction results** — the result callback distinguishes positive
  and negative responses, protocol errors, timeouts, cancellation, and successful
  completion without a response when positive-response suppression was requested.
- **Transport confirmation** — the transport reports transmission completion or
  failure through `nx_uds_client_confirm()`. A failed transmission can therefore
  terminate immediately instead of waiting for P2. If the send callback cannot
  accept a request, the client retries on later process calls until
  `send_timeout_us` expires.
- **Server-provided timing** — unless `fixed_timing` is enabled, the client adopts
  P2 and P2* from a successful DiagnosticSessionControl response and uses those
  values for later response timeouts.
- **Caller-provided buffers only** — the request in flight is kept in `req_buf` and
  the response that arrived is kept in `rsp_buf`, both caller-owned. Nothing is
  allocated.

The client accessors expose the state needed by the application: `_is_busy`
reports whether a transaction is active, `_session` returns the current session,
`_timing` returns the active timeout values, and `_set_send` allows a transport
binding to install the send callback after client initialization.

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

static void enter_programming_session(void)
{
    /* 0x10 0x02, physically addressed: request the programming session. */
    nx_uds_client_request(&clt, NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                          NX_UDS_SESSION_PROGRAMMING, NULL, 0u,
                          NX_TP_TA_PHYSICAL);
    while (nx_uds_client_is_busy(&clt)) {
        nx_uds_client_process(&clt);   /* every main-loop iteration */
    }
}
```

> **Note:** `nx_uds_client_request()` only initializes the transaction. The first
> `nx_uds_client_process()` call submits the request to the send callback, so
> `rsp_buf` remains empty immediately after `nx_uds_client_request()`. Acceptance
> by the send callback also does not complete the transaction. Completion is
> reported through the result callback, except that a request with positive-
> response suppression may complete successfully without response data.
