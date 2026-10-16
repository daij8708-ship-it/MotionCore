# Qt6 上位机（层 1–5）。刻意与 common/ 分开：
#   mc_core 永远不链接 Qt；反过来 host 依赖 mc_core（单向）。
find_package(Qt6 COMPONENTS Widgets QUIET)

if(NOT Qt6_FOUND)
    # 找不到 Qt 就**跳过** host，而不是让整个工程配置失败 ——
    # 协议核心库必须能在没装 Qt 的机器上单独构建与测试（ADR-001）。
    message(WARNING "未找到 Qt6 Widgets，跳过 host/（只构建 libmc_core 与演示程序）。"
                    "如需上位机：apt install qt6-base-dev libgl-dev")
    return()
endif()

set(CMAKE_AUTOMOC ON)

set(MC_HOST_SOURCES
    src/curve_model.cpp
    src/device.cpp
    src/device_manager.cpp
    src/ui/alarm_page.cpp
    src/ui/curve_page.cpp
    src/ui/curve_plot.cpp
    src/ui/debug_page.cpp
    src/ui/main_window.cpp
    src/ui/motion_page.cpp
    src/ui/overview_page.cpp
    src/ui/param_page.cpp
)

set(MC_HOST_HEADERS
    include/mc_host/curve_model.hpp
    include/mc_host/device.hpp
    include/mc_host/device_manager.hpp
    include/mc_host/telemetry_ring.hpp
    include/mc_host/ui/alarm_page.hpp
    include/mc_host/ui/curve_page.hpp
    include/mc_host/ui/curve_plot.hpp
    include/mc_host/ui/debug_page.hpp
    include/mc_host/ui/main_window.hpp
    include/mc_host/ui/motion_page.hpp
    include/mc_host/ui/overview_page.hpp
    include/mc_host/ui/param_page.hpp
)

# 做成库（而不是把源码直接塞进可执行文件），是为了让测试能链接同一份实现，
# 而不是各自复制一份 —— 复制出来的副本测过了，产物未必对。
add_library(mc_host_lib STATIC ${MC_HOST_SOURCES} ${MC_HOST_HEADERS})
target_include_directories(mc_host_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(mc_host_lib PUBLIC mc_core Qt6::Widgets)
target_compile_options(mc_host_lib PRIVATE -Wall -Wextra -Werror)

add_executable(mc_host src/main.cpp)
target_link_libraries(mc_host PRIVATE mc_host_lib)
target_compile_options(mc_host PRIVATE -Wall -Wextra -Werror)

# 单线程负载实测工具。刻意**不进 ctest**：它的输出是"这台机器上量到多少"，
# 不是"对不对"—— 拿它当门禁只会得到一个随机器和构建类型飘的红绿。
# 正确性判据（补发是否真的生效）在单测里，那边是确定的。
add_executable(mc_host_bench src/bench_main.cpp)
target_link_libraries(mc_host_bench PRIVATE mc_host_lib)
target_compile_options(mc_host_bench PRIVATE -Wall -Wextra -Werror)

if(BUILD_TESTING)
    add_subdirectory(tests)

    # 端到端自检：真的把窗口建起来、真的跑定时器、真的走协议，最后按判据决定退出码。
    # 它给的是"整套东西能不能一起跑"的证据 —— 单元测试给不了这个。
    add_test(NAME host_selftest COMMAND mc_host --selftest=3)
    set_tests_properties(host_selftest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit"
        TIMEOUT 60
    )
endif()
