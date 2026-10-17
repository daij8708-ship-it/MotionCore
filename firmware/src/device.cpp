#include "mc_fw/device.hpp"

#include <cstring>

namespace mcfw {
namespace {

bool finite(float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000U) != 0x7F800000U;
}

float absf(float value) noexcept {
    return value < 0.0F ? -value : value;
}

float clampf(float value, float low, float high) noexcept {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

float roundToWhole(float value) noexcept {
    return value >= 0.0F ? static_cast<float>(static_cast<std::int32_t>(value + 0.5F))
                         : static_cast<float>(static_cast<std::int32_t>(value - 0.5F));
}

}  // namespace

Device::Device(const Config& config) : config_(config) {
    reset(0U);
}

void Device::reset(std::uint32_t nowMs) {
    state_ = mc::DeviceStateMachine(mc::DeviceState::Initializing);
    faultCode_ = mc::FaultCode::None;

    runMode_ = 0U;
    target_ = 0.0F;
    kp_ = 0.0F;
    ki_ = 0.0F;
    kd_ = 0.0F;
    limits_ = Limits{};

    velocity_ = 0.0F;
    position_ = 0.0F;
    measuredVelocity_ = 0.0F;
    measuredPosition_ = 0.0F;
    lastCommand_ = 0.0F;
    pwmOutput_ = 0U;

    integralError_ = 0.0F;
    lastPidError_ = 0.0F;
    pidPrimed_ = false;

    homingAttained_ = false;
    homeSpeed_ = 0.0F;
    homeTimeoutMs_ = 0U;
    homeStartedMs_ = nowMs;

    hasReceivedFrame_ = false;
    lastValidFrameMs_ = nowMs;
    lastTickMs_ = nowMs;
    lastTelemetryMs_ = nowMs;
    lastFaultEventMs_ = nowMs;

    window_[0] = Sample{nowMs, 0.0F};
    windowHead_ = 1U;
    windowTail_ = 0U;
    windowCount_ = 1U;

    for (DedupEntry& entry : dedup_) {
        entry = DedupEntry{};
    }
    dedupHead_ = 0U;

    outHead_ = 0U;
    outTail_ = 0U;
    outCount_ = 0U;
    outboundSeq_ = 0U;
    stats_ = Stats{};
}

void Device::handleFrame(const ParsedFrame& frame, std::uint32_t nowMs) {
    ++stats_.framesHandled;
    hasReceivedFrame_ = true;
    lastValidFrameMs_ = nowMs;

    // HELLO 是会话起点：清空去重窗口，且自身不进入去重窗口。
    // 这两条缺一不可，理由与宿主侧 DeviceSimulator 完全相同。
    if (frame.type == mc::MessageType::Hello) {
        clearDedup();
        const ReplySpec reply = executeFrame(frame, nowMs);
        if (reply.hasReply) {
            queueFrame(reply.type, frame.seq, reply.payload, reply.payloadLen);
        }
        return;
    }

    if (const DedupEntry* seen = findDedup(frame.type, frame.seq)) {
        ++stats_.duplicatesSuppressed;
        if (seen->hasReply) {
            queueFrame(seen->replyType, frame.seq, seen->replyPayload, seen->replyLen);
        }
        return;
    }

    const ReplySpec reply = executeFrame(frame, nowMs);
    rememberReply(frame.type, frame.seq, reply);
    if (reply.hasReply) {
        queueFrame(reply.type, frame.seq, reply.payload, reply.payloadLen);
    }
}

Device::ReplySpec Device::ack(mc::MessageType type) const noexcept {
    ReplySpec reply;
    reply.hasReply = true;
    reply.type = type;
    return reply;
}

Device::ReplySpec Device::nack(std::uint16_t seq, mc::NackReason reason) const noexcept {
    ReplySpec reply;
    reply.hasReply = true;
    reply.type = mc::MessageType::Nack;
    reply.payloadLen = static_cast<std::uint8_t>(mc::kNackSize);
    mc::writeLe16(reply.payload, seq);
    reply.payload[2] = static_cast<std::uint8_t>(reason);
    return reply;
}

Device::ReplySpec Device::executeFrame(const ParsedFrame& frame, std::uint32_t nowMs) {
    if (state_.verdictFor(frame.type) == mc::CommandVerdict::RejectIllegalState) {
        return nack(frame.seq, mc::NackReason::IllegalState);
    }

    switch (frame.type) {
        case mc::MessageType::Hello:
            applyEvent(mc::DeviceEvent::HandshakeOk);
            return ack(mc::MessageType::HelloAck);

        case mc::MessageType::Heartbeat:
            return ack(mc::MessageType::HeartbeatAck);

        case mc::MessageType::SetMode: {
            if (frame.payloadLen != mc::kSetModeSize || frame.payload == nullptr ||
                frame.payload[0] > mc::kMaxRunMode) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            runMode_ = frame.payload[0];
            if (runMode_ == 0U) {
                target_ = 0.0F;
            }
            return ack(mc::MessageType::SetModeAck);
        }

        case mc::MessageType::Start: {
            if (runMode_ == 0U) {
                return nack(frame.seq, mc::NackReason::IllegalState);
            }
            if (!applyEvent(mc::DeviceEvent::StartAccepted).accepted) {
                return nack(frame.seq, mc::NackReason::IllegalState);
            }
            return ack(mc::MessageType::StartAck);
        }

        case mc::MessageType::Stop:
            applyEvent(mc::DeviceEvent::StopReceived);
            target_ = 0.0F;
            return ReplySpec{};

        case mc::MessageType::SetTarget: {
            if (frame.payloadLen != mc::kSetTargetSize || frame.payload == nullptr) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            const float value = mc::readLeFloat(frame.payload);
            if (!finite(value)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            // 位置环的目标站在限位之外就直接拒：不要等它撞到边界再报超限。
            if (runMode_ == 2U && limits_.enabled && !limitsAllow(value)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            target_ = value;
            return ack(mc::MessageType::SetTargetAck);
        }

        case mc::MessageType::SetPid: {
            if (frame.payloadLen != mc::kSetPidSize || frame.payload == nullptr) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            const float kp = mc::readLeFloat(frame.payload + 0U);
            const float ki = mc::readLeFloat(frame.payload + 4U);
            const float kd = mc::readLeFloat(frame.payload + 8U);
            const auto gainOk = [](float gain) {
                return finite(gain) && gain >= 0.0F && gain <= mc::kSanityMaxPidGain;
            };
            if (!gainOk(kp) || !gainOk(ki) || !gainOk(kd)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            kp_ = kp;
            ki_ = ki;
            kd_ = kd;
            return ack(mc::MessageType::SetPidAck);
        }

        case mc::MessageType::ReadParams: {
            if (frame.payloadLen != 0U) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            ReplySpec reply;
            reply.hasReply = true;
            reply.type = mc::MessageType::ReadParamsAck;
            reply.payloadLen = static_cast<std::uint8_t>(mc::kParamStateSize);
            reply.payload[0] = runMode_;
            mc::writeLeFloat(reply.payload + 1U, target_);
            mc::writeLeFloat(reply.payload + 5U, kp_);
            mc::writeLeFloat(reply.payload + 9U, ki_);
            mc::writeLeFloat(reply.payload + 13U, kd_);
            reply.payload[17] = limits_.enabled ? 1U : 0U;
            mc::writeLeFloat(reply.payload + 18U, limits_.minDeg);
            mc::writeLeFloat(reply.payload + 22U, limits_.maxDeg);
            return reply;
        }

        case mc::MessageType::Home: {
            if (frame.payloadLen != mc::kHomeParamSize || frame.payload == nullptr) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            const float speed = mc::readLeFloat(frame.payload);
            const std::uint32_t timeoutMs = mc::readLe32(frame.payload + 4U);
            if (!finite(speed) || speed <= 0.0F ||
                speed > mc::kSanityMaxHomeSpeedDegPerSec || timeoutMs == 0U ||
                timeoutMs > mc::kMaxHomeTimeoutMs) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            homeSpeed_ = speed < config_.maxVelocityDegPerSec ? speed
                                                             : config_.maxVelocityDegPerSec;
            homeTimeoutMs_ = timeoutMs;
            homeStartedMs_ = nowMs;
            homingAttained_ = false;
            runMode_ = 2U;
            target_ = 0.0F;
            resetPidState();
            if (!applyEvent(mc::DeviceEvent::HomeAccepted).accepted) {
                return nack(frame.seq, mc::NackReason::IllegalState);
            }
            return ack(mc::MessageType::HomeAck);
        }

        case mc::MessageType::SetLimits: {
            if (frame.payloadLen != mc::kSoftLimitsSize || frame.payload == nullptr) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            if (frame.payload[0] > 1U) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            const bool enabled = frame.payload[0] != 0U;
            const float minDeg = mc::readLeFloat(frame.payload + 1U);
            const float maxDeg = mc::readLeFloat(frame.payload + 5U);
            if (!finite(minDeg) || !finite(maxDeg) ||
                absf(minDeg) > mc::kSanityMaxAbsPositionDeg ||
                absf(maxDeg) > mc::kSanityMaxAbsPositionDeg) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            if (enabled && !(minDeg < maxDeg)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            if (enabled && (measuredPosition_ < minDeg || measuredPosition_ > maxDeg)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            limits_.enabled = enabled;
            limits_.minDeg = minDeg;
            limits_.maxDeg = maxDeg;
            return ack(mc::MessageType::SetLimitsAck);
        }

        case mc::MessageType::ClearFault: {
            if (frame.payloadLen != mc::kClearFaultSize || frame.payload == nullptr ||
                frame.payload[0] == 0U) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            if (frame.payload[0] != static_cast<std::uint8_t>(faultCode_)) {
                return nack(frame.seq, mc::NackReason::BadParameter);
            }
            const bool confirmed = applyEvent(mc::DeviceEvent::RecoveryConfirmed).accepted;
            const bool cleared = confirmed && applyEvent(mc::DeviceEvent::RecoveryOk).accepted;
            if (!cleared) {
                return nack(frame.seq, mc::NackReason::IllegalState);
            }
            return ack(mc::MessageType::ClearFaultAck);
        }

        default:
            return nack(frame.seq, mc::NackReason::UnknownType);
    }
}

void Device::rememberReply(mc::MessageType type, std::uint16_t seq, const ReplySpec& spec) {
    DedupEntry& entry = dedup_[dedupHead_];
    dedupHead_ = static_cast<std::uint8_t>((dedupHead_ + 1U) % kDedupSize);

    entry.used = true;
    entry.type = type;
    entry.seq = seq;
    entry.hasReply = spec.hasReply;
    entry.replyType = spec.type;
    entry.replyLen = spec.payloadLen;
    if (spec.payloadLen <= sizeof(entry.replyPayload)) {
        std::memcpy(entry.replyPayload, spec.payload, spec.payloadLen);
    } else {
        entry.hasReply = false;
        entry.replyLen = 0U;
    }
}

const Device::DedupEntry* Device::findDedup(mc::MessageType type,
                                            std::uint16_t seq) const noexcept {
    for (std::size_t i = 0; i < kDedupSize; ++i) {
        if (dedup_[i].used && dedup_[i].type == type && dedup_[i].seq == seq) {
            return &dedup_[i];
        }
    }
    return nullptr;
}

void Device::clearDedup() noexcept {
    for (DedupEntry& entry : dedup_) {
        entry = DedupEntry{};
    }
    dedupHead_ = 0U;
}

void Device::queueFrame(mc::MessageType type, std::uint16_t seq,
                        const std::uint8_t* payload, std::uint16_t payloadLen) {
    if (outCount_ >= kOutQueueSize) {
        ++stats_.outputDropped;
        return;
    }

    OutFrame& frame = outQueue_[outHead_];
    const std::size_t written = encodeFrame(type, seq, payload, payloadLen,
                                            frame.data, sizeof(frame.data));
    if (written == 0U) {
        ++stats_.outputDropped;
        return;
    }
    frame.len = static_cast<std::uint16_t>(written);
    outHead_ = static_cast<std::uint8_t>((outHead_ + 1U) % kOutQueueSize);
    ++outCount_;
}

std::size_t Device::popFrame(std::uint8_t* out, std::size_t capacity) noexcept {
    if (outCount_ == 0U) {
        return 0U;
    }
    const OutFrame& frame = outQueue_[outTail_];
    if (out == nullptr || capacity < frame.len) {
        return 0U;
    }
    std::memcpy(out, frame.data, frame.len);
    outTail_ = static_cast<std::uint8_t>((outTail_ + 1U) % kOutQueueSize);
    --outCount_;
    return frame.len;
}

void Device::tick(std::uint32_t nowMs) {
    if (nowMs == lastTickMs_) {
        return;
    }

    const std::uint32_t deltaMs = nowMs - lastTickMs_;
    lastTickMs_ = nowMs;
    float dtSeconds = static_cast<float>(deltaMs) / 1000.0F;
    if (dtSeconds > 0.5F) {
        dtSeconds = 0.5F;
    }

    integrate(dtSeconds);
    runSafetyChecks(nowMs);

    if ((nowMs - lastTelemetryMs_) >= config_.telemetryPeriodMs) {
        lastTelemetryMs_ = nowMs;
        queueTelemetry();
    }

    if (state_.is(mc::DeviceState::Fault) &&
        (nowMs - lastFaultEventMs_) >= config_.faultEventPeriodMs) {
        lastFaultEventMs_ = nowMs;
        queueFaultEvent();
    }
}

void Device::integrate(float dtSeconds) {
    const bool homing = state_.is(mc::DeviceState::Home);
    const bool driving = (state_.is(mc::DeviceState::Running) && runMode_ != 0U) || homing;

    float command = 0.0F;
    if (homing) {
        const float error = 0.0F - measuredPosition_;
        if (absf(error) <= config_.homeToleranceDeg) {
            command = 0.0F;
        } else {
            const float approach = config_.homeApproachSeconds > 0.001F
                                       ? config_.homeApproachSeconds
                                       : 0.001F;
            command = clampf(error / approach, -homeSpeed_, homeSpeed_);
        }
        resetPidState();
    } else if (driving) {
        if (runMode_ == 1U) {
            const float error = target_ - measuredVelocity_;
            command = target_ + pidOutput(error, dtSeconds, config_.maxVelocityDegPerSec);
        } else {
            const float error = target_ - measuredPosition_;
            command = pidOutput(error, dtSeconds, config_.maxVelocityDegPerSec);
        }
    } else {
        // 不驱动时把控制器状态清干净：否则停机期间积分继续累积，
        // 下一次启动会被一段没有来源的旧积分推一下。
        resetPidState();
    }

    lastCommand_ = command;

    const float maxDelta = config_.accelDegPerSec2 * dtSeconds;
    const float delta = clampf(command - velocity_, -maxDelta, maxDelta);
    float nextVelocity = velocity_ + delta;

    if (config_.inertiaTimeConstantMs > 0.0F) {
        const float tau = config_.inertiaTimeConstantMs / 1000.0F;
        const float alpha = dtSeconds / (tau + dtSeconds);
        nextVelocity = velocity_ + (nextVelocity - velocity_) * alpha;
    }

    velocity_ = nextVelocity;
    position_ += velocity_ * dtSeconds;
    updateMeasurement();
}

void Device::updateMeasurement() {
    const float countsPerDeg = config_.encoderCountsPerRev > 0.0F
                                   ? config_.encoderCountsPerRev / 360.0F
                                   : 0.0F;
    if (countsPerDeg > 0.0F) {
        measuredPosition_ = roundToWhole(position_ * countsPerDeg) / countsPerDeg;
    } else {
        measuredPosition_ = position_;
    }

    if (windowCount_ == kWindowSize) {
        windowTail_ = static_cast<std::uint8_t>((windowTail_ + 1U) % kWindowSize);
        --windowCount_;
    }
    window_[windowHead_] = Sample{lastTickMs_, measuredPosition_};
    windowHead_ = static_cast<std::uint8_t>((windowHead_ + 1U) % kWindowSize);
    ++windowCount_;

    const std::uint32_t windowMs = config_.velocityWindowMs > 1.0F
                                       ? static_cast<std::uint32_t>(config_.velocityWindowMs)
                                       : 1U;
    while (windowCount_ > 2U) {
        const std::uint32_t age = lastTickMs_ - window_[windowTail_].tMs;
        if (age <= windowMs) {
            break;
        }
        windowTail_ = static_cast<std::uint8_t>((windowTail_ + 1U) % kWindowSize);
        --windowCount_;
    }

    const Sample& oldest = window_[windowTail_];
    const std::uint8_t newestIndex =
        static_cast<std::uint8_t>((windowHead_ + kWindowSize - 1U) % kWindowSize);
    const Sample& newest = window_[newestIndex];
    const std::uint32_t span = newest.tMs - oldest.tMs;
    measuredVelocity_ = span > 0U
                            ? (newest.positionDeg - oldest.positionDeg) * 1000.0F /
                                  static_cast<float>(span)
                            : 0.0F;
}

void Device::runSafetyChecks(std::uint32_t nowMs) {
    // ① 设备侧通信丢失保护。只保护 Running：别的状态下没有输出。
    if (config_.commLossTimeoutMs > 0U && hasReceivedFrame_ &&
        state_.is(mc::DeviceState::Running) &&
        (nowMs - lastValidFrameMs_) > config_.commLossTimeoutMs) {
        faultCode_ = mc::FaultCode::CommunicationLoss;
        applyEvent(mc::DeviceEvent::FaultRaised);
    }

    // ② 停机判稳：用设备看得见的速度，不用内部真值。
    if (state_.is(mc::DeviceState::Stopping) &&
        absf(measuredVelocity_) <= config_.stopVelocityThresholdDegPerSec) {
        applyEvent(mc::DeviceEvent::StopCompleted);
    }

    // ③ 回零：到位或超时，二选一。
    if (state_.is(mc::DeviceState::Home)) {
        if (absf(measuredPosition_) <= config_.homeToleranceDeg) {
            defineOriginHere();
            applyEvent(mc::DeviceEvent::HomeCompleted);
        } else if (homeTimeoutMs_ > 0U &&
                   (nowMs - homeStartedMs_) > homeTimeoutMs_) {
            faultCode_ = mc::FaultCode::HomeFailed;
            applyEvent(mc::DeviceEvent::FaultRaised);
        }
    }

    // ④ 软限位：位置越界就撤输出并进故障。回零期间不判。
    if (limits_.enabled && !state_.is(mc::DeviceState::Home) &&
        state_.is(mc::DeviceState::Running) && !limitsAllow(measuredPosition_)) {
        faultCode_ = mc::FaultCode::OverLimit;
        applyEvent(mc::DeviceEvent::FaultRaised);
    }

    // ⑤ 超速保护。留 20% 余量，避免量化噪声把正常运动误判成超速。
    if ((state_.is(mc::DeviceState::Running) || state_.is(mc::DeviceState::Home)) &&
        absf(measuredVelocity_) > config_.maxVelocityDegPerSec * 1.2F) {
        faultCode_ = mc::FaultCode::OverSpeed;
        applyEvent(mc::DeviceEvent::FaultRaised);
    }
}

mc::TransitionResult Device::applyEvent(mc::DeviceEvent event) noexcept {
    const mc::TransitionResult result = state_.apply(event);
    if (!result.accepted) {
        return result;
    }

    if (result.stateChanged && state_.is(mc::DeviceState::Fault)) {
        target_ = 0.0F;
        if (faultCode_ == mc::FaultCode::None) {
            faultCode_ = mc::FaultCode::Forced;
        }
        // 让下一拍 tick 立刻补发一条故障事件。
        lastFaultEventMs_ = lastTickMs_ - config_.faultEventPeriodMs;
    }

    if (result.stateChanged && state_.is(mc::DeviceState::Ready)) {
        faultCode_ = mc::FaultCode::None;
    }

    return result;
}

void Device::defineOriginHere() {
    position_ = 0.0F;
    measuredPosition_ = 0.0F;
    velocity_ = 0.0F;
    measuredVelocity_ = 0.0F;
    lastCommand_ = 0.0F;
    target_ = 0.0F;
    homingAttained_ = true;

    window_[0] = Sample{lastTickMs_, 0.0F};
    windowHead_ = 1U;
    windowTail_ = 0U;
    windowCount_ = 1U;
}

void Device::resetPidState() noexcept {
    integralError_ = 0.0F;
    lastPidError_ = 0.0F;
    pidPrimed_ = false;
}

float Device::pidOutput(float error, float dtSeconds, float clampAbs) noexcept {
    integralError_ += error * dtSeconds;

    float derivative = 0.0F;
    if (pidPrimed_ && dtSeconds > 0.0F) {
        derivative = (error - lastPidError_) / dtSeconds;
    }
    pidPrimed_ = true;
    lastPidError_ = error;

    float output = kp_ * error + ki_ * integralError_ + kd_ * derivative;

    // 抗积分饱和：输出被限幅时，把多出来的那部分积分退回去。
    if (output > clampAbs || output < -clampAbs) {
        const float excess = output > clampAbs ? output - clampAbs : output + clampAbs;
        if (ki_ != 0.0F) {
            integralError_ -= excess / ki_;
        }
        output = output > clampAbs ? clampAbs : -clampAbs;
    }
    return output;
}

bool Device::limitsAllow(float positionDeg) const noexcept {
    if (!limits_.enabled) {
        return true;
    }
    if (!finite(positionDeg)) {
        return false;
    }
    return positionDeg >= limits_.minDeg && positionDeg <= limits_.maxDeg;
}

void Device::queueTelemetry() {
    std::uint8_t payload[mc::kTelemetrySize]{};
    mc::writeLe32(payload + 0U, lastTickMs_);
    mc::writeLeFloat(payload + 4U, measuredVelocity_);
    mc::writeLeFloat(payload + 8U, measuredPosition_);
    const float reportedTarget =
        (state_.is(mc::DeviceState::Running) && runMode_ != 0U) ? target_ : 0.0F;
    mc::writeLeFloat(payload + 12U, reportedTarget);
    payload[16] = static_cast<std::uint8_t>(state_.state());
    payload[17] = static_cast<std::uint8_t>(faultCode_);

    const float scale = config_.maxVelocityDegPerSec > 0.0F
                            ? config_.maxVelocityDegPerSec
                            : 1.0F;
    const float ratio = clampf(absf(lastCommand_) / scale, 0.0F, 1.0F);
    pwmOutput_ = static_cast<std::uint16_t>(
        ratio * static_cast<float>(config_.pwmFullScale) + 0.5F);
    mc::writeLe16(payload + 18U, pwmOutput_);
    payload[20] = homingAttained_ ? mc::kTelemetryFlagHomingAttained : 0U;

    queueFrame(mc::MessageType::Telemetry, outboundSeq_, payload,
               static_cast<std::uint16_t>(mc::kTelemetrySize));
    ++outboundSeq_;
    ++stats_.telemetrySent;
}

void Device::queueFaultEvent() {
    if (faultCode_ == mc::FaultCode::None) {
        return;
    }
    const std::uint8_t code = static_cast<std::uint8_t>(faultCode_);
    queueFrame(mc::MessageType::FaultEvent, outboundSeq_, &code, 1U);
    ++outboundSeq_;
    ++stats_.faultEventsSent;
}

}  // namespace mcfw
