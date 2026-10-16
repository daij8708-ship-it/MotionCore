#include "mc/ack_tracker.hpp"

#include <algorithm>

namespace mc {

AckTracker::AckTracker() : config_(Config{}) {}

AckTracker::AckTracker(Config config) : config_(config) {}

bool AckTracker::canRegister(MessageType requestType, std::uint16_t seq) const noexcept {
    return requiresAck(requestType) && !isPending(seq);
}

bool AckTracker::registerPending(MessageType requestType, std::uint16_t seq, std::uint64_t nowMs) {
    if (!requiresAck(requestType)) {
        // STOP / TELEMETRY / 各类 ACK 没有与之配对的应答，等它只会永远超时。
        ++stats_.rejectedNotAwaitable;
        return false;
    }
    if (isPending(seq)) {
        ++stats_.rejectedDuplicateSeq;
        return false;
    }

    Pending item{};
    item.requestType = requestType;
    item.seq = seq;
    item.deadlineMs = nowMs + config_.timeoutMs;
    item.attempt = 0U;
    pending_.push_back(item);

    ++stats_.registered;
    return true;
}

bool AckTracker::onAck(MessageType ackType, std::uint16_t seq) {
    const auto it = std::find_if(pending_.begin(), pending_.end(),
                                 [seq](const Pending& item) { return item.seq == seq; });
    if (it == pending_.end()) {
        ++stats_.staleAcks;
        return false;
    }

    MessageType expected = MessageType::Nack;
    if (!tryAckOf(it->requestType, expected) || expected != ackType) {
        // seq 对得上但应答类型不对：这是协议错误，不能当成完成 ——
        // 否则一个乱回的帧会把在等的请求"骗"成成功。请求保持等待。
        ++stats_.mismatchedAcks;
        return false;
    }

    pending_.erase(it);
    ++stats_.completed;
    return true;
}

bool AckTracker::cancelPending(std::uint16_t seq) {
    const auto it = std::find_if(pending_.begin(), pending_.end(),
                                 [seq](const Pending& item) { return item.seq == seq; });
    if (it == pending_.end()) {
        return false;
    }
    pending_.erase(it);
    ++stats_.rejectedByPeer;
    return true;
}

std::vector<TimeoutEvent> AckTracker::pollTimeouts(std::uint64_t nowMs) {
    std::vector<TimeoutEvent> events;

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowMs < it->deadlineMs) {
            ++it;
            continue;
        }

        TimeoutEvent event{};
        event.requestType = it->requestType;
        event.seq = it->seq;
        event.attempt = it->attempt;

        if (isPeriodicProbe(it->requestType)) {
            // 周期性探测帧超时只意味着"这一拍没收到应答"，不是故障：
            // 直接移除，下一个周期自然会发新的。链路是否失效由 LinkMonitor
            // 按"连续丢 N 拍"判定 —— 单次超时判不了链路，判了就会误报。
            event.outcome = TxOutcome::Dropped;
            ++stats_.timedOutDropped;
            events.push_back(event);
            it = pending_.erase(it);
            continue;
        }

        if (isRetryable(it->requestType) && it->attempt < config_.maxRetries) {
            event.outcome = TxOutcome::Retry;
            ++stats_.timedOutRetried;

            // 就地重登记：重发之后给一个新的超时窗口。
            ++it->attempt;
            it->deadlineMs = nowMs + config_.timeoutMs;
            events.push_back(event);
            ++it;
            continue;
        }

        // 走到这里有两种情况：
        //   a) 运动指令，不允许重传（ADR-003）
        //   b) 幂等命令，但重试次数已用尽 —— 链路大概率已经断了
        event.outcome = TxOutcome::Fault;
        ++stats_.timedOutFaulted;
        if (isRetryable(it->requestType)) {
            ++stats_.retriesExhausted;
        }
        events.push_back(event);
        it = pending_.erase(it);
    }

    return events;
}

bool AckTracker::isPending(std::uint16_t seq) const noexcept {
    return std::any_of(pending_.begin(), pending_.end(),
                       [seq](const Pending& item) { return item.seq == seq; });
}

void AckTracker::reset() noexcept {
    pending_.clear();
    stats_ = AckStats{};
}

}  // namespace mc
