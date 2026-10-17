#include "mc/alarm.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// 报警分级与代码表的用例。
//
// 重点不在"能记一条报警"—— 那个写错了立刻就能看见。重点是四条**规则**：
//   ① 同一原因反复出现不刷屏，但**要重新要求确认**；
//   ② 条件消失不等于条目消失（锁存的报警要留到被确认）；
//   ③ 未确认的报警**拦住启动**；不需要确认的等级不拦；
//   ④ 清单满了不许把"未确认的拦启动报警"静默挤掉。
namespace {

constexpr std::uint16_t kUnknownCode = 0x4321U;

const std::vector<std::uint16_t>& allKnownCodes() {
    static const std::vector<std::uint16_t> codes = {
        mc::kAlarmCodeCommLoss,       mc::kAlarmCodeOverSpeed,  mc::kAlarmCodeOverLimit,
        mc::kAlarmCodeCrcBurst,       mc::kAlarmCodeForced,     mc::kAlarmCodeOpenFailed,
        mc::kAlarmCodeLinkLost,       mc::kAlarmCodeSessionLost, mc::kAlarmCodeCommandTimeout,
        mc::kAlarmCodeHeartbeatDropped, mc::kAlarmCodeFaultCleared,
    };
    return codes;
}

}  // namespace

// ---------------------------------------------------------------------------
// 代码表
// ---------------------------------------------------------------------------

TEST(AlarmCatalog, EveryKnownCodeHasASpecWithAStableToken) {
    for (const std::uint16_t code : allKnownCodes()) {
        const mc::AlarmSpec* spec = mc::alarmSpec(code);
        ASSERT_NE(spec, nullptr) << "代码 " << code << " 在表里找不到";
        EXPECT_EQ(spec->code, code);
        ASSERT_NE(spec->token, nullptr);
        EXPECT_STRNE(spec->token, "");
        EXPECT_STRNE(spec->token, "?");
    }
}

TEST(AlarmCatalog, NothingButNoneIsOutsideTheTable) {
    // 0 是"无报警"，不是一条报警：它没有规格。
    EXPECT_EQ(mc::alarmSpec(mc::kAlarmCodeNone), nullptr);
    EXPECT_EQ(mc::alarmSpec(kUnknownCode), nullptr);
}

TEST(AlarmCatalog, LevelsAndAckRequirementsFollowTheDisposalRule) {
    // 这一张就是"等级决定处置"的规格表本身。它被写死在用例里是**故意的**：
    // 改了表就必须来改这里，从而必须想一遍"这个等级该不该拦启动"。
    const auto check = [](std::uint16_t code, mc::AlarmLevel level, bool requiresAck, bool latched) {
        const mc::AlarmSpec* spec = mc::alarmSpec(code);
        ASSERT_NE(spec, nullptr);
        EXPECT_EQ(spec->level, level) << "代码 " << code;
        EXPECT_EQ(spec->requiresAck, requiresAck) << "代码 " << code;
        EXPECT_EQ(spec->latched, latched) << "代码 " << code;
    };

    // 设备侧故障：最严重、要确认、要锁存。
    check(mc::kAlarmCodeCommLoss, mc::AlarmLevel::Fatal, true, true);
    check(mc::kAlarmCodeOverSpeed, mc::AlarmLevel::Fatal, true, true);
    check(mc::kAlarmCodeOverLimit, mc::AlarmLevel::Fatal, true, true);
    check(mc::kAlarmCodeCrcBurst, mc::AlarmLevel::Fatal, true, true);
    check(mc::kAlarmCodeForced, mc::AlarmLevel::Fatal, true, true);

    // 上位机侧：接不上/链路断了/命令没回音 —— 都是 Error（拦启动，但不是设备出事）。
    check(mc::kAlarmCodeOpenFailed, mc::AlarmLevel::Error, true, true);
    check(mc::kAlarmCodeLinkLost, mc::AlarmLevel::Error, true, true);
    check(mc::kAlarmCodeSessionLost, mc::AlarmLevel::Error, true, true);
    check(mc::kAlarmCodeCommandTimeout, mc::AlarmLevel::Error, true, true);

    // 心跳漏一拍只是提示，不该拦住任何人干活。
    check(mc::kAlarmCodeHeartbeatDropped, mc::AlarmLevel::Warning, false, false);
    // 人工清锁存是一条记录。
    check(mc::kAlarmCodeFaultCleared, mc::AlarmLevel::Info, false, false);

    // 反向对照：两级"不拦启动"的等级必须真的不需要确认 —— 否则上面两条检查
    // 只证明了"表里写了 false"，没证明"这个等级的性质如此"。
    EXPECT_FALSE(mc::alarmSpec(mc::kAlarmCodeHeartbeatDropped)->requiresAck);
    EXPECT_FALSE(mc::alarmSpec(mc::kAlarmCodeFaultCleared)->requiresAck);
}

