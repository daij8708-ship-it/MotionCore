#include "mc/device_simulator.hpp"

#include <algorithm>
#include <cmath>

namespace mc {
namespace {

// 单次积分的最大步长。长时间没有 tick 之后（例如界面卡了一下），
// 不能让一次 dt 把速度一次性推到目标 —— 那会掩盖控制器的问题。
constexpr double kMaxIntegrationStepSeconds = 0.5;

}  // namespace

DeviceSimulator::DeviceSimulator(SimulatorConfig config) : config_(config) {}

TransitionResult DeviceSimulator::applyEvent(DeviceEvent event) noexcept {
    const TransitionResult result = stateMachine_.apply(event);
    if (!result.accepted) {
        return result;
    }

    if (result.stateChanged && stateMachine_.is(DeviceState::Fault)) {
        // 进 Fault 就撤销目标：故障期间绝不能让电机继续追一个陈旧的目标值。
        // 速度仍按 accelDegPerSec2 限幅收敛到零 —— 那是"可控停车"，
        // 比把 PWM 一刀切断更安全（后者会让机械结构承受一次冲击）。
        target_ = 0.0F;
        if (faultCode_ == FaultCode::None) {
            faultCode_ = (config_.forcedFaultCode != FaultCode::None) ? config_.forcedFaultCode
                                                                     : FaultCode::Forced;
        }
    }

    if (result.stateChanged && stateMachine_.is(DeviceState::Ready)) {
        // 能回到 Ready 只有两条路：Initializing 握手成功、或走完 Recovery。
        // 两条路都不该再挂着故障码。
        faultCode_ = FaultCode::None;
    }

    return result;
}

DeviceSimulator::TickResult DeviceSimulator::tick(const std::uint8_t* incoming, std::size_t len,
                                                  std::uint64_t nowMs) {
    TickResult result;

    integrate(nowMs);

    if (incoming != nullptr && len > 0U) {
        const std::vector<Frame> frames = decoder_.feed(incoming, len);
        result.framesReceived = frames.size();
        if (!frames.empty()) {
            // 只要有一帧过了 CRC，就算"通信还活着" —— 这正是 F7 的判据。
            // 用"收到帧"而不是"收到字节"：线上一直有噪声但一帧都过不了 CRC，
            // 那不叫通信正常。
            hasReceivedFrame_ = true;
            lastValidFrameMs_ = nowMs;
        }
        for (const Frame& frame : frames) {
            handleFrame(frame, result);
        }
    }

    runSafetyChecks(nowMs, result);

    emitDueTelemetry(nowMs, result);

    return result;
}

void DeviceSimulator::emitDueTelemetry(std::uint64_t nowMs, TickResult& result) {
    const std::uint64_t period = config_.telemetryPeriodMs;
    if (period == 0U) {
        // 周期 0 必须显式分开写，不能让它落进下面的补发公式：那里的除法会
        // **整数除零**（变异 M33 实测：去掉这个分支，用例当场 SIGFPE 崩溃）。
        // 语义定为"来取就发一帧"。
        lastTelemetryMs_ = nowMs;
        emitTelemetry(result);
        return;
    }

    const std::uint64_t due = (nowMs - lastTelemetryMs_) / period;
    if (due == 0U) {
        return;
    }

    // 基准按**整数个周期**前进，而不是直接跳到 nowMs。
    // 跳到 nowMs 会把"不足一个周期的那点零头"吃掉，长期看就是帧数偏少；
    // 按周期前进则零头一直留着，累计误差恒小于一个周期。
    lastTelemetryMs_ += due * period;

    if (due > static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick)) {
        const std::uint64_t dropped = due - static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick);
        telemetryBacklogDropped_ += dropped;
        result.telemetryBacklogDropped = static_cast<std::size_t>(dropped);
    }

    const std::uint64_t toSend = std::min(due, static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick));
    // 补发的这几帧内容相同：运动模型一拍只积分一次，帧之间没有新的采样点。
    // 它们的作用是**帧率压力**（解码、缓冲、绘制的负载），不代表真实设备那种
    // 每个采样点都不同的遥测密度 —— 引用这些数字时必须带上这句。
    for (std::uint64_t i = 0U; i < toSend; ++i) {
        emitTelemetry(result);
    }
}

