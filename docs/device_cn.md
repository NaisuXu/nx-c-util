# 设备模块

## nx_ws2812 —— WS2812(B) RGB 灯带驱动

一个 WS2812/WS2812B 可寻址 RGB 灯珠的驱动，只负责协议编码：硬件外设（SPI、UART 或定时器加DMA）由调用方持有，并提供一个写回调，本模块把像素颜色转换成该外设需要发送的确切字节流。不使用动态内存。

- **位展开编码** —— WS2812 要求严格的时序协议（约 1.25 µs 的位周期，±150 ns 容差），没有哪个UART 或 SPI 时钟能直接命中。于是每个数据位在线上被展开成一个字节，用该字节的高位游程长度决定高电平时间。两个字节模板由调用方在配置里给出（`bit0_pattern` / `bit1_pattern`），因此任意外设、时钟和位序都能适配。参考值：SPI，MSB 优先 @ 2.4–3.6 MHz 用 `0xC0` / `0xF8`；UART，LSB 优先（8N1，TX 反相）@ 2.4–3.2 Mbaud 用 `0x03` / `0x1F`。
- **零分配、缓冲由调用方持有** —— 两个缓冲的尺寸都由宏给出，可用于给静态数组定尺寸。一个像素缓冲（`NX_WS2812_PIXEL_BUF_SIZE`，每颗 LED 3 字节，GRB 顺序）保存颜色状态；一个发送缓冲（`NX_WS2812_TX_BUF_SIZE`，每颗 LED 24 字节，再加 reset 字节），`nx_ws2812_update` 每次调用都会重建它，其余时候可当作暂存区。
- **非阻塞更新，带 busy 握手** —— `nx_ws2812_update` 从不阻塞。若外设仍在发送上一帧，它立即返回`false` 且不触碰发送缓冲，让在途数据保持完整，因此重试是安全的。可选的 `is_busy` 回调驱动这一机制，并通过 `nx_ws2812_busy` 暴露出来，让调用方按自己的节奏轮询，而不是在驱动内空转。这正契合 DMA 支撑的传输。`is_busy` 为 NULL 时，写在 `write` 返回后即视作完成（适用于阻塞式传输）。
- **无损全局亮度** —— WS2812 没有亮度寄存器，因此亮度是编码时在 `nx_ws2812_update` 内对每个通道做的乘法，绝不写回像素缓冲。你设置的颜色始终保持全分辨率，因此调暗后再调亮能精确还原原值，不累积舍入误差。
- **像素操作** —— `set_pixel` / `fill` / `set_all` 设置颜色，`get_pixel` 读回，`clear` 熄灭整条； `push` / `push_tail` 把整条灯带向一端移位，并在空出的一端喂入一个新颜色，越界的部分被丢弃。这是跑马灯、彗尾、VU 表等效果的基本操作。
- **单一串行上下文** —— `write` 和 `is_busy` 回调驱动的是同一个外设，因此共用一个 `io_ctx`，作为它们的第一个参数传入。
- **非线程安全** —— 多上下文访问需自行串行化。

```c
#include "nx_ws2812.h"

#define LED_COUNT   60u
#define RESET_BYTES 50u          /* trailing low period that latches the frame */

/* SPI, MSB-first @ ~3.2 MHz: one data bit -> one byte on the wire. */
#define BIT0 0xC0u               /* ~0.4us high = a "0" bit */
#define BIT1 0xF8u               /* ~0.8us high = a "1" bit */

/* caller-owned storage; the driver allocates nothing. Both sizes are macros, so
 * they work where a constant expression is required (a static array here). */
static uint8_t pixels[NX_WS2812_PIXEL_BUF_SIZE(LED_COUNT)];
static uint8_t tx[NX_WS2812_TX_BUF_SIZE(LED_COUNT, RESET_BYTES)];

static const nx_ws2812_cfg_t cfg = {
    .led_count    = LED_COUNT,
    .reset_bytes  = RESET_BYTES,
    .bit0_pattern = BIT0,
    .bit1_pattern = BIT1,
    .write        = spi_write,   /* pushes the encoded stream to the peripheral */
    .is_busy      = spi_busy,    /* NULL if write blocks until done (no DMA)     */
    .io_ctx       = &spi,        /* passed to write / is_busy                    */
};

nx_ws2812_t strip;
nx_ws2812_init(&strip, &cfg, pixels, tx);

nx_ws2812_set_pixel(&strip, 0, 255, 0, 0);   /* LED 0 red                       */
nx_ws2812_set_brightness(&strip, 128);       /* half brightness, applied at encode */

/* Non-blocking: refuses (returns false) if a prior frame is still on the wire. */
if (!nx_ws2812_update(&strip)) {
    /* peripheral busy or IO error; retry next tick, or poll nx_ws2812_busy */
}
```

