#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "mc_host/curve_model.hpp"
#include "mc_host/telemetry_ring.hpp"

// 曲线数据层的用例。
//
// 刻意**不碰任何 Qt 类**：抽稀与取窗是纯数据逻辑，能在没有显示器的环境里被钉住。
// 绘制留在 CurvePlot 里，那部分只做冒烟（见 test_ui_smoke.cpp）—— 把像素当判据
// 又脆又假，真要说"这条线对不对"是人看的事。
namespace {

using mchost::CurveChannel;
using mchost::CurvePoint;
using mchost::CurveWindow;
using mchost::TelemetryRing;
using mchost::TelemetrySample;

// 造一条时间戳严格递增的缓冲。单调性是 extractSeries 二分查找的前提，
// 测试数据必须满足它 —— 不满足的话测的就不是真实数据形状。
TelemetryRing makeRing(std::size_t count, double stepMs = 10.0) {
    TelemetryRing ring;
    for (std::size_t i = 0U; i < count; ++i) {
        TelemetrySample sample;
        sample.tMs = static_cast<double>(i) * stepMs;
        sample.velocityDegPerSec = static_cast<float>(i);
        sample.positionDeg = static_cast<float>(i) * 2.0F;
        sample.target = static_cast<float>(i) * 3.0F;
        ring.push(sample);
    }
    return ring;
}

bool containsPair(const std::vector<CurvePoint>& points, const CurvePoint& wanted) {
    for (const CurvePoint& point : points) {
        if (point.tMs == wanted.tMs && point.value == wanted.value) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(CurveModel, EachChannelReadsItsOwnField) {
    TelemetrySample sample;
    sample.velocityDegPerSec = 12.5F;
    sample.positionDeg = -340.0F;
    sample.target = 99.0F;

    EXPECT_DOUBLE_EQ(mchost::channelValue(sample, CurveChannel::Velocity), 12.5);
    EXPECT_DOUBLE_EQ(mchost::channelValue(sample, CurveChannel::Position), -340.0);
    EXPECT_DOUBLE_EQ(mchost::channelValue(sample, CurveChannel::Target), 99.0);
}

TEST(CurveModel, ChannelNamesAndUnitsMatchTheProtocolSemantics) {
    // 单位来自协议（PROTOCOL.md §5.3）：目标值的单位随运行模式变，界面上默认按速度环
    // 显示，切到位置环时由页面改标签 —— 模型这一层只给默认量纲。
    EXPECT_STREQ(mchost::channelUnit(CurveChannel::Velocity), "deg/s");
    EXPECT_STREQ(mchost::channelUnit(CurveChannel::Position), "deg");
    EXPECT_STREQ(mchost::channelName(CurveChannel::Velocity), "速度");
    EXPECT_STREQ(mchost::channelName(CurveChannel::Position), "位置");
    EXPECT_STREQ(mchost::channelName(CurveChannel::Target), "目标");
}

TEST(CurveModel, ExtractSeriesWindowBoundsAreInclusive) {
    const TelemetryRing ring = makeRing(10U);   // t = 0, 10, ... 90

    const CurveWindow window{20.0, 50.0};
    const std::vector<CurvePoint> points = mchost::extractSeries(ring, CurveChannel::Velocity, window);

    ASSERT_EQ(points.size(), 4U);
    EXPECT_DOUBLE_EQ(points.front().tMs, 20.0);
    EXPECT_DOUBLE_EQ(points.back().tMs, 50.0);
    EXPECT_DOUBLE_EQ(points.front().value, 2.0);   // 速度通道 = 序号
}

TEST(CurveModel, ExtractSeriesClipsToTheBufferedRange) {
    const TelemetryRing ring = makeRing(10U);

    // 窗口比数据宽：只给交叠的那一段，不补点、不外插。
    const std::vector<CurvePoint> points =
        mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{-100.0, 25.0});

    ASSERT_EQ(points.size(), 3U);
    EXPECT_DOUBLE_EQ(points.front().tMs, 0.0);
    EXPECT_DOUBLE_EQ(points.back().tMs, 20.0);
}

TEST(CurveModel, ExtractSeriesWithAnInvertedWindowIsEmpty) {
    const TelemetryRing ring = makeRing(10U);

    // fromMs > toMs 是"窗口不成立"，不是"要全部"。返回全部会把"没数据"伪装成"有数据"。
    EXPECT_TRUE(mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{50.0, 20.0}).empty());
}

TEST(CurveModel, ExtractSeriesWithAFutureWindowIsEmpty) {
    const TelemetryRing ring = makeRing(10U);

    EXPECT_TRUE(mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{200.0, 300.0}).empty());
}

TEST(CurveModel, ExtractSeriesOnAnEmptyRingIsEmpty) {
    const TelemetryRing ring;

    EXPECT_TRUE(mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{0.0, 1000.0}).empty());
}

