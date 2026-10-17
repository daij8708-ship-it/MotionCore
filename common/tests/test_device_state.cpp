#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mc/device_state.hpp"
#include "mc/link_monitor.hpp"

namespace {

using mc::CommandVerdict;
using mc::DeviceEvent;
using mc::DeviceState;
using mc::DeviceStateMachine;
using mc::MessageType;
using mc::TransitionResult;

constexpr std::array<DeviceState, mc::kDeviceStateCount> kAllStates = {
    DeviceState::Disconnected, DeviceState::Initializing, DeviceState::Ready, DeviceState::Running,
    DeviceState::Stopping,     DeviceState::Fault,        DeviceState::Recovery, DeviceState::Home,
};

constexpr std::array<DeviceEvent, mc::kDeviceEventCount> kAllEvents = {
    DeviceEvent::LinkOpened,        DeviceEvent::LinkLost,
    DeviceEvent::HandshakeOk,       DeviceEvent::HandshakeTimeout,
    DeviceEvent::StartAccepted,     DeviceEvent::StopReceived,
    DeviceEvent::StopCompleted,     DeviceEvent::FaultRaised,
    DeviceEvent::RecoveryConfirmed, DeviceEvent::RecoveryOk,
    DeviceEvent::HomeAccepted,      DeviceEvent::HomeCompleted,
};

// ---------------------------------------------------------------------------
// 规格表（`docs/PROTOCOL.md` §5.3 的门禁表 + 本增量裁定的 D1–D5）。
//
// 这是**期望**，不是实现：只写 (起点, 锁存位, 事件) → (是否合法, 终点, 锁存位)，
// 里面没有任何控制流。穷举用例拿它逐格对照被测实现 ——
// 如果两边一起改错（例如把"故障锁存跨断线保留"改成清掉），这条用例会红。
// ---------------------------------------------------------------------------
struct Expected {
    bool legal = false;
    DeviceState to = DeviceState::Disconnected;
    bool latchAfter = false;
};

Expected specTable(DeviceState from, bool latch, DeviceEvent event) {
    const bool disconnected = (from == DeviceState::Disconnected);

    switch (event) {
        case DeviceEvent::LinkOpened:
            if (disconnected) {
                return {true, DeviceState::Initializing, latch};
            }
            return {false, from, latch};

        case DeviceEvent::LinkLost:
            // 处处合法；锁存位**跨断线保留** —— 这正是"故障不能被拔线洗掉"的落点。
            return {true, DeviceState::Disconnected, latch};

        case DeviceEvent::HandshakeOk:
            if (disconnected) {
                return {false, from, latch};
            }
            if (from == DeviceState::Initializing) {
                return {true, latch ? DeviceState::Fault : DeviceState::Ready, latch};
            }
            return {true, from, latch};

        case DeviceEvent::HandshakeTimeout:
            if (from == DeviceState::Initializing) {
                return {true, DeviceState::Fault, true};
            }
            return {false, from, latch};

        case DeviceEvent::StartAccepted:
            if (from == DeviceState::Ready) {
                return {true, DeviceState::Running, latch};
            }
            return {false, from, latch};

        case DeviceEvent::StopReceived:
            // STOP 处处合法（幂等安全动作）。**回零中也要能停**：那是电机真的在转的时刻。
            // 停下之后零点**没有建立** —— 所以它落到 Stopping 而不是 Ready，
            // "这一趟回零作废"这件事在状态上就看得见。
            if (from == DeviceState::Running || from == DeviceState::Home) {
                return {true, DeviceState::Stopping, latch};
            }
            return {true, from, latch};

        case DeviceEvent::StopCompleted:
            if (from == DeviceState::Stopping) {
                return {true, DeviceState::Ready, latch};
            }
            return {false, from, latch};

        case DeviceEvent::FaultRaised:
            if (disconnected) {
                return {false, from, latch};
            }
            return {true, DeviceState::Fault, true};

        case DeviceEvent::RecoveryConfirmed:
            if (from == DeviceState::Fault) {
                return {true, DeviceState::Recovery, latch};
            }
            return {false, from, latch};

        case DeviceEvent::RecoveryOk:
            if (from == DeviceState::Recovery) {
                return {true, DeviceState::Ready, false};
            }
            return {false, from, latch};

        case DeviceEvent::HomeAccepted:
            // 回零与 START 同族：只在 Ready 开动。Running / Stopping 下必须先停，
            // Fault / Recovery 下谈不上（零点都还没建立）。
            if (from == DeviceState::Ready) {
                return {true, DeviceState::Home, latch};
            }
            return {false, from, latch};

        case DeviceEvent::HomeCompleted:
            // 只有"正在回零"才能报到位。Ready 里收到它是**被拒**的：
            // 那等于在没有回零的情况下宣称零点建立了，而这一条若是静默接受，
            // 界面上的"零点已建立"就再也说不清是哪种来源。
            if (from == DeviceState::Home) {
                return {true, DeviceState::Ready, latch};
            }
            return {false, from, latch};
    }
    return {false, from, latch};
}

std::string describe(DeviceState from, bool latch, DeviceEvent event) {
    return std::string(mc::deviceStateName(from)) + " latch=" + (latch ? "1" : "0") + " + " +
           mc::deviceEventName(event);
}

}  // namespace

