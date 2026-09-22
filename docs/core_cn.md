# 核心模块

## nx_list —— 侵入式双向循环链表

一个仅头文件的侵入式链表，设计类似 Linux 的 `list_head`。调用方把链接节点直接嵌入自己的结构体；加入链表时只需修改指针，不复制元素，也不分配内存。链表采用带哨兵头节点的双向循环结构，因此插入和删除无需单独处理头尾边界。

- **侵入式设计** —— 在业务结构体中嵌入 `nx_list_t`，再通过 `nx_list_entry`（`container_of` 宏）由链接节点取得所属结构体。
- **双向循环** —— 哨兵头节点的 `next` 指向第一个真实节点，`prev` 指向最后一个，形成一个环。空链表即 `head->next == head`。
- **统一的插入与删除** —— `nx_list_add` 可在任意节点后插入；删除节点时无需额外传入头节点。`nx_list_add_head` 和 `nx_list_add_tail` 分别封装头部、尾部插入。
- **支持遍历时删除** —— 普通遍历使用 `nx_list_for_each`；需要删除当前节点时使用 `nx_list_for_each_safe`。后者会在进入循环体前保存下一个节点，因此删除当前节点不会中断遍历。
- **零分配** —— 每个节点都存放在调用方提供的存储中；链表本身只包含链接指针。
- **仅头文件** —— 所有操作都是 `static inline`。

```c
#include "nx_list.h"

typedef struct task {
    int         id;
    const char *name;
    nx_list_t   link;       /* 嵌入式链表节点 */
} task_t;

nx_list_t head;
nx_list_init(&head);

task_t t1 = {1, "Init", {NULL, NULL}};
task_t t2 = {2, "Run",  {NULL, NULL}};

nx_list_add_tail(&head, &t1.link);   /* 插入尾部 */
nx_list_add_tail(&head, &t2.link);

nx_list_t *pos;
nx_list_for_each(pos, &head) {
    task_t *t = nx_list_entry(pos, task_t, link);   /* 取得节点所属的结构体 */
    printf("Task %d: %s\n", t->id, t->name);
}

nx_list_del(&t1.link);   /* 可从任意位置删除 */
```

## nx_queue —— 通用环形缓冲（FIFO）队列

一个容量固定、存储区由调用方提供的通用 FIFO 队列。

- **通用元素类型** —— 可存储任意大小的元素，以字节计（`element_size`）。
- **固定容量** —— 容量在初始化时设定，运行时永不增长。
- **可配置的满队列策略** —— 可选择 `NX_QUEUE_ON_FULL_REJECT` 拒绝新元素，也可选择 `NX_QUEUE_ON_FULL_OVERWRITE` 丢弃最旧元素并保留最新数据。
- **适用于协作式 SPSC 场景** —— 在单核环境中，一方只调用 `push`、另一方只调用 `pop`，且两者不会互相抢占时，可不加锁使用。`push` 和 `pop` 都会读改写共享计数；如果生产者和消费者可能互相抢占，或存在其他并发访问，应使用 `nx_lock` 保护操作。
- **辅助函数** —— 提供 `nx_queue_push`、`nx_queue_pop`、`nx_queue_peek`、`nx_queue_clear`，以及查询大小、容量、空和满状态的函数。

```c
#include "nx_queue.h"

int        storage[4];            /* 调用方持有的底层存储 */
nx_queue_t q;

/* 容量为 4，满队列时拒绝新元素 */
nx_queue_init(&q, storage, sizeof(int), 4, NX_QUEUE_ON_FULL_REJECT);

for (int i = 0; i < 5; i++) {
    nx_queue_push(&q, &i);        /* 第 5 次 push 返回 NX_QUEUE_ERR_FULL */
}

int v;
while (nx_queue_pop(&q, &v) == NX_QUEUE_OK) {
    /* 按 FIFO 顺序取出 0、1、2、3 */
}
```


## nx_ringbuf —— 面向字节的环形缓冲

一个存储原始字节流的 FIFO 环形缓冲区，底层存储由调用方提供。每次操作的长度可以不同，也允许只完成一部分数据，因此很适合 UART 收发等流式 I/O。

