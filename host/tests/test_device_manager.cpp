#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <QTemporaryDir>

#include "mc/audit.hpp"
#include "mc/device_simulator.hpp"
#include "mc/device_state.hpp"
#include "mc/interlock.hpp"
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
using mchost::HistoryStore;
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

mc::Recipe makeRecipe() {
    mc::Recipe recipe;
    recipe.name = "WAFER-A1";
    recipe.version = 1U;
    recipe.mode = 1U;
    recipe.target = 200.0F;
    recipe.kp = 1.6F;
    recipe.ki = 0.4F;
    recipe.kd = 0.02F;
    return recipe;
}

mc::SequenceStep makeStep(mc::SequenceStepKind kind) {
    mc::SequenceStep step;
    step.kind = kind;
    return step;
}

// 一部会被设备拒掉的程序：连发两次启动。
// 第二次 START 落在 Running 上，设备侧状态机判它"状态非法"（SET_MODE 与 START
// 都只在 Ready 放行），于是整部程序必须停在第 3 步。
mc::SequenceProgram makeProgramRefusedAtThirdStep() {
    mc::SequenceProgram program;
    program.name = "TASK-REFUSED";
    program.version = 1U;

    mc::SequenceStep mode = makeStep(mc::SequenceStepKind::SetMode);
    mode.mode = 1U;
    mc::SequenceStep start = makeStep(mc::SequenceStepKind::Start);

    program.steps = {mode, start, start};
    return program;
}

// 一部能跑完的程序：设模式 → 设目标 → 启动 → 等一会儿 → 停 → 等设备真的停下。
//
// 最后那一步不是凑数：STOP 被接受 ≠ 设备已经停下 —— 设备收到 STOP 之后先进
// Stopping 把速度收到零才回 Ready，而 SET_MODE 与 START 都只在 Ready 放行。
// 少了这一步，程序"跑完了"之后设备还在减速，下一部程序的第一条命令会被拒。
mc::SequenceProgram makeProgramThatSucceeds() {
    mc::SequenceProgram program;
    program.name = "TASK-OK";
    program.version = 1U;

    mc::SequenceStep mode = makeStep(mc::SequenceStepKind::SetMode);
    mode.mode = 1U;
    mc::SequenceStep target = makeStep(mc::SequenceStepKind::SetTarget);
    target.target = 200.0F;
    mc::SequenceStep start = makeStep(mc::SequenceStepKind::Start);
    mc::SequenceStep wait = makeStep(mc::SequenceStepKind::Wait);
    wait.waitMs = 200U;
    mc::SequenceStep stop = makeStep(mc::SequenceStepKind::Stop);
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 5000U;
    settle.note = "等它真的停下来";

    program.steps = {mode, target, start, wait, stop, settle};
    return program;
}

class DeviceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<DeviceManager>();
        // 历史库改到临时目录：这些用例会真的连接设备、真的产生遥测，
        // 走默认路径的话，跑一次 ctest 就往 $HOME 里塞一份"像真实运行记录"的库，
        // 而且**越跑越大**（每次运行都在同一个文件上接着写）。
        // 入口里已经把 QStandardPaths 切到测试模式，这里再压一层：
        // 用例产生的数据不出临时目录。
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        ASSERT_TRUE(manager_->openHistory(tempDir_->path() + QStringLiteral("/history.db")))
            << manager_->history()->error().toStdString();
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

    // 让轴先离开零点，再停在 Ready（回零只在 Ready 放行）。
    // minDeg 是"至少走多远"：原点就在脚下的回零什么都测不出来，
    // 而"刚到 50 deg/s 就停"这类写法会让轴其实几乎没动 —— 后面的用例就变成了空转。
    bool moveAwayFromZeroAndSettle(double minDeg) {
        if (!selectModeAndWait(1)) {
            return false;
        }
        if (!manager_->sendStart()) {
            return false;
        }
        if (!manager_->sendSetTarget(120.0)) {
            return false;
        }
        if (!mchost_test::waitUntil([this, minDeg]() {
                return std::fabs(device()->positionDeg()) >= minDeg;
            })) {
            return false;
        }
        if (!manager_->sendStop()) {
            return false;
        }
        return mchost_test::waitUntil(
            [this]() { return device()->deviceState() == mc::DeviceState::Ready; });
    }

    // 启动的前置条件里有一条是"设备此刻停在某个控制环上"（联锁 MODE_SELECTED）。
    // 下发 SET_MODE 是**异步**的：命令发出去 ≠ 设备已经改完，这中间设备还是 Stopped，
    // 此刻按启动会被联锁拦下。所以用例要等到设备真的换了环 —— 现场也是这样：
    // 没真换到环上，START 必被设备拒。
    bool selectModeAndWait(int mode) {
        if (!manager_->sendSetMode(mode)) {
            return false;
        }
        return mchost_test::waitUntil([this, mode]() {
            return static_cast<int>(device()->runMode()) == mode;
        });
    }

    // 按代码取一条联锁的判定结果。**不用整条 startBlockReason() 去断言** ——
    // 门禁关着可能只是因为别的规则（比如链路早断了），那样断言就变成在验别的规则了。
    // 返回值（而不是指针）：interlockView() 每次调用都新建一个 vector，
    // 拿着里面的指针出了这条语句就是悬垂引用。
    std::optional<DeviceManager::InterlockView> interlockRow(std::uint16_t code) const {
        for (const DeviceManager::InterlockView& row : manager_->interlockView()) {
            if (row.code == code) {
                return row;
            }
        }
        return std::nullopt;
    }

    std::unique_ptr<DeviceManager> manager_;
    std::unique_ptr<QTemporaryDir> tempDir_;
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

    ASSERT_TRUE(selectModeAndWait(1));
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
    ASSERT_TRUE(selectModeAndWait(1));
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
    ASSERT_TRUE(selectModeAndWait(1));
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
    // 每 1 字节丢 1 = 全丢。
    manager_->setDownlinkDropEveryNthByte(1);
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
    manager_->setUplinkDropEveryNthByte(1);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return !device()->sessionReady(); }));
    EXPECT_TRUE(manager_->isConnected());   // 传输还开着：失效的是链路质量，不是端口
    EXPECT_GT(manager_->alarms().entries().size(), 0U);

    // 恢复上行后不需要人工点任何东西：退避到点就重开一轮握手。
    manager_->setUplinkDropEveryNthByte(0);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return device()->sessionReady(); }));
    EXPECT_GT(device()->handshakeRounds(), roundsBefore);
}

