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

