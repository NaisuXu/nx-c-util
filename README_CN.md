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

## 目录结构

```
nx-c-util/
├── src/
│   ├── core/         # 核心构件（list, queue, ringbuf, timer, coro, ref_msg, mem_pool, lock）
│   ├── middleware/   # 协议解析器和协议栈（modbus_rtu, modbus_rtu_slave, can_bus，未来：can_isotp）
│   ├── algo/         # 算法（crc, sha256）
│   └── device/       # 平台无关的设备驱动（ws2812）
└── examples/
    ├── core/         # 核心模块使用示例
    ├── middleware/   # 中间件模块使用示例
    ├── algo/         # 算法模块使用示例
    └── device/       # 设备驱动使用示例
```

每个模块都设计为可独立使用。在你的项目中集成时，只需拷贝所需的 `.c` 和 `.h` 
文件即可。头文件使用单层 include（如 `#include "nx_list.h"`）无子目录前缀，
因此把拷贝文件所在的目录加入你的 include 路径即可。

## 模块

### 核心模块
- [nx_list](docs/core_cn.md#nx_list--侵入式双向循环链表) — 侵入式双向循环链表
- [nx_queue](docs/core_cn.md#nx_queue--通用环形缓冲fifo队列) — 通用环形缓冲（FIFO）队列
- [nx_ringbuf](docs/core_cn.md#nx_ringbuf--面向字节的环形缓冲) — 面向字节的环形缓冲
- [nx_tiered_mem_pool](docs/core_cn.md#nx_tiered_mem_pool--分级静态内存池) — 分级静态内存池
- [nx_ref_msg](docs/core_cn.md#nx_ref_msg--引用计数的零拷贝消息) — 引用计数的零拷贝消息
- [nx_timer](docs/core_cn.md#nx_timer--软件定时器管理器) — 软件定时器管理器
- [nx_coro](docs/core_cn.md#nx_coro--无栈协程) — 无栈协程
- [nx_lock](docs/core_cn.md#nx_lock--可插拔的临界区抽象) — 可插拔的临界区抽象

详细说明和示例请参阅[核心模块文档](docs/core_cn.md)。

### 中间件模块
- [nx_can_bus](docs/middleware_cn.md#nx_can_bus--can--can-fd-帧结构与辅助函数) — CAN / CAN FD 帧结构与辅助函数
- [nx_modbus_rtu](docs/middleware_cn.md#nx_modbus_rtu--modbus-rtu-帧结构与-crc) — Modbus RTU 帧结构与 CRC
- [nx_modbus_rtu_slave](docs/middleware_cn.md#nx_modbus_rtu_slave--事件驱动的-rtu-从站帧--订阅分发) — 事件驱动的 RTU 从站：帧 → 订阅分发

详细说明和示例请参阅[中间件模块文档](docs/middleware_cn.md)。

### 算法模块
- [nx_crc](docs/algo_cn.md#nx_crc--crc-8--crc-16--crc-32-校验) — CRC-8 / CRC-16 / CRC-32 校验
- [nx_sha256](docs/algo_cn.md#nx_sha256--sha-256-密码学哈希) — SHA-256 密码学哈希

详细说明和示例请参阅[算法模块文档](docs/algo_cn.md)。

### 设备模块
- [nx_ws2812](docs/device_cn.md#nx_ws2812--ws2812b-rgb-灯带驱动) — WS2812/WS2812B RGB LED 灯带驱动

详细说明和示例请参阅[设备模块文档](docs/device_cn.md)。


## 使用

库的源码在 `src/` 下按类别组织（`src/core/`、`src/middleware/`、`src/algo/`、
`src/device/`），可以直接拖进你的项目 —— 大多数模块除了标准 C 外没有依赖，可以独立
使用。头文件使用单层 include（如 `#include "nx_list.h"`），因此把拷贝文件所在的
目录加入你的 include 路径即可。

`examples/core/`、`examples/middleware/`、`examples/algo/` 和 `examples/device/` 目录
包含每个模块可运行的用法示例，通过 CMake 驱动，因此在任何平台上都以相同方式构建。

### 构建并运行示例

在仓库根目录下：

```sh
cmake -S . -B build
cmake --build build
```

然后运行生成的可执行文件：

- **Linux / macOS**

  ```sh
  ./build/nx_core_examples        # 核心模块（list, queue, ringbuf, mem_pool, ref_msg, timer, coro）
  ./build/nx_middleware_examples  # 中间件模块（modbus_rtu_slave）
  ./build/nx_algo_examples        # 算法模块（crc, sha256）
  ./build/nx_device_examples      # 设备驱动（ws2812）
  ```

- **Windows (MinGW / MSYS)**

  ```sh
  ./build/nx_core_examples.exe
  ./build/nx_middleware_examples.exe
  ./build/nx_algo_examples.exe
  ./build/nx_device_examples.exe
  ```

- **Windows (Visual Studio / MSVC)** —— 多配置生成器会把二进制放在按配置划分的子目录中：

  ```sh
  ./build/Debug/nx_core_examples.exe
  ./build/Debug/nx_middleware_examples.exe
  ./build/Debug/nx_algo_examples.exe
  ./build/Debug/nx_device_examples.exe
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
