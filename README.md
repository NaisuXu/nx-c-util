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
├── src/
│   ├── core/         # Core building blocks (list, queue, ringbuf, timer, coro, ref_msg, mem_pool, lock)
│   ├── middleware/   # Protocol parsers and stacks (modbus_rtu, modbus_rtu_slave, can_bus, future: can_isotp)
│   ├── algo/         # Algorithms (crc, sha256)
│   └── device/       # Platform-independent device drivers (ws2812)
└── examples/
    ├── core/         # Core module usage examples
    ├── middleware/   # Middleware module usage examples
    ├── algo/         # Algorithm module usage examples
    └── device/       # Device driver usage examples
```

Each module is designed to be independently usable. To integrate into your project,
simply copy the needed `.c` and `.h` files. Headers use single-level includes 
(e.g., `#include "nx_list.h"`) with no subdirectory prefix, so add the directory 
containing the copied files to your include path.

## Modules

### Core Modules
- [nx_list](docs/core_en.md#nx_list--intrusive-doubly-linked-circular-list) — intrusive doubly-linked circular list
- [nx_queue](docs/core_en.md#nx_queue--generic-ring-buffer-fifo-queue) — generic ring-buffer (FIFO) queue
- [nx_ringbuf](docs/core_en.md#nx_ringbuf--byte-oriented-ring-buffer) — byte-oriented ring buffer
- [nx_tiered_mem_pool](docs/core_en.md#nx_tiered_mem_pool--tiered-static-memory-pool) — tiered static memory pool
- [nx_ref_msg](docs/core_en.md#nx_ref_msg--reference-counted-zero-copy-messages) — reference-counted zero-copy messages
- [nx_timer](docs/core_en.md#nx_timer--software-timer-manager) — software timer manager
- [nx_coro](docs/core_en.md#nx_coro--stackless-coroutines) — stackless coroutines
- [nx_lock](docs/core_en.md#nx_lock--pluggable-critical-section-abstraction) — pluggable critical-section abstraction

See [Core Modules Documentation](docs/core_en.md) for detailed descriptions and examples.

### Middleware Modules
- [nx_can_bus](docs/middleware_en.md#nx_can_bus--can--can-fd-frame-structures-and-helpers) — CAN / CAN FD frame structures and helpers
- [nx_modbus_rtu](docs/middleware_en.md#nx_modbus_rtu--modbus-rtu-frame-structures-and-crc) — Modbus RTU frame structures and CRC
- [nx_modbus_rtu_slave](docs/middleware_en.md#nx_modbus_rtu_slave--event-driven-rtu-slave-frame--subscription-dispatch) — event-driven RTU slave: frame → subscription dispatch

See [Middleware Modules Documentation](docs/middleware_en.md) for detailed descriptions and examples.

### Algorithm Modules
- [nx_crc](docs/algo_en.md#nx_crc--crc-8--crc-16--crc-32-checksums) — CRC-8 / CRC-16 / CRC-32 checksums
- [nx_sha256](docs/algo_en.md#nx_sha256--sha-256-cryptographic-hash) — SHA-256 cryptographic hash

See [Algorithm Modules Documentation](docs/algo_en.md) for detailed descriptions and examples.

### Device Modules
- [nx_ws2812](docs/device_en.md#nx_ws2812--ws2812b-rgb-led-strip-driver) — WS2812/WS2812B RGB LED strip driver

See [Device Modules Documentation](docs/device_en.md) for detailed descriptions and examples.


## Usage

The library sources are organized by category under `src/` (`src/core/`, 
`src/middleware/`, `src/algo/`, `src/device/`) and can be dropped into your 
project — most modules have no dependencies beyond standard C and can be used 
independently. Headers use single-level includes (e.g., `#include "nx_list.h"`), 
so add the directory containing the copied files to your include path.

The `examples/core/`, `examples/middleware/`, `examples/algo/`, and `examples/device/` 
directories contain runnable usage examples for every module, driven through CMake so they 
build the same way on any platform.

### Build and run the examples

From the repository root:

```sh
cmake -S . -B build
cmake --build build
```

Then run the produced executables:

- **Linux / macOS**

  ```sh
  ./build/nx_core_examples        # Core modules (list, queue, ringbuf, mem_pool, ref_msg, timer, coro)
  ./build/nx_middleware_examples  # Middleware modules (modbus_rtu_slave)
  ./build/nx_algo_examples        # Algorithm modules (crc, sha256)
  ./build/nx_device_examples      # Device drivers (ws2812)
  ```

- **Windows (MinGW / MSYS)**

  ```sh
  ./build/nx_core_examples.exe
  ./build/nx_middleware_examples.exe
  ./build/nx_algo_examples.exe
  ./build/nx_device_examples.exe
  ```

- **Windows (Visual Studio / MSVC)** — multi-config generators place the binary
  in a per-config subdirectory:

  ```sh
  ./build/Debug/nx_core_examples.exe
  ./build/Debug/nx_middleware_examples.exe
  ./build/Debug/nx_algo_examples.exe
  ./build/Debug/nx_device_examples.exe
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
