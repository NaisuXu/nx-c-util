# 中间件模块

## nx_can_bus —— CAN / CAN FD 帧结构与辅助函数

一个仅头文件的 CAN 帧模型及辅助函数集合。它适用于连接主机与 CAN 总线的工具或适配器，因此帧对象除载荷外，还可携带传输方向、通道和错误信息。

- **同时表示经典 CAN 和 CAN FD** —— `nx_can_msg_t` 覆盖两种帧格式。载荷是柔性数组成员，调用方按实际长度分配存储，最多 64 字节。`is_ext`、`is_remote`、`is_fd`、`brs`、`esi` 和 `dlc` 等属性保存在位域中，也可通过 `flags.raw` 整体复制或比较。
- **方向与通道信息** —— `dir`（`nx_can_dir_t`）区分三种对象：`TX` 表示主机请求适配器发送，`RX` 表示从总线接收，`TXR` 表示适配器报告先前发送的结果。4 位 `ch` 字段标识 CAN 通道，范围为 `0..NX_CAN_MAX_CH`。这两个字段仅用于主机工具或适配器的上下文。
- **统一的错误与结果编码** —— `is_err` 与 4 位 `err_code`（`nx_can_err_t`）使用同一套错误码。对 `RX` 帧，它们描述总线错误帧；对 `TXR`，它们描述发送失败原因，例如位、填充、格式、ACK 或 CRC 错误，以及仲裁丢失、总线关闭、超时和溢出。
- **DLC 辅助函数** —— `nx_can_dlc_to_len` 和 `nx_can_len_to_dlc` 在 4 位 DLC 和实际字节长度之间转换，处理 CAN FD 的尺寸（12/16/20/24/32/48/64）以及经典的 0..8。
- **仅头文件** —— 每个辅助函数都是 `static inline`；只需包含头文件，无需编译或链接。

```c
#include "nx_can_bus.h"

/* 一帧收到的 CAN FD 报文，载荷为 16 字节 */
uint8_t          buf[sizeof(nx_can_msg_t) + 16];
nx_can_msg_t    *msg = (nx_can_msg_t *)buf;
msg->id             = 0x123;
msg->flags.raw      = 0;                    /* 先清除所有标志 */
msg->flags.bits.dir = NX_CAN_DIR_RX;
msg->flags.bits.is_fd = 1;
msg->flags.bits.dlc = nx_can_len_to_dlc(16);   /* -> DLC 10 */
/* ... 填充 msg->data[0..15] ... */

uint32_t len = nx_can_dlc_to_len(msg->flags.bits.dlc);   /* 16 */

/* 工具报告发送失败：仲裁丢失 */
nx_can_msg_t txr;
txr.flags.raw           = 0;
txr.flags.bits.dir      = NX_CAN_DIR_TXR;
txr.flags.bits.is_err   = 1;
txr.flags.bits.err_code = NX_CAN_ERR_ARB_LOST;
```

> **注意：** `flags.bits` 的位域排列由编译器决定，不能视为跨平台线格式。帧需要在线上传输或跨工具链交换时，必须把各字段显式编码为预先定义的线格式；直接复制整个结构体或 `flags.raw` 都不能消除不同工具链之间的位域布局差异。


## nx_modbus_rtu —— Modbus RTU 帧结构与 CRC

提供常用 Modbus RTU 帧的内存布局，以及基于查找表的 CRC-16/MODBUS 实现。帧结构与线上字节流一一对应；CRC 实现在 `.c` 文件中。

- **帧结构与线上字节一一对应** —— 所有字段均为 `uint8_t`，结构体对齐为 1，不产生填充。收到的字节缓冲区可直接转换为相应帧类型并就地解析，无需 packing pragma。若结构体中加入其他类型的字段，这一保证将不再成立。
- **覆盖常用帧** —— 定长与变长的请求和响应（`nx_modbus_rtu_req_fix_t` / `req_var_t` / `rsp_fix_t` / `rsp_var_t`），以及异常响应（`nx_modbus_rtu_rsp_exc_t`）。功能码和异常码各有自己的枚举（`nx_modbus_fc_t`、`nx_modbus_exc_t`），它们与传输无关，可被未来的 TCP 模块共用。
- **线序** —— 16 位字段（地址、数量、寄存器值）在线缆上高位在前，按下面的代码那样重组。尾部 CRC 是小端（低字节在前）。对于变长帧，CRC 不是命名字段，`nx_modbus_rtu_req_var_crc` / `rsp_var_crc` 返回载荷之后指向它的指针。
- **自包含的 CRC 实现** —— `nx_modbus_rtu_crc16` 使用 256 项查找表计算 CRC-16/MODBUS，不依赖 `nx_crc`。`nx_modbus_rtu_set_crc` 写入帧尾 CRC，`nx_modbus_rtu_check_crc` 校验收到的帧。两者都要求帧长至少为 5 字节，即最短异常响应的长度。
- **以头文件为主、无依赖** —— 帧结构和字节辅助函数都在头文件里；只有 CRC 在 `.c` 中。不依赖其他模块。

```c
#include "nx_modbus_rtu.h"

/* 构造“读保持寄存器”请求：从站 1，起始地址 0x0000，数量 10 */
uint8_t buf[8];
nx_modbus_rtu_req_fix_t *req = (nx_modbus_rtu_req_fix_t *)buf;
req->addr = 1u;
req->cmd  = NX_MODBUS_FC_READ_HOLDING_REGS;
req->addr_h = (uint8_t)(0x0000u >> 8);   /* 起始地址 */
req->addr_l = (uint8_t)(0x0000u & 0xFFu);
req->qty_h  = (uint8_t)(10u >> 8);       /* 数量     */
req->qty_l  = (uint8_t)(10u & 0xFFu);
nx_modbus_rtu_set_crc(buf, sizeof(buf));        /* 填 crc_l / crc_h */

/* 收到帧后，先校验 CRC，再读某个 16 位字段 */
if (nx_modbus_rtu_check_crc(buf, sizeof(buf))) {
    uint16_t qty = (uint16_t)(((uint16_t)req->qty_h << 8) | req->qty_l);   /* 10 */
    (void)qty;
}
```

> **注意：** 把字节缓冲强转为帧结构依赖于上面全 `uint8_t` 的布局；同样的布局也是没有 packing pragma 的原因。多字节字段在线缆上高位在前，不要把它们当作原生 `uint16_t` 读取。


## nx_modbus_rtu_slave —— 事件驱动的 RTU 从站：帧 → 订阅分发

构建在 `nx_modbus_rtu` 之上的链路与分发层。它从串行接口读取字节，切分并校验完整 RTU 帧，再将请求路由到已订阅相应功能码和地址范围的应用模块。模块本身不包含业务逻辑，也不直接访问硬件；所有 I/O 都通过非阻塞回调注入，由主循环反复调用 `nx_modbus_rtu_slave_process()` 推进。

