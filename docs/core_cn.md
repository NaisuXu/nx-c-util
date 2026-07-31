# 核心模块
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
`nx_timer_mgr_process(mgr, now)` 触发已到期的定时器。本模块不触碰任何硬件、不做任何分配，
因此在裸机、RTOS 或 PC 上表现一致。

- **tick 单位由调用者定义** —— 一个 tick 不是毫秒；无论你的源以什么为单位计数
  （1 ms 的 SysTick、10 ms 的 RTOS tick、PC 上的微秒），那就是每个 delay/period 的
  单位。`nx_timer_start(t, 100, 0)` 表示“从现在起 100 个 tick 后触发”。
- **一次性与周期性** —— `period = 0` 的定时器触发一次后停止；`period != 0` 的定时器
  重装并每 `period` 个 tick 再次触发。
- **回调上下文** —— 回调在 `nx_timer_mgr_process` 内部运行。你在哪里调用它就决定了回调的
  上下文：在主循环调用可获得宽松的回调，在 tick 中断里调用可获得更紧的延迟（此时回调要
  非常短）。
- **溢出安全** —— tick 是 `uint32_t` 会回绕；到期用有符号差值比较，因此只要单个定时器
  的 delay 或 period 不超过 `INT32_MAX` 个 tick（1 ms tick 下约 24.8 天），回绕就能
  被正确处理。
- **零分配** —— 每个定时器都存放在调用者的存储中；它们挂在一个侵入式链表上跟踪。
- **非线程安全** —— 若定时器的启动/停止与 `mgr_process` 处于不同上下文，需自行串行化访问。

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
    nx_timer_mgr_process(&mgr, tick);   /* fires the callback at tick 0, 10, 20, ... */
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

