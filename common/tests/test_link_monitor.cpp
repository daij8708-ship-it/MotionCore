#include <gtest/gtest.h>

#include <cstdint>

#include "mc/link_monitor.hpp"

namespace {

using mc::LinkMonitor;

LinkMonitor::Config cfg(std::uint64_t periodMs = 1000U,
                        std::uint32_t missedBeats = 3U) {
    LinkMonitor::Config c;
    c.heartbeatPeriodMs = periodMs;
    c.missedBeatsToDeclareLinkDown = missedBeats;
    c.initialAckTimeoutMs = 200U;
    c.timeoutMarginPercent = 300U;
    c.minAckTimeoutMs = 50U;
    c.maxAckTimeoutMs = 2000U;
    c.rttSampleCapacity = 16U;
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// 心跳调度
// ---------------------------------------------------------------------------

// tick() 只回答"该不该发"，不代替调用方发送 —— 所以"已发送"要有单独的回告。
TEST(LinkMonitor, TickOnlyAsksItDoesNotSend) {
    LinkMonitor monitor{cfg()};

    const LinkMonitor::Tick first = monitor.tick(0U);
    EXPECT_TRUE(first.sendHeartbeat);
    EXPECT_EQ(monitor.stats().heartbeatsSent, 0ULL) << "此刻还没有谁真的把它发出去";

    monitor.onHeartbeatSent(7U, 0U);
    EXPECT_EQ(monitor.stats().heartbeatsSent, 1ULL);
}

TEST(LinkMonitor, BeatsFollowTheConfiguredPeriod) {
    LinkMonitor monitor{cfg(1000U)};

    EXPECT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);

    EXPECT_FALSE(monitor.tick(500U).sendHeartbeat);
    EXPECT_FALSE(monitor.tick(999U).sendHeartbeat);

    EXPECT_TRUE(monitor.tick(1000U).sendHeartbeat);
    monitor.onHeartbeatSent(2U, 1000U);

    EXPECT_FALSE(monitor.tick(1500U).sendHeartbeat);
    EXPECT_EQ(monitor.stats().heartbeatsSent, 2ULL);
}

TEST(LinkMonitor, LateCallerDoesNotCauseCatchUpStorm) {
    LinkMonitor monitor{cfg(1000U)};

    EXPECT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);

    // 调用方迟到了 10 个周期（界面卡了一下）。只应补发**一拍**，
    // 而不是把攒下的 10 拍一次性灌到链路上。
    EXPECT_TRUE(monitor.tick(10000U).sendHeartbeat);
    monitor.onHeartbeatSent(2U, 10000U);
    EXPECT_EQ(monitor.stats().heartbeatsSent, 2ULL);

    EXPECT_FALSE(monitor.tick(10500U).sendHeartbeat);
    EXPECT_TRUE(monitor.tick(11000U).sendHeartbeat);
}

// ---------------------------------------------------------------------------
// RTT 采样
// ---------------------------------------------------------------------------

TEST(LinkMonitor, AckYieldsAnRttSampleAndKeepsLinkUp) {
    LinkMonitor monitor{cfg()};

    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(5U, 0U);
    EXPECT_TRUE(monitor.onHeartbeatAck(5U, 40U));

    EXPECT_TRUE(monitor.hasRttSample());
    ASSERT_TRUE(monitor.rttP95Ms().has_value());
    EXPECT_EQ(*monitor.rttP95Ms(), 40ULL);
    EXPECT_EQ(monitor.stats().heartbeatsAcked, 1ULL);
    EXPECT_EQ(monitor.stats().staleHeartbeatAcks, 0ULL);
    EXPECT_FALSE(monitor.tick(2000U).linkDown);
}

TEST(LinkMonitor, AckForUnknownSeqIsCountedAndChangesNothing) {
    LinkMonitor monitor{cfg()};

    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(5U, 0U);

    EXPECT_FALSE(monitor.onHeartbeatAck(99U, 10U));
    EXPECT_EQ(monitor.stats().staleHeartbeatAcks, 1ULL);
    EXPECT_EQ(monitor.stats().heartbeatsAcked, 0ULL);
    EXPECT_FALSE(monitor.hasRttSample());

    // 正对照：同一个方法在**在途**的 seq 上返回 true。
    EXPECT_TRUE(monitor.onHeartbeatAck(5U, 20U));
    EXPECT_EQ(monitor.stats().heartbeatsAcked, 1ULL);
}

