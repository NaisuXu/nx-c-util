# nx-c-util

[简体中文](/README_CN.md) | [English](/README.md)

## Brief

A utility library implemented in pure C, designed to provide simple and
convenient building blocks for embedded development.

Every component follows the same design philosophy:

- **Purely static** — all storage is provided by the caller; the library uses no
  dynamic memory and does not depend on `malloc`/`free`, making it suitable for
  heap-less targets.
- **Deterministic** — predictable, constant-time operations with no hidden
  overhead, well suited to real-time systems.
- **Portable** — standard C11 with no platform-specific dependencies; builds and
  runs on Windows, Linux, and macOS alike.

## Directory Structure

```
nx-c-util/
├── core/         # Core building blocks (list, queue, ringbuf, timer, ref_msg, mem_pool, lock)
├── middleware/   # Protocol parsers and stacks (modbus_rtu, can_bus, future: modbus_rtu_slave, can_isotp)
├── algo/         # Algorithms (crc, sha256)
├── device/       # Platform-independent device drivers (future)
└── examples/
    └── basic/    # Basic component usage examples
```

All includes use directory prefixes: `#include "core/nx_list.h"`. Add `-I.` to
your compiler flags so the root directory is in the include path.

## Modules

### nx_list — intrusive doubly-linked circular list

A header-only intrusive list (Linux `list_head` style). Where `nx_queue` tracks
standalone elements by copying them, `nx_list` embeds a link node directly in
the user's struct, so adding an item to the list only moves pointers — no copy,
no allocation, and the user struct stays exactly where it was allocated. A
doubly-linked circular layout (the sentinel head forms a ring) means inserts and
deletes have no head/tail special cases.

- **Intrusive** — the user embeds `nx_list_t` in their struct; `nx_list_entry`
  (a `container_of` macro) recovers the containing struct from the link.
- **Doubly-linked circular** — a sentinel head's `next` points to the first real
  node and `prev` to the last, forming a ring. Empty is `head->next == head`.
- **Symmetric add/del** — insert after any position with `nx_list_add`; delete a
  node from anywhere without knowing the head. Head and tail insertion are
  trivial wrappers (`nx_list_add_head` / `nx_list_add_tail`).
- **Safe iteration** — `nx_list_for_each` for read-only traversal,
  `nx_list_for_each_safe` for deletion during iteration (saves `next` before
  invoking the body, so the current node can be deleted without breaking the
  loop).
- **Zero allocation** — every node lives in the caller's storage; the list
  itself is just link pointers.
- **Header-only** — all operations are `static inline`.

```c
#include "core/nx_list.h"

typedef struct task {
    int         id;
    const char *name;
    nx_list_t   link;       /* embedded link node */
} task_t;

nx_list_t head;
nx_list_init(&head);

task_t t1 = {1, "Init", {NULL, NULL}};
task_t t2 = {2, "Run",  {NULL, NULL}};

nx_list_add_tail(&head, &t1.link);   /* add at tail */
nx_list_add_tail(&head, &t2.link);

nx_list_t *pos;
nx_list_for_each(pos, &head) {
    task_t *t = nx_list_entry(pos, task_t, link);   /* recover containing struct */
    printf("Task %d: %s\n", t->id, t->name);
}

nx_list_del(&t1.link);   /* remove from anywhere */
```

### nx_queue — generic ring-buffer (FIFO) queue

A fixed-capacity FIFO queue backed by a caller-provided buffer.

- **Generic element type** — stores elements of any size, measured in bytes
  (`element_size`).
- **Fixed capacity** — capacity is set at init time and never grows at runtime.
- **Full-queue policy** — choose per queue how a push behaves when full:
  `NX_QUEUE_ON_FULL_REJECT` (reject the new element) or
  `NX_QUEUE_ON_FULL_OVERWRITE` (drop the oldest element and keep the newest).
- **SPSC-friendly** — in a single-producer/single-consumer scenario (one side
  only pushes, the other only pops) it is naturally thread-safe; other
  concurrent access requires caller-side locking.
- **Helpers** — `push` / `pop` / `peek` / `clear` / `size` / `capacity` /
  `is_empty` / `is_full`.