void DeviceSimulator::integrate(std::uint64_t nowMs) {
    if (!hasTicked_) {
        hasTicked_ = true;
        lastTickMs_ = nowMs;
        lastTelemetryMs_ = nowMs;
        return;
    }
    if (nowMs <= lastTickMs_) {
        // 时钟回退（换了时间源 / 测试直接给了更小的值）：不积分，只对齐基准。
        // 遥测基准也必须一起回退 —— 否则 nowMs - lastTelemetryMs_ 在无符号数上
        // 会变成一个巨大的值，接下来每一拍都发一帧遥测。
        lastTickMs_ = nowMs;
        if (lastTelemetryMs_ > nowMs) {
            lastTelemetryMs_ = nowMs;
        }
        return;
    }

    const double rawDt = static_cast<double>(nowMs - lastTickMs_) / 1000.0;
    lastTickMs_ = nowMs;
    const double dt = std::min(rawDt, kMaxIntegrationStepSeconds);

    // 只有 Running 才追目标。Stopping / Fault 期间有效目标恒为 0，速度受
    // 同一个 accel 限幅收敛到零 —— 这条路径同时是"停机"和"故障后停车"。
    const float effectiveTarget =
        (stateMachine_.is(DeviceState::Running) && runMode_ != SimRunMode::Stopped) ? target_ : 0.0F;

    const double maxDelta = static_cast<double>(config_.accelDegPerSec2) * dt;
    double delta = static_cast<double>(effectiveTarget) - static_cast<double>(velocity_);
    delta = std::max(-maxDelta, std::min(maxDelta, delta));

    velocity_ = static_cast<float>(static_cast<double>(velocity_) + delta);
    position_ += velocity_ * static_cast<float>(dt);
}

void DeviceSimulator::runSafetyChecks(std::uint64_t nowMs, TickResult& result) {
    // ① 设备侧通信丢失保护（F7）。
    //    只保护 Running：别的状态下没有输出，超时无需动作。
    if (config_.commLossTimeoutMs > 0U && hasReceivedFrame_ &&
        stateMachine_.is(DeviceState::Running) &&
        (nowMs - lastValidFrameMs_) > config_.commLossTimeoutMs) {
        faultCode_ = FaultCode::CommunicationLoss;
        result.commLossFaulted = applyEvent(DeviceEvent::FaultRaised).accepted;
    }

    // ② 停机判稳。STOP 是单向的（ADR-004），设备拿不到"已停"的回执，
    //    只能靠自己把"速度已经收敛"翻译成一次状态迁移，上位机才有办法
    //    把 Stopping 和 Ready 分开显示。
    if (stateMachine_.is(DeviceState::Stopping) &&
        std::fabs(velocity_) <= config_.stopVelocityThresholdDegPerSec) {
        applyEvent(DeviceEvent::StopCompleted);
    }

    // ③ 故障注入（测试用）。
    if (config_.forcedFaultCode != FaultCode::None && !stateMachine_.is(DeviceState::Fault)) {
        faultCode_ = config_.forcedFaultCode;
        applyEvent(DeviceEvent::FaultRaised);
    }
}

void DeviceSimulator::handleFrame(const Frame& frame, TickResult& result) {
    // HELLO 必须**绕过去重窗口**（而且不记入窗口）。
    //
    // 不走这条特例会出现一个自指的僵局：上位机重启后仍从 seq=0 发 HELLO，
    // 而窗口里恰好还留着上一会话的 (Hello, 0) —— 于是这条 HELLO 被当成重传
    // 直接回放掉，`recent_.clear()` 永远不会执行，新会话随后的所有命令
    // 都会与旧会话的 seq 撞车并被静默吞掉。现象正是"握手看起来成功了，
    // 但一条命令都不生效"。
    //
    // HELLO 不需要去重是合理的：它没有业务副作用，只重置会话状态，
    // 且本身幂等；应答丢了再来一次反而正是我们想要的。
    if (frame.type == MessageType::Hello) {
        (void)executeFrame(frame, result);
        return;
    }

    if (const DedupEntry* seen = findRecent(frame.type, frame.seq)) {
        // 重传：重放上一次的应答，但**不重复执行副作用**。
        ++result.duplicatesSuppressed;
        replayReply(*seen, result);
        return;
    }

    const Reply reply = executeFrame(frame, result);
    rememberReply(frame.type, frame.seq, reply);
}

