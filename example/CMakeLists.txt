cmake_minimum_required(VERSION 3.10)

project(nx_c_util_examples C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Path to the library sources (../src relative to this file).
set(NX_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# One driver (nx_c_util_examples.c) runs every component's example routine.
add_executable(nx_c_util_examples
    nx_c_util_examples.c
    nx_queue_example.c
    nx_tiered_mem_pool_example.c
    nx_ref_msg_example.c
    nx_sha256_example.c
    nx_crc_example.c
    ${NX_SRC_DIR}/nx_queue.c
    ${NX_SRC_DIR}/nx_tiered_mem_pool.c
    ${NX_SRC_DIR}/nx_ref_msg.c
    ${NX_SRC_DIR}/nx_sha256.c
    ${NX_SRC_DIR}/nx_crc.c
)

target_include_directories(nx_c_util_examples PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NX_SRC_DIR}
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nx_c_util_examples PRIVATE -Wall -Wextra)
endif()