- **支持部分传输** —— `write`、`read`、`peek` 和 `discard` 均以字节为单位，并返回实际处理的长度。空间或数据不足时，函数会尽可能完成一部分，而不是整体失败；写入绝不会覆盖尚未读取的数据。
- **固定容量** —— 容量在初始化时设定，永不增长；整个缓冲都可用（不保留空槽）。
- **适合 DMA** —— `peek_linear` 返回最大的物理连续可读区，`poke_linear` 返回最大的连续可写区，DMA 可直接访问这些区域。DMA 写入结束后调用 `nx_ringbuf_commit` 提交数据；读取结束后调用 `nx_ringbuf_discard` 消费数据，无需额外的中转缓冲区。
- **适合 SPSC** —— 一个写者、一个读者时，只要两侧不互相抢占，在单核上无需加锁即安全。由于 `write` 和 `discard` 都会读改写共享的字节计数，若生产者抢占消费者（或反之），可能丢失一次更新。两侧可能互相抢占、或涉及任何其他并发访问时，用 `nx_lock` 包住这些操作（参见 `nx_lock`）。本模块不引入任何锁。
- **辅助函数** —— 可查询当前数据量、容量和剩余空间，`nx_ringbuf_clear` 可将缓冲区重置为空。

```c
#include "nx_ringbuf.h"

uint8_t      storage[64];      /* 调用方持有的底层存储 */
nx_ringbuf_t rb;
nx_ringbuf_init(&rb, storage, sizeof(storage));

/* 写入字节流；缓冲区接近满时，部分写入是正常结果 */
size_t written = nx_ringbuf_write(&rb, "hello", 5);   /* -> 5 */

char out[8];
size_t got = nx_ringbuf_read(&rb, out, sizeof(out));  /* 读取当前已有的数据 */

/* 零拷贝 DMA 发送：把连续可读区域直接交给 DMA */
size_t seg;
const uint8_t *src = nx_ringbuf_peek_linear(&rb, &seg);
if (src != NULL) {
    /* dma_send(src, seg); */
    nx_ringbuf_discard(&rb, seg);      /* DMA 完成后标记为已消费 */
}
```

## nx_tiered_mem_pool —— 分级静态内存池

一个容量固定的静态内存池，可替代 `malloc` 和 `free`。内存池将调用方提供的一块内存划分为多个分级（tier），每一级包含若干等大小的块；分配时从仍有空间的最小适配分级取块。

- **耗时有界且可预测** —— 分配会扫描数量有限的分级表，并在合适的分级中扫描受块数限制的位图。释放先扫描分级表以确定块的归属，再以常量时间更新对应的位图项。
- **块内无管理头** —— 所有权和分配状态存放在各分级的元数据与位图中，返回给调用方的整块空间均可用于载荷。
- **内置校验** —— 释放不属于该内存池或未落在块边界上的指针时返回 `NX_TIERED_ERR_INVALID`；重复释放返回 `NX_TIERED_ERR_DOUBLE_FREE`。两种错误都不会改变内存池状态。
- **分级内无外部碎片** —— 同一分级中的块大小相同；请求向上取整到块尺寸时仍可能产生内部碎片。
- **可配置的回退策略** —— 最小适配分级用尽时，默认继续尝试更大的分级；设置 `forbid_fallback` 后，只从最小适配分级分配。
- **运行期配置、单块内存布局** —— 分级表、位图和块存储均取自同一块调用方内存。初始化会报告对齐后的所需字节数；若内存起始地址未对齐，还需额外预留最多 `_Alignof(max_align_t) - 1` 字节的前置填充空间。
- **内置统计** —— 每级的块大小、数量、空闲数量，以及峰值占用（high-water mark），按索引读取。
- **可选加锁** —— 单上下文使用无需加锁。若多个上下文可能并发分配或释放，可在配置中提供 `nx_lock`，内存池会用它保护每次操作；传入 `NULL` 时，临界区辅助函数为空操作。