TEST(AlarmCatalog, DeviceFaultCodesAreTheSameNumbersAsTheWireField) {
    // 设备在遥测里只能发一个字节的 fault_code，所以那一份编号就是权威编号；
    // 上位机的代码表必须与它逐值对齐（另有编译期 static_assert 兜同一件事）。
    const std::pair<mc::FaultCode, std::uint16_t> pairs[] = {
        {mc::FaultCode::CommunicationLoss, mc::kAlarmCodeCommLoss},
        {mc::FaultCode::OverSpeed, mc::kAlarmCodeOverSpeed},
        {mc::FaultCode::OverLimit, mc::kAlarmCodeOverLimit},
        {mc::FaultCode::CrcBurst, mc::kAlarmCodeCrcBurst},
        {mc::FaultCode::Forced, mc::kAlarmCodeForced},
    };
    for (const auto& pair : pairs) {
        EXPECT_EQ(mc::alarmCodeForFault(pair.first), pair.second);
        EXPECT_EQ(static_cast<std::uint16_t>(pair.first), pair.second);
        EXPECT_NE(mc::alarmSpec(pair.second), nullptr);
    }
    EXPECT_EQ(mc::alarmCodeForFault(mc::FaultCode::None), mc::kAlarmCodeNone);
}

TEST(AlarmCatalog, EveryLevelHasAName) {
    for (int i = 0; i < static_cast<int>(mc::kAlarmLevelCount); ++i) {
        const char* name = mc::alarmLevelName(static_cast<mc::AlarmLevel>(i));
        ASSERT_NE(name, nullptr);
        EXPECT_STRNE(name, "?");
    }
}

// ---------------------------------------------------------------------------
// 报警簿
// ---------------------------------------------------------------------------

TEST(AlarmLog, RepeatingTheSameCauseUpdatesOneEntryInsteadOfFlooding) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeCrcBurst, "第 1 次", 100U, 1000U);
    log.raise(mc::kAlarmCodeCrcBurst, "第 2 次", 200U, 2000U);
    log.raise(mc::kAlarmCodeCrcBurst, "第 3 次", 300U, 3000U);

    ASSERT_EQ(log.entries().size(), 1U);
    const mc::AlarmEntry& entry = log.entries()[0];
    EXPECT_EQ(entry.occurrences, 3U);
    EXPECT_EQ(entry.firstTMs, 100U);
    EXPECT_EQ(entry.lastTMs, 300U);
    // 墙上时钟的"第一次"不被后来的更新推后：它是这起事件**开始**的时间，
    // 落库之后要回答的是"这条是什么时候开始出的"，不是"最后一次是什么时候"。
    EXPECT_EQ(entry.firstWallMs, 1000U);
    EXPECT_EQ(entry.wallMs, 3000U);
    EXPECT_EQ(entry.detail, "第 3 次");
    // 发生次数照实累计：去重是"不刷屏"，不是"不计数"。
    EXPECT_EQ(log.raisedTotal(), 3U);
}