```c
#include "core/nx_queue.h"

int        storage[4];            /* caller-owned backing storage */
nx_queue_t q;

/* capacity 4, reject new elements when full */
nx_queue_init(&q, storage, sizeof(int), 4, NX_QUEUE_ON_FULL_REJECT);

for (int i = 0; i < 5; i++) {
    nx_queue_push(&q, &i);        /* the 5th push returns NX_QUEUE_ERR_FULL */
}

int v;
while (nx_queue_pop(&q, &v) == NX_QUEUE_OK) {
    /* drains 0, 1, 2, 3 in FIFO order */
}
```


### nx_ringbuf — byte-oriented ring buffer

A byte-stream FIFO backed by a caller-provided buffer. Where `nx_queue` stores
fixed-size *elements* with all-or-nothing push/pop, `nx_ringbuf` stores a raw
*byte stream*: transfers move a variable number of bytes and may be partial. That
is the natural fit for serial I/O (UART RX/TX) and other streaming data.

- **Byte stream, partial transfers** — `write` / `read` / `peek` / `discard`
  operate on byte counts and return how many bytes were actually moved; a write
  that does not fully fit (or a read with fewer bytes available) transfers what
  it can rather than failing. No overwrite of unread data.
- **Fixed capacity** — capacity is set at init time and never grows; the whole
  buffer is usable (no reserved slot).
- **DMA-friendly** — `peek_linear` exposes the largest physically contiguous
  *readable* region and `poke_linear` the largest contiguous *writable* region,
  so a DMA engine can read from or write to the ring buffer directly; commit a
  direct fill with `nx_ringbuf_commit`, consume a direct read with
  `nx_ringbuf_discard`. No bounce buffer needed.
- **SPSC-friendly** — with one writer and one reader it is naturally thread-safe
  on a single core; other concurrent access requires caller-side locking (see
  `nx_lock`). This module introduces no locks.
- **Helpers** — `size` / `capacity` / `free` / `is_empty` / `is_full` / `clear`.

```c
#include "core/nx_ringbuf.h"

uint8_t      storage[64];      /* caller-owned backing storage */
nx_ringbuf_t rb;
nx_ringbuf_init(&rb, storage, sizeof(storage));

/* stream in; a partial write is normal when nearly full */
size_t written = nx_ringbuf_write(&rb, "hello", 5);   /* -> 5 */

char out[8];
size_t got = nx_ringbuf_read(&rb, out, sizeof(out));  /* reads what's available */

/* zero-copy DMA transmit: hand the contiguous readable region to the DMA */
size_t seg;
const uint8_t *src = nx_ringbuf_peek_linear(&rb, &seg);
if (src != NULL) {
    /* dma_send(src, seg); */
    nx_ringbuf_discard(&rb, seg);      /* mark consumed once the DMA is done */
}
```


### nx_tiered_mem_pool — tiered static memory pool

A deterministic, fragmentation-free replacement for `malloc`/`free`, built from
several "tiers" of equally sized blocks carved out of one caller-provided buffer.

- **Bounded, predictable timing** — a request is rounded up to the smallest
  large-enough tier and served from its in-use bitmap. Free is O(1); allocation
  scans a small bitmap bounded by the tier's block count.
- **Zero per-block overhead** — blocks carry no header; the owning tier is found
  by address range on free. Since blocks store no internal pointer, block sizes
  down to a single alignment unit are allowed.
- **Built-in double-free detection** — freeing an already-free block returns
  `NX_TIERED_ERR_DOUBLE_FREE` instead of corrupting the pool, at O(1).
- **No fragmentation** — within a tier every block is identical.
- **Configurable fallback** — when the ideal tier is exhausted the pool falls
  back to a larger one; set `forbid_fallback` to restrict to the best-fit tier.
- **One-struct configuration** — buffer, tier list, and policy live in one
  `nx_tiered_mem_pool_cfg_t`; init reports the exact bytes needed, so you can
  oversize the buffer and shrink to fit. The buffer needs no particular alignment.
- **Built-in statistics** — per-tier block size, count, free count, and a
  peak-usage high-water mark.
- **Not thread-safe** — concurrent access must be locked by the caller.

```c
#include "core/nx_tiered_mem_pool.h"

/* no special alignment needed; oversize it and let init report the exact need */
static uint8_t mem[32 * 8 + 128 * 4];

nx_tiered_mem_pool_t     pool;
nx_tiered_mem_pool_cfg_t cfg = {
    .memory      = mem,
    .memory_size = sizeof(mem),
    .tiers       = {
        { 32, 8 },     /* 8 blocks of 32 bytes  */
        { 128, 4 },    /* 4 blocks of 128 bytes */
    },
    .tier_count  = 2,
    /* forbid_fallback omitted -> false: a request may fall back to a larger tier */
};

size_t required = 0;
nx_tiered_mem_pool_init(&pool, &cfg, &required);   /* required = exact bytes needed */

void *p = nx_tiered_mem_pool_alloc(&pool, 20);     /* served by the 32-byte tier */
/* ... use p ... */
nx_tiered_mem_pool_free(&pool, p);                 /* owning tier inferred from address */
```


