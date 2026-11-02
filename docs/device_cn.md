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

## nx_ktm5220 —— KTM5220 磁编码器 SPI 驱动

该驱动依据 KTM5220 v0.5（2026-05-14）手册实现四线 SPI 协议。端口使用 Mode 3、MSB 优先，时钟不超过 20 MHz。模块沿用 `nx_kth7112` 的回调接入方式，不使用动态内存或浮点运算。

- **角度与报警** —— `nx_ktm5220_read_sample` 在同一帧读取 21 位角度和 3 位状态，并校验 CRC。角度范围为 `0 .. 0x1FFFFF`，可用 `nx_ktm5220_raw_to_mdeg` 转换为毫度。状态掩码分别表示转速过快或存储器 CRC 异常、ADC 幅值报警、供电欠压。CRC 正确时，即使存在报警，`read_sample` 仍返回 `NX_KTM5220_OK`，调用方应检查 `sample.status`；只读取角度的 `nx_ktm5220_read_angle` 则会在存在报警时返回 `NX_KTM5220_ERR_STATUS`。各读取接口出错时均不改动输出值。
- **寄存器与写保护** —— `read_reg8`/`write_reg8` 和 `read_reg16`/`write_reg16` 提供通用访问，16 位字段按低地址低字节组织，通过独立帧访问，并拒绝地址回绕。写入前调用 `nx_ktm5220_unlock`：向 `0x1A` 写入 `0xAA`，再进行带 CRC 的回读，确认值为 `0x01`；未解锁时写接口返回 `NX_KTM5220_ERR_LOCKED`。手册未提供主动上锁命令。芯片复位后应重新初始化句柄并在配置前解锁。写寄存器没有回显，需要确认写入结果时，应由应用额外回读。
- **零点与校准** —— `get_zero`/`set_zero` 访问 21 位零点偏移，`zero_here` 触发自动捕获当前位置并置零。`set_zero` 和 `update_reg8` 会保留无关位；`set_calibration` 控制 `CAL_REG_SET`，`read_calibration_status` 读取两位校准状态。电机转速与轮询超时由应用管理。多帧操作不具备原子性，写入中途出错时可能已有部分字节生效。
- **同步全双工传输** —— `cs(ctx, true)` 拉低 CSN，`false` 拉高。每个片选窗口仅调用一次 `transfer(ctx, tx, rx, len)`，完成整帧全双工收发，可直接映射到硬件 SPI 的收发接口。`tx`、`rx` 均非空且为独立缓冲区；回调必须在全部 `len` 字节发送、接收完成后才返回。驱动在接收响应期间提供全零占位字节，忽略命令期间处于高阻状态的 MISO 数据。完整帧长度为：角度读取 6 字节，寄存器读取 4 字节，寄存器写入和 MTP 烧写均为 3 字节。可选的 `is_busy` 用于在 SPI 端口忙时立即拒绝本帧；可选的 `delay_ns` 会在释放片选前请求 13 ns 延时，以满足最后一次 SCK 上升沿到 CSN 上升沿至少 12.5 ns 的要求。没有该回调时须由端口自身保证保持时间。`nx_ktm5220_busy` 只反映 SPI 端口状态，不能判断校准或 MTP 是否完成。共享句柄或 SPI 总线时，须串行执行完整 API 调用。

```c
#include "nx_ktm5220.h"

/* spi_cs 和 spi_transfer 由应用实现，均为同步回调。 */
bool encoder_init(nx_ktm5220_t *enc, void *spi_ctx)
{
    const nx_ktm5220_cfg_t cfg = {
        .cs         = spi_cs,
        .transfer   = spi_transfer, /* 整帧全双工传输 */
        .is_busy    = NULL,
        .delay_ns   = delay_ns,  /* 端口能保证 TH 时可设为 NULL */
        .io_ctx     = spi_ctx,
        .crc_invert = false,     /* 对应上电默认 SPI_CRC 配置 */
    };
    return nx_ktm5220_init(enc, &cfg);
}

bool encoder_read_mdeg(nx_ktm5220_t *enc, uint32_t *mdeg)
{
    nx_ktm5220_sample_t sample;
    if (mdeg == NULL ||
        nx_ktm5220_read_sample(enc, &sample) != NX_KTM5220_OK) {
        return false;
    }
    if (sample.status != 0u) {
        return false;            /* 应用按各状态位处理报警 */
    }
    *mdeg = nx_ktm5220_raw_to_mdeg(sample.raw);
    return true;
}

nx_ktm5220_ret_t encoder_set_offset(nx_ktm5220_t *enc, uint32_t raw)
{
    nx_ktm5220_ret_t ret = nx_ktm5220_unlock(enc);
    if (ret != NX_KTM5220_OK) {
        return ret;
    }
    return nx_ktm5220_set_zero(enc, raw);  /* 仅修改 RAM，不发送 MTP 命令 */
}
```

