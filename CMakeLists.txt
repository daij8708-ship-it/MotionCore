cmake_minimum_required(VERSION 3.16)

project(mc_motion_control
        VERSION 0.1.0
        DESCRIPTION "Industrial motion control and real-time monitoring system (Qt/C++ host + STM32 firmware)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 没指定构建类型时默认用 Debug，保证断言与调试符号都在。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)      # defines BUILD_TESTING (default ON) and calls enable_testing()

add_subdirectory(common)
add_subdirectory(tools)

# Qt6 上位机。host/ 自己在找不到 Qt6 时会安静跳过（并打一条警告），
# 所以"没装 Qt 的机器"照样能构建与测试 libmc_core —— 这条 ADR-001 不受影响。
# 想明确关掉它（例如只跑协议核心的 CI）用 -DMC_BUILD_HOST=OFF。
option(MC_BUILD_HOST "构建 Qt6 上位机（需要 Qt6 Widgets）" ON)

if(MC_BUILD_HOST)
    add_subdirectory(host)
endif()