// 报警要真的落进库里，而且要带着**代码与等级**，不能只剩一句人话。
//
// 这就是 alarm 表单独存在的理由：只把一句文本写进事件流水的话，
// "这一段里有多少条 Error 以上、确认了没有"事后再也查不出来。
TEST_F(DeviceManagerTest, AlarmsArePersistedWithTheirCodeLevelAndAcknowledgementState) {
    ASSERT_TRUE(connectAndWaitReady());

    // 触发路径是真实的：上行全丢 → 链路监控判失效 → 会话作废 → 报警。
    // 判据要指向**那一条**，不能是"清单非空"：全丢上行时最先来的是心跳漏拍
    // （Warning，不拦启动），拿它当判据会在链路失效之前就往下走。
    manager_->setUplinkDropEveryNthByte(1);
    const auto hasLinkLost = [this]() {
        for (const mc::AlarmEntry& entry : manager_->alarms().entries()) {
            if (entry.code == mc::kAlarmCodeLinkLost) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(mchost_test::waitUntil(hasLinkLost)) << "上行全丢之后没等到链路失效报警";

    HistoryStore* store = manager_->history();
    ASSERT_NE(store, nullptr);
    const qint64 sessionId = store->sessionId();
    ASSERT_GT(sessionId, 0) << "连接时应当已经开过一次会话";

    const auto findRow = [this, store, sessionId](std::uint16_t code) {
        QString error;
        for (const HistoryStore::AlarmRow& row : store->alarms(sessionId, error)) {
            if (row.code == static_cast<int>(code)) {
                return row;
            }
        }
        return HistoryStore::AlarmRow{};
    };

    const HistoryStore::AlarmRow row = findRow(mc::kAlarmCodeLinkLost);
    // 查得到，且**等级是 Error**：等级不是显示效果，它是"要不要确认、拦不拦启动"的依据，
    // 所以它必须与代码一起被保存下来。
    // 失败时把库里实际有的列出来：只报"没找到"会让人再跑一轮去查到底有什么。
    const auto describeAlarms = [store, sessionId]() {
        QString error;
        QString text;
        for (const HistoryStore::AlarmRow& r : store->alarms(sessionId, error)) {
            text += QStringLiteral("[0x%1/L%2/x%3] ")
                        .arg(r.code, 4, 16, QLatin1Char('0'))
                        .arg(r.level)
                        .arg(r.occurrences);
        }
        return text.toStdString();
    };
    ASSERT_EQ(row.code, static_cast<int>(mc::kAlarmCodeLinkLost))
        << "链路失效这条报警没有落进 alarm 表；库里实际有：" << describeAlarms();
    EXPECT_EQ(row.level, static_cast<int>(mc::AlarmLevel::Error));
    EXPECT_GT(row.occurrences, 0);
    EXPECT_FALSE(row.acknowledged);
    // 未确认的 Error 报警拦启动 —— 这是它被保存下来的**用途**，
    // 所以在这里一并钉住：门禁关着，而且是**这一条**在拦。
    // 只断言"门禁关着"是不够的：链路这会儿本来就是失效状态，别的原因也能关掉它。
    EXPECT_FALSE(manager_->startBlockReason().isEmpty());
    const auto alarmRule = interlockRow(mc::kInterlockNoUnacknowledgedAlarm);
    ASSERT_TRUE(alarmRule.has_value());
    EXPECT_FALSE(alarmRule->satisfied);

    // 人工确认之后，库里那一行也要跟着变成"已确认"。
    // 只改内存不落库的话，重启之后从库里读回来的还是"未确认"。
    manager_->acknowledgeAllAlarms();
    const auto afterRule = interlockRow(mc::kInterlockNoUnacknowledgedAlarm);
    ASSERT_TRUE(afterRule.has_value());
    EXPECT_TRUE(afterRule->satisfied) << "确认了报警，这一条规则还没放开";
    const HistoryStore::AlarmRow after = findRow(mc::kAlarmCodeLinkLost);
    EXPECT_TRUE(after.acknowledged);
    EXPECT_GT(after.ackWallMs, 0);

    manager_->setUplinkDropEveryNthByte(0);
}

// 一部程序跑到底：每一步都必须拿到"已确认"才轮到下一步。
TEST_F(DeviceManagerTest, AProgramRunsEveryStepInOrderAndOnlyAfterEachIsAcknowledged) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->startSequence(makeProgramThatSucceeds()))
        << manager_->sequenceStartError().toStdString();

    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Running);

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->sequencer().status().phase == mc::SequencePhase::Succeeded;
    })) << "程序没跑完： " << manager_->sequencer().status().detail;

    // 六步各留一条"已确认"的流水 —— 这是"整部程序确实按顺序跑完了"的证据。
    const std::vector<mc::StepReport>& reports = manager_->sequenceReports();
    ASSERT_EQ(reports.size(), std::size_t{6});
    const mc::SequenceStepKind expected[] = {
        mc::SequenceStepKind::SetMode,   mc::SequenceStepKind::SetTarget,
        mc::SequenceStepKind::Start,     mc::SequenceStepKind::Wait,
        mc::SequenceStepKind::Stop,      mc::SequenceStepKind::WaitReady,
    };
    for (std::size_t i = 0U; i < reports.size(); ++i) {
        EXPECT_EQ(reports[i].index, i);
        EXPECT_EQ(reports[i].kind, expected[i]);
        EXPECT_EQ(reports[i].outcome, mc::StepOutcome::Acked);
    }
    EXPECT_EQ(manager_->sequencer().status().completedSteps, std::size_t{6});

    // 跑完之后设备**真的**停着 —— 不是"STOP 发出去了"而已。最后那一步等的就是它。
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Ready);
    EXPECT_FALSE(device()->enabled());
}

