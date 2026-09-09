/**
 * @file    nx_ssd1306.c
 * @brief   SSD1306 monochrome OLED controller driver implementation.
 */
#include "nx_ssd1306.h"

#include <string.h>

/* Command opcodes this driver emits. Panel descriptors carry the rest. */
#define CMD_DISPLAY_OFF     0xAEU
#define CMD_DISPLAY_ON      0xAFU
#define CMD_NORMAL_VIDEO    0xA6U
#define CMD_INVERT_VIDEO    0xA7U
#define CMD_SET_CONTRAST    0x81U
#define CMD_SEG_REMAP_OFF   0xA0U   /* framebuffer column 0 maps to panel column 0 */
#define CMD_SEG_REMAP_ON    0xA1U   /* framebuffer column 0 maps to the last column */
#define CMD_COM_SCAN_INC    0xC0U   /* framebuffer row 0 maps to panel row 0 */
#define CMD_COM_SCAN_DEC    0xC8U   /* framebuffer row 0 maps to the last row */
#define CMD_PAGE_START      0xB0U   /* OR'd with the page index */
#define CMD_COL_LOW         0x00U   /* OR'd with the low nibble of the column */
#define CMD_COL_HIGH        0x10U   /* OR'd with the high nibble of the column */

/*
 * Init sequences. Order matters: the panel is configured while the display is
 * off and only switched on once geometry, charge pump and addressing mode are
 * all set. The two descriptors differ in multiplex ratio (0xA8) and COM pin
 * layout (0xDA), which is what a 32-row panel needs; the rest is shared.
 */
static const uint8_t s_init_128x64[] = {
    CMD_DISPLAY_OFF,
    0x20, 0x02,         /* page addressing mode, which is what this driver emits */
    0xA8, 0x3F,         /* multiplex ratio: 64 rows */
    0xD3, 0x00,         /* display offset: none */
    0x40,               /* start line: 0 */
    CMD_SEG_REMAP_ON,   /* both axes mirrored, matching common breakout wiring */
    CMD_COM_SCAN_DEC,
    0xDA, 0x12,         /* COM pins: alternative, no left/right remap */
    CMD_SET_CONTRAST, 0xCF,
    0xD9, 0xF1,         /* pre-charge period */
    0xDB, 0x40,         /* VCOMH deselect level */
    0xD5, 0x80,         /* display clock: divide by 1, oscillator frequency 8 */
    0xA4,               /* show RAM contents, i.e. the framebuffer */
    CMD_NORMAL_VIDEO,
    0x8D, 0x14,         /* charge pump on, so no external high voltage is needed */
    CMD_DISPLAY_ON
};

static const uint8_t s_init_128x32[] = {
    CMD_DISPLAY_OFF,
    0x20, 0x02,
    0xA8, 0x1F,         /* multiplex ratio: 32 rows */
    0xD3, 0x00,
    0x40,
    CMD_SEG_REMAP_ON,
    CMD_COM_SCAN_DEC,
    0xDA, 0x02,         /* COM pins: sequential, which a 32-row panel wants */
    CMD_SET_CONTRAST, 0xCF,
    0xD9, 0xF1,
    0xDB, 0x40,
    0xD5, 0x80,
    0xA4,
    CMD_NORMAL_VIDEO,
    0x8D, 0x14,
    CMD_DISPLAY_ON
};

const nx_ssd1306_panel_t nx_ssd1306_panel_128x64 = {
    .width      = 128U,
    .height     = 64U,
    .col_offset = 0U,
    .init_seq   = s_init_128x64,
    .init_len   = sizeof(s_init_128x64)
};

const nx_ssd1306_panel_t nx_ssd1306_panel_128x32 = {
    .width      = 128U,
    .height     = 32U,
    .col_offset = 0U,
    .init_seq   = s_init_128x32,
    .init_len   = sizeof(s_init_128x32)
};

/*
 * Every entry point starts by proving the handle is usable, so the checks live
 * here instead of being spelled out in each function.
 */
static inline bool ssd1306_ready(const nx_ssd1306_t *dev)
{
    return dev != NULL
        && dev->fb != NULL
        && dev->cfg.panel != NULL
        && dev->cfg.write_cmd != NULL
        && dev->cfg.write_data != NULL;
}

/* Shared tail of the command wrappers: hand the bytes to the command callback. */
static inline nx_ssd1306_ret_t ssd1306_cmd(nx_ssd1306_t *dev, const uint8_t *buf, size_t len)
{
    if (!dev->cfg.write_cmd(dev->cfg.io_ctx, buf, len)) {
        return NX_SSD1306_ERR_IO;
    }
    return NX_SSD1306_OK;
}

