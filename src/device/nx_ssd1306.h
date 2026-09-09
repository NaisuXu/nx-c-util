/**
 * @file    nx_ssd1306.h
 * @brief   SSD1306 monochrome OLED controller driver with dirty-page flushing.
 *
 * This module drives an SSD1306 controller over an injected byte bus (I2C or
 * 4-wire SPI). It owns a 1-bit-per-pixel framebuffer laid out the way the
 * controller's RAM is organized, tracks which parts of it changed, and pushes
 * only those parts out.
 *
 * Design:
 *   - The caller provides a panel descriptor (geometry plus the init sequence),
 *     a framebuffer, and two write callbacks: one for commands, one for pixel
 *     data. Which one is which is a property of the bus - an I2C driver picks
 *     the control byte, an SPI driver drives the D/C pin - so the split lives
 *     in the callbacks rather than in this module.
 *   - Drawing writes the framebuffer and marks pages dirty. Nothing reaches the
 *     panel until a flush call.
 *   - nx_ssd1306_process sends at most one page per call and returns, so a
 *     full-screen update spreads across main-loop iterations instead of
 *     blocking in one.
 *
 * Typical flow:
 *   1. Reset the panel in hardware and wait for it to settle.
 *   2. nx_ssd1306_init with a panel descriptor and a framebuffer; this sends the
 *      panel's init sequence.
 *   3. Draw with nx_ssd1306_set_pixel / nx_ssd1306_fill, or write the
 *      framebuffer directly and declare the touched rows with nx_ssd1306_touch.
 *   4. Call nx_ssd1306_process every main-loop iteration until it reports OK.
 *
 * Framebuffer layout:
 *   The controller addresses its RAM in pages of 8 vertically stacked pixels,
 *   so the framebuffer uses the same shape: byte (y / 8) * width + x holds the
 *   column of 8 pixels starting at row (y / 8) * 8, with row y in bit y % 8.
 *   That means a page is a contiguous run of `width` bytes and can be handed to
 *   the bus without any repacking.
 *
 * Memory:
 *   This module does not allocate memory. The caller provides a framebuffer of
 *   NX_SSD1306_FB_SIZE(width, height) bytes.
 *
 * Thread safety:
 *   Not thread-safe. Serialize access from multiple contexts yourself.
 */
#ifndef NX_SSD1306_H
#define NX_SSD1306_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pixel rows per page: the controller's RAM is addressed in these units. */
#define NX_SSD1306_PAGE_HEIGHT 8U

/** @brief Largest page count this driver tracks, which caps the panel at 64 rows. */
#define NX_SSD1306_MAX_PAGES 8U

/**
 * @brief Bytes of framebuffer needed for a @p width x @p height panel.
 *
 * One byte holds 8 vertically stacked pixels, so a panel needs
 * width * ceil(height / 8) bytes. This is a macro rather than a function so it
 * works where a constant expression is required - sizing a static array, for
 * instance.
 *
 * @code
 * static uint8_t fb[NX_SSD1306_FB_SIZE(128, 64)];   // 1024 bytes
 * @endcode
 */
#define NX_SSD1306_FB_SIZE(width, height) \
    ((size_t)(width) * (((size_t)(height) + NX_SSD1306_PAGE_HEIGHT - 1U) / NX_SSD1306_PAGE_HEIGHT))

/** @brief Result of a driver operation. */
typedef enum {
    NX_SSD1306_OK = 0,      /**< Done; for flush calls, the panel matches the framebuffer */
    NX_SSD1306_BUSY,        /**< Pages still pending, or the bus has not finished; call again */
    NX_SSD1306_ERR_PARAM,   /**< NULL pointer, uninitialized handle, or out-of-range argument */
    NX_SSD1306_ERR_IO       /**< A write callback reported failure */
} nx_ssd1306_ret_t;

/**
 * @brief Panel description: geometry plus the command sequence that configures it.
 *
 * Everything that varies between modules of this controller family lives here,
 * which is why the driver needs no per-chip conditionals. Ready-made
 * descriptors are provided below; a module whose vendor prescribes a different
 * sequence gets a caller-defined descriptor instead, with no library change.
 *
 * width / height: pixel dimensions. height must be a multiple of 8 and at most
 * 8 * NX_SSD1306_MAX_PAGES.
 *
 * col_offset: column index in controller RAM that displays as x = 0. Controllers
 * with more RAM columns than the panel shows need this; leave it 0 when RAM and
 * panel line up.
 *
 * init_seq / init_len: command bytes sent verbatim by nx_ssd1306_init, in one
 * write_cmd call. The sequence carries its own parameter bytes, so multi-byte
 * commands are just consecutive entries.
 */
