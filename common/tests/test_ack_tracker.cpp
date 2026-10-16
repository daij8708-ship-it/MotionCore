#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "mc/ack_tracker.hpp"

namespace {

using mc::AckTracker;
using mc::MessageType;
using mc::TimeoutEvent;
using mc::TxOutcome;

constexpr std::uint64_t kTimeoutMs = 200U;

AckTracker makeTracker(std::uint32_t maxRetries = 2U) {
    return AckTracker(AckTracker::Config{kTimeoutMs, maxRetries});
}

const TimeoutEvent* findEvent(const std::vector<TimeoutEvent>& events, std::uint16_t seq) {
    const auto it = std::find_if(events.begin(), events.end(),
                                 [seq](const TimeoutEvent& event) { return event.seq == seq; });
    return (it == events.end()) ? nullptr : &(*it);
}

}  // namespace

// ---------------------------------------------------------------------------
// 登记
// ---------------------------------------------------------------------------

TEST(AckTracker, RegisterRejectsTypesThatHaveNoAck) {
    AckTracker tracker = makeTracker();

    // STOP 是单向的、TELEMETRY 是周期上报、ACK 本身不会再被应答 ——
    // 等它们的 ACK 只会永远超时，所以必须在登记这一步就被挡住。
    EXPECT_FALSE(tracker.registerPending(MessageType::Stop, 1U, 0U));
    EXPECT_FALSE(tracker.registerPending(MessageType::Telemetry, 2U, 0U));
    EXPECT_FALSE(tracker.registerPending(MessageType::StartAck, 3U, 0U));

    EXPECT_EQ(tracker.pendingCount(), std::size_t{0});
    EXPECT_EQ(tracker.stats().rejectedNotAwaitable, 3ULL);

    // 正对照：同一方法在可等待的类型上返回 true。
    EXPECT_TRUE(tracker.registerPending(MessageType::Start, 4U, 0U));
    EXPECT_EQ(tracker.pendingCount(), std::size_t{1});
}

TEST(AckTracker, RegisterRejectsDuplicateSequence) {
    AckTracker tracker = makeTracker();

    EXPECT_TRUE(tracker.registerPending(MessageType::SetPid, 7U, 0U));
    EXPECT_FALSE(tracker.registerPending(MessageType::SetMode, 7U, 0U));
    EXPECT_EQ(tracker.stats().rejectedDuplicateSeq, 1ULL);
    EXPECT_EQ(tracker.pendingCount(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// 应答匹配
// ---------------------------------------------------------------------------

TEST(AckTracker, AckCompletesPendingRequest) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 100U, 0U));

    EXPECT_TRUE(tracker.onAck(MessageType::SetPidAck, 100U));
    EXPECT_EQ(tracker.pendingCount(), std::size_t{0});
    EXPECT_EQ(tracker.stats().completed, 1ULL);
    EXPECT_EQ(tracker.stats().staleAcks, 0ULL);
}

TEST(AckTracker, StaleAckIsCountedAndIgnored) {
    AckTracker tracker = makeTracker();

    // 没有任何在等 seq=5 的请求。
    EXPECT_FALSE(tracker.onAck(MessageType::StartAck, 5U));
    EXPECT_EQ(tracker.stats().staleAcks, 1ULL);
    EXPECT_EQ(tracker.stats().completed, 0ULL);

    // 完成之后再重复到达的同一个 ACK，也属于陈旧应答。
    ASSERT_TRUE(tracker.registerPending(MessageType::Start, 6U, 0U));
    EXPECT_TRUE(tracker.onAck(MessageType::StartAck, 6U));
    EXPECT_FALSE(tracker.onAck(MessageType::StartAck, 6U));
    EXPECT_EQ(tracker.stats().staleAcks, 2ULL);
    EXPECT_EQ(tracker.stats().completed, 1ULL);
}

// seq 对得上但应答类型不对 —— 这是协议错误，绝不能当成"完成"。
TEST(AckTracker, MismatchedAckTypeDoesNotCompleteTheRequest) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::Start, 9U, 0U));

    EXPECT_FALSE(tracker.onAck(MessageType::SetTargetAck, 9U));
    EXPECT_EQ(tracker.stats().mismatchedAcks, 1ULL);
    EXPECT_EQ(tracker.stats().completed, 0ULL);
    EXPECT_EQ(tracker.pendingCount(), std::size_t{1}) << "请求必须继续保持等待";

    // 正对照：正确的应答类型到达后照样能完成。
    EXPECT_TRUE(tracker.onAck(MessageType::StartAck, 9U));
    EXPECT_EQ(tracker.stats().completed, 1ULL);
}

// ---------------------------------------------------------------------------
// 超时边界
// ---------------------------------------------------------------------------

