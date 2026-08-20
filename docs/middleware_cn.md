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
- **面向业务模块的应答辅助函数** —— 三个构造函数覆盖业务模块能给出的全部答复：
  `nx_modbus_rtu_slave_reply_read()` 把收集到的数据包上字节数，
  `nx_modbus_rtu_slave_reply_write()` 构造回显请求的写确认，
  `nx_modbus_rtu_slave_reply_exception()` 上报一个异常码。它们都只要池和响应队列，业务模块
  无需持有从站句柄；应答的地址与功能码取自请求帧。三者都返回 `nx_modbus_rtu_slave_ret_t`，
  指明应答未能入队的原因 —— `ERR_NOMEM` 与 `ERR_FULL` 是值得打日志的资源短缺，
  `ERR_BROADCAST` 是广播请求的正常结果，`ERR_PARAM` 则是调用方的 bug。
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
> 广播（地址 0）默认被丢弃，只处理点对点请求；配置 `accept_broadcast = true` 后广播
> 会被分发，但从不应答 —— 不回响应，也不回异常。


### nx_tp_sdu —— 传输层服务数据单元

一个仅结构体的头文件，描述诊断传输层与上层之间交换的对象：一条完整的报文，加上上层
为了正确作答而必须知道的、关于它如何传过来的少量事实。这些事实与是谁承载了报文无关，
因此在这里描述一次，每个传输层都填写同一个结构体。

- **一条报文一次分配** —— 载荷是柔性数组成员，因此头部与它所描述的字节共处一个内存池
  块中，在调用者提供的队列之间按指针传递。
- **寻址方式** —— `ta_type` 记录报文是发给单个接收者还是发给所有接收者（见
  `nx_tp_ta_type_t`），这决定了到底该不该作答。
- **连接标记** —— `link` 标明报文属于哪条连接，编号方式由应用自己决定。传输层只把
  配置中的值拷贝进每一条推出的报文，从不解释它，因此一个上层可以用一个队列服务多个
  传输层实例，仍能把它们区分开。
- **它在报告什么** —— `kind` 区分共用同一个输出队列的两类东西：收到的报文，以及一次
  发送的结果（见 `nx_tp_sdu_kind_t`）。
- **结果** —— `result` 说明一次操作是怎么结束的（见 `nx_tp_result_t`）：正常完成、某一类
  超时、序号或流控错误、对端要求等待的次数超出容忍范围，或报文装不下可用于接收它的
  空间。枚举值按它们被结算的优先级顺序排列，因此可以直接比较大小。
- **32 位长度** —— `len` 以字节计算载荷。报文能有多长取决于配置及其背后的内存，
  而不取决于这个字段的宽度。
- **只有类型、无依赖** —— 无需编译、无需链接。一个零初始化的实例表示一条成功的、
  物理寻址的接收指示，因此只需填写与此不同的字段。

```c
#include "nx_tp_sdu.h"

/* a request handed to a transport: it sends len bytes taken from data */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, sizeof(nx_tp_sdu_t) + 2u);
nx_tp_sdu_t  *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
s->len     = 2u;
s->data[0] = 0x22u;
s->data[1] = 0xF1u;

/* coming back the other way, one field decides how to read the rest */
const nx_tp_sdu_t *in = (const nx_tp_sdu_t *)nx_ref_msg_data(msg);
if (in->kind == NX_TP_SDU_INDICATION) {
    handle(in->link, in->ta_type, in->data, in->len);   /* a message arrived */
} else if (in->result != NX_TP_N_OK) {
    report(in->link, in->result);                       /* a send ended badly */
}
```

> **注意：** 方向不是一个字段。报文往哪个方向走，由它所在的队列决定 —— 传输层从一个
> 队列读取发送请求，把收到的报文和发完的结果推到另一个队列。因此区分"收到一条报文"
> 与"一次发送结束了"的是 `kind`，而 `result` 只在后者上才携带结果。


