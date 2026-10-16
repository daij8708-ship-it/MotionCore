#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// 链路监控：心跳调度 + RTT 实测 + 链路失效判定
//
// 它回答三个此前没人回答的问题：
//   1) 谁按周期发心跳？—— `sendHeartbeat` 有接口，但没有任何地方调度它；
//   2) 链路什么时候算断？—— 用"连续丢 N 拍"累计判定，而不是"单次超时"；
//   3) 应答超时窗口该设多大？—— **用实测 RTT 推出来**，不再硬编码 200 ms。
//
// 第 3 点是被实测逼出来的：演示场景④里 100 ms 的单向延迟（RTT ≈ 200 ms）
// 会被硬编码的 200 ms 超时**误判成 Fault**。超时窗口必须按实测 RTT 配。
//
// 本类不持有线程、不做 I/O：时间全部由调用方以 nowMs 传入。因此"第几拍该发"
// "丢几拍算断""窗口收敛到什么值"都能在单元测试里用假时间精确复现，不用真的等。
// ---------------------------------------------------------------------------
class LinkMonitor {
public:
    struct Config {
        std::uint64_t heartbeatPeriodMs = 1000U;

        // 连续这么多拍都没有应答 → 判定链路失效。
        // 用"连续丢 N 拍"而不是"单次超时"：后者会被一次偶发丢包骗到。
        std::uint32_t missedBeatsToDeclareLinkDown = 3U;

        // 一个 RTT 样本都还没有时使用的窗口。**不能**给激进的小值 ——
        // 那会把健康的慢链路误判成超时。默认值与引入本类之前的硬编码值一致，便于对照。
        std::uint64_t initialAckTimeoutMs = 200U;

        // 推荐窗口 = RTT 的 P95 × (timeoutMarginPercent / 100)，再夹到 [min, max]。
        std::uint32_t timeoutMarginPercent = 300U;
        std::uint64_t minAckTimeoutMs = 50U;
        std::uint64_t maxAckTimeoutMs = 2000U;

        // RTT 样本窗口。取小值是**有意**的：链路状况会变，我们要的是"最近怎么样"，
        // 不是"历史平均"。取值必须 ≥ 1（0 无合理语义，构造时会夹到 1）。
        std::size_t rttSampleCapacity = 16U;
    };

    struct Tick {
        // 本拍**该不该**发心跳。真正发不发由调用方决定（可能要发失败），
        // 发出去之后必须回告 onHeartbeatSent()。
        bool sendHeartbeat = false;

        // 本拍推进之后的链路状态，以及是否发生了 健康 → 失效 的跳变。
        bool linkDown = false;
        bool linkJustFailed = false;

        // tick() 每次都会填上当前推荐窗口；changed 表示它与上一次不同。
        // （Tick 只能由 tick() 产生，所以这两个字段的默认值不会被读到。）
        std::uint64_t recommendedAckTimeoutMs = 0U;
        bool recommendationChanged = false;
    };

    struct Stats {
        std::uint64_t heartbeatsSent = 0U;
        std::uint64_t heartbeatsAcked = 0U;
        std::uint64_t staleHeartbeatAcks = 0U;   // 对不上任何在途心跳的应答
        std::uint64_t rttSamples = 0U;
        std::uint64_t linkDownTransitions = 0U;
    };

    LinkMonitor();
    explicit LinkMonitor(Config config);

    // 推进时间。必须被**定频**调用（每次调用代表一次"要不要发"的机会）。
    Tick tick(std::uint64_t nowMs);

    // 心跳**实际发出**之后由调用方回告：监控器据此登记在途与发送时刻，
    // 之后才能用应答算出 RTT。
    //
    // seq 必须由调用方的**唯一**序列号计数器提供，不能用监控器自己的计数器 ——
    // AckTracker 按 seq 判重而不看类型，两套计数器必然撞车，撞上之后命令会
    // **静默发不出去**（sendCommand 的 canRegister 前置检查会拦住它）。
    void onHeartbeatSent(std::uint16_t seq, std::uint64_t nowMs);

    // 某个心跳被应答了。返回 true 表示它确实匹配到一个在途心跳。
    bool onHeartbeatAck(std::uint16_t seq, std::uint64_t nowMs);

    bool linkDown() const noexcept { return linkDown_; }
    bool hasRttSample() const noexcept { return !rttSamples_.empty(); }

    // 会分配（要排序副本），因此不标 noexcept。
    std::optional<std::uint64_t> rttP95Ms() const;

    std::uint64_t recommendedAckTimeoutMs() const noexcept;
    const Stats& stats() const noexcept { return stats_; }

private:
    struct InFlight {
        std::uint16_t seq = 0U;
        std::uint64_t sentAtMs = 0U;
    };

    // 在途心跳最多留这么多拍。超出就丢最旧的 —— 那一拍永远不会被应答了，
    // 留在表里只会让 RTT 统计被一段远古的延迟污染。
    static constexpr std::size_t kMaxInFlight = 8U;

    Config config_;
    Stats stats_{};

    bool hasTicked_ = false;
    std::uint64_t lastTickMs_ = 0U;
    std::uint64_t nextBeatMs_ = 0U;
    std::uint64_t lastAckMs_ = 0U;   // 最后一次"确凿存活证据"的时刻
    bool linkDown_ = false;
    std::uint64_t lastRecommendationMs_ = 0U;

    std::vector<InFlight> inFlight_;
    std::vector<std::uint64_t> rttSamples_;
};

}  // namespace mc