```c
#include "nx_tiered_mem_pool.h"

/* 显式对齐后，初始化函数报告的所需字节数可直接用于判断容量。 */
static _Alignas(max_align_t) uint8_t mem[32 * 8 + 128 * 4 + 256];

static const nx_tiered_level_cfg_t tiers[] = {
    { 32, 8 },     /* 8 个 32 字节块  */
    { 128, 4 },    /* 4 个 128 字节块 */
};

nx_tiered_mem_pool_t     pool;
nx_tiered_mem_pool_cfg_t cfg = {
    .memory      = mem,
    .memory_size = sizeof(mem),
    .tiers       = tiers,
    .tier_count  = sizeof(tiers) / sizeof(tiers[0]),
    /* 省略 forbid_fallback，即为 false：请求可回退到更大的分级 */
};

size_t required = 0;
if (nx_tiered_mem_pool_init(&pool, &cfg, &required) != NX_TIERED_OK) {
    /* 分级列表有效时，required 给出当前配置所需的内存大小 */
}

void *p = nx_tiered_mem_pool_alloc(&pool, 20);     /* 由 32 字节分级满足 */
/* ... 使用 p ... */
nx_tiered_mem_pool_free(&pool, p);                 /* 根据地址判断所属分级 */

/* 内省：按索引遍历各分级 */
for (size_t i = 0; i < nx_tiered_mem_pool_tier_count(&pool); i++) {
    nx_tiered_level_stat_t st;
    nx_tiered_mem_pool_get_tier_stat(&pool, i, &st);
    /* 可监控 st.peak_used、检测耗尽等 */
}
```


## nx_ref_msg —— 引用计数的零拷贝消息

一个基于 `nx_tiered_mem_pool` 的零拷贝消息分发层。队列中保存的是 `nx_ref_msg_t *`，因此多个消费者可以共享同一份载荷。引用计数会让内存块一直有效，直到生产者和所有消费者都释放各自持有的引用。

- **每条消息只分配一次** —— `nx_ref_msg_alloc` 在同一块内存中创建消息头和载荷，并返回生产者持有的初始引用。`nx_ref_msg_data` 返回载荷地址，`nx_ref_msg_len` 返回当前长度；`nx_ref_msg_shrink` 可缩短该长度，无需重新分配。
- **所有权必须显式交接** —— 每次 `nx_ref_msg_publish` 成功入队都会增加一个由队列持有的引用。发布完成后，生产者释放初始引用；消费者弹出指针、读取共享载荷，并在使用完毕后释放其引用。最后一次释放会将内存块归还给内存池。载荷必须在发布前写好，释放引用后不得再访问消息。
- **消息队列不能静默丢弃指针** —— `nx_ref_msg_queue_init` 将元素大小固定为 `sizeof(nx_ref_msg_t *)`，并使用 `NX_QUEUE_ON_FULL_REJECT`。覆盖策略会在不释放引用的情况下丢弃旧指针。同理，不要通过 `nx_queue_pop(q, NULL)` 丢弃消息，也不要直接清空或重新初始化仍有消息的队列；应先逐条弹出并释放。
- **多队列发布采用尽力而为策略** —— `nx_ref_msg_publish_multi` 接收一个以 `NULL` 结尾的队列指针数组，遇到满队列后仍会继续尝试其余队列。全部成功时返回 `NX_REF_MSG_OK`，仅部分成功时返回 `NX_REF_MSG_PARTIAL`，非空列表全部失败时返回 `NX_REF_MSG_ERR_FULL`。可选输出参数给出成功投递数量和第一个失败队列的下标。
- **并发由调用方统一管理** —— 引用计数和队列操作都不是原子的。发布、消费或释放可能并发时，调用方必须同时串行化引用计数与队列操作。内存池自身配置的锁只保护内存分配和释放，不能保护消息引用计数或队列。

