#pragma once

#include <cstddef>
#include <cstdint>

#include "mc/device_state.hpp"
#include "mc/payload.hpp"
#include "mc_fw/frame_io.hpp"

namespace mcfw {

// 固件侧的运动模型参数。默认值与宿主侧 SimulatorConfig 保持一致，
// 这样同一套上位机操作在仿真器和真固件上看到的曲线形状可比。
struct Config {
    float maxVelocityDegPerSec = 384.0F;
    float accelDegPerSec2 = 1200.0F;
    float inertiaTimeConstantMs = 40.0F;
    float stopVelocityThresholdDegPerSec = 3.84F;
    float encoderCountsPerRev = 4096.0F;
    float velocityWindowMs = 50.0F;
    float homeToleranceDeg = 0.25F;
    float homeApproachSeconds = 0.2F;
    std::uint16_t pwmFullScale = 99U;
    std::uint32_t commLossTimeoutMs = 3000U;
    std::uint32_t telemetryPeriodMs = 10U;
    std::uint32_t faultEventPeriodMs = 500U;
};

struct Stats {
    std::uint32_t framesHandled = 0U;
    std::uint32_t acksSent = 0U;
    std::uint32_t nacksSent = 0U;
    std::uint32_t telemetrySent = 0U;
    std::uint32_t faultEventsSent = 0U;
    std::uint32_t duplicatesSuppressed = 0U;
    std::uint32_t outputDropped = 0U;
};

// 最小设备固件的纯逻辑主体：不碰 UART、不碰时钟、不碰 GPIO。
// 时间由调用方喂进来，收发由调用方搬字节，因此同一份代码可以在
// 宿主测试、PTY 端到端工具和 STM32 裸机上跑。
class Device {
public:
    explicit Device(const Config& config = Config{});

    void reset(std::uint32_t nowMs);

    // 处理一帧已经通过 CRC 的请求。应答进入内部发送队列。
    void handleFrame(const ParsedFrame& frame, std::uint32_t nowMs);

    // 推进时间：积分运动、跑安全检查、按周期产生遥测与故障事件。
    void tick(std::uint32_t nowMs);

    // 取出一帧待发送数据；队列为空返回 0。
    std::size_t popFrame(std::uint8_t* out, std::size_t capacity) noexcept;

    const Stats& stats() const noexcept { return stats_; }
    mc::DeviceState state() const noexcept { return state_.state(); }
    bool faultLatched() const noexcept { return state_.faultLatched(); }
    mc::FaultCode faultCode() const noexcept { return faultCode_; }
    bool homingAttained() const noexcept { return homingAttained_; }
    float positionDeg() const noexcept { return measuredPosition_; }
    float velocityDegPerSec() const noexcept { return measuredVelocity_; }
    float target() const noexcept { return target_; }
    std::uint8_t runMode() const noexcept { return runMode_; }
    std::uint16_t pwmOutput() const noexcept { return pwmOutput_; }

private:
    struct Limits {
        bool enabled = false;
        float minDeg = 0.0F;
        float maxDeg = 0.0F;
    };

    struct Sample {
        std::uint32_t tMs = 0U;
        float positionDeg = 0.0F;
    };

    struct ReplySpec {
        bool hasReply = false;
        mc::MessageType type = mc::MessageType::Nack;
        std::uint8_t payload[32]{};
        std::uint8_t payloadLen = 0U;
    };

    struct DedupEntry {
        bool used = false;
        mc::MessageType type = mc::MessageType::Nack;
        std::uint16_t seq = 0U;
        bool hasReply = false;
        mc::MessageType replyType = mc::MessageType::Nack;
        std::uint8_t replyPayload[32]{};
        std::uint8_t replyLen = 0U;
    };

    struct OutFrame {
        std::uint16_t len = 0U;
        std::uint8_t data[kMaxFrameBytes]{};
    };

    static constexpr std::size_t kOutQueueSize = 8U;
    static constexpr std::size_t kDedupSize = 8U;
    static constexpr std::size_t kWindowSize = 8U;

    ReplySpec executeFrame(const ParsedFrame& frame, std::uint32_t nowMs);
    ReplySpec ack(mc::MessageType type) const noexcept;
    ReplySpec nack(std::uint16_t seq, mc::NackReason reason) const noexcept;

    void rememberReply(mc::MessageType type, std::uint16_t seq, const ReplySpec& spec);
    const DedupEntry* findDedup(mc::MessageType type, std::uint16_t seq) const noexcept;
    void clearDedup() noexcept;

    void queueFrame(mc::MessageType type, std::uint16_t seq,
                    const std::uint8_t* payload, std::uint16_t payloadLen);
    void queueTelemetry();
    void queueFaultEvent();

    void integrate(float dtSeconds);
    void updateMeasurement();
    void runSafetyChecks(std::uint32_t nowMs);
    mc::TransitionResult applyEvent(mc::DeviceEvent event) noexcept;
    void defineOriginHere();
    void resetPidState() noexcept;
    float pidOutput(float error, float dtSeconds, float clampAbs) noexcept;
    bool limitsAllow(float positionDeg) const noexcept;

    Config config_;
    mc::DeviceStateMachine state_{mc::DeviceState::Initializing};
    mc::FaultCode faultCode_ = mc::FaultCode::None;

    std::uint8_t runMode_ = 0U;
    float target_ = 0.0F;
    float kp_ = 0.0F;
    float ki_ = 0.0F;
    float kd_ = 0.0F;
    Limits limits_{};

    float velocity_ = 0.0F;
    float position_ = 0.0F;
    float measuredVelocity_ = 0.0F;
    float measuredPosition_ = 0.0F;
    float lastCommand_ = 0.0F;
    std::uint16_t pwmOutput_ = 0U;

    float integralError_ = 0.0F;
    float lastPidError_ = 0.0F;
    bool pidPrimed_ = false;

    bool homingAttained_ = false;
    float homeSpeed_ = 0.0F;
    std::uint32_t homeTimeoutMs_ = 0U;
    std::uint32_t homeStartedMs_ = 0U;

    bool hasReceivedFrame_ = false;
    std::uint32_t lastValidFrameMs_ = 0U;
    std::uint32_t lastTickMs_ = 0U;
    std::uint32_t lastTelemetryMs_ = 0U;
    std::uint32_t lastFaultEventMs_ = 0U;

    Sample window_[kWindowSize]{};
    std::uint8_t windowHead_ = 0U;
    std::uint8_t windowTail_ = 0U;
    std::uint8_t windowCount_ = 0U;

    DedupEntry dedup_[kDedupSize]{};
    std::uint8_t dedupHead_ = 0U;

    OutFrame outQueue_[kOutQueueSize]{};
    std::uint8_t outHead_ = 0U;
    std::uint8_t outTail_ = 0U;
    std::uint8_t outCount_ = 0U;

    std::uint16_t outboundSeq_ = 0U;
    Stats stats_{};
};

}  // namespace mcfw
