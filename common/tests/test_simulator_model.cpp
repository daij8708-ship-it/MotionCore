#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/frame_codec.hpp"

// 设备侧运动模型与控制律（阶段 8）的用例。
//
// 这些用例**不走 HostSession**：它们测的是设备自己的机械/传感/控制，
// 把上位机的会话逻辑拉进来只会让失败原因更难定位。
// 驱动方式就是"把真帧喂进去、把真应答读出来"——与链路上发生的事一模一样。
//
// ⚠️ 所有数字都是**声明式仿真模型**的产物，不是任何实物的测量结果。
//    这里断言的是"模型按声明的参数在动"（惯性大→响应慢、增益为 0→纯前馈…），
//    不是"电机应该转多快"。
namespace {

class DeviceDriver {
public:
    explicit DeviceDriver(mc::SimulatorConfig config = mc::SimulatorConfig{}) : sim_(config) {}

    // 每帧之间至少隔一拍（10 ms）：同一拍里连跳状态与真链路的时序不同，
    // 那样测出来的响应也不能直接与链路现象对照。
    void send(mc::MessageType type, std::vector<std::uint8_t> payload = {}) {
        now_ += 10U;
        mc::Frame frame;
        frame.type = type;
        frame.seq = seq_++;
        frame.payload = std::move(payload);
        const mc::EncodeResult encoded = mc::encodeFrame(frame);
        EXPECT_TRUE(static_cast<bool>(encoded));
        (void)sim_.tick(encoded.bytes.data(), encoded.bytes.size(), now_);
    }

    void idle(std::size_t ticks) {
        for (std::size_t i = 0U; i < ticks; ++i) {
            now_ += 10U;
            (void)sim_.tick(nullptr, 0U, now_);
        }
    }

    // 一直推进到设备进入期望状态为止（或超出拍数上限）。
    // 停机这类收敛过程用固定拍数去赌是脆的：门限一改就飘。
    bool idleUntil(mc::DeviceState wanted, std::size_t maxTicks) {
        for (std::size_t i = 0U; i < maxTicks; ++i) {
            if (sim_.state() == wanted) {
                return true;
            }
            idle(1U);
        }
        return sim_.state() == wanted;
    }

    // 握手 + 选速度环 + 使能 + 给目标：跑到 Running 的最短路径。
    void startVelocityMode(float target) {
        send(mc::MessageType::Hello);
        send(mc::MessageType::SetMode, mc::encodeSetMode(mc::SetMode{1U}));
        send(mc::MessageType::Start);
        send(mc::MessageType::SetTarget, mc::encodeSetTarget(mc::SetTarget{target}));
    }

    void startPositionMode(float target, float kp, float ki, float kd) {
        send(mc::MessageType::Hello);
        send(mc::MessageType::SetPid, mc::encodeSetPid(mc::SetPid{kp, ki, kd}));
        send(mc::MessageType::SetMode, mc::encodeSetMode(mc::SetMode{2U}));
        send(mc::MessageType::Start);
        send(mc::MessageType::SetTarget, mc::encodeSetTarget(mc::SetTarget{target}));
    }

    mc::DeviceSimulator& sim() noexcept { return sim_; }
    std::uint64_t nowMs() const noexcept { return now_; }

    // 把设备**已经发出去**的字节读干净、解成帧。
    // 用来断言"线路上真的发了什么"，而不是只看设备内部的状态字段 ——
    // 后者抓不住"编码那一步把字段写成了 0"这类缺陷（见变异验证）。
    // 必须循环读到空：send() 从不取走发件箱，整场会话的帧都堆在里面，
    // 只读一次拿到的是最早那一段。
    std::vector<mc::Frame> drain() {
        std::vector<mc::Frame> frames;
        std::uint8_t buffer[4096];
        for (;;) {
            const std::size_t n = sim_.readOutgoing(buffer, sizeof(buffer));
            if (n == 0U) {
                break;
            }
            for (mc::Frame& frame : decoder_.feed(buffer, n)) {
                frames.push_back(std::move(frame));
            }
        }
        return frames;
    }

private:
    mc::DeviceSimulator sim_;
    mc::StreamDecoder decoder_;
    std::uint64_t now_ = 0U;
    std::uint16_t seq_ = 0U;
};

// 速度估计的量化噪声下限：窗口两端各有一个计数的不确定度，合起来最多一个计数。
// 一个计数 = 360/每转计数 度；换算成速度就是再除以窗口时长。
float quantizationFloorDegPerSec(const mc::SimulatorConfig& config) {
    return (360.0F / config.encoderCountsPerRev) * (1000.0F / config.velocityWindowMs);
}

}  // namespace