// 某一步没拿到确认 → 整部程序**当场停在那一步**，而且真正的理由要说出来。
// 用的是真实的设备侧门禁（第二次 START 落在 Running 上会被拒），不是造一个假结局。
TEST_F(DeviceManagerTest, AProgramStopsAtTheFirstStepTheDeviceRefusesAndSaysWhy) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->startSequence(makeProgramRefusedAtThirdStep()))
        << manager_->sequenceStartError().toStdString();

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->sequencer().status().phase == mc::SequencePhase::Failed;
    })) << "程序没停在被拒的那一步上";

    const mc::SequenceStatus& status = manager_->sequencer().status();
    EXPECT_EQ(status.stepIndex, std::size_t{2}) << "应当停在第 3 步（第二次 START）";
    EXPECT_EQ(status.completedSteps, std::size_t{2});
    // 停下来的原因里要带上**设备给的**理由和是哪一步，而不是一句"程序失败了"。
    EXPECT_NE(status.detail.find("START"), std::string::npos) << status.detail;
    EXPECT_NE(status.detail.find("不允许"), std::string::npos) << status.detail;
    EXPECT_FALSE(status.waitingOutcome);
    EXPECT_FALSE(manager_->sequencer().awaitingDispatch()) << "停在原地，不许往下走";

    // 第 3 步那一条流水带着"被拒"的结局。
    const std::vector<mc::StepReport>& reports = manager_->sequenceReports();
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.back().outcome, mc::StepOutcome::Nacked);
    EXPECT_EQ(reports.back().index, std::size_t{2});
}

// 未确认的报警拦启动这条规则对**自动流程**同样有效：它不该绕过人手按启动时的闸门。
// 同时，判据必须说得出理由，而不是只返回一个 false。
TEST_F(DeviceManagerTest, AProgramIsRefusedWhileAnUnacknowledgedAlarmIsBlockingStart) {
    ASSERT_TRUE(connectAndWaitReady());

    // 上行全丢 → 链路失效（Error，需要确认）→ 启动闸门关上。
    manager_->setUplinkDropEveryNthByte(1);
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        for (const mc::AlarmEntry& entry : manager_->alarms().entries()) {
            if (entry.code == mc::kAlarmCodeLinkLost) {
                return true;
            }
        }
        return false;
    }));
    manager_->setUplinkDropEveryNthByte(0);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return device()->sessionReady(); }));
    ASSERT_FALSE(manager_->startBlockReason().isEmpty());

    EXPECT_FALSE(manager_->startSequence(makeProgramThatSucceeds()));
    // 理由里要有"有几条报警没确认"和"该去哪儿处理"，操作员才知道下一步干什么。
    const std::string reason = manager_->sequenceStartError().toStdString();
    EXPECT_NE(reason.find("没确认"), std::string::npos) << reason;
    EXPECT_NE(reason.find("报警"), std::string::npos) << reason;
    EXPECT_NE(reason.find("故障报警"), std::string::npos)
        << "只说「不许」，没说「去哪儿」，等于把问题丢回给操作员：" << reason;
    // 而且**一条都没发出去**：拒在本地，而不是让设备去拒。
    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Idle);
    EXPECT_EQ(manager_->sequencer().status().completedSteps, std::size_t{0});
    EXPECT_FALSE(hasStatus("SET_MODE", CommandStatus::Acked)) << "被拒的程序不该发出任何命令";

    // 确认之后就能跑了 —— 门禁不是"永久拉黑"。
    // 这里盯的是**报警那一条规则**放开了，而不是整条门禁：设备这会儿还没选环
    // （选环是程序第一步要做的事），拿整条门禁当判据会验到别的规则上去。
    manager_->acknowledgeAllAlarms();
    const auto alarmRule = interlockRow(mc::kInterlockNoUnacknowledgedAlarm);
    ASSERT_TRUE(alarmRule.has_value());
    EXPECT_TRUE(alarmRule->satisfied);
    EXPECT_TRUE(manager_->startSequence(makeProgramThatSucceeds()))
        << manager_->sequenceStartError().toStdString();
    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Running);
}

// 联锁表本身要接对线：七条规则一条不少、顺序就是重要性顺序、
// 每条都带着一句"现在这条对这台设备意味着什么"，而且判定真的跟着现场变。
TEST_F(DeviceManagerTest, TheInterlockTableListsEveryRuleAndCurrentsItsJudgement) {
    const std::vector<mc::InterlockRule>& rules = mc::interlockRules();

    // 没连设备：现场是"什么都没开、什么都不知道"。链路/会话/设备状态这几条因此判不满足；
    // 而"没有未确认的报警""没有故障锁存"判的是**有没有**，没有就成立 ——
    // 快照只如实填现场、不做判断，所以这里能一条条对得上。
    const std::vector<DeviceManager::InterlockView> down = manager_->interlockView();
    ASSERT_EQ(down.size(), rules.size());
    for (std::size_t i = 0U; i < rules.size(); ++i) {
        EXPECT_EQ(down[i].code, rules[i].code) << "第 " << i << " 条的顺序不对";
        EXPECT_EQ(down[i].severity, rules[i].severity);
        EXPECT_FALSE(down[i].text.isEmpty()) << "第 " << i << " 条没有说明";
    }
    const auto unmet = [this](std::uint16_t code) {
        const auto row = interlockRow(code);
        EXPECT_TRUE(row.has_value()) << "联锁表里没有代码 " << code;
        return row.has_value() && !row->satisfied;
    };
    EXPECT_TRUE(unmet(mc::kInterlockLinkUp));
    EXPECT_TRUE(unmet(mc::kInterlockSessionReady));
    EXPECT_TRUE(unmet(mc::kInterlockDeviceReady));
    EXPECT_FALSE(unmet(mc::kInterlockNoUnacknowledgedAlarm))
        << "还没连设备，不代表有未确认的报警";
    EXPECT_FALSE(unmet(mc::kInterlockNoFaultLatch))
        << "还没连设备，不代表有故障锁存";
    const auto telemetry = interlockRow(mc::kInterlockTelemetryLive);
    ASSERT_TRUE(telemetry.has_value());
    EXPECT_EQ(telemetry->severity, mc::InterlockSeverity::Warn)
        << "没收到过遥测只是提示，不该拦启动";

    // 说得出**第一条**没满足的是什么：规则表按重要性排过序，第一条就是现在最该处理的。
    EXPECT_NE(manager_->startBlockReason().toStdString().find("端口没打开"), std::string::npos)
        << manager_->startBlockReason().toStdString();

    ASSERT_TRUE(connectAndWaitReady());
    // 一瞬之间可能还没收到遥测，所以只钉住"与现场直接对应的那几条"。
    for (std::uint16_t code : {mc::kInterlockLinkUp, mc::kInterlockSessionReady,
                               mc::kInterlockDeviceReady, mc::kInterlockNoUnacknowledgedAlarm,
                               mc::kInterlockNoFaultLatch}) {
        const auto row = interlockRow(code);
        ASSERT_TRUE(row.has_value()) << "联锁表里少了代码 " << code;
        EXPECT_TRUE(row->satisfied) << row->text.toStdString();
    }

    // 刚就绪但**还没选环**：这一条必须是不满足的 —— 设备侧同样会把 START 拒掉。
    // 这条时序是用例最容易踩的一格，所以正面钉住它。
    EXPECT_TRUE(unmet(mc::kInterlockModeSelected));
    EXPECT_NE(manager_->startBlockReason().toStdString().find("还没选控制模式"),
              std::string::npos)
        << manager_->startBlockReason().toStdString();
    EXPECT_FALSE(manager_->sendStart()) << "没选环就按启动，联锁该把它拦在本地";
    // 拦在本地 = 一条命令都没发出去。
    EXPECT_FALSE(hasStatus("START", CommandStatus::Awaiting));
    EXPECT_FALSE(hasStatus("START", CommandStatus::Acked));

    // 选环之后这一条当场满足，而且是**设备真的换了环**之后才满足。
    ASSERT_TRUE(selectModeAndWait(1));
    const auto modeAfter = interlockRow(mc::kInterlockModeSelected);
    ASSERT_TRUE(modeAfter.has_value());
    EXPECT_TRUE(modeAfter->satisfied) << modeAfter->text.toStdString();
    EXPECT_TRUE(manager_->startBlockReason().isEmpty());
    EXPECT_TRUE(manager_->sendStart());
}

