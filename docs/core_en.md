# Core Modules

## nx_list — intrusive circular doubly linked list

A header-only intrusive list in the style of Linux `list_head`. Each containing
structure embeds its own link node, so insertion and removal update pointers
without copying or allocating the containing object. A sentinel head closes the
list into a ring, eliminating special cases at the head and tail.

- **Intrusive storage** — embed `nx_list_t` in the containing structure;
  `nx_list_entry` recovers that structure from its link node.
- **Circular, doubly linked layout** — the sentinel's `next` points to the first
  item and its `prev` points to the last. The list is empty when
  `head->next == head`.
- **Uniform insertion and removal** — `nx_list_add` inserts after any position,
  while `nx_list_del` removes a node without requiring the list head.
  `nx_list_add_head` and `nx_list_add_tail` provide the common wrappers.
- **Safe iteration** — `nx_list_for_each` for read-only traversal,
  `nx_list_for_each_safe` for deletion during iteration (saves `next` before
  invoking the body, so the current node can be deleted without breaking the
  loop).
- **Zero allocation** — every node lives in caller-owned storage; the list
  itself consists only of link pointers.
- **Header-only** — all operations are `static inline`.

```c
#include "nx_list.h"

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

## nx_queue — generic ring-buffer (FIFO) queue

A fixed-capacity FIFO queue backed by a caller-provided buffer.

- **Generic element type** — stores elements of any size, measured in bytes
  (`element_size`).
- **Fixed capacity** — capacity is set at init time and never grows at runtime.
- **Full-queue policy** — choose per queue how a push behaves when full:
  `NX_QUEUE_ON_FULL_REJECT` (reject the new element) or
  `NX_QUEUE_ON_FULL_OVERWRITE` (drop the oldest element and keep the newest).
- **Cooperative SPSC use** — one producer and one consumer can share a queue on
  a single core when neither can preempt the other. Both operations update the
  shared element count, so preemptive or otherwise concurrent access must be
  serialized by the caller, for example with `nx_lock`.
- **Helpers** — `nx_queue_push`, `nx_queue_pop`, `nx_queue_peek`,
  `nx_queue_clear`, and the size, capacity, empty, and full queries.

```c
#include "nx_queue.h"

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


## nx_ringbuf — byte-oriented ring buffer

A byte-stream FIFO backed by a caller-provided buffer. Transfers operate on
variable byte counts and may be partial, which makes the module well suited to
serial I/O and other streaming data.

- **Byte stream with partial transfers** — `write`, `read`, `peek`, and
  `discard` return the number of bytes actually moved. A request may complete
  partially rather than fail outright, and writes never overwrite unread data.
- **Fixed capacity** — capacity is set at init time and never grows; the whole
  buffer is usable (no reserved slot).
- **DMA-friendly access** — `nx_ringbuf_peek_linear` exposes the largest
  contiguous readable region, while `nx_ringbuf_poke_linear` exposes the largest
  contiguous writable region. After direct access, call `nx_ringbuf_discard` to
  consume readable bytes or `nx_ringbuf_commit` to publish written bytes.
- **Cooperative SPSC use** — one writer and one reader can share a ring buffer on
  a single core when neither can preempt the other. Because both sides update the
  shared byte count, the caller must serialize preemptive or otherwise concurrent
  access. The module does not acquire a lock itself.
- **Helpers** — queries report the current size, capacity, and free space, while
  `nx_ringbuf_clear` resets the buffer to empty.