TEST(LinkMonitor, OnlyTheMostRecentBeatsCanStillBeAcked) {
    LinkMonitor monitor{cfg(100U)};

    // 连发 9 拍都不应答。超出在途容量之后，最旧的那一拍被丢弃 ——
    // 它永远不会被应答了，留着只会让远古延迟污染 RTT 统计。
    for (std::uint16_t i = 1U; i <= 9U; ++i) {
        ASSERT_TRUE(monitor.tick(static_cast<std::uint64_t>(i) * 100U).sendHeartbeat);
        monitor.onHeartbeatSent(i, static_cast<std::uint64_t>(i) * 100U);
    }

    EXPECT_FALSE(monitor.onHeartbeatAck(1U, 1000U)) << "最旧的一拍已被丢弃";
    EXPECT_EQ(monitor.stats().staleHeartbeatAcks, 1ULL);

    // 正对照：最近那一拍照样能被应答并产出样本。
    EXPECT_TRUE(monitor.onHeartbeatAck(9U, 1000U));
    EXPECT_TRUE(monitor.hasRttSample());
}

// ---------------------------------------------------------------------------
// 链路失效判定
// ---------------------------------------------------------------------------

TEST(LinkMonitor, LinkFailsAfterConsecutiveMissedBeats) {
    LinkMonitor monitor{cfg(1000U, 3U)};

    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 50U));   // 最后存活证据 = 50 ms

    // 容忍 3 × 1000 ms：差 1 ms 都不判，刚好到点才判。
    EXPECT_FALSE(monitor.tick(3049U).linkDown);

    const LinkMonitor::Tick failed = monitor.tick(3050U);
    EXPECT_TRUE(failed.linkDown);
    EXPECT_TRUE(failed.linkJustFailed);
    EXPECT_EQ(monitor.stats().linkDownTransitions, 1ULL);

    // 跳变只报一次：之后仍然 linkDown，但 linkJustFailed 已为 false。
    const LinkMonitor::Tick after = monitor.tick(4050U);
    EXPECT_TRUE(after.linkDown);
    EXPECT_FALSE(after.linkJustFailed);
    EXPECT_EQ(monitor.stats().linkDownTransitions, 1ULL);
}

TEST(LinkMonitor, AnAckAfterFailureRestoresTheLink) {
    LinkMonitor monitor{cfg(1000U, 3U)};

    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 0U));
    ASSERT_TRUE(monitor.tick(3000U).linkDown);

    ASSERT_TRUE(monitor.tick(4000U).sendHeartbeat);
    monitor.onHeartbeatSent(2U, 4000U);
    ASSERT_TRUE(monitor.onHeartbeatAck(2U, 4050U));
    EXPECT_FALSE(monitor.tick(4100U).linkDown);

    // 正对照：恢复不是"永久解除"—— 再静默 3 拍又重新判失效。
    EXPECT_TRUE(monitor.tick(7050U).linkDown);
}

TEST(LinkMonitor, ClockRollbackDoesNotDeclareTheLinkDown) {
    LinkMonitor monitor{cfg(1000U, 3U)};

    // 时间必须**先走到一个较大的值**，之后变小才算回退（0 → 5 是前进，不是回退）。
    ASSERT_TRUE(monitor.tick(5000U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 5000U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 5000U));

    // 时间源被换掉，nowMs 突然变小。若不做处理，倒算出的"静默"会把链路判死。
    const LinkMonitor::Tick back = monitor.tick(5U);
    EXPECT_FALSE(back.linkDown);
    EXPECT_TRUE(back.sendHeartbeat);   // 重新对齐后立即补一拍

    EXPECT_FALSE(monitor.tick(1005U).linkDown);
}

// ---------------------------------------------------------------------------
// 按实测 RTT 推荐超时窗口
// ---------------------------------------------------------------------------