// ---------------------------------------------------------------------------
// 线上取值：遥测帧的 state / fault_code 字段直接发这些编号，两端必须一致。
// ---------------------------------------------------------------------------
TEST(DeviceState, WireValuesMatchSpec) {
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Disconnected), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Initializing), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Ready), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Running), 3U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Stopping), 4U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Fault), 5U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Recovery), 6U);
    EXPECT_EQ(static_cast<std::uint8_t>(DeviceState::Home), 7U);
    EXPECT_EQ(mc::kDeviceStateCount, 8U);

    EXPECT_EQ(static_cast<std::uint8_t>(mc::FaultCode::None), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(mc::FaultCode::CommunicationLoss), 1U);
    // 回零失败也是一个**设备侧故障码**，它与报警代码表的对应关系由 alarm.hpp 的
    // static_assert 钉住 —— 这里只钉线上取值，免得两头都改了才有人发现对不上。
    EXPECT_EQ(static_cast<std::uint8_t>(mc::FaultCode::HomeFailed), 6U);

    // 每个取值都要有名字 —— 否则日志里只会出现 "?"，排查时等于没有状态。
    for (const DeviceState state : kAllStates) {
        EXPECT_STRNE(mc::deviceStateName(state), "?");
    }
    for (const DeviceEvent event : kAllEvents) {
        EXPECT_STRNE(mc::deviceEventName(event), "?");
    }
}

// ---------------------------------------------------------------------------
// 穷举：(8 状态 × 2 锁存位 × 12 事件) = 192 个组合逐格对照规格表
// ---------------------------------------------------------------------------
TEST(DeviceState, AllOneHundredNinetyTwoCombinationsMatchTheSpecTable) {
    const DeviceStateMachine probe;
    std::size_t legal = 0U;
    std::size_t illegal = 0U;

    for (const DeviceState from : kAllStates) {
        for (const bool latch : {false, true}) {
            for (const DeviceEvent event : kAllEvents) {
                const Expected want = specTable(from, latch, event);
                const TransitionResult got = probe.simulate(from, latch, event);

                SCOPED_TRACE(describe(from, latch, event));

                EXPECT_EQ(got.accepted, want.legal);
                EXPECT_EQ(got.to, want.to);
                EXPECT_EQ(got.faultLatchedAfter, want.latchAfter);

                if (want.legal) {
                    ++legal;
                } else {
                    ++illegal;
                    // 被拒的事件必须**什么都不改**：状态不动、且不算"发生过迁移"。
                    EXPECT_EQ(got.from, from);
                    EXPECT_FALSE(got.stateChanged);
                }
            }
        }
    }

    // 这两个数是设计表推出来的（不是"跑出来是多少就写多少"）：
    //   合法 = LinkLost 8×2 + StopReceived 8×2 + HandshakeOk 7×2 + FaultRaised 7×2
    //        + LinkOpened / HandshakeTimeout / StartAccepted / StopCompleted /
    //          RecoveryConfirmed / RecoveryOk / HomeAccepted / HomeCompleted 各 1×2
    //        = 16 + 16 + 14 + 14 + 8×2 = 76
    EXPECT_EQ(legal + illegal, std::size_t{192});
    EXPECT_EQ(legal, std::size_t{76});
    EXPECT_EQ(illegal, std::size_t{116});
}

