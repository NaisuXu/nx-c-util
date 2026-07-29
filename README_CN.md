# nx-c-util

[简体中文](/README_CN.md) | [English](/README.md)

## 简介

一个用纯 C 实现的工具库，为嵌入式开发提供简单、便捷的基础构件。

每个组件都遵循同样的设计理念：

- **纯静态** —— 所有存储都由调用者提供；库不使用任何动态内存，也不依赖
  `malloc`/`free`，适用于无堆的目标平台。
- **确定性** —— 操作可预测、恒定耗时、无隐藏开销，非常适合实时系统。
- **可移植** —— 标准 C11，无平台相关依赖；在 Windows、Linux、macOS 上都能构建
  和运行。

## 模块

### nx_list —— 侵入式双向循环链表

一个仅头文件的侵入式链表（Linux `list_head` 风格）。`nx_queue` 通过拷贝来跟踪独立
的元素，而 `nx_list` 把链接节点直接嵌入到用户的结构体中，因此把一个元素加入链表只
是移动指针 —— 无拷贝、无分配，用户结构体也仍然停留在原先分配的位置。双向循环布局
（哨兵头节点形成一个环）意味着插入和删除没有头/尾的特殊情况。

- **侵入式** —— 用户在自己的结构体中嵌入 `nx_list_t`；`nx_list_entry`
  （一个 `container_of` 宏）可从链接节点还原出所属结构体。
- **双向循环** —— 哨兵头节点的 `next` 指向第一个真实节点，`prev` 指向最后一个，
  形成一个环。空链表即 `head->next == head`。
- **对称的插入/删除** —— 用 `nx_list_add` 在任意位置之后插入；无需知道头节点即可
  从任意位置删除节点。头部和尾部插入是简单的封装（`nx_list_add_head` /
  `nx_list_add_tail`）。
- **安全遍历** —— `nx_list_for_each` 用于只读遍历，`nx_list_for_each_safe` 用于
  遍历期间删除（在执行循环体之前先保存 `next`，因此可以删除当前节点而不破坏循环）。
- **零分配** —— 每个节点都存放在调用者的存储中；链表本身只是链接指针。
- **仅头文件** —— 所有操作都是 `static inline`。

```c
#include "nx_list.h"

typedef struct task {
    int         id;
    const char *name;
    nx_list_t   link;       /* embedded link node */
} task_t;

nx_list_t head;
nx_list_init(&head);

task_t t1 = {1, "Init", {NULL, NULL}};
task_t t2 = {2, "Run",  {NULL, NULL}};

nx_list_add_tail(&head, &t1.link);   /* add at tail */
nx_list_add_tail(&head, &t2.link);

nx_list_t *pos;
nx_list_for_each(pos, &head) {
    task_t *t = nx_list_entry(pos, task_t, link);   /* recover containing struct */
    printf("Task %d: %s\n", t->id, t->name);
}

nx_list_del(&t1.link);   /* remove from anywhere */
```

### nx_queue —— 通用环形缓冲（FIFO）队列

一个由调用者提供缓冲、容量固定的 FIFO 队列。

- **通用元素类型** —— 可存储任意大小的元素，以字节计（`element_size`）。
- **固定容量** —— 容量在初始化时设定，运行时永不增长。
- **满队列策略** —— 每个队列可单独选择满时 push 的行为：
  `NX_QUEUE_ON_FULL_REJECT`（拒绝新元素）或 `NX_QUEUE_ON_FULL_OVERWRITE`
  （丢弃最旧元素，保留最新的）。
- **适合 SPSC** —— 在单生产者/单消费者场景下（一方只 push，另一方只 pop）天然线程
  安全；其他并发访问需要调用者自行加锁。
- **辅助函数** —— `push` / `pop` / `peek` / `clear` / `size` / `capacity` /
  `is_empty` / `is_full`。

```c
#include "nx_queue.h"

int        storage[4];            /* caller-owned backing storage */
nx_queue_t q;

/* capacity 4, reject new elements when full */
nx_queue_init(&q, storage, sizeof(int), 4, NX_QUEUE_ON_FULL_REJECT);

for (int i = 0; i < 5; i++) {
    nx_queue_push(&q, &i);        /* the 5th push returns NX_QUEUE_ERR_FULL */
}

int v;
while (nx_queue_pop(&q, &v) == NX_QUEUE_OK) {
    /* drains 0, 1, 2, 3 in FIFO order */
}
```


### nx_ringbuf —— 面向字节的环形缓冲