- **按订阅表分发** —— 每条订阅记录由功能码和闭区间地址范围组成。匹配的请求以引用计数 `nx_ref_msg` 发布到应用队列，无需复制载荷。同一功能码可按地址范围分给多个模块，一个请求也可同时匹配多个订阅者。若所有匹配队列都已满，从站返回 `0x06`（从站设备忙），提示主站稍后重试；若至少一个队列接收成功，则不返回异常。
- **按 Modbus 优先级校验请求** —— 对于模块能够识别帧布局的功能码，分发前先检查订阅表是否支持该功能（异常 `0x01`），再校验数量、`byte_count` 和单写值等结构字段（`0x03`），最后检查地址范围（`0x02`）。布局未知的功能码无法可靠确定帧长，解析器会逐字节重新同步，不会生成异常响应。应用模块收到的请求已通过结构校验，但具体值是否符合业务规则仍由应用判断；应用可通过异常响应辅助函数返回相应错误。
- **按功能码确定帧长** —— `01..06` 请求固定为 8 字节，`0F/10` 为 `9 + byte_count`，`17` 为 `13 + byte_count`。接收端因此无需依赖字符间隔（T3.5）判帧。地址或 CRC 不匹配时，解析器丢弃一个字节并重新同步；发送端则根据 `baud_rate` 在帧间保留 3.5 个字符时间。
- **注入式非阻塞 I/O** —— `read` 和 `write` 负责收发字节，`is_busy` 报告接口是否仍在发送，`dir_tx` 可控制 RS-485 DE 引脚，`get_us` 用于计算帧间隔。`read`、`write` 和 `is_busy` 共用 `io_ctx`，`dir_tx` 使用独立的 `dir_ctx`。若 `is_busy` 为 NULL，则认为 `write` 是阻塞式调用；若 `get_us` 为 NULL，则不等待帧间隔。`write` 返回 false 表示接口未接收该帧，模块会立即丢弃帧并释放方向控制。
- **显式释放实例状态** —— `nx_modbus_rtu_slave_deinit()` 会释放尚未发送完成的消息、撤销发送方向并把状态机恢复为空闲。重新初始化正在运行的实例或停止服务前应先调用它。该函数不会清空响应队列；队列中的消息引用仍由队列持有，消费者必须逐条取出并调用 `nx_ref_msg_release()`。
- **响应辅助函数** —— `nx_modbus_rtu_slave_reply_read()` 构造读响应，`nx_modbus_rtu_slave_reply_write()` 构造写确认，`nx_modbus_rtu_slave_reply_exception()` 构造异常响应。三者都直接接收内存池、响应队列、从站地址和功能码，无需传入从站句柄或完整请求指针。写确认还接收主机字节序的 16 位地址和 `data`：对 `05/06`，两者分别是目标地址和写入值；对 `0F/10`，两者分别是起始地址和数量。函数负责转换为 RTU 高字节在前的线序。返回值会区分内存不足、队列已满、广播请求和参数错误。
- **存储由调用方提供** —— 接收成帧缓冲、消息使用的分级内存池和共享响应队列全部由调用方持有，模块不使用系统堆。内存池耗尽时，响应会被丢弃，主站最终按超时处理。

```c
#include "nx_modbus_rtu_slave.h"

/* 一个业务模块负责保持寄存器 0x0000..0x000F */
const nx_modbus_rtu_slave_sub_t subs[] = {
    { NX_MODBUS_FC_READ_HOLDING_REGS, 0x0000, 0x000F, &valve_q },
    { NX_MODBUS_FC_WRITE_SINGLE_REG,  0x0000, 0x000F, &valve_q },
};

nx_modbus_rtu_slave_t     slave;
nx_modbus_rtu_slave_cfg_t cfg = {
    .slave_addr     = 0x11u,
    .baud_rate      = 115200u,         /* 用于计算 3.5 字符的发送间隔 */
    .pool           = &pool,           /* 消息使用的分级内存池 */
    .rx_buf         = rx_buf,
    .rx_size        = sizeof(rx_buf),
    .subs           = subs,
    .subs_count     = 2u,
    .response_queue = &response_queue, /* 业务响应和异常在此入队 */
    .read           = uart_read,       /* 注入的非阻塞 I/O */
    .write          = uart_write,
    .get_us         = board_micros,    /* is_busy 为 NULL 表示 write 阻塞 */
};
nx_modbus_rtu_slave_init(&slave, &cfg);

for (;;) {
    nx_modbus_rtu_slave_process(&slave);               /* 接收分发并推进发送 */
    valve_business(&valve_q, &response_queue, &pool);  /* 处理请求并推入响应 */
}
```

> **注意：** 从站校验的是**结构**（功能 `0x01`、值 `0x03`、地址 `0x02`），不是**语义**。业务模块仍需检查写入值是否在允许范围内，并可把自己的 `0x03` 异常推入响应队列。广播（地址 0）默认被丢弃，只处理点对点请求；配置 `accept_broadcast = true` 后广播会被分发，但从不应答：既不返回正常响应，也不返回异常。


## nx_modbus_rtu_master —— 事件驱动的 RTU 主站：队列 → 线路 → 订阅分发

构建在 `nx_modbus_rtu` 之上的主站链路与分发层。它从共享请求队列取出帧并发送，解析和校验收到的响应，再按从站地址和可选功能码分发给应用模块。所有 I/O 都通过非阻塞回调注入；主循环反复调用 `nx_modbus_rtu_master_process()` 推进收发状态机。

- **按从站地址分发** —— 每条订阅记录指定一个 `slave_addr` 和可选的 `func`。`func = 0` 表示接收该从站的所有响应；非零值可让不同应用按功能码共享同一设备。响应以引用计数消息发布，可同时到达多个订阅者；没有任何订阅者匹配时直接丢弃。
- **请求超时由应用管理** —— 本模块只负责发送和分发，不跟踪在途请求。需要超时或重试机制的应用应记录发送时间，并自行决定何时重试或放弃。响应队列暂时为空只表示尚未收到响应。
- **请求构造器** —— 每个功能码各有一个构造函数（`nx_modbus_rtu_master_read_holding_regs`、`..._write_multiple_regs` 等），负责构造帧、写入 CRC 并入队。它们只需要内存池和请求队列，业务模块无需持有主站句柄。数量越界、字节数与数量不一致、广播读等协议不允许的请求会立即被拒绝，无需等从站返回同样的结论。
- **按长度切片** —— 每个响应的长度都由它自身的字节决定（异常 5 字节，写确认 8 字节，读响应 `3 + byte_count + 2`），所以 RX 不需要字符间隔（T3.5）定时器，在到达时序不可信的繁忙总线上更稳。未收完的帧跨调用保留，收齐后才分发。遇到不支持的功能码或坏 CRC 时丢一个字节重同步。TX 侧每帧之后插入由 `baud_rate` 导出的 3.5 字符间隔。
- **每次调用只推进状态机** —— 一次 `process()` 不会连续发送多帧。配置 `is_busy` 后，当前帧发送完成前不会调用下一次 `write()`，也不会提前释放方向引脚，从而避免在共享 RS-485 总线上重叠驱动。
- **响应解析** —— 订阅者取到的是完整 ADU，CRC 已经校验。`nx_modbus_rtu_master_rsp_is_exception()` 判断是否为异常响应并给出异常码；`nx_modbus_rtu_master_rsp_data()` 定位读响应的载荷及其长度，返回指向消息内部的指针而不复制数据。
- **释放实例** —— `nx_modbus_rtu_master_deinit()` 归还发送中帧所占的内存池块，拉低方向引脚，并丢弃未接收完整的响应。该函数不会清空请求队列；队列中的消息引用仍由队列持有，消费者必须逐条取出并调用 `nx_ref_msg_release()`。

