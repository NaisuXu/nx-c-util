# Core Modules

## nx_list — intrusive doubly-linked circular list

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
- **SPSC-friendly** — in a single-producer/single-consumer scenario (one side
  only pushes, the other only pops) it is naturally thread-safe; other
  concurrent access requires caller-side locking.
- **Helpers** — `push` / `pop` / `peek` / `clear` / `size` / `capacity` /
  `is_empty` / `is_full`.

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

A deterministic, fragmentation-free replacement for `malloc`/`free`, built from
several "tiers" of equally sized blocks carved out of one caller-provided buffer.

- **Bounded, predictable timing** — a request is rounded up to the smallest
  large-enough tier and served from its in-use bitmap. Free is O(1); allocation
  scans a small bitmap bounded by the tier's block count.
- **Zero per-block overhead** — blocks carry no header; the owning tier is found
  by address range on free. Since blocks store no internal pointer, block sizes
  down to a single alignment unit are usable. The tier's bitmap is contiguous and
  separate from the data area (better cache behavior, no per-alloc overhead).
- **Defends itself from abuse** — a double free is detected and asserted; freeing
  an address not in any tier's range is caught; a config mismatch (block count
  cannot fit in the bitmap / a tier's range overlaps another / size inversion
  down the tiers) aborts init via assert.
- **Full introspection** — every tier reports its size, count, and used count, so
  the caller can watch the high-water mark on each tier or build a pool
  exhaustion detector.
- **Header-only, zero code size** — all operations (init, alloc, free, stats) are
  `static inline`.

```c
#include "nx_tiered_mem_pool.h"

/* tiers: 4 × 16-byte blocks, 4 × 64-byte blocks */
uint8_t data[4*16 + 4*64];

uint32_t bitmaps[2];  /* 4 bits per tier → one uint32_t each */

nx_tiered_mem_pool_t pool;
nx_tiered_mem_pool_tier_def_t defs[] = {
    {16, 4},
    {64, 4},
};
nx_tiered_mem_pool_init(&pool, data, sizeof(data), defs, 2, bitmaps);

void *a = nx_tiered_mem_pool_alloc(&pool, 10);    /* -> 16-byte tier */
void *b = nx_tiered_mem_pool_alloc(&pool, 50);    /* -> 64-byte tier */

nx_tiered_mem_pool_free(&pool, a);
nx_tiered_mem_pool_free(&pool, b);

/* introspection */
for (size_t i = 0; i < pool.tier_count; i++) {
    size_t used = nx_tiered_mem_pool_tier_used(&pool, i);
    /* watch high-water marks, detect exhaustion, etc. */
}
```


## nx_ref_msg — reference-counted zero-copy messages

A message-passing layer where messages are reference-counted blocks carved from a
static memory pool: publishing a message increments its ref-count, consuming
(delivery or explicit drop) decrements it, and the block is freed automatically
when the ref-count hits zero. This eliminates both copying (one allocation at
publish time, consumed by N readers in place) and free timing races (a reader
does not need to know whether it holds the last reference — `drop` handles it).

- **Publish once, consume many** — `publish` allocates a block from the pool and
  sets the ref-count to 1; `deliver` returns a pointer to the caller without
  copying and increments the ref-count by 1 for that delivery; when the reader
  is done (or skips the message) it calls `drop`, which decrements and frees the
  block only if the ref-count reaches zero. The producer can safely discard its
  ref at any time, and the message stays live until the last consumer drops it.
- **No external memory** — the refcount and size metadata live in a small header
  that precedes the user data in the pool block; the block layout is
  `header || user data`, and pointers handed to the user point *past* the
  header, so `drop(msg)` works backward to find it.
- **Type-safe messages** — the header records a `msg_type` (a user-defined
  integer id) so the consumer can dispatch on message type without parsing the
  payload; types are also used by `deliver` filters (deliver only messages of a
  particular type to a particular queue) to route messages without waking every
  consumer.