TEST(AckTracker, TimeoutFiresExactlyAtDeadline) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 1U, 1000U));

    EXPECT_TRUE(tracker.pollTimeouts(1000U + kTimeoutMs - 1U).empty()) << "差 1 ms 不应触发";

    const std::vector<TimeoutEvent> events = tracker.pollTimeouts(1000U + kTimeoutMs);
    ASSERT_EQ(events.size(), std::size_t{1});
    EXPECT_EQ(events[0].outcome, TxOutcome::Retry);
}

TEST(AckTracker, RetryWindowRestartsFromTheMomentOfPolling) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 1U, 0U));

    const std::vector<TimeoutEvent> first = tracker.pollTimeouts(kTimeoutMs);
    ASSERT_EQ(first.size(), std::size_t{1});
    EXPECT_EQ(first[0].outcome, TxOutcome::Retry);
    EXPECT_EQ(first[0].attempt, 0U);

    // 新窗口从"本次 poll 的时刻"起算，而不是从旧 deadline 起算 ——
    // 否则调用方迟到时会连着重发好几次。
    EXPECT_TRUE(tracker.pollTimeouts(kTimeoutMs + 1U).empty());
    EXPECT_TRUE(tracker.pollTimeouts(2U * kTimeoutMs - 1U).empty());

    const std::vector<TimeoutEvent> second = tracker.pollTimeouts(2U * kTimeoutMs);
    ASSERT_EQ(second.size(), std::size_t{1});
    EXPECT_EQ(second[0].outcome, TxOutcome::Retry);
    EXPECT_EQ(second[0].attempt, 1U);
}

// ---------------------------------------------------------------------------
// ADR-003：运动指令绝不自动重传
// ---------------------------------------------------------------------------

TEST(AckTracker, MotionCommandsFaultOnTimeoutAndAreNeverRetried) {
    for (const MessageType type : {MessageType::Start, MessageType::SetTarget}) {
        AckTracker tracker = makeTracker();
        ASSERT_TRUE(tracker.registerPending(type, 42U, 0U));

        const std::vector<TimeoutEvent> events = tracker.pollTimeouts(kTimeoutMs);
        ASSERT_EQ(events.size(), std::size_t{1}) << "type=" << mc::toString(type);
        EXPECT_EQ(events[0].outcome, TxOutcome::Fault) << "type=" << mc::toString(type);
        EXPECT_EQ(events[0].seq, 42U);
        EXPECT_EQ(tracker.pendingCount(), std::size_t{0}) << "必须移除，不能留在待重传队列里";
        EXPECT_EQ(tracker.stats().timedOutRetried, 0ULL);
        EXPECT_EQ(tracker.stats().timedOutFaulted, 1ULL);

        // 再来多少次 poll 都不会产生重传 —— 这条请求已经被判 Fault。
        EXPECT_TRUE(tracker.pollTimeouts(100U * kTimeoutMs).empty());

        // STOP 路径同理，只是它连登记都不允许。
        EXPECT_FALSE(tracker.registerPending(MessageType::Stop, 43U, 0U));
    }
}

TEST(AckTracker, IdempotentCommandRetriesThenFaultsWhenRetriesExhausted) {
    AckTracker tracker = makeTracker(2U);   // 允许 2 次额外重发
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 7U, 0U));

    std::uint64_t now = kTimeoutMs;
    for (std::uint32_t attempt = 0U; attempt < 2U; ++attempt) {
        const std::vector<TimeoutEvent> events = tracker.pollTimeouts(now);
        ASSERT_EQ(events.size(), std::size_t{1}) << "attempt=" << attempt;
        EXPECT_EQ(events[0].outcome, TxOutcome::Retry) << "attempt=" << attempt;
        EXPECT_EQ(events[0].attempt, attempt);
        EXPECT_EQ(tracker.pendingCount(), std::size_t{1});
        now += kTimeoutMs;
    }

    // 第 3 次超时：重试用尽 —— 链路大概率已经断了。
    const std::vector<TimeoutEvent> last = tracker.pollTimeouts(now);
    ASSERT_EQ(last.size(), std::size_t{1});
    EXPECT_EQ(last[0].outcome, TxOutcome::Fault);
    EXPECT_EQ(last[0].attempt, 2U);
    EXPECT_EQ(tracker.pendingCount(), std::size_t{0});
    EXPECT_EQ(tracker.stats().timedOutRetried, 2ULL);
    EXPECT_EQ(tracker.stats().timedOutFaulted, 1ULL);
    EXPECT_EQ(tracker.stats().retriesExhausted, 1ULL);

    EXPECT_TRUE(tracker.pollTimeouts(now + 100U * kTimeoutMs).empty());
}