TEST(LinkMonitor, WithoutAnySampleTheWindowStaysAtTheConfiguredInitialValue) {
    LinkMonitor monitor{cfg()};

    EXPECT_FALSE(monitor.hasRttSample());
    EXPECT_FALSE(monitor.rttP95Ms().has_value());
    // 没有样本时给激进的小值会把健康的慢链路误判成超时 —— 必须退回显式初值。
    EXPECT_EQ(monitor.recommendedAckTimeoutMs(), 200ULL);

    // 正对照：有样本之后同一个方法就跟着实测值走了。
    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 100U));
    EXPECT_EQ(monitor.recommendedAckTimeoutMs(), 300ULL);
}

TEST(LinkMonitor, RecommendedTimeoutFollowsMeasuredRoundTripTime) {
    LinkMonitor monitor{cfg()};

    // 单程 100 ms ⇒ RTT ≈ 200 ms。这是演示场景④的真实形状：
    // 硬编码 200 ms 的窗口会把这种链路误判成超时。
    ASSERT_TRUE(monitor.tick(0U).sendHeartbeat);
    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 200U));

    EXPECT_EQ(*monitor.rttP95Ms(), 200ULL);
    // 推荐窗口 = 200 × 300% = 600 ms，明显大于原来的 200 ms ——
    // 这就是"高延迟链路不再被误判"的依据。
    EXPECT_EQ(monitor.recommendedAckTimeoutMs(), 600ULL);
}

TEST(LinkMonitor, RecommendedTimeoutIsClampedToConfiguredBounds) {
    // RTT 极小 → 夹到下限，避免窗口小到必然误判。
    {
        LinkMonitor fast{cfg()};
        ASSERT_TRUE(fast.tick(0U).sendHeartbeat);
        fast.onHeartbeatSent(1U, 0U);
        ASSERT_TRUE(fast.onHeartbeatAck(1U, 1U));
        EXPECT_EQ(fast.recommendedAckTimeoutMs(), 50ULL);
    }
    // RTT 极大 → 夹到上限，避免窗口大到链路已经断了还不报。
    {
        LinkMonitor slow{cfg()};
        ASSERT_TRUE(slow.tick(0U).sendHeartbeat);
        slow.onHeartbeatSent(1U, 0U);
        ASSERT_TRUE(slow.onHeartbeatAck(1U, 5000U));
        EXPECT_EQ(slow.recommendedAckTimeoutMs(), 2000ULL);
    }
}

TEST(LinkMonitor, RecommendationChangedFiresOncePerActualChange) {
    LinkMonitor monitor{cfg()};

    const LinkMonitor::Tick first = monitor.tick(0U);
    EXPECT_TRUE(first.recommendationChanged);   // 初值第一次下发
    EXPECT_EQ(first.recommendedAckTimeoutMs, 200ULL);

    EXPECT_FALSE(monitor.tick(100U).recommendationChanged);
    EXPECT_FALSE(monitor.tick(200U).recommendationChanged);

    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 300U));   // RTT 300 → 900 ms
    const LinkMonitor::Tick changed = monitor.tick(300U);
    EXPECT_TRUE(changed.recommendationChanged);
    EXPECT_EQ(changed.recommendedAckTimeoutMs, 900ULL);

    EXPECT_FALSE(monitor.tick(400U).recommendationChanged);
}

TEST(LinkMonitor, DegenerateZeroConfigDoesNotInstantlyDeclareTheLinkDown) {
    LinkMonitor::Config zero;
    zero.heartbeatPeriodMs = 0U;
    zero.missedBeatsToDeclareLinkDown = 0U;
    zero.rttSampleCapacity = 0U;
    LinkMonitor monitor{zero};

    // 周期 0 与容忍 0 若原样使用，容忍时长 = 0×0 = 0 → 第一拍就把链路判死。
    const LinkMonitor::Tick first = monitor.tick(0U);
    EXPECT_TRUE(first.sendHeartbeat);
    EXPECT_FALSE(first.linkDown);

    // 样本容量 0 若原样使用，样本永远留不下（推荐值会悄悄停在初值却查不出原因）。
    monitor.onHeartbeatSent(1U, 0U);
    ASSERT_TRUE(monitor.onHeartbeatAck(1U, 10U));
    EXPECT_TRUE(monitor.hasRttSample());
}
