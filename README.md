# nx-c-util

[简体中文](README_CN.md) | [English](README.md)

## Overview

`nx-c-util` is a pure C library of small, composable building blocks for
embedded development.

The modules share three design goals:

- **No dynamic allocation** — callers provide all storage, and the library does
  not depend on `malloc` or `free`, making it suitable for heapless targets.
- **Predictable resource use** — modules expose their storage, configuration,
  and failure paths, making them straightforward to use in constrained and
  real-time systems.
- **Portability** — the code uses standard C11 and has no platform-specific
  dependencies. It builds on Windows, Linux, and macOS.

## Directory Structure

```
nx-c-util/
├── src/
│   ├── core/         # Core data structures and utilities
│   ├── middleware/   # Protocol and diagnostic middleware
│   ├── algo/         # Algorithms (crc, sha256)
│   └── device/       # Platform-independent device drivers (ws2812, kth7112)
└── examples/
    ├── core/         # Core module usage examples
    ├── middleware/   # Middleware module usage examples
    ├── algo/         # Algorithm module usage examples
    └── device/       # Device driver usage examples
```

Modules can be integrated independently. Copy the required `.c` and `.h` files
and add their directories to the include path. Headers use flat includes such
as `#include "nx_list.h"`, without subdirectory prefixes.

## Modules

### Core Modules

- [nx_list](docs/core_en.md#nx_list--intrusive-circular-doubly-linked-list) — intrusive circular doubly linked list
- [nx_queue](docs/core_en.md#nx_queue--generic-ring-buffer-fifo-queue) — generic ring-buffer (FIFO) queue
- [nx_ringbuf](docs/core_en.md#nx_ringbuf--byte-oriented-ring-buffer) — byte-oriented ring buffer
- [nx_tiered_mem_pool](docs/core_en.md#nx_tiered_mem_pool--tiered-static-memory-pool) — tiered static memory pool
- [nx_ref_msg](docs/core_en.md#nx_ref_msg--reference-counted-zero-copy-messages) — reference-counted zero-copy messages
- [nx_timer](docs/core_en.md#nx_timer--software-timer-manager) — software timer manager
- [nx_coro](docs/core_en.md#nx_coro--stackless-coroutines) — stackless coroutines
- [nx_lock](docs/core_en.md#nx_lock--pluggable-critical-section-abstraction) — pluggable critical-section abstraction
- [nx_log](docs/core_en.md#nx_log--asynchronous-logging-with-caller-owned-storage) — asynchronous logging with caller-owned storage
- [nx_event_flags](docs/core_en.md#nx_event_flags--polled-event-flags-for-cooperative-loops) — polled event flags for cooperative loops

See [Core Modules Documentation](docs/core_en.md) for detailed descriptions and examples.

### Middleware Modules

- [nx_can_bus](docs/middleware_en.md#nx_can_bus--can--can-fd-frame-structures-and-helpers) — CAN / CAN FD frame structures and helpers
- [nx_modbus_rtu](docs/middleware_en.md#nx_modbus_rtu--modbus-rtu-frame-structures-and-crc) — Modbus RTU frame structures and CRC
- [nx_modbus_rtu_slave](docs/middleware_en.md#nx_modbus_rtu_slave--event-driven-rtu-slave-frame--subscription-dispatch) — event-driven RTU slave: frame → subscription dispatch
- [nx_modbus_rtu_master](docs/middleware_en.md#nx_modbus_rtu_master--event-driven-rtu-master-queue--wire--subscription-dispatch) — event-driven RTU master: queue → wire → subscription dispatch
- [nx_tp_sdu](docs/middleware_en.md#nx_tp_sdu--transport-layer-service-data-unit) — transport-layer service data unit
- [nx_can_isotp](docs/middleware_en.md#nx_can_isotp--iso-15765-2-docan--iso-tp-segmented-transport) — ISO 15765-2 (DoCAN / ISO-TP) segmented transport
- [nx_uds](docs/middleware_en.md#nx_uds--iso-14229-vocabulary) — ISO 14229 vocabulary
- [nx_uds_server](docs/middleware_en.md#nx_uds_server--iso-14229-diagnostic-server-ecu-side) — ISO 14229 diagnostic server (ECU side)
- [nx_uds_svc_session](docs/middleware_en.md#nx_uds_svc_session--core-session-and-reset-services) — core session and reset services
- [nx_uds_svc_sec](docs/middleware_en.md#nx_uds_svc_sec--0x27-seedkey-exchange) — 0x27 seed/key exchange
- [nx_uds_svc_transfer](docs/middleware_en.md#nx_uds_svc_transfer--memory-upload-and-download-services) — memory upload and download services
- [nx_uds_tp_bind](docs/middleware_en.md#nx_uds_tp_bind--binding-a-uds-endpoint-to-a-transport) — binding a UDS endpoint to a transport
- [nx_uds_client](docs/middleware_en.md#nx_uds_client--iso-14229-diagnostic-client-tester-side) — ISO 14229 diagnostic client (tester side)

See [Middleware Modules Documentation](docs/middleware_en.md) for detailed descriptions and examples.

### Algorithm Modules

- [nx_crc](docs/algo_en.md#nx_crc--crc-8--crc-16--crc-32-checksums) — CRC-8 / CRC-16 / CRC-32 checksums
- [nx_sha256](docs/algo_en.md#nx_sha256--sha-256-cryptographic-hash) — SHA-256 cryptographic hash

See [Algorithm Modules Documentation](docs/algo_en.md) for detailed descriptions and examples.

### Device Modules

- [nx_ws2812](docs/device_en.md#nx_ws2812--ws2812b-rgb-led-strip-driver) — WS2812/WS2812B RGB LED strip driver
- [nx_kth7112](docs/device_en.md#nx_kth7112--kth7112-magnetic-angle-encoder-over-spi) — KTH7112 16-bit magnetic angle encoder over SPI

See [Device Modules Documentation](docs/device_en.md) for detailed descriptions and examples.


## Usage

The `examples/core/`, `examples/middleware/`, `examples/algo/`, and
`examples/device/` directories contain runnable examples for every module. They
are built with CMake on all supported platforms.

### Build and run the examples

From the repository root:

```sh
cmake -S . -B build
cmake --build build
```

Then run the produced executables:

- **Linux / macOS**

  ```sh
  ./build/nx_core_examples        # All core module examples
  ./build/nx_middleware_examples  # All middleware module examples
  ./build/nx_algo_examples        # Algorithm module examples
  ./build/nx_device_examples      # Device driver examples
  ```

- **Windows (MinGW / MSYS)**

  ```sh
  ./build/nx_core_examples.exe
  ./build/nx_middleware_examples.exe
  ./build/nx_algo_examples.exe
  ./build/nx_device_examples.exe
  ```

- **Windows (Visual Studio / MSVC)** — multi-config generators place the binaries
  in a per-config subdirectory:

  ```sh
  ./build/Debug/nx_core_examples.exe
  ./build/Debug/nx_middleware_examples.exe
  ./build/Debug/nx_algo_examples.exe
  ./build/Debug/nx_device_examples.exe
  ```

### Choosing a generator

`cmake -S . -B build` uses the platform's default generator. To select one
explicitly, pass `-G`:

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

This project is licensed under the [MIT License](LICENSE).
