# Algorithm Modules

## nx_crc — CRC-8 / CRC-16 / CRC-32 checksums

Bitwise CRC routines for 8-, 16-, and 32-bit checksums. The implementation uses
no lookup tables, keeping its static-data requirements small and predictable.

- **Three API levels** — named wrappers cover common standards. Generic one-shot
  functions (`nx_crc8_compute`, `nx_crc16_compute`, and `nx_crc32_compute`)
  accept Rocksoft model parameters for other variants. The incremental API
  (`nx_crc_init`, `nx_crc_update`, and `nx_crc_final`) handles data that arrives
  in chunks and produces the same result as a one-shot call.
- **Standard variants included** — CRC-8, CRC-8/ITU, CRC-8/ROHC, CRC-8/MAXIM;
  CRC-16/IBM, CRC-16/MAXIM, CRC-16/USB, CRC-16/MODBUS, CRC-16/CCITT,
  CRC-16/CCITT-FALSE, CRC-16/X25, CRC-16/XMODEM; and CRC-32 and
  CRC-32/MPEG-2. The header documents each variant's parameters and check value,
  defined as the CRC of `"123456789"`.
- **Table-free core** — one bitwise implementation handles every supported width
  and `refin` / `refout` combination without a polynomial lookup table.
- **Caller-owned state and defined NULL behavior** — the module uses no dynamic
  memory. `nx_crc_init` and `nx_crc_update` ignore a `NULL` context;
  `nx_crc_update` also ignores a `NULL` data pointer, and
  `nx_crc_final(NULL)` returns 0.

```c
#include "nx_crc.h"

const char *msg = "123456789";

/* Named standard variants. */
uint16_t c1 = nx_crc16_modbus(msg, 9);      /* 0x4B37 */
uint32_t c2 = nx_crc32(msg, 9);             /* 0xCBF43926 */

/* A generic computation with the CRC-16/MODBUS parameters. */
uint16_t c3 = nx_crc16_compute(msg, 9,
                               0x8005,      /* poly   */
                               0xFFFF,      /* init   */
                               true, true,  /* refin, refout */
                               0x0000);     /* xorout */
/* c3 == c1 */

/* The same CRC, supplied in two chunks. */
nx_crc_ctx_t ctx;
nx_crc_init(&ctx, 16, 0x8005, 0xFFFF, true, true, 0x0000);
nx_crc_update(&ctx, msg, 4);                /* "1234"  */
nx_crc_update(&ctx, msg + 4, 5);            /* "56789" */
uint16_t c4 = (uint16_t)nx_crc_final(&ctx); /* == c1 */
```


## nx_sha256 — SHA-256 cryptographic hash

A pure-C SHA-256 (FIPS 180-4) implementation producing a 32-byte digest.

- **One-shot and incremental APIs** — `nx_sha256` hashes a complete buffer in one
  call. `nx_sha256_init`, `nx_sha256_update`, and `nx_sha256_final` process data
  in chunks and produce the same digest.
- **Fixed, caller-owned state** — an incremental computation uses one
  caller-owned `nx_sha256_ctx_t`, which may be allocated on the stack. The
  implementation uses no dynamic memory and only a fixed table of round
  constants.
- **Defined NULL behavior** — a `NULL` data pointer contributes no bytes.
  Functions return safely without producing output when a required context or
  digest pointer is `NULL`.
- **Hash only, not a MAC** — SHA-256 alone does not authenticate a message. Use
  an established HMAC-SHA-256 implementation when authentication is required.

```c
#include "nx_sha256.h"

uint8_t digest[NX_SHA256_DIGEST_SIZE];

/* One-shot computation. */
nx_sha256("abc", 3, digest);
/* digest = ba7816bf 8f01cfea ... f20015ad */

/* The same digest, supplied in two chunks. */
nx_sha256_ctx_t ctx;
nx_sha256_init(&ctx);
nx_sha256_update(&ctx, "a", 1);
nx_sha256_update(&ctx, "bc", 2);
nx_sha256_final(&ctx, digest);
```


## Usage

The algorithm modules are under `src/algo/`. Copy the required `.c` and `.h`
files into your project and add their directory to the header search path.

The `examples/algo/` directory contains runnable examples built through CMake.

### Build and run the examples

From the repository root:

```sh
cmake -S . -B build
cmake --build build
```
