# 中间件模块
### nx_can_bus —— CAN / CAN FD 帧结构与辅助函数

一个仅头文件的模块，提供 CAN 帧的通用内存表示和小巧、无依赖的辅助函数。面向位于主机
与总线之间的工具/适配器，因此帧不仅携带数据，还带有方向和错误上下文。

- **经典 CAN 与 CAN FD** —— 一个 `nx_can_msg_t` 同时覆盖两者；载荷是柔性数组成员，
  因此调用者按实际长度（最多 64 字节）分配存储。帧属性（`is_ext`、`is_remote`、
  `is_fd`、`brs`、`esi`、`dlc`）被打包进一个位域，同时也暴露一个 `flags.raw` 字用于
  快速拷贝/比较。
- **主机/工具方向与通道** —— `dir`（见 `nx_can_dir_t`）区分 `TX`（主机请求工具发送）、
  `RX`（从总线收到）和 `TXR`（工具对先前 `TX` 的发送完成报告）。`ch` 是 4 位通道号，
  取值 0..`NX_CAN_MAX_CH`：与 `dir` 一样，只在工具/适配器上下文中有意义，用于标明帧
  属于哪个 CAN 接口。
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
- **线序** —— 16 位字段（地址、数量、寄存器值）在线缆上高位在前，按下面的代码那样重组。
  尾部 CRC 是小端（低字节在前）。对于变长帧，CRC 不是命名字段 —— `nx_modbus_rtu_req_var_crc`
  / `rsp_var_crc` 返回载荷之后指向它的指针。
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

> **注意：** 把字节缓冲强转为帧结构依赖于上面全 `uint8_t` 的布局；同样的布局也是没有
> packing pragma 的原因。多字节字段在线缆上高位在前 —— 不要
> 把它们当作原生 `uint16_t` 读取。


### nx_modbus_rtu_slave —— 事件驱动的 RTU 从站：帧 → 订阅分发

架设在 `nx_modbus_rtu`（帧结构 + CRC）之上的链路/分发层。它从线上拉取字节，切分并校验
完整的 RTU 帧，再把每个请求路由给订阅了它的业务模块 —— 自身不含任何业务逻辑。它不接触
硬件：所有 I/O 都以非阻塞回调的形式注入，并由主循环里单次 `process()` 调用驱动。

- **订阅分发** —— 每个业务模块在订阅表里声明一段 `(功能码 + 闭区间地址范围)`；匹配的
  请求会零拷贝地（以引用计数的 `nx_ref_msg`）扇出到该模块的队列。同一功能码可按范围拆
  给多个模块，同一请求也可同时送达多个订阅者。订阅者队列已满时该份副本被丢弃；若**所有**
  匹配的订阅者都收不下，请求会被回以 `0x06`（从站设备忙），让主站知道该重试，而不是面对
  一片沉默。部分投递不回异常 —— 请求毕竟已经在某处生效了。
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
  `get_us` 作为系统级时间源不带任何 ctx。`write` 返回 false 意味着字节根本没被接收，也就
  没有什么需要等待：该帧被丢弃，方向引脚在同一轮内落回，把总线段留给其它节点。
- **释放实例** —— `nx_modbus_rtu_slave_deinit()` 交还半途中断的发送帧所占的池块，拉低方向
  引脚，并把状态机停回空闲。对已经在跑的实例重新 init 之前、以及让实例退出服务时都应调用
  它；响应队列不予处理，里面的消息归推送者所有。
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


### nx_modbus_rtu_master —— 事件驱动的 RTU 主站：队列 → 线路 → 订阅分发

建立在 `nx_modbus_rtu`（帧结构 + CRC）之上的链路/分发层。它把业务模块压入共享队列的请求帧
发上线路，把回来的响应切片校验，再按响应来自哪个从站分发给拥有该设备的业务模块——自身不含
任何业务逻辑。它不碰硬件：所有 I/O 以非阻塞回调注入，由主循环里的一次 `process()` 驱动。

- **按从站地址分发** —— 业务模块拥有它对话的设备，所以订阅表的每一条认领一个 `slave_addr`，
  该设备的响应以零拷贝的引用计数消息（`nx_ref_msg`）投递到它自己的队列。可选的 `func` 过滤器
  用于两个模块按功能码拆分同一设备；`func = 0` 收下该地址的全部响应。一个响应可以同时到达
  多个订阅者。无人认领的响应被丢弃——主站不作答，因此也没有什么可以拿来替代。