```c
#include "nx_modbus_rtu_master.h"

/* 两个业务模块，各拥有一个设备 */
const nx_modbus_rtu_master_sub_t subs[] = {
    { 0x11u, 0u, &q_pump  },      /* 来自 0x11 的全部响应 */
    { 0x22u, 0u, &q_meter },      /* 来自 0x22 的全部响应 */
};

nx_modbus_rtu_master_t     master;
nx_modbus_rtu_master_cfg_t cfg = {
    .baud_rate     = 115200u,        /* 导出 3.5 字符的 TX 间隔 */
    .pool          = &pool,          /* 消息用的分级池 */
    .rx_buf        = rx_buf,
    .rx_size       = sizeof(rx_buf),
    .subs          = subs,
    .subs_count    = 2u,
    .request_queue = &request_queue, /* 此队列中的请求会被发送 */
    .read          = uart_read,      /* 注入的非阻塞 I/O */
    .write         = uart_write,
    .get_us        = board_micros,   /* is_busy 为 NULL 表示 write 是阻塞的 */
};
nx_modbus_rtu_master_init(&master, &cfg);

/* 某业务模块请求 10 个寄存器；响应会进入它自己的队列 */
nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue, 0x11u, 0x0000u, 10u);

for (;;) {
    nx_modbus_rtu_master_process(&master);           /* 推进发送并分发接收帧 */
    pump_business(&q_pump, &request_queue, &pool);  /* 处理响应并按需发起新请求 */
}
```

> **注意：** RTU 帧没有事务 ID，只能依靠从站地址和功能码匹配请求与响应。若同时向同一从站发送两个功能码相同的请求，返回的响应在帧层面无法区分。需要严格配对时，每个从站应只保留一个在途请求。已放弃请求的迟到响应也可能与当前请求的响应无法区分。


## nx_tp_sdu —— 传输层服务数据单元

一个只定义数据类型的头文件，用于描述诊断传输层与上层之间交换的对象。每个对象包含一条完整消息，以及上层处理和响应所需的寻址、链路和结果信息。不同传输层都填充同一个结构体，上层无需了解具体承载协议。

- **一条报文一次分配** —— 载荷是柔性数组成员，因此头部和载荷位于同一个内存池块中，并以指针形式在调用方提供的队列之间传递。
- **寻址方式** —— `ta_type` 记录报文是发给单个接收者还是发给所有接收者（见 `nx_tp_ta_type_t`），这决定了到底该不该作答。
- **连接标记** —— `link` 标明报文属于哪条连接，编号方式由应用自己决定。传输层只把配置中的值拷贝进每一条推出的报文，从不解释它，因此一个上层可以用一个队列服务多个传输层实例，仍能把它们区分开。
- **对象类型** —— `kind`（`nx_tp_sdu_kind_t`）区分收到的完整消息和发送确认。两类对象可以共用同一输出队列。
- **操作结果** —— `result`（`nx_tp_result_t`）说明操作如何结束，包括成功、各类超时、序号或流控错误、WAIT 次数超限，以及接收空间不足。枚举值按结算优先级排列，可直接比较。
- **32 位长度** —— `len` 以字节计算载荷。报文能有多长取决于配置及其背后的内存，而不取决于这个字段的宽度。
- **只有类型、无依赖** —— 无需编译、无需链接。一个零初始化的实例表示一条成功的、物理寻址的接收指示，因此只需填写与此不同的字段。

```c
#include "nx_tp_sdu.h"

/* 交给传输层的请求：发送 data 中的 len 个字节 */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, sizeof(nx_tp_sdu_t) + 2u);
nx_tp_sdu_t  *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
s->len     = 2u;
s->data[0] = 0x22u;
s->data[1] = 0xF1u;

/* 反向收到对象时，根据 kind 决定如何解释其余字段 */
const nx_tp_sdu_t *in = (const nx_tp_sdu_t *)nx_ref_msg_data(msg);
if (in->kind == NX_TP_SDU_INDICATION) {
    handle(in->link, in->ta_type, in->data, in->len);   /* 收到完整消息 */
} else if (in->result != NX_TP_N_OK) {
    report(in->link, in->result);                       /* 发送失败 */
}
```

> **注意：** SDU 没有单独的方向字段，方向由所在队列决定。传输层从输入队列读取待发消息，并将接收指示和发送确认写入输出队列。`kind` 用于区分接收指示与发送确认；`result` 则描述对应操作的结果。


## nx_can_isotp —— ISO 15765-2（DoCAN / ISO-TP）分段传输

一个基于队列的 ISO-TP 传输层，用于收发超过单个 CAN 帧容量的消息。接收方向从 CAN 队列取帧，重组完整消息后交给上层；发送方向从上层队列取消息，分段后按协议节奏写入 CAN 发送队列。模块不直接访问总线或硬件，所有对象都以 `nx_ref_msg` 形式在调用方提供的队列之间传递，并由 `nx_can_isotp_process()` 推进状态。

- **同一实例可用于 ECU 或测试仪** —— 配置对应的收发 ID 后，实例既可接收诊断请求并由上层响应，也可通过 `nx_can_isotp_send()` 主动发送请求。两种方向可以同时工作。
- **支持完整的 ISO-TP 帧类型** —— 支持单帧，包括 ISO 15765-2:2016 为大于 7 字节载荷定义的扩展长度格式；支持 12 位长度和 32 位扩展长度的首帧；支持序号在 0..15 之间回绕的连续帧；也支持携带 `CTS`、`WAIT`、`OVERFLOW`、块大小和 `STmin` 的流控帧。
- **物理寻址与功能寻址分别配置** —— `phys_rx_id` 是物理接收 ID，`phys_tx_id` 用于发送物理消息及接收过程中的流控帧。可选的 `func_rx_id` 和 `func_tx_id` 用于 1:N 功能寻址。功能寻址只允许单帧，因为共享请求 ID 无法承载针对单个节点的流控。收到的消息通过 `ta_type` 标记寻址类型。配置项直接填写 CAN ID，因此同时适用于 11 位 ID、UDS 的 `0x18DA..xx` 29 位 ID 及自定义方案。
- **超时参数可配置** —— `n_as_us` 和 `n_ar_us` 分别限制发送端、接收端将本地帧交给链路的等待时间；`n_bs_us` 限制发送端等待流控帧的时间；`n_cr_us` 限制接收端等待下一连续帧的时间；`n_wft_max` 限制连续接受 `WAIT` 帧的次数。每个 `WAIT` 都会重新开始一个完整的 `n_bs_us` 窗口。字段为 0 时使用默认值。
- **本地发送队列繁忙时自动重试** —— 向 `can_tx_queue` 入队失败表示本地链路暂时无法接收该帧。模块会在后续 `nx_can_isotp_process()` 调用中重试，直到成功或超过 `n_as_us` / `n_ar_us`，后者以 `N_TIMEOUT_A` 结束会话。计时从首次尝试入队时开始；模块无法观察帧实际离开总线的时刻，因此这里限制的是把帧交给本地发送队列的等待时间。用于拒绝接收的 `OVERFLOW` 流控帧只尝试入队一次，因为该接收会话已经结束。
- **接收方控制流量** —— `rx_block_size` 和 `rx_stmin` 会写入本实例发出的流控帧，要求对端每发送指定数量的连续帧后等待下一次许可，并满足最小帧间隔。每完成一个块，模块都会发送新的流控帧。
- **接收长度上限** —— `rx_max_len` 限制单个实例可接收的消息长度。首帧声明长度超限时，模块在分配内存前发送 `OVERFLOW`。设为 0 表示不施加实例级限制，但实际长度仍受内存池和模块硬上限约束。
- **模块级硬上限** —— `NX_CAN_ISOTP_MAX_MSG_LEN` 取 `size_t` 上限与 32 位线格式上限中的较小值，再减去随平台变化的消息头开销。因此，在 `size_t` 至少为 32 位的平台上，该值略小于 4 GiB。发送更长消息时，`nx_can_isotp_send()` 返回 `NX_CAN_ISOTP_ERR_LENGTH`；收到声明长度更大的首帧时，模块在分配前发送 `OVERFLOW`。具体实例通常还会受 `rx_max_len` 和内存池块尺寸限制。
- **可选结果通知** —— 启用 `confirm_tx` 后，模块会为发送结束和接收失败发布 SDU，报告流控或连续帧超时、序号错误、无效流控状态、WAIT 次数超限或长度溢出。上层通过 `kind` 区分这些通知与正常接收消息。
- **每个实例绑定一个 CAN 通道** —— `ch` 会写入所有发送帧，并用于过滤接收帧。多通道驱动可据此路由；单通道系统可让驱动和配置都使用 0。
- **帧格式可配置** —— `ext_id` 选择 29 位标识符，`fd_frames` 选择 CAN FD，`brs` 启用 CAN FD 数据段的位速率切换。接收时也会校验标识符宽度，数值相同的 11 位和 29 位 ID 不会被视为同一地址。
- **帧载荷长度可配置** —— `max_frame_len` 必须是 DLC 可精确表示的 8、12、16、20、24、32、48 或 64 字节之一。大于 8 字节时必须启用 `fd_frames`，`brs` 也依赖 CAN FD。更大的帧可减少分段数量。
- **可选 CAN 帧填充** —— 启用 `pad_frames` 后，经典 CAN 帧会补齐到 8 字节，未使用部分填入 `pad_byte`。未启用时，帧只携带实际数据。CAN FD 帧仍会按 DLC 可表示的长度补齐。
- **发送节奏有明确上限** —— `tx_frames_per_process` 限制每次 `process()` 最多生成的帧数；配置为 0 时按 255 帧处理，并非无限制。对端流控中的 `STmin` 则通过注入时钟控制连续帧间隔。
- **直接在最终消息中重组** —— 收到首帧后，模块从内存池分配完整消息空间，并将后续数据直接写入其中。重组完成后可直接发布给上层，无需再复制载荷。

