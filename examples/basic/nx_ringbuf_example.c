/**
 * @file    nx_ringbuf_example.c
 * @brief   Usage examples for the nx_ringbuf byte ring buffer.
 *
 * Demonstrates:
 *   1. Basic byte-stream write/read with partial transfers when nearly full.
 *   2. peek (look-ahead) vs read, and discard.
 *   3. DMA-style zero-copy fill via poke_linear + commit, drain via
 *      peek_linear + discard (the contiguous-region helpers).
 *
 * The ring buffer never allocates: every example provides its own static buffer.
 */
#include "nx_basic_examples.h"
#include "core/nx_ringbuf.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Example 1: byte stream with a partial write when nearly full       */
/* ------------------------------------------------------------------ */
static void example_basic_stream(void)
{
    printf("Example 1: byte stream write/read (partial when full)\n");

    uint8_t       storage[8];   /* caller-owned backing storage */
    nx_ringbuf_t  rb;
    nx_ringbuf_init(&rb, storage, sizeof(storage));

    /* Only 8 bytes fit; a 10-byte write is truncated to a partial write. */
    size_t w = nx_ringbuf_write(&rb, "ABCDEFGHIJ", 10);
    printf("  wrote %zu of 10 bytes (free was %zu)\n", w, sizeof(storage));

    /* Read 5 bytes back out. */
    char out[16] = {0};
    size_t r = nx_ringbuf_read(&rb, out, 5);
    printf("  read %zu bytes: \"%s\", %zu remain\n", r, out, nx_ringbuf_size(&rb));
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: peek (look-ahead) vs read, and discard                  */
/* ------------------------------------------------------------------ */
static void example_peek_and_discard(void)
{
    printf("Example 2: peek vs read, and discard\n");

    uint8_t       storage[16];
    nx_ringbuf_t  rb;
    nx_ringbuf_init(&rb, storage, sizeof(storage));

    nx_ringbuf_write(&rb, "header:body", 11);

    /* peek copies without consuming. */
    char hdr[8] = {0};
    nx_ringbuf_peek(&rb, hdr, 6);
    printf("  peek 6: \"%s\" (size still %zu)\n", hdr, nx_ringbuf_size(&rb));

    /* discard the header, then read the rest. */
    nx_ringbuf_discard(&rb, 7);   /* drop "header:" */
    char body[8] = {0};
    size_t n = nx_ringbuf_read(&rb, body, sizeof(body) - 1);
    printf("  after discarding 7, read %zu: \"%s\"\n", n, body);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: DMA-style zero-copy via the linear-region helpers       */
/* ------------------------------------------------------------------ */
static void example_dma_linear(void)
{
    printf("Example 3: zero-copy fill/drain (DMA-style linear regions)\n");

    uint8_t       storage[8];
    nx_ringbuf_t  rb;
    nx_ringbuf_init(&rb, storage, sizeof(storage));

    /* A "DMA receive" fills the contiguous free region directly, then commits.
     * (Here we memcpy in place of a real DMA engine.) */
    size_t seg = 0;
    uint8_t *dst = nx_ringbuf_poke_linear(&rb, &seg);
    if (dst != NULL) {
        size_t n = (seg < 4U) ? seg : 4U;
        memcpy(dst, "wxyz", n);          /* stand-in for DMA into the buffer */
        size_t c = nx_ringbuf_commit(&rb, n);
        printf("  poke gave %zu contiguous bytes, committed %zu\n", seg, c);
    }

    /* A "DMA transmit" reads straight from the contiguous readable region. */
    const uint8_t *src = nx_ringbuf_peek_linear(&rb, &seg);
    if (src != NULL) {
        printf("  peek_linear exposes %zu contiguous bytes: \"%.*s\"\n",
               seg, (int)seg, src);
        nx_ringbuf_discard(&rb, seg);    /* mark them consumed after the "DMA" */
    }
    printf("  size after drain: %zu\n\n", nx_ringbuf_size(&rb));
}

int nx_ringbuf_example_run(void)
{
    printf("########## nx_ringbuf examples ##########\n");
    example_basic_stream();
    example_peek_and_discard();
    example_dma_linear();
    return 0;
}
