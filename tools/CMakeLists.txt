# 可运行的演示/调试工具。全部零 Qt 依赖。
add_executable(mc_loopback_demo loopback_demo.cpp)

target_link_libraries(mc_loopback_demo PRIVATE mc_core)
target_compile_options(mc_loopback_demo PRIVATE -Wall -Wextra -Werror)

# 设备侧模拟器：让真上位机在没有 STM32 时，能通过一条真串口/伪终端与协议等价的
# 对端通信。只做 POSIX：它要用 tty 的系统调用，Windows 上没有对应的东西
# （Windows 上跑上位机时用进程内模拟器，或按 docs 里的说明走 COM 口）。
if(UNIX)
    add_executable(mc_sim mc_sim.cpp)

    target_link_libraries(mc_sim PRIVATE mc_core)
    target_compile_options(mc_sim PRIVATE -Wall -Wextra -Werror)
endif()

# mc_sim 自检作为一条 ctest：它不依赖任何外部工具（伪终端由内核提供），
# 因此"设备端能不能在真 tty 上跑完整会话"这件事在 CI 里也有结论。
if(UNIX AND BUILD_TESTING)
    add_test(NAME mc_sim.SelfTest COMMAND mc_sim --self-test)
    set_tests_properties(mc_sim.SelfTest PROPERTIES LABELS "unit" TIMEOUT 120)
endif()