### nx_can_isotp —— ISO 15765-2（DoCAN / ISO-TP）分段传输

一个队列到队列的传输层，负责承载超出单帧容量的报文。它从接收队列中取走收到的帧，重组出
完整报文后交给上层；也接收上层的报文，将其分段回若干帧并按节奏送入 CAN 发送队列。模块
不拥有总线、不接触硬件 —— 进出的每个对象都是通过调用者提供的队列传递的 `nx_ref_msg`，
而搬运它们的只有 `nx_can_isotp_process()`。

- **一个实例兼具两种角色** —— 配置好 ID 组合并由上层作答，它就是被诊断的 ECU；调用
  `nx_can_isotp_send()` 发出请求，它就是发起诊断的上位机。同一个实例可同时担任两者。
- **完整的协议数据单元** —— 单帧（含 2016 版对超过 7 字节载荷的长度转义）、带 12 位
  长度的首帧（超过 4095 时使用 32 位转义）、带 4 位序号（0..15 回绕）的连续帧，以及
  携带 `CTS` / `WAIT` / `OVERFLOW`、块大小与 `STmin` 的流控帧。
- **成对配置的寻址** —— `phys_rx_id` 是本实例接收的 ID，`phys_tx_id` 是它发出报文所用
  的 ID，接收分段报文期间发出的流控帧也走这个 ID。可选的 `func_rx_id` 增加功能寻址
  （1:N）接收，且只接受单帧 —— 共用的请求 ID 无法承载面向单个接收者的流控。可选的
  `func_tx_id` 增加功能寻址发送 —— 向 `nx_can_isotp_send()` 传入
  `NX_TP_TA_FUNCTIONAL` 即可广播一条请求 —— 同样只允许单帧，原因相同，且默认关闭：
  一个网络上至多有一个功能发送者，所以只有上位机实例才会配置它。收到的报文会在
  `ta_type` 中标明它是发给单个接收者还是发给所有人，上层据此区分两者。这里填的是
  具体的帧 ID 而非位域，因此一个实例适用于 UDS 的 `0x18DA..xx` 组合、厂商自定义方案，
  或普通的 11 位 ID。
- **可配置的时间参数** —— `n_as_us` 限定发送时一帧等待交给链路的时长，`n_ar_us` 限定
  接收时的同一件事，`n_bs_us` 限定发送时等待对端流控的时长，`n_cr_us` 限定接收时
  等待对端下一个连续帧的时长，`n_wft_max` 限定对端最多可用多少个连续的 `WAIT` 帧把发送
  方拖住，超出则放弃本次发送。每个 `WAIT` 都会重新给出一个完整的 `n_bs_us` 窗口。字段
  填 0 表示采用文档给出的默认值。
- **链路一时繁忙只花等待的时间** —— 帧是通过投递到 `can_tx_queue` 交给链路的，因此队
  列没有空位就是这里所说的"链路收不下这一帧"。这样的帧会在后续的
  `nx_can_isotp_process()` 中被再次投递 —— 发送队列偶尔塞满，正常情况下几毫秒内就会
  排空；只有超过 `n_as_us` 或 `n_ar_us`，本次会话才结束并报出 `N_TIMEOUT_A`，指明是本
  地链路的问题，而不是对端不说话了。帧真正离开总线的时刻只有驱动才观测得到，所以这两个
  参数从帧被投递进队列的那一刻起算，比实际发送略早一点，因而只会稍晚判定超时，不会提前
  误判。拒收报文用的溢出流控帧是个例外：这次接收无论如何都已经结束，所以那一帧只投递一
  次，不做保留。
- **由你决定的流控** —— `rx_block_size` 与 `rx_stmin` 是本实例接收时对外通告的参数，
  用来要求对端每发 N 帧暂停一次、并保持最小帧间隔；每当一个块发完，模块会补发一个新的
  流控帧。
