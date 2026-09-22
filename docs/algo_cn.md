# 算法模块

## nx_crc —— CRC-8 / CRC-16 / CRC-32 校验

一组按位计算 CRC-8、CRC-16 和 CRC-32 的函数。实现不使用查找表，静态数据占用较小且固定。

- **三层接口** —— 常用标准可直接调用命名函数；其他变体可通过一次性计算函数 `nx_crc8_compute`、`nx_crc16_compute` 或 `nx_crc32_compute` 传入 Rocksoft 模型参数；分段到达的数据则使用增量接口 `nx_crc_init`、`nx_crc_update` 和 `nx_crc_final`。增量计算与一次性计算会得到相同结果。
- **内置标准变体** —— CRC-8、CRC-8/ITU、CRC-8/ROHC、CRC-8/MAXIM；CRC-16/IBM/MAXIM/USB/MODBUS/CCITT/CCITT-FALSE/X25/XMODEM；CRC-32 和 CRC-32/MPEG-2。每个变体都在头文件中注明了参数及其校验值（即 `"123456789"` 的 CRC 结果）。
- **无需查找表** —— 同一个按位内核处理不同位宽及 `refin` / `refout` 组合，不需要多项式查找表。
- **调用方持有状态，NULL 行为明确** —— 模块不使用动态内存。`nx_crc_init` 和 `nx_crc_update` 会忽略 `NULL` 上下文，`nx_crc_update` 也会忽略 `NULL` 数据指针；`nx_crc_final(NULL)` 返回 0。

```c
#include "nx_crc.h"

const char *msg = "123456789";

/* 直接调用已命名的标准变体 */
uint16_t c1 = nx_crc16_modbus(msg, 9);      /* 0x4B37 */
uint32_t c2 = nx_crc32(msg, 9);             /* 0xCBF43926 */

/* 通过通用函数描述其他变体；这里显式给出 CRC-16/MODBUS 参数 */
uint16_t c3 = nx_crc16_compute(msg, 9,
                               0x8005,      /* 多项式 */
                               0xFFFF,      /* 初始值 */
                               true, true,  /* 输入/输出反射 */
                               0x0000);     /* 输出异或值 */
/* c3 与 c1 相等 */

/* 分段输入同一份数据 */
nx_crc_ctx_t ctx;
nx_crc_init(&ctx, 16, 0x8005, 0xFFFF, true, true, 0x0000);
nx_crc_update(&ctx, msg, 4);                /* "1234"  */
nx_crc_update(&ctx, msg + 4, 5);            /* "56789" */
uint16_t c4 = (uint16_t)nx_crc_final(&ctx); /* == c1 */
```


## nx_sha256 —— SHA-256 密码学哈希

一个符合 FIPS 180-4 的纯 C SHA-256 实现，输出 32 字节摘要。

- **两种计算方式** —— 完整缓冲区可直接调用 `nx_sha256`；分段到达的数据使用 `nx_sha256_init` / `nx_sha256_update` / `nx_sha256_final`。两种方式生成相同的摘要。
- **固定且由调用方持有的状态** —— 增量计算只需一个 `nx_sha256_ctx_t`，可由调用方放在栈上。实现不使用动态内存，除固定的轮常量表外也不需要其他查找表。
- **NULL 行为明确** —— `NULL` 数据指针不提供任何输入字节；必要的上下文或摘要指针为 `NULL` 时，函数会安全返回且不生成输出。
- **仅提供哈希，不提供 MAC** —— SHA-256 本身不能认证消息。需要消息认证时，应使用经过验证的 HMAC-SHA256 实现。

```c
#include "nx_sha256.h"

uint8_t digest[NX_SHA256_DIGEST_SIZE];

/* 一次性计算 */
nx_sha256("abc", 3, digest);
/* 摘要 = ba7816bf 8f01cfea ... f20015ad */

/* 分段输入得到相同摘要 */
nx_sha256_ctx_t ctx;
nx_sha256_init(&ctx);
nx_sha256_update(&ctx, "a", 1);
nx_sha256_update(&ctx, "bc", 2);
nx_sha256_final(&ctx, digest);
```

## 使用

算法模块位于 `src/algo/`。可将所需的 `.c` 和 `.h` 文件直接复制到项目中，并把文件所在目录加入头文件搜索路径。

`examples/algo/` 中提供了可运行的示例，并通过 CMake 统一构建。

### 构建并运行示例

在仓库根目录下：

```sh
cmake -S . -B build
cmake --build build
```
