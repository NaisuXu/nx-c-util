# UDS 设计笔记

**状态：** 设计阶段；尚未实现。

**日期：** 2026-08-20

## 背景

本文档记录 `nx-c-util` 中 UDS（ISO 14229）模块族的设计方向。该设计服务两条传输路径（通过 `nx_can_isotp` 的 CAN、通过未来的 `nx_modbus_rtu_tp` 的 Modbus RTU），支持客户端（测试器）和服务器（ECU）两种角色，并且必须将整个 ISO 14229-1 作为可扩展框架容纳进来——不仅仅是刷写。

## 核心原则

**UDS 是纯上层协议，不得知道其下链路的任何信息。**

这条原则经过对抗性审查严格应用后，完全消除了最初提议的 `nx_tp_port_t` 接缝结构体。UDS 真正需要的东西已经被以下方面满足：

1. `nx_tp_sdu_t` 上已有的字段（payload、`ta_type`、`link`、`result`）——这些是真实的 ISO 14229-2 A_PDU 服务接口参数。
2. UDS 服务器配置中两个应用选定的容量数。
3. 传输机制（pool、队列、ref-msg 协议）限定在 UDS 层下方的每条路径绑定 shim 中。

## 架构

### 分层

```
应用层
    |
    | indicate(bytes, len, ta_type, link)
    | output_callback(user, link, rsp, len, ta_type)
    v
nx_uds_server / nx_uds_client
    |
    | （无直接传输耦合）
    |
    v
绑定 shim（每条路径，各约 30 行）
  - nx_uds_port_isotp.c
  - nx_uds_port_modbus.c
    |
    | 通过 nx_queue_t（装 nx_ref_msg_t*）的 nx_tp_sdu_t
    v
nx_can_isotp / nx_modbus_rtu_tp
```

绑定 shim 拥有：
- 从传输层 TX 队列拉取 `nx_tp_sdu_t`
- 调用 `nx_uds_server_indicate()` 或 `nx_uds_client_confirm()`
- 从 pool 分配 + 将响应发布到传输层 RX 队列
- 所有 ref-msg 和队列机制

UDS 层只看到：
- 携带请求字节 + 寻址 + 链路标识的 `indicate()` 调用
- 用于发出响应的输出回调
- 携带传输结果的 `confirm()` 调用

### 模块

| 模块 | 类型 | 用途 |
|------|------|------|
| `nx_tp_sdu.h` | 纯头文件（结构体） | 传输 SDU：payload + `ta_type` + `link` + `kind` + `result` + `len` |
| `nx_uds.h` | 纯头文件 | 词汇表：SID、NRC、会话掩码、`nx_uds_xfer_t` 状态 |
| `nx_uds_server.{h,c}` | 核心 | 表驱动服务器：服务分发、P2/P2*/P4 定时器、0x78 泵、NRC 归属 |
| `nx_uds_transfer.{h,c}` | 服务族 | 0x34/0x35/0x36/0x37 handler（故意不叫 "download"——0x38 共享 0x36/0x37） |
| `nx_modbus_rtu_tp.{h,c}` | 传输层 | Modbus 承载层：ADU ↔ `nx_tp_sdu_t`，轮询主从，在 collect 时 confirm |
| `nx_uds_client.{h,c}` | 核心 | 事务器：单事务引擎、从 confirm 算 P2、0x78 吸收、无表 |
| `nx_uds_port_isotp.c` | Shim（示例） | 连接 `nx_can_isotp` ↔ `nx_uds_server` |
| `nx_uds_port_modbus.c` | Shim（示例） | 连接 `nx_modbus_rtu_tp` ↔ `nx_uds_server` |

客户端和服务器共享纯头文件词汇表，但零编译产物。

### 服务扩展点

服务集是**数据，不是代码**：调用者持有的 `const nx_uds_service_t[]` 表，没有特权核心。库自己的服务（0x10 DiagnosticSessionControl、0x11 ECUReset、0x3E TesterPresent、0x27 SecurityAccess）只是占据行的导出函数指针。

