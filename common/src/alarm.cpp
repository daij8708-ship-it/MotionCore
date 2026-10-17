#include "mc/alarm.hpp"

#include <utility>

namespace mc {
namespace {

// 代码表的唯一真源 —— 顺序无关紧要，查到即返回。
constexpr AlarmSpec kCatalog[] = {
    // --- 设备侧（编号与 telemetry 的 fault_code 相同）---
    // 这五条都是 Fatal + 需要确认 + 锁存：设备侧一旦置位，它自己就不会
    // 自动允许再启动（见 DeviceStateMachine 的锁存位），上位机这一侧必须同调。
    {kAlarmCodeCommLoss, "COMM_LOSS", AlarmLevel::Fatal, true, true},
    {kAlarmCodeOverSpeed, "OVER_SPEED", AlarmLevel::Fatal, true, true},
    {kAlarmCodeOverLimit, "OVER_LIMIT", AlarmLevel::Fatal, true, true},
    {kAlarmCodeCrcBurst, "CRC_BURST", AlarmLevel::Fatal, true, true},
    {kAlarmCodeForced, "FORCED", AlarmLevel::Fatal, true, true},
    {kAlarmCodeHomeFailed, "HOME_FAILED", AlarmLevel::Error, true, true},

    // --- 上位机侧 ---
    // 打不开端口：不是"设备出事了"，而是**这一侧根本没接上**。它够不上 Fatal
    // （设备那边一切正常），但必须是 Error：没确认之前不让启动是对的 ——
    // 一台连不上的设备上，任何"启动"都是幻觉。
    {kAlarmCodeOpenFailed, "OPEN_FAILED", AlarmLevel::Error, true, true},
    {kAlarmCodeLinkLost, "LINK_LOST", AlarmLevel::Error, true, true},
    {kAlarmCodeSessionLost, "SESSION_LOST", AlarmLevel::Error, true, true},
    {kAlarmCodeCommandTimeout, "COMMAND_TIMEOUT", AlarmLevel::Error, true, true},
    // 心跳漏拍：链路监控还没判失效，只是丢了一拍。要求确认就是不让人干活。
    {kAlarmCodeHeartbeatDropped, "HEARTBEAT_DROPPED", AlarmLevel::Warning, false, false},
    // 人工清除了故障锁存：一条**记录**，不要求任何处置。
    {kAlarmCodeFaultCleared, "FAULT_CLEARED", AlarmLevel::Info, false, false},
};

bool requiresAckFor(std::uint16_t code) noexcept {
    const AlarmSpec* spec = alarmSpec(code);
    // 不认识的代码按"需要确认"处理：对一条看不懂的报警，
    // "默认放行"是唯一不能选的那个默认值。
    return (spec == nullptr) ? true : spec->requiresAck;
}

}  // namespace

const char* alarmLevelName(AlarmLevel level) noexcept {
    switch (level) {
        case AlarmLevel::Info:    return "Info";
        case AlarmLevel::Warning: return "Warning";
        case AlarmLevel::Error:   return "Error";
        case AlarmLevel::Fatal:   return "Fatal";
    }
    return "?";
}

const AlarmSpec* alarmSpec(std::uint16_t code) noexcept {
    for (const AlarmSpec& spec : kCatalog) {
        if (spec.code == code) {
            return &spec;
        }
    }
    return nullptr;
}

std::uint16_t alarmCodeForFault(FaultCode code) noexcept {
    // 同值直通。这里刻意**不查表**：查表就会多出"表里没这一项怎么办"这个问题，
    // 而"两者同值"这条约定已经由头文件里的 static_assert 在编译期钉住了。
    return static_cast<std::uint16_t>(code);
}

AlarmLog::AlarmLog(std::size_t capacity) noexcept
    : capacity_(capacity == 0U ? kDefaultCapacity : capacity) {}

AlarmEntry& AlarmLog::raise(std::uint16_t code, std::string detail, std::uint64_t tMs,
                            std::uint64_t wallMs) {
    ++raisedTotal_;

    for (AlarmEntry& entry : entries_) {
        if (entry.code == code && entry.active) {
            entry.lastTMs = tMs;
            entry.wallMs = wallMs;
            ++entry.occurrences;
            if (!detail.empty()) {
                entry.detail = std::move(detail);
            }
            // 新的这一次需要新的确认：上一次的确认是对**上一次发生**的处置。
            entry.acknowledged = false;
            entry.ackWallMs = 0U;
            return entry;
        }
    }

    if (entries_.size() >= capacity_) {
        const std::size_t victim = pickEvictionCandidate();
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(victim));
        ++discarded_;
    }

    AlarmEntry fresh;
    fresh.code = code;
    const AlarmSpec* spec = alarmSpec(code);
    // 不认识的代码按最严重的等级记 —— 界面会把它显示成"未知报警（代码 XXXX）"，
    // 而"悄悄当成一条无关紧要的记录"会让一个真问题从清单上消失。
    fresh.level = (spec != nullptr) ? spec->level : AlarmLevel::Fatal;
    fresh.detail = std::move(detail);
    fresh.firstTMs = tMs;
    fresh.lastTMs = tMs;
    fresh.firstWallMs = wallMs;
    fresh.wallMs = wallMs;
    fresh.occurrences = 1U;
    entries_.push_back(std::move(fresh));
    return entries_.back();
}

bool AlarmLog::clear(std::uint16_t code) noexcept {
    // 从后往前找：同一个码可能有多条（一条已恢复、另一条后来又发生），
    // 要标成"已恢复"的是**当前挂着**的那一条。
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->code == code && it->active) {
            it->active = false;
            return true;
        }
    }
    return false;
}

bool AlarmLog::acknowledge(std::uint16_t code, std::uint64_t wallMs) noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->code == code && !it->acknowledged) {
            it->acknowledged = true;
            it->ackWallMs = wallMs;
            return true;
        }
    }
    return false;
}

std::size_t AlarmLog::acknowledgeAll(std::uint64_t wallMs) noexcept {
    std::size_t count = 0U;
    for (AlarmEntry& entry : entries_) {
        if (!entry.acknowledged) {
            entry.acknowledged = true;
            entry.ackWallMs = wallMs;
            ++count;
        }
    }
    return count;
}

void AlarmLog::clearList() noexcept {
    // 只清元素。raisedTotal_ 与 discarded_ **刻意留着**：点一下"清屏"就把
    // "一共发生过多少次""有没有被挤掉过"一起抹掉，等于自己把证据删了。
    entries_.clear();
}

std::size_t AlarmLog::unacknowledgedRequiringAck() const noexcept {
    std::size_t count = 0U;
    for (const AlarmEntry& entry : entries_) {
        if (!entry.acknowledged && requiresAckFor(entry.code)) {
            ++count;
        }
    }
    return count;
}

std::size_t AlarmLog::pickEvictionCandidate() const noexcept {
    // ① 先丢已经确认过的最旧一条；② 再丢不需要确认的最旧一条。
    for (std::size_t i = 0U; i < entries_.size(); ++i) {
        if (entries_[i].acknowledged) {
            return i;
        }
    }
    for (std::size_t i = 0U; i < entries_.size(); ++i) {
        if (!requiresAckFor(entries_[i].code)) {
            return i;
        }
    }
    // ③ 走到这里说明整本清单都是"需要确认且没确认"的条目。实际上不会发生 ——
    //    同码的挂起条目在 raise 里就被去重了，所以这类条目数被代码表的大小框住。
    //    但内存必须有界，所以还是丢最旧的一条，并且它会计入 discarded() 被看见。
    return 0U;
}

}  // namespace mc
