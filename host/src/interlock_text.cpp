#include "mc_host/interlock_text.hpp"

#include "mc/device_state.hpp"
#include "mc_host/alarm_text.hpp"

namespace mchost {
namespace {

QString modeText(std::uint8_t mode) {
    switch (mode) {
        case 0U: return QStringLiteral("0 停止控制（没选环）");
        case 1U: return QStringLiteral("1 速度环");
        case 2U: return QStringLiteral("2 位置环");
        default: return QStringLiteral("%1（越界）").arg(static_cast<int>(mode));
    }
}

}  // namespace

QString interlockRuleText(std::uint16_t code) {
    switch (code) {
        case mc::kInterlockLinkUp:
            return QStringLiteral("链路可用");
        case mc::kInterlockSessionReady:
            return QStringLiteral("会话就绪");
        case mc::kInterlockNoUnacknowledgedAlarm:
            return QStringLiteral("没有未确认的报警");
        case mc::kInterlockNoFaultLatch:
            return QStringLiteral("没有故障锁存");
        case mc::kInterlockDeviceReady:
            return QStringLiteral("设备停在 Ready");
        case mc::kInterlockModeSelected:
            return QStringLiteral("已选控制模式");
        case mc::kInterlockTelemetryLive:
            return QStringLiteral("见过设备的数据");
    }
    return QStringLiteral("未知联锁（代码 %1）").arg(alarmCodeText(code));
}

QString interlockSeverityText(mc::InterlockSeverity severity) {
    switch (severity) {
        case mc::InterlockSeverity::Block: return QStringLiteral("拦启动");
        case mc::InterlockSeverity::Warn:  return QStringLiteral("仅提示");
    }
    return QStringLiteral("未知");
}

QString interlockSentence(const mc::InterlockResult& result, const mc::InterlockSnapshot& snapshot,
                          const QString& worstUnacknowledgedAlarm) {
    if (result.satisfied) {
        // 满足时也要给一句 —— 清单上有一个"通过"的勾，人才知道它被判过了，
        // 而不是这一行没被实现。
        switch (result.code) {
            case mc::kInterlockLinkUp:
                return QStringLiteral("传输已打开、链路监控没判失效");
            case mc::kInterlockSessionReady:
                return QStringLiteral("握手已完成，命令能到设备");
            case mc::kInterlockNoUnacknowledgedAlarm:
                return QStringLiteral("没有需要人工确认的报警");
            case mc::kInterlockNoFaultLatch:
                return QStringLiteral("设备侧没有挂着的故障锁存");
            case mc::kInterlockDeviceReady:
                return QStringLiteral("设备停在 Ready（就绪、输出未使能）");
            case mc::kInterlockModeSelected:
                return QStringLiteral("设备当前模式：%1").arg(modeText(snapshot.deviceMode));
            case mc::kInterlockTelemetryLive:
                return QStringLiteral("本会话已收到 %1 帧遥测")
                    .arg(static_cast<qulonglong>(snapshot.telemetryCount));
            default:
                return QStringLiteral("已满足");
        }
    }

    switch (result.code) {
        case mc::kInterlockLinkUp:
            return snapshot.connected ? QStringLiteral("链路监控已判失效：命令发不出去")
                                      : QStringLiteral("端口没打开：先连上设备");
        case mc::kInterlockSessionReady:
            return QStringLiteral("会话未就绪（握手未完成，或刚失效正在重连）："
                                  "此时发的命令到不了设备");
        case mc::kInterlockNoUnacknowledgedAlarm:
            return QStringLiteral("还有 %1 条报警没确认（最严重的一条：%2）—— "
                                  "先去「故障报警」页确认，再启动")
                .arg(static_cast<int>(snapshot.unacknowledgedAlarms))
                .arg(worstUnacknowledgedAlarm.isEmpty() ? QStringLiteral("未知")
                                                        : worstUnacknowledgedAlarm);
        case mc::kInterlockNoFaultLatch:
            return QStringLiteral("设备侧故障还锁存着：原因必须在现场查清并排除，"
                                  "确认自检通过才允许再启动");
        case mc::kInterlockDeviceReady:
            return QStringLiteral("设备当前是 %1，启动要求停在 Ready（就绪、输出未使能）：先停机")
                .arg(QString::fromUtf8(mc::deviceStateName(snapshot.deviceState)));
        case mc::kInterlockModeSelected:
            return QStringLiteral("还没选控制模式（当前 %1）—— "
                                  "没选环就使能在语义上不成立，设备会直接拒。先去「运动控制」页下发模式")
                .arg(modeText(snapshot.deviceMode));
        case mc::kInterlockTelemetryLive:
            return QStringLiteral("还没收到过这台设备的任何遥测：启动之前它还什么都没告诉你");
    }
    return QStringLiteral("这条规则的判定结果是未知的（代码 %1）").arg(alarmCodeText(result.code));
}

}  // namespace mchost