```c
#include "nx_ref_msg.h"
#include <string.h>

/* 一个含 8 个 64 字节块的分级，并额外为元数据预留空间。 */
static _Alignas(max_align_t) uint8_t arena[8 * 64 + 256];
static const nx_tiered_level_cfg_t tiers[] = { { 64, 8 } };

nx_tiered_mem_pool_t pool;
nx_tiered_mem_pool_cfg_t pool_cfg = {
    .memory      = arena,
    .memory_size = sizeof(arena),
    .tiers       = tiers,
    .tier_count  = 1u,
};
if (nx_tiered_mem_pool_init(&pool, &pool_cfg, NULL) != NX_TIERED_OK) {
    /* 处理配置无效或内存不足 */
}

nx_ref_msg_t *queue_storage[4];
nx_queue_t queue;
nx_ref_msg_queue_init(&queue, queue_storage, 4u);

/* 生产者：分配、填充、发布，然后释放生产者引用。 */
nx_ref_msg_t *msg = nx_ref_msg_alloc(&pool, 5u);
if (msg != NULL) {
    memcpy(nx_ref_msg_data(msg), "hello", 5u);
    nx_ref_msg_publish(msg, &queue);    /* 成功时增加一个队列引用 */
    nx_ref_msg_release(msg);            /* 释放生产者引用 */
}

/* 消费者：弹出的指针持有一个引用。 */
nx_ref_msg_t *received = NULL;
if (nx_queue_pop(&queue, &received) == NX_QUEUE_OK) {
    /* consume(nx_ref_msg_data(received), nx_ref_msg_len(received)); */
    nx_ref_msg_release(received);
}
```

## nx_timer —— 软件定时器管理器

一个由调用方驱动的软件定时器管理器，内部使用侵入式链表保存活动定时器。应用周期性地把单调递增的 tick 值传给 `nx_timer_mgr_process`；该函数扫描活动定时器并同步执行到期回调。本模块不访问硬件，也不分配内存。

- **tick 单位由调用方定义** —— delay 和 period 都采用外部计数器的单位，可以是毫秒、RTOS tick 或微秒，并不固定为毫秒。
- **生命周期清晰** —— 先用 `nx_timer_mgr_init` 初始化管理器，再用 `nx_timer_init` 初始化每个定时器，最后通过 `nx_timer_start` 启动。重新启动活动定时器会按新参数重置它；对未启动的定时器调用 `nx_timer_stop` 也是安全的。
- **一次性与周期性** —— `period == 0` 表示一次性定时器；非零 period 会以上一次截止时间为基准重新装载，从而保持原有相位。一次 `nx_timer_mgr_process` 对同一个周期定时器最多调用一次回调，因此长时间逾期后会在后续多次处理调用中逐步追赶，而不会在一次调用中反复执行回调。
- **处理开销可预测** —— 启动和停止只修改侵入式链表，耗时为 O(1)。处理函数会扫描所有活动定时器，耗时为 O(N) 加回调执行时间；定时器按链表顺序访问，不按截止时间排序。
- **启动时刻与回绕约束** —— 新的截止时间基于管理器最近一次处理时记录的 tick，即 `last_tick + delay`。有符号差值比较可以处理 `uint32_t` 回绕，但每个 delay 和 period 不得超过 `INT32_MAX` 个 tick，而且两次处理之间不能跨过计数器一半以上的范围。
- **回调同步执行** —— 回调直接运行在 `nx_timer_mgr_process` 的调用上下文中。本模块不加锁；不同上下文可能调用 `start`、`stop` 或 `process` 时，必须由调用方串行化。回调中应避免修改其他定时器，否则可能影响当前链表遍历。

```c
#include "nx_timer.h"

static void on_timeout(nx_timer_t *t, void *arg) {
    (void)t;
    printf("Timer %s expired\n", (const char *)arg);
}

nx_timer_mgr_t mgr;
nx_timer_mgr_init(&mgr);

nx_timer_t t1;
nx_timer_init(&t1, on_timeout, "A");
nx_timer_start(&mgr, &t1, 100, 0);  /* 在 tick 100 触发一次 */

nx_timer_t t2;
nx_timer_init(&t2, on_timeout, "B");
nx_timer_start(&mgr, &t2, 50, 20);  /* 首次在 tick 50 触发，之后每 20 tick 一次 */

for (uint32_t now = 0; now < 200; now++) {
    nx_timer_mgr_process(&mgr, now);
}
```


## nx_coro —— 无栈协程