- **Lossless dispatch** — `deliver` pushes the message to one or more queues
  (ref-count incremented once per delivery), each queue draining into a
  different consumer. If a queue is full, `deliver` returns an error and does
  not drop the message; the publisher can retry or discard. This differs from
  "classic" pub/sub, which may silently drop slow consumers — here the caller
  sees full queues and decides the policy.
- **Pool exhaustion protection** — callers should size the pool and the queues so
  the total in-flight message memory never exceeds the pool; ref-counting means
  the pool self-drains as consumers finish with their copies, and when the pool
  is empty `publish` returns NULL, giving the publisher an explicit backpressure
  signal.

```c
#include "nx_ref_msg.h"

/* pool: 8 × 64-byte blocks */
uint8_t       pool_data[8 * 64];
uint32_t      pool_bitmap[1];
nx_queue_t    q;
int           q_storage[4];

nx_ref_msg_sys_t sys;
nx_tiered_mem_pool_tier_def_t tier = {64, 8};
nx_ref_msg_sys_init(&sys, pool_data, sizeof(pool_data), &tier, 1, pool_bitmap);

nx_queue_init(&q, q_storage, sizeof(void*), 4, NX_QUEUE_ON_FULL_REJECT);

/* publish a message of type 1 */
void *msg = nx_ref_msg_publish(&sys, 1, "hello", 5);
if (msg) {
    nx_ref_msg_deliver(&sys, msg, &q);   /* queue now holds a ref */
    nx_ref_msg_drop(&sys, msg);          /* publisher drops its ref */
}

/* consumer */
void *rmsg;
if (nx_queue_pop(&q, &rmsg) == NX_QUEUE_OK) {
    /* cast to the actual message struct, check msg_type, etc. */
    nx_ref_msg_drop(&sys, rmsg);         /* done; block freed if refcount=0 */
}
```


## nx_timer — software timer manager

A deterministic, tick-driven timer scheduler where every timer is caller-owned
and placed in an intrusive red-black tree sorted by expiry. On each tick,
`nx_timer_tick` walks the tree and fires every expired timer's callback; a
one-shot is removed automatically, a periodic timer is rescheduled for the next
interval. Zero dynamic memory.

- **Intrusive, deterministic** — timers embed the `nx_timer_t` struct and link
  into a red-black tree (`nx_list`-based), so there is no hidden allocation and
  the cost of add/remove/tick is bounded by tree depth (O(log N) rebalance when
  adding, O(1) to check the earliest, O(K) to fire K expired timers where K is
  usually small).
- **Tick-driven** — time is an abstract monotonic counter (ticks); the caller
  advances it by calling `nx_timer_tick(mgr, now)`. No threads, no OS hooks, no
  platform coupling.
- **One-shot and periodic** — configured per timer at start time; one-shot fires
  once and is removed, periodic reschedules for `interval` ticks in the future.
- **Safe manipulation from callbacks** — `nx_timer_tick` iterates in a way that
  tolerates a callback starting or stopping other timers (including itself).
- **Optional coalescing** — timers within the same tick fire in tree order (which
  is add order when they have the same deadline); callers that need tighter
  control can sort by a secondary key in the `nx_timer_t` struct.

```c
#include "nx_timer.h"

static void on_timeout(nx_timer_t *t, void *arg) {
    printf("Timer %s expired\n", (const char*)arg);
}

nx_timer_mgr_t mgr;
nx_timer_mgr_init(&mgr);

nx_timer_t t1;
/* one-shot, fires at tick 100 */
nx_timer_start(&mgr, &t1, 100, 0, on_timeout, "A");

nx_timer_t t2;
/* periodic, first fire at tick 50, then every 20 ticks */
nx_timer_start(&mgr, &t2, 50, 20, on_timeout, "B");

for (uint32_t now = 0; now < 200; now++) {
    nx_timer_tick(&mgr, now);   /* fires callbacks as timers expire */
}
```