```c
typedef struct {
    uint8_t  sid;               /**< 服务标识符 */
    uint8_t  flags;             /**< NX_UDS_SVC_* 标志 */
    uint8_t  sec_level;         /**< 最低解锁安全等级（0 = 无限制） */
    const uint8_t *subs;        /**< 允许的子功能（NULL = 任意） */
    uint8_t  subs_count;        /**< subs 数组长度 */
    uint8_t  session_mask;      /**< 此服务可用的会话位掩码 */
    uint16_t min_len;           /**< 最小请求长度（含 SID） */
    uint16_t max_len;           /**< 最大请求长度（0 = 无限制） */
    uint32_t p4_us;             /**< 每服务 P4 上限（0 = 使用全局 cfg.p4_us） */
    nx_uds_handler_fn handler;  /**< 服务 handler */
    void    *user;              /**< 传给 handler 的不透明用户指针 */
} nx_uds_service_t;
```

所有服务一个 handler 签名：

```c
typedef nx_uds_disposition_t (*nx_uds_handler_fn)(nx_uds_ctx_t *ctx, void *user);
```

阶段：`REQUEST`（首次调用）、`RESUME`（handler 上次返回了 `PENDING`）、`RESPONSE`（响应即将发送）、`CONFIRM`（传输完成）、`LINK_ERROR`（传输层报告失败）、`SILENCE`（功能寻址请求按 ISO 14229-1 抑制规则不会应答）、`ABORT`（事务取消：S3 超期、PENDING 中途来了新请求）。

处置：`DONE`、`PENDING`（层在下次 `process()` 重入，在自己的定时器上发 NRC 0x78）、`DEFER`（可选；见待定问题 2）、`NEGATIVE`（handler 向 `ctx->out` 写了 NRC）、`NO_RESPONSE`。

**添加一个服务**（例如 0x19 ReadDTCInformation）：
1. 写 handler 函数：`nx_uds_disposition_t nx_uds_handle_read_dtc(nx_uds_ctx_t *ctx, void *user)`。
2. 如果要复用就导出到头文件：`extern nx_uds_handler_fn nx_uds_svc_read_dtc;`。
3. 在应用的服务表中加一行，`sid=0x19`，适当的 flags/masks/lengths，`handler=nx_uds_handle_read_dtc`。
4. `src/middleware/` 下没有文件被打开。

### 容量数（从 port 结构体挪走）

UDS 需要知道两个上层量来实现三个 ISO 14229-1 行为：

1. **0x34 RequestDownload / 0x35 RequestUpload** 在正响应中发布 `maxNumberOfBlockLength`——服务器**计算**的一个数，客户端在尺寸 0x36 块时遵守。没有"构建它然后看失败与否"；服务器必须事先宣布。
2. **NRC 0x14 responseTooLong** 是当响应超过可传送内容时的定义答案。在小链路上（你的 Modbus collect 窗口），"让链路失败"会产生超时，而不是正确可观察的 0x14。
3. **0x22 ReadDataByIdentifier 带多个 DID / 0x19 枚举**：服务器必须在容量处停止枚举或发出 0x14，在 SDU 完成前就需要这个数。

这些是**应用选定的**容量，不是裸链路 MTU。服务器用于 0x34 的接收容量是 `min(buffer, flash 块约束, 链路 MTU)`；如果 UDS 只读链路 MTU，它会发布一个比自己 buffer 还大的 `maxNumberOfBlockLength`。

```c
typedef struct {
    /* ... 其他 cfg 字段 ... */

    uint32_t max_req_apdu;   /**< 此服务器接受的最大请求 A_PDU，字节数。
                                  由应用从 min(自己的接收 buffer、flash 块约束、
                                  传输 rx 限制) 推导。0x34 RequestDownload 
                                  handler 从此数减去 SID 开销计算 
                                  maxNumberOfBlockLength。 */
    uint32_t max_resp_apdu;  /**< 此服务器产生的最大响应 A_PDU，字节数。
                                  从 min(自己的发送 buffer、传输 tx 限制) 推导。
                                  钳制 ctx->out_cap；溢出时触发 NRC 0x14 
                                  responseTooLong；喂给 0x35 RequestUpload 
                                  maxNumberOfBlockLength。 */

    uint32_t p2_us;          /**< P2_server：从请求到首个响应或 pending 通知
                                  的时间，微秒 */
    uint32_t p2_star_us;     /**< P2*_server：从一个 0x78 到下一个或到最终
                                  响应的时间，微秒 */
    uint32_t p4_us;          /**< P4_server：从 indication 到最终响应（含每个
                                  0x78 扩展）的整个事务硬上限，微秒 */

    /* ... 服务表、get_us、输出回调等 ... */
} nx_uds_server_cfg_t;
```