一组仅头文件的宏，利用 Duff's device 技巧和 `__LINE__`，让普通 C 函数可以挂起，并在下次调用时从原位置继续执行。这样，“发送、等待响应、重试”等流程可以写成顺序代码，无需 RTOS，也无需为每个任务分配独立栈。

- **无栈** —— 协程状态存放在一个由调用方持有的小型结构体中，恢复点以源码行号表示；没有独立栈、上下文切换或动态分配。
- **从不阻塞** —— 协程在每个挂起点返回给调用方。模块本身不含调度器；应用的主循环就是调度器，反复调用每个协程推动它前进。
- **按时间或按条件挂起** —— `NX_CORO_YIELD` 主动让出一次；`NX_CORO_WAIT_UNTIL` / `NX_CORO_WAIT_WHILE` 按谓词挂起；`NX_CORO_SLEEP` / `NX_CORO_TIMEDSET` / `NX_CORO_TIMEDWAIT` 基于调用方提供的 tick 源挂起。tick 源是一个 `uint32_t (*)(void)` 单调计数器，回绕由无符号差值处理。
- **两种状态类型** —— `nx_coro_stack_t` 用于让出和条件等待；`nx_coro_stack_plus_t` 用 `NX_CORO_INIT_PLUS` 初始化，额外带上时间类宏所需的 tick 源。
- **可组合** —— `NX_CORO_SCHEDULE` 报告协程是否仍在运行，因此父协程只需等待子协程即可驱动它运行到结束。

基于 `switch` 的实现带来几项限制：

- 局部变量不能跨越挂起点，需要长期保留的状态应放入调用方持有的结构体。
- `NX_CORO_BEGIN` 与 `NX_CORO_END` 之间不能再放置其他 `switch`。
- 每行源码最多放置一个挂起点，而且所有挂起点必须在同一个函数的词法范围内。
- `NX_CORO_BEGIN` 之前的代码会在每次调用时执行。

```c
#include "nx_coro.h"

/* 需要跨越挂起点保留的状态放在结构体里，而不是局部变量 */
typedef struct {
    nx_coro_stack_t base;
    int             step;
} blink_t;

static nx_coro_ret_t blink(blink_t *st) {
    NX_CORO_BEGIN(&st->base);
    while (1) {
        printf("step %d\n", ++st->step);
        NX_CORO_YIELD(&st->base);   /* 此处返回，下次调用从这里恢复 */
    }
    NX_CORO_END(&st->base);
}

blink_t a = {0}, b = {0};
NX_CORO_INIT(&a.base);
NX_CORO_INIT(&b.base);

/* 主循环就是调度器：跑一趟让每个协程各推进一步 */
for (;;) {
    blink(&a);
    blink(&b);
}
```

> **注意：** 恢复点会展开为 `lc = __LINE__; case __LINE__:`。由于生成的 `case` 前没有 `break`，GCC/Clang 会把这里识别为可能的隐式贯穿。这一贯穿是有意的：首次执行时控制流顺序到达标签，后续调用则由外层 `switch` 跳回该标签。启用 `-Wextra` 时，可同时加入 `-Wno-implicit-fallthrough`。


## nx_lock —— 可插拔的临界区抽象

一个面向平台临界区的小型适配层。`nx_lock_t` 保存匹配的 `enter`、`exit` 回调和可选上下文指针。需要同步的代码先调用 `nx_lock_enter`，执行受保护的操作，再把返回的状态原样传给 `nx_lock_exit`。

- **同步原语由调用方选择** —— 回调可以关闭中断、获取互斥量，或进入其他平台临界区。适配层本身不会创建锁，也不规定调度策略。
- **成对保存和恢复状态** —— `enter` 返回由实现定义的 `uintptr_t`，例如进入前的中断使能状态；必须把它原样传给匹配的 `exit`，以便平台代码恢复先前状态。`enter` 和 `exit` 应始终作为有效的一对提供。
- **仅头文件转发** —— `nx_lock_enter` 和 `nx_lock_exit` 检查锁指针后调用配置的回调。传入 `NULL` 锁时，两者都是空操作，其中 `nx_lock_enter` 返回 0。
- **可由模块配置，也可显式使用** —— `nx_tiered_mem_pool`、`nx_log` 和 `nx_event_flags` 可接收配置好的锁。`nx_queue` 和 `nx_ringbuf` 不接收锁，需要同步时由调用方在外部显式包裹操作；并发共享 `nx_ref_msg` 或 `nx_timer` 时也必须由调用方串行化。

