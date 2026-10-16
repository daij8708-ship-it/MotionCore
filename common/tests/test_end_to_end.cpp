#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/host_session.hpp"
#include "mc/loopback_transport.hpp"

namespace {

// ---------------------------------------------------------------------------
// 测试台：把上位机、设备、以及两者之间的回环链路装在一起，
// 用一个 step() 推进"一拍"（默认 10 ms，与设备的遥测周期一致）。
// ---------------------------------------------------------------------------
struct Rig {
    mc::LoopbackPair pair;
    mc::HostSession host;
    mc::DeviceSimulator sim;
    std::uint64_t now = 0U;
    std::uint16_t seq = 0U;
    std::uint64_t baseFramesSent = 0U;
    std::uint64_t baseFramesDecoded = 0U;
    std::uint64_t baseTelemetryAccepted = 0U;

    Rig(const mc::SimulatorConfig& simConfig = mc::SimulatorConfig{})
        : pair(mc::makeLoopbackPair()), host(*pair.host), sim(simConfig) {
        // E2：测试台**不再手工发 HELLO**。会话自己会在 pump() 里发起握手、
        // 收到 HELLO_ACK 后转入就绪 —— 于是每一条端到端用例都在跑这条新路径，
        // 而不是各自绕过它。
        host.enableHandshake([this]() { return nextSeq(); });
        handshake();
    }

    void handshake() {
        // 阶段 4 起，设备开局处于 Initializing（构造等价于"链路已打开、尚未握手"），
        // 未握手时一切控制命令都会被门禁拒掉 —— 所以必须先握上手。
        for (std::size_t i = 0U; i < 30U && !host.sessionReady(); ++i) {
            run(1U);
        }
        EXPECT_TRUE(host.sessionReady()) << "干净链路上握手必须自己完成";
        EXPECT_EQ(host.handshakeRounds(), 1ULL) << "干净链路上一轮就该成";
        // 握手结束时链路上天然还剩一帧在途遥测（最后一拍设备必然发帧）。
        // 必须排空，否则它会在场景的第一拍被读走，把"只读"的计数污染掉。
        drainHost();
        baseFramesSent = host.framesSent();
        baseFramesDecoded = host.decoder().stats().framesDecoded;
        baseTelemetryAccepted = host.telemetryAccepted();
    }

    // 握手自己产生 1 帧 HELLO → 1 帧 HELLO_ACK → 1 帧遥测。
    // 需要"绝对帧数"的用例一律用下面这几个**增量**函数，
    // 不要拿总量去跟场景里的数字硬比 —— 那样每次改测试台都要回来改一堆断言。
    std::uint64_t framesSent() const { return host.framesSent() - baseFramesSent; }
    std::uint64_t framesDecoded() const {
        return host.decoder().stats().framesDecoded - baseFramesDecoded;
    }
    std::uint64_t telemetryAccepted() const {
        return host.telemetryAccepted() - baseTelemetryAccepted;
    }

    std::uint16_t nextSeq() { return seq++; }

    struct StepOutcome {
        mc::HostSession::PumpResult hostSide;
        mc::DeviceSimulator::TickResult deviceSide;
    };

    StepOutcome step(std::uint32_t dtMs = 10U) {
        now += dtMs;

        StepOutcome outcome;
        outcome.hostSide = host.pump(now);

        std::uint8_t toDevice[512];
        const std::size_t down = pair.device->receive(toDevice, sizeof(toDevice));
        outcome.deviceSide = sim.tick(toDevice, down, now);

        std::uint8_t fromDevice[512];
        const std::size_t up = sim.readOutgoing(fromDevice, sizeof(fromDevice));
        if (up > 0U) {
            pair.device->send(fromDevice, up);
        }
        return outcome;
    }

    mc::HostSession::PumpResult run(std::size_t rounds, std::uint32_t dtMs = 10U) {
        mc::HostSession::PumpResult total;
        for (std::size_t i = 0U; i < rounds; ++i) {
            const StepOutcome outcome = step(dtMs);
            total.bytesRead += outcome.hostSide.bytesRead;
            total.framesDecoded += outcome.hostSide.framesDecoded;
            total.telemetryAccepted += outcome.hostSide.telemetryAccepted;
            total.telemetryRejected += outcome.hostSide.telemetryRejected;
            total.acksMatched += outcome.hostSide.acksMatched;
            total.naksReceived += outcome.hostSide.naksReceived;
            total.unexpectedFrames += outcome.hostSide.unexpectedFrames;
            for (const mc::TimeoutEvent& event : outcome.hostSide.timeouts) {
                total.timeouts.push_back(event);
            }
            total.sessionLost = total.sessionLost || outcome.hostSide.sessionLost;
            total.handshakeCompleted = total.handshakeCompleted || outcome.hostSide.handshakeCompleted;
            total.handshakeRoundFailed =
                total.handshakeRoundFailed || outcome.hostSide.handshakeRoundFailed;
            total.transportOpen = outcome.hostSide.transportOpen;
        }
        return total;
    }

    // 让一条命令的往返走完（命令下去 + 应答上来）。
    mc::HostSession::PumpResult exchange() { return run(2U); }

    // 只读不产：把链路上在途的字节读干净，但不让设备再发新东西。
    // 每拍末尾设备都必然发出一帧，所以循环结束时总有一帧"在途"——
    // 想断言字节守恒，必须先这样排空，否则测到的是时序快照而不是协议。
    mc::HostSession::PumpResult drainHost(std::size_t rounds = 3U) {
        mc::HostSession::PumpResult total;
        for (std::size_t i = 0U; i < rounds; ++i) {
            const mc::HostSession::PumpResult partial = host.pump(now);
            total.bytesRead += partial.bytesRead;
            total.telemetryAccepted += partial.telemetryAccepted;
            total.telemetryRejected += partial.telemetryRejected;
            total.acksMatched += partial.acksMatched;
            total.naksReceived += partial.naksReceived;
            total.unexpectedFrames += partial.unexpectedFrames;
        }
        return total;
    }
};

// 绕过 HostSession 的跟踪器，直接往链路上灌一帧。
// 用来构造"重传 / 重复帧"这种上位机正常不会主动制造的情形。
bool sendRawCommand(Rig& rig, mc::MessageType type, std::uint16_t seq,
                    const std::vector<std::uint8_t>& payload) {
    mc::Frame frame;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;

    const mc::EncodeResult encoded = mc::encodeFrame(frame);
    if (!encoded) {
        return false;
    }
    return rig.pair.host->send(encoded.bytes.data(), encoded.bytes.size());
}

mc::LinkMonitor::Config monitorCfg(std::uint64_t periodMs = 1000U) {
    mc::LinkMonitor::Config c;
    c.heartbeatPeriodMs = periodMs;
    c.missedBeatsToDeclareLinkDown = 3U;
    c.initialAckTimeoutMs = 200U;
    c.timeoutMarginPercent = 300U;
    c.minAckTimeoutMs = 50U;
    c.maxAckTimeoutMs = 2000U;
    c.rttSampleCapacity = 16U;
    return c;
}

// 最朴素的"上位机 ↔ 设备"回环：只推进一拍，不做任何自动握手、不隐藏任何帧。
// Rig 会替调用方把会话握到手（干净链路前提），因此它测不了"链路从第一拍就是坏的"；
// 需要那种场景的用例用这个。
struct BareLink {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    mc::DeviceSimulator sim;
    std::uint64_t now = 0U;