```c
#include "nx_ringbuf.h"

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


## nx_tiered_mem_pool — tiered static memory pool

A fixed-capacity alternative to `malloc` and `free`. The pool divides one
caller-provided arena into tiers of equal-size blocks and serves each request
from the smallest suitable tier that has space.

- **Bounded work** — allocation scans the configured tier table and, for a
  suitable tier, at most that tier's bounded bitmap. Freeing first scans the
  bounded tier table to locate the block, then updates its bitmap entry in
  constant time.
- **No in-band block headers** — ownership and allocation state are stored in
  per-tier metadata and bitmaps, leaving every returned block entirely available
  to the caller.
- **Built-in validation** — an invalid pointer returns `NX_TIERED_ERR_INVALID`;
  freeing an already-free block returns `NX_TIERED_ERR_DOUBLE_FREE` instead of
  changing the pool state.
- **No external fragmentation within a tier** — each tier contains fixed-size
  blocks. Rounding a request up to a tier size can still introduce internal
  fragmentation.
- **Configurable fallback** — when the ideal tier is exhausted a request falls back
  to a larger tier; set `forbid_fallback` to serve only from the best-fit tier.
- **Runtime-configured, single-arena layout** — the tier table, bitmaps, and block
  storage all come from the supplied arena. Initialization reports the required
  size after alignment; an unaligned arena may need up to
  `_Alignof(max_align_t) - 1` additional bytes of leading padding.
- **Built-in statistics** — statistics for each tier include its effective block
  size, block count, free count, and peak usage, and can be queried by index.
- **Optional locking** — when allocation and deallocation may run concurrently,
  provide an `nx_lock` in the configuration. A `NULL` lock makes the critical-
  section helpers no-ops.

```c
#include "nx_tiered_mem_pool.h"

/* Aligning the arena makes the reported required size sufficient as-is. */
static _Alignas(max_align_t) uint8_t mem[32 * 8 + 128 * 4 + 256];

static const nx_tiered_level_cfg_t tiers[] = {
    { 32, 8 },     /* 8 blocks of 32 bytes  */
    { 128, 4 },    /* 4 blocks of 128 bytes */
};

nx_tiered_mem_pool_t     pool;
nx_tiered_mem_pool_cfg_t cfg = {
    .memory      = mem,
    .memory_size = sizeof(mem),
    .tiers       = tiers,
    .tier_count  = sizeof(tiers) / sizeof(tiers[0]),
    /* forbid_fallback omitted -> false: a request may fall back to a larger tier */
};

size_t required = 0;
if (nx_tiered_mem_pool_init(&pool, &cfg, &required) != NX_TIERED_OK) {
    /* required reports the arena size needed for this tier configuration */
}

void *p = nx_tiered_mem_pool_alloc(&pool, 20);     /* served by the 32-byte tier */
/* ... use p ... */
nx_tiered_mem_pool_free(&pool, p);                 /* owning tier inferred from address */

/* introspection: walk tiers by index */
for (size_t i = 0; i < nx_tiered_mem_pool_tier_count(&pool); i++) {
    nx_tiered_level_stat_t st;
    nx_tiered_mem_pool_get_tier_stat(&pool, i, &st);
    /* watch st.peak_used, detect exhaustion, etc. */
}
```


## nx_ref_msg — reference-counted zero-copy messages

A zero-copy dispatch layer for messages allocated from `nx_tiered_mem_pool`.
Each queue stores an `nx_ref_msg_t *`, so multiple consumers can share one
payload. A reference count keeps the pooled block alive until the producer and
all consumers have released their references.

- **One allocation per message** — `nx_ref_msg_alloc` allocates the header and
  payload together and returns the producer's initial reference. The payload is
  available through `nx_ref_msg_data`, and `nx_ref_msg_len` reports its current
  length. `nx_ref_msg_shrink` can reduce that reported length without reallocating
  the block.
- **Explicit ownership** — every successful `nx_ref_msg_publish` adds one
  queue-owned reference. After publishing, the producer releases its initial
  reference. A consumer pops the pointer, reads the shared payload, and releases
  its reference when finished; the final release returns the block to the pool.
  Fill the payload before publishing it and do not access a message after
  releasing its reference.
- **Reference-safe queues** — `nx_ref_msg_queue_init` fixes the queue element size
  to `sizeof(nx_ref_msg_t *)` and uses `NX_QUEUE_ON_FULL_REJECT`. An overwrite
  policy would discard a pointer without releasing its reference. For the same
  reason, do not discard an entry with `nx_queue_pop(q, NULL)`, or clear or
  reinitialize a non-empty message queue. Pop and release every queued message
  first.
- **Best-effort multi-queue publish** — `nx_ref_msg_publish_multi` accepts a
  NULL-terminated array of queue pointers and continues after a full queue. It
  returns `NX_REF_MSG_OK` when every queue accepted the message,
  `NX_REF_MSG_PARTIAL` when only some did, and `NX_REF_MSG_ERR_FULL` when a
  non-empty list delivered to none. Optional outputs report the delivery count
  and the first failed queue index.
- **Caller-managed concurrency** — the reference count and queues are not
  atomic. If publishing, consuming, or releasing can occur concurrently, the
  caller must serialize both the reference-count and queue operations. A lock
  configured on the underlying memory pool protects only pool allocation and
  deallocation.

```c
#include "nx_ref_msg.h"
#include <string.h>