```c
#include "nx_lock.h"
#include "nx_queue.h"

/* 平台回调示例：保存并关闭中断，之后恢复原状态。 */
static uintptr_t irq_enter(void *ctx) {
    (void)ctx;
    uintptr_t state = platform_irq_save();
    platform_irq_disable();
    return state;
}

static void irq_exit(void *ctx, uintptr_t state) {
    (void)ctx;
    platform_irq_restore(state);
}

static const nx_lock_t queue_lock = {
    .enter = irq_enter,
    .exit  = irq_exit,
    .ctx   = NULL,
};

int storage[4];
int item = 42;
nx_queue_t q;
nx_queue_init(&q, storage, sizeof(storage[0]), 4u, NX_QUEUE_ON_FULL_REJECT);

uintptr_t state = nx_lock_enter(&queue_lock);
nx_queue_push(&q, &item);
nx_lock_exit(&queue_lock, state);
```

保护区间应尽量短。关闭中断只能保护当前处理器核心；在多核目标上，应选择具备所需跨核语义的同步原语。

## nx_log —— 使用调用方存储的异步文本日志

一个异步文本日志模块。消息先格式化并写入调用方提供的环形缓冲区，主循环随后调用 `nx_log_process`，将缓存内容交给注入的输出回调。格式化仍发生在生产者路径上，但慢速或阻塞式输出只发生在消费路径上。模块不使用动态内存。

- **可直接阅读的文本格式** —— 每行通过 `vsnprintf` 格式化，并带有 `[级别] [tick] 文件:行: ` 前缀，可直接在串口终端查看。tick 时间戳来自 `get_tick`；该回调为 NULL 时不输出时间戳。
- **异步输出** —— 格式化后的整行先写入字节环形缓冲区。`nx_log_process` 在配置的锁下复制待输出数据，释放锁后再调用输出回调。生产者无需等待输出 I/O，慢速输出也只在一个统一位置执行。
- **输出回调可选** —— `write` 可为 NULL。此时日志保留在缓冲区中，调用方可通过 `nx_log_read` 从调试命令行或诊断命令读取，也可直接在调试器中查看。该模式适合没有实时输出接口的设备。
- **零分配、缓冲由调用方持有** —— 环形缓冲存储在配置里给出（`buffer` / `buffer_size`）；模块自身不做任何分配。
- **两级级别过滤** —— `NX_LOG_COMPILE_LEVEL` 在编译期裁掉比阈值更详细的调用点，使编译器能够移除相应的格式化工作；运行期 `level` 过滤其余日志，并可通过 `nx_log_set_level` 动态调整。
- **按整行写入，并提供满缓冲策略** —— 模块不会写入半行。超过 `NX_LOG_LINE_MAX` 的日志会在入队前截断。空间不足时，`NX_LOG_ON_FULL_OVERWRITE_OLD`（默认）逐行淘汰最旧记录，保留能够放入缓冲区的最新日志；`NX_LOG_ON_FULL_DROP_NEW` 则保留旧记录并丢弃新行。`nx_log_dropped` 会统计所有被丢弃或淘汰的行。
- **可选加锁** —— 协作式且不会互相抢占的访问无需加锁。存在多个并发生产者，或生产者可能抢占消费者时，应配置 `nx_lock`；它会保护环形缓冲区更新和整行淘汰，而格式化与输出 I/O 仍在临界区外。若从中断中记录日志，锁回调和所用 C 库本身也必须适用于中断上下文。