### nx_ref_msg — reference-counted zero-copy messages

A message dispatch layer: a message is allocated once from a memory pool and
delivered to one or more queues. What a queue stores is a *pointer* to the
message, not a copy, so every consumer shares the same data — zero copy. A
reference count decides when the block goes back to the pool.

- **Single allocation** — the message header and its data are one contiguous
  block (the data is a flexible array member, aligned to `max_align_t`), so
  `alloc` and the final `free` are each a single pool operation.
- **Reference-counting convention** — `alloc` returns the message with a refcount
  of 1 (the *producer reference*); each successful publish does +1; each consumer
  does `release` (-1) when done. The block is returned to the pool when the count
  reaches 0. The producer must `release` once after publishing to give up its own
  reference — this also lets a message delivered to *no* queue be freed with no
  leak.
- **Multi-queue publish** — `publish` sends to one queue, `publish_multi` sends to
  a `NULL`-terminated array of queues in one call; both increment the refcount only
  on a successful enqueue, so a full queue never leaks a reference. The queue set is
  organized by the caller; this module keeps no subscription table.
- **Best-effort with honest reporting** — `publish_multi` keeps going after a full
  queue so the others still receive the message, and its return code reflects the
  aggregate: `OK` (all accepted), `PARTIAL` (some full), or `ERR_FULL` (a non-empty
  list where none accepted). `out_delivered` gives the count; `out_first_failed`
  gives the index of the first full queue (or the queue count if none failed).
- **Reject-only queues** — initialize carrier queues with `nx_ref_msg_queue_init`
  (element size is fixed to a message pointer). The full-queue policy is forced to
  reject, because overwriting would silently drop an enqueued message and leak its
  reference.
- **Not thread-safe** — the refcount is a plain counter; concurrent access must be
  locked by the caller.

```c
#include "core/nx_ref_msg.h"

/* one consumer queue (its buffer holds message pointers) */
nx_ref_msg_t *qbuf[4];
nx_queue_t    q;
nx_ref_msg_queue_init(&q, qbuf, 4);

/* producer: allocate from a pool, fill, publish, then release its reference */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, 16);   /* refcount = 1 */
memcpy(nx_ref_msg_data(m), payload, 16);

nx_queue_t *group[] = { &q, /* &q2, &q3, ... */ NULL };  /* NULL-terminated */
size_t delivered = 0;
nx_ref_msg_publish_multi(m, group, &delivered, NULL); /* refcount = 1 + delivered */
nx_ref_msg_release(m);                             /* give up producer reference */

/* consumer: pop the shared message, use it, release when done */
nx_ref_msg_t *got = NULL;
if (nx_queue_pop(&q, &got) == NX_QUEUE_OK) {
    /* ... read nx_ref_msg_data(got), nx_ref_msg_len(got) ... */
    nx_ref_msg_release(got);                       /* frees when the last ref is gone */
}
```

### nx_timer — software timer manager

A tick-based software timer manager built on `nx_list`. The caller drives it: a
monotonically increasing tick counter advances (from a hardware timer, RTOS
tick, or the main loop), and `nx_timer_process(mgr, now)` is called periodically
to fire whichever timers have expired. This module touches no hardware and
allocates nothing, so it works the same on bare metal, under an RTOS, or on a
PC.

- **Tick unit is caller-defined** — a tick is not milliseconds; whatever your
  source counts in (1 ms SysTick, 10 ms RTOS tick, microseconds on a PC) is the
  unit of every delay/period. `nx_timer_start(t, 100, 0)` means "fire 100 ticks
  from now".
- **One-shot and periodic** — a timer with `period = 0` fires once and stops; a
  timer with `period != 0` reloads and fires again every `period` ticks.
- **Callback context** — callbacks run inside `nx_timer_process`. Where you call
  it decides their context: call from the main loop for relaxed callbacks, or
  from the tick interrupt for tighter latency (then keep callbacks very short).
