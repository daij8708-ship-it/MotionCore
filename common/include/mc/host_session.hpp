#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "mc/ack_tracker.hpp"
#include "mc/frame_codec.hpp"
#include "mc/link_monitor.hpp"
#include "mc/payload.hpp"
#include "mc/protocol.hpp"
#include "mc/transport.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 上位机侧会话
//
// 把"传输 + 拆帧 + 命令编码 + 应答跟踪"缝合成一个可用的整体。
// 它是**应用服务层**的种子：阶段 2 用 Qt 包一层（QTimer/readyRead 驱动 pump），
// UI 只与它对话。因此这里刻意零 Qt 依赖，也刻意不含任何界面概念。
//
// 时间由调用方以 nowMs 传入 —— 因此超时行为在测试里可以精确控制，
// 不需要真的等待。
// ---------------------------------------------------------------------------
// 握手参数。
//
// ⚠ 它**刻意**放在 HostSession 之外（而不是像 LinkMonitor::Config 那样嵌套）：
// 嵌套类型的默认成员初始化器在**外层类还没定义完**时不可用，GCC 会直接拒绝
// `HandshakeConfig config = HandshakeConfig{}` 这种默认实参
// （"default member initializer ... required before the end of its enclosing class"）。
struct HandshakeConfig {
    // 一轮握手失败（HELLO 重试用尽）后，重开一轮之前等多久；每失败一轮翻倍、封顶。
    // 一轮之内的重发次数不在这里 —— 它归 AckTracker::Config::maxRetries 管。
    std::uint64_t roundBackoffInitialMs = 200U;
    std::uint64_t roundBackoffMaxMs = 1600U;
};

class HostSession {
public:
    explicit HostSession(ITransport& transport);
    HostSession(ITransport& transport, AckTracker::Config trackerConfig);

    // --- 链路监控（可选）------------------------------------------------------
    // 不启用时行为与本增量之前**完全一致**：不发心跳、超时窗口固定。
    // 启用后：pump() 会按周期发心跳、用 HeartbeatAck 实测 RTT，
    // 并据此调整 tracker 的超时窗口（详见 LinkMonitor）。
    //
    // 必须提供序列号来源，且它**必须与命令共用同一个计数器**：
    // AckTracker 按 seq 判重而不看类型，两套计数器必然撞车，
    // 撞上之后命令会被 canRegister 前置检查拦住、**静默发不出去**。
    using SeqSource = std::function<std::uint16_t()>;

    // 传入空的 seqSource 时**不会**启用 —— 而不是启用一个发不出心跳的半成品。
    void enableLinkMonitor(LinkMonitor::Config config, SeqSource seqSource);
    bool linkMonitorEnabled() const noexcept { return linkMonitor_ != nullptr; }
    const LinkMonitor* linkMonitor() const noexcept { return linkMonitor_.get(); }

    // --- 会话生命周期（可选）--------------------------------------------------
    // 回答"什么时候该握手、握手成没成、断了要不要重来"。
    //
    // **不自己发明链路失效检测**：失效判定仍然只由 LinkMonitor 做（连续丢 N 拍），
    // 本层只消费它的结论。因此没有开链路监控时，本层看不出"链路失效"，
    // 一个已就绪的会话不会被自动作废 —— 这不是遗漏，而是"不重复造检测"的代价。
    enum class SessionState : std::uint8_t {
        NotReady = 0U,     // 没有可用会话：等退避到点就发下一轮的 HELLO
        Handshaking = 1U,  // HELLO 已在途，等 HELLO_ACK
        Ready = 2U,        // 握手完成，命令可以下发
    };

    // 与 enableLinkMonitor 一致：没有序列号来源就不启用（不装作启用成功）。
    // 两个 enable* 传入的来源必须指向**同一个计数器** —— 心跳、握手、命令
    // 共用一套 seq，否则 AckTracker 按 seq 判重时会撞车。
    void enableHandshake(SeqSource seqSource, HandshakeConfig config = HandshakeConfig{});

    bool handshakeEnabled() const noexcept { return handshakeEnabled_; }
    bool sessionReady() const noexcept { return sessionState_ == SessionState::Ready; }

    // 已经开过几轮握手（含当前这轮）。**不设总轮数上限**：要不要放弃是上位机
    // （人）的决定，本层只如实报数，不替调用方"想通"然后静默停手。
    std::uint64_t handshakeRounds() const noexcept { return handshakeRounds_; }

    struct PumpResult {
        std::size_t bytesRead = 0;
        std::size_t framesDecoded = 0;
        std::size_t telemetryAccepted = 0;
        std::size_t telemetryRejected = 0;   // 结构或语义非法，已丢弃，未进入数据通路
        std::size_t acksMatched = 0;
        std::size_t naksReceived = 0;
        std::size_t unexpectedFrames = 0;    // 陈旧 ACK / 无人认领的 NAK / 意外类型
        std::vector<TimeoutEvent> timeouts;
        std::size_t droppedProbes = 0;       // 心跳超时（漏拍）——不是故障
        bool heartbeatSent = false;