```c
#include "nx_log.h"

static uint8_t log_buf[512];   /* 调用方持有的环形缓冲存储 */

/* 输出端：把排出的字节写到 UART。它运行在主循环而非生产者中，
 * 因此这里可以使用阻塞式发送。 */
static void uart_sink(void *ctx, const uint8_t *data, size_t len) {
    uart_send(ctx, data, len);
}

nx_log_t log;
nx_log_cfg_t cfg = {
    .buffer      = log_buf,
    .buffer_size = sizeof(log_buf),
    .write       = uart_sink,
    .io_ctx      = &uart0,
    .get_tick    = board_millis,   /* 设为 NULL 则省略时间戳 */
    .level       = NX_LOG_LEVEL_INFO,
    .lock        = NULL,           /* 访问可能重叠时需要配置锁 */
};
nx_log_init(&log, &cfg);

NX_LOGI(&log, "link up, addr=%u", addr);   /* 已入队 */
NX_LOGD(&log, "raw=%02x", byte);           /* 比 INFO 更详细，已被过滤 */

for (;;) {
    nx_log_process(&log);   /* 把队列中的字节排到输出端 */
    /* ... 主循环的其余工作 ... */
}
```

> **注意：** 日志分两个阶段处理。`NX_LOGx` 只负责格式化并入队；主循环调用 `nx_log_process` 后，数据才会交给输出回调。若未配置输出回调，则通过 `nx_log_read` 主动读取。应根据两次排空之间可能出现的日志峰值设置 `buffer` 大小。缓冲区满时，`on_full` 决定丢弃新行还是淘汰旧行，两种策略都不会留下半行。只要对同一日志句柄的访问可能重叠，就应配置 `lock`。

## nx_event_flags —— 协作式循环的轮询事件标志

一个仅头文件的 32 位事件标志模块，可设置、清除、查询或消费应用自定义的标志位，无需调度器或阻塞式等待。它适合在协作式主循环中传递轻量信号；配置合适的锁后，也可用于 ISR 与主循环之间的通信。

- **32 个独立位** —— 每个位是一个事件（意义由用户定义）。
- **设置 / 清除 / 测试 / 获取** —— `set` 置位标志，`clear` 清零标志，`test` 只检查而不消费；配置锁时，`take` 会在一次受保护的操作中测试并清除指定掩码。广播状态适合使用 `test`，一次性工作适合使用 `take`。
- **屏障原语：`test_all`** —— 仅当掩码中的每个位都置位时才返回 `true`。可用于确认屏障或多部分就绪检查。
- **重复事件会合并** —— 在一次 `take` 之前多次设置同一位，最终仍只表现为一个置位状态。事件标志表达“至少发生过一次”，不记录发生次数。
- **可选同步** —— 初始化时传入有效的 `nx_lock`，可保护 `set`、`clear` 和 `take` 免受并发更新影响。`test`、`test_all` 和 `get` 只执行一次未加锁的 32 位读取；并发读取依赖目标平台提供一致的 32 位读操作，否则应由调用方另行串行化。
- **零分配、仅头文件** —— 实例只包含一个 `uint32_t` 标志字和一个锁指针；所有操作均为 `static inline`。

```c
#include "nx_event_flags.h"

/* 场景：ISR 设置标志，主循环获取它们 */
#define RX_READY   (1u << 0)
#define TX_DONE    (1u << 1)
#define TIMER_TICK (1u << 2)

/* 平台提供一对可在中断上下文使用的 enter/exit 回调。 */
extern const nx_lock_t g_isr_lock;

nx_event_flags_t g_events;
nx_event_flags_init(&g_events, &g_isr_lock);   /* 带锁以保证 ISR 安全 */

/* 在 ISR 中 */
void uart_rx_isr(void) {
    nx_event_flags_set(&g_events, RX_READY);   /* 在锁下原子操作 */
}

/* 在主循环中 */
for (;;) {
    if (nx_event_flags_take(&g_events, RX_READY)) {
        /* 标志已置位（现已清零）；处理 RX 字节 */
        handle_rx();
    }
    if (nx_event_flags_test(&g_events, TIMER_TICK)) {
        /* 非消耗性：标志保持置位直到显式清除 */
    }
}
```

> **注意：** `test` 和 `test_all` 只读取状态，不会清除标志，适合多个模块都需要观察的广播事件。配置锁后，`take` 会在锁内测试并清除标志，适合由单个消费者领取一次性工作。多次 `set` 会合并成一个置位状态，因此事件标志不能代替计数器。更新可能重叠时应配置锁；单上下文使用无需加锁。