- **超时归业务模块** —— 本模块只负责发送与分发，不记录哪些请求还在途中。需要知道"答复没来"的
  模块自己记下发送时刻，并自行决定何时重试或放弃。响应队列为空只意味着答复还没到，不代表出错。
- **请求构造器** —— 每个功能码一个（`nx_modbus_rtu_master_read_holding_regs`、
  `..._write_multiple_regs` 等），负责建好帧、打上 CRC 并入队。它们只要池和请求队列，业务模块
  无需持有主站句柄。协议不允许的一律当场拒绝——数量越界、字节数与数量不自洽、广播读——而不是
  花一个往返去换回同样的结论。
- **按长度切片** —— 每个响应的长度都由它自身的字节决定（异常 5 字节，写确认 8 字节，读响应
  `3 + byte_count + 2`），所以 RX 不需要字符间隔（T3.5）定时器——在到达时序不可信的繁忙总线上
  更稳。未收完的帧跨调用保留，收齐后才分发。遇到不支持的功能码或坏 CRC 时丢一个字节重同步。
  TX 侧每帧之后插入由 `baud_rate` 导出的 3.5 字符间隔。
- **一次调用推进一步** —— 每次 `process()` 只把发送路径推进一个状态，因此不会有哪一次调用把
  多帧连续压上线路。提供 `is_busy` 时，在途的帧同时挡住下一次 `write()` 和方向引脚的释放，
  这正是共享 RS-485 段不会被两帧同时驱动的原因。
- **响应解析** —— 订阅者取到的是一整个 ADU，CRC 已经校验过。
  `nx_modbus_rtu_master_rsp_is_exception()` 区分拒绝与答复并给出异常码；
  `nx_modbus_rtu_master_rsp_data()` 定位读响应的载荷及其长度，返回指向消息内部的指针而非拷贝。
- **释放实例** —— `nx_modbus_rtu_master_deinit()` 交还半途在发的帧所占的池块，拉低方向引脚，
  丢弃未收完的响应。请求队列不动：里面的消息属于压入它们的人。

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
    .request_queue = &request_queue, /* 模块压进这里的会被发出去 */
    .read          = uart_read,      /* 注入的非阻塞 I/O */
    .write         = uart_write,
    .get_us        = board_micros,   /* is_busy 为 NULL 表示 write 是阻塞的 */
};
nx_modbus_rtu_master_init(&master, &cfg);

/* 某业务模块请求 10 个寄存器；答复会落到它自己的队列上 */
nx_modbus_rtu_master_read_holding_regs(&pool, &request_queue, 0x11u, 0x0000u, 10u);