TEST(AlarmLog, ADifferentCodeGetsItsOwnEntry) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeCrcBurst, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    EXPECT_EQ(log.entries().size(), 2U);
    EXPECT_EQ(log.raisedTotal(), 2U);
}

TEST(AlarmLog, AReoccurrenceRequiresAFreshAcknowledgement) {
    // 上一次的确认是对**上一次发生**的处置。同一个原因又发生了一次，
    // 却因为"之前确认过"而被当成已处置 —— 那是最容易出事的一种混淆。
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    ASSERT_TRUE(log.blocksStart());
    ASSERT_TRUE(log.acknowledge(mc::kAlarmCodeLinkLost, 1100U));
    EXPECT_FALSE(log.blocksStart());

    log.raise(mc::kAlarmCodeLinkLost, "又断了", 200U, 2000U);
    EXPECT_TRUE(log.blocksStart()) << "重新发生之后必须重新要求确认";
    EXPECT_FALSE(log.entries()[0].acknowledged);
}

TEST(AlarmLog, RecoveringFromTheConditionDoesNotRemoveTheEntry) {
    // 锁存的报警要留到被确认。条件消失只是标一下"已恢复"。
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    ASSERT_TRUE(log.clear(mc::kAlarmCodeLinkLost));

    ASSERT_EQ(log.entries().size(), 1U);
    EXPECT_FALSE(log.entries()[0].active);
    // 已恢复但没确认 —— 仍然拦启动。
    EXPECT_TRUE(log.blocksStart());
    // 反向对照：同一条再 clear 一次没有东西可清了。
    EXPECT_FALSE(log.clear(mc::kAlarmCodeLinkLost));
}

TEST(AlarmLog, WarningAndInfoNeverBlockStart) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeHeartbeatDropped, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeFaultCleared, "", 100U, 1000U);

    EXPECT_EQ(log.entries().size(), 2U);
    EXPECT_EQ(log.unacknowledgedRequiringAck(), 0U);
    EXPECT_FALSE(log.blocksStart());
}

TEST(AlarmLog, AnEntryIsOnlyCountedAsBlockingWhenItsSpecRequiresAck) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeCommandTimeout, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeHeartbeatDropped, "", 100U, 1000U);

    EXPECT_EQ(log.unacknowledgedRequiringAck(), 1U);
    EXPECT_TRUE(log.blocksStart());

    EXPECT_TRUE(log.acknowledge(mc::kAlarmCodeCommandTimeout, 1200U));
    EXPECT_EQ(log.unacknowledgedRequiringAck(), 0U);
    EXPECT_FALSE(log.blocksStart());
}

TEST(AlarmLog, AnUnknownCodeIsTreatedAsTheWorstCase) {
    // 看不懂的报警默认放行是唯一不能选的默认值。
    mc::AlarmLog log{8U};
    const mc::AlarmEntry& entry = log.raise(kUnknownCode, "固件版本不匹配？", 100U, 1000U);

    EXPECT_EQ(entry.level, mc::AlarmLevel::Fatal);
    EXPECT_TRUE(log.blocksStart());
    EXPECT_EQ(mc::alarmSpec(entry.code), nullptr) << "界面上必须能认出这是未知代码";
}

TEST(AlarmLog, AcknowledgeAllCountsOnlyWhatWasStillPending) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeSessionLost, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeHeartbeatDropped, "", 100U, 1000U);
    ASSERT_TRUE(log.acknowledge(mc::kAlarmCodeLinkLost, 1100U));

    EXPECT_EQ(log.acknowledgeAll(1200U), 2U);
    EXPECT_FALSE(log.blocksStart());
    // 再确认一次没有东西可确认了 —— 不能报成"又确认了两条"。
    EXPECT_EQ(log.acknowledgeAll(1300U), 0U);
}

