find_package(GTest REQUIRED)

# 分两个测试二进制，理由很实际：
#   mc_host_core_tests 只用 QCoreApplication —— 不需要图形平台插件，任何环境都能跑；
#   mc_host_ui_tests   要 QApplication    —— 需要平台插件，用 offscreen 无头运行。
# 合成一个的话，"没有显示器"的环境里连不涉及界面的用例也会一起挂掉。

add_executable(mc_host_core_tests
    core_test_main.cpp
    test_axis_group.cpp
    test_curve_model.cpp
    test_device_manager.cpp
    test_history_store.cpp
    test_recipe_store.cpp
    test_sequence_store.cpp
    test_serial_device.cpp
)
target_link_libraries(mc_host_core_tests PRIVATE mc_host_lib GTest::gtest)
target_compile_options(mc_host_core_tests PRIVATE -Wall -Wextra)

add_executable(mc_host_ui_tests
    ui_test_main.cpp
    test_ui_smoke.cpp
)
target_link_libraries(mc_host_ui_tests PRIVATE mc_host_lib GTest::gtest)
target_compile_options(mc_host_ui_tests PRIVATE -Wall -Wextra)

include(GoogleTest)

# PRE_TEST：把"枚举用例"这一步推迟到测试运行时。
# 否则构建期就会执行一次测试二进制，而它一上来就要建 QApplication ——
# 在没有图形平台的环境里，构建会直接失败（而不是测试失败，这更难查）。
gtest_discover_tests(mc_host_core_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_TIMEOUT 30
)

gtest_discover_tests(mc_host_ui_tests
    PROPERTIES
        LABELS "unit"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
)
