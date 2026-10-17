#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mc/device_state.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 报警分级与代码表（P0）
//
// 之前"报警"只是一条{时刻, 文本}的记录：没有代码、没有等级、没有确认状态。
// 那对事后追查是不够的 —— 文本是给人看的，而**处置**要按等级分：
// 什么可以无视、什么必须有人确认之后才允许再启动。
//
// 四级不是"报警/不报警"两级能替代的，因为四级的**处置动作完全不同**：
//   Info    —— 留个记录，不影响任何人干活（"人工清除了故障锁存"）。
//   Warning —— 记下来、提示一下；不要求确认、不拦启动（"心跳漏了一拍"）。
//   Error   —— 必须人工确认之后才允许再启动（"链路失效""命令超时"）。
//   Fatal   —— 同上，且现场通常还有物理后果要先查（设备侧的故障锁存）。
// 把 Warning 与 Error 合成一级，就等于要么让心跳漏拍也拦住启动、
// 要么让链路失效可以被无视 —— 两种都不能接受。
//
// 代码空间（16 位）分两段，段的边界本身就是一条约定：
//   0x0000          无报警
//   0x0001..0x000F  **设备侧**故障码，与 telemetry 的 fault_code 字节逐值对应。
//                   不另立一套编号：设备在遥测里只能发一个字节，那一份就是权威编号，
//                   上位机的代码表必须与它对齐（由编译期 static_assert 钉住）。
//   0x0100..        **上位机侧**观察到的事件。设备不知道这些事（它不知道
//                   "上位机连不上串口"），所以这一段只能由宿主侧产生。
// ---------------------------------------------------------------------------

enum class AlarmLevel : std::uint8_t {
    Info = 0U,
    Warning = 1U,
    Error = 2U,
    Fatal = 3U,
};

inline constexpr std::uint8_t kAlarmLevelCount = 4U;

const char* alarmLevelName(AlarmLevel level) noexcept;

// 代码表的一行。规格是**常量**；"这一次到底发生了什么"是另一回事（见 AlarmEntry）。
struct AlarmSpec {
    std::uint16_t code;
    // 稳定标识（英文、全大写）。它出现在日志、测试与将来的固件里，
    // 所以是接口的一部分，不能随手改 —— 给人看的措辞另有一处（宿主侧的 alarmText）。
    const char* token;
    AlarmLevel level;
    // 是否必须人工确认。**它不等同于"严重"**：这是一条处置要求。
    // Info/Warning 不要求确认，Error/Fatal 要求。
    bool requiresAck;
    // 条件消失之后，这一条是否继续留在清单上（直到被确认）。
    bool latched;
};

inline constexpr std::uint16_t kAlarmCodeNone = 0x0000U;

// 设备侧（与 FaultCode 同值 —— 见文件末的 static_assert）
inline constexpr std::uint16_t kAlarmCodeCommLoss = 0x0001U;
inline constexpr std::uint16_t kAlarmCodeOverSpeed = 0x0002U;
inline constexpr std::uint16_t kAlarmCodeOverLimit = 0x0003U;
inline constexpr std::uint16_t kAlarmCodeCrcBurst = 0x0004U;
inline constexpr std::uint16_t kAlarmCodeForced = 0x0005U;
inline constexpr std::uint16_t kAlarmCodeHomeFailed = 0x0006U;

// 上位机侧
inline constexpr std::uint16_t kAlarmCodeOpenFailed = 0x0101U;
inline constexpr std::uint16_t kAlarmCodeLinkLost = 0x0102U;
inline constexpr std::uint16_t kAlarmCodeSessionLost = 0x0103U;
inline constexpr std::uint16_t kAlarmCodeCommandTimeout = 0x0104U;
inline constexpr std::uint16_t kAlarmCodeHeartbeatDropped = 0x0105U;
inline constexpr std::uint16_t kAlarmCodeFaultCleared = 0x0106U;

// 查表。**未知代码返回 nullptr**，不给一条"兜底规格"蒙混过去：
// 界面遇到不认识的码必须显示"未知报警（代码 XXXX）"，而不是一条编出来的文本。
// 编出来的文本会让人以为设备报的是那个意思。
const AlarmSpec* alarmSpec(std::uint16_t code) noexcept;

// 设备侧 fault_code → 报警代码。两者同值；这个函数存在的意义是把
// "它们必须同值"这条约定写在一个地方，而不是散在各处的心算里。
std::uint16_t alarmCodeForFault(FaultCode code) noexcept;

static_assert(static_cast<std::uint16_t>(FaultCode::CommunicationLoss) == kAlarmCodeCommLoss &&
                  static_cast<std::uint16_t>(FaultCode::OverSpeed) == kAlarmCodeOverSpeed &&
                  static_cast<std::uint16_t>(FaultCode::OverLimit) == kAlarmCodeOverLimit &&
                  static_cast<std::uint16_t>(FaultCode::CrcBurst) == kAlarmCodeCrcBurst &&
                  static_cast<std::uint16_t>(FaultCode::Forced) == kAlarmCodeForced &&
                  static_cast<std::uint16_t>(FaultCode::HomeFailed) == kAlarmCodeHomeFailed,
              "设备侧故障码与上位机代码表的编号必须逐值相同：它们是同一个编号的两处引用");