CRC 参数为多项式 `0x07`、初值 `0x00`、不反射。`cfg.crc_invert = false` 选择最终异或 `0x00`，`true` 选择 `0xFF`，必须与芯片的 `SPI_CRC` 配置一致。所提供的 v0.5 手册未公开该配置的寄存器地址，也未明确 CRC 覆盖范围。本驱动按响应数据计算：寄存器读取覆盖一个数据字节，角度读取覆盖完整的三个角度/状态字节；首次接入硬件时需核对这一解释。

`nx_ktm5220_write_mtp` 发送烧写命令并校验单字节应答 `0x55`；成功只代表命令获得应答，不能视为烧写完成。应用须保持供电稳定，等待至少 `NX_KTM5220_MTP_PROGRAM_TIME_MS`（2501 ms），随后重新上电，再进行其他操作。即使返回 I/O 错误，只要命令可能已到达芯片，也应执行等待，不能立即重试。驱动不阻塞等待，也不计时跟踪烧写完成状态。手册第 9.2.1 与 9.2.3 节对寄存器触发校准是否自动保存存在矛盾；`set_calibration` 仅发送触发操作，但芯片自身可能烧写 MTP，应用使用前需确认实际行为。

可运行的模拟 SPI 示例位于 `examples/device/nx_ktm5220_example.c`，随 `nx_device_examples` 一起执行。

## nx_mp6546 — MP6546 三相电机驱动

本模块根据 MP6546 Rev. 1.0（2023-03-20）实现寄存器语义，使用调用方提供的存储和整数运算。芯片根据 d/q 轴电压指令及转子角度生成三相输出；电流、速度、位置环，以及对齐、电压/电流限制、EN、时序和故障恢复由应用管理。

公开手册未给出完整的 I²C 事务和字节序，因此必须提供经过验证的同步 `read_reg16`、`write_reg16` 平台回调。本模块不包含 I²C 线上协议后端，也不根据寄存器表猜测帧格式。每个寄存器地址对应**一个 16 位寄存器**，不能当作两个相邻的 8 位寄存器访问。回调的 `uint16_t` 值按手册 bit[15:0] 的数值解释，与 CPU 大小端无关；字节序、START/重复 START/STOP、ACK、总线释放和有界超时均由移植层处理。即使使用 DMA，也须在全部传输完成后才返回。`addr7` 为未左移、不含读写位的地址，只支持 `0x07`、`0x08`、`0x09`。配置结构会复制到句柄中，`io_ctx` 指向的对象须在句柄使用期间保持有效。

- **初始化与配置** —— `nx_mp6546_init` 只校验并复制配置，不进行 I/O、探测、复位、EN 操作或电机启动；返回成功不代表硬件已经就绪。传感器、驱动模式和角度源接口通过读改写保留无关位和保留位。`set_motor_angle` 设置 1–15 对极、12 位机械角偏移及方向，不执行对齐，也不推断偏移符号；中途出错可能只完成部分配置。`set_test_angle` 设置 12 位固定角度，仅在矢量模式且显式选择 `NX_MP6546_ANGLE_TEST` 后使用。
- **样本与故障** —— `read_sample` 从同一次 `0x0C` 读取获得 15 位机械角和综合故障位，即使 `sample.fault` 为真也返回 `NX_MP6546_OK`。只读角度的 `read_angle` 遇到该故障会返回 `NX_MP6546_ERR_STATUS`，角度输出保持不变。`read_faults` 单独读取 `0x0D`，返回保持原位的 `NX_MP6546_FAULT_*` 标志，不包含 INL 补偿字节；它与角度样本并非同时采集。读取和转换接口在任何错误时均不修改输出。`raw_to_mdeg` 将有效的 `0..0x7FFF` 角度转换为 `0..359989` 毫度；输出位宽不代表传感器精度。
- **矢量指令与单位** —— `set_dq` 接受 `-512..511` 的有符号原始码，编码为 10 位补码，先写 Ud（`0x08`），再写 Uq（`0x09`）；`set_uq` 只写 Uq。这些指令字的高位保留位发送为零。应用须已配置矢量模式，接口不检查或切换模式。`voltage_to_raw` 通过整数运算计算 `round(512 * sqrt(3) * voltage_mv / bus_voltage_mv)`；母线电压为零或舍入结果越界时返回错误，不进行饱和截断。应用传入实测母线电压，并限制 d/q 电压矢量的总幅值、变化率、电流和堵转时间。
- **PWM 指令** —— 应用已选择占空比模式后，`set_pwm` 先校验全部三相，再执行 I/O；即使某相被关闭，其 `duty_permille` 也必须在 `0..1000` 范围内。使能相的占空比四舍五入到硬件的 8 位步长，低字节随占空比增大而减小：0‰ 编码为 `0x1FF`，1000‰ 编码为 `0x100`。关闭相编码为零，与使能但占空比为 0‰ 的情况不同。A/B/C 依次写入 `0x07/0x08/0x09`，手册说明写 `0x09` 时提交整组。中途出错可能留下待提交值或已生效值；应用完成恢复后，应重新提交完整三相组。
- **寄存器访问** —— 原始接口拒绝未公开地址、向只读寄存器写入和读取命令寄存器；原始写入的保留位及副作用由调用方负责。`update_reg16` 使用 `(old & ~mask) | (value & mask)`，只接受文档明确的普通可写字段，拒绝命令、只读、生产码和保留字段；掩码为零时不进行 I/O，读失败时不会继续写。修改硬件地址后，句柄不会自动更新；须由应用确认新地址，再用新地址重新初始化句柄。