一个由调用者提供缓冲的字节流 FIFO。`nx_queue` 存储的是固定大小的*元素*，push/pop
是全有或全无的；而 `nx_ringbuf` 存储的是原始*字节流*：传输的字节数可变，也允许部分
完成。这天然适合串口 I/O（UART 收/发）等流式数据。

- **字节流，部分传输** —— `write` / `read` / `peek` / `discard` 以字节数为单位操作，
  返回实际移动的字节数；写入无法完全放下（或读取时可用字节不足）时会尽量传输一部分，
  而不是直接失败。不会覆盖未读数据。
- **固定容量** —— 容量在初始化时设定，永不增长；整个缓冲都可用（不保留空槽）。
- **DMA 友好** —— `peek_linear` 暴露最大的物理连续*可读*区域，`poke_linear` 暴露
  最大的连续*可写*区域，因此 DMA 引擎可以直接从环形缓冲读或往里写；直接填充后用
  `nx_ringbuf_commit` 提交，直接读取后用 `nx_ringbuf_discard` 消费。无需中转缓冲。
- **适合 SPSC** —— 在单核上一个写者、一个读者时天然线程安全；其他并发访问需要调用者
  自行加锁（参见 `nx_lock`）。本模块不引入任何锁。
- **辅助函数** —— `size` / `capacity` / `free` / `is_empty` / `is_full` / `clear`。

```c
#include "nx_ringbuf.h"

uint8_t      storage[64];      /* caller-owned backing storage */
nx_ringbuf_t rb;
nx_ringbuf_init(&rb, storage, sizeof(storage));

/* stream in; a partial write is normal when nearly full */
size_t written = nx_ringbuf_write(&rb, "hello", 5);   /* -> 5 */

char out[8];
size_t got = nx_ringbuf_read(&rb, out, sizeof(out));  /* reads what's available */

/* zero-copy DMA transmit: hand the contiguous readable region to the DMA */
size_t seg;
const uint8_t *src = nx_ringbuf_peek_linear(&rb, &seg);
if (src != NULL) {
    /* dma_send(src, seg); */
    nx_ringbuf_discard(&rb, seg);      /* mark consumed once the DMA is done */
}
```

### nx_tiered_mem_pool —— 分级静态内存池

一个确定性、无碎片的 `malloc`/`free` 替代品，由若干“分级(tier)”构成，每级是从同一块
调用者提供的缓冲中划出的一批等大小的块。

- **有界、可预测的耗时** —— 请求向上取整到块足够大的最小分级，从该级的在用位图取块。
  释放是 O(1)；分配扫描一张受该级块数限制的小位图。
- **每块零开销** —— 块不带任何头部，释放时仅凭地址区间定位所属分级。由于块内不存指针，
  块大小可以小到一个对齐单位。
- **内置双重释放检测** —— 释放一个已空闲的块会返回 `NX_TIERED_ERR_DOUBLE_FREE` 而非
  破坏内存池，且是 O(1)。
- **无碎片** —— 同一分级内每个块都一样大。
- **可配置的向上借块** —— 理想分级用尽时自动回退到更大的分级；设 `forbid_fallback`
  可限定只用最合适的分级。
- **单结构体配置** —— 缓冲、分级列表、策略都放在一个 `nx_tiered_mem_pool_cfg_t` 中；
  初始化会报告实际所需字节数，因此可以把缓冲开大、跑一次后再缩到刚好合适。缓冲无需特定对齐。
- **内置统计** —— 每级的块大小、数量、空闲数量，以及峰值占用（high-water mark）。
- **非线程安全** —— 并发访问必须由调用者加锁。

```c
#include "nx_tiered_mem_pool.h"

/* no special alignment needed; oversize it and let init report the exact need */
static uint8_t mem[32 * 8 + 128 * 4];

nx_tiered_mem_pool_t     pool;
nx_tiered_mem_pool_cfg_t cfg = {
    .memory      = mem,
    .memory_size = sizeof(mem),
    .tiers       = {
        { 32, 8 },     /* 8 blocks of 32 bytes  */
        { 128, 4 },    /* 4 blocks of 128 bytes */
    },
    .tier_count  = 2,
    /* forbid_fallback omitted -> false: a request may fall back to a larger tier */
};

size_t required = 0;
nx_tiered_mem_pool_init(&pool, &cfg, &required);   /* required = exact bytes needed */

void *p = nx_tiered_mem_pool_alloc(&pool, 20);     /* served by the 32-byte tier */
/* ... use p ... */
nx_tiered_mem_pool_free(&pool, p);                 /* owning tier inferred from address */
```