// 报出来的速度是**量化的位置**差分出来的，因此它只能落在真值的量化噪声带里。
// 这条同时钉住两件事：① 上报的不是"上帝视角"真值；② 噪声有可以算出来的界。
TEST(SimulatorModel, ReportedVelocitySitsInsideTheQuantizationFloorOfTheTrueVelocity) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    driver.startVelocityMode(120.0F);
    driver.idle(200U);   // 2 s：足够收敛到稳态

    const float trueVelocity = driver.sim().trueVelocityDegPerSec();
    const float reported = driver.sim().velocityDegPerSec();
    const float floorDegPerSec = quantizationFloorDegPerSec(config);

    EXPECT_NEAR(trueVelocity, 120.0F, 0.5F) << "控制律该把真实速度稳在目标上";
    EXPECT_NEAR(reported, trueVelocity, floorDegPerSec)
        << "上报值与真值之差不能超过量化噪声下限（一个计数 / 窗口）";

    // 结构性判据：上报速度必须是**整数个计数**对应的速度
    //（一个计数 = 360/每转计数 度，除以窗口时长就是格距，即 floorDegPerSec）。
    //
    // 为什么不能只写"上报值 != 真值"：稳态下这两者的数值几乎相等，
    // 一个**直接拿未量化真值上报**的实现照样混得过去 ——
    // 实测确认过：把速度估计改成用 position_ 而不是 measuredPosition_（变异 M43-1），
    // 只断言"不相等"的写法**一条用例都没红**。能分辨真假的只有这个格点结构。
    const float countSteps = reported / floorDegPerSec;
    EXPECT_NEAR(countSteps, std::round(countSteps), 2.0e-2F)
        << "上报速度不落在「整数个计数」的格点上（" << countSteps
        << " 个计数）—— 它不可能来自量化位置的差分";
}

// 位置读数只能是整计数：它是一台 4096 计数/转的编码器能给出的全部信息。
TEST(SimulatorModel, ReportedPositionIsQuantizedToWholeEncoderCounts) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    driver.startVelocityMode(200.0F);

    const float stepDeg = 360.0F / config.encoderCountsPerRev;
    bool sawNonInteger = false;
    for (int i = 0; i < 40; ++i) {
        driver.idle(1U);
        const float position = driver.sim().positionDeg();
        const float counts = position / stepDeg;
        if (std::fabs(counts - std::round(counts)) > 1.0e-3F) {
            sawNonInteger = true;
        }
    }
    EXPECT_FALSE(sawNonInteger) << "位置读数必须是整计数（" << stepDeg << " deg/计数）";

    // 正对照：位置确实在走 —— 否则上面那条断言在"根本没动"时也会通过。
    EXPECT_GT(driver.sim().positionDeg(), 10.0F);
}