共享句柄或总线时，应串行执行完整 API 调用，包括读改写和多寄存器操作。D/Q 和多寄存器配置写入不保证原子性。任何失败的写入都可能已到达芯片，硬件可能处于部分更新或未知状态。模块不自动重试、回滚或操作 EN，何时停止输出以及如何恢复由应用决定。

矢量模式与占空比模式共用 `0x07..0x09`。模式设置接口不重新初始化这些寄存器，也不安排安全切换；旧值在切换后可能被解释为另一种输出指令。应用须管理模式切换、就绪时序和输出指令。EN 拉低会完全关闭芯片，因此不能假定 EN 低时仍可配置。`enter_normal` 只发送工作模式命令，不复位或清除故障；`enter_standby` 设置 `STDBY_EN` 后发送待机命令，LDO 行为取决于 `TRIM_ENP`。SCL/SCK 下降沿可唤醒芯片，共享总线上的通信也可能结束待机，因此待机不等于可靠关断。

使用 SPI 角度模式时，应用还须按第 11 页选择传感器角度读取：显式向 `NX_MP6546_REG_SPI_CMD`（`0x11`）写入零并检查结果。`read_sample` 只读取 `0x0C`，不会隐式修改传感器命令或配置其协议。

以下应用片段接入经过验证的端口。调用 `motor_step` 前，应用必须已经显式完成供电和矢量模式配置、极对数/方向/偏移标定，选择传感器角度源，完成对齐并建立 Ud = 0。调用方提供经过限幅的 Uq 指令，检查返回值，并按自身 EN/恢复策略处理故障或 I/O 错误。

```c
#include "nx_mp6546.h"
#include <stddef.h>

/* 应用端口：同步完成已经验证的寄存器事务。 */
extern bool port_read_reg16(void *ctx, uint8_t addr7,
                            uint8_t reg, uint16_t *value);
extern bool port_write_reg16(void *ctx, uint8_t addr7,
                             uint8_t reg, uint16_t value);

bool motor_bind(nx_mp6546_t *motor, void *port_ctx)
{
    const nx_mp6546_cfg_t cfg = {
        .read_reg16  = port_read_reg16,
        .write_reg16 = port_write_reg16,
        .io_ctx      = port_ctx,
        .addr7       = NX_MP6546_ADDR_LOW,
    };
    return nx_mp6546_init(motor, &cfg); /* 只建立软件绑定。 */
}

nx_mp6546_ret_t motor_step(nx_mp6546_t *motor, int32_t uq_mv,
                           uint32_t bus_mv, uint32_t *angle_mdeg)
{
    if (angle_mdeg == NULL) {
        return NX_MP6546_ERR_PARAM;
    }
    nx_mp6546_sample_t sample;
    nx_mp6546_ret_t ret = nx_mp6546_read_sample(motor, &sample);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    if (sample.fault) {
        return NX_MP6546_ERR_STATUS;
    }
    int16_t uq;
    ret = nx_mp6546_voltage_to_raw(uq_mv, bus_mv, &uq);
    if (ret != NX_MP6546_OK) {
        return ret;
    }
    ret = nx_mp6546_set_uq(motor, uq);
    if (ret != NX_MP6546_OK) {
        return ret; /* 输出可能已改变，由调用方处理恢复。 */
    }
    *angle_mdeg = nx_mp6546_raw_to_mdeg(sample.angle_raw);
    return NX_MP6546_OK;
}
```

模块不提供 OTP 烧录、故障标志清除或广播命令：手册未公开所需的编程、清除语义及完整广播协议，寄存器写成功不代表掉电保存。手册还存在 ADC 位数（第 1 页为 12 位、第 5 页为 10 位）、过热关断阈值（第 5 页与第 13 页）及 `FLT_N_SVM` 位宽/频率编码（第 12 页与第 15–16 页）的矛盾，因此只公开该滤波字段的原位掩码，不提供频率枚举。Ud 按第 12 页和寄存器表使用 `0x08`，第 17 页标题误写为 `0x09`。上述事项须向 MPS 确认；平台事务、控制时序和保护行为还需在真实硬件上验证，主机寄存器模拟不能证明硬件兼容性。