> **注意：** `nx_ws2812_update` 返回 `true` 只表示外设*接受*了这次写入，并不代表 LED 已经锁存。用 DMA 时传输仍在后台进行，`nx_ws2812_busy` 会报告它何时结束。请按你的实际时钟选取 `bit0_pattern` / `bit1_pattern` 和 `reset_bytes`：高电平时间窗口只有 ±150 ns 宽，而 reset 间隔必须把线拉低足够久才能完成锁存（WS2812 需 >50 µs，某些 WS2812B 版本需 >280 µs）。

## nx_ssd1306 —— SSD1306 系列 OLED 控制器驱动

一个 SSD1306 系列 OLED 控制器（SSD1306、SSD1315、SSD1309、SH1106）的驱动：命令集和整帧帧缓冲由本模块持有，硬件外设（I²C 或 SPI）由调用方拥有，并提供命令/数据两个写回调，本模块把帧缓冲的改动转换成该面板需要发送的确切字节流。不使用动态内存。

- **面板描述符** —— 内置的 `nx_ssd1306_panel_128x64` 与 `nx_ssd1306_panel_128x32` 携带几何参数与初始化序列；不同分辨率或接线的面板只需自备一个描述符（`width`、`height`、`col_offset`、`init_seq`、`init_len`）。命令集和分页数据布局在系列内各芯片是共通的，只有配置字节不同。
- **零分配、帧缓冲由调用方持有** —— 帧缓冲尺寸由宏给出（`NX_SSD1306_FB_SIZE(width, height)`，每 8 行一页、每列 1 字节），可用于给静态数组定尺寸。调用方传给 `nx_ssd1306_init` 并在句柄存续期间持有它。
- **按页跟踪脏标记** —— 只发送改动过的 8 行页。`set_pixel` / `fill` / `clear` 直接标记对应页为脏；通过 `nx_ssd1306_framebuffer` 直接编辑缓冲后必须用 `nx_ssd1306_touch` 声明，它只触碰该字节落到的页（并对行位置夹取）。由于控制器 RAM 上电后内容未定义，初始化后每一页都初始标记为脏。
- **非阻塞泵，带 busy 握手** —— `nx_ssd1306_process` 每次调用发送一页，返回 `NX_SSD1306_OK`（一帧完成）或 `NX_SSD1306_BUSY`（还有页待发，或总线尚未结束）。可选的 `is_busy` 回调在传输仍在进行时推迟发送该页，因此忙轮询不会撕坏半个帧。`nx_ssd1306_busy` 暴露这一推迟状态，`flush` 则循环 `process` 直到完成，供阻塞式调用点使用。
- **I²C 或 SPI，无硬件访问** —— 驱动分别通过两个回调发出命令字节和数据字节；在 I²C 上由调用方的 `write_cmd` / `write_data` 加控制字节（`0x00` / `0x40`），在 SPI 上由它们驱动 D/C 线。两个回调共用同一个 `io_ctx`（同一条总线），且不会在一次被延迟的写入尚未结束时被再次调用。
- **I/O 出错时重试** —— 写入失败会把该页保持为脏并返回 `NX_SSD1306_ERR_IO`，因此下一次 `process` 会重发该页，而不是推进成一个有空洞的帧。
- **命令封装** —— `send_cmd` / `display_on` / `set_contrast` / `invert` / `flip` 覆盖常见操作；`flip` 与内置描述符各自的 remap 字节都会镜像面板，让上下颠倒接线的屏也能正着显示文字。

```c
#include "src/device/nx_ssd1306.h"

/* 调用方持有的存储；驱动不分配任何东西。 */
static uint8_t fb[NX_SSD1306_FB_SIZE(128, 64)];

static const nx_ssd1306_cfg_t cfg = {
    .panel      = &nx_ssd1306_panel_128x64,
    .write_cmd  = i2c_write_cmd,   /* 先发 0x00 控制字节，再发命令字节 */
    .write_data = i2c_write_data,  /* 先发 0x40 控制字节，再发数据字节 */
    .is_busy    = i2c_busy,        /* 若总线阻塞到写完则传 NULL        */
    .io_ctx     = &i2c,            /* 传给 write_cmd / write_data     */
};

nx_ssd1306_t disp;
nx_ssd1306_init(&disp, &cfg, fb);

nx_ssd1306_set_pixel(&disp, 64, 32);   /* 128x64 面板的正中间那个像素 */

/* 非阻塞：每次调用发送一页，直到整帧到达总线。 */
while (nx_ssd1306_process(&disp) == NX_SSD1306_BUSY) {
    /* 让出主循环；再次改动 fb 前先轮询 nx_ssd1306_busy */
}
```

> **注意：** `nx_ssd1306_process` 返回 `NX_SSD1306_OK` 只表示整帧已发出，并不代表面板已经锁存——控制器仍需要几个显示时钟周期来刷出新的行，而且用 DMA 支撑的总线时传输可能在调用返回后仍在继续。另外，因为只有 `process` 会发送一页，一个画了图却不泵 `process` 的调用方会看到一块陈旧的屏：脏页一直躺在帧缓冲里，直到有东西把泵起来。
