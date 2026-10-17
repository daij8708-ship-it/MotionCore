#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/device_state.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// InterlockTable —— 软件联锁规则表（P0）
//
// 它解决的是"按不动启动的时候，人不知道到底缺什么"这件事。
// 设备侧的门禁只会一次回一个 NAK（"状态不允许"），而上位机侧的条件散在好几处
// 判断里 —— 操作员于是要一条一条试：先改模式、再想是不是报警没确认、
// 再怀疑是不是会话断了。这个过程既慢又容易漏。
//
// 所以这里把"启动之前必须成立的条件"列成一张**表**：每条有编号、有等级、
// 有它存在的理由，一次判完，把没满足的**全部**列出来（不短路）。
//
// ⚠️ 两条必须说清的边界：
//   ① 这张表是**软件**联锁。它代替不了任何物理安全回路 —— 急停、限位开关、
//      安全继电器都在电气回路上，与这里无关。软件能做的是"不发出不该发的命令"，
//      不是"保证人不受伤"。
//   ② 表里**不重复**设备侧状态机已经判过的东西吗？不 —— 恰恰相反：
//      "设备必须停在 Ready"这条设备也会判，但设备判完了只会回一句"状态不允许"，
//      而这里能说清"现在是 Running，要变成 Ready"。**同一条约束在两处出现不是
//      重复，是两层**：设备那一层是最后一道闸（它必须自己守住），
//      这一层是给人看的说明。判据的具体数值仍然只有设备那一侧是权威。
//
// 编号是稳定的接口（日志、界面、将来的固件都要用同一份），措辞另有一处
// （宿主侧的 interlock_text）。
// ---------------------------------------------------------------------------

enum class InterlockSeverity : std::uint8_t {
    // 不满足就**不许发**启动命令。
    Block = 0U,
    // 只提示，不拦。列出来是因为"操作员该知道"，而不是因为"设备会拒绝"。
    Warn = 1U,
};

struct InterlockRule {
    std::uint16_t code;
    const char* token;             // 稳定标识（英文、全大写）
    InterlockSeverity severity;
    const char* why;               // 这条规则为什么存在（英文，日志/固件共用）
};

inline constexpr std::uint16_t kInterlockLinkUp = 0x0001U;
inline constexpr std::uint16_t kInterlockSessionReady = 0x0002U;
inline constexpr std::uint16_t kInterlockNoUnacknowledgedAlarm = 0x0003U;
inline constexpr std::uint16_t kInterlockNoFaultLatch = 0x0004U;
inline constexpr std::uint16_t kInterlockDeviceReady = 0x0005U;
inline constexpr std::uint16_t kInterlockModeSelected = 0x0006U;
inline constexpr std::uint16_t kInterlockTelemetryLive = 0x0007U;

// ---------------------------------------------------------------------------
// 现场快照
//
// 规则**只看这个结构**，不看设备、不看界面、不看串口。这是它唯一的输入，
// 于是整张表可以脱离一切外部条件被穷举验证 —— 而"启动之前到底要满足什么"
// 这种东西，恰恰必须能被穷举验证。
//
// 下面这些默认值的意思是"**什么都没有**"（没连上、没会话、没遥测、没选模式），
// 它们**不能**被当成"按默认值就算过了"：调用方必须按现场如实填一遍。
// 注意"什么都没有"并不等于"每条规则都不满足" ——
// "没有任何报警""没有故障锁存"本来就是好状态（见用例）。
// ---------------------------------------------------------------------------
struct InterlockSnapshot {
    bool connected = false;            // 传输开着（端口打开、链路对象在）
    bool linkDown = false;             // 链路监控判失效
    bool sessionReady = false;         // 会话建立完成（握手过、没失效）
    // 设备此刻的状态。存**状态本身**而不是一个 deviceReady 布尔量：
    // 规则判的是"等不等于 Ready"，而给人看的说明要说清"现在是 Running"——
    // 存成布尔量的话，那句话就写不出来了。
    DeviceState deviceState = DeviceState::Disconnected;
    bool faultLatched = false;         // 设备侧故障还锁存着
    std::uint8_t deviceMode = 0U;      // 设备当前在用的环（0 = 停止控制）
    std::uint64_t telemetryCount = 0U; // 本会话收到过多少帧遥测
    std::size_t unacknowledgedAlarms = 0U;   // 需要确认却还没确认的报警条数
};

struct InterlockResult {
    std::uint16_t code = 0U;
    InterlockSeverity severity = InterlockSeverity::Block;
    bool satisfied = false;
};

// 规则表本身（顺序固定：Block 在前，按重要性排）。
const std::vector<InterlockRule>& interlockRules() noexcept;
// 查一条规则。**未知编号返回 nullptr**，不给一条"兜底规则"蒙混过去。
const InterlockRule* interlockRule(std::uint16_t code) noexcept;

// 逐条判定，**不短路**：一次把没满足的都列出来，是人来来回回试好几趟的替代品。
std::vector<InterlockResult> evaluateInterlocks(const InterlockSnapshot& snapshot);

// 有没有 Block 级的不满足。
bool interlocksBlock(const std::vector<InterlockResult>& results) noexcept;
// 第一条拦下的（按表里的顺序，也就是重要性顺序）；没有就返回 nullptr。
const InterlockResult* firstBlockingInterlock(const std::vector<InterlockResult>& results) noexcept;

}  // namespace mc