### nx_ref_msg —— 引用计数的零拷贝消息

一个消息分发层：消息从内存池中分配一次，投递给一个或多个队列。队列里存的是指向消息的
*指针*，而非拷贝，因此每个消费者共享同一份数据 —— 零拷贝。引用计数决定块何时归还给
内存池。

- **单次分配** —— 消息头和它的数据是一整块连续内存（数据是柔性数组成员，按
  `max_align_t` 对齐），因此 `alloc` 和最终的 `free` 各是一次池操作。
- **引用计数约定** —— `alloc` 返回引用计数为 1 的消息（*生产者引用*）；每次成功发布
  +1；每个消费者用完后 `release`（-1）。计数归 0 时块归还给池。生产者在发布之后必须
  `release` 一次以放弃自己的引用 —— 这也让投递给*零个*队列的消息能被无泄漏地释放。
- **多队列发布** —— `publish` 发往一个队列，`publish_multi` 一次发往一个以 `NULL`
  结尾的队列数组；两者都只在成功入队时才递增引用计数，因此满队列绝不会泄漏引用。队列
  集合由调用者组织；本模块不维护订阅表。
- **尽力而为且如实报告** —— `publish_multi` 在遇到满队列后仍继续，让其余队列照常收到
  消息，其返回码反映整体结果：`OK`（全部接受）、`PARTIAL`（部分满）或 `ERR_FULL`
  （非空列表中无一接受）。`out_delivered` 给出投递数量；`out_first_failed` 给出第一个
  满队列的下标（若无失败则为队列数量）。
- **仅拒绝的队列** —— 用 `nx_ref_msg_queue_init` 初始化载体队列（元素大小固定为一个
  消息指针）。满队列策略被强制为拒绝，因为覆盖会悄悄丢弃一条已入队的消息并泄漏它的引用。
- **非线程安全** —— 引用计数是普通计数器；并发访问必须由调用者加锁。

```c
#include "nx_ref_msg.h"

/* one consumer queue (its buffer holds message pointers) */
nx_ref_msg_t *qbuf[4];
nx_queue_t    q;
nx_ref_msg_queue_init(&q, qbuf, 4);

/* producer: allocate from a pool, fill, publish, then release its reference */
nx_ref_msg_t *m = nx_ref_msg_alloc(&pool, 16);   /* refcount = 1 */
memcpy(nx_ref_msg_data(m), payload, 16);

nx_queue_t *group[] = { &q, /* &q2, &q3, ... */ NULL };  /* NULL-terminated */
size_t delivered = 0;
nx_ref_msg_publish_multi(m, group, &delivered, NULL); /* refcount = 1 + delivered */
nx_ref_msg_release(m);                             /* give up producer reference */

/* consumer: pop the shared message, use it, release when done */
nx_ref_msg_t *got = NULL;
if (nx_queue_pop(&q, &got) == NX_QUEUE_OK) {
    /* ... read nx_ref_msg_data(got), nx_ref_msg_len(got) ... */
    nx_ref_msg_release(got);                       /* frees when the last ref is gone */
}
```

### nx_timer —— 软件定时器管理器

一个基于 tick 的软件定时器管理器，构建在 `nx_list` 之上。由调用者驱动：一个单调递增的
tick 计数器不断前进（来自硬件定时器、RTOS tick 或主循环），周期性调用
`nx_timer_process(mgr, now)` 触发已到期的定时器。本模块不触碰任何硬件、不做任何分配，
因此在裸机、RTOS 或 PC 上表现一致。

- **tick 单位由调用者定义** —— 一个 tick 不是毫秒；无论你的源以什么为单位计数
  （1 ms 的 SysTick、10 ms 的 RTOS tick、PC 上的微秒），那就是每个 delay/period 的
  单位。`nx_timer_start(t, 100, 0)` 表示“从现在起 100 个 tick 后触发”。
- **一次性与周期性** —— `period = 0` 的定时器触发一次后停止；`period != 0` 的定时器
  重装并每 `period` 个 tick 再次触发。
- **回调上下文** —— 回调在 `nx_timer_process` 内部运行。你在哪里调用它就决定了回调的
  上下文：在主循环调用可获得宽松的回调，在 tick 中断里调用可获得更紧的延迟（此时回调要
  非常短）。
- **溢出安全** —— tick 是 `uint32_t` 会回绕；到期用有符号差值比较，因此只要单个定时器
  的 delay 或 period 不超过 `INT32_MAX` 个 tick（1 ms tick 下约 24.8 天），回绕就能
  被正确处理。