TEST(AlarmLog, ClearingTheListTouchesNothingButTheList) {
    mc::AlarmLog log{8U};
    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    log.raise(mc::kAlarmCodeHeartbeatDropped, "", 100U, 1000U);

    log.clearList();
    EXPECT_TRUE(log.entries().empty());
    // 累计计数留着：点一下"清屏"就把"一共发生过多少次"抹掉，等于自己删证据。
    EXPECT_EQ(log.raisedTotal(), 2U);
}

TEST(AlarmLog, AFullLogNeverEvictsAnUnacknowledgedBlockingAlarm) {
    // 清单满了要腾位置。被挤掉的只能是"不需要确认的"或"已经确认过的"——
    // 把一条未确认的拦启动报警静默挤掉，等于在没人察觉的情况下把启动闸门打开。
    // 容量 2：第三条进来时必须腾位置。
    mc::AlarmLog log{2U};
    log.raise(mc::kAlarmCodeHeartbeatDropped, "", 10U, 10U);   // 不需要确认
    log.raise(mc::kAlarmCodeLinkLost, "", 20U, 20U);           // 需要确认、未确认

    log.raise(mc::kAlarmCodeFaultCleared, "", 30U, 30U);       // 满了，该挤掉警告那条

    bool sawLinkLost = false;
    bool sawWarning = false;
    for (const mc::AlarmEntry& entry : log.entries()) {
        if (entry.code == mc::kAlarmCodeLinkLost) {
            sawLinkLost = true;
        }
        if (entry.code == mc::kAlarmCodeHeartbeatDropped) {
            sawWarning = true;
        }
    }
    EXPECT_TRUE(sawLinkLost) << "未确认的拦启动报警被挤掉了";
    EXPECT_FALSE(sawWarning) << "该被挤掉的是不需要确认的那条";
    EXPECT_EQ(log.discarded(), 1U);
    EXPECT_TRUE(log.blocksStart());
}

TEST(AlarmLog, AConfirmedEntryIsEvictedBeforeAnUnconfirmedOne) {
    mc::AlarmLog log{2U};
    log.raise(mc::kAlarmCodeLinkLost, "", 10U, 10U);
    log.raise(mc::kAlarmCodeSessionLost, "", 20U, 20U);
    ASSERT_TRUE(log.acknowledge(mc::kAlarmCodeLinkLost, 30U));

    log.raise(mc::kAlarmCodeOpenFailed, "", 40U, 40U);   // 满了

    bool sawLinkLost = false;
    for (const mc::AlarmEntry& entry : log.entries()) {
        if (entry.code == mc::kAlarmCodeLinkLost) {
            sawLinkLost = true;
        }
    }
    EXPECT_FALSE(sawLinkLost) << "已确认的那条才是可以丢的";
    EXPECT_TRUE(log.blocksStart()) << "剩下两条都还没确认";
}

TEST(AlarmLog, AcknowledgeAndClearReportWhetherTheyFoundAnything) {
    mc::AlarmLog log{8U};
    EXPECT_FALSE(log.clear(mc::kAlarmCodeLinkLost));
    EXPECT_FALSE(log.acknowledge(mc::kAlarmCodeLinkLost, 100U));

    log.raise(mc::kAlarmCodeLinkLost, "", 100U, 1000U);
    EXPECT_TRUE(log.clear(mc::kAlarmCodeLinkLost));
    EXPECT_TRUE(log.acknowledge(mc::kAlarmCodeLinkLost, 1100U));
    // 已经确认过的再确认一次，没有东西可确认 —— 必须如实返回 false，
    // 否则界面会在一个"其实什么也没发生"的动作上报成功。
    EXPECT_FALSE(log.acknowledge(mc::kAlarmCodeLinkLost, 1200U));
}

TEST(AlarmLog, TheCapacityIsBoundedAndTheDefaultIsNotZero) {
    mc::AlarmLog log{0U};   // 0 是误用：换成默认值，而不是变成"一条都存不下"
    EXPECT_EQ(log.capacity(), mc::AlarmLog::kDefaultCapacity);
    EXPECT_GT(log.capacity(), 0U);
}