// 正在跑的时候链路没了：当场停下，而不是停在一个永远等不到结局的"等确认…"上。
TEST_F(DeviceManagerTest, AProgramStopsWhenTheLinkDiesMidRun) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->startSequence(makeProgramThatSucceeds()))
        << manager_->sequenceStartError().toStdString();
    ASSERT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Running);

    // 设备 → 上位机全丢：心跳全部不收应答，链路失效。
    manager_->setUplinkDropEveryNthByte(1);
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->sequencer().status().phase != mc::SequencePhase::Running;
    })) << "链路没了，程序还挂着";

    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Aborted);
    EXPECT_NE(manager_->sequencer().status().detail.find("链路"),
              std::string::npos)
        << manager_->sequencer().status().detail;
    manager_->setUplinkDropEveryNthByte(0);
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
    ASSERT_TRUE(selectModeAndWait(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(manager_->sendSetTarget(200.0));

    // 等**缓冲里最新的那条**达到稳态，而不是等设备的瞬时值过线就走。
    // 设备报的速度是在一个固定窗口上差分出来的（阶段 8 起是 50 ms），
    // 所以它天然滞后真值半个窗口 —— 加速段里读到的必然偏低。
    // 这条滞后是真实设备的性质，不是测试的噪声，因此判据要按稳态写。
    const auto newestBufferedVelocity = [this]() {
        const mchost::TelemetryRing& ring = manager_->telemetry();
        return ring.empty() ? 0.0F : ring.at(ring.size() - 1U).velocityDegPerSec;
    };
    ASSERT_TRUE(mchost_test::waitUntil(
        [this, &newestBufferedVelocity]() {
            return manager_->telemetry().size() > 20U && newestBufferedVelocity() > 150.0F;
        }));

    const mchost::TelemetrySample& newest = manager_->telemetry().at(manager_->telemetry().size() - 1U);

    // 缓冲里存的必须是解出来的真实字段，而不是"结构体建了但没填"的一片 0。
    EXPECT_GT(newest.velocityDegPerSec, 150.0F);
    EXPECT_GT(newest.tMs, 0.0);
    EXPECT_EQ(newest.faultCode, 0U);
}

TEST_F(DeviceManagerTest, TimingDiagnosticsIsOffByDefaultAndRecordsOnlyWhenAsked) {
    // 默认关闭：产品路径上一个样本都不该有。诊断本身要读两次单调时钟，
    // 常开等于让被测对象替我们分担测量误差。
    EXPECT_FALSE(manager_->timingDiagnosticsEnabled());
    EXPECT_EQ(manager_->timingSampleCount(), 0U);

    ASSERT_TRUE(connectAndWaitReady());
    mchost_test::pumpFor(200);
    EXPECT_EQ(manager_->timingSampleCount(), 0U) << "没开诊断却记了样本";
    EXPECT_TRUE(manager_->tickBusyUs().isEmpty());

    // 容量 16：拍数超过它之后，样本数必须**封顶**而不是无界增长。
    manager_->enableTimingDiagnostics(true, 16U);
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return manager_->timingSampleCount() >= 16U; }));
    mchost_test::pumpFor(200);
    EXPECT_EQ(manager_->timingSampleCount(), 16U);
    EXPECT_EQ(manager_->tickBusyUs().size(), 16);
    EXPECT_EQ(manager_->tickIntervalUs().size(), 16);
    EXPECT_GT(manager_->ticks(), 20U) << "拍数确实超过了容量，否则封顶这件事根本没被验到";

    // 拍间隔样本得是"一拍"的量级（kTickIntervalMs = 10 ms）：
    // 定时器真的在按 10 ms 跑，而不是量到了一堆 0 或一堆毫秒以外的数。
    EXPECT_GE(manager_->tickIntervalUs().at(0), 5000);
    EXPECT_GT(manager_->tickBusyUs().at(0), 0);

    // 关掉之后必须真的不再写：不是"开关是假的、一直在记"。
    manager_->enableTimingDiagnostics(false);
    EXPECT_TRUE(manager_->tickBusyUs().isEmpty());
    EXPECT_EQ(manager_->timingSampleCount(), 0U);
    mchost_test::pumpFor(100);
    EXPECT_EQ(manager_->timingSampleCount(), 0U);
}

