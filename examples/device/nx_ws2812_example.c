/**
 * @file    nx_ws2812_example.c
 * @brief   Usage examples for the nx_ws2812 LED strip driver.
 *
 * Demonstrates:
 *   1. Init + per-pixel colors, and what the bit-expanded wire stream looks like.
 *   2. Segment fills (nx_ws2812_fill) and the whole-strip shorthand.
 *   3. Marquee shifting from either end (nx_ws2812_push / _push_tail).
 *   4. Global brightness, including that it never damages the stored colors.
 *   5. Range checking.
 *   6. Non-blocking update: BUSY vs the error codes, and polling nx_ws2812_busy.
 *
 * There is no real LED strip here, so the write callback captures the encoded
 * bytes into a static buffer that the example then inspects - which doubles as a
 * check that the encoding is what WS2812 expects.
 */
#include "device/nx_ws2812.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define LED_COUNT 8U

/* SPI, MSB-first @ ~3.2 MHz reference patterns from the header. */
#define BIT0_PATTERN 0xC0U
#define BIT1_PATTERN 0xF8U

/* Trailing zero bytes that latch the frame; see nx_ws2812_cfg_t.reset_bytes. */
#define RESET_BYTES 50U

/* The size macros are constant expressions, so they can size static arrays. */
#define TX_BUF_SIZE NX_WS2812_TX_BUF_SIZE(LED_COUNT, RESET_BYTES)

/* ------------------------------------------------------------------ */
/* Fake peripheral: records the last transfer instead of driving pins  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t last[TX_BUF_SIZE];
    size_t  last_len;
    size_t  write_count;
    int     busy_countdown;   /* pretend to be busy for N polls */
    bool    fail_next;        /* make the next write report an error */
} fake_spi_t;

static bool fake_spi_write(const uint8_t *data, size_t len, void *arg)
{
    fake_spi_t *spi = (fake_spi_t *)arg;

    if (spi->fail_next) {
        spi->fail_next = false;
        return false;         /* simulate a peripheral error */
    }
    if (len > sizeof(spi->last)) {
        return false;
    }
    memcpy(spi->last, data, len);
    spi->last_len = len;
    spi->write_count++;

    return true;
}

static bool fake_spi_busy(void *arg)
{
    fake_spi_t *spi = (fake_spi_t *)arg;

    if (spi->busy_countdown > 0) {
        spi->busy_countdown--;
        return true;    /* still transmitting */
    }
    return false;
}

/* Caller-owned storage: the driver allocates nothing. */
static uint8_t   g_pixels[NX_WS2812_PIXEL_BUF_SIZE(LED_COUNT)];
static uint8_t   g_tx[TX_BUF_SIZE];
static fake_spi_t g_spi;

/* Decode one LED's 24 expanded bytes back into GRB, to verify the encoding. */
static void decode_led(const uint8_t *stream, size_t index,
                       uint8_t *out_g, uint8_t *out_r, uint8_t *out_b)
{
    const uint8_t *p = &stream[index * 24U];
    uint8_t        ch[3] = { 0U, 0U, 0U };

    for (size_t c = 0U; c < 3U; c++) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            ch[c] = (uint8_t)(ch[c] << 1);
            if (p[c * 8U + bit] == BIT1_PATTERN) {
                ch[c] |= 1U;
            }
        }
    }

    *out_g = ch[0];
    *out_r = ch[1];
    *out_b = ch[2];
}