```c
static _Alignas(max_align_t) uint8_t pool_mem[24u * 1024u];
static nx_tiered_mem_pool_t pool;
static nx_ref_msg_t *sdu_tx_buf[8], *sdu_rx_buf[4], *can_rx_buf[16], *can_tx_buf[16];
static nx_queue_t sdu_tx_q, sdu_rx_q, can_rx_q, can_tx_q;
static nx_can_isotp_t iso;

const nx_tiered_level_cfg_t tiers[] = {
    {sizeof(nx_ref_msg_t) + sizeof(nx_can_msg_t) + 8u,       32},  /* CAN 帧 */
    {sizeof(nx_ref_msg_t) + sizeof(nx_can_isotp_sdu_t) + 4096u, 4},/* 报文 */
};
const nx_tiered_mem_pool_cfg_t pool_cfg = {
    .memory = pool_mem, .memory_size = sizeof(pool_mem),
    .tiers = tiers, .tier_count = 2,
};
if (nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL) != NX_TIERED_OK) {
    /* 配置无效或容量不足：停止初始化。 */
}
nx_ref_msg_queue_init(&sdu_tx_q, sdu_tx_buf, 8);
nx_ref_msg_queue_init(&sdu_rx_q, sdu_rx_buf, 4);
nx_ref_msg_queue_init(&can_rx_q, can_rx_buf, 16);
nx_ref_msg_queue_init(&can_tx_q, can_tx_buf, 16);

const nx_can_isotp_cfg_t cfg = {
    .max_frame_len = NX_CAN_ISOTP_FRAME_8,  /* 8/12/16/20/24/32/48/64 */
    .ch            = 0u,                    /* 本实例服务的 CAN 通道 */
    .ext_id        = false,                 /* true = 29 位标识符 */
    .fd_frames     = false,                 /* true = CAN FD；大于 8 时必须 */
    .brs           = false,                 /* true = 位速率切换；需要 FD */
    .pad_frames    = true,                  /* 每帧补齐到 8 字节 */
    .pad_byte      = 0xCCu,                 /* 未用尾部填什么 */
    .phys_rx_id    = 0x7E0u,                /* 接收物理寻址报文的 ID */
    .phys_tx_id    = 0x7E8u,                /* 发送报文与流控所用的 ID */
    .func_rx_id    = 0x7DFu,                /* 功能寻址请求；填 0 表示关闭 */
    .func_tx_id    = 0u,                    /* 上位机实例在此填广播 ID */
    .pool          = &pool,
    .sdu_rx_queue  = &sdu_rx_q,             /* 上层 -> 模块：待发的报文 */
    .sdu_tx_queue  = &sdu_tx_q,             /* 模块 -> 上层：收到的报文 */
    .can_rx_queue  = &can_rx_q,             /* 驱动 -> 模块：收到的帧 */
    .can_tx_queue  = &can_tx_q,             /* 模块 -> 驱动：分段后的帧 */
    .link          = 1u,                    /* 写入每条推出的 SDU */
    .confirm_tx    = true,                  /* 上报每次发送的结果 */
    .get_us        = board_micros,
    .n_as_us       = 1000000u,              /* 0 = 1000 ms */
    .n_ar_us       = 1000000u,              /* 0 = 1000 ms */
    .n_bs_us       = 1000000u,              /* 0 = 1000 ms */
    .n_cr_us       = 1000000u,              /* 0 = 1000 ms */
    .n_wft_max     = 4u,                    /* 0 = 4 */
    .rx_max_len    = 4096u,                 /* 超过则回 OVERFLOW 拒收 */
    .rx_block_size = 8u,                    /* 0 = 一次收完整条报文 */
    .rx_stmin      = 0x0Au,                 /* 要求对端帧间隔 10 ms */
    .tx_frames_per_process = 1u,            /* 0 = 每次最多发送 255 帧 */
};
nx_can_isotp_init(&iso, &cfg);

for (;;) {
    can_driver_fill(&can_rx_q);   /* 驱动把收到的帧作为 nx_ref_msg 推入 */
    nx_can_isotp_process(&iso);   /* 重组、发流控、分段、按节奏发送 */
    can_driver_drain(&can_tx_q);  /* 驱动取走帧并发送出去 */

    nx_ref_msg_t *m;                          /* 取走模块推出的内容 */
    while (nx_queue_pop(&sdu_tx_q, &m) == NX_QUEUE_OK) {
        const nx_can_isotp_sdu_t *sdu = nx_ref_msg_data(m);
        if (sdu->kind == NX_TP_SDU_INDICATION && sdu->result == NX_TP_N_OK) {
            uds_handle(&iso, sdu->ta_type, sdu->data, sdu->len);
        } else {
            uds_report(&iso, sdu->kind, sdu->result);   /* 发送结束，或接收失败 */
        }
        nx_ref_msg_release(m);                /* 消费者释放自己的引用 */
    }
}
```

> **注意：** 队列名称均以本模块为参照：模块从 `sdu_rx_queue` 和 `can_rx_queue` 读取对象，向 `sdu_tx_queue` 和 `can_tx_queue` 写入对象。因此，上层通过 `sdu_rx_queue` 提交待发消息，通过 `sdu_tx_queue` 接收重组消息和结果通知。
>
> `process()` 会从 `can_rx_queue` 取走并最终释放每一帧，包括通道、标识符宽度或 ID 不匹配的帧。驱动必须正确填写 `ch` 和 `is_ext`，并使其与实例配置一致。多个实例不能直接共享同一个 `can_rx_queue`：先运行的实例会消费所有帧，其他实例无法再看到它们。应为每个实例配置独立接收队列，或先在驱动层分流。
>
> 从 `sdu_tx_queue` 取出的对象由消费者持有一个引用，处理完成后必须调用 `nx_ref_msg_release()`；否则内存块不会归还内存池。`can_tx_queue` 中的帧也遵循同一规则，应由驱动在发送完成后释放。


