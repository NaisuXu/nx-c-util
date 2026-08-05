# 中间件模块
### nx_can_bus —— CAN / CAN FD 帧结构与辅助函数

一个仅头文件的模块，提供 CAN 帧的通用内存表示和小巧、无依赖的辅助函数。面向位于主机
与总线之间的工具/适配器，因此帧不仅携带数据，还带有方向和错误上下文。

- **经典 CAN 与 CAN FD** —— 一个 `nx_can_msg_t` 同时覆盖两者；载荷是柔性数组成员，
  因此调用者按实际长度（最多 64 字节）分配存储。帧属性（`is_ext`、`is_remote`、
  `is_fd`、`brs`、`esi`、`dlc`）被打包进一个位域，同时也暴露一个 `flags.raw` 字用于
  快速拷贝/比较。
- **主机/工具方向与通道** —— `dir`（见 `nx_can_dir_t`）区分 `TX`（主机请求工具发送）、
  `RX`（从总线收到）和 `TXR`（工具对先前 `TX` 的发送完成报告）。`ch` 是 4 位通道号：
  与 `dir` 一样，只在工具/适配器上下文中有意义，用于标明帧属于哪个 CAN 接口。
- **错误/结果报告** —— 一个 `is_err` 标志加一个 4 位 `err_code`（见 `nx_can_err_t`）
  在两个方向上共用一套编码：在 `RX` 帧上它标明错误帧的成因，在 `TXR` 报告上它标明发送
  失败的原因（位/填充/格式/应答/CRC 错误、仲裁丢失、总线关闭、超时、溢出……）。
- **DLC 辅助函数** —— `nx_can_dlc_to_len` 和 `nx_can_len_to_dlc` 在 4 位 DLC 和实际
  字节长度之间转换，处理 CAN FD 的尺寸（12/16/20/24/32/48/64）以及经典的 0..8。
- **仅头文件** —— 每个辅助函数都是 `static inline`；只需包含头文件，无需编译或链接。

```c
#include "nx_can_bus.h"

/* a received CAN FD frame carrying 16 bytes */
uint8_t          buf[sizeof(nx_can_msg_t) + 16];
nx_can_msg_t    *msg = (nx_can_msg_t *)buf;
msg->id             = 0x123;
msg->flags.raw      = 0;                    /* clear all flags first */
msg->flags.bits.dir = NX_CAN_DIR_RX;
msg->flags.bits.is_fd = 1;
msg->flags.bits.dlc = nx_can_len_to_dlc(16);   /* -> DLC 10 */
/* ... fill msg->data[0..15] ... */

uint32_t len = nx_can_dlc_to_len(msg->flags.bits.dlc);   /* 16 */

/* the tool reports a failed transmit: arbitration was lost */
nx_can_msg_t txr;
txr.flags.raw           = 0;
txr.flags.bits.dir      = NX_CAN_DIR_TXR;
txr.flags.bits.is_err   = 1;
txr.flags.bits.err_code = NX_CAN_ERR_ARB_LOST;
```

> **注意：** `flags.bits` 是一种内存布局；位域的排列顺序由编译器定义。当帧要跨越线缆
> 传输或在不同工具链之间移动时，应序列化 `flags.raw`（或显式打包各字段），而不要直接
> memcpy 整个结构体。


### nx_modbus_rtu —— Modbus RTU 帧结构与 CRC

常用 Modbus RTU 帧的内存表示，外加一个表驱动的 CRC-16/MODBUS。帧结构与线缆字节流
1:1 对应，而 CRC 需要一张小的查找表，因此本模块带一个 `.c` 文件。

- **帧结构与线缆 1:1 对应** —— 每个结构体只由 `uint8_t` 字段组成，因此对齐为 1、无
  填充，收到的字节缓冲可直接强转为对应类型就地解析，无需任何 packing pragma。让每个
  帧字段都是 `uint8_t` 正是这一保证的来源 —— 非 `uint8_t` 字段可能引入填充、破坏
  1:1 对应。
- **覆盖常用帧** —— 定长与变长的请求和响应（`nx_modbus_rtu_req_fix_t` / `req_var_t`
  / `rsp_fix_t` / `rsp_var_t`），以及异常响应（`nx_modbus_rtu_rsp_exc_t`）。功能码
  和异常码各有自己的枚举（`nx_modbus_fc_t`、`nx_modbus_exc_t`），它们与传输无关，
  可被未来的 TCP 模块共用。
- **字节序辅助函数** —— 16 位字段（地址、数量、寄存器值）在线缆上是大端；用
  `nx_modbus_rtu_get_u16` / `set_u16` 转换。尾部 CRC 是小端（低字节在前）。对于变长
  帧，CRC 不是命名字段 —— `nx_modbus_rtu_req_var_crc` / `rsp_var_crc` 返回载荷之后
  指向它的指针。
- **自包含的 CRC** —— `nx_modbus_rtu_crc16` 用一张 256 项的表计算 CRC-16/MODBUS
  （不依赖 `nx_crc`）；`nx_modbus_rtu_set_crc` 填充帧尾部的 CRC，
  `nx_modbus_rtu_check_crc` 校验收到的 CRC。两个帧辅助函数都要求长度至少为 5（最短的
  有效 ADU 是 5 字节的异常响应）。
- **以头文件为主、无依赖** —— 帧结构和字节辅助函数都在头文件里；只有 CRC 在 `.c` 中。
  不依赖其他模块。

