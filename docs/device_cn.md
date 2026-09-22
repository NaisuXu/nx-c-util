# 设备模块

## nx_ws2812 —— WS2812(B) RGB 灯带驱动

该驱动用于编码 WS2812/WS2812B 可寻址 RGB LED 的像素数据。SPI、UART 或带 DMA 的定时器等硬件外设由调用方管理，并通过写回调接入；本模块负责生成外设所需的字节流，不使用动态内存。

- **可配置的位展开编码** —— WS2812 协议的位周期约为 1.25 µs，高电平时间容差只有 ±150 ns。本模块把每个 WS2812 数据位扩展为一个可配置的 8 位外设数据模式，以其中连续高位的长度控制脉宽。调用方通过 `bit0_pattern` 和 `bit1_pattern` 适配不同的外设时钟和位序。参考配置：MSB 优先的 SPI 在 2.4–3.6 MHz 时使用 `0xC0` / `0xF8`；LSB 优先的 UART 在 2.4–3.2 Mbaud 时使用 `0x03` / `0x1F`（8N1，TX 反相）。
- **零分配，缓冲区由调用方提供** —— 像素缓冲区使用 `NX_WS2812_PIXEL_BUF_SIZE` 计算大小，以 GRB 顺序保存每颗 LED 的 3 个字节；发送缓冲区使用 `NX_WS2812_TX_BUF_SIZE` 计算大小，为每颗 LED 保留 24 个编码字节，并附加复位所需的低电平字节。`nx_ws2812_update` 每次都会重建发送缓冲区，其余时间可将其视为临时存储。
- **感知忙状态的更新** —— 若外设仍在发送上一帧，`nx_ws2812_update` 会立即返回 `false`，且不会改动发送缓冲区，因此稍后重试不会破坏传输中的数据。可选的 `is_busy` 回调用于执行此检查，调用方也可通过 `nx_ws2812_busy` 查询状态，适合由 DMA 驱动的传输。若 `is_busy` 为 `NULL`，则认为 `write` 返回时传输已经完成；该调用是否阻塞取决于回调的具体实现。
- **无损全局亮度调节** —— WS2812 没有独立的亮度寄存器。本模块只在 `nx_ws2812_update` 编码时缩放各颜色通道，不会把缩放结果写回像素缓冲区。反复调暗和调亮不会累积舍入误差，原始颜色始终保持完整精度。
- **常用像素操作** —— `set_pixel`、`fill` 和 `set_all` 用于设置颜色，`get_pixel` 用于读取，`clear` 用于熄灭整条灯带。`push` 和 `push_tail` 可向任一方向移动全部像素，在空出的一端加入新颜色，并丢弃另一端移出的颜色，适合实现跑马灯、彗尾和 VU 表等效果。
- **共享 I/O 上下文** —— `write` 和 `is_busy` 操作同一个外设，因此共用 `io_ctx`，该指针会作为两个回调的第一个参数。
- **非线程安全** —— 若从多个上下文访问，调用方需自行串行化。

```c
#include "nx_ws2812.h"

#define LED_COUNT   60u
#define RESET_BYTES 50u          /* 用于锁存数据的帧尾低电平字节数 */

/* SPI，MSB 优先，约 3.2 MHz：每个数据位编码为一个发送字节。 */
#define BIT0 0xC0u               /* 高电平约 0.4 µs，表示 0 */
#define BIT1 0xF8u               /* 高电平约 0.8 µs，表示 1 */

/* 缓冲区由调用方持有；尺寸宏可直接用于静态数组。 */
static uint8_t pixels[NX_WS2812_PIXEL_BUF_SIZE(LED_COUNT)];
static uint8_t tx[NX_WS2812_TX_BUF_SIZE(LED_COUNT, RESET_BYTES)];

static const nx_ws2812_cfg_t cfg = {
    .led_count    = LED_COUNT,
    .reset_bytes  = RESET_BYTES,
    .bit0_pattern = BIT0,
    .bit1_pattern = BIT1,
    .write        = spi_write,   /* 将编码后的字节流交给外设              */
    .is_busy      = spi_busy,    /* 阻塞式写入可设为 NULL                  */
    .io_ctx       = &spi,        /* 传给 write 和 is_busy                  */
};

nx_ws2812_t strip;
nx_ws2812_init(&strip, &cfg, pixels, tx);

nx_ws2812_set_pixel(&strip, 0, 255, 0, 0);   /* 第 0 颗 LED 设为红色 */
nx_ws2812_set_brightness(&strip, 128);       /* 半亮度，编码时应用   */

/* 忙时立即拒绝：上一帧仍在发送时返回 false。 */
if (!nx_ws2812_update(&strip)) {
    /* 外设繁忙或 I/O 出错；下一周期重试，或轮询 nx_ws2812_busy。 */
}
```