## nx_uds —— ISO 14229 公共类型与服务描述

定义各 UDS 模块共享的枚举、掩码和结构体，包括服务与正响应标识符、负响应码、会话类型及掩码、服务处理阶段，以及服务处理器接口。该模块仅包含头文件，没有运行状态，也无需初始化。

- **服务标识符与正响应** —— `nx_uds_sid_t` 列出服务标识符。`NX_UDS_SID_TO_POS_RSP()` 和 `NX_UDS_POS_RSP_TO_SID()` 在请求 SID 与正响应 SID 之间转换；`NX_UDS_NEG_RSP_SID` 和 `NX_UDS_NEG_RSP_LEN` 描述固定三字节负响应。
- **负响应码** —— `nx_uds_nrc_t` 定义服务器可能返回的负响应码。`NX_UDS_NRC_NONE` 表示当前没有负响应码，不代表协议中的 `0x00` 响应码。
- **用位掩码声明会话** —— 每个标准会话值对应掩码中的同名位。`NX_UDS_SESSION_BIT()` 生成单个会话位，`NX_UDS_SESSION_MASK_ALL` 和 `NX_UDS_SESSION_MASK_NON_DEFAULT` 提供常用集合，`NX_UDS_SESSION_MAX` 给出可表示的最大会话值。
- **正响应抑制位** —— `NX_UDS_SUPPRESS_POS_RSP_BIT` 表示子功能字节的第 7 位。`NX_UDS_SUPPRESSES_POS_RSP()` 检查该位，`NX_UDS_SUB_FUNCTION()` 返回去除该位后的子功能值。
- **服务处理上下文** —— `nx_uds_ctx_t` 向处理器提供请求及长度、去除抑制位后的子功能、请求到达时的会话和安全等级、已预填正响应 SID 的输出缓冲区，以及可跨处理阶段保存的临时状态。`nx_uds_phase_t` 表示本次调用所处阶段，`nx_uds_disposition_t` 表示处理结果。
- **服务表项** —— `nx_uds_service_t` 以数据形式描述一个服务，包括 SID、处理函数、允许的会话和安全等级、支持的子功能、可选的子功能会话约束，以及请求长度范围。

下表按 ISO 14229-1 功能单元列出 SID 及其实现位置。基础会话与复位服务由 `nx_uds_svc_session` 提供；安全访问状态机位于 `nx_uds_svc_sec`；上传下载状态机位于 `nx_uds_svc_transfer`。其他服务没有专用模块，应用可自行实现处理器并通过 `nx_uds_service_t` 注册。

| SID | 服务 | 功能单元（ISO 14229-1:2020） | 模块 |
|---|---|---|---|
| 0x10 | DiagnosticSessionControl | 10 诊断与通信管理 | `nx_uds_svc_session` |
| 0x11 | ECUReset | 10 诊断与通信管理 | `nx_uds_svc_session` |
| 0x27 | SecurityAccess | 10 诊断与通信管理 | `nx_uds_svc_sec` |
| 0x28 | CommunicationControl | 10 诊断与通信管理 | — |
| 0x3E | TesterPresent | 10 诊断与通信管理 | `nx_uds_svc_session` |
| 0x83 | AccessTimingParameter | 10 诊断与通信管理 | — |
| 0x84 | SecuredDataTransmission | 10 诊断与通信管理 | — |
| 0x85 | ControlDTCSetting | 10 诊断与通信管理 | — |
| 0x86 | ResponseOnEvent | 10 诊断与通信管理 | — |
| 0x87 | LinkControl | 10 诊断与通信管理 | — |
| 0x22 | ReadDataByIdentifier | 11 数据传输 | — |
| 0x23 | ReadMemoryByAddress | 11 数据传输 | — |
| 0x24 | ReadScalingDataByIdentifier | 11 数据传输 | — |
| 0x2A | ReadDataByPeriodicIdentifier | 11 数据传输 | — |
| 0x2C | DynamicallyDefineDataIdentifier | 11 数据传输 | — |
| 0x2E | WriteDataByIdentifier | 11 数据传输 | — |
| 0x3D | WriteMemoryByAddress | 11 数据传输 | — |
| 0x14 | ClearDiagnosticInformation | 12 存储数据传输 | — |
| 0x19 | ReadDTCInformation | 12 存储数据传输 | — |
| 0x2F | InputOutputControlByIdentifier | 13 输入输出与例程控制 | — |
| 0x31 | RoutineControl | 13 输入输出与例程控制 | — |
| 0x34 | RequestDownload | 15 上传下载 | `nx_uds_svc_transfer` |
| 0x35 | RequestUpload | 15 上传下载 | `nx_uds_svc_transfer` |
| 0x36 | TransferData | 15 上传下载 | `nx_uds_svc_transfer` |
| 0x37 | RequestTransferExit | 15 上传下载 | `nx_uds_svc_transfer` |
| 0x38 | RequestFileTransfer | 15 上传下载 | — |

```c
#include "nx_uds.h"

/* 一个服务，用数据描述：读一个数据标识符，在所有会话可用，无需解锁，无子功能，请求长度固定。 */
static nx_uds_disposition_t read_did(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;

    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    /* out[0] 已填入 0x62；追加所请求的标识符与一个字节。 */
    ctx->out[1]  = ctx->req[1];
    ctx->out[2]  = ctx->req[2];
    ctx->out[3]  = 0x5Au;
    ctx->out_len = 4u;
    return NX_UDS_DISPOSITION_DONE;
}

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_READ_DATA_BY_IDENTIFIER,
        .handler      = read_did,
        .user         = NULL,
        .flags        = 0u,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .sec_level    = 0u,
        .min_len      = 3u,
        .max_len      = 3u
    }
};
```

> **注意：** 会话值直接用作位索引，因此掩码只能表示 `NX_UDS_SESSION_MAX` 以内的会话。ISO 14229 标准会话均在此范围内，但延伸到 `0x7E` 的厂商或供应商会话无法由该掩码表示；`NX_UDS_SESSION_BIT()` 对超出范围的值返回 0，不会执行越界移位。服务表项的会话掩码不能为 0，初始化时会拒绝此类配置。

## nx_uds_server —— ISO 14229 诊断服务器（ECU 侧）

ISO 14229 诊断服务器状态机。应用通过 `nx_uds_server_indicate()` 提交请求 A_PDU 及其寻址类型，服务器查找对应服务并生成响应 A_PDU，再通过应用提供的回调发送。模块与具体传输层无关，每个实例一次维护一个诊断事务。

应用通过 `nx_uds_service_t` 表注册服务；新增服务只需实现处理器并添加表项，无需修改服务器。服务器统一管理当前会话、安全等级、S3 会话超时、ISO 14229-2 响应时序、慢事务的 `responsePending` 通知，以及无法分派请求时的负响应。

