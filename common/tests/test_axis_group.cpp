#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "mc/axis_group.hpp"

// 轴组同步移动的规划（P1-7）。这些用例测的**只有数学**：
// 不看设备、不看界面、不跑链路 —— 因为"两轴是不是同时到"这件事
// 必须先能被算清楚，才谈得上让设备去执行。
namespace {

using mc::AxisMoveInput;
using mc::GroupPlanError;
using mc::GroupMovePlan;

std::vector<AxisMoveInput> twoAxes(double axFrom, double axTo, double ayFrom, double ayTo) {
    return {AxisMoveInput{axFrom, axTo}, AxisMoveInput{ayFrom, ayTo}};
}

}  // namespace

// 核心判据：**所有轴在同一个时刻到达**。
// 判法不是"看速度等于多少"，而是把每一轴的 行程/速度 算回时长，四者必须相等 ——
// 后者才是"同步"的定义；只钉速度值等于给"恰好算对的实现"让路。
TEST(AxisGroup, EveryAxisTakesTheSameTimeSoTheyArriveTogether) {
    // X 走 100 度、Y 走 30 度：Y 更近，所以它该慢下来
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(0.0, 100.0, 0.0, 30.0), 50.0);
    ASSERT_TRUE(plan.ok()) << mc::groupPlanErrorText(plan.error);
    ASSERT_EQ(plan.axes.size(), 2U);

    const double xDuration = std::abs(plan.axes[0].deltaDeg / plan.axes[0].speedDegPerSec);
    const double yDuration = std::abs(plan.axes[1].deltaDeg / plan.axes[1].speedDegPerSec);
    EXPECT_NEAR(xDuration, plan.durationSec, 1e-9) << "X 轴不在公共时长内走到";
    EXPECT_NEAR(yDuration, plan.durationSec, 1e-9) << "Y 轴不在公共时长内走到";

    // 最远的那一轴吃满速度上限、其余按比例慢下来 —— 这正是"缩放"的含义。
    EXPECT_NEAR(std::abs(plan.axes[0].speedDegPerSec), 50.0, 1e-9) << "走最远的那轴该跑满上限";
    EXPECT_NEAR(std::abs(plan.axes[1].speedDegPerSec), 15.0, 1e-9) << "30 度按 100/50 的时长应是 15";
    EXPECT_LT(std::abs(plan.axes[1].speedDegPerSec), std::abs(plan.axes[0].speedDegPerSec));
}

// 方向：速度**带符号**。"两轴都朝正走"是最简单的情形，
// 反向的那一轴必须是负速度 —— 否则它会朝相反的方向找目标。
TEST(AxisGroup, SpeedsCarryTheSignOfTheTravel) {
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(10.0, -40.0, -10.0, 10.0), 25.0);
    ASSERT_TRUE(plan.ok()) << mc::groupPlanErrorText(plan.error);
    EXPECT_LT(plan.axes[0].speedDegPerSec, 0.0) << "X 从 10 走到 -40，速度必须为负";
    EXPECT_GT(plan.axes[1].speedDegPerSec, 0.0);
}

// 一轴不动：它的速度是 0，**公共时长仍由动的那些轴决定**。
// 这一格很容易写成"有轴不动就报错" —— 那是错的：单轴运动本来就是
// "另一轴速度为 0 的轴组运动"，它必须能走通。
TEST(AxisGroup, AStationaryAxisGetsZeroSpeedAndDoesNotShortenTheDuration) {
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(0.0, 0.0, 0.0, 60.0), 20.0);
    ASSERT_TRUE(plan.ok()) << mc::groupPlanErrorText(plan.error);
    EXPECT_DOUBLE_EQ(plan.axes[0].speedDegPerSec, 0.0);
    EXPECT_NEAR(plan.durationSec, 3.0, 1e-9);   // 60 / 20
    EXPECT_NEAR(std::abs(plan.axes[1].speedDegPerSec), 20.0, 1e-9);
}

// 三轴也一样：这是"轴组"，不是"恰好两轴"。
TEST(AxisGroup, ScalesAcrossMoreThanTwoAxes) {
    std::vector<AxisMoveInput> axes = {
        AxisMoveInput{0.0, 400.0}, AxisMoveInput{0.0, 100.0}, AxisMoveInput{0.0, -200.0}};
    const GroupMovePlan plan = mc::planSynchronizedMove(axes, 80.0);
    ASSERT_TRUE(plan.ok()) << mc::groupPlanErrorText(plan.error);
    EXPECT_NEAR(plan.durationSec, 5.0, 1e-9);   // 400 / 80
    for (const auto& axis : plan.axes) {
        EXPECT_NEAR(std::abs(axis.deltaDeg / axis.speedDegPerSec), plan.durationSec, 1e-9);
    }
    EXPECT_LT(plan.axes[2].speedDegPerSec, 0.0);
}

// 结构性判据（与上面那条互补）：**没有任何一轴超过给定上限**。
// 它由规划本身保证（T 是按最远的那一轴定的），但必须由用例守住 ——
// 改成"所有轴都跑上限"就会同时打红这一条和上面那条。
TEST(AxisGroup, NoAxisEverExceedsTheGivenSpeedLimit) {
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(0.0, 12.0, 0.0, 900.0), 100.0);
    ASSERT_TRUE(plan.ok()) << mc::groupPlanErrorText(plan.error);
    EXPECT_TRUE(mc::planRespectsSpeedLimit(plan, 100.0));
    for (const auto& axis : plan.axes) {
        EXPECT_LE(std::abs(axis.speedDegPerSec), 100.0);
    }
}