typedef struct {
    uint8_t        width;      /**< Panel width in pixels; must be > 0 */
    uint8_t        height;     /**< Panel height in pixels; multiple of 8, <= 64 */
    uint8_t        col_offset; /**< RAM column shown at x = 0 */
    const uint8_t *init_seq;   /**< Configuration commands; must not be NULL */
    size_t         init_len;   /**< Length of init_seq in bytes; must be > 0 */
} nx_ssd1306_panel_t;

/** @brief 128x64 panel, internal charge pump, orientation as wired on most breakouts. */
extern const nx_ssd1306_panel_t nx_ssd1306_panel_128x64;

/** @brief 128x32 panel; differs from 128x64 in multiplex ratio and COM pin config. */
extern const nx_ssd1306_panel_t nx_ssd1306_panel_128x32;

/**
 * @brief Driver configuration, supplied by the caller at init time.
 *
 * write_cmd: sends @p len command bytes. The bus decides how a command is
 * distinguished from data - an I2C driver prefixes control byte 0x00, a 4-wire
 * SPI driver holds D/C low - so that choice belongs to this callback. Must
 * block until the bytes are accepted, though transmission may continue in the
 * background (DMA). Returns true on success, false on bus error.
 *
 * write_data: same contract for pixel bytes, with the data selector instead
 * (I2C control byte 0x40, or D/C high).
 *
 * is_busy: optional; returns true while a previous write is still transmitting.
 * The flush calls check it and defer rather than wait. NULL means writes are
 * complete when the callback returns, so the driver never reports busy -
 * correct for blocking transfers, wrong for DMA.
 *
 * io_ctx: opaque context passed as the first argument to all three callbacks,
 * which drive the same bus.
 */
typedef struct {
    const nx_ssd1306_panel_t *panel;   /**< Required; must outlive the handle */
    bool (*write_cmd)(void *ctx, const uint8_t *buf, size_t len);   /**< Required */
    bool (*write_data)(void *ctx, const uint8_t *buf, size_t len);  /**< Required */
    bool (*is_busy)(void *ctx);                                     /**< Optional; may be NULL */
    void *io_ctx;                      /**< Context passed to the callbacks (same bus) */
} nx_ssd1306_cfg_t;

/**
 * @brief SSD1306 driver instance.
 *
 * Opaque handle; initialize with nx_ssd1306_init.
 */
typedef struct {
    nx_ssd1306_cfg_t cfg;       /**< Configuration (copied at init) */
    uint8_t         *fb;        /**< Framebuffer, caller-owned */
    struct {
        uint8_t pages;          /**< Page count derived from panel height */
        uint8_t dirty;          /**< One bit per page, set when that page changed */
        uint8_t next_page;      /**< Where the next flush call resumes scanning */
    } run;                      /**< Internal runtime state */
} nx_ssd1306_t;

/**
 * @brief  Initialize a driver instance and configure the panel.
 *
 * Clears the framebuffer, sends the panel's init sequence, and marks every page
 * dirty so the first flush paints the whole screen - the controller's RAM holds
 * undefined contents after power-up.
 *
 * The framebuffer stays owned by the caller and must outlive @p dev, as must the
 * panel descriptor and its init sequence; the module allocates nothing.
 *
 * The panel must already be out of hardware reset and settled before this call.
 * This module has no notion of time, so releasing the reset pin and waiting the
 * interval the datasheet asks for is the caller's job.
 *
 * @param  dev         Driver instance, must not be NULL.
 * @param  cfg         Configuration, must not be NULL; copied into @p dev.
 * @param  framebuffer Pixel storage, NX_SSD1306_FB_SIZE(width, height) bytes.
 *
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments
 *         (NULL pointers, missing callbacks, or panel geometry outside the
 *         supported range), NX_SSD1306_ERR_IO if the init sequence was rejected.
 */
nx_ssd1306_ret_t nx_ssd1306_init(nx_ssd1306_t           *dev,
                                 const nx_ssd1306_cfg_t *cfg,
                                 uint8_t                *framebuffer);

/**
 * @brief  Panel width in pixels, or 0 if @p dev is NULL or uninitialized.
 */
static inline uint8_t nx_ssd1306_width(const nx_ssd1306_t *dev)
{
    if (dev == NULL || dev->cfg.panel == NULL) {
        return 0U;
    }
    return dev->cfg.panel->width;
}

/**
 * @brief  Panel height in pixels, or 0 if @p dev is NULL or uninitialized.
 */
static inline uint8_t nx_ssd1306_height(const nx_ssd1306_t *dev)
{
    if (dev == NULL || dev->cfg.panel == NULL) {
        return 0U;
    }
    return dev->cfg.panel->height;
}