    mc::HostSession::PumpResult step(mc::HostSession& host, std::uint32_t dtMs = 10U) {
        now += dtMs;
        const mc::HostSession::PumpResult hostSide = host.pump(now);

        std::uint8_t toDevice[512];
        const std::size_t down = pair.device->receive(toDevice, sizeof(toDevice));
        (void)sim.tick(toDevice, down, now);

        std::uint8_t fromDevice[512];
        const std::size_t up = sim.readOutgoing(fromDevice, sizeof(fromDevice));
        if (up > 0U) {
            pair.device->send(fromDevice, up);
        }
        return hostSide;
    }
};

// 把一段字节里**所有完整帧**解出来。用于"上位机到底发了什么类型"的断言 ——
// 这类断言只能看线上的东西，不能看本类的计数器，否则就是自证。
std::vector<mc::MessageType> decodeFrameTypes(const std::uint8_t* bytes, std::size_t len) {
    std::vector<mc::MessageType> types;
    std::size_t offset = 0U;
    while (offset < len) {
        const mc::DecodeResult decoded = mc::decodeFrame(bytes + offset, len - offset);
        if (!decoded) {
            break;
        }
        types.push_back(decoded.frame.type);
        offset += decoded.consumed;
    }
    return types;
}

bool isMotionCommand(mc::MessageType type) {
    return type == mc::MessageType::Start || type == mc::MessageType::SetTarget;
}

}  // namespace

// ---------------------------------------------------------------------------
// 正常链路
// ---------------------------------------------------------------------------

TEST(EndToEnd, FullCommandSequenceIsAcknowledged) {
    Rig rig;

    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetPid(mc::SetPid{1.0F, 0.5F, 0.01F}, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(100.0F, rig.nextSeq(), rig.now));

    const mc::HostSession::PumpResult result = rig.exchange();

    EXPECT_EQ(result.acksMatched, std::size_t{4});
    EXPECT_EQ(result.naksReceived, std::size_t{0});
    EXPECT_TRUE(result.timeouts.empty());
    EXPECT_EQ(rig.host.tracker().pendingCount(), std::size_t{0});

    EXPECT_EQ(rig.sim.runMode(), mc::SimRunMode::Velocity);
    EXPECT_TRUE(rig.sim.isEnabled());
    EXPECT_FLOAT_EQ(rig.sim.target(), 100.0F);
    EXPECT_FLOAT_EQ(rig.sim.pid().kp, 1.0F);

    // 内部不变式：两侧都不该有编码失败。
    EXPECT_EQ(rig.host.encodeFailures(), 0ULL);
    EXPECT_EQ(rig.sim.encodeFailures(), 0ULL);
}

TEST(EndToEnd, TelemetryFlowsAndMotionConvergesToTarget) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(100.0F, rig.nextSeq(), rig.now));
    rig.exchange();

    const mc::HostSession::PumpResult result = rig.run(60U);   // 600 ms

    EXPECT_GT(result.telemetryAccepted, std::size_t{40});
    EXPECT_EQ(result.telemetryRejected, std::size_t{0});
    ASSERT_TRUE(rig.host.hasTelemetry());
    EXPECT_NEAR(rig.host.lastTelemetry().velocityDegPerSec, 100.0F, 1.0F);
    EXPECT_GT(rig.host.lastTelemetry().positionDeg, 0.0F);
    EXPECT_GT(rig.host.lastTelemetry().pwmOutput, 0U);
    EXPECT_EQ(rig.host.lastTelemetry().state, static_cast<std::uint8_t>(mc::DeviceState::Running));
}

// ---------------------------------------------------------------------------
// 设备侧拒绝（非法状态 / 非法参数）
// ---------------------------------------------------------------------------

// **D2（阶段 4 裁定的行为变更）**：SET_TARGET 在 Ready（未启动）也放行。
//
// 改的理由不是"方便"，是安全：若只允许 Running 下设定目标，操作员就必须
// "先 START 再 SET_TARGET" —— 而 START 打开输出时用的是**上一次的旧目标**，
// 电机会先朝那个值动一下。"先设目标、再启动"没有这个瞬动。
// 放行 ≠ 会动：本用例同时断言电机**确实没动**。
TEST(EndToEnd, TargetBeforeStartIsAcceptedButDoesNotMoveTheMotor) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    rig.exchange();
    ASSERT_EQ(rig.sim.state(), mc::DeviceState::Ready);

    ASSERT_TRUE(rig.host.sendSetTarget(50.0F, rig.nextSeq(), rig.now));
    const mc::HostSession::PumpResult result = rig.exchange();

    EXPECT_EQ(result.acksMatched, std::size_t{1});
    EXPECT_EQ(result.naksReceived, std::size_t{0});
    EXPECT_FLOAT_EQ(rig.sim.target(), 50.0F);
    EXPECT_FALSE(rig.sim.isEnabled()) << "只是记住了目标，输出仍未使能";

    // 再跑一段，确认电机没动 —— 「放行」不能被误解成「开始运动」。
    rig.run(30U);
    EXPECT_FLOAT_EQ(rig.sim.velocityDegPerSec(), 0.0F);
    EXPECT_FLOAT_EQ(rig.sim.positionDeg(), 0.0F);
    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Ready);
}

// **D3**：SET_MODE 会改变 target 的**单位语义**（速度环是 deg/s，位置环是 deg），
// 运动中切换等于把同一个数值换成了另一种含义 —— 必须先停下来再改。
TEST(EndToEnd, SetModeWhileRunningIsRejectedWithIllegalState) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(80.0F, rig.nextSeq(), rig.now));
    rig.exchange();
    ASSERT_EQ(rig.sim.state(), mc::DeviceState::Running);

    ASSERT_TRUE(rig.host.sendSetMode(2U, rig.nextSeq(), rig.now));
    const mc::HostSession::PumpResult result = rig.exchange();

    ASSERT_EQ(result.naksReceived, std::size_t{1});
    EXPECT_EQ(rig.host.receivedNacks()[0].reason, mc::NackReason::IllegalState);
    EXPECT_EQ(rig.sim.runMode(), mc::SimRunMode::Velocity) << "被拒的模式切换不得生效";
    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Running) << "被拒不得打断已开始的运动";

    // 被拒之后必须**停止等待**，否则会白等到超时、甚至把被拒的运动指令误判成失控。
    EXPECT_EQ(rig.host.tracker().pendingCount(), std::size_t{0});
    EXPECT_EQ(rig.host.tracker().stats().rejectedByPeer, 1ULL);
    EXPECT_TRUE(result.timeouts.empty());
}

TEST(EndToEnd, StartWithoutModeIsRejected) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    const mc::HostSession::PumpResult result = rig.exchange();

    ASSERT_EQ(result.naksReceived, std::size_t{1});
    EXPECT_EQ(rig.host.receivedNacks()[0].reason, mc::NackReason::IllegalState);
    EXPECT_FALSE(rig.sim.isEnabled());
}

