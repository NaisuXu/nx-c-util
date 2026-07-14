# nx-c-util

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

## Modules

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

### nx_tiered_mem_pool — tiered static memory pool

A deterministic, fragmentation-free replacement for `malloc`/`free`, built from
several "tiers" of equally sized blocks carved out of one caller-provided buffer.

- **O(1) alloc/free** — allocation rounds a request up to the smallest tier whose
  block is large enough and pops from that tier's free list; free returns the
  block in constant time.
- **Zero per-block overhead** — on free, a pointer is mapped back to its owning
  tier purely by address range; blocks carry no header of their own.
- **No fragmentation** — within a tier every block is identical, so the pool
  never fragments.
- **Configurable fallback** — when the ideal tier is exhausted the pool can
  automatically fall back to a larger tier; set `forbid_fallback` at init time to
  restrict allocation strictly to the best-fit tier instead.
- **One-struct configuration** — all init parameters (buffer, tier list, policy)
  live in a single `nx_tiered_mem_pool_cfg_t` with the tier list embedded inline;
  init also reports the exact bytes the tiers need, so you can oversize the buffer
  and shrink it to fit after one run.
- **Built-in statistics** — per-tier block size, block count, free count, and a
  peak-usage high-water mark for tuning and diagnostics.
- **Not thread-safe** — concurrent access must be locked by the caller.

```c
#include "nx_tiered_mem_pool.h"

/* buffer must be max_align_t aligned; oversize it and let init report the exact need */
static _Alignas(max_align_t) uint8_t mem[32 * 8 + 128 * 4];

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

A message dispatch layer built on top of the two modules above: a message is
allocated once from an `nx_tiered_mem_pool` and delivered to one or more
`nx_queue`s. What a queue stores is a *pointer* to the message, not a copy, so
every consumer shares the same data — zero copy. A reference count decides when
the block goes back to the pool.

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
- **Reject-only queues** — initialize carrier queues with `nx_ref_msg_queue_init`
  (element size is fixed to a message pointer). The full-queue policy is forced to
  reject, because overwriting would silently drop an enqueued message and leak its
  reference.
- **Not thread-safe** — the refcount is a plain counter; concurrent access must be
  locked by the caller.

```c
#include "nx_ref_msg.h"

/* one consumer queue (its buffer holds message pointers) */
nx_ref_msg_t *qbuf[4];
nx_queue_t    q;
nx_ref_msg_queue_init(&q, qbuf, 4);

/* producer: allocate from a pool, fill, publish, then release its reference */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, 16);   /* refcount = 1 */
memcpy(nx_ref_msg_data(m), payload, 16);

nx_queue_t *group[] = { &q, /* &q2, &q3, ... */ NULL };  /* NULL-terminated */
size_t delivered = 0;
nx_ref_msg_publish_multi(m, group, &delivered);    /* refcount = 1 + delivered */
nx_ref_msg_release(m);                             /* give up producer reference */

/* consumer: pop the shared message, use it, release when done */
nx_ref_msg_t *got = NULL;
if (nx_queue_pop(&q, &got) == NX_QUEUE_OK) {
    /* ... read nx_ref_msg_data(got), nx_ref_msg_len(got) ... */
    nx_ref_msg_release(got);                       /* frees when the last ref is gone */
}
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