DeviceSimulator::Reply DeviceSimulator::executeFrame(const Frame& frame, TickResult& result) {
    // 门禁先行：状态机说不行就立刻回 NAK，**不产生任何副作用**。
    // 放在 switch 之前而不是散在各 case 里，是为了保证"新增命令类型时
    // 不可能忘记过门禁"—— 这是唯一一个入口。
    if (stateMachine_.verdictFor(frame.type) == CommandVerdict::RejectIllegalState) {
        return replyWithNack(frame.seq, NackReason::IllegalState, result);
    }

    switch (frame.type) {
        case MessageType::Hello:
            // HELLO 是会话起点，因此清空去重窗口。
            // 不清的后果很隐蔽：上位机重启后会从 seq=0 重新编号，那些命令会被
            // 当成上一次会话的重传而全部静默吞掉 —— 表现为"握手成功但一条命令都不生效"。
            recent_.clear();
            // ⚠️ 若存在**未确认的故障**，状态机会把结果判成 Fault 而不是 Ready ——
            // 重连不能顺带把故障洗掉。此时仍然回 ACK：协议层面的握手确实成功了，
            // 设备的状态由随后的遥测如实上报。
            applyEvent(DeviceEvent::HandshakeOk);
            return replyWithAck(MessageType::HelloAck, frame.seq, result);

        case MessageType::Heartbeat:
            return replyWithAck(MessageType::HeartbeatAck, frame.seq, result);

        case MessageType::SetMode: {
            // 门禁已保证此刻处于 Ready —— 运动中途改模式会换掉 target 的单位语义。
            SetMode payload;
            if (decodeSetMode(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetModeValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            runMode_ = static_cast<SimRunMode>(payload.mode);
            if (runMode_ == SimRunMode::Stopped) {
                target_ = 0.0F;
            }
            return replyWithAck(MessageType::SetModeAck, frame.seq, result);
        }

        case MessageType::Start:
            if (runMode_ == SimRunMode::Stopped) {
                // 门禁放行了（确实是 Ready），但"没选控制模式就使能"在语义上不成立。
                // 这不是状态机该管的事：状态没问题，缺的是配置。
                return replyWithNack(frame.seq, NackReason::IllegalState, result);
            }
            applyEvent(DeviceEvent::StartAccepted);
            return replyWithAck(MessageType::StartAck, frame.seq, result);

        case MessageType::Stop:
            applyEvent(DeviceEvent::StopReceived);
            target_ = 0.0F;
            // 单向：不回 ACK（ADR-004）。停止是幂等的安全动作，重复无副作用。
            return Reply{};

        case MessageType::SetTarget: {
            // 门禁已保证是 Ready 或 Running。Ready 也放行的理由见 device_state.cpp。
            SetTarget payload;
            if (decodeSetTarget(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetTargetValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            target_ = payload.target;
            return replyWithAck(MessageType::SetTargetAck, frame.seq, result);
        }

        case MessageType::SetPid: {
            SetPid payload;
            if (decodeSetPid(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetPidValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            pid_ = payload;
            return replyWithAck(MessageType::SetPidAck, frame.seq, result);
        }

        default:
            // 未知类型、以及设备不该收到的 ACK 帧，一律回 NAK 并说明原因。
            ++result.unknownTypes;
            return replyWithNack(frame.seq, NackReason::UnknownType, result);
    }
}

DeviceSimulator::Reply DeviceSimulator::replyWithAck(MessageType ackType, std::uint16_t seq,
                                                     TickResult& result) {
    queueAck(ackType, seq, result);
    Reply reply;
    reply.kind = Reply::Kind::Ack;
    reply.ackType = ackType;
    return reply;
}

DeviceSimulator::Reply DeviceSimulator::replyWithNack(std::uint16_t seq, NackReason reason,
                                                      TickResult& result) {
    queueNack(seq, reason, result);
    Reply reply;
    reply.kind = Reply::Kind::Nack;
    reply.reason = reason;
    return reply;
}

void DeviceSimulator::replayReply(const DedupEntry& entry, TickResult& result) {
    switch (entry.reply.kind) {
        case Reply::Kind::Ack:
            queueAck(entry.reply.ackType, entry.seq, result);
            break;
        case Reply::Kind::Nack:
            queueNack(entry.seq, entry.reply.reason, result);
            break;
        case Reply::Kind::None:
            // STOP：单向、无应答，且本身幂等 —— 重放就是"什么都不做"。
            break;
    }
}

void DeviceSimulator::rememberReply(MessageType type, std::uint16_t seq, const Reply& reply) {
    DedupEntry entry;
    entry.type = type;
    entry.seq = seq;
    entry.reply = reply;
    recent_.push_back(entry);

    if (recent_.size() > kSimulatorDedupWindowSize) {
        recent_.erase(recent_.begin());
    }
}

const DeviceSimulator::DedupEntry* DeviceSimulator::findRecent(MessageType type,
                                                              std::uint16_t seq) const noexcept {
    for (const DedupEntry& entry : recent_) {
        if (entry.type == type && entry.seq == seq) {
            return &entry;
        }
    }
    return nullptr;
}

void DeviceSimulator::emitTelemetry(TickResult& result) {
    Telemetry telemetry;
    telemetry.timestampMs = static_cast<std::uint32_t>(lastTickMs_);
    telemetry.velocityDegPerSec = velocity_;
    telemetry.positionDeg = position_;
    // 上报的是**有效目标**：Stopping / Fault 期间设备实际在追 0，
    // 若照实报旧目标，界面会显示"设备还在往那个值跑"。
    telemetry.target =
        (stateMachine_.is(DeviceState::Running) && runMode_ != SimRunMode::Stopped) ? target_ : 0.0F;
    telemetry.state = static_cast<std::uint8_t>(stateMachine_.state());
    telemetry.faultCode = static_cast<std::uint8_t>(faultCode_);

    float ratio = std::fabs(velocity_) / config_.maxVelocityDegPerSec;
    ratio = std::max(0.0F, std::min(1.0F, ratio));
    telemetry.pwmOutput =
        static_cast<std::uint16_t>(ratio * static_cast<float>(config_.pwmFullScale) + 0.5F);

    lastTelemetry_ = telemetry;

    queueFrame(MessageType::Telemetry, outboundSeq_, encodeTelemetry(telemetry));
    ++outboundSeq_;
    ++result.telemetrySent;
}

void DeviceSimulator::queueFrame(MessageType type, std::uint16_t seq,
                                const std::vector<std::uint8_t>& payload) {
    Frame frame;
    frame.version = kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;

    const EncodeResult encoded = encodeFrame(frame);
    if (!encoded) {
        // 不静默吞掉：计入内部不变式计数器，由测试断言它为 0。
        ++encodeFailures_;
        return;
    }
    outgoing_.insert(outgoing_.end(), encoded.bytes.begin(), encoded.bytes.end());
}

void DeviceSimulator::queueAck(MessageType ackType, std::uint16_t seq, TickResult& result) {
    queueFrame(ackType, seq, {});
    ++result.acksSent;
}

void DeviceSimulator::queueNack(std::uint16_t rejectedSeq, NackReason reason, TickResult& result) {
    Nack nack;
    nack.rejectedSeq = rejectedSeq;
    nack.reason = reason;

    nacksSent_.push_back(nack);

    // 帧头 SEQ 也回填被拒的 seq —— 这样上位机可以用**统一规则**做相关：
    // "任何响应帧的帧头 SEQ 都指向它对应的请求"。载荷里那一份是权威记录，
    // 两者必须一致（由端到端用例断言）。
    queueFrame(MessageType::Nack, rejectedSeq, encodeNack(nack));
    ++result.nacksSent;
}

std::size_t DeviceSimulator::readOutgoing(std::uint8_t* out, std::size_t maxLen) {
    if (out == nullptr || maxLen == 0U) {
        return 0U;
    }
    const std::size_t count = std::min(maxLen, outgoing_.size());
    std::copy_n(outgoing_.begin(), static_cast<std::ptrdiff_t>(count), out);
    outgoing_.erase(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

}  // namespace mc