nx_ssd1306_ret_t nx_ssd1306_init(nx_ssd1306_t           *dev,
                                 const nx_ssd1306_cfg_t *cfg,
                                 uint8_t                *framebuffer)
{
    if (dev == NULL || cfg == NULL || framebuffer == NULL) {
        return NX_SSD1306_ERR_PARAM;
    }
    if (cfg->panel == NULL || cfg->write_cmd == NULL || cfg->write_data == NULL) {
        return NX_SSD1306_ERR_PARAM;
    }

    const nx_ssd1306_panel_t *panel = cfg->panel;
    if (panel->width == 0U || panel->init_seq == NULL || panel->init_len == 0U) {
        return NX_SSD1306_ERR_PARAM;
    }
    if (panel->height == 0U
        || (panel->height % NX_SSD1306_PAGE_HEIGHT) != 0U
        || panel->height > (NX_SSD1306_PAGE_HEIGHT * NX_SSD1306_MAX_PAGES)) {
        return NX_SSD1306_ERR_PARAM;
    }

    dev->cfg = *cfg;   /* copy the config struct into the handle */
    dev->fb  = framebuffer;

    uint8_t pages = (uint8_t)(panel->height / NX_SSD1306_PAGE_HEIGHT);
    dev->run.pages     = pages;
    dev->run.next_page = 0U;
    /* Controller RAM holds undefined contents after power-up, so the first flush
       has to paint every page rather than trust it to match a cleared buffer. */
    dev->run.dirty     = (uint8_t)((1U << pages) - 1U);

    memset(dev->fb, 0, (size_t)panel->width * pages);

    return ssd1306_cmd(dev, panel->init_seq, panel->init_len);
}

nx_ssd1306_ret_t nx_ssd1306_set_pixel(nx_ssd1306_t *dev, uint8_t x, uint8_t y, bool on)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    if (x >= dev->cfg.panel->width || y >= dev->cfg.panel->height) {
        return NX_SSD1306_ERR_PARAM;
    }

    uint8_t page = (uint8_t)(y / NX_SSD1306_PAGE_HEIGHT);
    uint8_t mask = (uint8_t)(1U << (y % NX_SSD1306_PAGE_HEIGHT));
    size_t  idx  = (size_t)page * dev->cfg.panel->width + x;

    if (on) {
        dev->fb[idx] = (uint8_t)(dev->fb[idx] | mask);
    } else {
        dev->fb[idx] = (uint8_t)(dev->fb[idx] & (uint8_t)~mask);
    }
    dev->run.dirty = (uint8_t)(dev->run.dirty | (uint8_t)(1U << page));

    return NX_SSD1306_OK;
}

bool nx_ssd1306_get_pixel(const nx_ssd1306_t *dev, uint8_t x, uint8_t y)
{
    if (!ssd1306_ready(dev)) {
        return false;
    }
    if (x >= dev->cfg.panel->width || y >= dev->cfg.panel->height) {
        return false;
    }

    size_t idx = (size_t)(y / NX_SSD1306_PAGE_HEIGHT) * dev->cfg.panel->width + x;
    return (dev->fb[idx] & (uint8_t)(1U << (y % NX_SSD1306_PAGE_HEIGHT))) != 0U;
}

nx_ssd1306_ret_t nx_ssd1306_fill(nx_ssd1306_t *dev, bool on)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }

    memset(dev->fb, on ? 0xFF : 0x00,
           (size_t)dev->cfg.panel->width * dev->run.pages);
    dev->run.dirty = (uint8_t)((1U << dev->run.pages) - 1U);

    return NX_SSD1306_OK;
}

uint8_t *nx_ssd1306_framebuffer(nx_ssd1306_t *dev)
{
    if (!ssd1306_ready(dev)) {
        return NULL;
    }
    return dev->fb;
}

void nx_ssd1306_touch(nx_ssd1306_t *dev, uint8_t y0, uint8_t y1)
{
    if (!ssd1306_ready(dev)) {
        return;
    }

    uint8_t height = dev->cfg.panel->height;
    if (y1 < y0 || y0 >= height) {
        return;
    }
    if (y1 >= height) {
        y1 = (uint8_t)(height - 1U);
    }

    /* A page is the smallest unit the controller can be handed, so a partial row
       range still costs the whole page it lands in. */
    uint8_t first = (uint8_t)(y0 / NX_SSD1306_PAGE_HEIGHT);
    uint8_t last  = (uint8_t)(y1 / NX_SSD1306_PAGE_HEIGHT);
    for (uint8_t p = first; p <= last; p++) {
        dev->run.dirty = (uint8_t)(dev->run.dirty | (uint8_t)(1U << p));
    }
}

bool nx_ssd1306_dirty(const nx_ssd1306_t *dev)
{
    if (dev == NULL) {
        return false;
    }
    return dev->run.dirty != 0U;
}