/* One pool tier with eight 64-byte blocks, plus metadata headroom. */
static _Alignas(max_align_t) uint8_t arena[8 * 64 + 256];
static const nx_tiered_level_cfg_t tiers[] = { { 64, 8 } };

nx_tiered_mem_pool_t pool;
nx_tiered_mem_pool_cfg_t pool_cfg = {
    .memory      = arena,
    .memory_size = sizeof(arena),
    .tiers       = tiers,
    .tier_count  = 1u,
};
if (nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL) != NX_TIERED_OK) {
    /* handle invalid configuration or insufficient arena space */
}

nx_ref_msg_t *queue_storage[4];
nx_queue_t queue;
nx_ref_msg_queue_init(&queue, queue_storage, 4u);

/* Producer: allocate, fill, publish, and release the producer reference. */
nx_ref_msg_t *msg = nx_ref_msg_alloc(&pool, 5u);
if (msg != NULL) {
    memcpy(nx_ref_msg_data(msg), "hello", 5u);
    nx_ref_msg_publish(msg, &queue);    /* success adds a queue reference */
    nx_ref_msg_release(msg);            /* release the producer reference */
}

/* Consumer: the popped pointer owns one reference. */
nx_ref_msg_t *received = NULL;
if (nx_queue_pop(&queue, &received) == NX_QUEUE_OK) {
    /* consume(nx_ref_msg_data(received), nx_ref_msg_len(received)); */
    nx_ref_msg_release(received);
}
```


## nx_timer — software timer manager

A caller-driven software timer manager built on an intrusive list. The
application periodically passes a monotonically increasing tick value to
`nx_timer_mgr_process`, which scans the active timers and invokes callbacks for
those that have expired. The module uses no dynamic memory or hardware APIs.

- **Caller-defined tick unit** — delays and periods use the unit of the supplied
  counter, whether that is milliseconds, RTOS ticks, or microseconds.
- **Explicit lifecycle** — initialize the manager with `nx_timer_mgr_init`, each
  timer with `nx_timer_init`, and then arm it with `nx_timer_start`. Starting an
  active timer restarts it; `nx_timer_stop` is safe for an inactive timer.
- **One-shot and periodic operation** — `period == 0` selects a one-shot timer.
  A nonzero period reloads from the previous deadline to preserve phase. One
  call to `nx_timer_mgr_process` invokes a given periodic timer at most once, so
  an overdue timer catches up over subsequent calls rather than in a tight loop.
- **Predictable list scan** — start and stop update the intrusive list in constant
  time. Processing scans all active timers, so its cost is O(N) plus callback
  work. Timers are visited in list order, not sorted by deadline.
- **Start time and wraparound** — a new deadline is based on the manager's most
  recent processed tick (`last_tick + delay`). The signed-difference expiry test
  handles `uint32_t` wraparound when individual delays and periods do not exceed
  `INT32_MAX` ticks and processing is not paused across half of the counter's
  range.
- **Synchronous callbacks** — callbacks run inside `nx_timer_mgr_process` and
  therefore in its calling context. The module does not lock; serialize
  `start`, `stop`, and `process` when different contexts can call them. Avoid
  changing other timers from a callback because doing so can alter the current
  list traversal.

```c
#include "nx_timer.h"

static void on_timeout(nx_timer_t *t, void *arg) {
    (void)t;
    printf("Timer %s expired\n", (const char*)arg);
}

nx_timer_mgr_t mgr;
nx_timer_mgr_init(&mgr);

nx_timer_t t1;
nx_timer_init(&t1, on_timeout, "A");
nx_timer_start(&mgr, &t1, 100, 0);  /* one-shot at tick 100 */

