#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "mc/device_simulator.hpp"
#include "mc/device_state.hpp"
#include "mc/link_monitor.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

#include "host_test_util.hpp"

// 应用服务层（DeviceManager）的用例。
//
// 这里跑的是**真的** DeviceManager：真的 QTimer、真的事件循环、真的握手与超时。
// 刻意不把 tick 抽成可注入的接口来"方便测试" —— 那样测的就不是产品里那条路径了。
// 代价是依赖时序，所以一律用 waitUntil 轮询而不是 sleep。
namespace {

using mchost::CommandStatus;
using mchost::DeviceManager;
using mchost::IDevice;
using mchost::SimulatedDevice;

// 快节奏链路参数：生产默认心跳 1 s、失效判定要连丢 3 拍（=3 s），
// 断链用例会慢得不像话。字段仍是产品里的那些，只是取小 —— 不另造一条测试专用链路。
mc::LinkMonitor::Config fastMonitorConfig() {
    mc::LinkMonitor::Config config;
    config.heartbeatPeriodMs = 50U;
    config.missedBeatsToDeclareLinkDown = 3U;
    config.minAckTimeoutMs = 50U;
    return config;
}

class DeviceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<DeviceManager>();
        manager_->setDevice(
            std::make_unique<SimulatedDevice>(mc::SimulatorConfig{}, fastMonitorConfig()));
    }

    IDevice* device() const { return manager_->device(); }

    bool hasStatus(const std::string& name, CommandStatus status) const {
        for (const mchost::CommandRecord& record : device()->commandLog()) {
            if (record.name == name && record.status == status) {
                return true;
            }
        }
        return false;
    }

    bool hasNack(const std::string& name, mc::NackReason reason) const {
        for (const mchost::CommandRecord& record : device()->commandLog()) {
            if (record.name == name && record.status == CommandStatus::Nacked &&
                record.nackReason == reason) {
                return true;
            }
        }
        return false;
    }

    bool connectAndWaitReady() {
        manager_->connectDevice();
        return mchost_test::waitUntil([this]() { return device()->sessionReady(); });
    }

    std::unique_ptr<DeviceManager> manager_;
};

TEST_F(DeviceManagerTest, CommandsAreRefusedWhileDisconnected) {
    // 没连接时命令本地就不发：返回值是"没发出去"，而且命令日志里一条记录都不该有。
    // 记录一条 NotSent 是给"连上了但会话没就绪"用的，跟根本没连不是一回事。
    EXPECT_FALSE(manager_->sendSetMode(1));
    EXPECT_FALSE(manager_->sendStart());
    EXPECT_FALSE(manager_->sendStop());
    EXPECT_FALSE(manager_->sendSetTarget(100.0));
    EXPECT_FALSE(manager_->sendSetPid(1.0, 0.0, 0.0));
    EXPECT_TRUE(manager_->device()->commandLog().empty());
}

TEST_F(DeviceManagerTest, ConnectingHandshakesWithoutAnyManualHello) {
    ASSERT_TRUE(connectAndWaitReady());

    EXPECT_TRUE(manager_->isConnected());
    EXPECT_GT(manager_->ticks(), 0U);
    EXPECT_GE(device()->handshakeRounds(), 1U);
    // 链路监控确实开着：界面才敢把"正常"写成"正常（监控中）"，而不是一个没有来源的绿灯。
    EXPECT_NE(device()->linkStats(), nullptr);

    EXPECT_TRUE(mchost_test::waitUntil([this]() { return device()->telemetryAcceptedTotal() > 0U; }));
    EXPECT_EQ(device()->telemetryRejectedTotal(), 0U);
}

TEST_F(DeviceManagerTest, ModeAndStartReachTheDeviceAndComeBackAcked) {
    ASSERT_TRUE(connectAndWaitReady());

    ASSERT_TRUE(manager_->sendSetMode(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));
    EXPECT_TRUE(device()->enabled());
    EXPECT_EQ(device()->runMode(), mc::SimRunMode::Velocity);

    // 设备进了 Running 只说明它执行了；上位机侧的"已确认"要等应答回来才成立 —— 分开断言。
    EXPECT_TRUE(mchost_test::waitUntil([this]() { return hasStatus("SET_MODE", CommandStatus::Acked); }));
    EXPECT_TRUE(mchost_test::waitUntil([this]() { return hasStatus("START", CommandStatus::Acked); }));
}

TEST_F(DeviceManagerTest, AModeChangeWhileRunningIsNackedAndChangesNothing) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->sendSetMode(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));

    ASSERT_TRUE(manager_->sendSetMode(2));

    // 门禁的权威在设备侧：运行中改模式会被拒，界面上要看得见原因，而不是静默无效。
    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return hasNack("SET_MODE", mc::NackReason::IllegalState); }));
    // 被拒 = 没有副作用。这一条才是"拒绝"的实质，光看日志文字证明不了。
    EXPECT_EQ(device()->runMode(), mc::SimRunMode::Velocity);
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Running);
}

