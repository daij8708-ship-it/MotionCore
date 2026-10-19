#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mc/frame_codec.hpp"
#include "mc/payload.hpp"
#include "mc_fw/device.hpp"

namespace {

struct Rig {
    mcfw::Device device;
    mcfw::FrameParser parser;
    std::vector<std::vector<std::uint8_t>> rawOut;
    std::uint32_t now = 0U;

    void drain() {
        std::uint8_t buffer[256]{};
        std::size_t written = 0U;
        while ((written = device.popFrame(buffer, sizeof(buffer))) > 0U) {
            rawOut.emplace_back(buffer, buffer + written);
        }
    }

    void feed(const std::vector<std::uint8_t>& bytes, std::uint32_t at) {
        now = at;
        for (std::uint8_t byte : bytes) {
            mcfw::ParsedFrame frame;
            if (parser.push(byte, frame)) {
                device.handleFrame(frame, at);
            }
        }
        drain();
    }

    void send(mc::MessageType type, std::uint16_t seq,
              const std::vector<std::uint8_t>& payload, std::uint32_t at) {
        mc::Frame frame;
        frame.version = mc::kProtocolVersion;
        frame.type = type;
        frame.seq = seq;
        frame.payload = payload;
        const mc::EncodeResult encoded = mc::encodeFrame(frame);
        if (!encoded) {
            ADD_FAILURE() << "请求帧编码失败";
            return;
        }
        feed(encoded.bytes, at);
    }

    void advanceTo(std::uint32_t target) {
        for (std::uint32_t t = now + 1U; t <= target; ++t) {
            device.tick(t);
            drain();
        }
        now = target;
    }

    std::vector<mc::Frame> frames() const {
        std::vector<mc::Frame> decoded;
        for (const std::vector<std::uint8_t>& raw : rawOut) {
            const mc::DecodeResult result = mc::decodeFrame(raw.data(), raw.size());
            if (!result) {
                ADD_FAILURE() << "设备输出了无法解码的帧";
                continue;
            }
            decoded.push_back(result.frame);
        }
        return decoded;
    }

    std::vector<mc::Frame> framesOfType(mc::MessageType type) const {
        std::vector<mc::Frame> selected;
        for (const mc::Frame& frame : frames()) {
            if (frame.type == type) {
                selected.push_back(frame);
            }
        }
        return selected;
    }

    void clearOut() { rawOut.clear(); }
};

std::vector<std::uint8_t> setMode(std::uint8_t mode) {
    mc::SetMode value;
    value.mode = mode;
    return mc::encodeSetMode(value);
}

std::vector<std::uint8_t> setTarget(float target) {
    mc::SetTarget value;
    value.target = target;
    return mc::encodeSetTarget(value);
}

std::vector<std::uint8_t> setPid(float kp, float ki, float kd) {
    mc::SetPid value;
    value.kp = kp;
    value.ki = ki;
    value.kd = kd;
    return mc::encodeSetPid(value);
}

std::vector<std::uint8_t> setLimits(bool enabled, float minDeg, float maxDeg) {
    mc::SoftLimits value;
    value.enabled = enabled;
    value.minDeg = minDeg;
    value.maxDeg = maxDeg;
    return mc::encodeSoftLimits(value);
}

std::vector<std::uint8_t> home(float speed, std::uint32_t timeoutMs) {
    mc::HomeParam value;
    value.speedDegPerSec = speed;
    value.timeoutMs = timeoutMs;
    return mc::encodeHomeParam(value);
}

std::vector<std::uint8_t> clearFault(std::uint8_t code) {
    mc::ClearFault value;
    value.faultCode = code;
    return mc::encodeClearFault(value);
}

mc::Nack readNack(const mc::Frame& frame) {
    mc::Nack value;
    EXPECT_EQ(mc::decodeNack(frame.payload.data(), frame.payload.size(), value),
              mc::PayloadError::None);
    return value;
}

mc::ParamState readParamState(const mc::Frame& frame) {
    mc::ParamState value;
    EXPECT_EQ(mc::decodeParamState(frame.payload.data(), frame.payload.size(), value),
              mc::PayloadError::None);
    return value;
}

}  // namespace

