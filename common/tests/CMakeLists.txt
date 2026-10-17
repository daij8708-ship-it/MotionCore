find_package(GTest REQUIRED)

add_executable(mc_core_tests
    test_ack_tracker.cpp
    test_alarm.cpp
    test_audit.cpp
    test_axis_group.cpp
    test_crc16.cpp
    test_device_state.cpp
    test_end_to_end.cpp
    test_frame_codec.cpp
    test_interlock.cpp
    test_link_monitor.cpp
    test_log.cpp
    test_loopback_transport.cpp
    test_payload.cpp
    test_recipe.cpp
    test_ring_buffer.cpp
    test_sequence.cpp
    test_sim_loop.cpp
    test_simulator_model.cpp
)

# 只链接 mc_core 与 GTest —— 不链接 Qt，也不链接任何其他库。
target_link_libraries(mc_core_tests PRIVATE
    mc_core
    GTest::gtest
    GTest::gtest_main
)

target_compile_options(mc_core_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
# 逐个 TEST 注册为独立的 ctest 条目，这样 ctest 的输出里能直接看到用例数与名称。
gtest_discover_tests(mc_core_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_TIMEOUT 30
)