// --- 配方下装（P0）-----------------------------------------------------------
//
// 下装是三条命令的编排，所以用例要盯三件事：**顺序**、**证据分层**、**一条都不多发**。

TEST_F(DeviceManagerTest, DownloadingARecipeSendsThreeCommandsInOrderAndVerifiesTheReadBack) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->openRecipes(tempDir_->path() + QStringLiteral("/recipes.db")))
        << manager_->recipes()->error().toStdString();

    ASSERT_TRUE(manager_->sendRecipe(makeRecipe()));
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->recipeDownload().phase == mchost::RecipeDownloadPhase::Succeeded;
    })) << manager_->recipeDownload().detail.toStdString();

    EXPECT_EQ(manager_->recipeDownload().commandsAcked, 3);

    // 三条配置命令 + 一条回读，都真的走了协议，而不是"界面说成功"。
    EXPECT_TRUE(hasStatus("SET_MODE", CommandStatus::Acked));
    EXPECT_TRUE(hasStatus("SET_PID", CommandStatus::Acked));
    EXPECT_TRUE(hasStatus("SET_TARGET", CommandStatus::Acked));
    EXPECT_TRUE(hasStatus("READ_PARAMS", CommandStatus::Acked));
    // 回读是真读到了东西：设备回了至少一份可用的参数，且没有坏数据。
    EXPECT_GT(device()->paramStatesReceivedTotal(), 0U);
    EXPECT_EQ(device()->paramStatesRejectedTotal(), 0U);

    // 顺序：seq 单调递增，比 seq 就是比下发次序。SET_MODE 必须在前 ——
    // 它决定 target 的**单位语义**，先发目标值的话，那段时间里它是什么意思说不清。
    std::uint16_t seqMode = 0U;
    std::uint16_t seqPid = 0U;
    std::uint16_t seqTarget = 0U;
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        if (record.name == "SET_MODE" && seqMode == 0U) {
            seqMode = record.seq;
        }
        if (record.name == "SET_PID" && seqPid == 0U) {
            seqPid = record.seq;
        }
        if (record.name == "SET_TARGET" && seqTarget == 0U) {
            seqTarget = record.seq;
        }
    }
    ASSERT_NE(seqMode, 0U);
    ASSERT_NE(seqPid, 0U);
    ASSERT_NE(seqTarget, 0U);
    EXPECT_LT(seqMode, seqPid);
    EXPECT_LT(seqPid, seqTarget);

    // 设备侧真的存下了那个目标值 —— 这才是"下装生效"的实质，不是日志里那三行字。
    EXPECT_FLOAT_EQ(device()->deviceTarget(), 200.0F);
}

TEST_F(DeviceManagerTest, ARecipeDownloadWhileRunningIsRefusedWithAReasonAndSendsNothing) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(selectModeAndWait(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));

    EXPECT_FALSE(manager_->sendRecipe(makeRecipe()));
    EXPECT_EQ(manager_->recipeDownload().phase, mchost::RecipeDownloadPhase::Failed);
    // 理由要能落成一句可执行的话。设备在 Running 时 SET_MODE 会被拒，所以这里
    // 该说的是"必须停在 Ready"，而不是笼统的"下装失败"。
    EXPECT_TRUE(manager_->recipeDownload().detail.contains(QStringLiteral("Ready")))
        << manager_->recipeDownload().detail.toStdString();

    // 一条命令都没发出去。最有力的证据是 SET_PID：它在下装序列里排第二，
    // 它出现了就说明第一条确实发过了。
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        EXPECT_NE(record.name, "SET_PID");
    }
}

TEST_F(DeviceManagerTest, AnInvalidRecipeIsRefusedBeforeAnythingGoesOut) {
    ASSERT_TRUE(connectAndWaitReady());

    mc::Recipe bad = makeRecipe();
    bad.name.clear();

    EXPECT_FALSE(manager_->sendRecipe(bad));
    EXPECT_EQ(manager_->recipeDownload().phase, mchost::RecipeDownloadPhase::Failed);
    EXPECT_TRUE(manager_->recipeDownload().detail.contains(QStringLiteral("名称不能为空")))
        << manager_->recipeDownload().detail.toStdString();
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        EXPECT_NE(record.name, "SET_PID");
    }
}

TEST_F(DeviceManagerTest, ALostLinkSettlesAnInFlightRecipeDownloadAsFailed) {
    ASSERT_TRUE(connectAndWaitReady());
    manager_->setDownlinkDropEveryNthByte(1);   // 上位机 → 设备全丢

    ASSERT_TRUE(manager_->sendRecipe(makeRecipe()));
    EXPECT_TRUE(manager_->recipeDownload().phase == mchost::RecipeDownloadPhase::Sending ||
                manager_->recipeDownload().phase == mchost::RecipeDownloadPhase::Failed);

    // 不能在"等确认…"上停住：链路没了，这次下装已经不可能完成。
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->recipeDownload().phase == mchost::RecipeDownloadPhase::Failed;
    })) << manager_->recipeDownload().detail.toStdString();
    EXPECT_LT(manager_->recipeDownload().commandsAcked, 3);
    EXPECT_FALSE(manager_->recipeDownload().detail.isEmpty());
}

// --- 操作员权限与操作审计（P1）------------------------------------------------
//
// 判据分两层，缺一层都不算验到：
//   ① **设备侧一个字节都没动**（权限不该只是"界面显示了一句拒绝"）；
//   ② 流水里留下了"谁、什么时候、想做什么、被拒的理由"。

