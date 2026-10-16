#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/device_state.hpp"
#include "mc/frame_codec.hpp"
#include "mc/payload.hpp"
#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 最小可用版虚拟设备（协议对端）
//
// 这是"没有硬件时唯一的对端"，因此在**协议层面**必须与真实设备等价：
// 同一套帧格式、同一套校验、同一套拒绝规则、**同一个状态机**。
// 差别只在运动模型 —— 下面是**声明式的仿真模型，不是实物测量结果**：
//
//   速度：v 以 accelDegPerSec2 的速率限幅地趋近目标（一阶速率限制）
//   位置：p 对 v 积分
//   PWM ：与 |v| 成正比
//
// 完整版（惯性、编码器量化、PID 跟随、设备侧故障注入）属阶段 8。
//
// 阶段 4 起，本类的状态**一律由 DeviceStateMachine 裁决**，不再自己现算：
// 开局处于 Initializing（构造等价于"链路已打开、尚未握手"），必须走完一次
// HELLO 往返才进 Ready，此后才接受控制命令。
// ---------------------------------------------------------------------------

// 运行模式与设备状态是**正交**的两件事：
//   DeviceState 回答"现在允不允许被控制"，SimRunMode 回答"用哪个环去控制"。
enum class SimRunMode : std::uint8_t {
    Stopped = 0U,
    Velocity = 1U,
    Position = 2U,
};

static_assert(static_cast<std::uint8_t>(SimRunMode::Position) == kMaxRunMode,
              "SimRunMode 必须与 SET_MODE 载荷允许的模式编号一一对应");

// 设备侧序列号去重窗口：记住最近处理过的这么多个命令帧及其应答。
//
// 为什么是"窗口"而不是"只记最后一个"：应答在途时可能有多条命令重叠。
// 为什么取 32 而不更大：重传只可能发生在应答超时窗口之内（毫秒级），
// 32 条命令的跨度远超该窗口，再大只是白占内存。
inline constexpr std::size_t kSimulatorDedupWindowSize = 32U;

struct SimulatorConfig {
    float maxVelocityDegPerSec = 384.0F;   // 与上位机的量程一致
    float accelDegPerSec2 = 1200.0F;
    std::uint32_t telemetryPeriodMs = 10U; // 100 Hz
    std::uint16_t pwmFullScale = 99U;      // 对应固件定时器的 ARR = 99

    // Stopping → Ready 的判据：|v| 落在这个门限内就算停稳。
    // 取满量程的 1%（3.84 deg/s ≈ 0.64 rpm）：远低于任何有意义的指令速度，
    // 又大于速率限幅一拍的步长（1200 × 0.01 / 2 = 6 deg/s）——
    // 取太小"停稳"永远等不到，取太大把"还在滑行"报成"已停"。
    float stopVelocityThresholdDegPerSec = 3.84F;

    // 设备侧通信丢失保护（F7）的超时窗口。0 = 关闭（仅供对照实验）。
    std::uint64_t commLossTimeoutMs = kDeviceCommLossTimeoutMs;

    // 故障注入：非 None 时设备进入 Fault 并持续上报该故障码。
    FaultCode forcedFaultCode = FaultCode::None;
};

class DeviceSimulator {
public:
    explicit DeviceSimulator(SimulatorConfig config = SimulatorConfig{});

    struct TickResult {
        std::size_t framesReceived = 0;
        std::size_t acksSent = 0;
        std::size_t nacksSent = 0;
        std::size_t telemetrySent = 0;
        std::size_t unknownTypes = 0;
        std::size_t malformedPayloads = 0;
        std::size_t duplicatesSuppressed = 0;   // 判为重传、只重放应答未重复执行的帧数
        bool commLossFaulted = false;           // 本拍因通信丢失进入 Fault（F7）
    };

    // 推进一个 tick：先积分运动，再处理收到的字节，再做安全检查，最后决定是否上报遥测。
    TickResult tick(const std::uint8_t* incoming, std::size_t len, std::uint64_t nowMs);

    // 取出本设备待发出的字节。
    std::size_t readOutgoing(std::uint8_t* out, std::size_t maxLen);

    // --- 观测（供测试与上位机断言）------------------------------------------
    SimRunMode runMode() const noexcept { return runMode_; }
    DeviceState state() const noexcept { return stateMachine_.state(); }

    // "输出带电中" = Running 或 Stopping（Stopping 期间电机还在减速，仍带电）。
    // 用 isEnabled() 判断"电机是不是在动"是不够的：Fault 之后速度还在限幅收敛。
    bool isEnabled() const noexcept {
        return stateMachine_.is(DeviceState::Running) || stateMachine_.is(DeviceState::Stopping);
    }