nx_timer_t t2;
nx_timer_init(&t2, on_timeout, "B");
nx_timer_start(&mgr, &t2, 50, 20);  /* first at 50, then every 20 ticks */

for (uint32_t now = 0; now < 200; now++) {
    nx_timer_mgr_process(&mgr, now);
}
```


## nx_coro — stackless coroutines

A header-only set of macros that let an ordinary C function suspend in the
middle and resume there on the next call, built on Duff's device and `__LINE__`.
This turns workflows such as "send, wait for a reply, then retry" into
straight-line code without an RTOS or a per-task stack.

- **Stackless** — the coroutine state is a small caller-owned structure whose
  resume point is a source line number. There is no per-coroutine stack, context
  switch, or allocation.
- **Never blocks** — a coroutine returns to its caller at every suspend point.
  There is no scheduler in the module; the application's main loop is the
  scheduler, calling each coroutine again and again.
- **Suspend for a delay or until a condition is met** — `NX_CORO_YIELD` gives up
  a turn, `NX_CORO_WAIT_UNTIL` and `NX_CORO_WAIT_WHILE` suspend on a predicate,
  and `NX_CORO_SLEEP`, `NX_CORO_TIMEDSET`, and `NX_CORO_TIMEDWAIT` use a
  caller-supplied tick source — a `uint32_t (*)(void)` monotonic counter, with
  wrap-around handled by unsigned differences.
- **Two state types** — `nx_coro_stack_t` for yield and condition waits;
  `nx_coro_stack_plus_t`, initialized with `NX_CORO_INIT_PLUS`, adds the tick
  source the time-based macros need.
- **Composable** — `NX_CORO_SCHEDULE` reports whether a coroutine is still
  running, allowing a parent coroutine to run a child to completion.

The `switch`-based implementation has several restrictions:

- Local variables do not survive a suspend point; keep persistent state in the
  caller-owned state structure.
- Do not place another `switch` between `NX_CORO_BEGIN` and `NX_CORO_END`.
- Put at most one suspend point on each source line, and keep every suspend point
  lexically within the same function.
- Code before `NX_CORO_BEGIN` runs on every call.

```c
#include "nx_coro.h"

/* state that must survive a suspend point lives in the struct, not in locals */
typedef struct {
    nx_coro_stack_t base;
    int             step;
} blink_t;

static nx_coro_ret_t blink(blink_t *st) {
    NX_CORO_BEGIN(&st->base);
    while (1) {
        printf("step %d\n", ++st->step);
        NX_CORO_YIELD(&st->base);   /* returns now, resumes here next call */
    }
    NX_CORO_END(&st->base);
}

blink_t a = {0}, b = {0};
NX_CORO_INIT(&a.base);
NX_CORO_INIT(&b.base);

/* the main loop is the scheduler: one pass advances each coroutine one step */
for (;;) {
    blink(&a);
    blink(&b);
}
```

> **Note:** a resume point expands to `lc = __LINE__; case __LINE__:`, which
> GCC/Clang read as a case falling through because no `break` precedes the
> generated `case` label. This fallthrough is intentional: execution reaches the
> label directly on the first pass and the outer `switch` jumps back to it on a
> later call. Use `-Wno-implicit-fallthrough` if you build with `-Wextra`.


## nx_lock — pluggable critical-section abstraction

A small adapter for platform-specific critical sections. An `nx_lock_t` stores
matching `enter` and `exit` callbacks plus an optional context pointer. Code that
needs synchronization calls `nx_lock_enter`, performs the protected operation,
and passes the returned state to `nx_lock_exit`.

- **Caller-selected primitive** — callbacks can disable interrupts, acquire a
  mutex, or enter another platform-specific critical section. The adapter does
  not create a lock or impose a scheduling policy.
- **Saved-state pairing** — `enter` returns an implementation-defined
  `uintptr_t`, such as the previous interrupt-enable state. Pass that value
  unchanged to the matching `exit` call so the platform implementation can
  restore its prior state. Supply `enter` and `exit` as a valid pair.
- **Header-only forwarding** — `nx_lock_enter` and `nx_lock_exit` validate the
  lock pointer and invoke the configured callbacks. Passing a `NULL` lock makes
  both helpers no-ops, with `nx_lock_enter` returning zero.
- **Explicit or configured use** — `nx_tiered_mem_pool`, `nx_log`, and
  `nx_event_flags` accept a configured lock. `nx_queue` and `nx_ringbuf` do not;
  callers must explicitly wrap their operations when synchronization is needed.
  `nx_ref_msg` reference counts and `nx_timer` operations likewise require
  external serialization when shared concurrently.

```c
#include "nx_lock.h"
#include "nx_queue.h"

