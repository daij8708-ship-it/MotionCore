#include "mc_host/audit_text.hpp"

namespace mchost {

QString operatorRoleText(mc::OperatorRole role) {
    switch (role) {
        case mc::OperatorRole::Viewer: return QStringLiteral("观察者");
        case mc::OperatorRole::Operator: return QStringLiteral("操作员");
        case mc::OperatorRole::Engineer: return QStringLiteral("工程师");
    }
    // 表里没有的角色**不编名字**：显示成"未知角色（2）"能让人立刻看出
    // 是代码和数据对不上，而不是以为真有一个叫这个名字的角色。
    return QStringLiteral("未知角色（%1）").arg(static_cast<int>(role));
}

QString operatorRoleDutyText(mc::OperatorRole role) {
    switch (role) {
        case mc::OperatorRole::Viewer:
            return QStringLiteral("只能看：曲线、报警、历史都能翻，"
                                  "但任何会产生后果的动作都发不出去 —— 投屏、培训、看数据用。");
        case mc::OperatorRole::Operator:
            return QStringLiteral("能开动设备：使能/停止、改模式与目标、下装已定稿的配方、"
                                  "跑程序、确认报警。**不能**改 PID、不能改配方与程序的内容、"
                                  "不能改连接参数，也不能清空报警清单。");
        case mc::OperatorRole::Engineer:
            return QStringLiteral("全部权限：在操作员的基础上，还能改 PID 与配方/程序的内容、"
                                  "配置数据源与串口、清故障锁存、清空报警清单、改本页的角色。");
    }
    return QStringLiteral("未知角色：它什么都做不了 —— 判不了的一律不放行。");
}

QString operatorActionText(mc::OperatorAction action) {
    switch (action) {
        case mc::OperatorAction::SetMode: return QStringLiteral("下发运行模式");
        case mc::OperatorAction::Start: return QStringLiteral("使能并启动");
        case mc::OperatorAction::Stop: return QStringLiteral("停止");
        case mc::OperatorAction::SetTarget: return QStringLiteral("下发目标值");
        case mc::OperatorAction::SetPid: return QStringLiteral("下发 PID 参数");
        case mc::OperatorAction::SetLimits: return QStringLiteral("下发软限位");
        case mc::OperatorAction::Home: return QStringLiteral("回零");
        case mc::OperatorAction::ClearFault: return QStringLiteral("清除故障锁存");
        case mc::OperatorAction::Connect: return QStringLiteral("连接设备");
        case mc::OperatorAction::Disconnect: return QStringLiteral("断开设备");
        case mc::OperatorAction::SetDataSource: return QStringLiteral("切换数据源");
        case mc::OperatorAction::ConfigureSerial: return QStringLiteral("配置串口");
        case mc::OperatorAction::AcknowledgeAlarm: return QStringLiteral("确认报警");
        case mc::OperatorAction::AcknowledgeAllAlarms: return QStringLiteral("确认全部报警");
        case mc::OperatorAction::ClearAlarmList: return QStringLiteral("清空报警清单");
        case mc::OperatorAction::DownloadRecipe: return QStringLiteral("下装配方");
        case mc::OperatorAction::SaveRecipe: return QStringLiteral("保存配方");
        case mc::OperatorAction::DeleteRecipe: return QStringLiteral("删除配方");
        case mc::OperatorAction::RunProgram: return QStringLiteral("执行程序");
        case mc::OperatorAction::AbortProgram: return QStringLiteral("中止程序");
        case mc::OperatorAction::SaveProgram: return QStringLiteral("保存程序");
        case mc::OperatorAction::DeleteProgram: return QStringLiteral("删除程序");
        case mc::OperatorAction::SetRole: return QStringLiteral("切换角色");
        case mc::OperatorAction::InjectFault: return QStringLiteral("注入故障");
        case mc::OperatorAction::ClearInjectedFaults:
            return QStringLiteral("清除注入的故障");
    }
    return QStringLiteral("未知动作（%1）").arg(static_cast<int>(action));
}

QString permissionText(mc::PermissionMask permission) {
    switch (permission) {
        case mc::kPermissionView: return QStringLiteral("查看");
        case mc::kPermissionControlMotion: return QStringLiteral("运动控制");
        case mc::kPermissionAcknowledgeAlarm: return QStringLiteral("确认报警");
        case mc::kPermissionClearAlarmList: return QStringLiteral("清空报警清单");
        case mc::kPermissionClearFault: return QStringLiteral("清除故障锁存");
        case mc::kPermissionDownloadRecipe: return QStringLiteral("下装配方");
        case mc::kPermissionEditRecipe: return QStringLiteral("编辑配方");
        case mc::kPermissionOperateProgram: return QStringLiteral("执行程序");
        case mc::kPermissionEditProgram: return QStringLiteral("编辑程序");
        case mc::kPermissionConfigureConnection: return QStringLiteral("配置连接");
        case mc::kPermissionManageRole: return QStringLiteral("管理角色");
        case mc::kPermissionEditDeviceConfig: return QStringLiteral("改设备参数");
        case mc::kPermissionInjectFault: return QStringLiteral("故障注入");
        default: break;
    }
    // 组合位或未知位：把核心库给的稳定标识原样列出来。
    // 这里**不**猜一个中文名 —— 猜出来的权限名会让人以为那是一项真实存在的权限。
    const char* token = mc::permissionToken(permission);
    if (token[0] == '\0') {
        return QStringLiteral("未知权限（0x%1）")
            .arg(static_cast<unsigned>(permission), 4, 16, QLatin1Char('0'));
    }
    return QString::fromLatin1(token);
}

QString permissionRefusalText(mc::OperatorRole role, mc::OperatorAction action) {
    const mc::PermissionMask required = mc::requiredPermission(action);
    const QString who = operatorRoleText(role);
    if (required == mc::kPermissionNone) {
        // 动作表里漏写了权限要求。这不是"用户没权限"，是**代码有问题**，
        // 所以要说清是哪一种，别让人去设置页里找一个不存在的开关。
        return QStringLiteral("「%1」这个动作没有声明它需要什么权限，"
                              "按规矩一律不放行 —— 这是软件的问题，不是权限的问题。")
            .arg(operatorActionText(action));
    }
    return QStringLiteral("当前角色是「%1」，%2 需要「%3」权限 —— 这次操作**没有下发到设备**。"
                          "要改角色请到「系统设置」页（改角色本身也会记进审计）。")
        .arg(who)
        .arg(operatorActionText(action))
        .arg(permissionText(required));
}

QString auditSentence(const mc::AuditEntry& entry) {
    QString text = QStringLiteral("%1｜%2｜%3")
                       .arg(operatorRoleText(entry.role))
                       .arg(operatorActionText(entry.action))
                       .arg(QString::fromStdString(entry.subject));
    if (entry.dispatched) {
        return text + QStringLiteral("：已执行");
    }
    // 没发生的理由**原样带上**：它才是这一行存在的意义。
    return text + QStringLiteral("：未执行（%1）").arg(QString::fromStdString(entry.reason));
}

}  // namespace mchost