- **有上限的接收** —— `rx_max_len` 是本实例能接收的最大报文长度。首帧声明的长度超过它
  时，模块在从内存池中取走任何空间之前就回一个溢出流控帧，因此能收多长由配置决定，而不
  取决于池子当时剩多少。填 0 表示首帧到达时池子能分配多大就收多大，但仍不超过模块
  自身的上限。
- **高于任何配置的硬上限** —— `NX_CAN_ISOTP_MAX_MSG_LEN` 是本模块能处理的最长报文：
  线上的长度字段能表达的值，减去一个内存池块用在头部上的字节 —— 只要 `size_t` 有 32
  位宽，它就是 4294967255 字节。`nx_can_isotp_send()` 对更长的请求返回
  `NX_CAN_ISOTP_ERR_LENGTH`，首帧声明超过它则在任何分配发生之前就回一个溢出流控帧，
  因此从总线上读到的长度绝不会被重组进一块装不下它的内存。某个具体实例实际能收多长还
  要更小：取决于它的内存池和 `rx_max_len`。
- **结果上报** —— 置上 `confirm_tx` 后，每次发送结束、以及每次接收失败，都会推出一条
  SDU 说明原因：等流控或等连续帧超时、序号错乱、未定义的流控状态、对端 WAIT 次数超限，
  或报文长度超出可接收范围。`kind` 用于把这些与真正收到的报文区分开。
- **CAN 帧填充** —— 置上 `pad_frames` 后，每个发出的帧都会被补齐到 8 个数据字节，未用
  的尾部填入 `pad_byte`，以满足要求定长帧的诊断网络。不置上则每帧只携带它实际装下的
  字节。
- **有节奏、有边界的发送** —— `tx_frames_per_process` 限制每次 `process()` 调用最多
  发出多少帧，对端流控中的 `STmin` 则依据注入的时钟为连续帧留出间隔。
- **零拷贝重组** —— 首帧到达时从内存池中分配一块报文大小的空间并直接在其中重组，因此
  完成的报文无需拷贝即可交付上层。

```c
static uint8_t pool_mem[8192];
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
nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL);
nx_ref_msg_queue_init(&sdu_tx_q, sdu_tx_buf, 8);
nx_ref_msg_queue_init(&sdu_rx_q, sdu_rx_buf, 4);
nx_ref_msg_queue_init(&can_rx_q, can_rx_buf, 16);
nx_ref_msg_queue_init(&can_tx_q, can_tx_buf, 16);

const nx_can_isotp_cfg_t cfg = {
    .max_frame_len = NX_CAN_ISOTP_FRAME_8,  /* 8 = 经典 CAN，64 = CAN FD */
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
    .tx_frames_per_process = 1u,            /* 0 = 流控允许多少就发多少 */
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

> **注意：** 所有队列的命名都是站在本模块的角度：模块读 `sdu_rx_queue` 和
> `can_rx_queue`，写 `sdu_tx_queue` 和 `can_tx_queue`。也就是说，上层用来读取报文的
> 是模块的**发送**队列，上层用来投递发送请求的是模块的**接收**队列。
>
> `process()` 会取走 `can_rx_queue` 上的每一帧，ID 与两个接收 ID 都不匹配的帧被直接
> 释放。因此只有在两个实例的 ID 集合互不相交时，它们才可以共用一个 `can_rx_queue` ——
> 先运行的实例会拿走帧，无论是否匹配。若无法保证这一点，就给每个实例各配一个接收队列，
> 或在驱动层先做过滤。从 `sdu_tx_queue` 取出的每个对象都是消费者持有的一个引用，处理完
> 必须调用 `nx_ref_msg_release()`；漏掉它造成的是内存池泄漏而非释放后使用，因为只有引用
> 计数归零时块才会被归还。推入 `can_tx_queue` 的帧同理，由驱动在发送完成后释放。
