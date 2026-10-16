#include "mc/link_monitor.hpp"

#include <algorithm>

namespace mc {

LinkMonitor::LinkMonitor() : LinkMonitor(Config{}) {}

LinkMonitor::LinkMonitor(Config config) : config_(config) {
    // 几个"零"没有合理语义，而且会构造出必然错误的状态，因此在入口夹住：
    //   周期 0        → 每拍都发，而容忍度 N×0 = 0 → 链路立刻被判失效
    //   容忍拍数 0    → 同样是"立刻判失效"
    //   样本容量 0    → 永远不会留下样本，推荐值看似停在初值却看不出原因
    if (config_.heartbeatPeriodMs == 0U) {
        config_.heartbeatPeriodMs = 1U;
    }
    if (config_.missedBeatsToDeclareLinkDown == 0U) {
        config_.missedBeatsToDeclareLinkDown = 1U;
    }
    if (config_.rttSampleCapacity == 0U) {
        config_.rttSampleCapacity = 1U;
    }
}

LinkMonitor::Tick LinkMonitor::tick(std::uint64_t nowMs) {
    Tick out;

    if (!hasTicked_) {
        hasTicked_ = true;
        lastAckMs_ = nowMs;
        nextBeatMs_ = nowMs;   // 第一拍立即发，不必先等一个周期
    } else if (nowMs < lastTickMs_) {
        // 时钟回退（换时间源，或测试直接给了更小的值）：重新对齐基准并清掉在途心跳。
        // 不这样处理的话，倒算出来的静默会瞬间把链路判死。
        lastAckMs_ = nowMs;
        nextBeatMs_ = nowMs;
        inFlight_.clear();
        linkDown_ = false;
    }
    lastTickMs_ = nowMs;

    if (nowMs >= nextBeatMs_) {
        out.sendHeartbeat = true;

        // 相对 now 顺延，而不是 nextBeatMs_ += period：调用方迟到时（界面卡了一下）
        // 不应该产生一串"追赶"心跳，那会在链路上制造突发流量。
        nextBeatMs_ = nowMs + config_.heartbeatPeriodMs;
    }

    const bool wasDown = linkDown_;

    const std::uint64_t silenceMs = (nowMs > lastAckMs_) ? (nowMs - lastAckMs_) : 0U;
    const std::uint64_t toleranceMs = static_cast<std::uint64_t>(
        config_.missedBeatsToDeclareLinkDown) * config_.heartbeatPeriodMs;

    linkDown_ = silenceMs >= toleranceMs;
    out.linkDown = linkDown_;
    out.linkJustFailed = linkDown_ && !wasDown;
    if (out.linkJustFailed) {
        ++stats_.linkDownTransitions;
    }

    out.recommendedAckTimeoutMs = recommendedAckTimeoutMs();
    out.recommendationChanged = (out.recommendedAckTimeoutMs != lastRecommendationMs_);
    lastRecommendationMs_ = out.recommendedAckTimeoutMs;

    return out;
}

void LinkMonitor::onHeartbeatSent(std::uint16_t seq, std::uint64_t nowMs) {
    InFlight beat;
    beat.seq = seq;
    beat.sentAtMs = nowMs;
    inFlight_.push_back(beat);

    if (inFlight_.size() > kMaxInFlight) {
        // 超出容量的那一拍永远不会被应答了，留在表里只会污染 RTT 统计。
        inFlight_.erase(inFlight_.begin());
    }

    ++stats_.heartbeatsSent;
}

bool LinkMonitor::onHeartbeatAck(std::uint16_t seq, std::uint64_t nowMs) {
    const auto it = std::find_if(inFlight_.begin(), inFlight_.end(),
                                 [seq](const InFlight& beat) { return beat.seq == seq; });
    if (it == inFlight_.end()) {
        // 一个我们没在等的心跳应答（陈旧 / 重复 / 伪造）。
        ++stats_.staleHeartbeatAcks;
        return false;
    }

    const std::uint64_t rttMs = (nowMs > it->sentAtMs) ? (nowMs - it->sentAtMs) : 0U;
    inFlight_.erase(it);

    rttSamples_.push_back(rttMs);
    if (rttSamples_.size() > config_.rttSampleCapacity) {
        rttSamples_.erase(rttSamples_.begin());
    }

    ++stats_.heartbeatsAcked;
    ++stats_.rttSamples;
    lastAckMs_ = nowMs;
    linkDown_ = false;
    return true;
}

std::optional<std::uint64_t> LinkMonitor::rttP95Ms() const {
    if (rttSamples_.empty()) {
        return std::nullopt;
    }

    std::vector<std::uint64_t> sorted = rttSamples_;
    std::sort(sorted.begin(), sorted.end());

    // 最近秩法：idx = ceil(0.95 × n) − 1。
    // 样本少时它自然退化成"取最大值" —— 对超时窗口这种宁保守勿激进的量，
    // 这是想要的行为，不是缺陷。
    const std::size_t idx = ((sorted.size() * 95U) + 99U) / 100U - 1U;
    return sorted[idx];
}

std::uint64_t LinkMonitor::recommendedAckTimeoutMs() const noexcept {
    const std::optional<std::uint64_t> p95 = rttP95Ms();
    if (!p95.has_value()) {
        // 一个样本都没有。此时给激进的小值会把健康的慢链路误判成超时，
        // 所以退回到显式配置的初值，等真实 RTT 到手再收紧或放宽。
        return config_.initialAckTimeoutMs;
    }

    const std::uint64_t scaled = (*p95) * config_.timeoutMarginPercent / 100U;
    return std::clamp(scaled, config_.minAckTimeoutMs, config_.maxAckTimeoutMs);
}

}  // namespace mc
