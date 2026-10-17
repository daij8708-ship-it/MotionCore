#pragma once

#include <cstdint>

#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 设备状态机（阶段 4）
//
// 为什么它必须住在 libmc_core 里、而且必须是**纯逻辑**：
//   ① 上位机 UI、模拟器、将来的固件三处都要回答同一个问题 ——
//      "现在这个状态，这条命令能不能收"。规则写三份必然漂移；写一份还能被穷举测试。
//   ② 它不碰串口、不碰 Qt、不碰时钟（时间是调用方喂进来的事件），
//      因此 (状态 × 故障锁存位 × 事件) 的**全部组合**都能被断言：
//      合法的必须生效，非法的一律被拒且状态不变。
//
// ⚠️ 一个很容易漏掉的规则：**故障一旦置位，就只能由 RecoveryOk 清除**。
//    若不设锁存位，`Fault ──链路断──► Disconnected ──重连──► Initializing ──握手──► Ready`
//    会把故障顺手洗掉 —— 直接违反 `docs/PROTOCOL.md` §5.3 的不变式 2
//    （"Fault 不允许自动复位"）。而且这条路径**恰好是最容易发生的那一条**：
//    设备出事 → 人跑过去拔线 → 重新上电 → 界面显示一切正常。
// ---------------------------------------------------------------------------

enum class DeviceState : std::uint8_t {
    Disconnected = 0U,   // 链路断 / 端口未开
    Initializing = 1U,   // 已连上，等 HELLO 往返
    Ready        = 2U,   // 握手完成、参数一致，输出未使能
    Running      = 3U,   // 已使能，按 target 运动
    Stopping     = 4U,   // 收到 STOP，正把速度收到零
    Fault        = 5U,   // 有故障：只允许 STOP / HEARTBEAT / HELLO / READ_PARAMS / CLEAR_FAULT
    Recovery     = 6U,   // 人工确认后的中间态，恢复校验通过才回 Ready
    // 回零中（P1-6）。它与 Running 是**两回事**：都在让电机转，但回零
    // 不接受任何目标值（目标是"零点"，一个由设备定义的地方），也不接受改模式。
    // 把它并进 Running 会让"现在这个运动是不是操作员要的那个"说不清。
    Home         = 7U,
};

inline constexpr std::uint8_t kDeviceStateCount = 8U;

// 遥测帧的 state 字段直接发这个枚举的编号（0..7）。两端必须一致，
// 因此这张映射表由 DeviceState.WireValuesMatchSpec 用例钉住 ——
// 不靠"反正都是连着的 0..7"心算。
const char* deviceStateName(DeviceState state) noexcept;

enum class DeviceEvent : std::uint8_t {
    LinkOpened = 0U,       // 端口打开
    LinkLost,              // 通信丢失 / 端口关闭
    HandshakeOk,           // HELLO 往返成功 + 参数校验通过
    HandshakeTimeout,      // 握手超时
    StartAccepted,         // START 被门禁放行
    StopReceived,          // 收到 STOP（幂等安全动作）
    StopCompleted,         // 速度已收敛到零
    FaultRaised,           // 产生故障（超速 / 超限位 / CRC 连错 / 设备侧通信丢失）
    RecoveryConfirmed,     // 人工确认
    RecoveryOk,            // 恢复校验通过
    HomeAccepted,          // 回零被门禁放行，设备开始朝零点走
    HomeCompleted,         // 到位并把那里定义成零点
};

inline constexpr std::uint8_t kDeviceEventCount = 12U;

const char* deviceEventName(DeviceEvent event) noexcept;

// 遥测帧的 fault_code 字段取值。
enum class FaultCode : std::uint8_t {
    None = 0U,
    CommunicationLoss = 1U,   // 设备侧在运行中太久没收到任何合法帧（F7）
    OverSpeed = 2U,           // 阶段 4 固件侧实现
    OverLimit = 3U,
    CrcBurst = 4U,
    Forced = 5U,              // 仅供测试注入
    HomeFailed = 6U,          // 回零超时/走不到零点（P1-6）
};