TEST_F(DeviceManagerTest, TheViewerCanWatchButCannotMoveAnything) {
    ASSERT_TRUE(connectAndWaitReady());
    // 先确认"这台设备本来是能被开动的"（否则下面的拒绝可能只是因为别的原因）。
    ASSERT_TRUE(selectModeAndWait(1));

    ASSERT_TRUE(manager_->setOperatorRole(mc::OperatorRole::Viewer));

    EXPECT_FALSE(manager_->sendStart());
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("观察者"), std::string::npos)
        << manager_->lastRefusalText().toStdString();
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("运动控制"), std::string::npos)
        << "理由里要写清缺的是哪一项权限";
    EXPECT_FALSE(manager_->sendSetTarget(100.0));
    EXPECT_FALSE(manager_->sendStop());
    EXPECT_FALSE(manager_->sendSetPid(1.0, 0.0, 0.0));

    // ① **一个字节都没发出去**：命令日志里一条 START / SET_TARGET / STOP 都没有。
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        EXPECT_NE(record.name, "START");
        EXPECT_NE(record.name, "SET_TARGET");
        EXPECT_NE(record.name, "STOP");
    }
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Ready);
    EXPECT_FALSE(device()->enabled());

    // 连"换个数据源"这种不碰设备的事也不行 —— 那是配置连接的权限。
    EXPECT_FALSE(manager_->useSimulatedDevice());
    EXPECT_TRUE(manager_->isConnected()) << "被拒的操作不该顺带把链路断开";

    // 观察者**也不能把自己升上去**：这是权限模型里最容易漏的一格。
    EXPECT_FALSE(manager_->setOperatorRole(mc::OperatorRole::Engineer));
    EXPECT_EQ(manager_->operatorRole(), mc::OperatorRole::Viewer);

    // ② 流水里留了 6 条"未执行"：启动、目标、停止、PID、换数据源、以及把自己升上去。
    // 前面那三条（连接、选模式、改角色）是"已执行"。
    EXPECT_EQ(manager_->audit().notDispatchedTotal(), 6U);
    EXPECT_GE(manager_->audit().dispatchedTotal(), 3U);
    const mc::AuditEntry* latest = manager_->audit().latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->action, mc::OperatorAction::SetRole);
    EXPECT_FALSE(latest->dispatched);
    EXPECT_EQ(latest->role, mc::OperatorRole::Viewer);
}

TEST_F(DeviceManagerTest, TheOperatorRunsTheMachineButDoesNotRedefineHowItRuns) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(manager_->setOperatorRole(mc::OperatorRole::Operator));

    // 现场动作：可以。
    EXPECT_TRUE(selectModeAndWait(1));
    EXPECT_TRUE(manager_->sendSetTarget(100.0));
    EXPECT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));
    EXPECT_TRUE(manager_->sendStop());

    // 改定义的动作：都是"没发生"，而且理由里点明了缺哪项权限。
    EXPECT_FALSE(manager_->sendSetPid(2.0, 0.5, 0.0));
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("改设备参数"), std::string::npos)
        << manager_->lastRefusalText().toStdString();
    for (const mchost::CommandRecord& record : device()->commandLog()) {
        EXPECT_NE(record.name, "SET_PID") << "被拒的 PID 不该出现在链路上";
    }

    // 库的写操作同样收在权限里：操作员能下装配方，但不能改配方内容。
    ASSERT_TRUE(manager_->openRecipes(tempDir_->path() + QStringLiteral("/recipes.db")));
    qint64 id = 0;
    QString error;
    EXPECT_FALSE(manager_->saveRecipe(makeRecipe(), &id, error));
    EXPECT_NE(error.toStdString().find("编辑配方"), std::string::npos) << error.toStdString();
    EXPECT_EQ(id, 0) << "被拒的保存不该留下 id";

    // 清空报警清单是"抹掉证据"，也不在操作员手里。
    EXPECT_FALSE(manager_->clearAlarms());
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("清空报警清单"), std::string::npos);
}

TEST_F(DeviceManagerTest, WhatHappenedAndWhatDidNotBothReachTheAuditTrailAndTheDatabase) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(selectModeAndWait(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));

    // 一次成功的操作 → 一条"已执行"；一次被拒的操作 → 一条"未执行 + 理由"。
    ASSERT_TRUE(manager_->setOperatorRole(mc::OperatorRole::Viewer));
    EXPECT_FALSE(manager_->sendStart());

    const mc::AuditLog& log = manager_->audit();
    EXPECT_GE(log.recordedTotal(), 4U) << "连接、选模式、启动、改角色、被拒的启动都该有记录";
    EXPECT_GE(log.dispatchedTotal(), 1U);
    EXPECT_EQ(log.notDispatchedTotal(), 1U);

    // 最新的那条就是被拒的 START，理由不是空的。
    const mc::AuditEntry* latest = log.latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->action, mc::OperatorAction::Start);
    EXPECT_FALSE(latest->dispatched);
    EXPECT_FALSE(latest->reason.empty());
    EXPECT_GT(latest->wallMs, 0U) << "流水要看的是墙上时钟：会话时刻重启之后就没意义了";

    // 库里也有一份，而且是**同一件事**（不是另记一套）。
    HistoryStore* store = manager_->history();
    ASSERT_NE(store, nullptr);
    QString error;
    const std::vector<HistoryStore::AuditRow> rows = store->audits(0, error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0].action, static_cast<int>(mc::OperatorAction::Start));
    EXPECT_FALSE(rows[0].dispatched);
    EXPECT_EQ(rows[0].role, static_cast<int>(mc::OperatorRole::Viewer));
    EXPECT_FALSE(rows[0].reason.isEmpty());
    // 库里总条数 ≥ 内存里记的条数：内存那本有窗口上限，库没有。
    EXPECT_GE(rows.size(), log.recordedTotal());
}

TEST_F(DeviceManagerTest, TheRefusalTextSaysWhichOfTheThreeKindsItIs) {
    // 三类拒绝的**处置完全不同**（去改角色 / 去连接 / 去处理联锁），
    // 所以它们必须说得出是哪一类，而不是一句"操作失败"。
    EXPECT_FALSE(manager_->sendStart());
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("还没连上设备"), std::string::npos)
        << manager_->lastRefusalText().toStdString();

    ASSERT_TRUE(connectAndWaitReady());
    // 连上了但没选环：这一条由联锁表说（"还没选控制模式"）。
    EXPECT_FALSE(manager_->sendStart());
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("还没选控制模式"), std::string::npos)
        << manager_->lastRefusalText().toStdString();

    // 没权限：理由里点名角色与缺的权限。
    ASSERT_TRUE(manager_->setOperatorRole(mc::OperatorRole::Viewer));
    EXPECT_FALSE(manager_->sendStart());
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("没有下发到设备"), std::string::npos)
        << manager_->lastRefusalText().toStdString();
}