- **零分配** —— 每个定时器都存放在调用者的存储中；它们挂在一个侵入式链表上跟踪。
- **非线程安全** —— 若定时器的启动/停止与 `process` 处于不同上下文，需自行串行化访问。

```c
#include "nx_timer.h"

static void led_blink(nx_timer_t *t, void *arg) {
    int *state = (int *)arg;
    *state = !(*state);
    printf("LED %s\n", *state ? "ON" : "OFF");
}

nx_timer_mgr_t mgr;
nx_timer_t     timer;
int            led_state = 0;

nx_timer_mgr_init(&mgr);
nx_timer_init(&timer, led_blink, &led_state);

/* blink every 10 ticks, starting immediately */
nx_timer_start(&mgr, &timer, 0, 10);

/* in your tick ISR or main loop: */
for (uint32_t tick = 0; tick < 100; tick++) {
    nx_timer_process(&mgr, tick);   /* fires the callback at tick 0, 10, 20, ... */
}
```


### nx_lock —— 可插拔的临界区抽象

其他模块刻意做成无锁 —— 它们不持有任何锁，把同步交给调用者。`nx_lock` 是提供同步的
推荐、可移植方式：一对小巧的 `enter` / `exit` 函数指针，由调用者用最适合目标平台的原语
填充，然后包裹住需要保护的那些短小复合操作（一次队列 push/pop、一次池 alloc/free、
一次引用计数变更）。

- **互斥，而非计数** —— 这是 `enter` / `exit`（保护数据结构不被并发访问），不是
  `take` / `give`（等待某个资源）。它是一对必须正确嵌套的对称调用。
- **保存/恢复以支持嵌套** —— `enter` 返回一个由实现定义的保存状态，交还给与之匹配的
  `exit`。在裸机 MCU 上，`enter` 通常保存中断使能状态并关中断，`exit` 精确恢复它 ——
  因此嵌套在另一个临界区内的临界区不会在内层 `exit` 时错误地重新开中断。
- **仅头文件、零平台依赖** —— `nx_lock_enter` / `nx_lock_exit` 是 `static inline`
  封装，只做空指针检查并转发到调用者的函数指针；库核心保持不变，仍然不做任何加锁。
- **NULL 即空操作** —— NULL 锁（或 NULL 的 `enter` / `exit`）返回 0 且什么都不做，
  因此同样的调用点在单线程构建中会被编译成空。

```c
#include "nx_lock.h"
#include "nx_queue.h"

/* Cortex-M bare metal: disable interrupts, saving/restoring PRIMASK */
static uintptr_t cm_enter(void *ctx) { (void)ctx; uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void      cm_exit (void *ctx, uintptr_t s) { (void)ctx; __set_PRIMASK((uint32_t)s); }

static const nx_lock_t g_lock = { cm_enter, cm_exit, NULL };

/* wrap the short compound operation, and only that */
uintptr_t s = nx_lock_enter(&g_lock);
nx_queue_push(&q, &item);
nx_lock_exit(&g_lock, s);
```

> **注意：** 保护区间要尽量小 —— 处于其中时，中断（或抢占）被挡住。只包裹 O(1) 操作，
> 绝不要包裹周围的业务逻辑。在严格单生产者/单消费者的 `nx_queue` 上你可能根本不需要锁
> （见上面 `nx_queue` 的说明）；在多核 MCU 上，关中断只能守护本核 —— 那里要用真正的
> 自旋锁。

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

### nx_crc —— CRC-8 / CRC-16 / CRC-32 校验

按位计算的 CRC 例程，不用查找表，因此没有东西需要预分配或存储，且每次调用都是确定性的。

- **三个层次** —— 面向常见标准的命名封装；接受 Rocksoft 模型参数（多项式、初值、
  输入/输出反射、最终异或）以支持任意变体的通用一次性函数（`nx_crc8_compute` /
  `nx_crc16_compute` / `nx_crc32_compute`）；以及用于分片到达数据的增量上下文 API
  （`nx_crc_init` / `nx_crc_update` / `nx_crc_final`）—— 分片计算与一次性调用得到
  完全相同的结果。
- **内置标准变体** —— CRC-8、CRC-8/ITU、CRC-8/ROHC、CRC-8/MAXIM；CRC-16
  IBM/MAXIM/USB/MODBUS/CCITT/CCITT-FALSE/X25/XMODEM；CRC-32 和 CRC-32/MPEG-2。
  每个都在头文件中注明了参数及其校验值（`"123456789"` 的 CRC）。