// 机械惯性：时间常数越大，同样的指令下速度爬起来越慢。
TEST(SimulatorModel, InertiaDelaysTheResponseToTheSameCommand) {
    mc::SimulatorConfig fast;
    fast.inertiaTimeConstantMs = 0.0F;     // 对照：无惯性（瞬时跟随）
    mc::SimulatorConfig slow;
    slow.inertiaTimeConstantMs = 400.0F;   // 一阶滞后 400 ms

    DeviceDriver fastDriver(fast);
    DeviceDriver slowDriver(slow);
    fastDriver.startVelocityMode(200.0F);
    slowDriver.startVelocityMode(200.0F);

    // 只跑 60 ms：两边都还没到目标，但惯性大的那个明显更慢。
    fastDriver.idle(6U);
    slowDriver.idle(6U);

    const float fastVelocity = fastDriver.sim().trueVelocityDegPerSec();
    const float slowVelocity = slowDriver.sim().trueVelocityDegPerSec();
    EXPECT_GT(fastVelocity, slowVelocity)
        << "时间常数大反而更快就说明惯性没接进模型：" << fastVelocity << " vs " << slowVelocity;
    EXPECT_LT(slowVelocity, 100.0F) << "400 ms 的时间常数下，60 ms 不可能爬到一半";
}

// 增益全 0 的速度环 = 纯前馈：照常跟随，且**没有稳态误差**（被控对象本身是一阶惯性）。
// 这条是"阶段 8 的闭环与阶段 0 的最小模型应当可对照"的那个对照。
TEST(SimulatorModel, ZeroGainsLeaveVelocityModeAsPureFeedforward) {
    DeviceDriver driver;
    driver.startVelocityMode(80.0F);
    driver.idle(300U);

    EXPECT_NEAR(driver.sim().trueVelocityDegPerSec(), 80.0F, 0.5F);
    EXPECT_EQ(driver.sim().pid().kp, 0.0F);
}

// 位置环的**负对照**：没整定过就不动。真设备上没整定的位置环也不会动，
// 把它做成"照目标给速度"才是骗人。
TEST(SimulatorModel, PositionModeWithZeroGainsDoesNotMoveTheAxis) {
    DeviceDriver driver;
    driver.startPositionMode(90.0F, 0.0F, 0.0F, 0.0F);
    driver.idle(200U);

    EXPECT_FLOAT_EQ(driver.sim().trueVelocityDegPerSec(), 0.0F);
    EXPECT_NEAR(driver.sim().positionDeg(), 0.0F, 0.1F) << "没有增益就不该有任何位移";
}

// 正对照：给上增益之后，位置环真的把轴开到目标位置。
TEST(SimulatorModel, PositionModeWithGainsDrivesTheAxisToTheTarget) {
    DeviceDriver driver;
    driver.startPositionMode(90.0F, 3.0F, 0.0F, 0.0F);
    driver.idle(400U);   // 4 s：单环 P 控制，收敛是渐近的

    const float stepDeg = 360.0F / mc::SimulatorConfig{}.encoderCountsPerRev;
    // 精度受编码器分辨率限制：闭环停在"误差小到不足以产生可测速度"的地方，
    // 也就是目标附近的几个计数之内。这是个物理上界，不是整定问题。
    EXPECT_NEAR(driver.sim().positionDeg(), 90.0F, 5.0F * stepDeg);
    EXPECT_NEAR(driver.sim().trueVelocityDegPerSec(), 0.0F, 0.5F) << "到位置后应当停住";
}

// 停机后再使能，不许因为**陈旧的控制器状态**窜一下。
//
// 这里打的正是 integrate() 的不驱动分支：STOP 之后设备处于 Stopping/Ready，
// 每一拍都把 PID 状态清掉，所以重新 Start 时控制器是干净的。
// 若那处清状态被去掉，下面这条会在重新使能的第一拍看到一个满量程的输出 ——
// 因为停机前的积分残量（目标 0 减去一个陈旧的积分项）会被直接当成速度指令发下去。
TEST(SimulatorModel, ReEnablingAfterAStopDoesNotLurchFromStaleControllerState) {
    mc::SimulatorConfig config;
    config.initialPid = mc::SetPid{1.0F, 4.0F, 0.1F};   // 带积分项，才谈得上"陈旧状态"
    DeviceDriver driver(config);

    driver.startVelocityMode(300.0F);
    driver.idle(100U);   // 跑够 1 s：积分项在爬升期攒起来

    driver.send(mc::MessageType::Stop);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 200U))
        << "STOP 之后设备该自己收敛到 Ready（状态="
        << static_cast<int>(driver.sim().state()) << "）";

    driver.send(mc::MessageType::Start);   // 重新使能。target 已被 STOP 清成 0
    driver.idle(1U);

    const mc::Telemetry telemetry = driver.sim().lastTelemetry();
    EXPECT_LE(telemetry.pwmOutput, 1U)
        << "停机后重新使能的第一拍不该有输出 —— 非 0 就是陈旧积分被当成了指令";
    // 剩下的那点残速是**惯性滑行**，不是窜动：停机判据（|v| ≤ 3.84 deg/s）
    // 本来就会在速度归零之前先判 Ready，之后只会继续衰减。
    // 钉住"在停机门限之内"，等于钉住"它确实是自己停稳的，不是被这一拍推起来的"。
    EXPECT_LE(std::fabs(driver.sim().trueVelocityDegPerSec()),
              config.stopVelocityThresholdDegPerSec + 0.1F);
}