    bool faultLatched() const noexcept { return stateMachine_.faultLatched(); }
    FaultCode faultCode() const noexcept { return faultCode_; }
    const DeviceStateMachine& stateMachine() const noexcept { return stateMachine_; }

    float target() const noexcept { return target_; }
    float velocityDegPerSec() const noexcept { return velocity_; }
    float positionDeg() const noexcept { return position_; }
    const SetPid& pid() const noexcept { return pid_; }
    const StreamDecoder& decoder() const noexcept { return decoder_; }

    // 本设备发出的最近 N 条 NAK（含拒绝原因），便于断言"该拒的拒了"。
    const std::vector<Nack>& nacksSent() const noexcept { return nacksSent_; }

    // 自身编码失败的次数。设备自己的载荷都是定长小对象，理论上永远是 0 ——
    // 因此这是一个**内部不变式**：非 0 就说明本模块有 bug，不能静默吞掉。
    std::uint64_t encodeFailures() const noexcept { return encodeFailures_; }

    Telemetry lastTelemetry() const noexcept { return lastTelemetry_; }

    // 把状态机事件喂进来（链路层、故障检测、以及测试都走这一个窄口）。
    // 刻意只开这一个口，而不是把 DeviceStateMachine& 整个暴露出去 ——
    // 否则"谁能改状态"就说不清了。
    TransitionResult applyEvent(DeviceEvent event) noexcept;

private:
    // 对一帧请求作出的应答。去重命中时用它**重放**上一次的应答 ——
    // 只去重不重放是不行的：对端丢了 ACK 后重传，会一个应答都拿不到，
    // 幂等命令的重传机制就被去重机制废掉了。
    struct Reply {
        enum class Kind : std::uint8_t { None = 0U, Ack = 1U, Nack = 2U };
        Kind kind = Kind::None;                        // None = 该请求本来就不应答（只有 STOP）
        MessageType ackType = MessageType::Nack;       // 仅 Ack 时有效
        NackReason reason = NackReason::IllegalState;  // 仅 Nack 时有效
    };

    struct DedupEntry {
        MessageType type = MessageType::Nack;
        std::uint16_t seq = 0U;
        Reply reply{};
    };

    void integrate(std::uint64_t nowMs);
    void runSafetyChecks(std::uint64_t nowMs, TickResult& result);
    void handleFrame(const Frame& frame, TickResult& result);
    Reply executeFrame(const Frame& frame, TickResult& result);
    Reply replyWithAck(MessageType ackType, std::uint16_t seq, TickResult& result);
    Reply replyWithNack(std::uint16_t seq, NackReason reason, TickResult& result);
    void replayReply(const DedupEntry& entry, TickResult& result);
    void rememberReply(MessageType type, std::uint16_t seq, const Reply& reply);
    const DedupEntry* findRecent(MessageType type, std::uint16_t seq) const noexcept;
    void emitTelemetry(TickResult& result);
    void queueFrame(MessageType type, std::uint16_t seq, const std::vector<std::uint8_t>& payload);
    void queueAck(MessageType ackType, std::uint16_t seq, TickResult& result);
    void queueNack(std::uint16_t rejectedSeq, NackReason reason, TickResult& result);

    SimulatorConfig config_;
    StreamDecoder decoder_;
    std::vector<std::uint8_t> outgoing_;

    // 开局 Initializing：构造等价于"链路已打开、尚未握手"。
    DeviceStateMachine stateMachine_{DeviceState::Initializing};

    SimRunMode runMode_ = SimRunMode::Stopped;
    FaultCode faultCode_ = FaultCode::None;
    float target_ = 0.0F;
    float velocity_ = 0.0F;
    float position_ = 0.0F;
    SetPid pid_{};

    bool hasTicked_ = false;
    std::uint64_t lastTickMs_ = 0U;
    std::uint64_t lastTelemetryMs_ = 0U;

    // 最近一次收到**任何 CRC 通过的帧**的时刻 —— F7 判据用的就是它。
    // 用"收到帧"而不是"收到字节"：字节流一直有噪声、但没有一帧能过 CRC，
    // 那不叫通信正常。
    bool hasReceivedFrame_ = false;
    std::uint64_t lastValidFrameMs_ = 0U;

    Telemetry lastTelemetry_{};
    std::vector<DedupEntry> recent_;
    std::vector<Nack> nacksSent_;
    std::uint16_t outboundSeq_ = 0U;
    std::uint64_t encodeFailures_ = 0U;
};

}  // namespace mc