TEST(EndToEnd, OutOfRangePidGainIsRejectedByTheDevice) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetPid(mc::SetPid{-1.0F, 0.0F, 0.0F}, rig.nextSeq(), rig.now));
    const mc::HostSession::PumpResult result = rig.exchange();

    ASSERT_EQ(result.naksReceived, std::size_t{1});
    EXPECT_EQ(rig.host.receivedNacks()[0].reason, mc::NackReason::BadParameter);
    EXPECT_FLOAT_EQ(rig.sim.pid().kp, 0.0F) << "非法参数绝不能被写入设备状态";
}

// ---------------------------------------------------------------------------
// 单向停止
// ---------------------------------------------------------------------------

TEST(EndToEnd, StopIsUnidirectionalAndHaltsMotion) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(200.0F, rig.nextSeq(), rig.now));
    rig.run(40U);
    ASSERT_GT(rig.sim.velocityDegPerSec(), 50.0F);

    const mc::HostSession::PumpResult beforeStop = rig.run(1U);
    ASSERT_TRUE(rig.host.sendStop(rig.nextSeq()));
    const mc::HostSession::PumpResult afterStop = rig.run(60U);

    EXPECT_EQ(afterStop.acksMatched, std::size_t{0}) << "STOP 不回 ACK（ADR-004）";
    EXPECT_EQ(rig.host.tracker().pendingCount(), std::size_t{0}) << "STOP 不登记等待";
    EXPECT_FALSE(rig.sim.isEnabled());
    EXPECT_NEAR(rig.sim.velocityDegPerSec(), 0.0F, 0.5F);

    // 停下来之后仍应正常上报遥测 —— 只是目标归零。
    EXPECT_GT(afterStop.telemetryAccepted, std::size_t{40});
    EXPECT_FLOAT_EQ(rig.host.lastTelemetry().target, 0.0F);
    EXPECT_EQ(rig.host.lastTelemetry().state, static_cast<std::uint8_t>(mc::DeviceState::Ready));
    (void)beforeStop;
}

// ---------------------------------------------------------------------------
// 链路故障：这是"协议真的在保护系统"的唯一证据
// ---------------------------------------------------------------------------

// ADR-003 的端到端体现：链路整段丢字节 → 运动指令超时判 Fault，**绝不重发**。
TEST(EndToEnd, DroppedLinkMakesMotionCommandFaultInsteadOfRetrying) {
    Rig rig;
    rig.pair.host->outgoingFaults().dropEveryNthByte = 1U;   // 每个字节都丢

    ASSERT_TRUE(rig.host.sendSetTarget(120.0F, rig.nextSeq(), rig.now));
    ASSERT_EQ(rig.framesSent(), 1ULL);

    const mc::HostSession::PumpResult result = rig.run(30U);   // 300 ms > 默认 200 ms 超时

    ASSERT_EQ(result.timeouts.size(), std::size_t{1});
    EXPECT_EQ(result.timeouts[0].requestType, mc::MessageType::SetTarget);
    EXPECT_EQ(result.timeouts[0].outcome, mc::TxOutcome::Fault);

    // 关键断言：没有重发 —— 场景里上位机总共只发出过这一帧。
    EXPECT_EQ(rig.framesSent(), 1ULL);
    EXPECT_EQ(rig.host.tracker().stats().timedOutRetried, 0ULL);
    EXPECT_EQ(rig.host.tracker().stats().timedOutFaulted, 1ULL);
    EXPECT_EQ(rig.host.tracker().pendingCount(), std::size_t{0});
    EXPECT_EQ(rig.sim.target(), 0.0F) << "设备从未收到任何命令";
}

// 链路损坏 → 坏帧在解码器就被挡住，**永远不会变成"非法遥测"进入数据通路**。
//
// 注意：这里刻意**不断言具体是哪一道关卡拒绝的**。取反的字节可能落在长度字段上
// （被 LEN 上限挡住）、也可能落在载荷上（被 CRC 挡住）、还可能落在 SOF 上（触发重同步）——
// 落点决定走哪条路径。真正要守的性质是"没有任何坏数据被当成好数据接收"。
TEST(EndToEnd, CorruptedLinkYieldsNoTelemetryAndLosesNoBytes) {
    Rig rig;

    // 损坏必须**从第一个字节起**就生效。
    // 故障注入作用在发送侧，已经在链路上的字节不会被追溯损坏 ——
    // 若在中途才打开，之前那一帧仍会完好到达，断言就会漏判。
    rig.pair.device->outgoingFaults().corruptEveryNthByte = 2U;

    // 注意：这三条命令走的是**下行**链路（健康），所以设备照样会被成功使能并开始上报；
    // 被损坏的是它的**上行**应答与遥测。
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(80.0F, rig.nextSeq(), rig.now));

    std::size_t telemetrySentByDevice = 0U;
    std::size_t accepted = 0U;
    std::size_t rejected = 0U;
    for (std::size_t round = 0U; round < 40U; ++round) {
        const Rig::StepOutcome outcome = rig.step();
        telemetrySentByDevice += outcome.deviceSide.telemetrySent;
        accepted += outcome.hostSide.telemetryAccepted;
        rejected += outcome.hostSide.telemetryRejected;
    }

    // --- 两条正对照：先证明链路真的在传、真的在坏，下面的"零"才有意义 ---
    EXPECT_GT(telemetrySentByDevice, std::size_t{30}) << "设备确实在发遥测";
    EXPECT_GT(rig.pair.device->outgoingStats().bytesCorrupted, 0ULL) << "链路确实在损坏字节";

    // --- 结论 ---
    EXPECT_EQ(accepted, std::size_t{0}) << "一条遥测都不许进来";
    EXPECT_EQ(rejected, std::size_t{0})
        << "而且不是被应用层拒绝的 —— 它们在解码器里就已经不是帧了";
    EXPECT_EQ(rig.framesDecoded(), 0ULL);

    // 拒绝确实发生过（总拒绝活动 > 0），只是不锁定具体哪一道关卡。
    const mc::StreamDecoder::Stats& stats = rig.host.decoder().stats();
    const std::uint64_t rejectionActivity =
        stats.framesCrcError + stats.framesBadLength + stats.framesBadVersion + stats.bytesDiscarded;
    EXPECT_GT(rejectionActivity, 0ULL);
}