/**
 * @brief  Set or clear one pixel, with the origin at the top-left corner.
 *
 * Marks the containing page dirty. Takes effect on the next flush call.
 *
 * @param  dev Driver instance.
 * @param  x   Column, 0 .. width-1.
 * @param  y   Row, 0 .. height-1.
 * @param  on  true lights the pixel, false clears it.
 *
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM if @p dev is NULL or
 *         uninitialized, or the coordinate lies outside the panel.
 */
nx_ssd1306_ret_t nx_ssd1306_set_pixel(nx_ssd1306_t *dev, uint8_t x, uint8_t y, bool on);

/**
 * @brief  Read one pixel back out of the framebuffer.
 *
 * Reports what the framebuffer holds, which is what the next flush will send.
 *
 * @param  dev Driver instance.
 * @param  x   Column, 0 .. width-1.
 * @param  y   Row, 0 .. height-1.
 * @return true if the pixel is set; false if it is clear, or the arguments are
 *         out of range.
 */
bool nx_ssd1306_get_pixel(const nx_ssd1306_t *dev, uint8_t x, uint8_t y);

/**
 * @brief  Set every pixel on or off and mark the whole screen dirty.
 *
 * @param  dev Driver instance.
 * @param  on  true fills the screen, false blanks it.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM if @p dev is NULL or
 *         uninitialized.
 */
nx_ssd1306_ret_t nx_ssd1306_fill(nx_ssd1306_t *dev, bool on);

/**
 * @brief  Blank the framebuffer (whole-screen nx_ssd1306_fill with false).
 *
 * @param  dev Driver instance.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM if @p dev is NULL or
 *         uninitialized.
 */
static inline nx_ssd1306_ret_t nx_ssd1306_clear(nx_ssd1306_t *dev)
{
    return nx_ssd1306_fill(dev, false);
}

/**
 * @brief  Get the framebuffer pointer for direct drawing.
 *
 * Rendering code can write the buffer itself - see the layout described at the
 * top of this file - and then declare what it touched with nx_ssd1306_touch.
 * Bytes written this way are invisible to the dirty tracking until it is told.
 *
 * @param  dev Driver instance.
 * @return The framebuffer, or NULL if @p dev is NULL or uninitialized.
 */
uint8_t *nx_ssd1306_framebuffer(nx_ssd1306_t *dev);

/**
 * @brief  Mark the pages covering rows @p y0 .. @p y1 as changed.
 *
 * Rows are inclusive on both ends and get rounded outward to whole pages, since
 * a page is the smallest unit the controller can be handed. Values past the
 * bottom edge are clamped; a range that starts past it is ignored.
 *
 * Only direct framebuffer writers need this - nx_ssd1306_set_pixel and
 * nx_ssd1306_fill already mark what they change.
 *
 * @param  dev Driver instance; NULL is ignored.
 * @param  y0  First row of the changed region.
 * @param  y1  Last row of the changed region; must be >= @p y0, or the call is
 *             ignored.
 */
void nx_ssd1306_touch(nx_ssd1306_t *dev, uint8_t y0, uint8_t y1);

/**
 * @brief  Report whether the framebuffer holds changes the panel has not seen.
 *
 * @param  dev Driver instance.
 * @return true if at least one page is pending; false if the panel is up to
 *         date, or @p dev is NULL.
 */
bool nx_ssd1306_dirty(const nx_ssd1306_t *dev);

/**
 * @brief  Report whether the bus is still transmitting.
 *
 * Reflects cfg.is_busy, so with no busy callback configured this is always
 * false.
 *
 * @param  dev Driver instance.
 * @return true if a transfer is in flight; false if idle, or if @p dev is NULL
 *         or has no busy callback.
 */
bool nx_ssd1306_busy(const nx_ssd1306_t *dev);

/**
 * @brief  Push at most one dirty page to the panel. Never blocks.
 *
 * Call this once per main-loop iteration. Each call sends the three address
 * commands for one page followed by that page's bytes, so the cost per call is
 * bounded by the panel width no matter how much of the screen changed. Pages are
 * visited round-robin from where the previous call left off, so a region redrawn
 * every iteration cannot starve the rest of the screen.
 *
 * If the bus reports busy, this returns immediately without sending anything and
 * without clearing any dirty bit.
 *
 * @param  dev Driver instance.
 *
 * @return NX_SSD1306_OK when no pages remain pending, NX_SSD1306_BUSY when
 *         there is more to send or the bus was busy, NX_SSD1306_ERR_PARAM if
 *         @p dev is NULL or uninitialized, NX_SSD1306_ERR_IO if a write failed.
 *
 * @note   A failed write leaves the page marked dirty, so the next call retries
 *         it rather than dropping the update.
 */