for (;;) {
    nx_modbus_rtu_master_process(&master);        /* TX 泵 + RX 分发 */
    pump_business(&q_pump, &request_queue, &pool);   /* 排空收件箱，再次发问 */
}
```

> **注意：** RTU 帧不带事务 ID，响应只能靠从站地址加功能码来对应请求。对同一从站发出两笔功能码
> 相同的未完成请求时，回来的两个响应在帧层面无法区分谁对应谁——需要严格配对的模块应对每个从站
> 一次只留一笔在途。对已经放弃的请求迟到的响应同理：它看起来和当前请求的答复完全一样。


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
- **一个实例，一条总线** —— `ch` 是本实例所服务的 CAN 通道。它会被打进每一个发出的帧，
  横跨多条总线的驱动因此能从帧上读出该往哪条总线发；收帧时也要匹配它，路由错了的帧会被
  跳过。一个模块只管一条总线的驱动两端都留 0，匹配就恒成立。
- **可配置的帧格式** —— `ext_id`、`fd_frames` 和 `brs` 决定每个发出的帧带什么：29 位
  标识符、CAN FD 帧，以及让数据段跑在更高速率上的位速率切换。`ext_id` 同时决定哪些收到
  的帧属于本实例的流量 —— 数值相同的 11 位标识符和 29 位标识符指的是两个不同的地址。
- **帧的载荷尺寸** —— `max_frame_len` 是每帧携带的载荷字节数，必须是数据长度码能精确
  表达的八个尺寸之一：8、12、16、20、24、32、48 或 64。大于 8 需要置上 `fd_frames`，
  `brs` 同样需要它。更大的尺寸让单帧装得更多、每个连续帧也装得更多，因此在 8 字节下需要
  分段的报文可能一帧就发完。
- **CAN 帧填充** —— 置上 `pad_frames` 后，每个发出的帧都会被补齐到 8 个数据字节，未用
  的尾部填入 `pad_byte`，以满足要求定长帧的诊断网络。不置上则每帧只携带它实际装下的
  字节。超过 8 字节时无论是否置上都会填充，因为帧必须落在一个能表达的尺寸上。
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
> `process()` 会取走 `can_rx_queue` 上的每一帧，`ch` 或标识符位宽与配置不一致的、或者
> ID 与两个接收 ID 都不匹配的帧被直接释放。因此驱动若在收帧时不填 `ch` 或 `is_ext`，
> 配置了非默认值的实例会把全部流量都丢掉 —— 要么在驱动的接收路径里填好这两个标志，要么
> 让 `ch` 和 `ext_id` 保持驱动实际会上报的值。匹配 `ch` 只是把送到正确实例的帧分拣出来，
> 并不能让一个 `can_rx_queue` 被共用：取出的每一帧无论是否匹配都会被释放，先运行的实例
> 会拿走它，其余实例根本看不到。因此仍要给每个实例各配一个接收队列，或在驱动层先做分流。从 `sdu_tx_queue` 取出的每个对象都是消费者持有的一个引用，处理完
> 必须调用 `nx_ref_msg_release()`；漏掉它造成的是内存池泄漏而非释放后使用，因为只有引用
> 计数归零时块才会被归还。推入 `can_tx_queue` 的帧同理，由驱动在发送完成后释放。

### nx_uds —— ISO 14229 词汇表

诊断模块共享的枚举、掩码与结构体：服务标识符与正响应标识符、负响应码、会话类型及其位掩码、
服务处理器被调用的相位，以及处理器本身的契约。纯头文件，无状态，无需初始化。

- **服务标识符及其正响应** —— `nx_uds_sid_t` 列出各服务；`NX_UDS_SID_TO_POS_RSP()` 与
  `NX_UDS_POS_RSP_TO_SID()` 在请求标识符与应答它的正响应标识符之间换算；
  `NX_UDS_NEG_RSP_SID` 与 `NX_UDS_NEG_RSP_LEN` 描述三字节的负响应。
- **响应码为枚举** —— `nx_uds_nrc_t` 覆盖服务器会发出的各码位，其中 `NX_UDS_NRC_NONE`
  表示"没有码"，这样零值字段表示无事可报，而不是码 0x00。
- **会话用位掩码表示** —— 会话的位就是它自身的值，因此服务行用单个 `uint32_t` 命名它
  可用的各会话。`NX_UDS_SESSION_BIT()` 构造一位，`NX_UDS_SESSION_MASK_ALL` 与
  `NX_UDS_SESSION_MASK_NON_DEFAULT` 覆盖常见集合，`NX_UDS_SESSION_MAX` 界定掩码所能触及的范围。
- **抑制位** —— `NX_UDS_SUPPRESS_POS_RSP_BIT`、`NX_UDS_SUPPRESSES_POS_RSP()` 与
  `NX_UDS_SUB_FUNCTION()` 读取并剥离子功能字节的第 7 位，该位要求正响应不被发送。
- **处理器契约** —— `nx_uds_ctx_t` 是处理器眼中的一次事务：请求及其长度、已剥离抑制位的
  子功能、到达时所处会话与解锁等级、一个已写好响应标识符的响应缓冲区，以及一处跨同一事务
  各相位保存临时内容的位置。`nx_uds_phase_t` 说明处理器为何被调用，`nx_uds_disposition_t`
  说明它作了什么决定。
- **服务表行** —— `nx_uds_service_t` 以数据形式描述一个服务：其标识符、处理器、所需的会话
  与安全等级、可用的子功能（以及各子功能可选的可达会话），还有其请求落在一个怎样的长度窗口内。

```c
#include "nx_uds.h"

