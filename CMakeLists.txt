cmake_minimum_required(VERSION 3.10)

project(nx_c_util C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Include path: project root so "core/nx_list.h" works
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Library sources
set(NX_CORE_SOURCES
    core/nx_queue.c
    core/nx_ringbuf.c
    core/nx_tiered_mem_pool.c
    core/nx_ref_msg.c
    core/nx_timer.c
)

set(NX_ALGO_SOURCES
    algo/nx_crc.c
    algo/nx_sha256.c
)

set(NX_MIDDLEWARE_SOURCES
    middleware/nx_modbus_rtu.c
)

# Basic examples executable
add_executable(nx_basic_examples
    examples/basic/nx_basic_examples.c
    examples/basic/nx_list_example.c
    examples/basic/nx_queue_example.c
    examples/basic/nx_ringbuf_example.c
    examples/basic/nx_tiered_mem_pool_example.c
    examples/basic/nx_ref_msg_example.c
    examples/basic/nx_timer_example.c
    examples/basic/nx_crc_example.c
    examples/basic/nx_sha256_example.c
    ${NX_CORE_SOURCES}
    ${NX_ALGO_SOURCES}
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nx_basic_examples PRIVATE -Wall -Wextra)
endif()