- **一次只处理一个事务** —— 当前请求完成前不会启动第二个事务。忙碌期间调用 `nx_uds_server_indicate()` 会返回 `ERR_BUSY`；需要多轮主循环的处理器可一直保留事务，直到完成或超时。
- **响应时序由配置决定** —— P2、P2* 和 P4 均以微秒配置。超过 P2 时，服务器发送 `responsePending`；到达 P4 或发送次数达到 `max_pending` 时，以先发生者为准终止事务，并返回 `p4_nrc` 指定的负响应码。
- **维护会话和安全状态** —— 服务器记录当前会话及已解锁安全等级。非默认会话在持续 `s3_us` 未收到请求后回退到默认会话；每个已接受的请求都会重新启动 S3 计时。
- **只使用调用方缓冲区** —— 请求在事务期间保存在 `req_buf`，响应在 `out_buf` 中构造。处理器可跨多个主循环周期继续使用这两个缓冲区，服务器不会动态分配内存。
- **未匹配的请求也走统一响应路径** —— 没有服务表项匹配时，服务器启动一个负响应事务，并通过与正响应相同的输出机制发送。

服务器还提供服务处理器所需的状态接口：`_session` 和 `_sec_level` 查询当前会话及安全等级，`_set_session` 和 `_set_sec_level` 更新状态，`_touch` 重启 S3 计时器，`_timing`、`_now` 和 `_apdu_limits` 查询时序与缓冲限制，`_is_busy` 判断是否有事务正在运行。

```c
#include "nx_uds.h"
#include "nx_uds_server.h"

static bool send_response(void *user, uint8_t link, const uint8_t *rsp,
                          uint32_t len, uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;
    return can_send(myself, rsp, len);   /* 已排队，或返回 false 稍后重试 */
}

static nx_uds_disposition_t read_did(nx_uds_ctx_t *ctx, void *user)
{
    (void)user;

    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    ctx->out[1] = ctx->req[1];   /* 所请求的标识符 */
    ctx->out[2] = ctx->req[2];
    ctx->out[3] = 0x5Au;         /* 一个字节的数据 */
    ctx->out_len = 4u;
    return NX_UDS_DISPOSITION_DONE;
}

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_READ_DATA_BY_IDENTIFIER,
        .handler      = read_did,
        .flags        = 0u,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .min_len      = 3u,
        .max_len      = 3u
    }
};

static uint32_t board_micros(void) { return timer_read_us(); }

static nx_uds_server_t srv;
static uint8_t req_buf[64];
static uint8_t out_buf[64];

static void server_setup(void)
{
    nx_uds_server_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.services       = services;
    cfg.services_count = 1u;
    cfg.out_fn         = send_response;
    cfg.req_buf        = req_buf;
    cfg.req_buf_size   = sizeof(req_buf);
    cfg.out_buf        = out_buf;
    cfg.out_buf_size   = sizeof(out_buf);
    cfg.max_req_apdu   = sizeof(req_buf);
    cfg.get_us         = board_micros;

    nx_uds_server_init(&srv, &cfg);
}
```

> **注意：** 事务进行期间到达的新请求会收到 `ERR_BUSY`，当前事务不会被取消。调用方决定如何处理被拒绝的新请求：可返回 `0x21` 提示客户端重试，也可静默丢弃功能寻址请求。保持当前事务不变，可避免一条无关请求（例如广播 TesterPresent）中断正在执行的操作。

## nx_uds_svc_session —— 基础会话与复位服务

提供三个常用的基础 UDS 服务处理器：`0x10` DiagnosticSessionControl、`0x11` ECUReset 和 `0x3E` TesterPresent。它们与应用自定义服务一样，通过普通 `nx_uds_service_t` 表项注册。每个表项的 `user` 指针指向相应配置；模块本身不保存独立运行状态。

- **0x10 会话控制** —— 正响应回显目标会话，并携带服务器实际使用的 P2 和 P2* 时序。只有正响应成功交给传输层后，服务器才切换会话。进入任何会话都会重新锁定安全访问状态。
- **0x11 ECU 复位** —— 正响应回显复位类型；子功能 `0x04` 还会返回配置的掉电时间。复位同样在正响应成功交给传输层后执行，避免客户端只看到 ECU 突然离线。
- **0x3E TesterPresent** —— 正响应回显子功能，不执行其他业务操作。服务器会为每个已接受请求重启 S3 计时器，因此该服务可用于保持当前诊断会话。

头文件为每个处理器给出了服务表项必须使用的长度和子功能约束。初始化不会自动纠正错误表项；若声明了处理器不支持的长度或子功能，服务器可能生成错误响应，因此应按文档配置。

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_session.h"

static nx_uds_server_t srv;

static bool allow_session(void *user, uint8_t from, uint8_t to, uint8_t *nrc)
{
    (void)user; (void)from; (void)to; (void)nrc;
    return !driving_now();          /* 行驶中拒绝进入编程会话 */
}

static nx_uds_svc_session_cfg_t session_cfg = {
    .srv      = &srv,
    .allow_fn = allow_session,
};

static void do_reset(void *user, uint8_t reset_type)
{
    (void)user;
    if (reset_type == NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN) {
        power_down_requested = true;      /* 0x04 只记录，不立即复位 */
    } else {
        board_reset(reset_type);
    }
}

static nx_uds_svc_session_reset_cfg_t reset_cfg = {
    .do_fn           = do_reset,
    .power_down_time = 0xFFu,            /* 没有可用的掉电时间 */
};

static const uint8_t sessions[] = {
    NX_UDS_SESSION_DEFAULT, NX_UDS_SESSION_PROGRAMMING, NX_UDS_SESSION_EXTENDED,
};
static const uint8_t resets[] = {
    NX_UDS_RESET_HARD, NX_UDS_RESET_KEY_OFF_ON, NX_UDS_RESET_SOFT,
    NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN,
};
static const uint8_t tester_present_sub = NX_UDS_SVC_SESSION_TESTER_PRESENT_SUB;

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler      = nx_uds_svc_session_control,
        .user         = &session_cfg,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = sessions,
        .subs_count   = 3u,
        .min_len      = 2u,
        .max_len      = 2u
    },
    {
        .sid          = NX_UDS_SID_ECU_RESET,
        .handler      = nx_uds_svc_session_ecu_reset,
        .user         = &reset_cfg,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = resets,
        .subs_count   = 4u,
        .min_len      = 2u,
        .max_len      = 2u
    },
    {
        .sid          = NX_UDS_SID_TESTER_PRESENT,
        .handler      = nx_uds_svc_session_tester_present,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = &tester_present_sub,
        .subs_count   = 1u,
        .min_len      = 2u,
        .max_len      = 2u
    },
};
```

> **注意：** `0x10` 和 `0x11` 的状态变更发生在 `CONFIRM` 或 `SILENCE` 阶段，而不是生成响应时。处理器会再次确认缓冲区中是本服务的正响应，因此负响应不会触发会话切换或复位。若请求设置了“抑制正响应”位，操作仍会在本应发送响应的时刻执行；若响应未能交给传输层，则不会执行。

## nx_uds_svc_sec —— 0x27 种子/密钥交换

实现 UDS `0x27` SecurityAccess 的种子 / 密钥流程。每个安全等级对应一对子功能：奇数子功能请求种子，紧随其后的偶数子功能提交密钥。应用通过回调生成种子并验证密钥；本模块不实现密码算法，也不生成随机数，只负责请求顺序、尝试计数和延时锁定。

- **子功能对映射安全等级** —— 等级 *n* 使用 `NX_UDS_SVC_SEC_SEED_SUB(n)` 和 `NX_UDS_SVC_SEC_KEY_SUB(n)`。例如等级 1 对应 `0x01/0x02`，等级 2 对应 `0x03/0x04`，上限为 `NX_UDS_SVC_SEC_MAX_LEVEL`。只有配置表中列出的等级才可用。
- **每个等级的字节数固定** —— 每个等级声明其种子与密钥各有多长。无论种子是现算的还是该等级已解锁，种子应答都恰好这么长；密钥必须恰好是声明的长度。
- **每个种子只验证一次** —— 提交密钥后，无论验证成功还是失败，对应种子都会失效。再次尝试前必须重新请求种子，不能重复提交同一密钥。
- **失败次数与延时锁定** —— 每次形式正确但验证失败的密钥都会增加尝试计数。达到 `max_attempts` 后，在 `delay_us` 内所有 `0x27` 请求都返回 `0x37`，且不会调用应用回调。会话切换或重新锁定不会清除该状态。应用可通过 `nx_uds_svc_sec_get_lockout()` 和 `nx_uds_svc_sec_set_lockout()` 将尝试次数及剩余等待时间持久化。

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_sec.h"

static nx_uds_server_t srv;

static bool make_seed(void *user, uint8_t level, const uint8_t *record,
                      uint32_t record_len, uint8_t *seed, uint32_t seed_cap,
                      uint32_t *seed_len)
{
    (void)user; (void)level; (void)record; (void)record_len;
    (void)seed_cap;
    seed[0] = random_byte();
    *seed_len = 1u;
    return true;
}

static bool judge_key(void *user, uint8_t level, const uint8_t *seed,
                      uint32_t seed_len, const uint8_t *key, uint32_t key_len)
{
    (void)user;
    return level == 1u && seed_len == 1u && key_len == 1u
           && key[0] == (uint8_t)(seed[0] ^ 0x5Au);   /* 仅是示例 */
}

static const nx_uds_svc_sec_level_t levels[] = {
    { .level = 1u, .seed_len = 1u, .key_len = 1u },
};

static uint8_t seed_buf[4];

static nx_uds_svc_sec_t sec;
static void sec_setup(void)
{
    nx_uds_svc_sec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.srv          = &srv;
    cfg.levels       = levels;
    cfg.levels_count = 1u;
    cfg.seed_fn      = make_seed;
    cfg.verify_fn    = judge_key;
    cfg.seed_buf     = seed_buf;
    cfg.seed_buf_size = sizeof(seed_buf);

    nx_uds_svc_sec_init(&sec, &cfg);
}
```

