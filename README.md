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
- **Built-in statistics** — per-tier block size, block count, free count, and a
  peak-usage high-water mark for tuning and diagnostics.
- **Not thread-safe** — concurrent access must be locked by the caller.

```c
#include "nx_tiered_mem_pool.h"

/* buffer must be max_align_t aligned; sized to fit all tiers */
static _Alignas(max_align_t) uint8_t mem[32 * 8 + 128 * 4];

const nx_tiered_level_cfg_t tiers[] = {
    { 32, 8 },    /* 8 blocks of 32 bytes  */
    { 128, 4 },   /* 4 blocks of 128 bytes */
};

nx_tiered_mem_pool_t pool;

/* last arg = forbid_fallback: false lets a request fall back to a larger tier */
nx_tiered_mem_pool_init(&pool, mem, sizeof(mem), tiers, 2, false);

void *p = nx_tiered_mem_pool_alloc(&pool, 20);   /* served by the 32-byte tier */
/* ... use p ... */
nx_tiered_mem_pool_free(&pool, p);               /* owning tier inferred from address */
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