// ---------------------------------------------------------------------------
// 回零（P1-6）：应答说"开始了"，**状态**说"成没成"
// ---------------------------------------------------------------------------

TEST_F(DeviceManagerTest, HomingReachesSucceededOnlyWhenTheDeviceComesBackToReady) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(moveAwayFromZeroAndSettle(15.0)) << "前提：轴要真的离开零点，回零才有行程可走";

    ASSERT_TRUE(manager_->sendHome(mc::HomeParam{60.0F, 5000U}));
    // 刚发出去时**不能**说成功：应答只说明回零开始了。
    EXPECT_EQ(manager_->homeStatus().phase, mchost::HomePhase::Started);
    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return hasStatus("HOME", CommandStatus::Acked); }));

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->homeStatus().phase == mchost::HomePhase::Succeeded;
    })) << manager_->homeStatus().detail.toStdString();

    // 零点建立在这里：位置被搬到了 0，设备停在 Ready。
    EXPECT_NEAR(device()->positionDeg(), 0.0F, 1.0F);
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Ready);
    EXPECT_FALSE(manager_->homeStatus().detail.contains(QStringLiteral("没有建立")));
}

// 回零与 START 走**同一道闸**：都在 Ready 才能发。
// 这条若松掉，等于给了一条绕过启动联锁让电机动起来的路径。
TEST_F(DeviceManagerTest, HomingIsRefusedWhileRunningAndLeavesTheDeviceAlone) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(selectModeAndWait(1));
    ASSERT_TRUE(manager_->sendStart());
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Running; }));

    EXPECT_FALSE(manager_->sendHome(mc::HomeParam{60.0F, 5000U}));
    EXPECT_FALSE(manager_->lastRefusalText().isEmpty());
    EXPECT_FALSE(hasStatus("HOME", CommandStatus::Acked)) << "被拦下的命令一条都不该出去";
    EXPECT_EQ(manager_->homeStatus().phase, mchost::HomePhase::Idle);
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Running) << "拒绝不许把设备打停";
}

TEST_F(DeviceManagerTest, AHomeThatCannotReachZeroIsReportedAsFailedAndRaisesAnAlarm) {
    // 时限小到根本来不及：设备会在自己那一侧超时并进 Fault(HomeFailed)。
    // 这一条同时钉住"上位机不许把进 Fault 的回零报成成功"。
    manager_->setDevice(std::make_unique<SimulatedDevice>(mc::SimulatorConfig{},
                                                         fastMonitorConfig()));
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(moveAwayFromZeroAndSettle(15.0));

    ASSERT_TRUE(manager_->sendHome(mc::HomeParam{60.0F, 1U}));

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->homeStatus().phase == mchost::HomePhase::Failed;
    })) << manager_->homeStatus().detail.toStdString();

    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Fault);
    EXPECT_NE(device()->faultCode(), mc::FaultCode::None);
    // "零点没有建立"必须在界面上说得出来 —— 位置没有被搬走就是它的证据。
    EXPECT_GT(std::fabs(device()->positionDeg()), 1.0);
    const std::string failedDetail = manager_->homeStatus().detail.toStdString();
    EXPECT_NE(failedDetail.find("零点"), std::string::npos) << failedDetail;
    EXPECT_NE(failedDetail.find("没"), std::string::npos) << failedDetail;
    // 失败那条还必须把**设备给的**原因带上：只说"失败了"等于让人无从下手。
    EXPECT_NE(failedDetail.find("回零"), std::string::npos) << failedDetail;

    bool sawHomeAlarm = false;
    for (const mc::AlarmEntry& entry : manager_->alarms().entries()) {
        if (entry.code == mc::kAlarmCodeHomeFailed) {
            sawHomeAlarm = true;
        }
    }
    EXPECT_TRUE(sawHomeAlarm) << "回零失败要进报警列表，而不是只改一行状态文字";
}

// 被 STOP 打断的回零：**零点没有建立**，而且这一格必须与"成功"分开报。
// 两者在曲线页上都表现为"停下来了"，含义却完全相反。
TEST_F(DeviceManagerTest, AHomeStoppedByTheOperatorIsReportedAsAbortedNotSucceeded) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(moveAwayFromZeroAndSettle(25.0));

    // 用一个很低的回零速度，好让它在半路上被停住（25 deg 要走 5 s）。
    ASSERT_TRUE(manager_->sendHome(mc::HomeParam{5.0F, 30000U}));
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return device()->deviceState() == mc::DeviceState::Home; }));
    EXPECT_TRUE(manager_->sendStop());

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->homeStatus().phase == mchost::HomePhase::Aborted;
    })) << manager_->homeStatus().detail.toStdString();

    // 措辞可以改，但两件事必须同时出现在那句话里：说的是"零点"，而且**否定的**
    // （成功那一条写的是"零点已建立"，所以"没"这个字是把两者分开的最短判据）。
    const std::string abortedDetail = manager_->homeStatus().detail.toStdString();
    EXPECT_NE(abortedDetail.find("零点"), std::string::npos) << abortedDetail;
    EXPECT_NE(abortedDetail.find("没"), std::string::npos) << abortedDetail;
    EXPECT_GT(std::fabs(device()->positionDeg()), 1.0);
}

// ---------------------------------------------------------------------------
// 软限位与参数回读（P1-6）
// ---------------------------------------------------------------------------

// 回读之前，"设备存的是什么"是**不知道**，不能拿一组 0 当真值用。
TEST_F(DeviceManagerTest, NothingIsKnownAboutTheDeviceStoredParametersBeforeAReadback) {
    ASSERT_TRUE(connectAndWaitReady());

    mc::ParamState state;
    EXPECT_FALSE(manager_->deviceParamState(state)) << "还没回读过就不该有值";
    mc::SoftLimits limits;
    EXPECT_FALSE(manager_->deviceSoftLimits(limits)) << "软限位同理：不知道就说不知道";
}

