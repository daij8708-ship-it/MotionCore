#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "mc/audit.hpp"

// 角色、权限与审计流水的用例。
//
// 这里的判据一律是**穷举的**：权限表是一张表，它的正确性不该靠"我记得给过"。
// 所以逐动作、逐角色地断言，而不是挑几个代表。表错了要在这里红，
// 而不是在现场"某个按钮点了没反应"。
namespace {

using mc::AuditLog;
using mc::OperatorAction;
using mc::OperatorRole;

// 动作清单里的每一个动作，各出现一次。加动作时这里会漏 —— 由 kOperatorActionCount
// 与这段长度的一致性断言钉住（见 ForEveryActionTheTableSaysSomething）。
constexpr OperatorAction kAllActions[] = {
    OperatorAction::SetMode,           OperatorAction::Start,
    OperatorAction::Stop,              OperatorAction::SetTarget,
    OperatorAction::SetPid,            OperatorAction::Home,
    OperatorAction::ClearFault,
    OperatorAction::Connect,           OperatorAction::Disconnect,
    OperatorAction::SetDataSource,     OperatorAction::ConfigureSerial,
    OperatorAction::AcknowledgeAlarm,  OperatorAction::AcknowledgeAllAlarms,
    OperatorAction::ClearAlarmList,    OperatorAction::DownloadRecipe,
    OperatorAction::SaveRecipe,        OperatorAction::DeleteRecipe,
    OperatorAction::RunProgram,        OperatorAction::AbortProgram,
    OperatorAction::SaveProgram,       OperatorAction::DeleteProgram,
    OperatorAction::SetRole,            OperatorAction::InjectFault,
    OperatorAction::ClearInjectedFaults, OperatorAction::SetLimits,
};

}  // namespace

TEST(AuditTable, TheActionListInThisTestCoversEveryAction) {
    // 这条用例的唯一目的：让"清单变了但测试没跟上"当场红。
    static_assert(sizeof(kAllActions) / sizeof(kAllActions[0]) == mc::kOperatorActionCount,
                  "动作清单与用例里的枚举不同步：加了动作就要在这里补上它");
    EXPECT_EQ(sizeof(kAllActions) / sizeof(kAllActions[0]), mc::kOperatorActionCount);
}

TEST(AuditTable, EveryActionHasATokenAndARequiredPermission) {
    for (OperatorAction action : kAllActions) {
        EXPECT_STRNE(mc::operatorActionToken(action), "")
            << "动作 " << static_cast<int>(action) << " 没有稳定标识";
        // 权限必须是**单项位**：多个位 OR 在一起的话，"需要哪些权限"就说不清了，
        // 而且 roleHasPermission 的"全都得有"语义会让判定悄悄变严或变松。
        const mc::PermissionMask required = mc::requiredPermission(action);
        EXPECT_NE(required, mc::kPermissionNone)
            << "动作 " << mc::operatorActionToken(action) << " 不需要任何权限 = 漏写了";
        EXPECT_EQ(required & (required - 1U), 0U)
            << "动作 " << mc::operatorActionToken(action) << " 需要的权限不是单项位";
        EXPECT_STRNE(mc::permissionToken(required), "");
    }
}

TEST(AuditTable, EveryPermissionIsGrantedToSomeRole) {
    // 一项谁都拿不到的权限 = 一个永远按不动、且理由说不通的按钮。
    // 反过来说，每一项权限都得有它的用处。
    for (OperatorAction action : kAllActions) {
        const mc::PermissionMask required = mc::requiredPermission(action);
        bool granted = false;
        for (std::uint8_t i = 0U; i < mc::kOperatorRoleCount; ++i) {
            if (mc::roleHasPermission(static_cast<OperatorRole>(i), required)) {
                granted = true;
            }
        }
        EXPECT_TRUE(granted) << "没有任何角色拿得到 " << mc::permissionToken(required)
                             << "（动作 " << mc::operatorActionToken(action) << "）";
    }
}

TEST(AuditTable, TheViewerCanDoNothingThatHasConsequences) {
    // 观察者是**只读**的：这是它存在的全部理由。
    for (OperatorAction action : kAllActions) {
        EXPECT_FALSE(mc::roleHasPermission(OperatorRole::Viewer, mc::requiredPermission(action)))
            << "观察者不该能做 " << mc::operatorActionToken(action);
    }
    EXPECT_TRUE(mc::roleHasPermission(OperatorRole::Viewer, mc::kPermissionView));
}

TEST(AuditTable, TheOperatorRunsTheMachineButDoesNotChangeHowItRuns) {
    // 操作员：现场动作（启停、目标、模式、下装配方、跑程序、确认报警）可以做；
    // 改定义（PID、配方内容、程序内容、连接参数、角色）不行。
    const OperatorAction allowed[] = {
        OperatorAction::SetMode,   OperatorAction::Start,
        OperatorAction::Stop,      OperatorAction::SetTarget,
        OperatorAction::Home,      OperatorAction::DownloadRecipe,
        OperatorAction::RunProgram,
        OperatorAction::AbortProgram, OperatorAction::AcknowledgeAlarm,
        OperatorAction::AcknowledgeAllAlarms,
    };
    const OperatorAction refused[] = {
        OperatorAction::SetPid,      OperatorAction::SetLimits,
        OperatorAction::ClearFault,
        OperatorAction::SaveRecipe,  OperatorAction::DeleteRecipe,
        OperatorAction::SaveProgram, OperatorAction::DeleteProgram,
        OperatorAction::Connect,     OperatorAction::Disconnect,
        OperatorAction::SetDataSource, OperatorAction::ConfigureSerial,
        OperatorAction::ClearAlarmList, OperatorAction::SetRole,
        OperatorAction::InjectFault, OperatorAction::ClearInjectedFaults,
    };
    for (OperatorAction action : allowed) {
        EXPECT_TRUE(mc::roleHasPermission(OperatorRole::Operator, mc::requiredPermission(action)))
            << "操作员应当能做 " << mc::operatorActionToken(action);
    }
    for (OperatorAction action : refused) {
        EXPECT_FALSE(mc::roleHasPermission(OperatorRole::Operator, mc::requiredPermission(action)))
            << "操作员不该能做 " << mc::operatorActionToken(action);
    }
}