/* 一个服务，用数据描述：读一个数据标识符，在所有会话可用，无需解锁，无子功能，
 * 请求长度固定。 */
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

> **注意：** 会话的位就是它自身的值，因此掩码触及 `NX_UDS_SESSION_MAX` 为止，无法更远。
> ISO 14229 定义的各会话都远在其内，但厂商与供应商区间一直延伸到 0x7E，无法用掩码命名 ——
> `NX_UDS_SESSION_BIT()` 对它们不产生位，而不是按类型宽度移位，因此列出其中之一的某行
> 是没有任何会话能匹配的行。掩码为 0 表示不命名任何会话，并在初始化时被拒绝，因而零值行
> 不会悄然失效。

### nx_uds_server —— ISO 14229 诊断服务器（ECU 侧）

ISO 14229 对话的服务端：接收一个请求 A_PDU，找到实现它的服务，产出响应 A_PDU。本模块
不知道请求是经由什么到达的 —— 请求通过 `nx_uds_server_indicate()` 以纯字节外加它的寻址
方式进入，响应则经由一个由应用接线的回调送出。每个实例服务一条对话。

服务器拥有的正是各服务所共享的部分。服务集合是应用自己的，以 `nx_uds_service_t` 行组成的
表持有；加一个服务就是写一个处理器并加一行，本模块无需改动。在这一分派之上，服务器还持有
会话与使其跌落的 S3 定时器、ISO 14229-2 规定的响应时序、把慢事务撑长的等待通知，以及不
属于任何服务的负响应。

- **一次只处理一个事务** —— 一个请求被接收、跑到它的响应，然后才接纳下一个。
  `nx_uds_server_indicate()` 以 `ERR_BUSY` 拒绝而非开启第二个事务，需要多轮循环的处理器
  会把事务一直攥在手里直到完成。
- **响应时序由配置驱动** —— 服务器把自身约束在 P2、P2* 与 P4（皆以微秒计），在超过 P2
  时发出等待通知，并在 P4 或 `max_pending` 二者先到者处放弃该事务（以配置的 `p4_nrc` 应答）。
- **会话是一种资源** —— 服务器跟踪当前会话与已解锁的安全等级，非默认会话在 `s3_us`
  的静默后跌落，并在它接受的每个请求上重启该定时器。
- **只用调用方提供的缓冲区** —— 请求在其事务存续期间被拷入 `req_buf`，响应在 `out_buf`
  中组装；需要跨多轮循环的处理器读这一个缓冲区、写另一个缓冲区。不做任何分配。
- **匹配不到服务的请求也有答复** —— 它开启一个产生负响应的事务，因此拒绝以与正响应
  相同的方式送出。

服务器对外暴露服务需要用来与它对话的若干项：`_session` 与 `_sec_level` 报告请求到达时
的状态，`_set_session` 与 `_set_sec_level` 改变它，`_touch` 重启静默定时器，`_timing`、
`_now` 与 `_apdu_limits` 公布服务器自持的约定，`_is_busy` 则询问是否有事务在运行。

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

> **注意：** 事务运行期间到达的请求以 `ERR_BUSY` 拒绝，正在运行的事务被完全置之不理。
> 另一种做法 —— 取消当前的以接纳新来者 —— 会把进行中的工作丢给任何请求，包括一条
> 从未专门发给本服务器的广播 TesterPresent。被拒绝的请求如何处理由调用方决定：用 0x21
> 回复它会让客户端重发，而对一条功能性寻址的请求直接丢弃也是正确的。

### nx_uds_svc_std —— 始终需要的服务处理器

无论还实现了什么，诊断服务器都应回答的三个服务：0x10 诊断会话控制、0x11 ECU 复位与
0x3E 存在测试。每个都是占据普通服务表行的普通处理器，与应用自己的服务一同入表、以同样
方式被访问。各自通过行的 `user` 指针传入自己的配置结构体，这里不保留任何自身状态。

- **0x10 会话控制** —— 回显会话自身，并给出响应窗口 P2 与 P2*，在该答复抵达客户端后才
  进入该会话。窗口取自服务器而非在此配置，因此宣布的就是被执行的。进入任何会话都会
  重新锁定安全，无论进入的是哪个会话。