**两个数，不是一个：** 你的 Modbus 路径是不对称的——deposit 窗口（服务器在 0x36 上接收的）和 collect 窗口（它在 0x35 上能交回的）不同。`max_req_apdu` 和 `max_resp_apdu` 喂给不同的 UDS 机制，必须独立。

**接线纪律：** 应用从 `min(buffer, segment, link)` 计算一个常量，并将其接入 `nx_can_isotp_cfg.rx_max_len`（用于传输层自己的溢出检查）和 `nx_uds_server_cfg.max_req_apdu`。如果它们漂移，服务器发布一个传输层随后会拒绝的 `maxNumberOfBlockLength`——手动纪律替换 port 结构体本会给出的编译时保证。为正确分层接受的代价。

### 传输契约（文档化要求，不是结构体）

服务 UDS 的承载层：

- **对从 `tx_queue` 接受的每条消息发布一个 `NX_TP_SDU_CONFIRM`**，携带 `result`（`NX_TP_N_OK` 或一个定义的失败），永不省略。不能观测到传输完成的承载层在接受时 confirm；confirmation 不是可选的。
- **对接收的每条完整消息发布 `NX_TP_SDU_INDICATION`**，`ta_type` 命名它是如何被寻址的（物理 vs 功能）。
- **对超出其容量的消息拒绝**，在 confirm 上用 `result != NX_TP_N_OK`（如 `NX_TP_N_BUFFER_OVFLW`），永不静默截断。
- **在功能传输上：** 要么兑现它（在 1:n ID 上单帧），要么在 publish/confirm 时拒绝；永不在错误 ID 上传输它。

当 `confirm_tx = true` 且 `func_tx_id` 设置（或如果不需要功能 TX 就留零）时，`nx_can_isotp` 满足这点。Modbus 承载层通过在主站读取最后一块时 confirm 来满足。

### 消除 Port 结构体的行为后果

1. **0x78 泵在每个服务器实例上无条件运行**。当 `P2` 到期而答案未就绪时，层发出一条 pending 通知，向链路提供直到被接受（通过"每事务最多一条 pending 通知未决"合并），然后重启 `P2*` 窗口。轮询 vs 自由跑承载层没有条件逻辑；同一套代码在两者上都正确。队列反压在慢排空时自然节流泵。

2. **P4 是无条件的事务上限**，应用于每条链路。其超期 NRC 是每服务选择（表行字段），默认 0x21 `busyRepeatRequest`，不是硬编码的 0x22。

3. **客户端 P2 总是从 confirm 开始**。confirm-wait 被客户端侧超时钳住，所以缺失的 confirm 降级为命名超时而不是挂起。

4. **0x11 ECUReset 响应然后复位排序**完全挪进 handler。每个 handler 接收一个 `CONFIRM` 阶段（响应传输后递送）；0x11 handler 在那儿撬动调用者提供的执行器，执行器带自己的安定延迟。服务器永不特殊对待 SID。

5. **功能寻址响应抑制**（在功能寻址请求上丢弃 NRC 0x11/0x12/0x31/0x7E/0x7F）读 `sdu->ta_type`，永不读 config bit。传输层基于接收地址戳 `ta_type`；UDS 无条件应用 ISO 14229-1 抑制规则。

6. **多链路解复用**（当一个应用想要一个服务多个传输的共享 `rx_queue` 时）是 8 行在 `sdu->link` 上分发的应用代码。每个 UDS 实例保持独立的 session/security 状态。替代方案（一个服务器中 `serve_any_link = true`）是安全缺陷：通过一条路径解锁会解锁另一条。