// ---------------------------------------------------------------------------
// 回零（P1-6）
//
// 回零这一段的判据是"位置进入容差带"，而位置是**编码器量化过**的读数 ——
// 下面几条用例合起来钉住三件事：零点**建立在哪里**、走不到时**报什么**、
// 被 STOP 打断时**没建立**。
// ---------------------------------------------------------------------------

namespace {

// 让轴先离开零点：速度环跑一段再停稳，回到 Ready。
// 回零只在 Ready 放行，而且"原点就在脚下"的回零什么都测不出来。
void moveAwayFromZeroAndSettle(DeviceDriver& driver, float targetDegPerSec = 120.0F,
                               std::size_t runTicks = 50U) {
    driver.startVelocityMode(targetDegPerSec);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Running, 50U));
    driver.idle(runTicks);
    driver.send(mc::MessageType::Stop);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 200U));
}

}  // namespace

TEST(SimulatorModel, HomingDefinesTheOriginWhereItArrivesAndReturnsToReady) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    moveAwayFromZeroAndSettle(driver);
    ASSERT_GT(std::fabs(driver.sim().positionDeg()), 5.0F) << "前提：轴确实离开了零点";

    driver.send(mc::MessageType::Home,
                mc::encodeHomeParam(mc::HomeParam{60.0F, 5000U}));
    ASSERT_EQ(driver.sim().state(), mc::DeviceState::Home);

    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 2000U))
        << "回零应当在容差带里结束（容差 0.25 deg > 一个编码器计数 0.088 deg）";
    // 零点**建立在这里**：坐标被搬到了 0，而不是"数值改小一点"。
    EXPECT_NEAR(driver.sim().positionDeg(), 0.0F, 1.0F);
    EXPECT_EQ(driver.sim().faultCode(), mc::FaultCode::None);
}

// 走不到零点的回零必须**有个头**：报一条说得出原因的故障，而不是永远挂着。
// 时限小到根本来不及，是现场真会发生的一种：回零时限填成了 1 ms。
// 它同时钉住"没有超时的回零"为什么不能要 —— 那种情况从界面上看和"还在走"一模一样。
TEST(SimulatorModel, HomingThatCannotFinishInTimeFaultsWithANamedCause) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    moveAwayFromZeroAndSettle(driver);
    const float before = driver.sim().positionDeg();
    ASSERT_GT(std::fabs(before), 5.0F);

    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{60.0F, 1U}));
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Fault, 200U));

    EXPECT_EQ(driver.sim().faultCode(), mc::FaultCode::HomeFailed)
        << "故障码要能指向「回零没走完」，而不是一个笼统的故障";
    EXPECT_TRUE(driver.sim().faultLatched());
    // 零点**没有**建立：位置没有被搬到 0。
    EXPECT_GT(std::fabs(driver.sim().positionDeg()), 1.0F);
}