- **无表** —— 单一的按位内核处理所有位宽和 refin/refout 组合，因此不编入任何多项式表；
  代码小、无表 RAM。
- **NULL 安全** —— NULL 数据指针不贡献任何字节（视为零长缓冲）而非解引用，NULL 上下文
  为空操作；存储由调用者拥有，库不使用任何动态内存。

```c
#include "nx_crc.h"

const char *msg = "123456789";

/* a named standard variant */
uint16_t c1 = nx_crc16_modbus(msg, 9);      /* 0x4B37 */
uint32_t c2 = nx_crc32(msg, 9);             /* 0xCBF43926 */

/* any other variant via the generic function
 * (here: CRC-16/MODBUS spelled out explicitly) */
uint16_t c3 = nx_crc16_compute(msg, 9,
                               0x8005,      /* poly   */
                               0xFFFF,      /* init   */
                               true, true,  /* refin, refout */
                               0x0000);     /* xorout */
/* c3 == c1 */

/* the same CRC, fed in over several chunks */
nx_crc_ctx_t ctx;
nx_crc_init(&ctx, 16, 0x8005, 0xFFFF, true, true, 0x0000);
nx_crc_update(&ctx, msg, 4);                /* "1234"  */
nx_crc_update(&ctx, msg + 4, 5);            /* "56789" */
uint16_t c4 = (uint16_t)nx_crc_final(&ctx); /* == c1 */
```


### nx_sha256 —— SHA-256 密码学哈希

一个纯 C 的 SHA-256（FIPS 180-4）实现，产生 32 字节摘要。

- **两种哈希方式** —— 针对整块缓冲的一次性辅助函数（`nx_sha256`），以及用于分片到达
  数据的增量上下文 API（`nx_sha256_init` / `nx_sha256_update` / `nx_sha256_final`）；
  分片计算与一次性调用得到完全相同的摘要。
- **固定的、调用者拥有的存储** —— 运行状态是调用者放在栈上的单个 `nx_sha256_ctx_t`；
  无动态内存，除固定的轮常量外无任何表，完全确定性。
- **NULL 安全** —— NULL 数据指针不贡献任何字节，NULL 上下文或摘要指针是无害的空操作。
- **纯哈希，而非 MAC** —— 若需消息认证，在其之上构建 HMAC-SHA256。

```c
#include "nx_sha256.h"

uint8_t digest[NX_SHA256_DIGEST_SIZE];

/* one-shot */
nx_sha256("abc", 3, digest);
/* digest = ba7816bf 8f01cfea ... f20015ad */

/* the same digest, fed in over several chunks */
nx_sha256_ctx_t ctx;
nx_sha256_init(&ctx);
nx_sha256_update(&ctx, "a", 1);
nx_sha256_update(&ctx, "bc", 2);
nx_sha256_final(&ctx, digest);
```

## 使用

库的源码位于 `src/`，可以直接拖进你的项目 —— 只需编译 `.c` 文件并把 `src/` 加入
包含路径。

`example/` 目录包含每个模块可运行的用法示例，通过 CMake 驱动，因此在任何平台上都以
相同方式构建。

### 构建并运行示例

在仓库根目录下：

```sh
cd example
cmake -S . -B build
cmake --build build
```

然后运行生成的可执行文件：

- **Linux / macOS**

  ```sh
  ./build/nx_c_util_examples
  ```

- **Windows (MinGW / MSYS)**

  ```sh
  ./build/nx_c_util_examples.exe
  ```

- **Windows (Visual Studio / MSVC)** —— 多配置生成器会把二进制放在按配置划分的子目录中：

  ```sh
  ./build/Debug/nx_c_util_examples.exe
  ```

### 选择生成器

`cmake -S . -B build` 使用你平台的默认生成器，多数情况下已经够用。要显式指定一个，
传入 `-G`：

```sh
# Windows, MinGW toolchain
cmake -S . -B build -G "MinGW Makefiles"

# Windows, Visual Studio 2022
cmake -S . -B build -G "Visual Studio 17 2022"

# Linux / macOS, Unix Makefiles
cmake -S . -B build -G "Unix Makefiles"

# Any platform with Ninja installed
cmake -S . -B build -G "Ninja"
```

需要 CMake 3.10 或更新版本，以及一个支持 C11 的编译器（GCC、Clang 或 MSVC）。

## 许可证

本项目采用 MIT 许可证，详见 LICENSE 文件。