TEST(FirmwareDevice, HandshakeAndHeartbeat) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);

    const std::vector<mc::Frame> replies = rig.frames();
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].type, mc::MessageType::HelloAck);
    EXPECT_EQ(replies[0].seq, 1U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Ready);

    rig.clearOut();
    rig.send(mc::MessageType::Heartbeat, 2U, {}, 5U);
    const std::vector<mc::Frame> beats = rig.frames();
    ASSERT_EQ(beats.size(), 1U);
    EXPECT_EQ(beats[0].type, mc::MessageType::HeartbeatAck);
    EXPECT_EQ(beats[0].seq, 2U);
}

TEST(FirmwareDevice, VelocityMoveProducesTelemetryAndStops) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 3U, setTarget(100.0F), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Running);

    rig.advanceTo(1030U);
    const std::vector<mc::Frame> telemetry = rig.framesOfType(mc::MessageType::Telemetry);
    ASSERT_GE(telemetry.size(), 90U);

    float peakVelocity = 0.0F;
    float lastPosition = 0.0F;
    for (const mc::Frame& frame : telemetry) {
        mc::Telemetry sample;
        ASSERT_EQ(mc::decodeTelemetry(frame.payload.data(), frame.payload.size(), sample),
                  mc::PayloadError::None);
        peakVelocity = sample.velocityDegPerSec > peakVelocity ? sample.velocityDegPerSec
                                                               : peakVelocity;
        lastPosition = sample.positionDeg;
    }
    EXPECT_GT(peakVelocity, 50.0F);
    EXPECT_GT(lastPosition, 10.0F);

    rig.clearOut();
    rig.send(mc::MessageType::Stop, 5U, {}, 1040U);
    EXPECT_TRUE(rig.frames().empty());   // STOP 单向：没有 ACK
    rig.advanceTo(3000U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Ready);
    EXPECT_LT(rig.device.velocityDegPerSec(), 5.0F);
}

TEST(FirmwareDevice, ReadParamsReturnsWhatWasStored) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(2U), 10U);
    rig.send(mc::MessageType::SetPid, 3U, setPid(1.0F, 0.5F, 0.25F), 20U);
    rig.send(mc::MessageType::SetTarget, 4U, setTarget(20.0F), 30U);
    rig.send(mc::MessageType::ReadParams, 5U, {}, 40U);

    const std::vector<mc::Frame> replies = rig.framesOfType(mc::MessageType::ReadParamsAck);
    ASSERT_EQ(replies.size(), 1U);
    const mc::ParamState state = readParamState(replies[0]);
    EXPECT_EQ(state.mode, 2U);
    EXPECT_FLOAT_EQ(state.target, 20.0F);
    EXPECT_FLOAT_EQ(state.kp, 1.0F);
    EXPECT_FLOAT_EQ(state.ki, 0.5F);
    EXPECT_FLOAT_EQ(state.kd, 0.25F);
    EXPECT_FALSE(state.limits.enabled);
}

TEST(FirmwareDevice, SoftLimitsAreStoredAndEnforced) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetLimits, 2U, setLimits(true, -10.0F, 10.0F), 10U);

    ASSERT_EQ(rig.framesOfType(mc::MessageType::SetLimitsAck).size(), 1U);

    rig.clearOut();
    rig.send(mc::MessageType::SetMode, 3U, setMode(2U), 20U);
    rig.send(mc::MessageType::SetTarget, 4U, setTarget(20.0F), 30U);
    const std::vector<mc::Frame> nacks = rig.framesOfType(mc::MessageType::Nack);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(readNack(nacks[0]).reason, mc::NackReason::BadParameter);

    rig.clearOut();
    rig.send(mc::MessageType::SetTarget, 5U, setTarget(5.0F), 40U);
    EXPECT_EQ(rig.framesOfType(mc::MessageType::SetTargetAck).size(), 1U);
}

