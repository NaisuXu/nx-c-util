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

## nx_kth7112 —— KTH7112 磁编码器 SPI 驱动

KTH7112 16 位磁编码器的驱动，覆盖它的三线 SPI 协议（Mode 3）：命令字节、帧结构、芯片在每次读取后附加的 CRC-8，以及寄存器锁定状态机。片选、写、读和延时都由调用方以回调形式提供，SPI 端口也归调用方所有。不使用动态内存，不使用浮点。

- **一次调用一帧** —— `nx_kth7112_read_angle` 返回 16 位原始角度码，`nx_kth7112_read_reg8` / `_write_reg8` / `_read_reg16` / `_write_reg16` 访问寄存器组。每次调用就是完整的一帧片选，并在当前上下文里同步执行：一帧只有几个字节，比一个控制周期短得多，不必拆到多次迭代里。需要度数又不想用浮点的调用方，可以用 `nx_kth7112_raw_to_mdeg` 把角度码换算成千分之一度。
- **读取一律校验 CRC-8/ITU** —— 芯片在读数后附加的 CRC 为多项式 `0x07`、初值 `0x00`、结果异或 `0x55`，由本模块自带的 256 项查表算出。校验不通过就返回 `NX_KTH7112_ERR_CRC`，调用方的输出变量保持原值，一次失败的读取不会被当成有效数据用掉。
- **寄存器写入带回显确认** —— 写寄存器时，芯片会在同一帧里回送它实际接受的值，驱动拿它和发出的值比对。两者不一致返回 `NX_KTH7112_ERR_IO`，表示写入没有得到确认，而不只是没有权限。
- **锁定状态被跟踪，锁定期间的写入不上总线** —— 芯片上电后处于锁定状态，会静默丢弃寄存器写入。驱动在句柄里记着这个状态，锁定期间发起写入直接返回 `NX_KTH7112_ERR_LOCKED`，总线上不会出现任何一帧。解锁可以重复调用，要重新建立该状态，再调一次 `nx_kth7112_unlock` 即可。
- **多字节字段低位在前** —— 低字节放在低地址，所以 `NX_KTH7112_REG_ZERO_L` 对应 `ZERO[7:0]`，`NX_KTH7112_REG_ZERO_H` 对应高字节。`_read_reg16` / `_write_reg16` 传入低字节地址，一次搬两个字节，每个字节各自经过 CRC 校验或回显比对。
- **两个可选回调都可以留空** —— `is_busy` 和 `delay_ns` 都允许为 NULL。`is_busy` 为 NULL 表示假定端口随时就绪，适合阻塞式传输；`delay_ns` 为 NULL 表示不做帧间等待，只有在端口本身已经保证该时序时才成立。
- **非线程安全** —— 锁定状态存放在句柄里，多上下文访问需自行串行化。

```c
#include "nx_kth7112.h"

static const nx_kth7112_cfg_t cfg = {
    .cs         = spi_cs,        /* 必需：拉低和释放片选              */
    .write      = spi_write,     /* 必需：移出字节                    */
    .read       = spi_read,      /* 必需：移入字节                    */
    .is_busy    = spi_busy,      /* 可选：端口随时就绪则为 NULL       */
    .delay_ns   = delay_ns,      /* 可选：端口自带节奏则为 NULL       */
    .io_ctx     = &spi,
};

nx_kth7112_t enc;
nx_kth7112_init(&enc, &cfg);

uint16_t raw;
if (nx_kth7112_read_angle(&enc, &raw) == NX_KTH7112_OK) {
    /* raw == 角度 * 65536 / 360 */
}

/* 芯片上电后是锁定状态，写寄存器之前先解锁。 */
nx_kth7112_unlock(&enc);
nx_kth7112_write_reg8(&enc, NX_KTH7112_REG_FW, 0x44u);   /* 会比对回显 */
nx_kth7112_write_mtp(&enc);                              /* 写入 MTP，掉电保存 */
nx_kth7112_lock(&enc);                                   /* 重新锁定，拒绝后续写入 */
```

> **注意：** `nx_kth7112_write_mtp` 把寄存器组烧写进非易失存储器，且不可逆。芯片要求两次烧写间隔大于 `NX_KTH7112_MTP_MIN_INTERVAL_MS`（400 ms），驱动没有时间源、自己量不了，要由调用方拉开间隔，并保证烧写过程中不断电。另外两项时序要求归端口管，不归驱动管：写寄存器时第 24 个 SCK 的高电平要持续 100 ns 以上，两帧之间要间隔 150 ns 以上。时钟只有几 MHz 时这两项余量充足，但越靠近芯片 10 Mbps 的上限越紧——到那时一个时钟周期本身就是 100 ns 的下限。请确认端口实际跑的时钟，以及它在最后一个时钟上会不会展宽，必要时用 `delay_ns` 补足。