bool nx_ssd1306_busy(const nx_ssd1306_t *dev)
{
    if (dev == NULL || dev->cfg.is_busy == NULL) {
        return false;   /* no way to tell, so report idle */
    }
    return dev->cfg.is_busy(dev->cfg.io_ctx);
}

nx_ssd1306_ret_t nx_ssd1306_process(nx_ssd1306_t *dev)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    if (dev->run.dirty == 0U) {
        return NX_SSD1306_OK;
    }
    if (dev->cfg.is_busy != NULL && dev->cfg.is_busy(dev->cfg.io_ctx)) {
        return NX_SSD1306_BUSY;
    }

    /* Resume where the previous call stopped and wrap around, so a region
       redrawn on every iteration cannot hold the rest of the screen back. */
    uint8_t pages = dev->run.pages;
    uint8_t page  = dev->run.next_page;
    for (uint8_t i = 0U; i < pages; i++) {
        if ((dev->run.dirty & (uint8_t)(1U << page)) != 0U) {
            break;
        }
        page = (uint8_t)((page + 1U) % pages);
    }

    uint8_t width = dev->cfg.panel->width;
    uint8_t col   = dev->cfg.panel->col_offset;
    uint8_t cmd[3];
    cmd[0] = (uint8_t)(CMD_PAGE_START | page);
    cmd[1] = (uint8_t)(CMD_COL_LOW  | (col & 0x0FU));
    cmd[2] = (uint8_t)(CMD_COL_HIGH | (col >> 4));

    if (!dev->cfg.write_cmd(dev->cfg.io_ctx, cmd, sizeof(cmd))) {
        return NX_SSD1306_ERR_IO;
    }
    if (!dev->cfg.write_data(dev->cfg.io_ctx, &dev->fb[(size_t)page * width], width)) {
        return NX_SSD1306_ERR_IO;
    }

    /* Cleared only once both writes landed: a failure above leaves the bit set
       and next_page pointing here, so the next call retries this page. */
    dev->run.dirty     = (uint8_t)(dev->run.dirty & (uint8_t)~(1U << page));
    dev->run.next_page = (uint8_t)((page + 1U) % pages);

    return (dev->run.dirty != 0U) ? NX_SSD1306_BUSY : NX_SSD1306_OK;
}

nx_ssd1306_ret_t nx_ssd1306_flush(nx_ssd1306_t *dev)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }

    for (;;) {
        /* BUSY covers both "pages still queued" and "bus still transmitting",
           and either way the answer is to call again. */
        nx_ssd1306_ret_t ret = nx_ssd1306_process(dev);
        if (ret != NX_SSD1306_BUSY) {
            return ret;
        }
    }
}

nx_ssd1306_ret_t nx_ssd1306_send_cmd(nx_ssd1306_t *dev, const uint8_t *cmd, size_t len)
{
    if (!ssd1306_ready(dev) || cmd == NULL || len == 0U) {
        return NX_SSD1306_ERR_PARAM;
    }
    return ssd1306_cmd(dev, cmd, len);
}

nx_ssd1306_ret_t nx_ssd1306_display_on(nx_ssd1306_t *dev, bool on)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    uint8_t cmd = on ? (uint8_t)CMD_DISPLAY_ON : (uint8_t)CMD_DISPLAY_OFF;
    return ssd1306_cmd(dev, &cmd, 1U);
}

nx_ssd1306_ret_t nx_ssd1306_set_contrast(nx_ssd1306_t *dev, uint8_t level)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    uint8_t cmd[2] = { CMD_SET_CONTRAST, level };
    return ssd1306_cmd(dev, cmd, sizeof(cmd));
}

nx_ssd1306_ret_t nx_ssd1306_invert(nx_ssd1306_t *dev, bool on)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    uint8_t cmd = on ? (uint8_t)CMD_INVERT_VIDEO : (uint8_t)CMD_NORMAL_VIDEO;
    return ssd1306_cmd(dev, &cmd, 1U);
}

nx_ssd1306_ret_t nx_ssd1306_flip(nx_ssd1306_t *dev, bool mirror_x, bool mirror_y)
{
    if (!ssd1306_ready(dev)) {
        return NX_SSD1306_ERR_PARAM;
    }
    uint8_t cmd[2];
    cmd[0] = mirror_x ? (uint8_t)CMD_SEG_REMAP_ON : (uint8_t)CMD_SEG_REMAP_OFF;
    cmd[1] = mirror_y ? (uint8_t)CMD_COM_SCAN_DEC : (uint8_t)CMD_COM_SCAN_INC;
    return ssd1306_cmd(dev, cmd, sizeof(cmd));
}
