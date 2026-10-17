cmake_minimum_required(VERSION 3.16)

project(mc_firmware LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MC_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# 固件核心：只依赖零 Qt 的协议常量与状态机，可在宿主和裸机上编译。
add_library(mc_fw STATIC
    src/frame_io.cpp
    src/device.cpp
    "${MC_COMMON_DIR}/src/device_state.cpp"
)
target_include_directories(mc_fw PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MC_COMMON_DIR}/include"
)
target_compile_options(mc_fw PRIVATE -Wall -Wextra -Wpedantic)

option(MC_FW_BUILD_TESTS "构建宿主端单元测试" ON)
option(MC_FW_BUILD_SIM "构建宿主端 PTY 设备工具" ON)
set(MC_FW_PLATFORM "host" CACHE STRING "host 或 stm32f103")

if(MC_FW_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    # 测试用宿主侧的帧编解码与载荷编码做独立参照：固件自己实现了一份
    # 逐位 CRC 和收发路径，两边必须逐字节对上。
    add_executable(mc_fw_tests
        tests/test_frame_io.cpp
        tests/test_device.cpp
        "${MC_COMMON_DIR}/src/crc16.cpp"
        "${MC_COMMON_DIR}/src/frame_codec.cpp"
        "${MC_COMMON_DIR}/src/payload.cpp"
        "${MC_COMMON_DIR}/src/protocol.cpp"
    )
    target_link_libraries(mc_fw_tests PRIVATE mc_fw GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(mc_fw_tests)
endif()

if(MC_FW_BUILD_SIM AND UNIX)
    add_executable(mc_fw_sim
        port/host/main.cpp
        "${MC_COMMON_DIR}/src/pty.cpp"
    )
    target_link_libraries(mc_fw_sim PRIVATE mc_fw)
endif()

if(MC_FW_BUILD_TESTS AND UNIX AND TARGET mc_fw_sim)
    add_executable(mc_fw_pty_test
        tests/test_pty_end_to_end.cpp
        "${MC_COMMON_DIR}/src/crc16.cpp"
        "${MC_COMMON_DIR}/src/frame_codec.cpp"
        "${MC_COMMON_DIR}/src/payload.cpp"
        "${MC_COMMON_DIR}/src/protocol.cpp"
    )
    target_link_libraries(mc_fw_pty_test PRIVATE mc_fw GTest::gtest_main)
    target_compile_definitions(mc_fw_pty_test PRIVATE
        MC_FW_SIM_PATH="$<TARGET_FILE:mc_fw_sim>"
    )
    add_dependencies(mc_fw_pty_test mc_fw_sim)
    gtest_discover_tests(mc_fw_pty_test)
endif()

if(MC_FW_PLATFORM STREQUAL "stm32f103")
    add_executable(mc_fw_stm32f103
        port/stm32f103/main.cpp
        port/stm32f103/board_stm32f103.cpp
        port/stm32f103/startup_stm32f103.c
    )
    target_link_libraries(mc_fw_stm32f103 PRIVATE mc_fw)
    set_target_properties(mc_fw_stm32f103 PROPERTIES SUFFIX ".elf")
    target_link_options(mc_fw_stm32f103 PRIVATE
        -T${CMAKE_CURRENT_SOURCE_DIR}/port/stm32f103/linker.ld
        -nostartfiles
        -Wl,--gc-sections
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/mc_fw_stm32f103.map
        -specs=nano.specs
        -specs=nosys.specs
    )
endif()
