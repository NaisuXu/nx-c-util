cmake_minimum_required(VERSION 3.10)

project(nx_c_util C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include path: project root so "src/core/nx_list.h" works
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Library sources
set(NX_CORE_SOURCES
    src/core/nx_queue.c
    src/core/nx_ringbuf.c
    src/core/nx_tiered_mem_pool.c
    src/core/nx_ref_msg.c
    src/core/nx_timer.c
    src/core/nx_log.c
)

set(NX_ALGO_SOURCES
    src/algo/nx_crc.c
    src/algo/nx_sha256.c
)

set(NX_MIDDLEWARE_SOURCES
    src/middleware/nx_can_isotp.c
    src/middleware/nx_modbus_rtu.c
    src/middleware/nx_modbus_rtu_slave.c
    src/middleware/nx_uds_server.c
    src/middleware/nx_uds_svc.c
    src/middleware/nx_uds_sec.c
    src/middleware/nx_uds_transfer.c
    src/middleware/nx_uds_tp_bind.c
)

set(NX_DEVICE_SOURCES
    src/device/nx_ws2812.c
)

# Core examples executable
add_executable(nx_core_examples
    examples/core/nx_core_examples.c
    examples/core/nx_list_example.c
    examples/core/nx_queue_example.c
    examples/core/nx_ringbuf_example.c
    examples/core/nx_tiered_mem_pool_example.c
    examples/core/nx_ref_msg_example.c
    examples/core/nx_timer_example.c
    examples/core/nx_coro_example.c
    examples/core/nx_log_example.c
    examples/core/nx_event_flags_example.c
    ${NX_CORE_SOURCES}
)

# Algo examples executable
add_executable(nx_algo_examples
    examples/algo/nx_algo_examples.c
    examples/algo/nx_crc_example.c
    examples/algo/nx_sha256_example.c
    ${NX_ALGO_SOURCES}
)

# Device examples executable
add_executable(nx_device_examples
    examples/device/nx_device_examples.c
    examples/device/nx_ws2812_example.c
    ${NX_DEVICE_SOURCES}
)

# Middleware examples executable
add_executable(nx_middleware_examples
    examples/middleware/nx_middleware_examples.c
    examples/middleware/nx_modbus_rtu_slave_example.c
    examples/middleware/nx_can_isotp_example.c
    examples/middleware/nx_uds_server_example.c
    examples/middleware/nx_uds_svc_example.c
    examples/middleware/nx_uds_transfer_example.c
    examples/middleware/nx_uds_tp_bind_example.c
    ${NX_MIDDLEWARE_SOURCES}
    ${NX_CORE_SOURCES}
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nx_core_examples PRIVATE -Wall -Wextra -Wno-implicit-fallthrough)
    target_compile_options(nx_algo_examples PRIVATE -Wall -Wextra -Wno-implicit-fallthrough)
    target_compile_options(nx_device_examples PRIVATE -Wall -Wextra -Wno-implicit-fallthrough)
    target_compile_options(nx_middleware_examples PRIVATE -Wall -Wextra -Wno-implicit-fallthrough)
endif()