## nx_coro — stackless coroutines

A header-only set of macros that let an ordinary C function suspend in the
middle and resume there on the next call, built on Duff's device and `__LINE__`.
That turns a sequence like "send, wait for the reply, retry" into straight-line
code instead of an explicit state machine, without an RTOS and without a stack
per task.

- **Stackless** — nothing is saved across a suspend point except one line
  number. The whole coroutine state is a caller-owned struct of one to three
  words: no per-coroutine stack, no context switch, no allocation.
- **Never blocks** — a coroutine returns to its caller at every suspend point.
  There is no scheduler in the module; the application's main loop is the
  scheduler, calling each coroutine again and again.
- **Suspend on time or on a condition** — `NX_CORO_YIELD` gives up a turn,
  `NX_CORO_WAIT_UNTIL` / `NX_CORO_WAIT_WHILE` suspend on a predicate, and
  `NX_CORO_SLEEP` / `NX_CORO_TIMEDSET` / `NX_CORO_TIMEDWAIT` suspend on a
  caller-supplied tick source — a `uint32_t (*)(void)` monotonic counter, with
  wrap-around handled by unsigned differences.
- **Two state types** — `nx_coro_stack_t` for yield and condition waits;
  `nx_coro_stack_plus_t`, initialized with `NX_CORO_INIT_PLUS`, adds the tick
  source the time-based macros need.
- **Composable** — `NX_CORO_SCHEDULE` reports whether a coroutine is still
  running, so a parent runs a child to completion by waiting on it.

Restrictions follow from the `switch`-based implementation: locals do not
survive a suspend point (persistent state goes in the struct), you cannot write
a `switch` of your own between `BEGIN` and `END`, there is at most one suspend
point per source line, suspend points must be lexically inside the same
function, and code placed before `NX_CORO_BEGIN` runs on every call.

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
> GCC/Clang read as a case falling through for want of a `break`. The switch
> only ever jumps to those labels, so that fallthrough cannot happen — build
> with `-Wno-implicit-fallthrough` if you use `-Wextra`.


## nx_lock — pluggable critical-section abstraction

A minimal portability shim: the core modules that need mutual exclusion
(`nx_queue`, `nx_ringbuf`) call `nx_lock_acquire` / `nx_lock_release`, which
default to no-ops (safe for bare-metal single-core). On platforms that need real
locks, define the hook macros to your platform's primitives (CMSIS-RTOS mutex,
FreeRTOS critical section, pthread mutex, etc.) before including the headers.

- **Defaults to no-op** — if you don't define the hooks, `nx_lock_acquire` and
  `nx_lock_release` expand to nothing, so single-threaded or SPSC usage has zero
  overhead.
- **Define once, used everywhere** — the lock object (`nx_lock_t`) and the
  acquire/release macros are in one header; every module that needs a lock
  includes it and uses the same definition.
- **Caller controls the policy** — critical sections, mutexes, spinlocks,
  recursive locks — the library does not pick; it only brackets the region. You
  supply the actual locking primitives by defining the three hooks:
  `NX_LOCK_DEFINE`, `NX_LOCK_ACQUIRE`, `NX_LOCK_RELEASE`.

```c
#include "nx_lock.h"
#include "nx_queue.h"

/* example: map to FreeRTOS critical sections (disable interrupts) */
#define NX_LOCK_DEFINE(name)      /* empty; no storage needed */
#define NX_LOCK_ACQUIRE(lock)     taskENTER_CRITICAL()
#define NX_LOCK_RELEASE(lock)     taskEXIT_CRITICAL()

/* now nx_queue, nx_ringbuf, etc. use the above hooks when they lock */
int        storage[4];
nx_queue_t q;
nx_queue_init(&q, storage, sizeof(int), 4, NX_QUEUE_ON_FULL_REJECT);
/* push/pop internally call NX_LOCK_ACQUIRE / RELEASE */
```