// 伪造的 NaN 遥测：结构性合法（长度对），语义非法 —— 必须被拒绝，且不能覆盖上一个好样本。
TEST(EndToEnd, ForgedNanTelemetryIsRejectedAndDoesNotReplaceTheLastGoodSample) {
    Rig rig;

    const auto sendTelemetry = [&rig](const mc::Telemetry& value, std::uint16_t seq) {
        mc::Frame frame;
        frame.type = mc::MessageType::Telemetry;
        frame.seq = seq;
        frame.payload = mc::encodeTelemetry(value);
        const mc::EncodeResult encoded = mc::encodeFrame(frame);
        EXPECT_TRUE(encoded);
        return rig.pair.device->send(encoded.bytes.data(), encoded.bytes.size());
    };

    mc::Telemetry good;
    good.velocityDegPerSec = 42.0F;
    good.positionDeg = 7.0F;
    ASSERT_TRUE(sendTelemetry(good, 1U));

    mc::HostSession::PumpResult result = rig.host.pump(rig.now);
    ASSERT_EQ(result.telemetryAccepted, std::size_t{1});
    ASSERT_TRUE(rig.host.hasTelemetry());
    EXPECT_FLOAT_EQ(rig.host.lastTelemetry().velocityDegPerSec, 42.0F);

    mc::Telemetry forged;
    forged.velocityDegPerSec = std::numeric_limits<float>::quiet_NaN();
    forged.positionDeg = 7.0F;
    ASSERT_TRUE(sendTelemetry(forged, 2U));

    result = rig.host.pump(rig.now);

    EXPECT_EQ(result.telemetryAccepted, std::size_t{0});
    EXPECT_EQ(result.telemetryRejected, std::size_t{1});
    EXPECT_FLOAT_EQ(rig.host.lastTelemetry().velocityDegPerSec, 42.0F)
        << "最后一个好样本必须被保留，不能被 NaN 覆盖";
    EXPECT_EQ(rig.telemetryAccepted(), 1ULL);
    EXPECT_EQ(rig.host.telemetryRejected(), 1ULL);
}

// 健康链路 + 排空之后：字节守恒，且解码器不残留半帧。
TEST(EndToEnd, HealthyLinkDeliversEveryByteAndStrandsNothing) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(150.0F, rig.nextSeq(), rig.now));
    rig.run(30U);

    // 只读不产地排空，把在途字节读干净（再推进 tick 只会又产生新的在途数据）。
    const mc::HostSession::PumpResult drained = rig.drainHost();
    EXPECT_GT(drained.bytesRead, std::size_t{0}) << "确实读到了在途数据，否则这条排空没意义";

    EXPECT_EQ(rig.pair.device->outgoingStats().bytesSent,
              rig.pair.host->incomingStats().bytesReceived)
        << "设备发出的字节必须全部被上位机读走";

    EXPECT_EQ(rig.host.decoder().stats().framesCrcError, 0ULL);
    EXPECT_EQ(rig.host.decoder().stats().framesBadLength, 0ULL);
    EXPECT_EQ(rig.host.decoder().stats().bytesDiscarded, 0ULL);
    EXPECT_EQ(rig.host.decoder().pending(), std::size_t{0})
        << "排空后暂存区里不该剩下任何字节 —— 否则说明卡在半帧上";
    EXPECT_GT(rig.host.telemetryAccepted(), 0ULL);
}

TEST(EndToEnd, TransportCloseIsReportedByPump) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    rig.exchange();

    rig.pair.device->close();

    const mc::HostSession::PumpResult result = rig.host.pump(rig.now + 1000U);
    EXPECT_FALSE(result.transportOpen);
}

// ---------------------------------------------------------------------------
// 帧级不变式
// ---------------------------------------------------------------------------

// NAK 的帧头 SEQ 必须回填被拒的 seq —— 上位机才能用"统一规则"做相关。
TEST(EndToEnd, DeviceNackEchoesRejectedSeqInBothHeaderAndPayload) {
    mc::DeviceSimulator sim;

    mc::Frame request;
    request.type = mc::MessageType::SetTarget;
    request.seq = 0x1234U;
    request.payload = mc::encodeSetTarget(mc::SetTarget{10.0F});
    const mc::EncodeResult encoded = mc::encodeFrame(request);
    ASSERT_TRUE(encoded);

    sim.tick(encoded.bytes.data(), encoded.bytes.size(), 0U);

    std::uint8_t out[256];
    const std::size_t count = sim.readOutgoing(out, sizeof(out));
    ASSERT_GT(count, std::size_t{0});

    const mc::DecodeResult decoded = mc::decodeFrame(out, count);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.frame.type, mc::MessageType::Nack);
    EXPECT_EQ(decoded.frame.seq, 0x1234U);

    mc::Nack nack;
    ASSERT_EQ(mc::decodeNack(decoded.frame.payload.data(), decoded.frame.payload.size(), nack),
              mc::PayloadError::None);
    EXPECT_EQ(nack.rejectedSeq, 0x1234U);
    EXPECT_EQ(nack.reason, mc::NackReason::IllegalState);
}

// ---------------------------------------------------------------------------
// 设备侧序列号去重
// ---------------------------------------------------------------------------

// 重复命令必须"**重放应答**但不重复执行"。
// 只去重不重放是不够的：对端丢了 ACK 后重传会一个应答都拿不到，白等到超时 ——
// 那样幂等命令的重传机制就被去重机制废掉了。
TEST(EndToEnd, DuplicateSequenceReplaysTheReplyWithoutReExecuting) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    rig.exchange();
    ASSERT_TRUE(rig.sim.isEnabled());

    // 第一次 SET_TARGET(seq=7) → 目标 100。
    ASSERT_TRUE(sendRawCommand(rig, mc::MessageType::SetTarget, 7U,
                               mc::encodeSetTarget(mc::SetTarget{100.0F})));
    rig.step();
    ASSERT_FLOAT_EQ(rig.sim.target(), 100.0F);

    // 同一个 seq **第二次**到达，而且载荷被刻意改成 999。
    // 真正的重传不会改载荷 —— 这里改掉正是为了把"重放应答"与"重新执行"分开：
    // 若重新执行，目标会变成 999。
    ASSERT_TRUE(sendRawCommand(rig, mc::MessageType::SetTarget, 7U,
                               mc::encodeSetTarget(mc::SetTarget{999.0F})));
    const Rig::StepOutcome outcome = rig.step();

    EXPECT_EQ(outcome.deviceSide.duplicatesSuppressed, std::size_t{1});
    EXPECT_EQ(outcome.deviceSide.acksSent, std::size_t{1}) << "必须重放应答，不能只是沉默";
    EXPECT_FLOAT_EQ(rig.sim.target(), 100.0F) << "重复帧绝不能重复执行副作用";
    EXPECT_EQ(rig.sim.nacksSent().size(), std::size_t{0}) << "重放的是原来的 ACK，不是 NAK";
    EXPECT_EQ(rig.sim.encodeFailures(), 0ULL);
}

// F7（设备侧通信丢失保护）的**正面证据**：运行中链路真的静默之后，
// 设备必须自己把车停下来并进入 Fault，而不是带着陈旧目标一直转。
TEST(EndToEnd, CommLossWhileRunningStopsTheMotorAndEntersFault) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(200.0F, rig.nextSeq(), rig.now));
    rig.run(40U);   // 400 ms

    // 正对照：先证明它确实在跑 —— 否则"它停下来了"什么都证明不了。
    ASSERT_EQ(rig.sim.state(), mc::DeviceState::Running);
    ASSERT_GT(rig.sim.velocityDegPerSec(), 50.0F);

    // 此后上位机一个字都不发。保护窗口 3000 ms，这里多跑一倍有余。
    rig.run(700U);

    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Fault);
    EXPECT_TRUE(rig.sim.faultLatched());
    EXPECT_EQ(rig.sim.faultCode(), mc::FaultCode::CommunicationLoss);
    EXPECT_FLOAT_EQ(rig.sim.target(), 0.0F);
    EXPECT_NEAR(rig.sim.velocityDegPerSec(), 0.0F, 1.0F)
        << "设备必须自己停车，不能带着陈旧目标一直转";
    EXPECT_EQ(rig.host.lastTelemetry().state, static_cast<std::uint8_t>(mc::DeviceState::Fault));
}