/* Example platform hooks: save, disable, and later restore interrupts. */
static uintptr_t irq_enter(void *ctx) {
    (void)ctx;
    uintptr_t state = platform_irq_save();
    platform_irq_disable();
    return state;
}

static void irq_exit(void *ctx, uintptr_t state) {
    (void)ctx;
    platform_irq_restore(state);
}

static const nx_lock_t queue_lock = {
    .enter = irq_enter,
    .exit  = irq_exit,
    .ctx   = NULL,
};

int storage[4];
int item = 42;
nx_queue_t q;
nx_queue_init(&q, storage, sizeof(storage[0]), 4u, NX_QUEUE_ON_FULL_REJECT);

uintptr_t state = nx_lock_enter(&queue_lock);
nx_queue_push(&q, &item);
nx_lock_exit(&queue_lock, state);
```

Keep the protected region short. Disabling interrupts protects only the current
core; use a primitive with the required cross-core semantics on a multicore
target.

## nx_log — asynchronous logging with caller-owned storage

A logging facility that formats messages into a caller-owned ring buffer. The
main loop later calls `nx_log_process` to drain buffered bytes to an injected
sink. Formatting remains on the producer's path, but slow or blocking sink I/O
runs only from the consumer path. The module uses no dynamic memory.

- **Plain text** — messages are formatted with `vsnprintf` and carry a
  `[level-tag] [tick] file:line: ` prefix, so the output is readable directly on a
  serial terminal with no decoding tool. The tick timestamp comes from an
  injected `get_tick` source and is omitted when that callback is NULL.
- **Asynchronous delivery** — a formatted line is enqueued into a byte ring
  buffer. `nx_log_process` later copies buffered chunks under the configured
  lock and invokes the sink after releasing it. Producers are decoupled from the
  sink's latency, and the sink runs in one place rather than at every call site.
- **Push or pull consumption** — the `write` sink may be NULL. In that mode,
  logs remain buffered until the caller retrieves them with `nx_log_read`, for
  example through a debug shell or diagnostic command.
- **Zero allocation, caller-owned buffer** — the ring-buffer storage is supplied
  through the `buffer` and `buffer_size` configuration fields; the module
  allocates nothing.
- **Two-stage level filtering** — `NX_LOG_COMPILE_LEVEL` strips call sites more
  verbose than the configured compile-time threshold, allowing the compiler to
  remove their formatting work. The runtime `level` filters what remains and
  can be changed with `nx_log_set_level`.
- **Whole-line-or-nothing, with a full-buffer policy** — a line is never written
  halfway. Lines longer than `NX_LOG_LINE_MAX` are truncated before enqueueing.
  When the buffer cannot hold a new line, `on_full` decides:
  `NX_LOG_ON_FULL_OVERWRITE_OLD` (the default) evicts the oldest whole lines to
  preserve the newest lines that fit; `NX_LOG_ON_FULL_DROP_NEW` preserves the
  buffered lines and drops the new one. `nx_log_dropped` counts every discarded
  or evicted line.
- **Optional locking** — cooperative, non-preempting use needs no lock. For
  concurrent producers or a producer that can preempt the consumer, configure
  an `nx_lock`. It protects ring-buffer updates and whole-line eviction;
  formatting and sink I/O remain outside the critical section. The supplied
  callbacks and C library must themselves be suitable for any interrupt context
  from which logging is performed.

```c
#include "nx_log.h"

static uint8_t log_buf[512];   /* caller-owned ring-buffer storage */