> **注意：** 只有格式正确并实际进入密钥验证的请求才计为失败尝试。未先请求种子就提交密钥时，服务器返回 `0x24`（请求顺序错误），但不增加计数。若种子请求抑制了正响应，种子不会发送，也不会建立可供后续验证的待处理状态。失败计数由所有安全等级共享，不能通过轮换等级绕过上限。

## nx_uds_svc_transfer —— 内存上传与下载服务

实现四个 UDS 内存传输服务：`0x34` RequestDownload、`0x35` RequestUpload、`0x36` TransferData 和 `0x37` RequestTransferExit。四个处理器分别注册为普通服务表项，并共享一次传输所需的状态，包括方向、目标区域、当前进度和下一个块序号。实际内存访问由应用回调完成，本模块不会直接解引用传输地址。

- **启动、传输和结束** —— `0x34` 启动下载（客户端向服务器写入），`0x35` 启动上传（客户端从服务器读取），随后通过任意数量的 `0x36` 块传输数据，最后由 `0x37` 结束。一个实例同时只允许一个活动传输。设置 `require_full_size = true` 后，只有声明区域全部处理完成才能正常结束；默认值 `false` 允许应用通过关闭回调自行判断是否接受提前结束。
- **响应中公布最大块长** —— 启动响应给出服务器可接收的最大完整 `0x36` 消息长度，其中包含 SID 和块序号。`nx_uds_svc_transfer_payload_room()` 可扣除这两个字节，得到实际载荷空间。
- **重复块具有幂等性** —— 若客户端因响应丢失而重发上一块序号，服务器会重新返回相同响应，但不会再次写入或推进地址。其他非预期序号会被拒绝，传输保持活动，客户端仍可用正确序号继续。
- **最后一个上传块可以更短** —— 上传时只读取声明区域中尚未发送的字节。下载块若会越过客户端声明的区域末尾，则直接拒绝。
- **结束校验由应用决定** —— `0x37` 在构造正响应前调用关闭回调，应用可在此校验完整映像或标记分区有效。传输处理器本身不会更改会话或安全等级；与其他已接受请求一样，服务器接收 `0x37` 时会按常规刷新 S3 计时器。

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_transfer.h"

static nx_uds_server_t srv;

static bool open_transfer(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t addr,
                          nx_uds_svc_transfer_addr_t size, uint8_t format, uint32_t *block_len,
                          uint8_t *nrc)
{
    (void)user; (void)format; (void)block_len; (void)nrc;
    return (dir == NX_UDS_SVC_TRANSFER_DOWNLOAD) && addr == FLASH_BASE && size > 0u;
}

static bool write_block(void *user, nx_uds_svc_transfer_addr_t addr, const uint8_t *data,
                        uint32_t len, uint8_t *nrc)
{
    (void)user; (void)addr; (void)data; (void)len; (void)nrc;
    return true;    /* flash_write_buffered(addr, data, len); */
}

static bool read_block(void *user, nx_uds_svc_transfer_addr_t addr, uint8_t *out,
                       uint32_t len, uint8_t *nrc)
{
    (void)user; (void)addr; (void)out; (void)len; (void)nrc;
    return true;    /* flash_read(addr, out, len); */
}

static bool close_transfer(void *user, nx_uds_svc_transfer_dir_t dir, nx_uds_svc_transfer_addr_t done,
                           nx_uds_svc_transfer_addr_t size, const uint8_t *record,
                           uint32_t record_len, uint8_t *out, uint32_t out_cap,
                           uint32_t *out_len, uint8_t *nrc)
{
    (void)user; (void)dir; (void)done; (void)size; (void)record;
    (void)record_len; (void)out; (void)out_cap; (void)out_len; (void)nrc;
    return flash_checksum_ok();
}

static nx_uds_svc_transfer_t xfer;
static void xfer_setup(void)
{
    nx_uds_svc_transfer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.srv           = &srv;
    cfg.open_fn       = open_transfer;
    cfg.write_fn      = write_block;
    cfg.read_fn       = read_block;
    cfg.close_fn      = close_transfer;
    cfg.max_block_len = FLASH_WRITE_UNIT;
    cfg.require_full_size = true;

    nx_uds_svc_transfer_init(&xfer, &cfg);
}

