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