// F7 的**负对照**：把保护窗口关掉（0 = 关闭），同样长度的静默就不再停车。
// 这正是没有 F7 保护时的行为 —— 用它来证明上一条测的是保护本身，而不是别的东西。
TEST(EndToEnd, WithoutTheCommLossWindowTheDeviceKeepsRunningOnAStaleTarget) {
    mc::SimulatorConfig config;
    config.commLossTimeoutMs = 0U;
    Rig rig(config);

    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(200.0F, rig.nextSeq(), rig.now));
    rig.run(700U);   // 与上一条同样长的一段静默

    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Running);
    EXPECT_FALSE(rig.sim.faultLatched());
    EXPECT_NEAR(rig.sim.velocityDegPerSec(), 200.0F, 1.0F)
        << "上位机早就不说话了，电机还在追那个陈旧目标 —— 这就是 F7 缺陷本身";
}

// HELLO 是会话起点，必须清空去重窗口。
// 否则上位机重启后从 seq=0 重新编号，新命令会被当成旧会话的重传静默吞掉 ——
// 表现为"握手成功但一条命令都不生效"。
TEST(EndToEnd, HelloClearsTheDedupWindowSoARestartedHostIsNotSilentlyIgnored) {
    Rig rig;
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    rig.exchange();

    ASSERT_TRUE(sendRawCommand(rig, mc::MessageType::SetTarget, 7U,
                               mc::encodeSetTarget(mc::SetTarget{100.0F})));
    rig.step();
    ASSERT_FLOAT_EQ(rig.sim.target(), 100.0F);

    // 上位机重启：重新握手。
    ASSERT_TRUE(sendRawCommand(rig, mc::MessageType::Hello, 0U, {}));
    rig.step();

    // 再用同一个 seq=7 发一条**新**命令 —— 它必须生效。
    ASSERT_TRUE(sendRawCommand(rig, mc::MessageType::SetTarget, 7U,
                               mc::encodeSetTarget(mc::SetTarget{250.0F})));
    const Rig::StepOutcome outcome = rig.step();

    EXPECT_EQ(outcome.deviceSide.duplicatesSuppressed, std::size_t{0});
    EXPECT_FLOAT_EQ(rig.sim.target(), 250.0F) << "新会话的命令被当成旧会话的重传吞掉了";
}

// ---------------------------------------------------------------------------
// 实测 RTT 撑开超时窗口
// ---------------------------------------------------------------------------

// 这是演示场景④暴露的问题的正面解决：硬编码 200 ms 的窗口挡不住更长的往返，
// 会把**运动指令**误判成超时 → Fault。按实测 RTT 撑开窗口之后就不该再误判。
TEST(EndToEnd, MeasuredRttWidensTheTimeoutSoHighLatencyDoesNotFaultAMotionCommand) {
    Rig rig;
    // 上行延迟 60 次接收 —— 明显超过原来的硬编码 200 ms 窗口。
    rig.pair.device->outgoingFaults().latencyPolls = 60U;
    rig.host.enableLinkMonitor(monitorCfg(), [&rig]() { return rig.nextSeq(); });

    // 先让监控器用心跳把真实 RTT 量出来。
    for (std::size_t round = 0U; round < 600U; ++round) {
        rig.step();
    }

    ASSERT_TRUE(rig.host.linkMonitor()->hasRttSample())
        << "监控器必须拿到 RTT 样本，否则下面的自适应无从谈起";
    const std::uint64_t rtt = *rig.host.linkMonitor()->rttP95Ms();
    const std::uint64_t effectiveWindow = rig.host.tracker().timeoutMs();

    // --- 正对照：延迟确实超过了原来的窗口 ---
    // 若没超过，这个用例就什么都没证明 —— 窗口根本没被撑开的必要。
    EXPECT_GT(rtt, 200ULL);
    EXPECT_GT(effectiveWindow, 200ULL) << "窗口必须按实测 RTT 撑开，不能停在硬编码的 200 ms";
    EXPECT_EQ(effectiveWindow, rig.host.linkMonitor()->recommendedAckTimeoutMs());

    // 命令的 seq 从高位起编，避免与心跳用过的低位 seq 撞车
    // （AckTracker 按 seq 判重、不看类型）。
    rig.seq = 40000U;

    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(120.0F, rig.nextSeq(), rig.now));

    std::size_t timeoutCount = 0U;
    for (std::size_t round = 0U; round < 200U; ++round) {
        timeoutCount += rig.step().hostSide.timeouts.size();
    }

    EXPECT_EQ(timeoutCount, std::size_t{0}) << "高延迟链路不该把运动指令误判成超时";
    EXPECT_EQ(rig.host.tracker().stats().timedOutFaulted, 0ULL);
    EXPECT_EQ(rig.host.tracker().pendingCount(), std::size_t{0});
    EXPECT_FLOAT_EQ(rig.sim.target(), 120.0F) << "指令确实到了设备";
}

// 心跳漏拍只算"漏了一拍"，绝不能被判成故障。
TEST(EndToEnd, AHeartbeatTimeoutIsNotReportedAsAFault) {
    Rig rig;
    rig.host.enableLinkMonitor(monitorCfg(200U), [&rig]() { return rig.nextSeq(); });

    // 让整条链路彻底坏掉：设备发出的字节全丢。
    rig.pair.device->outgoingFaults().dropEveryNthByte = 1U;

    std::size_t heartbeatTimeouts = 0U;
    std::size_t heartbeatFaults = 0U;
    for (std::size_t round = 0U; round < 100U; ++round) {
        for (const mc::TimeoutEvent& event : rig.step().hostSide.timeouts) {
            if (event.requestType == mc::MessageType::Heartbeat) {
                ++heartbeatTimeouts;
                if (event.outcome == mc::TxOutcome::Fault) {
                    ++heartbeatFaults;
                }
            }
        }
    }

    // 正对照：心跳确实发出去了、而且确实超时了 —— 否则下面的"零"没意义。
    ASSERT_GT(rig.host.linkMonitor()->stats().heartbeatsSent, 0ULL);
    ASSERT_GT(heartbeatTimeouts, std::size_t{0}) << "心跳确实超时了";

    EXPECT_GT(rig.host.tracker().stats().timedOutDropped, 0ULL);
    // ⚠ 口径必须**逐事件**判。链路断掉之后握手层的 HELLO 也会用尽重试并判 Fault
    // （那是它该做的事），拿 tracker 的聚合计数会把那笔账记到心跳头上。
    EXPECT_EQ(heartbeatFaults, std::size_t{0}) << "心跳漏拍被判成故障就说明 Dropped 语义没生效";

    // 连续丢 N 拍之后链路确实被判失效。
    EXPECT_TRUE(rig.step().hostSide.linkDown);
}

