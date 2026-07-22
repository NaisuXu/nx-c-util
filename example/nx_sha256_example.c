/**
 * @file    nx_sha256_example.c
 * @brief   Usage examples for nx_sha256 (SHA-256 cryptographic hash).
 *
 * Demonstrates the two ways to hash and shows they agree:
 *   1. One-shot: hash a whole buffer in a single call.
 *   2. Incremental: feed the same data in several chunks and get the same digest.
 *
 * ----------------------------------------------------------------------------
 * To verify your own data: change INPUT_TEXT below to whatever you want to hash,
 * then rebuild and run. The program prints the digest as hex; compare it against
 * a known-good tool, e.g. on a shell:
 *     printf '%s' "your text" | sha256sum
 * ----------------------------------------------------------------------------
 */
#include "nx_c_util_examples.h"
#include "nx_sha256.h"

#include <stdio.h>
#include <string.h>

/* >>> Change this to the data you want to hash and verify. <<< */
#define INPUT_TEXT "abc"

/* Print a digest as lowercase hex on one line. */
static void print_hex(const char *label, const uint8_t *digest)
{
    printf("  %s", label);
    for (size_t i = 0; i < NX_SHA256_DIGEST_SIZE; i++) {
        printf("%02x", digest[i]);
    }
    printf("\n");
}

int nx_sha256_example_run(void)
{
    const char  *text = INPUT_TEXT;
    const size_t len  = strlen(text);
    int          rc   = 0;

    printf("########## nx_sha256 examples ##########\n");
    printf("input = \"%s\" (%zu bytes)\n\n", text, len);

    /* ---- 1. one-shot: hash the whole buffer at once ---- */
    printf("Example 1: one-shot hash\n");
    uint8_t digest_oneshot[NX_SHA256_DIGEST_SIZE];
    nx_sha256(text, len, digest_oneshot);
    print_hex("sha256 = ", digest_oneshot);
    printf("\n");

    /* ---- 2. incremental: feed the same data in two chunks ---- */
    printf("Example 2: incremental hash (fed in chunks)\n");
    uint8_t digest_stream[NX_SHA256_DIGEST_SIZE];
    nx_sha256_ctx_t ctx;
    nx_sha256_init(&ctx);
    /* Split the input roughly in half to show chunking; any split works. */
    size_t first = len / 2u;
    nx_sha256_update(&ctx, text, first);
    nx_sha256_update(&ctx, text + first, len - first);
    nx_sha256_final(&ctx, digest_stream);
    print_hex("sha256 = ", digest_stream);
    printf("\n");

    /* ---- 3. the two methods must agree ---- */
    printf("Example 3: one-shot and incremental digests match\n");
    if (memcmp(digest_oneshot, digest_stream, NX_SHA256_DIGEST_SIZE) == 0) {
        printf("  match: yes\n");
    } else {
        printf("  match: NO (this should never happen)\n");
        rc = 1;
    }
    printf("\n");

    return rc;
}