/* the sink: push the drained bytes out a UART (blocking is fine, it runs
 * on the main loop, not in the producer). */
static void uart_sink(void *ctx, const uint8_t *data, size_t len) {
    uart_send(ctx, data, len);
}

nx_log_t log;
nx_log_cfg_t cfg = {
    .buffer      = log_buf,
    .buffer_size = sizeof(log_buf),
    .write       = uart_sink,
    .io_ctx      = &uart0,
    .get_tick    = board_millis,   /* NULL to omit the timestamp */
    .level       = NX_LOG_LEVEL_INFO,
    .lock        = NULL,           /* configure when access can overlap */
};
nx_log_init(&log, &cfg);

NX_LOGI(&log, "link up, addr=%u", addr);   /* enqueued */
NX_LOGD(&log, "raw=%02x", byte);           /* more verbose than INFO: filtered */

for (;;) {
    nx_log_process(&log);   /* drain queued bytes to the sink */
    /* ... rest of the main loop ... */
}
```

> **Note:** logging is a two-stage process. A `NX_LOGx` call only *formats and
> enqueues*; the bytes leave the buffer only when `nx_log_process` runs on the
> main loop (or `nx_log_read` pulls them out when there is no sink). Size `buffer`
> for the burst you expect between drains: once it is full, `on_full` decides
> whether the new line is dropped (and counted by `nx_log_dropped`) or the oldest
> lines are evicted — either way a line is never partially enqueued. Configure
> a lock whenever access to the same handle can overlap.

## nx_event_flags — polled event flags for cooperative loops

A header-only event-flags module with 32 caller-defined bits. Code can set,
clear, test, or consume selected flags without a scheduler or blocking wait. It
is intended for lightweight signaling in cooperative loops and, when configured
with a suitable lock, between an ISR and the main loop.

- **32 independent bits** — each bit is one event (user-defined meaning).
- **Set, clear, test, and take** — `set` raises flags, `clear` lowers them, and
  `test` checks without consuming. `take` tests and clears the selected mask as
  one protected operation when a lock is configured. Use `test` for broadcast
  state and `take` for one-shot work.
- **Barrier primitive: test_all** — returns true only when every bit in the mask
  is set. Use this for acknowledgment barriers or multipart readiness checks.
- **Coalescing** — setting the same flag multiple times before a take still reads
  as one raised bit. A flag records that an event happened at least once; it is
  not a counter.
- **Optional synchronization** — pass a valid `nx_lock` at initialization to
  protect `set`, `clear`, and `take` from concurrent updates. `test`, `test_all`,
  and `get` perform an unlocked 32-bit read; concurrent readers therefore depend
  on the target providing a coherent 32-bit read, or must be serialized by the
  caller.
- **Zero allocation, header-only** — the instance contains one `uint32_t` flag
  word and one lock pointer; all operations are `static inline`.

```c
#include "nx_event_flags.h"

/* Scenario: ISR sets flags, main loop takes them */
#define RX_READY   (1u << 0)
#define TX_DONE    (1u << 1)
#define TIMER_TICK (1u << 2)

/* The platform supplies an interrupt-safe enter/exit callback pair. */
extern const nx_lock_t g_isr_lock;

nx_event_flags_t g_events;
nx_event_flags_init(&g_events, &g_isr_lock);   /* with lock for ISR safety */

/* In the ISR */
void uart_rx_isr(void) {
    nx_event_flags_set(&g_events, RX_READY);   /* atomic under lock */
}

/* In the main loop */
for (;;) {
    if (nx_event_flags_take(&g_events, RX_READY)) {
        /* flag was set (and is now cleared); process the RX byte */
        handle_rx();
    }
    if (nx_event_flags_test(&g_events, TIMER_TICK)) {
        /* non-consuming: flag stays set until explicitly cleared */
    }
}
```

> **Note:** `test` / `test_all` are read-only and leave the flags raised — use
> these for broadcast events that multiple modules need to observe. `take`
> performs a test-and-clear under the configured lock, so one consumer handles
> the coalesced event. Multiple `set` calls before a `take` still produce one
> raised bit. Configure a lock whenever updates can overlap; single-context use
> needs none.