// 回零中 STOP 必须有效（那是电机动着的时刻），但它**不产生零点**。
// 这一格若错，界面上的"停下来了"会被读成"回零成功了"，而后面的坐标全错。
TEST(SimulatorModel, StopDuringHomingStopsTheAxisButLeavesNoOrigin) {    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    moveAwayFromZeroAndSettle(driver);
    const float before = driver.sim().positionDeg();
    ASSERT_GT(std::fabs(before), 5.0F);

    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{60.0F, 5000U}));
    ASSERT_EQ(driver.sim().state(), mc::DeviceState::Home);
    driver.idle(10U);   // 让它真的朝零点走了一小段

    driver.send(mc::MessageType::Stop);
    EXPECT_EQ(driver.sim().state(), mc::DeviceState::Stopping);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 200U));

    // "没有建立零点"的证据就是：位置**没有被搬到 0**，还停在原来那一带。
    EXPECT_GT(std::fabs(driver.sim().positionDeg()), 1.0F);
    EXPECT_NEAR(driver.sim().positionDeg(), before, 10.0F)
        << "被停下的回零也不该把轴开走一大段";
}

// ---------------------------------------------------------------------------
// 遥测里的"本轮回零已建立零点"位（v3）
//
// 这一位存在的唯一理由：上位机**判不出来**"走到位"与"被 STOP 打断"。
// 被打断的那条路径是 Home → Stopping → Ready，而回零本来就是低速 ——
// 那一拍的 |v| 常常已经在停机门限之内，Stopping 会在同一拍里过去。
// 所以下面这一组用例测的不是"位怎么编解码"，而是**位与"这次到底成没成"对不对**。
// ---------------------------------------------------------------------------
TEST(SimulatorModel, HomingAttainedBitIsClearedOnStartAndSetOnlyOnCompletion) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    EXPECT_FALSE(driver.sim().homingAttained()) << "出厂就是没回过零";

    moveAwayFromZeroAndSettle(driver);
    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{60.0F, 5000U}));
    EXPECT_EQ(driver.sim().state(), mc::DeviceState::Home);
    EXPECT_FALSE(driver.sim().homingAttained()) << "刚开始的一趟不算建立";

    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 2000U));
    EXPECT_TRUE(driver.sim().homingAttained());

    // 反向对照：**再回一次、半路停住**，位必须回到 false ——
    // 它说的是"这一轮"，不是"这台机器历史上回过零"。若只在成功时置位、
    // 不在开始时清零，这一格就会留着上一次的 1，上位机把被打断读成成功。
    moveAwayFromZeroAndSettle(driver);
    ASSERT_TRUE(driver.sim().homingAttained()) << "前提：上一轮成功过，位还在";
    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{5.0F, 30000U}));
    ASSERT_EQ(driver.sim().state(), mc::DeviceState::Home);
    EXPECT_FALSE(driver.sim().homingAttained()) << "新一轮开始就该清零";

    driver.send(mc::MessageType::Stop);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 200U));
    EXPECT_FALSE(driver.sim().homingAttained())
        << "被打断的一趟从头到尾都不该被读成'零点已建立'";
}

// 位必须**随遥测发出去**：上位机拿到的只有帧，它没有别的途径。
// 这条同时钉住"帧长度是 21 字节"这件事在两端是一致的 —— 少一位的话
// 上位机读到的是一个更短（也就会被判为坏数据）的载荷。
TEST(SimulatorModel, TheHomingAttainedBitTravelsInEveryTelemetryFrame) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    moveAwayFromZeroAndSettle(driver);
    ASSERT_FALSE(driver.sim().homingAttained());
    driver.idle(2U);
    EXPECT_EQ(driver.sim().lastTelemetry().statusFlags, 0U);

    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{60.0F, 5000U}));
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 2000U));
    ASSERT_TRUE(driver.sim().homingAttained());
    // 到位之后还要再发一帧：位是**持续**报的，不是只在跳变那一拍报一次。
    driver.idle(2U);
    const mc::Telemetry telemetry = driver.sim().lastTelemetry();
    EXPECT_EQ(telemetry.statusFlags & mc::kTelemetryFlagHomingAttained,
              mc::kTelemetryFlagHomingAttained);
    // 位必须**落在线上**：把设备真正发出的帧解回来，逐字节确认。
    // 只断言内部结构体是不够的 —— 那样"编码那一步把它写成了 0"照样绿。
    bool sawBitOnTheWire = false;
    for (const mc::Frame& frame : driver.drain()) {
        if (frame.type != mc::MessageType::Telemetry) {
            continue;
        }
        EXPECT_EQ(frame.payload.size(), mc::kTelemetrySize);
        mc::Telemetry onWire;
        ASSERT_EQ(mc::decodeTelemetry(frame.payload.data(), frame.payload.size(), onWire),
                  mc::PayloadError::None);
        if ((onWire.statusFlags & mc::kTelemetryFlagHomingAttained) != 0U) {
            sawBitOnTheWire = true;
        }
    }
    EXPECT_TRUE(sawBitOnTheWire) << "「零点已建立」这一位必须在线上，而不只在设备内部";
}