/* 四行承载该传输；每行以同一句柄作为 user。 */
static const nx_uds_service_t services[] = {
    { .sid = NX_UDS_SID_REQUEST_DOWNLOAD,  .handler = nx_uds_svc_transfer_request_download,
      .user = &xfer, .min_len = 5u, .max_len = 33u },
    { .sid = NX_UDS_SID_REQUEST_UPLOAD,    .handler = nx_uds_svc_transfer_request_upload,
      .user = &xfer, .min_len = 5u, .max_len = 33u },
    { .sid = NX_UDS_SID_TRANSFER_DATA,     .handler = nx_uds_svc_transfer_data,
      .user = &xfer, .min_len = 2u, .max_len = 0u },
    { .sid = NX_UDS_SID_REQUEST_TRANSFER_EXIT,
      .handler = nx_uds_svc_transfer_exit, .user = &xfer,
      .min_len = 1u, .max_len = 0u },
};
```

> **注意：** `0x36` SID 后的第一个字节是块序号，不是子功能。该服务表项不能设置 `NX_UDS_SVC_HAS_SUB_FUNCTION`，否则序号的第 7 位会被误认为“抑制正响应”位，导致一半序号不返回响应。该表项的 `max_len` 应设为 0；真正的长度上限来自启动响应公布的块长，超出时按传输边界错误处理。

## nx_uds_tp_bind —— UDS 端点与传输层绑定

连接 UDS 端点与任意使用 `nx_tp_sdu_t` 的传输层。端点可由 `side` 选择为 `nx_uds_server` 或 `nx_uds_client`；传输层通过一条队列发布接收指示和发送确认，并从另一条队列读取待发响应或请求。绑定模块负责在队列和所选端点之间转换、转发消息。它只依赖两个队列和一个内存池，不依赖具体传输协议；每条传输通路使用一个绑定实例。

- **统一处理消息生命周期与重试** —— 端点复制所需数据后，绑定立即释放输入消息。输出队列暂时无法接收时，服务器或客户端会在后续周期重试同一响应或请求。服务器响应始终使用物理寻址；客户端请求则保留事务指定的寻址类型。
- **每次最多处理一条输入消息** —— `nx_uds_tp_bind_process()` 每次最多从输入队列取出一个对象，因为服务器一次只回答一个请求，客户端一次也只发起一个事务。该函数不会推进服务器或客户端状态机；应用应明确安排传输层、绑定和端点的调用顺序。
- **每条通路独立配置内存池** —— 不同绑定可使用不同内存池，避免一条通路的消息洪泛耗尽其他通路的存储。
- **统计失败与丢弃** —— `busy` 和 `refused` 统计未被端点接受的输入消息；`no_memory`、`queue_full` 和 `too_long` 统计输出消息发布失败的尝试。端点会重试暂时失败的输出，因此同一消息可能使计数增加多次；这些计数不都代表最终丢失了不同消息。持续增长通常表示容量、长度限制或处理节奏需要调整。

初始化绑定时，它会把自身安装为所选端点的输出通路：服务器侧覆盖 `out_fn` / `out_user`，客户端侧安装发送回调。因此应先初始化服务器或客户端，再初始化绑定。

```c
#include "nx_queue.h"
#include "nx_ref_msg.h"
#include "nx_tiered_mem_pool.h"
#include "nx_tp_sdu.h"
#include "nx_uds_server.h"
#include "nx_uds_tp_bind.h"

static nx_uds_server_t srv;
static nx_tiered_mem_pool_t pool;
static nx_queue_t sdu_in_q, sdu_out_q;

static nx_uds_tp_bind_t bind;
static void bind_setup(void)
{
    nx_uds_tp_bind_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.side        = NX_UDS_TP_SERVER;
    cfg.srv         = &srv;
    cfg.sdu_in      = &sdu_in_q;   /* 传输层推入的已收到内容 */
    cfg.sdu_out     = &sdu_out_q;  /* 传输层取出并发送的内容 */
    cfg.pool        = &pool;
    cfg.link        = 1u;
    cfg.max_sdu_len = 4096u;

    nx_uds_tp_bind_init(&bind, &cfg);
}
```

> **注意：** 在服务器侧，无论请求采用物理寻址还是功能寻址，响应都必须使用物理寻址。传输层根据待发 SDU 的 `ta_type` 选择 CAN ID；绑定模块会在每个服务器响应中写入 `NX_TP_TA_PHYSICAL`，避免把响应发到广播 ID。客户端侧则保留 `nx_uds_client_request()` 指定的寻址类型。

## nx_uds_client —— ISO 14229 诊断客户端（测试仪侧）

ISO 14229 诊断客户端状态机。应用提交请求 A_PDU，客户端通过注入的发送回调交给传输层；收到响应后，应用通过 `nx_uds_client_indicate()` 将响应字节和寻址类型交回客户端。结果回调会报告收到的响应，或说明事务为何未获得响应。每个实例一次只处理一个事务，适合测试仪按请求 / 响应方式与 ECU 通信。

事务从提交请求开始。发送成功后，客户端在 P2 窗口内等待响应；若收到 NRC `0x78`（`responsePending`），则切换到 P2* 窗口继续等待，并限制允许的扩展次数。`0x10` 正响应可公布新的 P2 和 P2*，客户端默认在后续事务中采用这些值；启用 `fixed_timing` 后则始终使用本地配置。

- **一次只处理一个事务** —— 当前请求产生最终结果后，客户端才接受下一请求。事务进行期间调用 `nx_uds_client_request()` 会返回 `ERR_BUSY`。
- **结果回调提供事务状态** —— 回调会区分正响应、负响应、协议错误、超时和取消。若请求设置了“抑制正响应”位，则等待窗口内保持静默也可作为正常结果。
- **发送结果由传输层确认** —— 传输层通过 `nx_uds_client_confirm()` 报告请求是否成功发送。若请求未进入链路，客户端可立即结束事务，无需等待响应超时。发送通道暂时繁忙时，客户端会在后续 `process()` 调用中重试同一请求，直到成功或超过 `send_timeout_us`。
- **默认采用服务器公布的时序** —— 未启用 `fixed_timing` 时，客户端使用服务器在会话控制正响应中公布的 P2 和 P2*；这样，超时判断与服务器承诺的响应窗口一致。
- **只使用调用方缓冲区** —— 当前请求保存在 `req_buf`，收到的响应保存在 `rsp_buf`。两个缓冲区均由调用方提供，客户端不会动态分配内存。

客户端提供若干状态查询接口：`_is_busy` 判断是否有事务正在进行，`_session` 返回当前会话，`_timing` 返回当前等待窗口，`_set_send` 允许绑定模块在客户端初始化后安装发送通道。

```c
#include "nx_uds.h"
#include "nx_uds_client.h"

static bool send_request(void *user, uint8_t link, const uint8_t *req,
                         uint32_t len, uint8_t ta_type)
{
    (void)user; (void)link; (void)ta_type;
    return can_send(myself, req, len);   /* 已排队，或返回 false 稍后重试 */
}

static void report(void *user, nx_uds_client_t *clt, nx_uds_client_result_t result)
{
    (void)user;
    const uint8_t *rsp = clt->cfg.rsp_buf;
    uint32_t len = nx_uds_client_resp_len(clt);
    if (result == NX_UDS_CLIENT_RESULT_NEGATIVE) {
        reason_code = rsp[2];            /* 拒绝该请求的 NRC */
    }
}

static uint32_t board_micros(void) { return timer_read_us(); }

static nx_uds_client_t clt;
static uint8_t req_buf[16];
static uint8_t rsp_buf[64];

static void client_setup(void)
{
    nx_uds_client_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.result_fn     = report;
    cfg.send_fn       = send_request;
    cfg.req_buf       = req_buf;
    cfg.req_buf_size  = sizeof(req_buf);
    cfg.rsp_buf       = rsp_buf;
    cfg.rsp_buf_size  = sizeof(rsp_buf);
    cfg.link          = 1u;
    cfg.get_us        = board_micros;

    nx_uds_client_init(&clt, &cfg);
}

static void enter_programming_session(void)
{
    /* 0x10 0x02，物理寻址：请求进入编程会话。 */
    nx_uds_client_request(&clt, NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                          NX_UDS_SESSION_PROGRAMMING, NULL, 0u,
                          NX_TP_TA_PHYSICAL);
    while (nx_uds_client_is_busy(&clt)) {
        nx_uds_client_process(&clt);   /* 每次主循环迭代 */
    }
}
```

> **注意：** `nx_uds_client_request()` 只构造并登记请求，不会立即调用发送通道。第一次 `nx_uds_client_process()` 才尝试发送，因此在 `request()` 返回后立即读取 `rsp_buf` 不会得到有效响应。即使发送通道已接收请求，事务也仍在进行，只有结果回调触发后才算结束。对抑制正响应的请求，正常结束时 `rsp_buf` 也不会包含响应数据。