        // 下面两个只在 linkMonitorEnabled() 为 true 时有意义。
        // 未启用时 linkDown 恒为 false，含义是"**没有在监控**"，而不是"链路正常"。
        bool linkDown = false;
        std::uint64_t ackTimeoutMs = 0U;     // 本拍生效的应答超时窗口

        // 下面三个只在 handshakeEnabled() 为 true 时有意义。
        bool handshakeCompleted = false;     // 本拍收到 HELLO_ACK，会话转入"就绪"
        bool sessionLost = false;            // 本拍链路失效，一个已就绪的会话被作废
        bool handshakeRoundFailed = false;   // 本拍一轮 HELLO 重试用尽，退避后将重开

        bool transportOpen = true;
    };

    // --- 发送命令 -------------------------------------------------------------
    // 返回 false 表示**没有发出去**（登记冲突、编码失败、传输拒收）。
    // 需要应答的命令在发送前会先确认能登记，避免发出无人认领的请求。
    bool sendHello(std::uint16_t seq, std::uint64_t nowMs);
    bool sendHeartbeat(std::uint16_t seq, std::uint64_t nowMs);
    bool sendSetMode(std::uint8_t mode, std::uint16_t seq, std::uint64_t nowMs);
    bool sendStart(std::uint16_t seq, std::uint64_t nowMs);
    bool sendSetTarget(float target, std::uint16_t seq, std::uint64_t nowMs);
    bool sendSetPid(const SetPid& pid, std::uint16_t seq, std::uint64_t nowMs);

    // STOP 是单向的（ADR-004）：不等应答，因此不需要 nowMs。
    bool sendStop(std::uint16_t seq);

    // --- 驱动 -----------------------------------------------------------------
    // 读传输 → 拆帧 → 分发（遥测入库 / 应答入跟踪器 / NAK 记录）→ 检查超时。
    PumpResult pump(std::uint64_t nowMs, std::size_t maxBytesPerPump = 4096U);

    // --- 观测 -----------------------------------------------------------------
    bool hasTelemetry() const noexcept { return hasTelemetry_; }
    const Telemetry& lastTelemetry() const noexcept { return lastTelemetry_; }
    std::uint64_t telemetryAccepted() const noexcept { return telemetryAccepted_; }
    std::uint64_t telemetryRejected() const noexcept { return telemetryRejected_; }
    const std::vector<Nack>& receivedNacks() const noexcept { return receivedNacks_; }

    const AckTracker& tracker() const noexcept { return tracker_; }
    const StreamDecoder& decoder() const noexcept { return decoder_; }

    std::uint64_t framesSent() const noexcept { return framesSent_; }
    std::uint64_t encodeFailures() const noexcept { return encodeFailures_; }
    std::uint64_t sendFailures() const noexcept { return sendFailures_; }

private:
    // 可重传命令的"发件留底"：超时判 Retry 时要原样重发的就是它。
    // 只给 isRetryable() 的类型留底 —— 运动指令（START / SET_TARGET）根本不会
    // 出现在这里，这就是"绝不自作主张重发运动指令"的机制本身，而不是一句注释。
    struct InFlightRequest {
        MessageType type = MessageType::Hello;
        std::uint16_t seq = 0U;
        std::vector<std::uint8_t> payload;
    };

    bool sendFrame(MessageType type, std::uint16_t seq, const std::vector<std::uint8_t>& payload);
    bool sendCommand(MessageType type, std::uint16_t seq,
                     const std::vector<std::uint8_t>& payload, std::uint64_t nowMs);
    void dispatch(const Frame& frame, PumpResult& result, std::uint64_t nowMs);

    void resendRetryable(const TimeoutEvent& event);
    void forgetInFlight(std::uint16_t seq);
    void tickHandshake(std::uint64_t nowMs);

    ITransport& transport_;
    std::unique_ptr<LinkMonitor> linkMonitor_;
    SeqSource seqSource_;      // 心跳 / 握手 / 命令共用的唯一序列号来源
    AckTracker tracker_;
    StreamDecoder decoder_;
    std::vector<InFlightRequest> inFlight_;

    bool handshakeEnabled_ = false;
    HandshakeConfig handshakeConfig_{};
    SessionState sessionState_ = SessionState::NotReady;
    std::uint64_t handshakeRounds_ = 0U;
    std::uint64_t nextRoundAttemptMs_ = 0U;
    std::uint64_t roundBackoffMs_ = 0U;
    std::uint16_t helloSeq_ = 0U;      // 本轮 HELLO 的 seq（在途期间不变，重发也用它）
    bool lastLinkDown_ = false;        // 只在跳变沿上作废会话，不在"一直断着"时反复作废

    Telemetry lastTelemetry_{};
    bool hasTelemetry_ = false;
    std::vector<Nack> receivedNacks_;

    std::uint64_t telemetryAccepted_ = 0U;
    std::uint64_t telemetryRejected_ = 0U;
    std::uint64_t framesSent_ = 0U;
    std::uint64_t encodeFailures_ = 0U;
    std::uint64_t sendFailures_ = 0U;
};

}  // namespace mc
