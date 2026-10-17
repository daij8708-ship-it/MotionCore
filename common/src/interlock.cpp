#include "mc/interlock.hpp"

namespace mc {
namespace {

// 表的唯一真源。顺序就是**重要性顺序**（也是界面上的显示顺序）：
// 链路 → 会话 → 报警 → 故障 → 状态 → 模式 → （提示）。
// 一条都不许"顺手加进来"：每加一条都要能说清它在拦什么，
// 说不清的那条会把整张表变成没人信的仪式。
constexpr InterlockRule kRules[] = {
    {kInterlockLinkUp, "LINK_UP", InterlockSeverity::Block,
     "命令要发得出去：传输没开或链路已判失效时，任何启动都只是幻觉"},
    {kInterlockSessionReady, "SESSION_READY", InterlockSeverity::Block,
     "会话没就绪时命令到不了设备 —— 设备可能停在故障态而这里一无所知"},
    {kInterlockNoUnacknowledgedAlarm, "NO_UNACKNOWLEDGED_ALARM", InterlockSeverity::Block,
     "需要确认的报警还没被人工确认：跳过它就等于把「有人得看一眼」跳过去了"},
    {kInterlockNoFaultLatch, "NO_FAULT_LATCH", InterlockSeverity::Block,
     "设备侧故障锁存未解除：原因没查清之前不许再启动"},
    {kInterlockDeviceReady, "DEVICE_READY", InterlockSeverity::Block,
     "启动要求设备停在 Ready（就绪、输出未使能）—— 设备侧也会判，这里是给人看的说明"},
    {kInterlockModeSelected, "MODE_SELECTED", InterlockSeverity::Block,
     "没选控制环就使能在语义上不成立：设备会直接拒（清空目标）"},
    {kInterlockTelemetryLive, "TELEMETRY_LIVE", InterlockSeverity::Warn,
     "启动之前还没见过这台设备的任何数据 —— 提示一下，不拦"},
};

}  // namespace

const std::vector<InterlockRule>& interlockRules() noexcept {
    static const std::vector<InterlockRule> rules(std::begin(kRules), std::end(kRules));
    return rules;
}

const InterlockRule* interlockRule(std::uint16_t code) noexcept {
    for (const InterlockRule& rule : kRules) {
        if (rule.code == code) {
            return &rule;
        }
    }
    return nullptr;
}

std::vector<InterlockResult> evaluateInterlocks(const InterlockSnapshot& snapshot) {
    std::vector<InterlockResult> results;
    results.reserve(interlockRules().size());

    for (const InterlockRule& rule : interlockRules()) {
        InterlockResult result;
        result.code = rule.code;
        result.severity = rule.severity;

        switch (rule.code) {
            case kInterlockLinkUp:
                result.satisfied = snapshot.connected && !snapshot.linkDown;
                break;
            case kInterlockSessionReady:
                result.satisfied = snapshot.sessionReady;
                break;
            case kInterlockNoUnacknowledgedAlarm:
                result.satisfied = (snapshot.unacknowledgedAlarms == 0U);
                break;
            case kInterlockNoFaultLatch:
                result.satisfied = !snapshot.faultLatched;
                break;
            case kInterlockDeviceReady:
                result.satisfied = (snapshot.deviceState == DeviceState::Ready);
                break;
            case kInterlockModeSelected:
                result.satisfied = (snapshot.deviceMode != 0U);
                break;
            case kInterlockTelemetryLive:
                result.satisfied = (snapshot.telemetryCount > 0U);
                break;
            default:
                // 表里出现了这里没判过的代码 = 本模块有 bug。
                // 当成"不满足"是最安全的默认值：一条判不了的规则不能算通过。
                result.satisfied = false;
                break;
        }
        results.push_back(result);
    }
    return results;
}

bool interlocksBlock(const std::vector<InterlockResult>& results) noexcept {
    for (const InterlockResult& result : results) {
        if (result.severity == InterlockSeverity::Block && !result.satisfied) {
            return true;
        }
    }
    return false;
}

const InterlockResult* firstBlockingInterlock(const std::vector<InterlockResult>& results) noexcept {
    for (const InterlockResult& result : results) {
        if (result.severity == InterlockSeverity::Block && !result.satisfied) {
            return &result;
        }
    }
    return nullptr;
}

}  // namespace mc
