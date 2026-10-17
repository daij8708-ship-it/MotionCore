#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>

#include "mc/device_simulator.hpp"
#include "mc/link_monitor.hpp"
#include "mc/log.hpp"
#include "mc_host/axis_group.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

#include "host_test_util.hpp"

// 双轴协调（P1-7）与运行日志（P1-8）在**宿主层**的用例。
//
// 这一层测的是**编排**，不是数学：同步规划的"同时到达"由核心的纯函数用例
// （common/tests/test_axis_group.cpp）穷举证明，这里只回答"命令真的发出去了吗、
// 任轴出事时另一轴真的被停了吗、关键事件真的进了日志吗"。
namespace {

using mchost::AxisGroup;
using mchost::CommandStatus;
using mchost::DeviceManager;
using mchost::SimulatedDevice;

mc::LinkMonitor::Config fastMonitorConfig() {
    mc::LinkMonitor::Config config;
    config.heartbeatPeriodMs = 50U;
    config.missedBeatsToDeclareLinkDown = 3U;
    config.minAckTimeoutMs = 50U;
    return config;
}

bool hasStatus(const DeviceManager& manager, const std::string& name, CommandStatus status) {
    if (manager.device() == nullptr) {
        return false;
    }
    for (const mchost::CommandRecord& record : manager.device()->commandLog()) {
        if (record.name == name && record.status == status) {
            return true;
        }
    }
    return false;
}

// 规划是纯函数：没连设备也该算得出来（位置都按 0 起算）。
TEST(AxisGroupHost, PreviewIsSynchronizedAndRespectsTheSpeedLimit) {    AxisGroup group;
    const mc::GroupMovePlan plan = group.previewMove(200.0, 60.0, 100.0);
    ASSERT_TRUE(plan.ok());
    // 公共时长按最远的那一轴定：200 / 100 = 2 s；另一轴缩到 60 / 2 = 30 deg/s。
    EXPECT_NEAR(plan.durationSec, 2.0, 1e-9);
    EXPECT_NEAR(plan.axes[0].speedDegPerSec, 100.0, 1e-9);
    EXPECT_NEAR(plan.axes[1].speedDegPerSec, 30.0, 1e-9);
    // 没有一轴超过上限 —— 这正是"按最远轴的时长定速度"的推论。
    EXPECT_TRUE(mc::planRespectsSpeedLimit(plan, 100.0));
}

// 没连上时说得出为什么开不动，而不是"点了没反应"。
TEST(AxisGroupHost, RefusesToMoveWhenAxesAreNotReadyAndSaysWhy) {
    AxisGroup group;
    group.useSimulatedAxes();
    EXPECT_FALSE(group.allReady());
    EXPECT_FALSE(group.startBlockReason().isEmpty());

    EXPECT_FALSE(group.moveTo(20.0, 6.0, 100.0));
    EXPECT_FALSE(group.lastRefusalText().isEmpty());
    // 拒绝就**不许**有命令溜上链路。
    for (std::size_t i = 0U; i < group.axisCount(); ++i) {
        EXPECT_FALSE(hasStatus(group.axis(i), "SET_TARGET", CommandStatus::Acked));
        EXPECT_FALSE(hasStatus(group.axis(i), "SET_TARGET", CommandStatus::Awaiting));
    }
}

// 协调移动：两轴都拿到命令、都走完，结局都是"已确认"。
TEST(AxisGroupHost, ASynchronizedMoveDispatchesToBothAxesAndCompletes) {
    AxisGroup group;
    group.useSimulatedAxes();
    ASSERT_TRUE(group.connectAll());
    ASSERT_TRUE(mchost_test::waitUntil([&group]() { return group.allReady(); }, 8000))
        << group.startBlockReason().toStdString();

    // 短行程：20 deg / 100 deg·s⁻¹ = 0.2 s，用例不必真等两秒。
    ASSERT_TRUE(group.moveTo(20.0, 6.0, 100.0)) << group.lastRefusalText().toStdString();

    // 关键：**同一拍里不能已经把 START 发出去了**。启动是跨拍的 ——
    // 先切速度环、等设备报上新模式，再发启动。同一次调用里连着发两条，
    // 第二条会被本地联锁当场拦下（那时快照还是旧模式）。这条断言钉的就是这个顺序。
    EXPECT_EQ(group.status().phase, AxisGroup::Phase::Starting);
    for (std::size_t i = 0U; i < group.axisCount(); ++i) {
        for (const mchost::CommandRecord& record : group.axis(i).device()->commandLog()) {
            EXPECT_NE(record.name, "START")
                << "启动命令与「切模式」落在同一拍：设备还没报上新模式，联锁会拒它";
        }
    }

    ASSERT_TRUE(mchost_test::waitUntil(
        [&group]() { return group.status().phase == AxisGroup::Phase::Done; }, 5000))
        << group.status().detail.toStdString();

    for (std::size_t i = 0U; i < group.axisCount(); ++i) {
        EXPECT_TRUE(hasStatus(group.axis(i), "SET_TARGET", CommandStatus::Acked))
            << "轴" << i + 1 << "的 SET_TARGET 没有被确认";
        EXPECT_EQ(group.status().axisOutcomes[static_cast<int>(i)], CommandStatus::Acked);
    }
    group.disconnectAll();
}

// 全组安全闸：任一轴进入故障，另一轴必须立刻被停。
//
// 这一条比"协调得准不准"重要得多 —— 一台轴停了、另一台还在走，对机械就是被拧一下。
TEST(AxisGroupHost, AnyAxisFaultStopsTheWholeGroup) {
    AxisGroup group;
    group.useSimulatedAxes();
    // 让第 1 轴的通信丢失保护窗口短一点，不必等生产默认的三秒。
    mc::SimulatorConfig simConfig;
    simConfig.commLossTimeoutMs = 150U;
    group.axis(0).setDevice(std::make_unique<SimulatedDevice>(simConfig, fastMonitorConfig()));

    ASSERT_TRUE(group.connectAll());
    ASSERT_TRUE(mchost_test::waitUntil([&group]() { return group.allReady(); }, 8000));

    // 下一趟很长的移动，好让"出事"发生在运动进行中。
    ASSERT_TRUE(group.moveTo(1000.0, 300.0, 100.0));
    ASSERT_TRUE(mchost_test::waitUntil(
        [&group]() { return group.status().phase == AxisGroup::Phase::Moving; }, 5000))
        << group.status().detail.toStdString();

    // 把第 1 轴的下行全丢：设备再也收不到帧，到点自己进 Fault。
    group.axis(0).setDownlinkDropEveryNthByte(1);

    ASSERT_TRUE(mchost_test::waitUntil(
        [&group]() { return group.status().phase == AxisGroup::Phase::Aborted; }, 6000))
        << group.status().detail.toStdString();
    EXPECT_FALSE(group.status().safetyDetail.isEmpty());
    // 第 2 轴（健康的那一台）确实收到了 STOP —— 单向命令，没有应答。
    EXPECT_TRUE(hasStatus(group.axis(1), "STOP", CommandStatus::Unidirectional))
        << "安全闸触发了，但健康那一轴没有收到 STOP";
    group.disconnectAll();
}

// 运行日志：连接与命令结局真的被 DeviceManager 写进日志器（P1-8b）。
TEST(DeviceLoggingHost, ConnectsAndCommandOutcomesLandInTheLog) {
    mc::Logger logger(64U);
    logger.setMinLevel(mc::LogLevel::Debug);

    DeviceManager manager(DeviceManager::StorePolicy::Disabled);
    manager.setLogger(&logger);
    manager.setDevice(std::make_unique<SimulatedDevice>(mc::SimulatorConfig{}, fastMonitorConfig()));

    ASSERT_TRUE(manager.connectDevice());
    ASSERT_TRUE(mchost_test::waitUntil([&manager]() { return manager.device()->sessionReady(); },
                                     8000));

    bool sawLinkInfo = false;
    for (const mc::LogRecord& record : logger.ring().records()) {
        if (record.tag == "link" && record.level == mc::LogLevel::Info) {
            sawLinkInfo = true;
        }
    }
    EXPECT_TRUE(sawLinkInfo) << "连接成功没有写进日志";

    ASSERT_TRUE(manager.sendSetMode(1));
    ASSERT_TRUE(mchost_test::waitUntil(
        [&manager]() { return hasStatus(manager, "SET_MODE", CommandStatus::Acked); }, 5000));

    // 命令从"等待应答"推进到"已确认"是一次**变化**，要留下一条 cmd 日志；
    // 而且同一状态不会被每拍重复写（过滤靠 loggedCommandStatus_）。
    bool sawCmdInfo = false;
    for (const mc::LogRecord& record : logger.ring().records()) {
        if (record.tag == "cmd" && record.level == mc::LogLevel::Info &&
            record.text.find("SET_MODE") != std::string::npos) {
            sawCmdInfo = true;
        }
    }
    EXPECT_TRUE(sawCmdInfo) << "命令被确认这件事没写进日志";

    manager.disconnectDevice();
}

// 级别过滤发生在**入口**：调到只记错误，比它低的压根不进环形、也不计进"已记录"。
TEST(DeviceLoggingHost, TheLevelFilterIsAppliedBeforeAnythingIsRecorded) {
    mc::Logger logger(64U);
    logger.setMinLevel(mc::LogLevel::Error);

    DeviceManager manager(DeviceManager::StorePolicy::Disabled);
    manager.setLogger(&logger);
    manager.setDevice(std::make_unique<SimulatedDevice>(mc::SimulatorConfig{}, fastMonitorConfig()));

    ASSERT_TRUE(manager.connectDevice());
    ASSERT_TRUE(mchost_test::waitUntil([&manager]() { return manager.device()->sessionReady(); },
                                     8000));

    // 连接成功是 Info 级：这一档全被挡掉，环形里不该有它。
    EXPECT_EQ(logger.totalWritten(), 0U);
    EXPECT_GT(logger.totalFiltered(), 0U);
    EXPECT_TRUE(logger.ring().records().empty());
    manager.disconnectDevice();
}

}  // namespace