> **注意：** `nx_ws2812_update` 返回 `true` 只表示外设已经接受数据，并不表示 LED 已完成锁存。使用 DMA 时，传输仍会在后台继续；可通过 `nx_ws2812_busy` 判断何时结束。请根据实际外设时钟选择 `bit0_pattern`、`bit1_pattern` 和 `reset_bytes`。高电平容差只有 ±150 ns；复位阶段则必须让数据线保持足够长的低电平，WS2812 通常要求超过 50 µs，部分 WS2812B 版本要求超过 280 µs。

## nx_kth7112 —— KTH7112 磁编码器 SPI 驱动

该驱动实现 KTH7112 的三线 SPI Mode 3 协议，包括命令帧组装、读取响应的 CRC 校验和寄存器锁定。SPI 端口由调用方管理，并通过片选、写、读和可选的时序回调接入。本模块不使用动态内存或浮点运算。

- **同步的帧级 API** —— `nx_kth7112_read_angle` 返回 16 位原始角度码；`nx_kth7112_read_reg8`、`nx_kth7112_write_reg8`、`nx_kth7112_read_reg16` 和 `nx_kth7112_write_reg16` 用于访问寄存器组。这些接口会在调用方上下文中同步完成所需的片选帧。`nx_kth7112_raw_to_mdeg` 可在不使用浮点运算的情况下，将原始角度码转换为毫度（千分之一度）。
- **每次读取均校验 CRC-8/ITU** —— 芯片会在每个读取响应后附加 CRC 字节。其参数为多项式 `0x07`、初值 `0x00`、输入和输出均不反射、最终异或值 `0x55`；标准测试串 `"123456789"` 的校验值为 `0xA1`。本模块使用内置的 256 项查找表计算 CRC，并在返回数据前完成校验。校验失败时返回 `NX_KTH7112_ERR_CRC`，且调用方的输出变量保持不变。
- **寄存器写入带回显确认** —— 写寄存器时，芯片会在同一帧中回送实际接受的值，驱动将其与发送值进行比较；若不一致，则返回 `NX_KTH7112_ERR_IO`。
- **由软件跟踪锁定状态** —— 芯片上电时锁定寄存器写入，并会静默忽略锁定期间的写操作。驱动同样从锁定状态开始；此时调用写接口会直接返回 `NX_KTH7112_ERR_LOCKED`，不会访问总线。`nx_kth7112_unlock` 可重复调用；重新初始化驱动后，或硬件锁定状态可能发生变化时，应再次调用该函数。
- **低字节位于低地址** —— 多字节字段的低字节存放在较低地址。例如，`NX_KTH7112_REG_ZERO_L` 对应 `ZERO[7:0]`，`NX_KTH7112_REG_ZERO_H` 对应 `ZERO[15:8]`。`nx_kth7112_read_reg16` 和 `nx_kth7112_write_reg16` 接收低字节地址，并通过两个独立帧访问相邻寄存器；每帧分别执行 CRC 校验或回显确认。
- **可选的端口回调** —— `is_busy` 和 `delay_ns` 均可设为 `NULL`。`is_busy` 为 `NULL` 时，驱动假定端口已就绪，适合阻塞式传输；`delay_ns` 为 `NULL` 时，驱动不会插入帧间延时，此时必须由端口自身满足时序要求。
- **非线程安全** —— 锁定状态存放在句柄中；若从多个上下文访问，调用方需自行串行化。

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

> **注意：** `nx_kth7112_write_mtp` 会将寄存器组写入非易失存储器，且该操作不可撤销。两次烧写操作之间必须间隔超过 `NX_KTH7112_MTP_MIN_INTERVAL_MS`（400 ms），烧写期间还需保持供电稳定。SPI 端口也必须保证寄存器写入帧中第 24 个时钟的高电平持续时间超过 100 ns，并使帧间隔超过 150 ns。请通过 SPI 时钟和末尾时钟行为满足前一项要求。驱动会在每帧结束后通过 `delay_ns` 请求 150 ns 延时；回调或端口必须确保实际间隔超过 150 ns。接近器件的 10 Mbps 上限时，应明确验证这两项时序。