TEST(AckTracker, AckArrivingAfterARetryStillCompletes) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 11U, 0U));

    ASSERT_EQ(tracker.pollTimeouts(kTimeoutMs).size(), std::size_t{1});
    ASSERT_EQ(tracker.pendingCount(), std::size_t{1});

    EXPECT_TRUE(tracker.onAck(MessageType::SetPidAck, 11U));
    EXPECT_EQ(tracker.stats().completed, 1ULL);
    EXPECT_EQ(tracker.stats().timedOutFaulted, 0ULL);
    EXPECT_EQ(tracker.pendingCount(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 多请求并发与整体状态
// ---------------------------------------------------------------------------

TEST(AckTracker, MultipleRequestsAreTrackedIndependently) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::Start, 1U, 0U));
    ASSERT_TRUE(tracker.registerPending(MessageType::SetTarget, 2U, 0U));
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 3U, 0U));
    EXPECT_EQ(tracker.pendingCount(), std::size_t{3});

    // 只完成中间那个，其余两个不受影响。
    EXPECT_TRUE(tracker.onAck(MessageType::SetTargetAck, 2U));
    EXPECT_EQ(tracker.pendingCount(), std::size_t{2});
    EXPECT_TRUE(tracker.isPending(1U));
    EXPECT_FALSE(tracker.isPending(2U));
    EXPECT_TRUE(tracker.isPending(3U));

    const std::vector<TimeoutEvent> events = tracker.pollTimeouts(kTimeoutMs);
    ASSERT_EQ(events.size(), std::size_t{2});

    const TimeoutEvent* start = findEvent(events, 1U);
    const TimeoutEvent* pid = findEvent(events, 3U);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(pid, nullptr);
    EXPECT_EQ(start->outcome, TxOutcome::Fault);   // 运动指令
    EXPECT_EQ(pid->outcome, TxOutcome::Retry);     // 幂等命令

    EXPECT_EQ(tracker.pendingCount(), std::size_t{1});
}

TEST(AckTracker, ResetClearsPendingAndStats) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::SetPid, 1U, 0U));
    EXPECT_FALSE(tracker.registerPending(MessageType::Stop, 2U, 0U));

    tracker.reset();

    EXPECT_EQ(tracker.pendingCount(), std::size_t{0});
    EXPECT_EQ(tracker.stats().registered, 0ULL);
    EXPECT_EQ(tracker.stats().rejectedNotAwaitable, 0ULL);
}

// ---------------------------------------------------------------------------
// 心跳：期待应答，但漏拍不是故障
// ---------------------------------------------------------------------------

TEST(AckTracker, HeartbeatTimeoutIsDroppedNotFaulted) {
    AckTracker tracker = makeTracker();
    ASSERT_TRUE(tracker.registerPending(MessageType::Heartbeat, 1U, 0U));

    const std::vector<TimeoutEvent> events = tracker.pollTimeouts(kTimeoutMs);
    ASSERT_EQ(events.size(), std::size_t{1});
    EXPECT_EQ(events[0].outcome, TxOutcome::Dropped);
    EXPECT_EQ(tracker.stats().timedOutDropped, 1ULL);
    EXPECT_EQ(tracker.pendingCount(), std::size_t{0}) << "漏拍应直接出队，等下一拍";
    EXPECT_EQ(tracker.stats().timedOutRetried, 0ULL);

    // 关键：心跳漏拍**不得**计入故障。否则健康链路上丢一个心跳就报 Fault，
    // 而链路是否失效本应由"连续丢 N 拍"累计判定。
    EXPECT_EQ(tracker.stats().timedOutFaulted, 0ULL);

    // 正对照：这次改动绝不能削弱安全取舍 —— 运动指令超时仍然判 Fault。
    EXPECT_TRUE(tracker.registerPending(MessageType::Start, 2U, kTimeoutMs));
    const std::vector<TimeoutEvent> motion = tracker.pollTimeouts(2U * kTimeoutMs);
    ASSERT_EQ(motion.size(), std::size_t{1});
    EXPECT_EQ(motion[0].outcome, TxOutcome::Fault);
}

// ---------------------------------------------------------------------------
// 自适应超时窗口
// ---------------------------------------------------------------------------

TEST(AckTracker, SetTimeoutAppliesToNewRequestsOnly) {
    AckTracker tracker = makeTracker();   // 默认 200 ms
    ASSERT_EQ(tracker.timeoutMs(), kTimeoutMs);

    ASSERT_TRUE(tracker.registerPending(MessageType::Start, 1U, 0U));   // 截止 200 ms
    tracker.setTimeoutMs(600U);
    EXPECT_EQ(tracker.timeoutMs(), 600ULL);

    // 在途请求保留各自的截止时刻，**不追溯延长** ——
    // 否则一次窗口调整会把早已超时的请求悄悄变成"还没到点"。
    const std::vector<TimeoutEvent> events = tracker.pollTimeouts(600U);
    ASSERT_EQ(events.size(), std::size_t{1});
    EXPECT_EQ(events[0].seq, 1U);

    // 新登记的请求用新窗口：600 ms 发出，1200 ms 才到期。
    ASSERT_TRUE(tracker.registerPending(MessageType::SetTarget, 2U, 600U));
    EXPECT_TRUE(tracker.pollTimeouts(1199U).empty());
    EXPECT_FALSE(tracker.pollTimeouts(1200U).empty());
}
