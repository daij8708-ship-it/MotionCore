#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// 轴组同步移动的规划（P1-7）
//
// 问题：两台轴各自按自己的速度走，会**先到先停** —— 于是中间过程里机构是歪的。
// 对需要保持姿态的东西（龙门的两条腿、双驱的同步带、抓取机构的对位）来说，
// 中间过程歪一下就是机械被打、工件被划，事后还查不出来。
//
// 做法：**速度缩放式同步**。
//   ① 先算出一个公共的移动时长 T = 行程最大的那一轴的行程 / 允许的最大速度；
//   ② 每一轴按 v_i = Δ_i / T 走。
// 于是：没有一轴超过最大速度（T 就是按最远的那一轴定的），且所有轴在**同一时刻**到达。
//
// ⚠️ 它**不是插补**：中间不约束直线度，只保证"同时出发、同时到达"。
//    把它说成插补就是在吹 —— 真插补要设备侧按插补周期分发同步位置，
//    而本协议里设备收到的仍是一条条独立的命令。
//
// 这是**纯函数**：不看设备、不看界面、不改任何状态。于是"同步到达"这件事
// 可以被穷举验证，而不是等真机上拿两只手去比。
// ---------------------------------------------------------------------------

// 一个轴组最多几轴。与协议无关，纯粹是"别无限增长"；
// 双轴是本项目当前真正用到的规模（见 host 侧的 AxisGroup）。
inline constexpr std::size_t kMaxAxesPerGroup = 8U;

// 规划失败的原因。**不返回"一个假的 0 秒计划"**：调用方据此拒发命令，
// 而不是发出一组速度为 0 的"运动指令"。
enum class GroupPlanError : std::uint8_t {
    None = 0U,
    NoAxes,                // 一轴都没有
    TooManyAxes,           // 超过 kMaxAxesPerGroup
    MaxSpeedNotPositive,   // 最大速度不是正值 —— 没有它 T 无从谈起
    NonFinite,             // 行程或速度里有 NaN / Inf
    TravelTooLarge,        // 行程超出防荒谬上界
    NothingToDo,           // 所有轴的行程都是 0：没有运动，不该发命令
};

const char* groupPlanErrorName(GroupPlanError error) noexcept;
// 给人看的一句中文（界面 / 状态栏直接用）。
const char* groupPlanErrorText(GroupPlanError error) noexcept;

struct AxisMoveInput {
    double fromDeg = 0.0;
    double toDeg = 0.0;
};

struct AxisMovePlan {
    double deltaDeg = 0.0;        // to - from（带符号）
    double speedDegPerSec = 0.0;  // 带符号；0 = 这一轴不需要动
};

struct GroupMovePlan {
    GroupPlanError error = GroupPlanError::NoAxes;
    double durationSec = 0.0;        // 公共移动时长
    double maxTravelDeg = 0.0;       // 最长那一轴的行程绝对值（诊断 / 界面显示）
    std::vector<AxisMovePlan> axes;  // 与输入**同序**

    bool ok() const noexcept { return error == GroupPlanError::None; }
};

// 规划一次同步移动。
GroupMovePlan planSynchronizedMove(const std::vector<AxisMoveInput>& axes,
                                   double maxSpeedDegPerSec);

// 防荒谬上界。与协议层的量程**分开**：那是"帧能不能解"，
// 这是"规划得出来吗"。1e7 度远超任何真实行程，但远小于 double 溢出。
inline constexpr double kMaxGroupTravelDeg = 1.0e7;

// 规划出来的各轴速度是否都真的没超过给定上限（供调用方 / 测试复核）。
bool planRespectsSpeedLimit(const GroupMovePlan& plan, double maxSpeedDegPerSec) noexcept;

}  // namespace mc
