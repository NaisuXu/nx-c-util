/**
 * @file    nx_ssd1306_example.c
 * @brief   Usage examples for the nx_ssd1306 OLED controller driver.
 *
 * Demonstrates:
 *   1. Init: the panel sequence on the wire, and why every page starts pending.
 *   2. Pixel access and the page-oriented framebuffer layout.
 *   3. Page-at-a-time flushing with nx_ssd1306_process, including round-robin resume.
 *   4. Direct framebuffer drawing declared with nx_ssd1306_touch.
 *   5. Deferring while the bus is still transmitting.
 *   6. A failed write leaving the page pending so it gets retried.
 *   7. A second panel descriptor, showing what geometry changes.
 *   8. The command wrappers: contrast, inversion, sleep, orientation, raw commands.
 *   9. Range checking and uninitialized handles.
 *
 * There is no real panel here, so the write callbacks capture command and pixel
 * traffic into separate buffers that the example then inspects - which doubles as
 * a check that the byte stream is what an SSD1306 expects.
 */
#include "nx_device_examples.h"
#include "src/device/nx_ssd1306.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define PANEL_WIDTH   128U
#define PANEL_HEIGHT   64U

/* ------------------------------------------------------------------ */
/* Fake bus: records the last transfer instead of driving I2C or SPI   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t cmd[32];        /* last command burst */
    size_t  cmd_len;
    size_t  cmd_count;
    uint8_t data[PANEL_WIDTH];  /* last pixel burst, at most one page */
    size_t  data_len;
    size_t  data_count;
    int     busy_countdown; /* pretend to be transmitting for N polls */
    bool    fail_cmd;       /* make the next command write report an error */
    bool    fail_data;      /* make the next pixel write report an error */
} fake_bus_t;

/*
 * A real implementation picks the command selector here: an I2C driver prefixes
 * control byte 0x00, a 4-wire SPI driver holds D/C low.
 */
static bool fake_write_cmd(void *ctx, const uint8_t *buf, size_t len)
{
    fake_bus_t *bus = (fake_bus_t *)ctx;

    if (bus->fail_cmd) {
        bus->fail_cmd = false;
        return false;         /* simulate a bus error */
    }
    if (len > sizeof(bus->cmd)) {
        return false;
    }
    memcpy(bus->cmd, buf, len);
    bus->cmd_len = len;
    bus->cmd_count++;

    return true;
}

/* Same contract with the data selector instead: I2C 0x40, or D/C high. */
static bool fake_write_data(void *ctx, const uint8_t *buf, size_t len)
{
    fake_bus_t *bus = (fake_bus_t *)ctx;

    if (bus->fail_data) {
        bus->fail_data = false;
        return false;
    }
    if (len > sizeof(bus->data)) {
        return false;
    }
    memcpy(bus->data, buf, len);
    bus->data_len = len;
    bus->data_count++;

    return true;
}

/* Each poll consumes one tick of the countdown, so a pending transfer drains. */
static bool fake_bus_busy(void *ctx)
{
    fake_bus_t *bus = (fake_bus_t *)ctx;

    if (bus->busy_countdown > 0) {
        bus->busy_countdown--;
        return true;    /* still transmitting */
    }
    return false;
}

/* Caller-owned storage: the driver allocates nothing. */
static uint8_t    g_fb[NX_SSD1306_FB_SIZE(PANEL_WIDTH, PANEL_HEIGHT)];
static uint8_t    g_fb32[NX_SSD1306_FB_SIZE(128, 32)];
static fake_bus_t g_bus;

/* ------------------------------------------------------------------ */
/* Example 1: what nx_ssd1306_init puts on the bus                     */
/* ------------------------------------------------------------------ */

