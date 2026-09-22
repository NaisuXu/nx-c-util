# nx-c-util

[简体中文](README_CN.md) | [English](README.md)

## 简介

`nx-c-util` 是一个用纯 C 编写的嵌入式工具库，提供可按需组合的基础模块。

每个组件都遵循同样的设计理念：

- **不使用动态内存** —— 所有存储均由调用方提供，不依赖 `malloc`/`free`，适合无堆环境。
- **资源使用可预测** —— 各模块明确给出存储需求、配置项和失败路径，便于在资源受限和实时系统中使用。
- **便于移植** —— 使用标准 C11，不依赖特定平台，可在 Windows、Linux 和 macOS 上构建运行。

## 目录结构

```
nx-c-util/
├── src/
│   ├── core/         # 核心数据结构与工具
│   ├── middleware/   # 协议与诊断中间件
│   ├── algo/         # 算法（crc, sha256）
│   └── device/       # 平台无关的设备驱动（ws2812, kth7112）
└── examples/
    ├── core/         # 核心模块使用示例
    ├── middleware/   # 中间件模块使用示例
    ├── algo/         # 算法模块使用示例
    └── device/       # 设备驱动使用示例
```

各模块可独立集成。只需复制所需的 `.c` 和 `.h` 文件，并将其所在目录加入头文件搜索路径。头文件采用扁平引用方式，例如 `#include "nx_list.h"`，不带子目录前缀。

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
- [nx_log](docs/core_cn.md#nx_log--使用调用方存储的异步文本日志) — 使用调用方存储的异步文本日志
- [nx_event_flags](docs/core_cn.md#nx_event_flags--协作式循环的轮询事件标志) — 协作式循环的轮询事件标志

详细说明和示例请参阅[核心模块文档](docs/core_cn.md)。

### 中间件模块

- [nx_can_bus](docs/middleware_cn.md#nx_can_bus--can--can-fd-帧结构与辅助函数) — CAN / CAN FD 帧结构与辅助函数
- [nx_modbus_rtu](docs/middleware_cn.md#nx_modbus_rtu--modbus-rtu-帧结构与-crc) — Modbus RTU 帧结构与 CRC
- [nx_modbus_rtu_slave](docs/middleware_cn.md#nx_modbus_rtu_slave--事件驱动的-rtu-从站帧--订阅分发) — 事件驱动的 RTU 从站：帧 → 订阅分发
- [nx_modbus_rtu_master](docs/middleware_cn.md#nx_modbus_rtu_master--事件驱动的-rtu-主站队列--线路--订阅分发) — 事件驱动的 RTU 主站：队列 → 线路 → 订阅分发
- [nx_tp_sdu](docs/middleware_cn.md#nx_tp_sdu--传输层服务数据单元) — 传输层服务数据单元
- [nx_can_isotp](docs/middleware_cn.md#nx_can_isotp--iso-15765-2docan--iso-tp分段传输) — ISO 15765-2（DoCAN / ISO-TP）分段传输
- [nx_uds](docs/middleware_cn.md#nx_uds--iso-14229-公共类型与服务描述) — ISO 14229 公共类型与服务描述
- [nx_uds_server](docs/middleware_cn.md#nx_uds_server--iso-14229-诊断服务器ecu-侧) — ISO 14229 诊断服务器（ECU 侧）
- [nx_uds_svc_session](docs/middleware_cn.md#nx_uds_svc_session--基础会话与复位服务) — 基础会话与复位服务
- [nx_uds_svc_sec](docs/middleware_cn.md#nx_uds_svc_sec--0x27-种子密钥交换) — 0x27 种子/密钥交换
- [nx_uds_svc_transfer](docs/middleware_cn.md#nx_uds_svc_transfer--内存上传与下载服务) — 内存上传与下载服务
- [nx_uds_tp_bind](docs/middleware_cn.md#nx_uds_tp_bind--uds-端点与传输层绑定) — UDS 端点与传输层绑定
- [nx_uds_client](docs/middleware_cn.md#nx_uds_client--iso-14229-诊断客户端测试仪侧) — ISO 14229 诊断客户端（测试仪侧）

详细说明和示例请参阅[中间件模块文档](docs/middleware_cn.md)。

### 算法模块

- [nx_crc](docs/algo_cn.md#nx_crc--crc-8--crc-16--crc-32-校验) — CRC-8 / CRC-16 / CRC-32 校验
- [nx_sha256](docs/algo_cn.md#nx_sha256--sha-256-密码学哈希) — SHA-256 密码学哈希

详细说明和示例请参阅[算法模块文档](docs/algo_cn.md)。

### 设备模块

- [nx_ws2812](docs/device_cn.md#nx_ws2812--ws2812b-rgb-灯带驱动) — WS2812/WS2812B RGB LED 灯带驱动
- [nx_kth7112](docs/device_cn.md#nx_kth7112--kth7112-磁编码器-spi-驱动) — KTH7112 16 位磁编码器 SPI 驱动

详细说明和示例请参阅[设备模块文档](docs/device_cn.md)。


## 使用

`examples/core/`、`examples/middleware/`、`examples/algo/` 和 `examples/device/` 包含各模块的可运行示例，并可在所有受支持的平台上通过 CMake 构建。

### 构建并运行示例

在仓库根目录下：

```sh
cmake -S . -B build
cmake --build build
```

然后运行生成的可执行文件：

- **Linux / macOS**

  ```sh
  ./build/nx_core_examples        # 全部核心模块示例
  ./build/nx_middleware_examples  # 全部中间件模块示例
  ./build/nx_algo_examples        # 算法模块示例
  ./build/nx_device_examples      # 设备驱动示例
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

`cmake -S . -B build` 会使用当前平台的默认生成器。需要显式选择时，可通过 `-G` 指定：

```sh
# Windows，MinGW 工具链
cmake -S . -B build -G "MinGW Makefiles"

# Windows，Visual Studio 2022
cmake -S . -B build -G "Visual Studio 17 2022"

# Linux / macOS，Unix Makefiles
cmake -S . -B build -G "Unix Makefiles"

# 已安装 Ninja 的任意平台
cmake -S . -B build -G "Ninja"
```

构建环境需要 CMake 3.10 或更高版本，以及支持 C11 的编译器（GCC、Clang 或 MSVC）。

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。