// ---------------------------------------------------------------------------
// 会话生命周期：自动握手、退避重开、断链重连
// ---------------------------------------------------------------------------

// 会话生命周期是**可选**能力，和链路监控一样：没有序列号来源就不启用 ——
// 而不是启用一个发不出 HELLO 的半成品，然后让调用方以为自己在握手。
TEST(EndToEnd, HandshakeIsNotHalfEnabledWithoutASequenceSource) {
    BareLink link;
    mc::HostSession host(*link.pair.host);
    host.enableHandshake(mc::HostSession::SeqSource{});

    EXPECT_FALSE(host.handshakeEnabled());

    for (std::size_t i = 0U; i < 20U; ++i) {
        (void)link.step(host);
    }
    EXPECT_EQ(host.framesSent(), 0ULL) << "没启用就不该有任何自发流量";
    EXPECT_FALSE(host.sessionReady());
}

// 链路从第一拍就是坏的：一轮一轮地试、退避逐轮拉长，而且**永不放弃**
// —— "要不要放弃"是上位机的决定，本层只如实报数。链路一恢复就该自己握上手。
TEST(EndToEnd, FailedHandshakeRoundsBackOffAndReopenUntilTheLinkComesBack) {
    BareLink link;
    std::uint16_t seq = 0U;
    mc::HostSession host(*link.pair.host);
    host.enableHandshake([&seq]() { return seq++; });

    // 下行全丢：HELLO 永远到不了设备。
    link.pair.host->outgoingFaults().dropEveryNthByte = 1U;

    std::vector<std::uint64_t> roundStarts;
    std::uint64_t seenRounds = 0U;
    for (std::size_t i = 0U; i < 900U; ++i) {
        (void)link.step(host);
        if (host.handshakeRounds() != seenRounds) {
            seenRounds = host.handshakeRounds();
            roundStarts.push_back(link.now);
        }
    }

    ASSERT_GE(roundStarts.size(), std::size_t{3})
        << "链路一直不通时必须持续重开轮次，而不是发一次就沉默";
    EXPECT_FALSE(host.sessionReady()) << "一次都没成功过，不该就绪";

    // 退避必须**真的**在增长，否则就是拿固定周期对着断链猛打。
    const std::uint64_t firstGap = roundStarts[1] - roundStarts[0];
    const std::uint64_t lastGap = roundStarts.back() - roundStarts[roundStarts.size() - 2];
    EXPECT_GT(lastGap, firstGap) << "重试间隔没有退避";
    // 而且要有上限：不能膨胀到再也不试。
    // 一轮最长 3 × 200 ms（首发 + 2 次重发）+ 退避上限 1600 ms。
    EXPECT_LE(lastGap, 3000ULL) << "退避上限没生效";

    // --- 恢复链路：必须**自己**握上手 ---
    link.pair.host->outgoingFaults() = mc::LinkFaultConfig{};
    bool reportedCompletion = false;
    for (std::size_t i = 0U; i < 3000U && !host.sessionReady(); ++i) {
        const mc::HostSession::PumpResult outcome = link.step(host);
        reportedCompletion = reportedCompletion || outcome.handshakeCompleted;
    }
    EXPECT_TRUE(host.sessionReady()) << "链路恢复后必须自动重连，而不是等人重启程序";
    EXPECT_TRUE(reportedCompletion) << "转入就绪的那一拍必须被上报出来";
}

// 修掉的缺陷要留下判据：`AckTracker` 判了"可重传"，`pump()` 就必须**真的重发**。
// 只判不发的后果很隐蔽 —— 请求会一直挂在等待表里，而调用方因为 canRegister
// 已经为 false，连自己重发都做不到（sendCommand 会直接返回 false）。
TEST(EndToEnd, ARetryableRequestIsActuallyRetransmittedOnTheWire) {
    BareLink link;
    std::uint16_t seq = 0U;
    mc::HostSession host(*link.pair.host);
    host.enableHandshake([&seq]() { return seq++; });

    link.pair.host->outgoingFaults().dropEveryNthByte = 1U;   // 链路一直不通

    // 610 ms = 首发(t=10) + 第 1 次重发(t=210) + 第 2 次重发(t=410) + 判定用尽(t=610)。
    // 默认 maxRetries = 2，所以一轮恰好 3 帧。
    for (std::size_t i = 0U; i < 61U; ++i) {
        (void)link.step(host);
    }

    EXPECT_EQ(host.handshakeRounds(), 1ULL);
    EXPECT_EQ(host.framesSent(), 3ULL)
        << "首发 1 次 + 重发 2 次；『只判不发』的缺陷会让它停在 1";
    EXPECT_EQ(host.tracker().stats().timedOutRetried, 2ULL);
    EXPECT_EQ(host.tracker().stats().retriesExhausted, 1ULL);
}

// 断链路径上的两条安全裁定，**看线上实际发了什么**（不看本类计数器，免得自证）：
//   ① 已登记的运动指令在应答丢失后绝不重发（ADR-003）；
//   ② 在途请求以**超时**结清，而不是被静默丢弃。
TEST(EndToEnd, AckLossNeverResendsAMotionCommandAndSettlesItAsATimeout) {
    BareLink link;
    std::uint16_t seq = 0U;
    mc::HostSession host(*link.pair.host);
    host.enableHandshake([&seq]() { return seq++; });

    for (std::size_t i = 0U; i < 30U && !host.sessionReady(); ++i) {
        (void)link.step(host);
    }
    ASSERT_TRUE(host.sessionReady());

    // 上行断掉：命令到得了设备，应答回不来 —— 这正是"要不要重发"的诱因。
    link.pair.device->outgoingFaults().dropEveryNthByte = 1U;

    const std::uint64_t framesBefore = host.framesSent();
    ASSERT_TRUE(host.sendSetTarget(120.0F, seq++, link.now));

    std::vector<mc::MessageType> sentTypes;
    for (std::size_t i = 0U; i < 80U; ++i) {
        link.now += 10U;
        (void)host.pump(link.now);

        std::uint8_t bytes[512];
        const std::size_t n = link.pair.device->receive(bytes, sizeof(bytes));
        for (const mc::MessageType type : decodeFrameTypes(bytes, n)) {
            sentTypes.push_back(type);
        }
        (void)link.sim.tick(bytes, n, link.now);

        std::uint8_t fromDevice[512];
        const std::size_t up = link.sim.readOutgoing(fromDevice, sizeof(fromDevice));
        if (up > 0U) {
            link.pair.device->send(fromDevice, up);
        }
    }

    // --- ① 线上事实：只发过那一条，一次都没重发 ---
    ASSERT_EQ(sentTypes.size(), std::size_t{1}) << "断链期间上位机只该发过那一条命令";
    EXPECT_EQ(sentTypes[0], mc::MessageType::SetTarget);
    EXPECT_EQ(host.framesSent() - framesBefore, 1ULL);

    // --- ② 结清方式：超时，且判 Fault（安全事件），不是默默消失 ---
    const mc::AckStats& stats = host.tracker().stats();
    EXPECT_EQ(stats.timedOutRetried, 0ULL) << "运动指令根本不该被判成『可重传』";
    EXPECT_EQ(stats.timedOutFaulted, 1ULL);
    EXPECT_EQ(host.tracker().pendingCount(), std::size_t{0}) << "不该永远挂在等待表里";
    EXPECT_EQ(link.sim.target(), 120.0F) << "命令确实到了设备（丢的是应答，不是命令）";
}