- **Overflow-safe** — ticks are `uint32_t` and wrap around; expiry is compared
  with a signed difference, so wrap is handled correctly as long as no single
  timer's delay or period exceeds `INT32_MAX` ticks (~24.8 days at 1 ms tick).
- **Zero allocation** — every timer lives in caller-owned storage; they are
  tracked on an intrusive list.
- **Not thread-safe** — serialize access yourself if timers are started/stopped
  from a different context than `process`.

```c
#include "core/nx_timer.h"

static void led_blink(nx_timer_t *t, void *arg) {
    int *state = (int *)arg;
    *state = !(*state);
    printf("LED %s\n", *state ? "ON" : "OFF");
}

nx_timer_mgr_t mgr;
nx_timer_t     timer;
int            led_state = 0;

nx_timer_mgr_init(&mgr);
nx_timer_init(&timer, led_blink, &led_state);

/* blink every 10 ticks, starting immediately */
nx_timer_start(&mgr, &timer, 0, 10);

/* in your tick ISR or main loop: */
for (uint32_t tick = 0; tick < 100; tick++) {
    nx_timer_process(&mgr, tick);   /* fires the callback at tick 0, 10, 20, ... */
}
```

### nx_lock — pluggable critical-section abstraction

The other modules are deliberately lock-free — they keep no locks and leave
synchronization to the caller. `nx_lock` is the recommended, portable way to
provide it: a tiny `enter` / `exit` function-pointer pair the caller fills in
with the primitive best suited to the target, then wraps around the short
compound operations that need protecting (a queue push/pop, a pool alloc/free, a
refcount change).

- **Mutual exclusion, not counting** — this is `enter` / `exit` (protect a data
  structure from a concurrent access), not `take` / `give` (wait for a resource).
  It is a symmetric pair that must nest correctly.
- **Save / restore for nesting** — `enter` returns an implementation-defined
  saved state that the matching `exit` is given back. On a bare-metal MCU `enter`
  typically saves the interrupt-enable state and disables interrupts, and `exit`
  restores exactly that — so a critical section nested in another does not
  wrongly re-enable interrupts on the inner exit.
- **Header-only, zero platform dependency** — `nx_lock_enter` / `nx_lock_exit`
  are `static inline` wrappers that just null-check and forward to the caller's
  function pointers; the library core is unchanged and still does no locking.
- **NULL is a no-op** — a NULL lock (or NULL `enter` / `exit`) returns 0 and does
  nothing, so the same call sites compile to nothing on a single-threaded build.

```c
#include "core/nx_lock.h"
#include "core/nx_queue.h"

/* Cortex-M bare metal: disable interrupts, saving/restoring PRIMASK */
static uintptr_t cm_enter(void *ctx) { (void)ctx; uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void      cm_exit (void *ctx, uintptr_t s) { (void)ctx; __set_PRIMASK((uint32_t)s); }

static const nx_lock_t g_lock = { cm_enter, cm_exit, NULL };

/* wrap the short compound operation, and only that */
uintptr_t s = nx_lock_enter(&g_lock);
nx_queue_push(&q, &item);
nx_lock_exit(&g_lock, s);
```

> **Note:** keep the protected region tiny — while inside it, interrupts (or
> preemption) are held off. Wrap only the O(1) operation, never the surrounding
> business logic. On a strict single-producer/single-consumer `nx_queue` you may
> not need a lock at all (see the `nx_queue` note above); on a multi-core MCU,
> disabling interrupts guards only the local core — use a real spinlock there.
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
#include "middleware/nx_can_bus.h"

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
#include "middleware/nx_modbus_rtu.h"

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


### nx_crc — CRC-8 / CRC-16 / CRC-32 checksums

Bit-wise CRC routines with no lookup tables, so there is nothing to size or
store and every call is deterministic.

- **Three layers** — named wrappers for the common standards; generic one-shot
  functions (`nx_crc8_compute` / `nx_crc16_compute` / `nx_crc32_compute`) taking
  the Rocksoft model parameters (polynomial, init, input/output reflection,
  final XOR) for any variant; and an incremental context API
  (`nx_crc_init` / `nx_crc_update` / `nx_crc_final`) for data that arrives in
  pieces — a chunked computation yields exactly the same result as the one-shot
  call.
- **Standard variants included** — CRC-8, CRC-8/ITU, CRC-8/ROHC, CRC-8/MAXIM;
  CRC-16 IBM/MAXIM/USB/MODBUS/CCITT/CCITT-FALSE/X25/XMODEM; CRC-32 and
  CRC-32/MPEG-2. Each is documented in the header with its parameters and its
  check value (the CRC of `"123456789"`).
