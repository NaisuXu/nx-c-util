# Algorithm Modules
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
#include "nx_crc.h"

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
#include "nx_sha256.h"

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

The library sources are organized by category (`core/`, `middleware/`, `algo/`) 
and can be dropped directly into your project — just compile the `.c` files and 
add the project root to your include path with `-I.` so directory-prefixed 
includes like `#include "nx_list.h"` work.

The `examples/basic/` directory contains runnable usage examples for every module,
driven through CMake so they build the same way on any platform.