TEST_F(DeviceManagerTest, SoftLimitsAreAcceptedStoredAndReadBackFromTheDevice) {
    ASSERT_TRUE(connectAndWaitReady());

    const mc::SoftLimits wanted{true, -120.0F, 120.0F};
    ASSERT_TRUE(manager_->sendSoftLimits(wanted));
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return hasStatus("SET_LIMITS", CommandStatus::Acked); }));

    // 上位机记录的是"发出去的东西"，设备的实存值只能回读 —— 两者分开验证。
    ASSERT_TRUE(manager_->requestParamReadback());
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        mc::ParamState read;
        return manager_->deviceParamState(read);
    }));

    mc::ParamState state;
    ASSERT_TRUE(manager_->deviceParamState(state));
    EXPECT_TRUE(state.limits.enabled);
    EXPECT_FLOAT_EQ(state.limits.minDeg, wanted.minDeg);
    EXPECT_FLOAT_EQ(state.limits.maxDeg, wanted.maxDeg);
    // 走的是同一个来源（回读回来的那一组），不是上位机把发出去的值又显示了一遍：
    // deviceSoftLimits 与 deviceParamState 都取自设备的最后一次回读。
    mc::SoftLimits limits{};
    ASSERT_TRUE(manager_->deviceSoftLimits(limits));
    EXPECT_TRUE(limits.enabled);
    EXPECT_FLOAT_EQ(limits.minDeg, wanted.minDeg);
}

// 把**当前位置**关在门外的限位：设备侧明确拒掉，而且一个字节都不落地。
// 上位机这一层还会先拦一道"min 必须小于 max"的结构问题，所以这里用一个结构合法、
// 但会把当前位置关在门外的区间。
TEST_F(DeviceManagerTest, LimitsThatLockOutTheCurrentPositionAreNackedByTheDevice) {
    ASSERT_TRUE(connectAndWaitReady());
    ASSERT_TRUE(moveAwayFromZeroAndSettle(15.0));
    const double here = device()->positionDeg();

    const mc::SoftLimits outside{true, static_cast<float>(here + 5.0),
                                static_cast<float>(here + 60.0)};
    ASSERT_TRUE(manager_->sendSoftLimits(outside));
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return hasNack("SET_LIMITS", mc::NackReason::BadParameter);
    })) << "设备必须明确拒绝，而不是设完之后下一拍自己报超限";

    // 被拒的那一组**一个字节都不该落地**：回读出来还是"未启用"。
    ASSERT_TRUE(manager_->requestParamReadback());
    mc::SoftLimits read{};
    ASSERT_TRUE(mchost_test::waitUntil([this, &read]() {
        return manager_->deviceSoftLimits(read);
    }));
    EXPECT_FALSE(read.enabled) << "被拒的限位不该落地";
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Ready) << "拒绝不该把设备打进故障";
}

// 结构上就说不通的限位在上位机这一层就被拦下 —— 连一条报文都不该发出去。
TEST_F(DeviceManagerTest, StructurallyImpossibleLimitsNeverReachTheWire) {
    ASSERT_TRUE(connectAndWaitReady());

    EXPECT_FALSE(manager_->sendSoftLimits(mc::SoftLimits{true, 50.0F, 10.0F}));
    EXPECT_FALSE(hasStatus("SET_LIMITS", CommandStatus::Acked));
    EXPECT_NE(manager_->lastRefusalText().toStdString().find("最小值必须小于最大值"),
              std::string::npos)
        << manager_->lastRefusalText().toStdString();
}

// ---------------------------------------------------------------------------
// 程序里的回零步
// ---------------------------------------------------------------------------

TEST_F(DeviceManagerTest, AProgramCanHomeAndThenWaitForTheOriginToBeEstablished) {
    ASSERT_TRUE(connectAndWaitReady());

    mc::SequenceProgram program;
    program.name = "HOME-AND-SETTLE";
    mc::SequenceStep home;
    home.kind = mc::SequenceStepKind::Home;
    home.target = 60.0F;      // 回零速度
    home.waitMs = 5000U;      // 回零时限
    mc::SequenceStep settle;
    settle.kind = mc::SequenceStepKind::WaitReady;
    settle.waitMs = 10000U;
    program.steps = {home, settle};

    ASSERT_TRUE(manager_->startSequence(program)) << manager_->sequenceStartError().toStdString();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->sequencer().status().phase != mc::SequencePhase::Running;
    })) << manager_->sequencer().status().detail;

    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Succeeded);
    const std::vector<mc::StepReport>& reports = manager_->sequenceReports();
    ASSERT_EQ(reports.size(), std::size_t{2});
    EXPECT_EQ(reports[0].kind, mc::SequenceStepKind::Home);
    EXPECT_EQ(reports[0].outcome, mc::StepOutcome::Acked);
    EXPECT_EQ(reports[1].kind, mc::SequenceStepKind::WaitReady);
    // 等就绪那一步等到的正是"零点已建立"：设备回到 Ready 且坐标归零。
    EXPECT_EQ(device()->deviceState(), mc::DeviceState::Ready);
    EXPECT_NEAR(device()->positionDeg(), 0.0F, 1.0F);
}

// 回零步后面没有"等设备就绪"：整份程序在装载时就被拒。
// 放它跑下去的后果是**下一步**撞上"设备不 Ready"，而那个报错指向的地方不是病因。
TEST_F(DeviceManagerTest, AProgramWithAHomeStepMustCarryASettleStep) {
    ASSERT_TRUE(connectAndWaitReady());

    mc::SequenceProgram program;
    program.name = "HOME-ONLY";
    mc::SequenceStep home;
    home.kind = mc::SequenceStepKind::Home;
    home.target = 60.0F;
    home.waitMs = 5000U;
    program.steps = {home};

    EXPECT_FALSE(manager_->startSequence(program));
    EXPECT_NE(manager_->sequenceStartError().toStdString().find("等设备就绪"), std::string::npos)
        << manager_->sequenceStartError().toStdString();
    EXPECT_EQ(manager_->sequencer().status().phase, mc::SequencePhase::Idle);
    EXPECT_FALSE(hasStatus("HOME", CommandStatus::Acked)) << "被拒的程序一步都不该跑";
}

}  // namespace