- **Table-free** — a single bit-wise core handles every width and refin/refout
  combination, so no polynomial tables are compiled in; small code, no table RAM.
- **NULL-safe** — a NULL data pointer contributes no bytes (treated as a
  zero-length buffer) instead of dereferencing, and a NULL context is a no-op;
  storage is caller-owned and the library uses no dynamic memory.

```c
#include "algo/nx_crc.h"

const char *msg = "123456789";

/* a named standard variant */
uint16_t c1 = nx_crc16_modbus(msg, 9);      /* 0x4B37 */
uint32_t c2 = nx_crc32(msg, 9);             /* 0xCBF43926 */

/* any other variant via the generic function
 * (here: CRC-16/MODBUS spelled out explicitly) */
uint16_t c3 = nx_crc16_compute(msg, 9,
                               0x8005,      /* poly   */
                               0xFFFF,      /* init   */
                               true, true,  /* refin, refout */
                               0x0000);     /* xorout */
/* c3 == c1 */

/* the same CRC, fed in over several chunks */
nx_crc_ctx_t ctx;
nx_crc_init(&ctx, 16, 0x8005, 0xFFFF, true, true, 0x0000);
nx_crc_update(&ctx, msg, 4);                /* "1234"  */
nx_crc_update(&ctx, msg + 4, 5);            /* "56789" */
uint16_t c4 = (uint16_t)nx_crc_final(&ctx); /* == c1 */
```


### nx_sha256 — SHA-256 cryptographic hash

A pure-C SHA-256 (FIPS 180-4) implementation producing a 32-byte digest.

- **Two ways to hash** — a one-shot helper (`nx_sha256`) for a whole buffer, and
  an incremental context API (`nx_sha256_init` / `nx_sha256_update` /
  `nx_sha256_final`) for data that arrives in pieces; a chunked computation
  yields exactly the same digest as the one-shot call.
- **Fixed, caller-owned storage** — the running state is a single
  `nx_sha256_ctx_t` the caller places on the stack; no dynamic memory, no tables
  beyond the fixed round constants, fully deterministic.
- **NULL-safe** — a NULL data pointer contributes no bytes and a NULL context or
  digest pointer is a harmless no-op.
- **Plain hash, not a MAC** — for message authentication, build HMAC-SHA256 on
  top of it.

```c
#include "algo/nx_sha256.h"

uint8_t digest[NX_SHA256_DIGEST_SIZE];

/* one-shot */
nx_sha256("abc", 3, digest);
/* digest = ba7816bf 8f01cfea ... f20015ad */

/* the same digest, fed in over several chunks */
nx_sha256_ctx_t ctx;
nx_sha256_init(&ctx);
nx_sha256_update(&ctx, "a", 1);
nx_sha256_update(&ctx, "bc", 2);
nx_sha256_final(&ctx, digest);
```


## Usage

The library sources live in `src/` and can be dropped directly into your project
— just compile the `.c` files and add `src/` to your include path.

The `example/` directory contains runnable usage examples for every module,
driven through CMake so they build the same way on any platform.

### Build and run the examples

From the repository root:

```sh
cd example
cmake -S . -B build
cmake --build build
```

Then run the produced executable:

- **Linux / macOS**

  ```sh
  ./build/nx_c_util_examples
  ```

- **Windows (MinGW / MSYS)**

  ```sh
  ./build/nx_c_util_examples.exe
  ```

- **Windows (Visual Studio / MSVC)** — multi-config generators place the binary
  in a per-config subdirectory:

  ```sh
  ./build/Debug/nx_c_util_examples.exe
  ```

### Choosing a generator

`cmake -S . -B build` uses your platform's default generator, which is enough in
most cases. To pick one explicitly, pass `-G`:

```sh
# Windows, MinGW toolchain
cmake -S . -B build -G "MinGW Makefiles"

# Windows, Visual Studio 2022
cmake -S . -B build -G "Visual Studio 17 2022"

# Linux / macOS, Unix Makefiles
cmake -S . -B build -G "Unix Makefiles"

# Any platform with Ninja installed
cmake -S . -B build -G "Ninja"
```

CMake 3.10 or newer and a C11-capable compiler (GCC, Clang, or MSVC) are
required.

## License

This project is under the MIT licence, see the LICENSE file.