/* ------------------------------------------------------------------ */
/* Example 1: init, set pixels, inspect the encoded stream            */
/* ------------------------------------------------------------------ */
static void example_basic(nx_ws2812_t *strip)
{
    printf("Example 1: per-pixel colors and the encoded wire stream\n");

    nx_ws2812_set_pixel(strip, 0, 255, 0, 0);     /* red   */
    nx_ws2812_set_pixel(strip, 1, 0, 255, 0);     /* green */
    nx_ws2812_set_pixel(strip, 2, 0, 0, 255);     /* blue  */

    assert(nx_ws2812_update(strip));

    printf("  %u LEDs -> %zu bytes on the wire (24 per LED + %u reset)\n",
           LED_COUNT, g_spi.last_len, RESET_BYTES);
    assert(g_spi.last_len == TX_BUF_SIZE);

    /* LED 0 is red: green byte 0x00 expands to eight bit0 patterns, red 0xFF to
     * eight bit1 patterns. Show the first 16 bytes to make that visible. */
    printf("  LED0 (red) first 16 encoded bytes:");
    for (size_t i = 0U; i < 16U; i++) {
        printf(" %02X", g_spi.last[i]);
    }
    printf("\n");

    uint8_t r, g, b;
    decode_led(g_spi.last, 0, &g, &r, &b);
    printf("  decoded LED0 = R%u G%u B%u\n", r, g, b);
    assert(r == 255U && g == 0U && b == 0U);

    decode_led(g_spi.last, 1, &g, &r, &b);
    printf("  decoded LED1 = R%u G%u B%u\n", r, g, b);
    assert(r == 0U && g == 255U && b == 0U);

    /* The tail is the latch gap: all zeros. */
    assert(g_spi.last[LED_COUNT * 24U] == 0x00U);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: segment fills                                           */
/* ------------------------------------------------------------------ */
static void example_fill(nx_ws2812_t *strip)
{
    printf("Example 2: segment fills\n");

    nx_ws2812_clear(strip);
    nx_ws2812_fill(strip, 0, 4, 255, 128, 0);   /* first half amber */
    nx_ws2812_fill(strip, 4, 4, 0, 0, 255);     /* second half blue */

    uint8_t r, g, b;
    nx_ws2812_get_pixel(strip, 3, &r, &g, &b);
    printf("  LED3 = R%u G%u B%u (amber segment)\n", r, g, b);
    assert(r == 255U && g == 128U && b == 0U);

    nx_ws2812_get_pixel(strip, 4, &r, &g, &b);
    printf("  LED4 = R%u G%u B%u (blue segment)\n", r, g, b);
    assert(r == 0U && g == 0U && b == 255U);

    /* set_all is the whole-strip shorthand for fill. */
    nx_ws2812_set_all(strip, 10, 20, 30);
    nx_ws2812_get_pixel(strip, 7, &r, &g, &b);
    printf("  after set_all, LED7 = R%u G%u B%u\n", r, g, b);
    assert(r == 10U && g == 20U && b == 30U);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: marquee shifting from either end                        */
/* ------------------------------------------------------------------ */

/* Print the strip as one letter per LED, so a shift is visible at a glance. */
static void print_strip(const nx_ws2812_t *strip, const char *label)
{
    printf("  %-18s", label);
    for (size_t i = 0U; i < LED_COUNT; i++) {
        uint8_t r, g, b;
        nx_ws2812_get_pixel(strip, i, &r, &g, &b);

        char c = '.';                                   /* off        */
        if (r && !g && !b)      { c = 'R'; }            /* red        */
        else if (!r && g && !b) { c = 'G'; }            /* green      */
        else if (!r && !g && b) { c = 'B'; }            /* blue       */
        else if (r || g || b)   { c = '?'; }            /* mixed      */
        printf(" %c", c);
    }
    printf("\n");
}

static void example_push(nx_ws2812_t *strip)
{
    printf("Example 3: push new LEDs in at either end (marquee)\n");

    nx_ws2812_clear(strip);
    print_strip(strip, "cleared:");

    /* One red LED enters at index 0; everything else slides one step away. */
    nx_ws2812_push(strip, 1, 255, 0, 0);
    print_strip(strip, "push 1 red:");

    uint8_t r, g, b;
    nx_ws2812_get_pixel(strip, 0, &r, &g, &b);
    assert(r == 255U && g == 0U && b == 0U);

    /* Two green LEDs push the red one along to index 2. */
    nx_ws2812_push(strip, 2, 0, 255, 0);
    print_strip(strip, "push 2 green:");

    nx_ws2812_get_pixel(strip, 2, &r, &g, &b);
    printf("  the red LED moved to index 2: R%u G%u B%u\n", r, g, b);
    assert(r == 255U && g == 0U && b == 0U);

    /* Keep pushing blue and the earlier colors fall off the far end. */
    for (size_t i = 0U; i < 6U; i++) {
        nx_ws2812_push(strip, 1, 0, 0, 255);
    }
    print_strip(strip, "push 6 blue:");

    nx_ws2812_get_pixel(strip, LED_COUNT - 1U, &r, &g, &b);
    printf("  tail LED is now green (red was discarded): R%u G%u B%u\n", r, g, b);
    assert(r == 0U && g == 255U && b == 0U);

    /* A push of count >= led_count replaces the whole strip. */
    nx_ws2812_push(strip, LED_COUNT + 4U, 255, 0, 0);
    print_strip(strip, "push 12 red:");
    nx_ws2812_get_pixel(strip, LED_COUNT - 1U, &r, &g, &b);
    assert(r == 255U && g == 0U && b == 0U);

    /* count == 0 changes nothing. */
    assert(nx_ws2812_push(strip, 0, 0, 255, 0));
    nx_ws2812_get_pixel(strip, 0, &r, &g, &b);
    assert(r == 255U && g == 0U && b == 0U);
    printf("  push 0 is a no-op\n");

    /* push_tail is the mirror image: colors slide back toward the controller. */
    nx_ws2812_clear(strip);
    nx_ws2812_push_tail(strip, 1, 255, 0, 0);
    print_strip(strip, "push_tail 1 red:");
    nx_ws2812_get_pixel(strip, LED_COUNT - 1U, &r, &g, &b);
    assert(r == 255U && g == 0U && b == 0U);

    nx_ws2812_push_tail(strip, 2, 0, 255, 0);
    print_strip(strip, "push_tail 2 green:");
    nx_ws2812_get_pixel(strip, LED_COUNT - 3U, &r, &g, &b);
    printf("  the red LED moved to index %u: R%u G%u B%u\n",
           LED_COUNT - 3U, r, g, b);
    assert(r == 255U && g == 0U && b == 0U);

    /* The two are inverses: push then push_tail restores the layout. */
    nx_ws2812_push(strip, 2, 0, 0, 255);
    print_strip(strip, "push 2 blue:");
    nx_ws2812_push_tail(strip, 2, 0, 0, 255);
    print_strip(strip, "push_tail 2 blue:");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 4: brightness scaling is lossless for stored colors        */
/* ------------------------------------------------------------------ */
static void example_brightness(nx_ws2812_t *strip)
{
    printf("Example 4: global brightness\n");

    nx_ws2812_set_all(strip, 255, 255, 255);
    assert(nx_ws2812_get_brightness(strip) == 255U);

    uint8_t r, g, b;

    /* Half brightness: the wire values drop... */
    nx_ws2812_set_brightness(strip, 128);
    assert(nx_ws2812_update(strip));
    decode_led(g_spi.last, 0, &g, &r, &b);
    printf("  brightness 128 -> wire R%u G%u B%u\n", r, g, b);
    assert(r == 128U && g == 128U && b == 128U);

    /* ...but the pixel buffer still holds the original full-range color. */
    nx_ws2812_get_pixel(strip, 0, &r, &g, &b);
    printf("  stored color unchanged: R%u G%u B%u\n", r, g, b);
    assert(r == 255U && g == 255U && b == 255U);

    /* So dimming and restoring round-trips exactly - no accumulated error. */
    nx_ws2812_set_brightness(strip, 32);
    assert(nx_ws2812_update(strip));
    nx_ws2812_set_brightness(strip, 255);
    assert(nx_ws2812_update(strip));
    decode_led(g_spi.last, 0, &g, &r, &b);
    printf("  after 128 -> 32 -> 255, wire R%u G%u B%u\n", r, g, b);
    assert(r == 255U && g == 255U && b == 255U);

    /* Brightness 0 blacks out the strip without touching stored colors. */
    nx_ws2812_set_brightness(strip, 0);
    assert(nx_ws2812_update(strip));
    decode_led(g_spi.last, 0, &g, &r, &b);
    printf("  brightness 0 -> wire R%u G%u B%u\n", r, g, b);
    assert(r == 0U && g == 0U && b == 0U);

    nx_ws2812_set_brightness(strip, 255);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 5: range checks                                            */
/* ------------------------------------------------------------------ */
static void example_guards(nx_ws2812_t *strip)
{
    printf("Example 5: range checks\n");

    /* Out-of-range writes are rejected rather than clamped. */
    printf("  set_pixel(index=%u): %s\n", LED_COUNT,
           nx_ws2812_set_pixel(strip, LED_COUNT, 1, 2, 3) ? "ok" : "rejected");
    assert(!nx_ws2812_set_pixel(strip, LED_COUNT, 1, 2, 3));

    printf("  fill(first=6, count=4) past the end: %s\n",
           nx_ws2812_fill(strip, 6, 4, 1, 2, 3) ? "ok" : "rejected");
    assert(!nx_ws2812_fill(strip, 6, 4, 1, 2, 3));

    /* An empty range is a no-op, not an error. */
    printf("  fill(first=0, count=0): %s\n",
           nx_ws2812_fill(strip, 0, 0, 1, 2, 3) ? "ok (no-op)" : "rejected");
    assert(nx_ws2812_fill(strip, 0, 0, 1, 2, 3));

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 6: non-blocking update and the busy handshake              */
/* ------------------------------------------------------------------ */
static void example_busy(nx_ws2812_t *strip)
{
    printf("Example 6: non-blocking update\n");

    /* Idle peripheral: the frame goes out immediately. */
    assert(!nx_ws2812_busy(strip));
    bool ok = nx_ws2812_update(strip);
    printf("  idle    -> update returned %s, busy now %s\n",
           ok ? "true" : "false", nx_ws2812_busy(strip) ? "true" : "false");
    assert(ok);

    /* Pretend the transfer takes 3 more polls to drain. update must refuse
     * rather than spin, and must not have called write_fn. */
    g_spi.busy_countdown = 3;
    size_t before = g_spi.write_count;

    ok = nx_ws2812_update(strip);
    printf("  busy    -> update returned %s, write_fn calls %zu -> %zu\n",
           ok ? "true" : "false", before, g_spi.write_count);
    assert(!ok);
    assert(g_spi.write_count == before);   /* nothing was sent */

    /* A caller polls on its own terms instead of blocking inside the driver. */
    size_t polls = 0U;
    while (nx_ws2812_busy(strip)) {
        polls++;                            /* real code would do other work */
    }
    printf("  drained after %zu polls of nx_ws2812_busy\n", polls);

    ok = nx_ws2812_update(strip);
    printf("  retry   -> update returned %s, write_fn calls now %zu\n",
           ok ? "true" : "false", g_spi.write_count);
    assert(ok);
    assert(g_spi.write_count == before + 1U);

    /* Errors stay distinguishable from BUSY by checking nx_ws2812_busy first. */
    nx_ws2812_t uninit = { 0 };
    ok = nx_ws2812_update(&uninit);
    printf("  uninit  -> update returned %s (busy=%s, so it's a param error)\n",
           ok ? "true" : "false", nx_ws2812_busy(&uninit) ? "true" : "false");
    assert(!ok);
    assert(!nx_ws2812_busy(&uninit));

    g_spi.fail_next = true;
    ok = nx_ws2812_update(strip);
    printf("  io fail -> update returned %s (busy=%s, so it's an IO error)\n",
           ok ? "true" : "false", nx_ws2812_busy(strip) ? "true" : "false");
    assert(!ok);
    assert(!nx_ws2812_busy(strip));
    printf("\n");
}

int main(void)
{
    printf("########## nx_ws2812 examples ##########\n");

    memset(&g_spi, 0, sizeof(g_spi));

    static const nx_ws2812_cfg_t cfg = {
        .led_count    = LED_COUNT,
        .reset_bytes  = RESET_BYTES,
        .bit0_pattern = BIT0_PATTERN,
        .bit1_pattern = BIT1_PATTERN,
        .write_fn     = fake_spi_write,
        .busy_fn      = fake_spi_busy,
        .write_arg    = &g_spi,
        .busy_arg     = &g_spi,
    };

    nx_ws2812_t strip;
    if (!nx_ws2812_init(&strip, &cfg, g_pixels, g_tx)) {
        printf("init failed\n");
        return 1;
    }

    example_basic(&strip);
    example_fill(&strip);
    example_push(&strip);
    example_brightness(&strip);
    example_guards(&strip);
    example_busy(&strip);

    return 0;
}