TEST(CurveModel, ExtractSeriesPicksTheRequestedChannel) {
    const TelemetryRing ring = makeRing(10U);

    const std::vector<CurvePoint> velocity =
        mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{30.0, 30.0});
    const std::vector<CurvePoint> position =
        mchost::extractSeries(ring, CurveChannel::Position, CurveWindow{30.0, 30.0});
    const std::vector<CurvePoint> target =
        mchost::extractSeries(ring, CurveChannel::Target, CurveWindow{30.0, 30.0});

    ASSERT_EQ(velocity.size(), 1U);
    ASSERT_EQ(position.size(), 1U);
    ASSERT_EQ(target.size(), 1U);
    EXPECT_DOUBLE_EQ(velocity.front().value, 3.0);
    EXPECT_DOUBLE_EQ(position.front().value, 6.0);
    EXPECT_DOUBLE_EQ(target.front().value, 9.0);
}

TEST(CurveModel, DecimationLeavesASeriesBelowTheLimitUntouched) {
    const TelemetryRing ring = makeRing(10U);
    const std::vector<CurvePoint> points =
        mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{0.0, 90.0});
    ASSERT_EQ(points.size(), 10U);

    const std::vector<CurvePoint> out = mchost::decimateSeries(points, 10U);

    // 抽稀只该在真的超限时发生 —— 没超限还"聪明"一下就是凭空引入误差。
    ASSERT_EQ(out.size(), points.size());
    for (std::size_t i = 0U; i < points.size(); ++i) {
        EXPECT_DOUBLE_EQ(out[i].tMs, points[i].tMs);
        EXPECT_DOUBLE_EQ(out[i].value, points[i].value);
    }
}

TEST(CurveModel, DecimationWithAZeroBudgetReturnsNothing) {
    const TelemetryRing ring = makeRing(10U);
    const std::vector<CurvePoint> points =
        mchost::extractSeries(ring, CurveChannel::Velocity, CurveWindow{0.0, 90.0});

    EXPECT_TRUE(mchost::decimateSeries(points, 0U).empty());
}

TEST(CurveModel, DecimationStaysWithinTheBudget) {
    std::vector<CurvePoint> points;
    for (std::size_t i = 0U; i < 4000U; ++i) {
        points.push_back(CurvePoint{static_cast<double>(i), std::sin(static_cast<double>(i) * 0.05)});
    }

    const std::vector<CurvePoint> out = mchost::decimateSeries(points, 300U);

    EXPECT_LE(out.size(), 300U);
    EXPECT_GT(out.size(), 0U);
}

TEST(CurveModel, DecimationKeepsTheGlobalExtremes) {
    std::vector<CurvePoint> points;
    for (std::size_t i = 0U; i < 1000U; ++i) {
        points.push_back(CurvePoint{static_cast<double>(i), 0.0});
    }
    // 尖峰刻意放在**桶内部**而不是桶边界上：落在边界的点即使抽稀逻辑退化成"取桶首"
    // 也能侥幸留下，那样的用例证明不了 min/max 分桶真的在起作用。
    points[505].value = 1000.0;    // 一闪而过的过速尖峰
    points[506].value = -1000.0;

    const std::vector<CurvePoint> out = mchost::decimateSeries(points, 100U);

    // 这正是 min/max 分桶存在的理由：等间隔取样会让这两个点从图上消失，
    // 而尖峰恰恰是监控最该看见的东西。
    EXPECT_TRUE(containsPair(out, CurvePoint{505.0, 1000.0}));
    EXPECT_TRUE(containsPair(out, CurvePoint{506.0, -1000.0}));
}

TEST(CurveModel, DecimationOutputStaysInTimeOrder) {
    std::vector<CurvePoint> points;
    for (std::size_t i = 0U; i < 1234U; ++i) {
        points.push_back(CurvePoint{static_cast<double>(i), std::sin(static_cast<double>(i) * 0.3)});
    }

    const std::vector<CurvePoint> out = mchost::decimateSeries(points, 128U);

    ASSERT_GE(out.size(), 2U);
    for (std::size_t i = 1U; i < out.size(); ++i) {
        // 乱序输出的折线会自己打结 —— 桶内先极小后极大是**按时间**排的，不是按值。
        EXPECT_LE(out[i - 1U].tMs, out[i].tMs);
    }
}

TEST(CurveModel, DecimationOnlyEverEmitsPointsThatWereInTheInput) {
    std::vector<CurvePoint> points;
    for (std::size_t i = 0U; i < 777U; ++i) {
        points.push_back(CurvePoint{static_cast<double>(i),
                                    std::sin(static_cast<double>(i) * 0.11) * 50.0});
    }

    const std::vector<CurvePoint> out = mchost::decimateSeries(points, 64U);

    // 抽稀是**选点**不是**造点**：不插值、不平滑。每个输出点都必须能在输入里找到原件。
    ASSERT_GT(out.size(), 0U);
    for (const CurvePoint& point : out) {
        EXPECT_TRUE(containsPair(points, point));
    }
}
