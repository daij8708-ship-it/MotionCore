#include "mc_host/alarm_text.hpp"

namespace mchost {

QString alarmText(std::uint16_t code) {
    switch (code) {
        // --- 设备侧故障（编号与遥测的 fault_code 相同）---
        case mc::kAlarmCodeCommLoss:
            return QStringLiteral("设备侧通信丢失：运行中太久没有收到任何合法帧，设备已自行撤目标");
        case mc::kAlarmCodeOverSpeed:
            return QStringLiteral("超速：转速超过允许值，设备已撤目标");
        case mc::kAlarmCodeOverLimit:
            return QStringLiteral("超出行程：位置越过软限位，设备已撤目标");
        case mc::kAlarmCodeCrcBurst:
            return QStringLiteral("校验连续出错：短时间内多帧 CRC 失败，通信质量不可信");
        case mc::kAlarmCodeForced:
            return QStringLiteral("故障注入：这一条是演示/测试注进去的，不是真实现象");
        case mc::kAlarmCodeHomeFailed:
            return QStringLiteral("回零失败：在给定时限内没走到零点，设备已进故障（零点**没有**"
                                  "建立，别拿现在的坐标当基准）");
        // --- 上位机侧 ---
        case mc::kAlarmCodeOpenFailed:
            return QStringLiteral("打不开设备端口：端口被占用、线没插、名字不对或权限不足");
        case mc::kAlarmCodeLinkLost:
            return QStringLiteral("链路失效：连续多拍没有收到应答，链路监控判定链路不可用");
        case mc::kAlarmCodeSessionLost:
            return QStringLiteral("会话作废：链路失效导致会话失效，正在重新握手");
        case mc::kAlarmCodeCommandTimeout:
            return QStringLiteral("命令超时未确认：已按超时结清（运动指令不会自动重发）");
        case mc::kAlarmCodeHeartbeatDropped:
            return QStringLiteral("心跳漏拍：丢了一拍心跳，链路还在，不构成故障");
        case mc::kAlarmCodeFaultCleared:
            return QStringLiteral("故障锁存已清除：人工确认 + 恢复校验通过");
        default:
            break;
    }
    return QStringLiteral("未知报警（代码 %1）：本版本的代码表里没有它，请先查清再处置")
        .arg(code, 4, 16, QLatin1Char('0'))
        .toUpper();
}

QString alarmLevelText(mc::AlarmLevel level) {
    switch (level) {
        case mc::AlarmLevel::Info:    return QStringLiteral("信息");
        case mc::AlarmLevel::Warning: return QStringLiteral("警告");
        case mc::AlarmLevel::Error:   return QStringLiteral("错误");
        case mc::AlarmLevel::Fatal:   return QStringLiteral("严重");
    }
    return QStringLiteral("未知");
}

QString alarmCodeText(std::uint16_t code) {
    // 只把数字部分大写，前缀原样 —— 理由见头文件。
    const QString digits =
        QString::number(static_cast<uint>(code), 16).toUpper().rightJustified(4, QLatin1Char('0'));
    return QStringLiteral("0x") + digits;
}

}  // namespace mchost