### NRC 归属划分

- **核心**（层自己发出这些）：0x11 `serviceNotSupported`、0x12 `sub-functionNotSupported`、0x7E `sub-functionNotSupportedInActiveSession`、0x13 `incorrectMessageLengthOrInvalidFormat`、0x14 `responseTooLong`、0x21 `busyRepeatRequest`（P4 超期默认）、0x33 `securityAccessDenied`、0x7F `serviceNotSupportedInActiveSession`、0x78 `requestCorrectlyReceived-ResponsePending`，以及 P4 超期时的 0x22 `conditionsNotCorrect`（如果服务表行指定了它）。
- **Handler**（所有其他的，包括 0x22/0x24/0x31/0x35/0x36/0x72 在其域特定使用中）。

### 绑定 Shim 示例（CAN 路径，示意性）

```c
/**
 * @file    nx_uds_port_isotp.c
 * @brief   连接 nx_can_isotp 到 nx_uds_server。
 *
 * 每条传输路径一个 shim，写一次。CAN 路径上约 30 行，因为
 * nx_can_isotp 已经说 ref-msg + queue 习语。
 */

void app_uds_pump_can(nx_uds_server_t *srv, nx_can_isotp_t *iso)
{
    nx_ref_msg_t *m;
    while (nx_queue_pop(iso->cfg.sdu_tx_queue, &m) == NX_QUEUE_OK) {
        const nx_tp_sdu_t *s = (const nx_tp_sdu_t *)nx_ref_msg_data(m);
        if (s->kind == NX_TP_SDU_INDICATION) {
            nx_uds_server_indicate(srv, s->data, s->len, s->ta_type, s->link);
        } else {  /* NX_TP_SDU_CONFIRM */
            nx_uds_server_confirm(srv, s->link, s->result);
        }
        nx_ref_msg_release(m);
    }
    nx_uds_server_process(srv);   /* 驱动 0x78 泵，就绪时调用 out_fn */
}

/* 服务器的输出回调，在 init 时接线，分配 + 发布： */
void app_uds_out_can(void *user, uint8_t link, const uint8_t *rsp,
                     uint32_t len, nx_tp_ta_type_t ta_type)
{
    app_ctx_t *app = (app_ctx_t *)user;
    nx_can_isotp_t *iso = app->iso_can;   /* app 知道哪个链路是哪个 */
    
    nx_ref_msg_t *m = nx_ref_msg_alloc(iso->cfg.pool,
                                       sizeof(nx_tp_sdu_t) + len);
    if (m == NULL) { return; }   /* 分配失败时丢弃 */
    
    nx_tp_sdu_t *s = (nx_tp_sdu_t *)nx_ref_msg_data(m);
    s->len     = len;
    s->link    = link;
    s->kind    = NX_TP_SDU_INDICATION;   /* 请求下行 */
    s->ta_type = ta_type;
    s->result  = NX_TP_N_OK;
    memcpy(s->data, rsp, len);
    
    nx_ref_msg_publish(m, iso->cfg.sdu_rx_queue);
    nx_ref_msg_release(m);   /* 生产者引用 */
}
```

Modbus shim 类似长度；Modbus 传输以 ISO-TP 同样方式排队 SDU。

## 待定问题（真正的 UDS 层分叉）

这是对抗性审查后幸存的仅有三个设计分叉。每个都是 UDS 层问题（不是传输或产品问题），不能通过更努力思考解决，并导致实质上不同的头文件或状态。

### 1. Session 和 Security 作用域

当一个产品终结两个独立 UDS 服务器实例（CAN 路径和 Modbus 路径）时，活动诊断会话和解锁的安全等级的作用域是什么？

**选项：**

- **每实例**（默认）：每个 `nx_uds_server_t` 在其 `.run` 结构体中拥有 `session`、`sec_level`、`s3_deadline`。通过一条路径解锁不会解锁另一条；两个刷写对话可以同时打开，UDS 层不知道。如果产品必须防止并发刷写，互锁属于 flash 驱动（资源所有者），不属于 UDS。