TEST(AuditTable, TheEngineerCanDoEverythingInTheTable) {
    for (OperatorAction action : kAllActions) {
        EXPECT_TRUE(mc::roleHasPermission(OperatorRole::Engineer, mc::requiredPermission(action)))
            << "工程师应当能做 " << mc::operatorActionToken(action);
    }
}

TEST(AuditTable, AnUnknownRoleOrPermissionIsRefusedForEveryone) {
    // 判不了的一律不放行 —— 与联锁表里"一条判不了的规则不能算通过"同一条原则。
    const auto unknownRole = static_cast<OperatorRole>(99U);
    EXPECT_FALSE(mc::roleHasPermission(unknownRole, mc::kPermissionView));
    EXPECT_EQ(mc::rolePermissions(unknownRole), mc::kPermissionNone);
    EXPECT_FALSE(mc::roleHasPermission(OperatorRole::Engineer, mc::kPermissionNone));
    EXPECT_FALSE(mc::roleHasPermission(OperatorRole::Engineer, mc::PermissionMask{0x8000U}))
        << "高位权限（还没定义的那一位）不该被当成'工程师天然拥有'";
    EXPECT_STREQ(mc::permissionToken(mc::PermissionMask{0x8000U}), "") << "未知权限不编名字";
    EXPECT_STREQ(mc::operatorRoleToken(unknownRole), "");
}

TEST(AuditLog, EveryAttemptGetsASeqAndBothOutcomesAreCounted) {
    AuditLog log;
    const std::uint64_t first = log.record(1000U, 10U, OperatorRole::Operator,
                                           OperatorAction::Start, "启动", true);
    const std::uint64_t second = log.record(2000U, 20U, OperatorRole::Viewer,
                                            OperatorAction::Start, "启动", false, "看不了");
    EXPECT_EQ(first, 1U);
    EXPECT_EQ(second, 2U);
    ASSERT_EQ(log.entries().size(), 2U);
    EXPECT_EQ(log.recordedTotal(), 2U);
    EXPECT_EQ(log.dispatchedTotal(), 1U);
    EXPECT_EQ(log.notDispatchedTotal(), 1U);
    // **没发生的也要留**：它回答的是"有人试过但没成"，这正是排查误操作的起点。
    EXPECT_FALSE(log.entries().back().dispatched);
    EXPECT_EQ(log.entries().back().reason, "看不了");
    EXPECT_EQ(log.entries().back().role, OperatorRole::Viewer);
    ASSERT_NE(log.latest(), nullptr);
    EXPECT_EQ(log.latest()->seq, 2U);
    EXPECT_EQ(log.latest()->wallMs, 2000U);
    EXPECT_EQ(log.latest()->elapsedMs, 20U);
}

TEST(AuditLog, TheWindowIsBoundedButTheCountersAreNot) {
    // 上限管的是"留多少条"，不是"记得住多少件事"。
    // 计数跟着窗口截断的话，"发生过多少次"会随着使用时间越变越小 ——
    // 那正是审计里最不该出现的性质。
    AuditLog log(3U);
    for (std::uint64_t i = 0U; i < 10U; ++i) {
        log.record(1000U + i, i, OperatorRole::Engineer, OperatorAction::Start, "x",
                   (i % 2U) == 0U, (i % 2U) == 0U ? "" : "被拒");
    }
    EXPECT_EQ(log.entries().size(), 3U);
    EXPECT_EQ(log.discarded(), 7U);
    EXPECT_EQ(log.recordedTotal(), 10U);
    EXPECT_EQ(log.dispatchedTotal(), 5U);
    EXPECT_EQ(log.notDispatchedTotal(), 5U);
    // 窗口里留的是**最近**的三条。
    EXPECT_EQ(log.entries().front().seq, 8U);
    EXPECT_EQ(log.entries().back().seq, 10U);
    EXPECT_EQ(log.capacity(), 3U);
}

TEST(AuditLog, AZeroCapacityStillKeepsTheLatestEntry) {
    // 容量 0 是"写错了"的样子。让它**保留一条**而不是丢光：
    // 一条都留不住的流水，等于把"刚才谁按了什么"变成查不到的事。
    AuditLog log(0U);
    log.record(1U, 1U, OperatorRole::Engineer, OperatorAction::Start, "x", true);
    EXPECT_EQ(log.entries().size(), 1U);
    EXPECT_EQ(log.capacity(), 1U);
    EXPECT_EQ(log.discarded(), 0U);
    log.record(2U, 2U, OperatorRole::Engineer, OperatorAction::Stop, "x", true);
    EXPECT_EQ(log.entries().size(), 1U);
    EXPECT_EQ(log.discarded(), 1U);
    ASSERT_NE(log.latest(), nullptr);
    EXPECT_EQ(log.latest()->action, OperatorAction::Stop);
}

TEST(AuditLog, AnEmptyLogHasNoLatestEntry) {
    AuditLog log;
    EXPECT_EQ(log.latest(), nullptr);
    EXPECT_EQ(log.recordedTotal(), 0U);
}