TEST_F(DeviceManagerTest, StopIsUnidirectionalAndReallyReachesTheDevice) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->sendSetMode(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));

    ASSERT_TRUE(manager_->sendStop());

    // STOP 在协议上就没有应答（ADR-004）：记录必须是"单向"，绝不能停在"等待应答"上
    // —— 那等于把协议设计当成了丢包。
    EXPECT_TRUE(hasStatus("STOP", CommandStatus::Unidirectional));
    EXPECT_FALSE(hasStatus("STOP", CommandStatus::Awaiting));
    // 没有应答可等，所以"到没到"只能看设备有没有真的动：离开 Running 才是证据。
    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() != mc::DeviceState::Running; }));
}

TEST_F(DeviceManagerTest, DownlinkFailureSettlesAMotionCommandAsTimedOut) {
    ASSERT_TRUE(connectAndWaitReady());

    // 上位机 → 设备全丢：命令发得出去、回不来，正是"未确认的运动指令"这个场景。
    manager_->injectDownlinkDrop(true);
    ASSERT_TRUE(manager_->sendSetTarget(120.0));

    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return hasStatus("SET_TARGET", CommandStatus::TimedOut); }));
    // ADR-003：不会自作主张重发运动指令 —— 日志里只有这一条，没有第二条 SET_TARGET。
    std::size_t count = 0U;
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        if (record.name == "SET_TARGET") {
            ++count;
        }
    }
    EXPECT_EQ(count, 1U);
}

TEST_F(DeviceManagerTest, UplinkFailureTearsDownTheSessionAndItReconnectsByItself) {
    ASSERT_TRUE(connectAndWaitReady());
    const std::uint64_t roundsBefore = device()->handshakeRounds();

    // 设备 → 上位机全丢：心跳全部不收应答，链路监控判失效，会话被作废。
    manager_->injectUplinkDrop(true);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return !device()->sessionReady(); }));
    EXPECT_TRUE(manager_->isConnected());   // 传输还开着：失效的是链路质量，不是端口
    EXPECT_FALSE(manager_->alarms().isEmpty());

    // 恢复上行后不需要人工点任何东西：退避到点就重开一轮握手。
    manager_->injectUplinkDrop(false);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return device()->sessionReady(); }));
    EXPECT_GT(device()->handshakeRounds(), roundsBefore);
}

TEST_F(DeviceManagerTest, DisconnectSettlesAnInFlightCommandAsTimedOut) {
    ASSERT_TRUE(connectAndWaitReady());

    ASSERT_TRUE(manager_->sendSetTarget(150.0));
    ASSERT_TRUE(hasStatus("SET_TARGET", CommandStatus::Awaiting));

    // 两次事件循环之间没有 tick，所以这条命令确实还在途 —— 不是"碰巧已经结算完了"。
    manager_->disconnectDevice();

    EXPECT_FALSE(manager_->isConnected());
    // 断开时不能假设"它大概没生效"：一条未确认的运动指令必须按超时结清（ADR-003）。
    EXPECT_TRUE(hasStatus("SET_TARGET", CommandStatus::TimedOut));
    EXPECT_FALSE(hasStatus("SET_TARGET", CommandStatus::Awaiting));
}

TEST_F(DeviceManagerTest, TelemetryIsBufferedAndClearingItKeepsTheCounters) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return manager_->telemetry().size() > 50U; }));

    const std::uint64_t acceptedBefore = device()->telemetryAcceptedTotal();
    const std::uint64_t discardedBefore = manager_->telemetry().discarded();

    manager_->clearTelemetry();

    EXPECT_EQ(manager_->telemetry().size(), 0U);
    // "清屏"只清元素。把"因覆盖丢弃"的计数一起抹掉，等于自己把证据删了。
    EXPECT_EQ(manager_->telemetry().discarded(), discardedBefore);
    EXPECT_EQ(device()->telemetryAcceptedTotal(), acceptedBefore);
}

TEST_F(DeviceManagerTest, BufferedTelemetryCarriesTheDeviceValuesNotZeros) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->sendSetMode(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(manager_->sendSetTarget(200.0));
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return device()->velocityDegPerSec() > 50.0F; }));
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return manager_->telemetry().size() > 20U; }));

    const mchost::TelemetrySample& newest = manager_->telemetry().at(manager_->telemetry().size() - 1U);

    // 缓冲里存的必须是解出来的真实字段，而不是"结构体建了但没填"的一片 0。
    EXPECT_GT(newest.velocityDegPerSec, 50.0F);
    EXPECT_GT(newest.tMs, 0.0);
    EXPECT_EQ(newest.faultCode, 0U);
}

}  // namespace