- **0x11 ECU 复位** —— 回显复位类型，0x04 另外附带配置的掉电时间，在该答复抵达客户端后
  才执行复位。在答复发出前就复位的服务器，在客户端看来就像是自行重启了。
- **0x3E 存在测试** —— 回显子功能，其余什么也不做；服务器对接受的每个请求都会重启静默
  定时器，而这个服务正是无话可说、只想保持会话的客户端所发送的东西。

每个处理器都公布其行必须声明的内容，因为声明了别的什么并不会被纠正：一个服务不期望的
长度边界，或列入了产品做不到的子功能，产生的是一台答错的服务器，而非拒绝启动的服务器。

```c
#include "nx_uds.h"
#include "nx_uds_server.h"
#include "nx_uds_svc_std.h"

static nx_uds_server_t srv;

static bool allow_session(void *user, uint8_t from, uint8_t to, uint8_t *nrc)
{
    (void)user; (void)from; (void)to; (void)nrc;
    return !driving_now();          /* 行驶中拒绝进入编程会话 */
}

static nx_uds_svc_std_session_cfg_t session_cfg = {
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

static nx_uds_svc_std_reset_cfg_t reset_cfg = {
    .do_fn           = do_reset,
    .power_down_time = 0xFEu,            /* 没有可用的掉电时间 */
};

static const uint8_t sessions[] = {
    NX_UDS_SESSION_DEFAULT, NX_UDS_SESSION_PROGRAMMING, NX_UDS_SESSION_EXTENDED,
};
static const uint8_t resets[] = {
    NX_UDS_RESET_HARD, NX_UDS_RESET_KEY_OFF_ON, NX_UDS_RESET_SOFT,
    NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN,
};
static const uint8_t tester_present_sub = NX_UDS_SVC_STD_TESTER_PRESENT_SUB;

static const nx_uds_service_t services[] = {
    {
        .sid          = NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .handler      = nx_uds_svc_std_session_control,
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
        .handler      = nx_uds_svc_std_ecu_reset,
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
        .handler      = nx_uds_svc_std_tester_present,
        .flags        = NX_UDS_SVC_HAS_SUB_FUNCTION,
        .session_mask = NX_UDS_SESSION_MASK_ALL,
        .subs         = &tester_present_sub,
        .subs_count   = 1u,
        .min_len      = 2u,
        .max_len      = 2u
    },
};
```

> **注意：** 0x10 与 0x11 在 CONFIRM/SILENCE 相位行动，而非在它们生成答复时，因此变更或
> 复位只在客户端已被告知请求获准后发生。在那一刻对请求采取行动的代码询问缓冲区里的响应
> 是否正是该服务的正响应，从而拒绝不会被误当作它取代的接受。正响应被抑制的请求仍然会
> 被采取行动（在答复本应被发出的时刻）；答复未到达链路的请求则不会。

### nx_uds_svc_sec —— 0x27 种子/密钥交换

解锁某个安全等级的种子/密钥交换。每个等级是一对子功能 —— 奇数的要种子，其后偶数的送
密钥 —— 而"已解锁等级"正是服务行的 `sec_level` 所命名的。算法不在这里：应用通过两个回调
产生种子并判定密钥，本模块从不接触秘密，也不发明随机数。它拥有的是序列与防猜测机制。

- **配对即等级** —— 等级 *n* 是子功能 `NX_UDS_SVC_SEC_SEED_SUB(n)` 与 `NX_UDS_SVC_SEC_KEY_SUB(n)`，
  故等级 1 是 0x01/0x02，等级 2 是 0x03/0x04，直到 `NX_UDS_SVC_SEC_MAX_LEVEL`。等级列表中
  缺席的等级并不存在。
- **每个等级的字节数固定** —— 每个等级声明其种子与密钥各有多长。无论种子是现算的还是该
  等级已解锁，种子应答都恰好这么长；密钥必须恰好是声明的长度。
- **种子一经判定即被消耗** —— 同一把密钥不能提交两次，错误的密钥会消耗种子，于是第二次
  尝试从索要新种子开始。
