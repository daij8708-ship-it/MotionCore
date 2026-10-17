#include "mc/axis_group.hpp"

#include <cmath>

namespace mc {
namespace {

// 按错误码取措辞。**用 switch 而不是数组**：新增一个错误码时编译器会替我们
// 把这里指出来（-Werror=switch），不会静默掉进一个"未知错误"的兜底里。
const char* errorToken(GroupPlanError error) noexcept {
    switch (error) {
        case GroupPlanError::None: return "NONE";
        case GroupPlanError::NoAxes: return "NO_AXES";
        case GroupPlanError::TooManyAxes: return "TOO_MANY_AXES";
        case GroupPlanError::MaxSpeedNotPositive: return "MAX_SPEED_NOT_POSITIVE";
        case GroupPlanError::NonFinite: return "NON_FINITE";
        case GroupPlanError::TravelTooLarge: return "TRAVEL_TOO_LARGE";
        case GroupPlanError::NothingToDo: return "NOTHING_TO_DO";
    }
    return "UNKNOWN";
}

const char* errorText(GroupPlanError error) noexcept {
    switch (error) {
        case GroupPlanError::None: return "规划成功";
        case GroupPlanError::NoAxes: return "轴组里一个轴都没有";
        case GroupPlanError::TooManyAxes: return "轴数超过轴组上限";
        case GroupPlanError::MaxSpeedNotPositive: return "最大速度必须是正值";
        case GroupPlanError::NonFinite: return "行程或速度里有非有限值";
        case GroupPlanError::TravelTooLarge: return "行程超出上界";
        case GroupPlanError::NothingToDo: return "所有轴的行程都是零，没有运动可做";
    }
    return "未知的规划错误";
}

}  // namespace

const char* groupPlanErrorName(GroupPlanError error) noexcept { return errorToken(error); }

const char* groupPlanErrorText(GroupPlanError error) noexcept { return errorText(error); }

GroupMovePlan planSynchronizedMove(const std::vector<AxisMoveInput>& axes,
                                   double maxSpeedDegPerSec) {
    GroupMovePlan plan;
    plan.axes.assign(axes.size(), AxisMovePlan{});

    if (axes.empty()) {
        plan.error = GroupPlanError::NoAxes;
        return plan;
    }
    if (axes.size() > kMaxAxesPerGroup) {
        plan.error = GroupPlanError::TooManyAxes;
        return plan;
    }
    if (!std::isfinite(maxSpeedDegPerSec)) {
        plan.error = GroupPlanError::NonFinite;
        return plan;
    }
    if (maxSpeedDegPerSec <= 0.0) {
        plan.error = GroupPlanError::MaxSpeedNotPositive;
        return plan;
    }

    double maxTravel = 0.0;
    for (std::size_t i = 0U; i < axes.size(); ++i) {
        const double from = axes[i].fromDeg;
        const double to = axes[i].toDeg;
        if (!std::isfinite(from) || !std::isfinite(to)) {
            plan.error = GroupPlanError::NonFinite;
            return plan;
        }
        const double delta = to - from;
        // delta 本身溢出的情况由上面两个 isfinite + 下面这条上界一起挡住
        // （两者都有限时，差的绝对值最大约 2 倍量级，仍远小于上界的两倍）。
        if (!std::isfinite(delta) || std::abs(delta) > kMaxGroupTravelDeg) {
            plan.error = std::isfinite(delta) ? GroupPlanError::TravelTooLarge
                                              : GroupPlanError::NonFinite;
            return plan;
        }
        plan.axes[i].deltaDeg = delta;
        const double travel = std::abs(delta);
        if (travel > maxTravel) {
            maxTravel = travel;
        }
    }

    if (maxTravel == 0.0) {
        // 所有轴都不动。**这不是成功**：发一组"速度为 0 的运动指令"会让
        // 操作员看到"协调启动成功"，而实际上什么都没发生 —— 那也是误报。
        plan.error = GroupPlanError::NothingToDo;
        return plan;
    }

    const double duration = maxTravel / maxSpeedDegPerSec;
    plan.durationSec = duration;
    plan.maxTravelDeg = maxTravel;
    plan.error = GroupPlanError::None;
    for (AxisMovePlan& axis : plan.axes) {
        // 行程最远的那一轴正好跑到上限，其余按比例慢下来 ——
        // 于是"同一时刻出发"必然推出"同一时刻到达"。
        axis.speedDegPerSec = axis.deltaDeg / duration;
    }
    return plan;
}

bool planRespectsSpeedLimit(const GroupMovePlan& plan, double maxSpeedDegPerSec) noexcept {
    if (!plan.ok() || !std::isfinite(maxSpeedDegPerSec)) {
        return false;
    }
    for (const AxisMovePlan& axis : plan.axes) {
        if (!std::isfinite(axis.speedDegPerSec)) {
            return false;
        }
        // 用一点相对容差：maxTravel / vmax 与 vmax / (maxTravel/vmax) 之间
        // 有一次除法再乘回来，浮点上不保证逐位相等。
        const double limit = maxSpeedDegPerSec * (1.0 + 1.0e-9);
        if (std::abs(axis.speedDegPerSec) > limit) {
            return false;
        }
    }
    return true;
}

}  // namespace mc
