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
- **Exception replies for business modules** — `nx_modbus_rtu_slave_reply_exception()`
  builds an exception response from the address and function code in the request frame
  and queues it. It takes only the pool and the response queue, so a business module
  needs no slave handle; a broadcast request builds no frame and returns false.
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