const char* faultCodeName(FaultCode code) noexcept;

// 命令门禁的结论。**只看状态，不看载荷** —— 载荷合法性由 payload 层的 isValid 负责。
enum class CommandVerdict : std::uint8_t {
    Allow = 0U,
    RejectIllegalState = 1U,
};

struct TransitionResult {
    bool accepted = false;      // 这个 (状态, 锁存位, 事件) 组合是否合法
    bool stateChanged = false;  // 合法但幂等（例如 Ready 里再收一次 STOP）时为 false
    DeviceState from = DeviceState::Disconnected;
    DeviceState to = DeviceState::Disconnected;
    bool faultLatchedAfter = false;   // 事件之后故障锁存位的值
};

// 门禁表的唯一真源。做成自由函数是为了能被"对 8×10 全表逐格断言"的用例直接调用。
CommandVerdict commandVerdict(DeviceState state, MessageType type) noexcept;

class DeviceStateMachine {
public:
    explicit DeviceStateMachine(DeviceState initial = DeviceState::Disconnected) noexcept;

    DeviceState state() const noexcept { return state_; }
    bool is(DeviceState state) const noexcept { return state_ == state; }

    // 故障是否处于锁存（还没被人工确认清除）。
    // 锁存期间即使重连成功也回不到 Ready —— 见文件头那段说明。
    bool faultLatched() const noexcept { return faultLatched_; }

    CommandVerdict verdictFor(MessageType type) const noexcept {
        return commandVerdict(state_, type);
    }

    // 施加一个事件。被拒时状态与锁存位都不变，并累加 rejectedTransitions()。
    TransitionResult apply(DeviceEvent event) noexcept;

    // 只读查询：给定起点与锁存位，施加事件会得到什么。不改本机状态、不计数。
    // 供穷举用例把 (8 状态 × 2 锁存位 × 12 事件) 全表逐格对照规格 ——
    // 这些组合里有相当一部分（例如 Running 且带故障锁存）在实际中不可达，
    // 但"不可达"是运行时性质，不能拿它当不测的理由。
    TransitionResult simulate(DeviceState from, bool faultLatched,
                              DeviceEvent event) const noexcept;

    std::uint64_t rejectedTransitions() const noexcept { return rejectedTransitions_; }

private:
    void enterFault() noexcept;

    DeviceState state_;
    bool faultLatched_ = false;
    std::uint64_t rejectedTransitions_ = 0U;
};

// 设备侧通信丢失保护（F7）的超时窗口。
//
// 放在这里当显式常量，不散落到调用方：这是安全相关的定值。
//
// 定这个数的**唯一依据是上位机的发帧节奏**，不是"越快越好"：
//   设备能收到东西的唯一来源就是上位机，而上位机除了心跳（默认周期 1000 ms）
//   并不定期发别的，所以正常情况下设备看到的最长静默就是一个心跳周期。
//   ⇒ 窗口**必须大于**心跳周期，否则心跳之间的空档就会被判成丢线 ——
//     链路完全正常，设备自己跳 Fault。
//   （我第一版把它写成"远小于心跳周期"，方向是反的，一跑端到端用例就暴露了：
//     健康链路上的设备自己进了 Fault。）
//
// 取 3000 ms = 上位机允许连丢 3 拍，与 LinkMonitor::missedBeatsToDeclareLinkDown
// 同一个口径。代价是设备最多带着陈旧目标再跑 3 s（384 deg/s ≈ 3.2 圈）。
// 固件侧更快的中断检测（UART 错误/空闲中断）属阶段 4 的固件改造；
// 本值只保证一件事：**绝不会无限期地带着陈旧目标跑下去**。
inline constexpr std::uint64_t kDeviceCommLossTimeoutMs = 3000U;

}  // namespace mc