- **等待期** —— 错误密钥计为一次尝试；当计数达到 `max_attempts`，模块开始一段 `delay_us`
  的时间，其间每个 0x27 请求都以 0x37 拒绝、且不咨询任何回调。计数与等待期既胜过会话
  变更也胜过重新上锁，而 `nx_uds_svc_sec_get_lockout()` / `nx_uds_svc_sec_set_lockout()` 这对函数
  把它们存放到某个能挨过掉电的地方。

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

> **注意：** 错误的密钥只有当它所在请求的形式正确时才计为一次尝试。没有未决种子就送来的
> 密钥以 0x24（请求顺序错误）应答且不计次；客户端要求不被告知的种子不会被发出 —— 也就
> 没有可计次的东西。计数跨所有等级合计，因此逐级尝试的客户端仍是一个客户端，而达到上限
> 正是开启等待期的原因。

### nx_uds_svc_transfer —— 搬移一块内存

内存传输服务：0x34 请求下载、0x35 请求上传、0x36 传输数据与 0x37 请求传输退出。四个
处理器占据四行普通的服务表行，外加一次传输所需的状态 —— 以哪个方向运行、覆盖哪块区域、
进展到何处、下一块是哪个。内存不在这里：应用通过两个回调读写它，本模块从不触及任何地址。

- **开启、搬运、关闭** —— 传输由 0x34（客户端写内存）或 0x35（客户端读它）开启，
  由任意多次 0x36 交换搬运，由 0x37 关闭。一次只运行一个，而一次开启只有当
  声明的整个区域都已搬完才算完成。
- **块长被宣布** —— 开启应答给出一个块的长度，以整条消息而非仅其载荷计算，并取自
  服务器所能承载的。`nx_uds_svc_transfer_payload_room()` 减去计数器与标识符所占的两个字节开销。
- **同一块到达两次不会被搬运两次** —— 与上一已提交块相同的计数器会再次应答，从同一处、
  以同样长度，因此丢失的应答可恢复，而不是一次翻倍的写入。既非下一块也非上一块的
  计数器会被拒绝，传输保持开启以便客户端重试。
- **上传的最后一块较短** —— 服务器读取声名区域所余下的部分；而会写越过客户端自己声明
  末端的下载会被拒绝。
- **完成与否由应用说了算** —— 0x37 在组装其答复之前调用关闭回调，这正是在那里校验已写
  映像、或将某分区标记为有效的时机。它不动会话、已解锁等级与静默定时器：客户端通常
  在一个会话里跑若干次传输。

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

> **注意：** 0x36 标识符之后的那个字节是块序号计数器，不是子功能。该行不得声明
> `NX_UDS_SVC_HAS_SUB_FUNCTION`，因为把该字节顶位当作请求静默读取，会令每次传输的一半
> 都得不到任何应答。该行的 `max_len` 为 0 —— 真正的上限是已被宣布的长度，而其上的块按
> 越界而不是按长度错误来拒绝。

### nx_uds_tp_bind —— 把服务器接到某个传输层

介于诊断服务器与某个讲 `nx_tp_sdu_t` 的传输层之间的几十行。传输层把收到的、以及发完的
内容作为引用计数消息推入一条队列，再从另一条队列读取它应发送的内容；本模块正是把那些
消息在服务器内外搬动的东西。它与传输层无关 —— 它只见两条队列与一个池，因此同样的代码
把服务器接到任何一个填充 `nx_tp_sdu_t` 的传输层。每条通路一个实例。

- **手写时容易出错的那些机制** —— 收到的消息在服务器复制完毕后立即被释放；传输层暂时
  接不下的响应留给服务器再次呈递而非丢弃；回答某请求期间到达的请求会保持会话存活而非
  任其超时；发布的每个响应都寻址到欠它的那一个客户端，而不是发到整个链路上。
- **每趟一步** —— `nx_uds_tp_bind_process()` 每次至多从入站队列取走一条消息，因为服务器
  一次应答一个请求，第二个只会被拒绝而非排队。这里并不泵动服务器；驱动什么、以何顺序，
  由应用来安排。
- **属于自己的池** —— 池按通路而非共享，因此一条通路上的洪泛无法饿死另一条。
- **丢了什么被计数** —— 统计信息上报每条被丢弃而未应答的消息，于是一条计数器攀升的通路
  是在悄然失败。