// 断链 → 会话作废 → 自动重连，全流程走一遍。三条安全红线一起验：
//   ① 重连成功**绝不**自动恢复运动 —— 故障锁存着，必须人工再来一次；
//   ② 断链路径上**绝不**重发已登记的运动指令（线上抓帧判定）；
//   ③ 在途请求以**超时**结清，不被静默丢弃。
TEST(EndToEnd, LinkFailureTearsDownTheSessionAndReconnectNeverResumesMotion) {
    Rig rig;
    // 断链判定(≈150 ms)要**明显快于**命令的超时预算(900 ms)：
    // 这样"断链那一刻仍有在途请求"是确定的，而不是碰巧撞上的时序。
    mc::LinkMonitor::Config fastLoss = monitorCfg(50U);
    fastLoss.minAckTimeoutMs = 900U;
    rig.host.enableLinkMonitor(fastLoss, [&rig]() { return rig.nextSeq(); });

    // 先把设备真的跑起来 —— 否则"重连后没有自动继续跑"什么都证明不了。
    ASSERT_TRUE(rig.host.sendSetMode(1U, rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendStart(rig.nextSeq(), rig.now));
    ASSERT_TRUE(rig.host.sendSetTarget(200.0F, rig.nextSeq(), rig.now));
    rig.run(40U);
    ASSERT_EQ(rig.sim.state(), mc::DeviceState::Running);
    ASSERT_GT(rig.sim.velocityDegPerSec(), 50.0F);

    // 等超时窗口按实测 RTT 收敛上来再发命令：否则它会带着初始 200 ms 的窗口
    // 与"断链判定"撞在同一时间尺度上，断言就变成碰运气。
    for (std::size_t i = 0U; i < 200U && rig.host.tracker().timeoutMs() < 900ULL; ++i) {
        rig.run(1U);
    }
    ASSERT_GE(rig.host.tracker().timeoutMs(), 900ULL);

    // ② / ③ 的诱因：一条运动指令刚发出去，应答还没回来就断链。
    const std::uint16_t targetSeq = rig.nextSeq();
    ASSERT_TRUE(rig.host.sendSetTarget(250.0F, targetSeq, rig.now));

    // 双向断链：上行断 → 心跳无应答 → 链路判失效；下行断 → 设备一个字节都收不到 → F7 生效。
    rig.pair.host->outgoingFaults().dropEveryNthByte = 1U;
    rig.pair.device->outgoingFaults().dropEveryNthByte = 1U;

    bool sessionLost = false;
    for (std::size_t i = 0U; i < 300U && !sessionLost; ++i) {
        sessionLost = rig.step().hostSide.sessionLost;
    }
    ASSERT_TRUE(sessionLost) << "链路失效必须作废会话，而不是继续假装就绪";
    EXPECT_FALSE(rig.host.sessionReady());

    // ③ 断链**不许**把在途请求从等待表里抹掉 —— 它必须自己走完超时。
    // 静默丢弃的后果：这条运动指令既不报超时也不报应答，凭空消失（ADR-003 要求它必须被上报）。
    EXPECT_TRUE(rig.host.tracker().isPending(targetSeq))
        << "断链顺手丢弃了在途请求";

    const mc::HostSession::PumpResult outage = rig.run(600U);   // 再跑 6 s，远超 F7 的 3 s 窗口

    EXPECT_TRUE(outage.handshakeRoundFailed) << "一轮 HELLO 用尽重试必须被上报";
    EXPECT_GT(rig.host.handshakeRounds(), 1ULL) << "会话作废后必须自己重开握手轮次";

    // 设备侧：F7 必须让它自己停下来并**锁存**故障。
    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Fault);
    EXPECT_TRUE(rig.sim.faultLatched());
    EXPECT_EQ(rig.sim.faultCode(), mc::FaultCode::CommunicationLoss);

    // ② ③：在途的运动指令必须以**超时(Fault)**结清，且一次都没被重发。
    std::size_t motionFaults = 0U;
    std::size_t motionRetries = 0U;
    for (const mc::TimeoutEvent& event : outage.timeouts) {
        if (!isMotionCommand(event.requestType)) {
            continue;
        }
        if (event.outcome == mc::TxOutcome::Fault) {
            ++motionFaults;
        } else if (event.outcome == mc::TxOutcome::Retry) {
            ++motionRetries;
        }
    }
    EXPECT_EQ(motionFaults, std::size_t{1}) << "那条在途的运动指令必须恰好以 Fault 结清一次";
    EXPECT_EQ(motionRetries, std::size_t{0}) << "ADR-003：断链路径上一条运动指令都不许重发";
    // 正对照：可重传的 HELLO 确实一直在重发 —— 否则上面的"零"只是因为压根没有重发机制。
    EXPECT_GT(rig.host.tracker().stats().timedOutRetried, 0ULL);

    // --- 恢复链路：会话必须自己重新握上手 ---
    rig.pair.host->outgoingFaults() = mc::LinkFaultConfig{};
    rig.pair.device->outgoingFaults() = mc::LinkFaultConfig{};
    for (std::size_t i = 0U; i < 600U && !rig.host.sessionReady(); ++i) {
        rig.run(1U);
    }
    EXPECT_TRUE(rig.host.sessionReady()) << "链路恢复后必须自动重连";

    // ① 最关键的一条：重连**不等于**恢复运动。
    EXPECT_EQ(rig.sim.state(), mc::DeviceState::Fault) << "重连不得把锁存的故障洗掉";
    EXPECT_TRUE(rig.sim.faultLatched());
    EXPECT_FALSE(rig.sim.isEnabled()) << "重连之后绝不允许电机自己转起来";
    EXPECT_NEAR(rig.sim.velocityDegPerSec(), 0.0F, 1.0F);
    EXPECT_EQ(rig.sim.runMode(), mc::SimRunMode::Velocity) << "模式是配置，本来就不该被清掉";
}

// ---------------------------------------------------------------------------
// 设备侧产帧速率（遥测补发）
//
// 真实设备在自己的定时器上产帧，**不会**因为上位机取得晚就少发。所以产帧
// 必须按设备自己的周期补，而不是"每拍最多一帧" —— 后者的实际帧率会变成
// 上位机的 tick 率，把链路的能力测成了上位机的取数节奏。
// ---------------------------------------------------------------------------

