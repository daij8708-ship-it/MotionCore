#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 应答跟踪器
//
// 把 docs/PROTOCOL.md §5 的应答/重传策略变成可执行、可测试的代码：
//   - 只有幂等命令（HELLO / SET_MODE / SET_PID / FAULT_EVENT）在超时后允许重传
//   - 运动指令（START / SET_TARGET）超时后一律判 Fault，**绝不自动重发**
//     —— 重发一条可能已被丢弃的运动指令会产生未定义运动（ADR-003）
//   - STOP 与 TELEMETRY 根本没有 ACK，登记即被拒绝
//
// 本类不含任何线程与 I/O：时间由调用方以 nowMs 传入，因此可在单元测试里
// 精确地推进时间，不需要真的等 200 ms。
// ---------------------------------------------------------------------------

// 一个请求超时后的处置决策。由**调用方**执行，本类只做判定。
enum class TxOutcome : std::uint8_t {
    Retry = 0,    // 幂等命令且重试次数未用尽 → 可以原样重发
    Fault = 1,    // 运动指令超时，或重试用尽 → 进入 Fault，等待人工确认
    Dropped = 2,  // 周期性探测帧（心跳）超时 → 只是漏了一拍，**不是故障**
};

struct TimeoutEvent {
    MessageType requestType = MessageType::Nack;
    std::uint16_t seq = 0U;
    TxOutcome outcome = TxOutcome::Fault;
    std::uint32_t attempt = 0U;   // 超时的是第几次尝试（0 = 首发）
};

struct AckStats {
    std::uint64_t registered = 0;           // 登记成功的请求数
    std::uint64_t completed = 0;             // 收到正确应答并完成的请求数
    std::uint64_t timedOutRetried = 0;       // 超时后判定为可重传的次数
    std::uint64_t timedOutFaulted = 0;       // 超时后判定为 Fault 的次数
    std::uint64_t timedOutDropped = 0;       // 周期探测帧超时（漏拍）次数，不计入故障
    std::uint64_t retriesExhausted = 0;      // 因重试用尽而判 Fault 的次数
    std::uint64_t rejectedByPeer = 0;        // 对端回 NAK，请求已被明确拒绝
    std::uint64_t staleAcks = 0;             // 无人认领的 ACK（陈旧/重复/伪造）
    std::uint64_t mismatchedAcks = 0;        // seq 在等，但 ACK 类型对不上
    std::uint64_t rejectedNotAwaitable = 0;  // 登记了根本不会有 ACK 的类型
    std::uint64_t rejectedDuplicateSeq = 0;  // 同一 seq 重复登记
};

class AckTracker {
public:
    struct Config {
        std::uint64_t timeoutMs = 200U;
        std::uint32_t maxRetries = 2U;   // 允许的**额外**重发次数（不含首发）
    };

    AckTracker();
    explicit AckTracker(Config config);

    // 纯查询：这个请求能否被登记（不会产生任何副作用与统计）。
    // 用于"先确认能登记，再真的把命令发出去"—— 避免发出一个无人认领的请求。
    bool canRegister(MessageType requestType, std::uint16_t seq) const noexcept;

    // 登记一个等待应答的请求。
    // 返回 false 表示被显式拒绝（两种情况都会计入统计，不静默接受）：
    //   1) requiresAck(requestType) == false —— 例如 STOP / TELEMETRY 没有 ACK 可等
    //   2) 同一 seq 已在等待中
    bool registerPending(MessageType requestType, std::uint16_t seq, std::uint64_t nowMs);

    // 收到一个应答帧。
    // 返回 true  = 匹配到一个在等的请求，该请求已被移除。
    // 返回 false = 陈旧 / 重复 / 类型对不上 / 无人认领的 ACK，只计数，无副作用。
    bool onAck(MessageType ackType, std::uint16_t seq);

    // 对端明确拒绝了该请求（收到 NAK）—— 请求已终结，继续等应答只会白等到超时。
    // 返回 true 表示确实有一个在等的请求被移除。
    bool cancelPending(std::uint16_t seq);

    // 推进时间，返回本次所有到期请求的处置决策。
    // 判为 Retry 的条目会被**就地重新登记**：attempt+1，deadline 顺延为 nowMs + timeoutMs
    // （相对 now 顺延，避免调用方迟到时产生追赶风暴）。
    std::vector<TimeoutEvent> pollTimeouts(std::uint64_t nowMs);

    // 调整等待应答的超时窗口 —— 供按实测 RTT 自适应（见 LinkMonitor）使用。
    // 只影响**之后**登记的请求：已在等待的条目保留各自的截止时刻，不追溯改写，
    // 否则一次窗口调整会把在途请求瞬间判成超时。
    void setTimeoutMs(std::uint64_t timeoutMs) noexcept { config_.timeoutMs = timeoutMs; }
    std::uint64_t timeoutMs() const noexcept { return config_.timeoutMs; }

    std::size_t pendingCount() const noexcept { return pending_.size(); }
    bool isPending(std::uint16_t seq) const noexcept;
    const AckStats& stats() const noexcept { return stats_; }

    void reset() noexcept;

private:
    struct Pending {
        MessageType requestType;
        std::uint16_t seq;
        std::uint64_t deadlineMs;
        std::uint32_t attempt;   // 0 = 首发
    };

    Config config_;
    std::vector<Pending> pending_;
    AckStats stats_{};
};

}  // namespace mc