static void example_init(nx_ssd1306_t *dev)
{
    printf("Example 1: init sequence and the first full repaint\n");

    /* The whole descriptor sequence goes out in a single command write. */
    assert(g_bus.cmd_count == 1U);
    assert(g_bus.cmd_len >= 3U);
    assert(g_bus.cmd[0] == 0xAEU);                     /* display off first */
    assert(g_bus.cmd[g_bus.cmd_len - 1U] == 0xAFU);    /* display on last */
    assert(g_bus.cmd[1] == 0x20U && g_bus.cmd[2] == 0x02U);  /* page addressing */
    assert(g_bus.data_count == 0U);                    /* no pixels sent yet */

    assert(nx_ssd1306_width(dev) == PANEL_WIDTH);
    assert(nx_ssd1306_height(dev) == PANEL_HEIGHT);

    /* Controller RAM is undefined after power-up, so every page starts pending. */
    assert(nx_ssd1306_dirty(dev));

    /* The framebuffer itself starts blank. */
    const uint8_t *fb = nx_ssd1306_framebuffer(dev);
    assert(fb != NULL);
    for (size_t i = 0; i < NX_SSD1306_FB_SIZE(PANEL_WIDTH, PANEL_HEIGHT); i++) {
        assert(fb[i] == 0x00U);
    }

    printf("  %zu command bytes in %zu write, %zu pages pending\n",
           g_bus.cmd_len, g_bus.cmd_count,
           (size_t)(PANEL_HEIGHT / NX_SSD1306_PAGE_HEIGHT));
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: pixel access and the framebuffer layout                  */
/* ------------------------------------------------------------------ */

static void example_pixels(nx_ssd1306_t *dev)
{
    printf("Example 2: pixels and the page-oriented layout\n");

    assert(nx_ssd1306_clear(dev) == NX_SSD1306_OK);

    uint8_t  width = nx_ssd1306_width(dev);
    uint8_t *fb    = nx_ssd1306_framebuffer(dev);
    assert(fb != NULL);

    assert(nx_ssd1306_set_pixel(dev, 0, 0, true) == NX_SSD1306_OK);
    assert(nx_ssd1306_set_pixel(dev, 3, 9, true) == NX_SSD1306_OK);
    assert(nx_ssd1306_set_pixel(dev, 127, 63, true) == NX_SSD1306_OK);

    /* Byte (y / 8) * width + x holds 8 stacked rows, with row y in bit y % 8. */
    assert(fb[0] == 0x01U);
    assert(fb[(size_t)1 * width + 3] == 0x02U);
    assert(fb[(size_t)7 * width + 127] == 0x80U);

    /* Reading back reports what the next flush will send. */
    assert(nx_ssd1306_get_pixel(dev, 3, 9));
    assert(!nx_ssd1306_get_pixel(dev, 3, 10));

    assert(nx_ssd1306_set_pixel(dev, 3, 9, false) == NX_SSD1306_OK);
    assert(!nx_ssd1306_get_pixel(dev, 3, 9));

    /* Filling covers every byte; clearing is the same call with false. */
    assert(nx_ssd1306_fill(dev, true) == NX_SSD1306_OK);
    assert(nx_ssd1306_get_pixel(dev, 64, 33));
    assert(fb[0] == 0xFFU);
    assert(nx_ssd1306_clear(dev) == NX_SSD1306_OK);
    assert(!nx_ssd1306_get_pixel(dev, 64, 33));

    printf("  framebuffer %zu bytes for %ux%u, 8 rows per byte\n",
           NX_SSD1306_FB_SIZE(PANEL_WIDTH, PANEL_HEIGHT),
           (unsigned)width, (unsigned)nx_ssd1306_height(dev));
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: one page per nx_ssd1306_process call                     */
/* ------------------------------------------------------------------ */

static void example_process(nx_ssd1306_t *dev)
{
    printf("Example 3: one page per process call\n");

    /* Start from a known state: whole screen lit, every page pending. */
    assert(nx_ssd1306_fill(dev, true) == NX_SSD1306_OK);
    assert(nx_ssd1306_dirty(dev));

    g_bus.cmd_count  = 0;
    g_bus.data_count = 0;

    uint8_t width = nx_ssd1306_width(dev);
    uint8_t pages = (uint8_t)(nx_ssd1306_height(dev) / NX_SSD1306_PAGE_HEIGHT);

    for (uint8_t p = 0U; p < pages; p++) {
        nx_ssd1306_ret_t ret = nx_ssd1306_process(dev);

        /* Address triplet: page select, then the low and high column nibbles. */
        assert(g_bus.cmd_len == 3U);
        assert(g_bus.cmd[0] == (uint8_t)(0xB0U | p));
        assert(g_bus.cmd[1] == 0x00U);
        assert(g_bus.cmd[2] == 0x10U);

        /* Exactly one page of pixels, so the cost per call stays bounded. */
        assert(g_bus.data_len == width);
        assert(g_bus.data[0] == 0xFFU);

        /* More to send reports BUSY; the last page reports OK. */
        assert(ret == ((p + 1 < pages) ? NX_SSD1306_BUSY : NX_SSD1306_OK));
    }

    assert(g_bus.cmd_count == pages);
    assert(g_bus.data_count == pages);
    assert(!nx_ssd1306_dirty(dev));

    /* With nothing pending, a call is cheap and sends nothing. */
    assert(nx_ssd1306_process(dev) == NX_SSD1306_OK);
    assert(g_bus.cmd_count == pages);

    /*
     * Round-robin resume: scanning picks up where the previous call stopped, so
     * a page redrawn every iteration cannot hold the others back.
     */
    assert(nx_ssd1306_set_pixel(dev, 0, 0, true) == NX_SSD1306_OK);    /* page 0 */
    assert(nx_ssd1306_set_pixel(dev, 0, 24, true) == NX_SSD1306_OK);   /* page 3 */

    assert(nx_ssd1306_process(dev) == NX_SSD1306_BUSY);
    assert(g_bus.cmd[0] == 0xB0U);                      /* page 0 */
    assert(nx_ssd1306_process(dev) == NX_SSD1306_OK);
    assert(g_bus.cmd[0] == (uint8_t)(0xB0U | 3U));      /* skipped straight to 3 */

    printf("  %u pages, %zu command writes and %zu data writes for a full frame\n",
           (unsigned)pages, g_bus.cmd_count - 2U, g_bus.data_count - 2U);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 4: drawing into the framebuffer directly                    */
/* ------------------------------------------------------------------ */

static void example_touch(nx_ssd1306_t *dev)
{
    printf("Example 4: direct framebuffer drawing and nx_ssd1306_touch\n");

    assert(nx_ssd1306_clear(dev) == NX_SSD1306_OK);
    assert(nx_ssd1306_flush(dev) == NX_SSD1306_OK);
    assert(!nx_ssd1306_dirty(dev));

    uint8_t  width = nx_ssd1306_width(dev);
    uint8_t *fb    = nx_ssd1306_framebuffer(dev);
    assert(fb != NULL);

    /* Rendering code can write the buffer itself; the tracking cannot see that. */
    memset(&fb[(size_t)1 * width], 0xAA, width);   /* rows 8..15 */
    assert(!nx_ssd1306_dirty(dev));

    /* Declaring the rows is what queues them. Rows round outward to whole pages. */
    nx_ssd1306_touch(dev, 8, 9);
    assert(nx_ssd1306_dirty(dev));

    g_bus.cmd_count  = 0;
    g_bus.data_count = 0;
    assert(nx_ssd1306_flush(dev) == NX_SSD1306_OK);
    assert(g_bus.cmd_count == 1U);                      /* one page went out */
    assert(g_bus.data_count == 1U);
    assert(g_bus.cmd[0] == (uint8_t)(0xB0U | 1U));
    assert(g_bus.data[0] == 0xAAU);

    /* An inverted range, or one starting past the bottom edge, is ignored. */
    nx_ssd1306_touch(dev, 10, 5);
    assert(!nx_ssd1306_dirty(dev));
    nx_ssd1306_touch(dev, 200, 210);
    assert(!nx_ssd1306_dirty(dev));

    /* A range running past the bottom is clamped to the last row. */
    nx_ssd1306_touch(dev, 56, 200);
    assert(nx_ssd1306_dirty(dev));
    g_bus.cmd_count = 0;
    assert(nx_ssd1306_flush(dev) == NX_SSD1306_OK);
    assert(g_bus.cmd_count == 1U);
    assert(g_bus.cmd[0] == (uint8_t)(0xB0U | 7U));

    printf("  rows 8..9 cost one page, rows 56..200 clamp to the last page\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 5: deferring while the bus is busy                          */
/* ------------------------------------------------------------------ */

static void example_busy(nx_ssd1306_t *dev)
{
    printf("Example 5: deferring while the bus is busy\n");

    /* nx_ssd1306_busy just reports the callback; each poll drains one tick. */
    g_bus.busy_countdown = 1;
    assert(nx_ssd1306_busy(dev));
    assert(!nx_ssd1306_busy(dev));

    assert(nx_ssd1306_fill(dev, false) == NX_SSD1306_OK);
    assert(nx_ssd1306_dirty(dev));

    g_bus.cmd_count      = 0;
    g_bus.data_count     = 0;
    g_bus.busy_countdown = 3;

    /* Busy means return immediately: nothing sent, nothing marked clean. */
    for (int i = 0; i < 3; i++) {
        assert(nx_ssd1306_process(dev) == NX_SSD1306_BUSY);
    }
    assert(g_bus.cmd_count == 0U);
    assert(g_bus.data_count == 0U);
    assert(nx_ssd1306_dirty(dev));

    /* Once the transfer drains, the next call resumes where it left off. */
    assert(nx_ssd1306_process(dev) == NX_SSD1306_BUSY);
    assert(g_bus.cmd_count == 1U);
    assert(nx_ssd1306_flush(dev) == NX_SSD1306_OK);
    assert(!nx_ssd1306_dirty(dev));

    printf("  3 busy polls sent 0 bytes; the update survived and completed\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 6: a rejected write is retried, not dropped                 */
/* ------------------------------------------------------------------ */

static void example_io_error(nx_ssd1306_t *dev)
{
    printf("Example 6: a failed write leaves the page pending\n");

    assert(nx_ssd1306_clear(dev) == NX_SSD1306_OK);
    assert(nx_ssd1306_flush(dev) == NX_SSD1306_OK);

    assert(nx_ssd1306_set_pixel(dev, 5, 20, true) == NX_SSD1306_OK);   /* page 2 */

    g_bus.cmd_count  = 0;
    g_bus.data_count = 0;
    g_bus.fail_data  = true;
    assert(nx_ssd1306_process(dev) == NX_SSD1306_ERR_IO);
    assert(g_bus.data_count == 0U);
    assert(nx_ssd1306_dirty(dev));   /* the page was not marked clean */

    /* The next call retries the same page rather than losing the update. */
    assert(nx_ssd1306_process(dev) == NX_SSD1306_OK);
    assert(g_bus.cmd[0] == (uint8_t)(0xB0U | 2U));
    assert(g_bus.data_count == 1U);
    assert(g_bus.data[5] == (uint8_t)(1U << 4));   /* row 20 is bit 4 of page 2 */
    assert(!nx_ssd1306_dirty(dev));

    printf("  write rejected once, page 2 resent on the next call\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 7: a different panel descriptor                             */
/* ------------------------------------------------------------------ */

static void example_panel_128x32(void)
{
    printf("Example 7: the 128x32 panel descriptor\n");

    fake_bus_t bus;
    memset(&bus, 0, sizeof(bus));

    /* No busy callback: correct for a bus whose writes complete on return. */
    nx_ssd1306_cfg_t cfg = {
        .panel      = &nx_ssd1306_panel_128x32,
        .write_cmd  = fake_write_cmd,
        .write_data = fake_write_data,
        .is_busy    = NULL,
        .io_ctx     = &bus,
    };

    nx_ssd1306_t small;
    assert(nx_ssd1306_init(&small, &cfg, g_fb32) == NX_SSD1306_OK);
    assert(nx_ssd1306_width(&small) == 128U);
    assert(nx_ssd1306_height(&small) == 32U);
    assert(!nx_ssd1306_busy(&small));   /* no callback, so never busy */

    /* Same command set; the descriptor carries the multiplex ratio for 32 rows. */
    assert(bus.cmd[3] == 0xA8U && bus.cmd[4] == 0x1FU);

    /* Four pages instead of eight, so a full frame is four calls. */
    assert(nx_ssd1306_fill(&small, true) == NX_SSD1306_OK);
    bus.cmd_count  = 0;
    bus.data_count = 0;
    assert(nx_ssd1306_flush(&small) == NX_SSD1306_OK);
    assert(bus.cmd_count == 4U);
    assert(bus.data_count == 4U);

    /* Rows past the panel are out of range. */
    assert(nx_ssd1306_set_pixel(&small, 0, 32, true) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_set_pixel(&small, 0, 31, true) == NX_SSD1306_OK);

    printf("  128x32: %zu framebuffer bytes, %zu writes per full frame\n",
           NX_SSD1306_FB_SIZE(128, 32), bus.data_count);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 8: the command wrappers                                     */
/* ------------------------------------------------------------------ */

static void example_commands(nx_ssd1306_t *dev)
{
    printf("Example 8: command wrappers\n");

    assert(nx_ssd1306_set_contrast(dev, 0x40) == NX_SSD1306_OK);
    assert(g_bus.cmd_len == 2U && g_bus.cmd[0] == 0x81U && g_bus.cmd[1] == 0x40U);

    /* Inversion happens as the controller scans, so the framebuffer is untouched. */
    assert(nx_ssd1306_invert(dev, true) == NX_SSD1306_OK);
    assert(g_bus.cmd_len == 1U && g_bus.cmd[0] == 0xA7U);
    assert(!nx_ssd1306_dirty(dev));
    assert(nx_ssd1306_invert(dev, false) == NX_SSD1306_OK);
    assert(g_bus.cmd[0] == 0xA6U);

    /* Sleep mode keeps RAM contents, so the image comes back as it was. */
    assert(nx_ssd1306_display_on(dev, false) == NX_SSD1306_OK);
    assert(g_bus.cmd_len == 1U && g_bus.cmd[0] == 0xAEU);
    assert(nx_ssd1306_display_on(dev, true) == NX_SSD1306_OK);
    assert(g_bus.cmd[0] == 0xAFU);

    /* Orientation: segment remap plus COM scan direction, one byte each. */
    assert(nx_ssd1306_flip(dev, false, false) == NX_SSD1306_OK);
    assert(g_bus.cmd_len == 2U && g_bus.cmd[0] == 0xA0U && g_bus.cmd[1] == 0xC0U);
    assert(nx_ssd1306_flip(dev, true, true) == NX_SSD1306_OK);
    assert(g_bus.cmd[0] == 0xA1U && g_bus.cmd[1] == 0xC8U);

    /* Raw commands cover whatever the wrappers do not, here the start line. */
    static const uint8_t start_line[] = { 0x40U };
    assert(nx_ssd1306_send_cmd(dev, start_line, sizeof(start_line)) == NX_SSD1306_OK);
    assert(g_bus.cmd_len == 1U && g_bus.cmd[0] == 0x40U);

    /* A command the bus rejects surfaces as an I/O error. */
    g_bus.fail_cmd = true;
    assert(nx_ssd1306_invert(dev, true) == NX_SSD1306_ERR_IO);

    printf("  contrast, inversion, sleep, orientation and raw commands verified\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 9: guards                                                   */
/* ------------------------------------------------------------------ */

static void example_guards(nx_ssd1306_t *dev)
{
    printf("Example 9: range checking and uninitialized handles\n");

    uint8_t width  = nx_ssd1306_width(dev);
    uint8_t height = nx_ssd1306_height(dev);

    assert(nx_ssd1306_set_pixel(dev, width, 0, true) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_set_pixel(dev, 0, height, true) == NX_SSD1306_ERR_PARAM);
    assert(!nx_ssd1306_get_pixel(dev, width, 0));
    assert(!nx_ssd1306_get_pixel(dev, 0, height));

    static const uint8_t one_cmd = 0xA6U;
    assert(nx_ssd1306_send_cmd(dev, NULL, 1U) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_send_cmd(dev, &one_cmd, 0U) == NX_SSD1306_ERR_PARAM);

    /* An uninitialized handle is rejected everywhere instead of trusted. */
    nx_ssd1306_t uninit = { 0 };
    assert(nx_ssd1306_width(&uninit) == 0U);
    assert(nx_ssd1306_height(&uninit) == 0U);
    assert(nx_ssd1306_framebuffer(&uninit) == NULL);
    assert(nx_ssd1306_process(&uninit) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_flush(&uninit) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_fill(&uninit, true) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_display_on(&uninit, true) == NX_SSD1306_ERR_PARAM);
    assert(!nx_ssd1306_dirty(&uninit));
    assert(!nx_ssd1306_busy(&uninit));
    assert(!nx_ssd1306_get_pixel(&uninit, 0, 0));
    nx_ssd1306_touch(&uninit, 0, 7);   /* ignored rather than a crash */

    /* Init insists on a panel, a framebuffer and both write callbacks. */
    nx_ssd1306_t tmp;
    nx_ssd1306_cfg_t missing = {
        .panel      = &nx_ssd1306_panel_128x64,
        .write_cmd  = NULL,
        .write_data = fake_write_data,
        .io_ctx     = &g_bus,
    };
    assert(nx_ssd1306_init(&tmp, &missing, g_fb) == NX_SSD1306_ERR_PARAM);
    assert(nx_ssd1306_init(NULL, &missing, g_fb) == NX_SSD1306_ERR_PARAM);

    /* Geometry outside the supported range is rejected before anything is sent. */
    static const uint8_t stub_seq[] = { 0xAFU };
    nx_ssd1306_panel_t odd = {
        .width      = 128U,
        .height     = 12U,          /* not a whole number of pages */
        .col_offset = 0U,
        .init_seq   = stub_seq,
        .init_len   = sizeof(stub_seq)
    };
    nx_ssd1306_cfg_t odd_cfg = {
        .panel      = &odd,
        .write_cmd  = fake_write_cmd,
        .write_data = fake_write_data,
        .io_ctx     = &g_bus,
    };
    assert(nx_ssd1306_init(&tmp, &odd_cfg, g_fb) == NX_SSD1306_ERR_PARAM);

    /* A rejected init sequence is reported rather than silently ignored. */
    nx_ssd1306_cfg_t good_cfg = {
        .panel      = &nx_ssd1306_panel_128x64,
        .write_cmd  = fake_write_cmd,
        .write_data = fake_write_data,
        .is_busy    = fake_bus_busy,
        .io_ctx     = &g_bus,
    };
    g_bus.fail_cmd = true;
    assert(nx_ssd1306_init(&tmp, &good_cfg, g_fb) == NX_SSD1306_ERR_IO);

    printf("  out-of-range coordinates, missing callbacks and bad geometry rejected\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int nx_ssd1306_example_run(void)
{
    printf("########## nx_ssd1306 examples ##########\n");

    memset(&g_bus, 0, sizeof(g_bus));

    static const nx_ssd1306_cfg_t cfg = {
        .panel      = &nx_ssd1306_panel_128x64,
        .write_cmd  = fake_write_cmd,
        .write_data = fake_write_data,
        .is_busy    = fake_bus_busy,
        .io_ctx     = &g_bus,
    };

    nx_ssd1306_t dev;
    if (nx_ssd1306_init(&dev, &cfg, g_fb) != NX_SSD1306_OK) {
        printf("init failed\n");
        return 1;
    }

    example_init(&dev);
    example_pixels(&dev);
    example_process(&dev);
    example_touch(&dev);
    example_busy(&dev);
    example_io_error(&dev);
    example_panel_128x32();
    example_commands(&dev);
    example_guards(&dev);

    return 0;
}
