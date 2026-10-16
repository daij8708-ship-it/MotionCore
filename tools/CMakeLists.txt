# 可运行的演示/调试工具。全部零 Qt 依赖。
add_executable(mc_loopback_demo loopback_demo.cpp)

target_link_libraries(mc_loopback_demo PRIVATE mc_core)
target_compile_options(mc_loopback_demo PRIVATE -Wall -Wextra -Werror)