// ---------------------------------------------------------------------------
// 运行时的报警条目：规格 + 这一次发生的现场信息
// ---------------------------------------------------------------------------
struct AlarmEntry {
    std::uint16_t code = kAlarmCodeNone;
    AlarmLevel level = AlarmLevel::Info;
    std::string detail;            // 现场补充（次数、端口名、后果），可为空
    std::uint64_t firstTMs = 0U;   // 第一次出现的**会话**时刻（与曲线同一口径）
    std::uint64_t lastTMs = 0U;    // 最近一次出现的会话时刻
    // 墙上时钟的"第一次 / 最近一次"。会话时刻是相对连接起点的（仿真里才有意义），
    // 而落库之后要回答的是"上周三几点出的这条"——只有墙上时钟答得了。
    std::uint64_t firstWallMs = 0U;
    std::uint64_t wallMs = 0U;
    std::uint32_t occurrences = 0U;
    bool active = true;            // 条件现在还在不在
    bool acknowledged = false;     // 人工确认过没有
    std::uint64_t ackWallMs = 0U;
};

// ---------------------------------------------------------------------------
// 报警簿：有界、可按等级过滤、能回答"现在能不能启动"
//
// 它住在核心库而不是界面里，理由与状态机相同：**"未确认的报警阻止启动"是一条
// 规则**，不是一种显示效果。规则写在控件里，就没法脱离界面穷举验证；
// 而且将来固件侧也要问同一个问题（"现在允许启动吗"）。
// ---------------------------------------------------------------------------
class AlarmLog {
public:
    static constexpr std::size_t kDefaultCapacity = 200U;

    explicit AlarmLog(std::size_t capacity = kDefaultCapacity) noexcept;

    // 记一条发生。**按代码对"当前还挂着"的同码条目去重**：
    // 同一个原因反复出现（例如 CRC 连错）只累加次数并推后时刻，不再刷一条新的 ——
    // 否则一次连错就能把整本清单挤空，而清单是有界的。
    // 重复出现会**把确认状态复位**：上一次的确认是对上一次发生的处置，
    // 新的这一次需要新的确认。
    //
    // 返回的引用只在**下一次 raise 之前**有效（容器可能重排或被挤掉）。
    AlarmEntry& raise(std::uint16_t code, std::string detail, std::uint64_t tMs,
                      std::uint64_t wallMs);

    // 条件消失。条目**不会**因此消失（要等确认），只是标成"已恢复"。
    // 返回是否找到了一条挂着的同码条目。
    bool clear(std::uint16_t code) noexcept;

    // 人工确认。**可以确认一条仍然挂着的报警**（它的意思是"我看见了"），
    // 这不代表物理原因已经排除 —— 那件事只有现场的人能判断。
    bool acknowledge(std::uint16_t code, std::uint64_t wallMs) noexcept;
    // 全部确认，返回被确认的条数（只确认还没确认过的）。
    std::size_t acknowledgeAll(std::uint64_t wallMs) noexcept;

    // 只清清单（把屏幕擦干净）。**不碰累计计数**，也不碰任何设备状态 ——
    // 清屏与"让设备重新允许启动"差着一个数量级，合成一个动作是安全设计上的错误。
    void clearList() noexcept;

    const std::vector<AlarmEntry>& entries() const noexcept { return entries_; }
    // 需要确认却还没确认的条数。
    std::size_t unacknowledgedRequiringAck() const noexcept;
    // 现在允许启动吗。这就是"启动门禁"的唯一判据来源（见宿主侧 sendStart 的说明）。
    bool blocksStart() const noexcept { return unacknowledgedRequiringAck() > 0U; }

    std::uint64_t raisedTotal() const noexcept { return raisedTotal_; }
    std::size_t capacity() const noexcept { return capacity_; }
    // 因容量上限被挤掉的条数。它是**看得见**的：藏起来等于让"清单里少了东西"
    // 变成一件查不出来的事。
    std::size_t discarded() const noexcept { return discarded_; }

private:
    // 满了要腾位置时，挑最旧的那一条**可以丢的**：只有"不需要确认，或已经确认过"
    // 的条目可以被挤掉。一条未确认的拦启动报警不能被静默丢弃 ——
    // 那会在没人察觉的情况下把启动闸门打开。
    std::size_t pickEvictionCandidate() const noexcept;

    std::vector<AlarmEntry> entries_;
    std::size_t capacity_;
    std::uint64_t raisedTotal_ = 0U;
    std::size_t discarded_ = 0U;
};

}  // namespace mc