TEST(FirmwareDevice, IllegalStateCommandsAreRefusedWithTheRightReason) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);

    rig.clearOut();
    rig.send(mc::MessageType::Start, 2U, {}, 10U);
    std::vector<mc::Frame> nacks = rig.framesOfType(mc::MessageType::Nack);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(readNack(nacks[0]).reason, mc::NackReason::IllegalState);

    rig.clearOut();
    rig.send(mc::MessageType::SetMode, 3U, setMode(1U), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Running);

    rig.clearOut();
    rig.send(mc::MessageType::SetMode, 5U, setMode(1U), 40U);
    nacks = rig.framesOfType(mc::MessageType::Nack);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(readNack(nacks[0]).reason, mc::NackReason::IllegalState);
}

TEST(FirmwareDevice, CommLossFaultsAndClearFaultNeedsTheMatchingCode) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 3U, setTarget(100.0F), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);

    rig.advanceTo(3500U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Fault);
    EXPECT_EQ(rig.device.faultCode(), mc::FaultCode::CommunicationLoss);

    const std::vector<mc::Frame> events = rig.framesOfType(mc::MessageType::FaultEvent);
    ASSERT_GE(events.size(), 1U);
    ASSERT_EQ(events.back().payload.size(), 1U);
    EXPECT_EQ(events.back().payload[0], static_cast<std::uint8_t>(mc::FaultCode::CommunicationLoss));

    rig.clearOut();
    rig.send(mc::MessageType::ClearFault, 5U, clearFault(2U), 3510U);
    std::vector<mc::Frame> nacks = rig.framesOfType(mc::MessageType::Nack);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(readNack(nacks[0]).reason, mc::NackReason::BadParameter);

    rig.clearOut();
    rig.send(mc::MessageType::ClearFault, 6U, clearFault(1U), 3520U);
    EXPECT_EQ(rig.framesOfType(mc::MessageType::ClearFaultAck).size(), 1U);
    EXPECT_EQ(rig.device.state(), mc::DeviceState::Ready);
    EXPECT_EQ(rig.device.faultCode(), mc::FaultCode::None);
}

TEST(FirmwareDevice, HomeReturnsToTheOriginAndSetsTheAttainedBit) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 3U, setTarget(50.0F), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);
    rig.advanceTo(500U);
    rig.send(mc::MessageType::Stop, 5U, {}, 510U);
    rig.advanceTo(1500U);
    ASSERT_EQ(rig.device.state(), mc::DeviceState::Ready);
    ASSERT_GT(rig.device.positionDeg(), 5.0F);

    rig.clearOut();
    rig.send(mc::MessageType::Home, 6U, home(50.0F, 5000U), 1510U);
    EXPECT_EQ(rig.framesOfType(mc::MessageType::HomeAck).size(), 1U);
    rig.advanceTo(6000U);

    EXPECT_EQ(rig.device.state(), mc::DeviceState::Ready);
    EXPECT_TRUE(rig.device.homingAttained());
    EXPECT_LT(rig.device.positionDeg(), 1.0F);

    bool sawAttainedBit = false;
    for (const mc::Frame& frame : rig.framesOfType(mc::MessageType::Telemetry)) {
        mc::Telemetry sample;
        ASSERT_EQ(mc::decodeTelemetry(frame.payload.data(), frame.payload.size(), sample),
                  mc::PayloadError::None);
        sawAttainedBit = sawAttainedBit || ((sample.statusFlags & mc::kTelemetryFlagHomingAttained) != 0U);
    }
    EXPECT_TRUE(sawAttainedBit);
}

TEST(FirmwareDevice, HomeTimeoutFaultsWithHomeFailed) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 3U, setTarget(50.0F), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);
    rig.advanceTo(500U);
    rig.send(mc::MessageType::Stop, 5U, {}, 510U);
    rig.advanceTo(1500U);
    ASSERT_EQ(rig.device.state(), mc::DeviceState::Ready);

    rig.send(mc::MessageType::Home, 6U, home(1.0F, 100U), 1510U);
    rig.advanceTo(1800U);

    EXPECT_EQ(rig.device.state(), mc::DeviceState::Fault);
    EXPECT_EQ(rig.device.faultCode(), mc::FaultCode::HomeFailed);
}