// ---------------------------------------------------------------------------
// 软限位（P1-6）
// ---------------------------------------------------------------------------

// 限位是**位置**行程限制：越界就撤输出并进故障，而不是"提示一下"。
TEST(SimulatorModel, TheAxisIsStoppedAndFaultedWhenItRunsPastTheSoftLimit) {
    mc::SimulatorConfig config;
    config.softLimits = mc::SoftLimits{true, -5.0F, 5.0F};
    DeviceDriver driver(config);

    driver.startVelocityMode(120.0F);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Fault, 200U));

    EXPECT_EQ(driver.sim().faultCode(), mc::FaultCode::OverLimit);
    EXPECT_LE(driver.sim().positionDeg(), 5.0F + 5.0F)
        << "越界之后输出要撤掉：它是**撤输出**，不是继续朝界外走";
}

// 反向对照：**关掉就是不判**，两个数值一个都不参与。
// 这条与上面那条合起来才说明"是 enabled 在起作用"，而不是那组数值碰巧在拦。
TEST(SimulatorModel, DisabledSoftLimitsDoNotConstrainAnythingEvenIfNumbersAreSet) {
    mc::SimulatorConfig config;
    config.softLimits = mc::SoftLimits{false, -5.0F, 5.0F};   // 数值还在，但没启用
    DeviceDriver driver(config);

    driver.startVelocityMode(120.0F);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Running, 50U));
    driver.idle(100U);   // 1 s：按 120 deg/s 早就走过 5 deg 了

    EXPECT_GT(driver.sim().positionDeg(), 5.0F);
    EXPECT_EQ(driver.sim().state(), mc::DeviceState::Running);
    EXPECT_EQ(driver.sim().faultCode(), mc::FaultCode::None);
}

// 把**当前位置**关在门外的限位必须被明确拒掉。
// 放行它的后果是"设完下一拍就报超限故障"—— 那看起来像设备自己出了问题，
// 其实是刚被设成这个样子；排查方向会被带偏一整轮。
TEST(SimulatorModel, LimitsThatWouldLockOutTheCurrentPositionAreRefused) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    driver.startVelocityMode(120.0F);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Running, 50U));
    driver.idle(30U);
    const float here = driver.sim().positionDeg();
    ASSERT_GT(here, 3.0F);

    driver.send(mc::MessageType::SetLimits,
                mc::encodeSoftLimits(mc::SoftLimits{true, here + 5.0F, here + 50.0F}));

    ASSERT_FALSE(driver.sim().nacksSent().empty());
    EXPECT_EQ(driver.sim().nacksSent().back().reason, mc::NackReason::BadParameter);
    EXPECT_FALSE(driver.sim().softLimits().enabled) << "被拒的限位一个字节都不该落地";
    EXPECT_EQ(driver.sim().state(), mc::DeviceState::Running) << "拒绝不该把设备打进故障";
}

