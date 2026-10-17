#include "mc_host/log_text.hpp"

namespace mchost {

QString commandStatusText(CommandStatus status) {
    switch (status) {
        case CommandStatus::NotSent:
            return QStringLiteral("没发出去");
        case CommandStatus::Awaiting:
            return QStringLiteral("等待应答");
        case CommandStatus::Acked:
            return QStringLiteral("已确认");
        case CommandStatus::Nacked:
            return QStringLiteral("被拒绝");
        case CommandStatus::TimedOut:
            return QStringLiteral("超时判故障");
        case CommandStatus::Unidirectional:
            // STOP 是**单向下行**：协议上就没有应答，所以这不是"丢了"。
            // 措辞必须把它与"没收到答复"分开 —— 两者含义完全相反。
            return QStringLiteral("已发出（单向，无应答）");
    }
    return QStringLiteral("未知状态");
}

QString commandEventText(const CommandRecord& record) {
    QString line = QString::fromStdString(record.name);
    line += QStringLiteral(" #%1 ").arg(record.seq);
    line += commandStatusText(record.status);
    if (record.status == CommandStatus::Nacked) {
        line += QStringLiteral("：%1").arg(QString::fromUtf8(nackReasonName(record.nackReason)));
    }
    return line;
}

}  // namespace mchost