- **共享 ECU 状态**：两个实例的 cfg 都指向的调用者拥有的 `nx_uds_diag_state_t`。两者看到一个会话和一个安全等级。需要规则：谁的 S3 超期丢弃共享会话、当第一个实例持有编程会话时第二个实例应答什么（NRC 0x22 / 0x21 / 继续），以及是否需要所有权字段。

- **分裂**：每实例 session 和 S3（对话的属性）；通过指针共享安全等级（ECU 的属性）。通过 CAN 解锁会解锁 Modbus；会话保持独立。

**推荐：** 每实例。ISO 14229-1 为一个连接编写，没给两个连接规则；标准自己的 session/security/S3 定义是为单个服务器陈述的。共享安排创建一个安全面（一个测试器的解锁到达另一个的路径），并强迫三条仲裁规则进入每实例设计永不需要的层。

### 2. DEFER 处置

服务器保留 `DEFER` 处置（handler 拿走请求，稍后通过独立的 `nx_uds_reply(ticket, ...)` 回答，业务模块永不持有服务器句柄）吗，还是只出 `PENDING`（handler 在每次 `process()` 被重新调用直到返回 `DONE`）？

**选项：**

- **仅 PENDING**：无 ticket 类型、无世代计数器、无第二条响应发射路径。Handler 轮询自己的完成（`RESUME` 阶段的 `st->flash_busy()`），就绪时返回 `DONE`。一个嵌入式事务槽、一个响应发射器（`process()`），handler 契约的 `RESUME` 阶段携带所有延续。如果出现真正的异步完成者，稍后可增量添加。

- **DEFER**：定义 `nx_uds_ticket_t`（带世代计数器）、调用者提供的可通过 ticket 寻址的事务槽存储、一个在没有服务器句柄的情况下发布迟到答案的独立 `nx_uds_reply()`，以及一个让迟到回复和 0x78 泵在 `indicate()` 调用之间触发的响应发射模型（可能是 sink）。当完成 UDS 操作的东西在时间/上下文上与服务器主循环解耦时有理（RTOS 任务、完成 flash 写入的 ISR）。

**推荐：** 第一版仅 PENDING。`RESUME` 已经让 handler 跨周期轮询业务模块（`st->flash_busy()` 就是全部实现），所以 `DEFER` 买到的不是"慢答案"，而是具体的"来自不会被轮询的模块的答案"——更窄的需求。ticket 机器是只有真正异步完成者才能证明的表面。当有服务在 `PENDING` 下写不出来时重估；这个增加是纯增量的，不会破坏现有 handler。

### 3. 客户端架构

客户端被服务器用的同一张 `const` 服务表驱动（表驱动且对称：`nx_uds_service_t` 长出响应侧字段，`nx_uds_client` 吃服务器吃的同一张表）吗，还是保持事务器（无表：`nx_uds_client_request(c, req, len, ta_type)` 提交不透明字节，层从传输 confirm 运行 P2，吸收 0x78，交回最终响应字节加 NRC 如果是负的；调用者解析）？

**选项：**

- **表驱动且对称**：`nx_uds_service_t` 长出预期响应长度边界、带阶段的响应 handler 等。一行描述两种角色的服务。客户端成为验证器。

- **事务器（无表）**：客户端的整个状态是一个事务、一个截止时间、一个提交的消息引用。它运行 P2/P2*/0x78 状态机（真正难的部分）和传输失败案例（`result != NX_TP_N_OK`），交回字节。调用者解释。可选：库已经出货行的服务的一小组导出请求构建器 / 响应解析器，作为 helper 坐在客户端旁边而不是里面。

**推荐：** 事务器。表驱动客户端必须编码响应格式才值得，而编码 ISO 14229-1 的响应格式恰恰是整个数据驱动设计要避免的封闭集合承诺——它会让库在客户端侧重新进入知道服务的生意，刚在服务器侧退出。把客户端保持在真正通用且真正难的东西上：confirm 锚定的 P2、0x78/P2* 吸收、传输失败案例、功能请求多响应者案例。调用者解析它期待的。稍后如果出货的行想要可选 helper（行伴侣）再加。