nx_ssd1306_ret_t nx_ssd1306_process(nx_ssd1306_t *dev);

/**
 * @brief  Push every dirty page to the panel before returning.
 *
 * Drives nx_ssd1306_process in a loop, which makes this the convenient choice
 * for setup code and for callers that have nothing else to do while the screen
 * updates. A full 128x64 frame is 1024 data bytes plus 24 command bytes, so the
 * time this occupies is set by the bus - around 25 ms on 400 kHz I2C.
 *
 * @param  dev Driver instance.
 *
 * @return NX_SSD1306_OK once the panel matches the framebuffer,
 *         NX_SSD1306_ERR_PARAM if @p dev is NULL or uninitialized,
 *         NX_SSD1306_ERR_IO if a write failed.
 *
 * @note   With an is_busy callback configured this spins on it until the bus
 *         goes idle. Callers that cannot afford to wait should drive
 *         nx_ssd1306_process from their own loop instead.
 */
nx_ssd1306_ret_t nx_ssd1306_flush(nx_ssd1306_t *dev);

/**
 * @brief  Send raw command bytes to the controller.
 *
 * The escape hatch for anything this driver does not wrap - hardware scrolling,
 * page and column window setup, vendor-specific registers. Parameter bytes
 * follow their command in the same buffer.
 *
 * @param  dev Driver instance.
 * @param  cmd Command bytes, must not be NULL.
 * @param  len Number of bytes; must be > 0.
 *
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments,
 *         NX_SSD1306_ERR_IO if the write failed.
 *
 * @note   Commands that repoint the address counter or change the addressing
 *         mode will confuse the page writes this driver emits. Restore the
 *         controller to page addressing mode (0x20, 0x02) before flushing again.
 */
nx_ssd1306_ret_t nx_ssd1306_send_cmd(nx_ssd1306_t *dev, const uint8_t *cmd, size_t len);

/**
 * @brief  Turn the display on, or put it into sleep mode.
 *
 * Sleep mode keeps RAM contents and cuts panel current to a trickle, so it is
 * the right way to blank a screen that will come back.
 *
 * @param  dev Driver instance.
 * @param  on  true turns the display on, false puts it to sleep.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments,
 *         NX_SSD1306_ERR_IO if the write failed.
 */
nx_ssd1306_ret_t nx_ssd1306_display_on(nx_ssd1306_t *dev, bool on);

/**
 * @brief  Set the drive current, which sets apparent brightness.
 *
 * @param  dev   Driver instance.
 * @param  level 0 is dimmest, 255 is brightest. The bundled panel descriptors
 *               start at 0xCF.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments,
 *         NX_SSD1306_ERR_IO if the write failed.
 */
nx_ssd1306_ret_t nx_ssd1306_set_contrast(nx_ssd1306_t *dev, uint8_t level);

/**
 * @brief  Show the framebuffer inverted, or normally.
 *
 * Inversion happens in the controller as it drives the panel, so the
 * framebuffer is untouched and no flush is needed.
 *
 * @param  dev Driver instance.
 * @param  on  true swaps lit and unlit pixels, false restores normal video.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments,
 *         NX_SSD1306_ERR_IO if the write failed.
 */
nx_ssd1306_ret_t nx_ssd1306_invert(nx_ssd1306_t *dev, bool on);

/**
 * @brief  Choose how framebuffer coordinates map onto the glass.
 *
 * The controller can mirror each axis as it scans, which covers panels mounted
 * upside down relative to the framebuffer's top-left origin. Mirroring both
 * axes turns the image 180 degrees.
 *
 * The bundled panel descriptors enable both, matching how the great majority of
 * breakout boards are wired; that is the orientation nx_ssd1306_init leaves the
 * panel in.
 *
 * Only the scan direction changes, so the framebuffer is untouched. Pixels
 * already on the glass keep their old orientation until the next flush - call
 * nx_ssd1306_touch over the full height, or nx_ssd1306_fill, to repaint.
 *
 * @param  dev      Driver instance.
 * @param  mirror_x true maps framebuffer column 0 to the panel's last column.
 * @param  mirror_y true maps framebuffer row 0 to the panel's last row.
 * @return NX_SSD1306_OK on success, NX_SSD1306_ERR_PARAM on invalid arguments,
 *         NX_SSD1306_ERR_IO if the write failed.
 */
nx_ssd1306_ret_t nx_ssd1306_flip(nx_ssd1306_t *dev, bool mirror_x, bool mirror_y);

#ifdef __cplusplus
}
#endif

#endif /* NX_SSD1306_H */