该绑定把自己安装为服务器的输出通路，因而服务器自己的 `out_fn` 与 `out_user` 会被覆盖。
先初始化服务器，再绑定它。

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

    cfg.srv         = &srv;
    cfg.sdu_in      = &sdu_in_q;   /* 传输层推入的已收到内容 */
    cfg.sdu_out     = &sdu_out_q;  /* 传输层取出并发送的内容 */
    cfg.pool        = &pool;
    cfg.link        = 1u;
    cfg.max_sdu_len = 4096u;

    nx_uds_tp_bind_init(&bind, &cfg);
}
```

> **注意：** 无论请求如何抵达，响应总是物理寻址的。传输层在它被要求发送的消息上读取
> `ta_type` 来选择 CAN 标识符，因此把请求的功能性寻址一路传下去，会让响应用到广播抵达的
> 那个地址上 —— 在 ECU 上也就是 0，无人监听的地址。绑定在每个出站响应里写入
> `NX_TP_TA_PHYSICAL`，正是为此。

### nx_uds_client —— ISO 14229 诊断客户端（测试工具侧）

ISO 14229 对话的另一半：接收一个请求 A_PDU，报告结果 —— 返回的响应 A_PDU，或为什么没有返回。
本模块不知道请求是经由什么到达的 —— 请求经一个由应用接线的回调送出，响应通过
`nx_uds_client_indicate()` 以纯字节外加其寻址方式进入。每个实例一次只跑一个事务，因此客户端
是一个提问并等待的测试工具，而非源源不断发流的对端。

一个事务是一个问题以及对其答案的等待。客户端把请求提供给发送通道，等待 P2 等待响应，若
服务器说答案还在路上 —— 携带 0x78 responsePending 的负响应 —— 则等待 P2* 并继续，最多到
有界的扩展次数。等待窗口由服务器设定：0x10 正响应公布 P2 与 P2*，客户端即为本次对话采纳它们，
除非配置了 `fixed_timing`，此时它始终使用自己的值。

- **一次只处理一个事务** —— 一个请求被装好、跑到它的结果，然后才接纳下一个。有事务在飞时，
  `nx_uds_client_request()` 以 `ERR_BUSY` 拒绝。
- **报告的是结果而不仅是字节** —— 结果回调指明事务如何结束：正响应、拒绝、协议错误、超时、
  取消，或是请求本就要求无正响应时的静默（唯一不算失败的静默）。
- **发送通道给出答复** —— 载体对请求做了什么，经 `nx_uds_client_confirm()` 回报，因此若请求
  从未上到链路上，客户端立刻听到，而不是等响应窗口耗尽。对一时接不下请求的载体，客户端在
  下一趟重新提供同一个请求而非丢弃 —— 但只到 `send_timeout_us` 为止。
- **时序由服务器设定** —— 除非设置 `fixed_timing`，等待窗口即公布的取值，因此客户端在服务器
  说响应已迟到时超时，而非在应用猜测会迟到时超时。
- **只用调用方提供的缓冲区** —— 在飞的请求存放在 `req_buf`，抵达的响应存放在 `rsp_buf`，
  皆由调用方持有。不做任何分配。

客户端对外暴露应用驱动会话所需的状态：`_is_busy` 询问是否有对话在运行，`_session` 报告
当前会话，`_timing` 报告正在使用的等待窗口，`_set_send` 则让某个绑定在客户端初始化之后把
自己装为发送通道。

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

static void ask_session(void)
{
    /* 0x10 0x00，物理寻址：这个 ECU 处于哪个会话？ */
    nx_uds_client_request(&clt, NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL, 0x00u, NULL, 0u,
                          NX_TP_TA_PHYSICAL);
    while (nx_uds_client_is_busy(&clt)) {
        nx_uds_client_process(&clt);   /* 每次主循环迭代 */
    }
}
```

> **注意：** `nx_uds_client_request()` 并不会发送任何东西。它把请求装好，第一次
> `nx_uds_client_process()` 调用才把它提供给发送通道，因此调用 `request()` 后立刻读
> `rsp_buf`，看到的永远是一个零字节。发送通道接下请求也不意味着事务在发货 —— 它只在结果
> 触发时才算结束，对大多数信号而言那发生在结果回调里；而要求静默的请求，其终结时永远不会
> 在缓冲区里放一个响应。