```c
#include "nx_modbus_rtu.h"

/* build a "read holding registers" request: addr 1, start 0x0000, count 10 */
uint8_t buf[8];
nx_modbus_rtu_req_fix_t *req = (nx_modbus_rtu_req_fix_t *)buf;
req->addr = 1u;
req->cmd  = NX_MODBUS_FC_READ_HOLDING_REGS;
nx_modbus_rtu_set_u16(&req->addr_h, 0x0000u);   /* starting address */
nx_modbus_rtu_set_u16(&req->qty_h, 10u);        /* quantity         */
nx_modbus_rtu_set_crc(buf, sizeof(buf));        /* fill crc_l / crc_h */

/* on a received frame, verify the CRC then read a 16-bit field */
if (nx_modbus_rtu_check_crc(buf, sizeof(buf))) {
    uint16_t qty = nx_modbus_rtu_get_u16(&req->qty_h);   /* 10 */
    (void)qty;
}
```

> **注意：** 把字节缓冲强转为帧结构依赖于上面全 `uint8_t` 的布局；同样的布局也是没有
> packing pragma 的原因。多字节字段仍需用 `get_u16` / `set_u16` 处理大端线序 —— 不要
> 把它们当作原生 `uint16_t` 读取。


### nx_modbus_rtu_slave —— 事件驱动的 RTU 从站：帧 → 订阅分发

架设在 `nx_modbus_rtu`（帧结构 + CRC）之上的链路/分发层。它从线上拉取字节，切分并校验
完整的 RTU 帧，再把每个请求路由给订阅了它的业务模块 —— 自身不含任何业务逻辑。它不接触
硬件：所有 I/O 都以非阻塞回调的形式注入，并由主循环里单次 `process()` 调用驱动。

- **订阅分发** —— 每个业务模块在订阅表里声明一段 `(功能码 + 闭区间地址范围)`；匹配的
  请求会零拷贝地（以引用计数的 `nx_ref_msg`）扇出到该模块的队列。同一功能码可按范围拆
  给多个模块，同一请求也可同时送达多个订阅者。
- **按异常优先级做结构校验** —— 分发之前，从站先结算仅凭帧本身即可判定的部分：功能是否
  受支持（`0x01`）、数量 / byte_count / 单写值的合法性（`0x03`）、地址是否落在范围内
  （`0x02`）。因此被分发的请求必然是结构良好的；而某个值对具体寄存器是否**在业务上**可
  接受，仍归业务模块判断，它可以推送自己的异常响应。
- **基于长度的成帧** —— 每个受支持帧的长度都由功能码决定（`01..06` 为 8 字节，`0F/10`
  为 `9 + byte_count`），因此接收侧无需字符间（T3.5）定时器 —— 在到达时序不可信的繁忙
  总线上更稳健。地址或 CRC 出错后，丢弃一个字节重新同步。发送侧在每帧之后插入一段 3.5
  字符的间隔（由 `baud_rate` 推导）。
- **注入式非阻塞 I/O** —— `read` / `write` 搬运字节，`is_busy` 报告接口是否仍在发送
  （使共享、非独占的总线只在空闲时才被驱动），可选的 `dir_tx` 翻转 RS-485 方向（DE）
  引脚，`get_us` 为发送间隔计时。`is_busy` 为 NULL 时把 `write` 视作阻塞完成；`get_us`
  为 NULL 时跳过间隔。串口回调（`read` / `write` / `is_busy`）共用 `io_ctx`，当驱动是
  模块自有的单一实例时可保持 NULL；`dir_tx` 用独立的 `dir_ctx`（DE 引脚常是另一个 GPIO），
  `get_us` 作为系统级时间源不带任何 ctx。
- **自身不做任何分配** —— 接收成帧缓冲、每条消息背后的分层内存池、共享的响应队列全部由
  调用方持有。内存耗尽时优雅降级：响应被丢弃，主站超时即可。

```c
#include "nx_modbus_rtu_slave.h"

/* one business module owns holding registers 0x0000..0x000F */
const nx_modbus_rtu_slave_sub_t subs[] = {
    { NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 0x000F, &valve_q },
    { NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0000, 0x000F, &valve_q },
};

nx_modbus_rtu_slave_t     slave;
nx_modbus_rtu_slave_cfg_t cfg = {
    .slave_addr     = 0x11u,
    .baud_rate      = 115200u,         /* derives the 3.5-char TX gap */
    .pool           = &pool,           /* tiered pool for messages */
    .rx_buf         = rx_buf,
    .rx_size        = sizeof(rx_buf),
    .subs           = subs,
    .subs_count     = 2u,
    .response_queue = &response_queue, /* business replies + exceptions go here */
    .read           = uart_read,       /* injected non-blocking I/O */
    .write          = uart_write,
    .get_us         = board_micros,    /* is_busy NULL => write is blocking */
};
nx_modbus_rtu_slave_init(&slave, &cfg);

for (;;) {
    nx_modbus_rtu_slave_process(&slave);            /* RX dispatch + TX pump */
    valve_business(&valve_q, &response_queue, &pool);  /* drain inbox, push replies */
}
```

> **注意：** 从站校验的是**结构**（功能 `0x01`、值 `0x03`、地址 `0x02`），不是**语义**。
> 业务模块仍需对被要求写入的实际值做范围检查，并可把自己的 `0x03` 异常推入响应队列。
> 广播（地址 0）会被分发但从不应答 —— 不回响应，也不回异常。