// ---------------------------------------------------------------------------
// 拒绝路径：规划不出来时**必须给出原因**，而不是回一个速度为 0 的计划。
// "发一组速度 0 的运动指令"在界面上看起来就是"协调启动成功"，那是误报。
// ---------------------------------------------------------------------------

TEST(AxisGroup, AnEmptyGroupIsRefused) {
    const GroupMovePlan plan = mc::planSynchronizedMove({}, 10.0);
    EXPECT_EQ(plan.error, GroupPlanError::NoAxes);
    EXPECT_FALSE(plan.ok());
    EXPECT_TRUE(plan.axes.empty());
}

TEST(AxisGroup, TooManyAxesIsRefusedRatherThanSilentlyTruncated) {
    std::vector<AxisMoveInput> axes;
    for (std::size_t i = 0U; i < mc::kMaxAxesPerGroup + 1U; ++i) {
        axes.push_back(AxisMoveInput{0.0, 10.0});
    }
    const GroupMovePlan plan = mc::planSynchronizedMove(axes, 10.0);
    EXPECT_EQ(plan.error, GroupPlanError::TooManyAxes);
}

TEST(AxisGroup, ANonPositiveSpeedLimitIsRefused) {
    EXPECT_EQ(mc::planSynchronizedMove(twoAxes(0.0, 10.0, 0.0, 10.0), 0.0).error,
              GroupPlanError::MaxSpeedNotPositive);
    EXPECT_EQ(mc::planSynchronizedMove(twoAxes(0.0, 10.0, 0.0, 10.0), -5.0).error,
              GroupPlanError::MaxSpeedNotPositive);
}

TEST(AxisGroup, NonFiniteInputsAreRefused) {
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(mc::planSynchronizedMove(twoAxes(0.0, nan, 0.0, 10.0), 10.0).error,
              GroupPlanError::NonFinite);
    EXPECT_EQ(mc::planSynchronizedMove(twoAxes(0.0, 10.0, 0.0, inf), 10.0).error,
              GroupPlanError::NonFinite);
    EXPECT_EQ(mc::planSynchronizedMove(twoAxes(0.0, 10.0, 0.0, 10.0), nan).error,
              GroupPlanError::NonFinite);
}

TEST(AxisGroup, AnAbsurdTravelIsRefused) {
    const GroupMovePlan plan =
        mc::planSynchronizedMove(twoAxes(0.0, mc::kMaxGroupTravelDeg * 2.0, 0.0, 1.0), 10.0);
    EXPECT_EQ(plan.error, GroupPlanError::TravelTooLarge);
}

// 全都不动 = **没有运动可做**，不是"成功地走了 0 度"。
TEST(AxisGroup, AGroupThatWouldNotMoveAtAllIsRefused) {
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(7.0, 7.0, -3.0, -3.0), 10.0);
    EXPECT_EQ(plan.error, GroupPlanError::NothingToDo);
    EXPECT_FALSE(plan.ok());
}

// 错误码的 token 与中文措辞都必须是**互不相同**的：两条不同的失败
// 在界面上长得一样，排查时就被带偏了。
TEST(AxisGroup, EveryErrorHasItsOwnTokenAndText) {
    const GroupPlanError all[] = {
        GroupPlanError::None,          GroupPlanError::NoAxes,
        GroupPlanError::TooManyAxes,   GroupPlanError::MaxSpeedNotPositive,
        GroupPlanError::NonFinite,     GroupPlanError::TravelTooLarge,
        GroupPlanError::NothingToDo,
    };
    for (std::size_t i = 0U; i < std::size(all); ++i) {
        EXPECT_STRNE(mc::groupPlanErrorName(all[i]), "");
        EXPECT_STRNE(mc::groupPlanErrorText(all[i]), "");
        for (std::size_t j = i + 1U; j < std::size(all); ++j) {
            EXPECT_STRNE(mc::groupPlanErrorName(all[i]), mc::groupPlanErrorName(all[j]))
                << "第 " << i << " 与第 " << j << " 个错误码的 token 撞了";
            EXPECT_STRNE(mc::groupPlanErrorText(all[i]), mc::groupPlanErrorText(all[j]))
                << "第 " << i << " 与第 " << j << " 个错误码的措辞撞了";
        }
    }
}

// 规划失败时**不给出可用的速度**：调用方一旦拿它去发命令，
// 就会把"没规划出来"变成"发了一组 0 速度"。这条守的是"失败不留半成品"。
TEST(AxisGroup, AFailedPlanCarriesNoUsableSpeeds) {
    const GroupMovePlan plan = mc::planSynchronizedMove(twoAxes(0.0, 10.0, 0.0, 10.0), -1.0);
    ASSERT_FALSE(plan.ok());
    for (const auto& axis : plan.axes) {
        EXPECT_DOUBLE_EQ(axis.speedDegPerSec, 0.0);
    }
    EXPECT_FALSE(mc::planRespectsSpeedLimit(plan, 10.0));
}