TEST(EndToEnd, DeviceEmitsTelemetryOnItsOwnPeriodNotTheHostTickRate) {
    mc::SimulatorConfig config;
    config.telemetryPeriodMs = 10U;   // 与 tick 间隔相同：每拍一帧
    mc::DeviceSimulator fast(config);

    std::uint64_t now = 0U;
    std::size_t frames = 0U;
    for (int i = 0; i < 10; ++i) {
        now += 10U;
        frames += fast.tick(nullptr, 0U, now).telemetrySent;
    }
    // 首拍只用来对齐产帧基准（否则会把"上电之前"也算成产帧时间），故 10 拍得 9 帧。
    EXPECT_EQ(frames, std::size_t{9});

    mc::SimulatorConfig dense;
    dense.telemetryPeriodMs = 1U;     // 比 tick 密 10 倍
    mc::DeviceSimulator stress(dense);

    now = 0U;
    frames = 0U;
    for (int i = 0; i < 10; ++i) {
        now += 10U;
        frames += stress.tick(nullptr, 0U, now).telemetrySent;
    }
    // 这才是有意义的那一半：设备该产 10 倍，就得产 10 倍。
    EXPECT_EQ(frames, std::size_t{90});
}

TEST(EndToEnd, DeviceCapsPerTickTelemetryAndCountsWhatItCouldNotHandOver) {
    mc::SimulatorConfig config;
    config.telemetryPeriodMs = 1U;
    mc::DeviceSimulator sim(config);

    ASSERT_TRUE(sim.tick(nullptr, 0U, 0U).telemetrySent == 0U) << "首拍只对齐基准";

    // 中间隔了 100 ms 没人来取：该产 100 帧，但单拍上限是 16 帧。
    const mc::DeviceSimulator::TickResult stalled = sim.tick(nullptr, 0U, 100U);
    EXPECT_EQ(stalled.telemetrySent, mc::kMaxTelemetryFramesPerTick);
    EXPECT_EQ(stalled.telemetryBacklogDropped, 100U - mc::kMaxTelemetryFramesPerTick);
    EXPECT_EQ(sim.telemetryBacklogDropped(), 100U - mc::kMaxTelemetryFramesPerTick);
    // 上限必须能被链路一次性搬走：一帧 30 B × 16 = 480 B < 单次交换上限 512 B。
    EXPECT_LE(mc::kMaxTelemetryFramesPerTick * 30U, 512U);

    // 欠发归欠发，基准已经按整数个周期前进过了 —— 不能因为"上一拍丢了"就
    // 把丢失的量摊到后面每一拍上，那会让帧率永久性地偏。
    const mc::DeviceSimulator::TickResult next = sim.tick(nullptr, 0U, 110U);
    EXPECT_EQ(next.telemetrySent, std::size_t{10});
    EXPECT_EQ(next.telemetryBacklogDropped, std::size_t{0});
}

// 周期 0 是"来取就发一帧"。这条用例的价值在**它必须能返回**：
// 补发循环写的是 `nowMs - lastTelemetryMs_ >= period`，period 为 0 时恒成立，
// 照补发算就是一个死循环 —— 所以这两条路径必须显式分开。
TEST(EndToEnd, ZeroTelemetryPeriodMeansOneFramePerTickAndDoesNotSpin) {
    mc::SimulatorConfig config;
    config.telemetryPeriodMs = 0U;
    mc::DeviceSimulator sim(config);

    EXPECT_EQ(sim.tick(nullptr, 0U, 0U).telemetrySent, std::size_t{1});
    EXPECT_EQ(sim.tick(nullptr, 0U, 10U).telemetrySent, std::size_t{1});
    EXPECT_EQ(sim.tick(nullptr, 0U, 5000U).telemetrySent, std::size_t{1}) << "隔多久也只发一帧";
    EXPECT_EQ(sim.telemetryBacklogDropped(), 0ULL);
}

// 补发不能只在模拟器内部成立：帧得**真的**顺着链路到上位机，
// 并且上位机要全部解出来（一帧不丢、一帧不拒）。
TEST(EndToEnd, DenseTelemetryReallyArrivesAtTheHostOverTheLink) {
    mc::SimulatorConfig config;
    config.telemetryPeriodMs = 1U;
    Rig rig(config);

    const mc::HostSession::PumpResult result = rig.run(10U);   // 100 ms

    // 10 拍 × 每拍 10 帧；第 10 拍产出的那一批还在链路上（本拍的读发生在产之前），
    // 加上握手排空过一轮，故正好 9 拍的量。
    EXPECT_EQ(result.telemetryAccepted, std::size_t{90});
    EXPECT_EQ(result.telemetryRejected, std::size_t{0});
    EXPECT_EQ(result.unexpectedFrames, std::size_t{0});
    EXPECT_EQ(rig.sim.encodeFailures(), 0ULL);
    EXPECT_EQ(rig.sim.telemetryBacklogDropped(), 0ULL) << "单拍 10 帧没到上限，不该有欠发";
}

// 时钟只给到毫秒时，每拍产几帧会在 0 / 1 / 2 之间抖 —— 但**总帧数必须守恒**。
//
// 这条是从实测里追出来的：45 s 档某次运行上位机收了 4469 帧，而显示缓冲只写进
// 3856 条（差值不是丢帧，见下）。上位机把 `QElapsedTimer::elapsed()` 的整毫秒数
// 当 tick 时刻传下来，于是 dt 的真实值就带上了 ±1 ms 的量化抖动：
//   t=10 → 该产 1 帧，产 1；t=19（只走了 9 ms）→ 一帧都不该产，产 0；
//   t=30（这回一次走了 11 ms）→ 该产 2 帧，产 2。
// 一条 9 ms 的步长会让某一拍收不到帧，随后的 11 ms 步长又把这笔补回来。
// 关键是**平均恒为 1.000 帧/拍**：协议侧一帧不丢，只是"上位机哪一拍读到"不同 ——
// 而显示缓冲是"一拍只收最后一帧"，所以它的写入条数会略少于收帧数。
TEST(EndToEnd, MillisecondQuantizedClockDithersTheFrameCountWithoutLosingAny) {
    mc::SimulatorConfig config;
    config.telemetryPeriodMs = 10U;
    mc::DeviceSimulator sim(config);

    // 真实间隔 10 ms，但读数被量化成 10 / 9 / 11 / 11 / 11 / 11（首拍用于对齐基准）。
    const std::uint64_t ticks[] = {0U, 10U, 19U, 30U, 41U, 52U, 63U};
    const std::size_t expected[] = {0U, 1U, 0U, 2U, 1U, 1U, 1U};

    std::size_t total = 0U;
    for (std::size_t i = 0U; i < 7U; ++i) {
        const std::size_t sent = sim.tick(nullptr, 0U, ticks[i]).telemetrySent;
        EXPECT_EQ(sent, expected[i]) << "第 " << i << " 拍（t=" << ticks[i] << " ms）";
        total += sent;
    }

    // 守恒：首拍（t=0）只用于对齐基准，此后跨度 63 ms，10 ms 一个周期 →
    // 必须**恰好**产 6 帧。抖的是"哪一拍产"，不是"总共产多少"。
    EXPECT_EQ(total, std::size_t{6}) << "63 ms / 10 ms = 6 帧，与抖动无关";
    EXPECT_EQ(sim.telemetryBacklogDropped(), 0ULL);
}