// 正向对照：合法的限位必须**真的存进设备**，而且回读能拿到同一组。
// 只测"越界会被拦"是不够的 —— 拦是拿**当前载荷**判的，存是另一回事：
// 把存储那一步整个删掉，"越界会被拦"照样绿（见变异验证里的 C5）。
TEST(SimulatorModel, SetLimitsCommandIsStoredAndComesBackInParamState) {
    mc::SimulatorConfig config;
    DeviceDriver driver(config);
    driver.startVelocityMode(120.0F);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Running, 50U));
    driver.idle(30U);

    const float here = driver.sim().positionDeg();
    const mc::SoftLimits wanted{true, here - 40.0F, here + 40.0F};
    driver.send(mc::MessageType::SetLimits, mc::encodeSoftLimits(wanted));
    EXPECT_TRUE(driver.sim().nacksSent().empty()) << "合法的限位不该被拒";

    // 设备**存着**的这一份就是 READ_PARAMS 的同一个来源。
    EXPECT_TRUE(driver.sim().softLimits().enabled) << "启用了却存成没启用，等于这条命令白发了";
    EXPECT_FLOAT_EQ(driver.sim().softLimits().minDeg, wanted.minDeg);
    EXPECT_FLOAT_EQ(driver.sim().softLimits().maxDeg, wanted.maxDeg);

    // 再走一遍线：READ_PARAMS 的应答里必须带同一组限位。
    driver.drain();   // 丢掉刚才那些应答，只看这一轮
    driver.send(mc::MessageType::ReadParams);
    bool sawLimitsInReadback = false;
    for (const mc::Frame& frame : driver.drain()) {
        if (frame.type != mc::MessageType::ReadParamsAck) {
            continue;
        }
        mc::ParamState state;
        ASSERT_EQ(mc::decodeParamState(frame.payload.data(), frame.payload.size(), state),
                  mc::PayloadError::None);
        EXPECT_TRUE(state.limits.enabled);
        EXPECT_FLOAT_EQ(state.limits.minDeg, wanted.minDeg);
        EXPECT_FLOAT_EQ(state.limits.maxDeg, wanted.maxDeg);
        sawLimitsInReadback = true;
    }
    EXPECT_TRUE(sawLimitsInReadback) << "回读应答必须带设备存着的那组限位";
}

// 回零期间**不判限位**。理由不是"宽松一点"，而是此刻坐标系正在被重新定义：
// 拿一组相对旧零点定义的限位去拦一段"去找零点"的运动是本末倒置。
//
// 这条路径在真机上恰好是最需要回零的时候：轴在越界那一刻进故障 → 人工确认 → Ready，
// 此时位置仍在限位之外。若回零期间还判限位，它会当场再报一次同样的故障 ——
// 操作员就卡在一个修不了的死循环里。
TEST(SimulatorModel, SoftLimitsAreNotEnforcedWhileHoming) {
    mc::SimulatorConfig config;
    config.softLimits = mc::SoftLimits{true, -30.0F, 30.0F};
    DeviceDriver driver(config);

    // 前提：轴真的越界而过，并停在限位之外。
    driver.startVelocityMode(120.0F);
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Fault, 300U));
    ASSERT_EQ(driver.sim().faultCode(), mc::FaultCode::OverLimit);
    const float outside = driver.sim().positionDeg();
    ASSERT_GT(outside, config.softLimits.maxDeg);

    // 人工确认（载荷要带**当前锁存的那个**故障码）→ 回到 Ready。
    driver.send(mc::MessageType::ClearFault,
                mc::encodeClearFault(
                    mc::ClearFault{static_cast<std::uint8_t>(mc::FaultCode::OverLimit)}));
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 50U));
    ASSERT_GT(driver.sim().positionDeg(), config.softLimits.maxDeg)
        << "前提：此刻的位置仍在限位之外";

    // 回零：它会穿过那段越界的行程走回零点，全程不报超限。
    driver.send(mc::MessageType::Home, mc::encodeHomeParam(mc::HomeParam{60.0F, 5000U}));
    ASSERT_TRUE(driver.idleUntil(mc::DeviceState::Ready, 2000U))
        << "回零期间判限位的话，这里会当场再报一次 OverLimit（状态="
        << static_cast<int>(driver.sim().state()) << "）";
    EXPECT_NEAR(driver.sim().positionDeg(), 0.0F, 1.0F) << "零点建立在新到的位置上";
}
