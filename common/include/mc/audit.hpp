#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace mc {

// ---------------------------------------------------------------------------
// 操作员角色、动作权限与审计流水（P1）
//
// 现场的问题不只是"能不能让设备动"，还有"**刚才是谁按的**"。只有一个面板加
// 一堆按钮的系统，事后追查只能靠人回忆；而"配方被谁改了""故障还没查清谁又启动了"
// 这些事，必须是答得出来的。
//
// 所以这一层只做两件事，而且都在核心库、都能脱离界面穷举验证：
//   ① 权限：每个**会产生后果的动作**都要一项权限，角色决定有没有那项权限；
//   ② 流水：每一次尝试都留一条记录 —— **包括被拒的**。被拒的那条信息量更大：
//      它回答的是"有人试过，但没成"，而这恰恰是排查误操作的起点。
//
// ⚠️ 这是**软件**权限。它挡的是误操作，挡不住人：能改数据库文件的人就能改流水。
//    真正的准入控制靠操作系统账户与物理钥匙，那些不在这里 —— 所以界面上
//    必须把这句话写出来，而不是做一个像保险柜的按钮。
// ---------------------------------------------------------------------------

// 角色分级的依据是"这个动作改的是什么"：
//   改**现场**（启停、目标、确认报警）：操作员就能做；
//   改**设备与程序的配置**（PID、配方、程序、连接参数）：工程师；
//   观察者什么都发不出去 —— 这正是它存在的理由：投屏、培训、看数据的人
//   不该因为手边有键盘就能让设备动起来。
enum class OperatorRole : std::uint8_t {
    Viewer = 0U,
    Operator = 1U,
    Engineer = 2U,
};

inline constexpr std::uint8_t kOperatorRoleCount = 3U;

// 权限是一组位。**用位而不是枚举**：一个角色拿到的是一组权限，
// 逐个枚举组合会写出 3! 种情况，而它们其实是"哪几位亮着"。
using PermissionMask = std::uint16_t;

inline constexpr PermissionMask kPermissionNone = 0x0000U;
inline constexpr PermissionMask kPermissionView = 0x0001U;
inline constexpr PermissionMask kPermissionControlMotion = 0x0002U;
inline constexpr PermissionMask kPermissionAcknowledgeAlarm = 0x0004U;
inline constexpr PermissionMask kPermissionClearAlarmList = 0x0008U;
inline constexpr PermissionMask kPermissionClearFault = 0x0010U;
inline constexpr PermissionMask kPermissionDownloadRecipe = 0x0020U;
inline constexpr PermissionMask kPermissionEditRecipe = 0x0040U;
inline constexpr PermissionMask kPermissionOperateProgram = 0x0080U;
inline constexpr PermissionMask kPermissionEditProgram = 0x0100U;
inline constexpr PermissionMask kPermissionConfigureConnection = 0x0200U;
inline constexpr PermissionMask kPermissionManageRole = 0x0400U;
inline constexpr PermissionMask kPermissionEditDeviceConfig = 0x0800U;
// 故障注入。它不是"改设备参数"：注入的是**上位机自己的链路**，
// 而且能当场把一条正在跑的程序打断（链路失效 → 会话作废 → 程序中止）。
// 所以它单列一项，而且只给工程师 —— 现场没人该在别人跑程序的时候顺手点一下。
inline constexpr PermissionMask kPermissionInjectFault = 0x1000U;

// 每一个**会产生后果**的动作。它是权限判定的入口：动作 → 需要的权限。
// 清单只收"会让设备、数据库或连接状态发生变化"的动作；
// 看一眼曲线、切一个页签不在里面 —— 那些不需要许可，也不进流水
// （进了只会把流水淹掉，而流水是要拿来查事的）。
enum class OperatorAction : std::uint16_t {
    SetMode = 0U,
    Start,
    Stop,
    SetTarget,
    SetPid,
    // 回零。它在权限上属于**运动控制**：与启停同一件事（电机会转）。
    Home,
    ClearFault,
    Connect,
    Disconnect,
    SetDataSource,
    ConfigureSerial,
    AcknowledgeAlarm,
    AcknowledgeAllAlarms,
    ClearAlarmList,
    DownloadRecipe,
    SaveRecipe,
    DeleteRecipe,
    RunProgram,
    AbortProgram,
    SaveProgram,
    DeleteProgram,
    SetRole,
    InjectFault,
    ClearInjectedFaults,
    // 软限位。权限上它与 SetPid 同族（都是"改设备侧配置"），但**审计动作必须分开**：
    // 流水要回答的是"刚才改的是什么"，而把一次限位改动记成"设置 PID"，
    // 事后查流水的人会去找一个根本没被碰过的参数。
    //
    // ⚠️ 它**刻意追加在末尾**，没有按语义插到 SetPid 后面：这个枚举的数值会写进
    // 审计表（action INTEGER），插在中间会让已经落盘的旧行被读成另一种动作。
    // 流水里"那件事到底是什么"不该随时间改变 —— 这与"窗口外销毁的计数仍要累计"
    // 是同一条原则。
    SetLimits,
};

