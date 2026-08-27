# 算法模块
### nx_crc —— CRC-8 / CRC-16 / CRC-32 校验

按位计算的 CRC 例程，不用查找表，因此无需预分配或存储任何东西，每次调用都是确定性的。

- **三个层次** —— 面向常见标准的命名封装；接受 Rocksoft 模型参数（多项式、初值、输入/输出反射、最终异或）以支持任意变体的通用一次性函数（`nx_crc8_compute` / `nx_crc16_compute` / `nx_crc32_compute`）；以及为分片到达数据准备的增量上下文 API（`nx_crc_init` / `nx_crc_update` / `nx_crc_final`）。分片计算与一次性调用得到完全相同的结果。
- **内置标准变体** —— CRC-8、CRC-8/ITU、CRC-8/ROHC、CRC-8/MAXIM；CRC-16/IBM/MAXIM/USB/MODBUS/CCITT/CCITT-FALSE/X25/XMODEM；CRC-32 和 CRC-32/MPEG-2。每个变体都在头文件中注明了参数及其校验值（即 `"123456789"` 的 CRC 结果）。
- **无表** —— 一个按位内核处理所有位宽和 refin/refout 组合，因此不编入任何多项式表，代码小、不占表 RAM。
- **NULL 安全** —— 数据指针为 NULL 时不贡献任何字节（视作零长缓冲），而非解引用；上下文为 NULL 时是空操作。存储由调用者拥有，库不使用任何动态内存。

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

- **两种哈希方式** —— 针对整块缓冲的一次性辅助函数（`nx_sha256`），以及为分片到达数据准备的增量上下文 API（`nx_sha256_init` / `nx_sha256_update` / `nx_sha256_final`）。分片计算与一次性调用得到完全相同的摘要。
- **固定的、调用者拥有的存储** —— 运行状态是调用者放在栈上的单个 `nx_sha256_ctx_t`；无动态内存，除固定的轮常量外无任何表，完全确定性。
- **NULL 安全** —— 数据指针为 NULL 时不贡献任何字节；上下文或摘要指针为 NULL 时是无害的空操作。
- **纯哈希，而非 MAC** —— 若需要消息认证，在其之上构建 HMAC-SHA256。

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

库的源码在 `src/` 下按类别组织（`src/core/`、`src/middleware/`、`src/algo/`），可以直接拖进你的项目，大多数模块除了标准 C 外没有依赖，可以独立使用。头文件使用单层 include（如`#include "nx_list.h"`），因此把拷贝文件所在的目录加入你的 include 路径即可。

`examples/core/` 目录包含每个模块可运行的用法示例，通过 CMake 驱动，在任何平台上都以相同方式构建。

### 构建并运行示例

在仓库根目录下：

```sh
cmake -S . -B build
cmake --build build
```