// ---------------------------------------------------------------------------
// 真实迁移路径
// ---------------------------------------------------------------------------
TEST(DeviceState, HappyPathReachesRunning) {
    DeviceStateMachine machine(DeviceState::Initializing);

    ASSERT_TRUE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);

    ASSERT_TRUE(machine.apply(DeviceEvent::StartAccepted).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Running);
    EXPECT_TRUE(machine.is(DeviceState::Running));
}

TEST(DeviceState, StopPassesThroughStoppingBeforeReady) {
    DeviceStateMachine machine(DeviceState::Running);

    const TransitionResult stop = machine.apply(DeviceEvent::StopReceived);
    ASSERT_TRUE(stop.accepted);
    ASSERT_TRUE(stop.stateChanged);
    EXPECT_EQ(machine.state(), DeviceState::Stopping);

    const TransitionResult settled = machine.apply(DeviceEvent::StopCompleted);
    ASSERT_TRUE(settled.accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
}

// 被拒的事件既不改变状态、也不改变锁存位，并且会被计数 ——
// 计数存在的意义是：现场排查"为什么这条命令没生效"时能一眼看出是被状态机挡掉的。
TEST(DeviceState, RejectedEventChangesNothingAndIsCounted) {
    DeviceStateMachine machine(DeviceState::Ready);
    EXPECT_EQ(machine.rejectedTransitions(), 0ULL);

    const TransitionResult rejected = machine.apply(DeviceEvent::StopCompleted);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
    EXPECT_FALSE(machine.faultLatched());
    EXPECT_EQ(machine.rejectedTransitions(), 1ULL);

    // 合法事件不应该污染这个计数。
    EXPECT_TRUE(machine.apply(DeviceEvent::StartAccepted).accepted);
    EXPECT_EQ(machine.rejectedTransitions(), 1ULL);
}

TEST(DeviceState, StopIsAcceptedFromEveryStateAndIdempotentOutsideTheMovingStates) {
    for (const DeviceState from : kAllStates) {
        DeviceStateMachine machine(from);
        const TransitionResult result = machine.apply(DeviceEvent::StopReceived);
        SCOPED_TRACE(mc::deviceStateName(from));

        EXPECT_TRUE(result.accepted);
        // 只有"电机动着"的状态会因此改变：Running 和 Home。
        // 其余状态收到 STOP 是幂等的（不算错，也不算迁移）——
        // 一个在任何状态下都接受的安全动作，才可能在下行链路单向、无应答的前提下
        // 被反复发送。
        if (from == DeviceState::Running || from == DeviceState::Home) {
            EXPECT_EQ(machine.state(), DeviceState::Stopping);
            EXPECT_TRUE(result.stateChanged);
        } else {
            EXPECT_EQ(machine.state(), from);
            EXPECT_FALSE(result.stateChanged);
        }
    }
}

TEST(DeviceState, LinkLossFromEveryStateLandsInDisconnected) {
    for (const DeviceState from : kAllStates) {
        DeviceStateMachine machine(from);
        SCOPED_TRACE(mc::deviceStateName(from));

        EXPECT_TRUE(machine.apply(DeviceEvent::LinkLost).accepted);
        EXPECT_EQ(machine.state(), DeviceState::Disconnected);
    }
}

// ---------------------------------------------------------------------------
// 故障：只能人工确认后按 Recovery → Ready 出去，任何"自动复活"的路径都必须被拒
// ---------------------------------------------------------------------------
TEST(DeviceState, FaultCanOnlyBeLeftByRecovery) {
    DeviceStateMachine machine(DeviceState::Running);
    ASSERT_TRUE(machine.apply(DeviceEvent::FaultRaised).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Fault);
    EXPECT_TRUE(machine.faultLatched());

    // 每一条"想绕过 Recovery"的路都必须被拒。
    // 注意 STOP 不在此列：它在任何状态都合法（幂等安全动作），只是不改变状态。
    for (const DeviceEvent bypass :
         {DeviceEvent::LinkOpened, DeviceEvent::StartAccepted, DeviceEvent::StopCompleted,
          DeviceEvent::RecoveryOk, DeviceEvent::HandshakeTimeout}) {
        SCOPED_TRACE(mc::deviceEventName(bypass));
        DeviceStateMachine copy(DeviceState::Running);
        copy.apply(DeviceEvent::FaultRaised);
        EXPECT_FALSE(copy.apply(bypass).accepted);
        EXPECT_EQ(copy.state(), DeviceState::Fault);
    }

    ASSERT_TRUE(machine.apply(DeviceEvent::RecoveryConfirmed).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Recovery);
    EXPECT_TRUE(machine.faultLatched()) << "人工确认只是进入 Recovery，故障本身还没清";

    ASSERT_TRUE(machine.apply(DeviceEvent::RecoveryOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
    EXPECT_FALSE(machine.faultLatched());
}

// **本模块最重要的一条**：故障不能被"拔线重连"顺手洗掉。
// 这条路径恰好是最容易发生的那一条 —— 设备出事 → 人跑过去拔线 → 重新上电 →
// 界面显示一切正常，然后接着按启动。
TEST(DeviceState, FaultSurvivesLinkLossAndReconnectUntilConfirmed) {
    DeviceStateMachine machine(DeviceState::Running);
    ASSERT_TRUE(machine.apply(DeviceEvent::FaultRaised).accepted);

    ASSERT_TRUE(machine.apply(DeviceEvent::LinkLost).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Disconnected);
    EXPECT_TRUE(machine.faultLatched()) << "断线不得清除故障锁存";

    ASSERT_TRUE(machine.apply(DeviceEvent::LinkOpened).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Initializing);

    ASSERT_TRUE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Fault) << "重连成功也必须落回 Fault，而不是 Ready";
    EXPECT_TRUE(machine.faultLatched());

    // 对照组：走完 Recovery 之后，同一条断线重连路径应当落到 Ready。
    // 两组断言合起来才说明"是锁存位在起作用"，而不是那条路径本身就走不通。
    ASSERT_TRUE(machine.apply(DeviceEvent::RecoveryConfirmed).accepted);
    ASSERT_TRUE(machine.apply(DeviceEvent::RecoveryOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
    EXPECT_FALSE(machine.faultLatched());

    ASSERT_TRUE(machine.apply(DeviceEvent::LinkLost).accepted);
    ASSERT_TRUE(machine.apply(DeviceEvent::LinkOpened).accepted);
    ASSERT_TRUE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
}

TEST(DeviceState, HandshakeTimeoutEntersFaultAndLatches) {
    DeviceStateMachine machine(DeviceState::Initializing);
    ASSERT_TRUE(machine.apply(DeviceEvent::HandshakeTimeout).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Fault);
    EXPECT_TRUE(machine.faultLatched());
}

TEST(DeviceState, RepeatedHelloDoesNotDisturbMotionNorClearFault) {
    DeviceStateMachine running(DeviceState::Running);
    EXPECT_TRUE(running.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(running.state(), DeviceState::Running) << "会话级消息不该打断运动";

    DeviceStateMachine faulted(DeviceState::Running);
    faulted.apply(DeviceEvent::FaultRaised);
    EXPECT_TRUE(faulted.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(faulted.state(), DeviceState::Fault) << "重握手不得复位故障";
}

// 拒绝也要能被区分出来：Disconnected 下握手成功是不可能的（链路根本没通），
// 静默接受会让"上位机以为连上了"这件事无法在状态里被看见。
TEST(DeviceState, HelloWhileDisconnectedIsRejected) {
    DeviceStateMachine machine(DeviceState::Disconnected);
    EXPECT_FALSE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Disconnected);
}

TEST(DeviceState, FaultIsNotRaisedWhileDisconnected) {
    DeviceStateMachine machine(DeviceState::Disconnected);
    EXPECT_FALSE(machine.apply(DeviceEvent::FaultRaised).accepted);
    EXPECT_FALSE(machine.faultLatched());
}

// ---------------------------------------------------------------------------
// 命令门禁表：逐格断言，不是"抽查几条"
// ---------------------------------------------------------------------------
TEST(DeviceState, CommandGateTableMatchesSpec) {
    const std::array<MessageType, 11> types = {
        MessageType::Hello,    MessageType::Heartbeat,   MessageType::Stop,
        MessageType::ReadParams, MessageType::SetMode,   MessageType::SetPid,
        MessageType::SetLimits,  MessageType::Start,     MessageType::SetTarget,
        MessageType::Home,       MessageType::ClearFault,
    };

    // 行 = kAllStates 的顺序，列 = 上面 types 的顺序。true = 放行。
    const bool expected[8][11] = {
        //  Hello  HB     STOP   READ   SET_MODE SET_PID SET_LIMITS START  SET_TARGET HOME   CLEAR_FAULT
        {true, true, true, true, false, false, false, false, false, false, false},  // Disconnected
        {true, true, true, true, false, false, false, false, false, false, false},  // Initializing
        {true, true, true, true, true, true, true, true, true, true, false},        // Ready
        {true, true, true, true, false, true, true, false, true, false, false},     // Running
        {true, true, true, true, false, true, true, false, false, false, false},    // Stopping
        {true, true, true, true, false, false, false, false, false, false, true},   // Fault
        {true, true, true, true, false, false, false, false, false, false, false},  // Recovery
        {true, true, true, true, false, false, false, false, false, false, false},  // Home
    };

    for (std::size_t row = 0U; row < kAllStates.size(); ++row) {
        for (std::size_t col = 0U; col < types.size(); ++col) {
            SCOPED_TRACE(std::string(mc::deviceStateName(kAllStates[row])) + " + " +
                         std::to_string(static_cast<int>(types[col])));
            const bool allowed =
                mc::commandVerdict(kAllStates[row], types[col]) == CommandVerdict::Allow;
            EXPECT_EQ(allowed, expected[row][col]);
        }
    }
}

// 门禁表里最关键的两条性质，单独再用属性断言钉一次 ——
// 表格抄错一格时，属性断言还能独立地把它抓出来。
TEST(DeviceState, ControlCommandsAreGatedOnReadyOrRunningOnly) {
    for (const DeviceState state : kAllStates) {
        const bool readyOrRunning = (state == DeviceState::Ready || state == DeviceState::Running);
        SCOPED_TRACE(mc::deviceStateName(state));

        EXPECT_EQ(mc::commandVerdict(state, MessageType::SetTarget) == CommandVerdict::Allow,
                  readyOrRunning);

        // START 更严：只有 Ready。
        EXPECT_EQ(mc::commandVerdict(state, MessageType::Start) == CommandVerdict::Allow,
                  state == DeviceState::Ready);

        // STOP / HEARTBEAT 处处放行 —— 堵掉它们等于自毁安全通道与断线检测。
        EXPECT_EQ(mc::commandVerdict(state, MessageType::Stop) == CommandVerdict::Allow, true);
        EXPECT_EQ(mc::commandVerdict(state, MessageType::Heartbeat) == CommandVerdict::Allow, true);

        // 故障确认只在 Fault 放行：Recovery 里说明人工确认已经做过了，
        // 其余状态要么没有故障可确认，要么链路根本发不出去。
        EXPECT_EQ(mc::commandVerdict(state, MessageType::ClearFault) == CommandVerdict::Allow,
                  state == DeviceState::Fault);

        // 回零与 START 同一道闸（都是"让电机动起来"），但比 START 更不容许并行：
        // 回零的前提是"此刻没有别的运动目标"。
        EXPECT_EQ(mc::commandVerdict(state, MessageType::Home) == CommandVerdict::Allow,
                  state == DeviceState::Ready);

        // 软限位与 SET_PID 同族：改的是保护参数、不改变目标值。
        const bool parameterWritable = (state == DeviceState::Ready ||
                                        state == DeviceState::Running ||
                                        state == DeviceState::Stopping);
        EXPECT_EQ(mc::commandVerdict(state, MessageType::SetLimits) == CommandVerdict::Allow,
                  parameterWritable);
        EXPECT_EQ(mc::commandVerdict(state, MessageType::SetPid) == CommandVerdict::Allow,
                  parameterWritable);
    }
}

// ---------------------------------------------------------------------------
// F7 的窗口必须**长于**上位机的心跳周期 —— 这是它唯一不能违反的条件。
// 反过来（窗口比心跳短）的后果很隐蔽：链路完全正常，设备却在两次心跳之间
// 判自己"通信丢失"并跳 Fault。我第一版就是写反了，端到端用例当场抓到。
// ---------------------------------------------------------------------------
TEST(DeviceState, CommLossWindowMustOutlastTheHostHeartbeatPeriod) {
    const mc::LinkMonitor::Config heartbeat{};

    EXPECT_GT(mc::kDeviceCommLossTimeoutMs, heartbeat.heartbeatPeriodMs)
        << "窗口短于心跳周期 ⇒ 心跳空档被判成丢线，健康链路上设备自己跳 Fault";

    EXPECT_GE(mc::kDeviceCommLossTimeoutMs,
              heartbeat.heartbeatPeriodMs * heartbeat.missedBeatsToDeclareLinkDown)
        << "两端判「链路失效」应当同口径，否则会出现「上位机说正常、设备已停机」";

    EXPECT_LT(mc::kDeviceCommLossTimeoutMs, 5000U)
        << "窗口不宜再长：它直接等于电机带着陈旧目标还能跑多久";
}

// ---------------------------------------------------------------------------
// 回零（P1-6）：入口只有 Ready 一条，出口只有'零点建立'一条
// ---------------------------------------------------------------------------
TEST(DeviceState, HomeStartsOnlyFromReadyAndOnlyLeavesThroughCompletion) {
    // 入口：Running 下必须先 STOP —— 回零的前提是"此刻没有别的运动目标"。
    DeviceStateMachine running(DeviceState::Running);
    EXPECT_FALSE(running.apply(DeviceEvent::HomeAccepted).accepted);
    EXPECT_EQ(running.state(), DeviceState::Running);

    DeviceStateMachine ready(DeviceState::Ready);
    const TransitionResult started = ready.apply(DeviceEvent::HomeAccepted);
    ASSERT_TRUE(started.accepted);
    EXPECT_TRUE(started.stateChanged);
    EXPECT_EQ(ready.state(), DeviceState::Home);

    ASSERT_TRUE(ready.apply(DeviceEvent::HomeCompleted).accepted);
    EXPECT_EQ(ready.state(), DeviceState::Ready);

    // 反向：没在回零却报"到位"必须被拒。放行它等于让"零点已建立"这句话
    // 分不清是设备报的还是谁顺手写上去的 —— 而后面所有位置都相对它。
    EXPECT_FALSE(ready.apply(DeviceEvent::HomeCompleted).accepted);
    EXPECT_EQ(ready.state(), DeviceState::Ready);
}

TEST(DeviceState, StopDuringHomingGoesToStoppingNotReady) {
    DeviceStateMachine machine(DeviceState::Home);

    const TransitionResult stop = machine.apply(DeviceEvent::StopReceived);
    ASSERT_TRUE(stop.accepted);
    // 关键的一格：被 STOP 打断的回零**不经过 HomeCompleted**。
    // 设备侧只在 HomeCompleted 时才把当前位置定义成 0（见 DeviceSimulator
    // 的 defineOriginHere），所以"停下 ⇒ 零点没建立"这件事由这两处合起来证明，
    // 状态机这边负责的是"别把它伪装成一次正常的结束"。
    EXPECT_EQ(machine.state(), DeviceState::Stopping);

    ASSERT_TRUE(machine.apply(DeviceEvent::StopCompleted).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Ready);
}

TEST(DeviceState, RepeatedHelloDoesNotDisturbHoming) {
    DeviceStateMachine machine(DeviceState::Home);
    EXPECT_TRUE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Home) << "会话级消息不该打断回零";
}

// 回零中出故障：与其它状态一样锁存，而且**不能靠重连洗掉** ——
// 这条路径在回零场景里格外常见（撞上机械限位 → 人拔线重来）。
TEST(DeviceState, FaultDuringHomingLatchesAndSurvivesReconnect) {
    DeviceStateMachine machine(DeviceState::Home);
    ASSERT_TRUE(machine.apply(DeviceEvent::FaultRaised).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Fault);
    EXPECT_TRUE(machine.faultLatched());

    ASSERT_TRUE(machine.apply(DeviceEvent::LinkLost).accepted);
    ASSERT_TRUE(machine.apply(DeviceEvent::LinkOpened).accepted);
    ASSERT_TRUE(machine.apply(DeviceEvent::HandshakeOk).accepted);
    EXPECT_EQ(machine.state(), DeviceState::Fault);
    EXPECT_TRUE(machine.faultLatched());
}