inline constexpr std::uint16_t kOperatorActionCount =
    static_cast<std::uint16_t>(OperatorAction::SetLimits) + 1U;

// 稳定标识（英文、全大写）。它要进数据库、进日志、进测试，所以是接口的一部分，
// 不能随手改；给人看的中文措辞另有一处（宿主侧的 audit_text）。
const char* operatorRoleToken(OperatorRole role) noexcept;
const char* operatorActionToken(OperatorAction action) noexcept;
// 单项权限的标识。传进来的必须是**单个位**；组合位或未知位返回空串 ——
// 猜一个名字出来会让人以为那是一项真实存在的权限。
const char* permissionToken(PermissionMask permission) noexcept;

// 动作 → 需要的权限。**未知动作返回 kPermissionNone**：
// 一项判不了的权限要求不能让任何角色满足（包括工程师），
// 这与联锁表里"一条判不了的规则不能算通过"是同一条原则。
PermissionMask requiredPermission(OperatorAction action) noexcept;
// 角色 → 权限集合。未知角色返回 kPermissionNone（什么都不能做）。
PermissionMask rolePermissions(OperatorRole role) noexcept;
bool roleHasPermission(OperatorRole role, PermissionMask permission) noexcept;

// ---------------------------------------------------------------------------
// 审计流水
//
// 有界，但**累计计数不受上限影响**：窗口里只留最近 N 条，而"一共发生过多少次、
// 其中多少次被拒"是累计的。把计数也跟着窗口截断，等于让"次数"这件事
// 随着使用时间变短 —— 那正是审计里最不该出现的性质。
// ---------------------------------------------------------------------------
struct AuditEntry {
    std::uint64_t seq = 0U;        // 审计序号，从 1 开始单调递增
    std::uint64_t wallMs = 0U;     // 墙上时钟：事后追查看的是这个
    std::uint64_t elapsedMs = 0U;  // 上位机运行时钟（与曲线、会话同一口径）
    OperatorRole role = OperatorRole::Viewer;
    OperatorAction action = OperatorAction::SetMode;
    std::string subject;           // 动作的对象（"速度环(1)""配方 WAFER-A1 v1"）
    // 这一次**真的发生了没有**。false = 没发生，理由在 reason 里。
    // 刻意不叫 "allowed"："没发生"有三种——没权限、被本地条件拦下（没连上、
    // 联锁没满足）、以及本来就无事可做（"没有需要确认的报警"）。
    // 字段叫 allowed 的话，第三种就会被读成"被拒绝"，而它并不是。
    bool dispatched = false;
    std::string reason;            // 没发生的理由（发生了就为空）
};

class AuditLog {
public:
    static constexpr std::size_t kDefaultCapacity = 500U;

    explicit AuditLog(std::size_t capacity = kDefaultCapacity) noexcept;

    // 记一次尝试。返回审计序号。
    std::uint64_t record(std::uint64_t wallMs, std::uint64_t elapsedMs, OperatorRole role,
                         OperatorAction action, std::string subject, bool dispatched,
                         std::string reason = {});

    const std::deque<AuditEntry>& entries() const noexcept { return entries_; }
    // 最新一条（没有返回 nullptr）。界面与库里都按它对齐。
    const AuditEntry* latest() const noexcept;

    std::uint64_t recordedTotal() const noexcept { return nextSeq_ - 1U; }
    std::uint64_t dispatchedTotal() const noexcept { return dispatchedTotal_; }
    std::uint64_t notDispatchedTotal() const noexcept { return notDispatchedTotal_; }
    // 因容量上限被挤掉的条数。它必须看得见：藏起来等于让"流水里少了东西"
    // 变成一件查不出来的事。
    std::size_t discarded() const noexcept { return discarded_; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::deque<AuditEntry> entries_;
    std::size_t capacity_;
    std::uint64_t nextSeq_ = 1U;
    std::uint64_t dispatchedTotal_ = 0U;
    std::uint64_t notDispatchedTotal_ = 0U;
    std::size_t discarded_ = 0U;
};

}  // namespace mc