TEST(FirmwareDevice, DuplicateSequenceReplaysTheAckWithoutReapplying) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 7U, setTarget(10.0F), 20U);
    rig.send(mc::MessageType::SetTarget, 7U, setTarget(20.0F), 30U);

    EXPECT_EQ(rig.framesOfType(mc::MessageType::SetTargetAck).size(), 2U);
    EXPECT_EQ(rig.device.stats().duplicatesSuppressed, 1U);

    rig.clearOut();
    rig.send(mc::MessageType::ReadParams, 8U, {}, 40U);
    const std::vector<mc::Frame> replies = rig.framesOfType(mc::MessageType::ReadParamsAck);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_FLOAT_EQ(readParamState(replies[0]).target, 10.0F);
}

TEST(FirmwareDevice, UnknownTypeGetsAnUnknownTypeNack) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.clearOut();
    rig.send(static_cast<mc::MessageType>(0x33U), 2U, {}, 10U);

    const std::vector<mc::Frame> nacks = rig.framesOfType(mc::MessageType::Nack);
    ASSERT_EQ(nacks.size(), 1U);
    EXPECT_EQ(readNack(nacks[0]).reason, mc::NackReason::UnknownType);
}

TEST(FirmwareDevice, TelemetryRunsAtTheConfiguredPeriod) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.advanceTo(100U);

    const std::size_t count = rig.framesOfType(mc::MessageType::Telemetry).size();
    EXPECT_GE(count, 9U);
    EXPECT_LE(count, 11U);
}

TEST(FirmwareDevice, FaultEventIsPeriodicWhileFaulted) {
    Rig rig;
    rig.send(mc::MessageType::Hello, 1U, {}, 0U);
    rig.send(mc::MessageType::SetMode, 2U, setMode(1U), 10U);
    rig.send(mc::MessageType::SetTarget, 3U, setTarget(100.0F), 20U);
    rig.send(mc::MessageType::Start, 4U, {}, 30U);

    rig.advanceTo(3600U);
    ASSERT_EQ(rig.device.state(), mc::DeviceState::Fault);
    const std::size_t firstCount = rig.framesOfType(mc::MessageType::FaultEvent).size();
    EXPECT_GE(firstCount, 1U);

    rig.advanceTo(4600U);
    const std::size_t laterCount = rig.framesOfType(mc::MessageType::FaultEvent).size();
    EXPECT_GT(laterCount, firstCount);
}

TEST(FirmwareDevice, ExternalFeedbackReplacesTheVirtualPlant) {
    mcfw::Config config;
    config.externalFeedback = true;
    config.encoderCountsPerRev = 2800.0F;
    mcfw::Device device(config);

    // 每 10 ms 喂 10°，50 ms 窗口内的位移是 50°，速度应约为 1000 deg/s。
    // 如果外部反馈没有生效，虚拟模型在无指令时位置会一直停在 0。
    for (std::uint32_t t = 10U; t <= 50U; t += 10U) {
        device.setExternalPositionDeg(static_cast<float>(t));
        device.tick(t);
    }
    EXPECT_NEAR(device.positionDeg(), 50.0F, 0.2F);
    EXPECT_NEAR(device.velocityDegPerSec(), 1000.0F, 30.0F);

    // 默认（仿真）模式下，喂进来的外部位置必须完全无效。
    mcfw::Device virtualDevice;
    virtualDevice.setExternalPositionDeg(123.0F);
    virtualDevice.tick(10U);
    EXPECT_NEAR(virtualDevice.positionDeg(), 0.0F, 0.001F);
    EXPECT_NEAR(virtualDevice.velocityDegPerSec(), 0.0F, 0.001F);
}
