#include "mc/audit.hpp"

namespace mc {
namespace {

// 表的唯一真源。三张表（角色标识、动作标识、动作→权限）都写在这里，
// 顺序与枚举一致 —— 加一个动作时，编译器会用 -Werror=switch 逼着这里表态。
constexpr const char* kRoleTokens[kOperatorRoleCount] = {
    "VIEWER",
    "OPERATOR",
    "ENGINEER",
};

}  // namespace

const char* operatorRoleToken(OperatorRole role) noexcept {
    const auto index = static_cast<std::uint8_t>(role);
    if (index >= kOperatorRoleCount) {
        return "";
    }
    return kRoleTokens[index];
}

const char* operatorActionToken(OperatorAction action) noexcept {
    switch (action) {
        case OperatorAction::SetMode: return "SET_MODE";
        case OperatorAction::Start: return "START";
        case OperatorAction::Stop: return "STOP";
        case OperatorAction::SetTarget: return "SET_TARGET";
        case OperatorAction::SetPid: return "SET_PID";
        case OperatorAction::Home: return "HOME";
        case OperatorAction::ClearFault: return "CLEAR_FAULT";
        case OperatorAction::Connect: return "CONNECT";
        case OperatorAction::Disconnect: return "DISCONNECT";
        case OperatorAction::SetDataSource: return "SET_DATA_SOURCE";
        case OperatorAction::ConfigureSerial: return "CONFIGURE_SERIAL";
        case OperatorAction::AcknowledgeAlarm: return "ACK_ALARM";
        case OperatorAction::AcknowledgeAllAlarms: return "ACK_ALL_ALARMS";
        case OperatorAction::ClearAlarmList: return "CLEAR_ALARM_LIST";
        case OperatorAction::DownloadRecipe: return "DOWNLOAD_RECIPE";
        case OperatorAction::SaveRecipe: return "SAVE_RECIPE";
        case OperatorAction::DeleteRecipe: return "DELETE_RECIPE";
        case OperatorAction::RunProgram: return "RUN_PROGRAM";
        case OperatorAction::AbortProgram: return "ABORT_PROGRAM";
        case OperatorAction::SaveProgram: return "SAVE_PROGRAM";
        case OperatorAction::DeleteProgram: return "DELETE_PROGRAM";
        case OperatorAction::SetRole: return "SET_ROLE";
        case OperatorAction::InjectFault: return "INJECT_FAULT";
        case OperatorAction::ClearInjectedFaults: return "CLEAR_INJECTED_FAULTS";
        case OperatorAction::SetLimits: return "SET_LIMITS";
    }
    // 走到这里说明动作清单和这张表脱节了。给空串而不是编一个名字：
    // 编出来的标识会进库、被当成一条真实存在的动作引用。
    return "";
}

const char* permissionToken(PermissionMask permission) noexcept {
    switch (permission) {
        case kPermissionView: return "VIEW";
        case kPermissionControlMotion: return "CONTROL_MOTION";
        case kPermissionAcknowledgeAlarm: return "ACKNOWLEDGE_ALARM";
        case kPermissionClearAlarmList: return "CLEAR_ALARM_LIST";
        case kPermissionClearFault: return "CLEAR_FAULT";
        case kPermissionDownloadRecipe: return "DOWNLOAD_RECIPE";
        case kPermissionEditRecipe: return "EDIT_RECIPE";
        case kPermissionOperateProgram: return "OPERATE_PROGRAM";
        case kPermissionEditProgram: return "EDIT_PROGRAM";
        case kPermissionConfigureConnection: return "CONFIGURE_CONNECTION";
        case kPermissionManageRole: return "MANAGE_ROLE";
        case kPermissionEditDeviceConfig: return "EDIT_DEVICE_CONFIG";
        case kPermissionInjectFault: return "INJECT_FAULT";
        default: return "";   // 0（无权限）、组合位、未知位
    }
}

PermissionMask requiredPermission(OperatorAction action) noexcept {
    switch (action) {
        case OperatorAction::SetMode:
        case OperatorAction::Start:
        case OperatorAction::Stop:
        case OperatorAction::SetTarget:
        case OperatorAction::Home:
            return kPermissionControlMotion;
        case OperatorAction::SetPid:
            // 改增益是**改设备配置**：参数不对会直接把环路调振，不是操作员该拍的板。
            return kPermissionEditDeviceConfig;
        case OperatorAction::SetLimits:
            // 软限位是**设备侧的约束参数**，与增益同属"配置"而不是"操作"：
            // 它决定的是"哪里算越界"，不是"现在往哪走"。
            return kPermissionEditDeviceConfig;
        case OperatorAction::ClearFault:
            return kPermissionClearFault;
        case OperatorAction::Connect:
        case OperatorAction::Disconnect:
        case OperatorAction::SetDataSource:
        case OperatorAction::ConfigureSerial:
            return kPermissionConfigureConnection;
        case OperatorAction::AcknowledgeAlarm:
        case OperatorAction::AcknowledgeAllAlarms:
            return kPermissionAcknowledgeAlarm;
        case OperatorAction::ClearAlarmList:
            // 清清单会**抹掉证据**（未确认的报警从屏幕上消失），所以它不是
            // "确认一下"那个级别的事。
            return kPermissionClearAlarmList;
        case OperatorAction::DownloadRecipe:
            // 下装是把一份已经定稿的配方送到设备上跑 —— 生产动作，操作员即可。
            return kPermissionDownloadRecipe;
        case OperatorAction::SaveRecipe:
        case OperatorAction::DeleteRecipe:
            return kPermissionEditRecipe;
        case OperatorAction::RunProgram:
        case OperatorAction::AbortProgram:
            return kPermissionOperateProgram;
        case OperatorAction::SaveProgram:
        case OperatorAction::DeleteProgram:
            return kPermissionEditProgram;
        case OperatorAction::SetRole:
            return kPermissionManageRole;
        case OperatorAction::InjectFault:
        case OperatorAction::ClearInjectedFaults:
            return kPermissionInjectFault;
    }
    // 判不了的动作：不给任何角色放行。默认放行是这里最危险的一种写法，
    // 因为漏掉的分支恰恰是还没想清楚的那一个。
    return kPermissionNone;
}

PermissionMask rolePermissions(OperatorRole role) noexcept {
    switch (role) {
        case OperatorRole::Viewer:
            return kPermissionView;
        case OperatorRole::Operator:
            return kPermissionView | kPermissionControlMotion | kPermissionAcknowledgeAlarm |
                   kPermissionDownloadRecipe | kPermissionOperateProgram;
        case OperatorRole::Engineer:
            // 工程师**不是**"什么都有"：这里逐项列出来，就是为了让"到底有哪些权限"
            // 在源码里数得清。写成"非 0 即全部"的话，将来新增一项权限会自动
            // 落到工程师头上 —— 那不是决定，那是漏掉。
            return kPermissionView | kPermissionControlMotion | kPermissionAcknowledgeAlarm |
                   kPermissionClearAlarmList | kPermissionClearFault |
                   kPermissionDownloadRecipe | kPermissionEditRecipe |
                   kPermissionOperateProgram | kPermissionEditProgram |
                   kPermissionConfigureConnection | kPermissionManageRole |
                   kPermissionEditDeviceConfig | kPermissionInjectFault;
    }
    return kPermissionNone;
}

bool roleHasPermission(OperatorRole role, PermissionMask permission) noexcept {
    if (permission == kPermissionNone) {
        // 无权限要求 = 没人能过。这条只能走到"动作表里漏了、或权限位写错了"的情形，
        // 而那时**拒绝**才是安全的一侧。
        return false;
    }
    return (rolePermissions(role) & permission) == permission;
}

AuditLog::AuditLog(std::size_t capacity) noexcept : capacity_(capacity == 0U ? 1U : capacity) {}

std::uint64_t AuditLog::record(std::uint64_t wallMs, std::uint64_t elapsedMs, OperatorRole role,
                              OperatorAction action, std::string subject, bool dispatched,
                              std::string reason) {
    AuditEntry entry;
    entry.seq = nextSeq_++;
    entry.wallMs = wallMs;
    entry.elapsedMs = elapsedMs;
    entry.role = role;
    entry.action = action;
    entry.subject = std::move(subject);
    entry.dispatched = dispatched;
    entry.reason = std::move(reason);

    if (dispatched) {
        ++dispatchedTotal_;
    } else {
        ++notDispatchedTotal_;
    }

    entries_.push_back(std::move(entry));
    while (entries_.size() > capacity_) {
        entries_.pop_front();
        ++discarded_;
    }
    return entries_.back().seq;
}

const AuditEntry* AuditLog::latest() const noexcept {
    return entries_.empty() ? nullptr : &entries_.back();
}

}  // namespace mc
