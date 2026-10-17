#pragma once

#include <QString>

#include "mc/audit.hpp"

namespace mchost {

// 角色、动作、权限的中文措辞。
//
// 核心库只留稳定标识（进数据库、进日志、进测试的那一份），
// 给人看的措辞全在这里 —— 与报警、配方、程序、联锁是同一套做法：
// 换一种语言不需要碰判定逻辑，而判定逻辑也不需要知道界面上叫什么。
QString operatorRoleText(mc::OperatorRole role);
// 这个角色能干什么、不能干什么。设置页上要写出来：
// 只说"观察者"三个字，等于让人自己去猜它能不能按启动。
QString operatorRoleDutyText(mc::OperatorRole role);
QString operatorActionText(mc::OperatorAction action);
QString permissionText(mc::PermissionMask permission);

// 被拒的一句话：说清"现在是谁、这个动作要什么权限、去哪儿改"。
// 只说"没权限"，操作员下一步还是不知道该干什么。
QString permissionRefusalText(mc::OperatorRole role, mc::OperatorAction action);

// 一条流水的单行描述（状态栏、日志用）。表格另有自己的列，不用这个。
QString auditSentence(const mc::AuditEntry& entry);

}  // namespace mchost
