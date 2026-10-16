# libmc_core —— 协议与算法核心库
#
# 硬约束（ADR-001：核心库零 Qt 依赖）：
#   本目标不得链接 Qt，也不得链接任何第三方库。
#   它必须能被纯 g++ + GoogleTest 构建与测试，以便在没有 Qt 的环境下验证。
add_library(mc_core STATIC
    src/ack_tracker.cpp
    src/crc16.cpp
    src/device_simulator.cpp
    src/device_state.cpp
    src/frame_codec.cpp
    src/host_session.cpp
    src/link_monitor.cpp
    src/loopback_transport.cpp
    src/payload.cpp
    src/protocol.cpp
)

target_include_directories(mc_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

# 版本号单一真源：由顶层 project(VERSION ...) 生成，避免头文件与构建脚本各写一份。
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mc/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/mc/version.hpp
    @ONLY
)

target_compile_options(mc_core PRIVATE -Wall -Wextra -Werror)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