## 实现增量

1. **`nx_tp_sdu.h` 不变**（已出货）。
2. **`nx_uds.h`** — 纯头文件词汇表：SID/NRC 枚举、会话掩码、`nx_uds_xfer_t`。
3. **`nx_uds_server.{h,c}`** — 核心服务器，带 assert fixture，包括一个**敌对端口**（迟到 confirm、丢消息、发垃圾 SDU），证明层永不信任传输。
4. **CAN 路径运行** — `nx_can_isotp` 不变，`examples/` 中一个绑定 shim 示例，你的 CAN 刷写工作。
5. **`nx_uds_transfer.{h,c}`** — 0x34/0x35/0x36/0x37 handler。
6. **`nx_modbus_rtu_tp.{h,c}`** — Modbus 承载层（ADU ↔ `nx_tp_sdu_t`）。
7. **`nx_uds_client.{h,c}`** — 事务器。
8. **`nx_modbus_rtu_master.{h,c}`** — 可选（如果你的 Modbus 路径当前没有 master 且需要一个）。

Modbus 承载层不阻塞任何东西；CAN 刷写在增量 4 工作。

## 关键陷阱

这些是设计审查期间浮出水面的承载正确性约束：

1. **`maxNumberOfBlockLength` 的 `- 2`**（从 `max_req_apdu` 减去 SID + blockSequenceCounter）必须恰好在一个地方存在。遗留代码有四份拷贝；第四份已经不一致。

2. **S3 在接受 indication 时重启**，不是在组装响应时。慢 handler 不能让 S3 超期。

3. **`nx_uds_server_process()` 每次调用必须恰好接受一个 indication**，不排空队列。不像 `nx_can_isotp_process()`（正确排空），UDS 服务器一次持有一个事务，接受第二个请求会中止第一个。

4. **每条路径分开的 pool**。服务两个传输的一个共享 pool 是预算耗尽攻击：CAN 路径上的洪水会饿死 Modbus 路径的消息槽。

5. **服务器中无 `nx_lock` 钩子**。层是单线程的（每实例一个 `process()`）；并发是应用的工作。文档化 `nx_queue` SPSC 警告：双方读-修改-写共享计数，所以 ISR 生产者抢占主循环消费者会丢更新。在裸金属上，如果传输是 ISR 驱动的，在临界区包裹 `indicate()`。

6. **`blockSequenceCounter` 回绕 0xFF → 0x00**，不是 0xFF → 0x01。计数器周期为 256；`0x01` 只在一次新传输的首块上特殊，而不是回绕的目标值。（行为已对多个独立实现交叉验证；此处原先引用的“表 407”无法核实，已删除，不将其带入代码。）

7. **无 `ctx->srv` 回指针**。Handler 接收 `ctx`（事务状态）和 `user`（来自表行），永不接收服务器句柄。这保持事务自包含，handler 可测试而无需完整服务器实例。

8. **零拷贝请求、调用者 buffer 响应**。Handler 读 `ctx->req`（指向接收的 SDU，直到 `DONE`/`NEGATIVE`/`DEFER` 有效）并写入 `ctx->out`（事务槽中的固定调用者提供 buffer，由预先钳到 `min(buffer size, cfg.max_resp_apdu)` 的 `ctx->out_cap` 尺寸化）。无中间拷贝；无动态分配。

## 参考文献

- ISO 14229-1:2020 — 统一诊断服务（UDS）— 第 1 部分：应用层
- ISO 14229-2:2013 — 第 2 部分：会话层服务
- ISO 14229-3:2012 — 第 3 部分：UDSonCAN
- ISO 15765-2:2016 — 道路车辆 — 基于控制器局域网的诊断通信（DoCAN）— 第 2 部分：传输协议和网络层服务

## 修订历史

- 2026-08-20：初始设计笔记。对抗性审查消除了 `nx_tp_port_t`；容量数重定位到 `nx_uds_server_cfg_t`；传输机制限定在绑定 shim。三个待定问题（session/security 作用域、DEFER、客户端架构）。
