#include "mc/interlock.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// 软件联锁规则表的用例。
//
// 这张表的全部价值在于"一次把没满足的都列出来"，所以用例的重点是两件事：
//   ① 每一条规则**只看它该看的那一项** —— 用"只翻一项、只有一条不满足"钉住；
//   ② 不短路：一次判完，顺序固定。
namespace {

// 一份"全都满足"的现场。每条规则各只改自己那一项，用来确认它没读错字段。
mc::InterlockSnapshot satisfiedSnapshot() {
    mc::InterlockSnapshot snapshot;
    snapshot.connected = true;
    snapshot.linkDown = false;
    snapshot.sessionReady = true;
    snapshot.deviceState = mc::DeviceState::Ready;
    snapshot.faultLatched = false;
    snapshot.deviceMode = 1U;   // 速度环
    snapshot.telemetryCount = 100U;
    snapshot.unacknowledgedAlarms = 0U;
    return snapshot;
}

// 在结果里找某一条。
const mc::InterlockResult* find(const std::vector<mc::InterlockResult>& results,
                               std::uint16_t code) {
    for (const mc::InterlockResult& result : results) {
        if (result.code == code) {
            return &result;
        }
    }
    return nullptr;
}

// 不满足的**Block** 规则有哪几条。
std::vector<std::uint16_t> unmetBlocks(const std::vector<mc::InterlockResult>& results) {
    std::vector<std::uint16_t> codes;
    for (const mc::InterlockResult& result : results) {
        if (result.severity == mc::InterlockSeverity::Block && !result.satisfied) {
            codes.push_back(result.code);
        }
    }
    return codes;
}

}  // namespace

TEST(InterlockTable, EveryRuleHasAStableTokenAndAReasonItExists) {
    const std::vector<mc::InterlockRule>& rules = mc::interlockRules();
    ASSERT_FALSE(rules.empty());

    std::vector<std::uint16_t> codes;
    bool hasBlock = false;
    bool hasWarn = false;
    for (const mc::InterlockRule& rule : rules) {
        ASSERT_NE(rule.token, nullptr);
        EXPECT_STRNE(rule.token, "");
        EXPECT_STRNE(rule.token, "?");
        // "为什么有这条规则"必须写得出来 —— 说不清的那条会把整张表变成仪式。
        ASSERT_NE(rule.why, nullptr);
        EXPECT_STRNE(rule.why, "");
        for (const std::uint16_t seen : codes) {
            EXPECT_NE(seen, rule.code) << "编号重复：" << rule.code;
        }
        codes.push_back(rule.code);
        const mc::InterlockRule* looked = mc::interlockRule(rule.code);
        ASSERT_NE(looked, nullptr);
        EXPECT_EQ(std::string(looked->token), std::string(rule.token));
        EXPECT_EQ(looked->severity, rule.severity);
        hasBlock = hasBlock || (rule.severity == mc::InterlockSeverity::Block);
        hasWarn = hasWarn || (rule.severity == mc::InterlockSeverity::Warn);
    }
    EXPECT_TRUE(hasBlock);
    EXPECT_TRUE(hasWarn) << "只有一种等级的话，等级这个字段就是多余的";
    // 不认识的编号没有规则 —— 不给兜底，也不给"未知规则"。
    EXPECT_EQ(mc::interlockRule(0x7FFFU), nullptr);
    EXPECT_EQ(mc::interlockRule(0x0000U), nullptr);
}

TEST(InterlockTable, ASatisfiedPlantBlocksNothing) {
    const std::vector<mc::InterlockResult> results = mc::evaluateInterlocks(satisfiedSnapshot());
    EXPECT_FALSE(mc::interlocksBlock(results));
    EXPECT_EQ(mc::firstBlockingInterlock(results), nullptr);
    // 一条都不许漏判：结果条数与表一样长，顺序也一致。
    ASSERT_EQ(results.size(), mc::interlockRules().size());
    for (std::size_t i = 0U; i < results.size(); ++i) {
        EXPECT_EQ(results[i].code, mc::interlockRules()[i].code);
        EXPECT_TRUE(results[i].satisfied) << mc::interlockRules()[i].token;
    }
}

// 每一条 Block 规则各只翻自己那一项：翻完之后**只有这一条**不满足。
// 这一条用例同时钉住了两件事：规则读的是对的字段，而且没有顺手读到别的字段
// （比如"会话没就绪"顺手把"设备没就绪"也算上 —— 那种串味最难查）。
TEST(InterlockTable, EachBlockingRuleReadsExactlyItsOwnField) {
    const auto check = [](const char* token, std::uint16_t code,
                          const mc::InterlockSnapshot& snapshot) {
        const std::vector<mc::InterlockResult> results = mc::evaluateInterlocks(snapshot);
        const std::vector<std::uint16_t> unmet = unmetBlocks(results);
        ASSERT_EQ(unmet.size(), std::size_t{1}) << token << " 牵动了别的规则";
        EXPECT_EQ(unmet[0], code) << token;
        EXPECT_TRUE(mc::interlocksBlock(results));
        const mc::InterlockResult* blocking = mc::firstBlockingInterlock(results);
        ASSERT_NE(blocking, nullptr);
        EXPECT_EQ(blocking->code, code);
    };

    mc::InterlockSnapshot snapshot = satisfiedSnapshot();
    snapshot.connected = false;   // 端口没打开
    check("LINK_UP", mc::kInterlockLinkUp, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.linkDown = true;     // 端口开着，但链路已判失效
    check("LINK_UP", mc::kInterlockLinkUp, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.sessionReady = false;
    check("SESSION_READY", mc::kInterlockSessionReady, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.unacknowledgedAlarms = 1U;
    check("NO_UNACKNOWLEDGED_ALARM", mc::kInterlockNoUnacknowledgedAlarm, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.faultLatched = true;
    check("NO_FAULT_LATCH", mc::kInterlockNoFaultLatch, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.deviceState = mc::DeviceState::Running;   // 还在跑
    check("DEVICE_READY", mc::kInterlockDeviceReady, snapshot);

    snapshot = satisfiedSnapshot();
    snapshot.deviceMode = 0U;   // 停在"停止控制"
    check("MODE_SELECTED", mc::kInterlockModeSelected, snapshot);
}

// 提示级的规则不满足时**不拦**：这一条是整个等级设计的支点。
// 没有它，"Warn 与 Block 分成两个等级"就只是一句没有落地的说法。
TEST(InterlockTable, AWarningNeverBlocksAndIsStillReported) {
    mc::InterlockSnapshot snapshot = satisfiedSnapshot();
    snapshot.telemetryCount = 0U;   // 还没收到过任何遥测

    const std::vector<mc::InterlockResult> results = mc::evaluateInterlocks(snapshot);
    const mc::InterlockResult* warn = find(results, mc::kInterlockTelemetryLive);
    ASSERT_NE(warn, nullptr);
    EXPECT_EQ(warn->severity, mc::InterlockSeverity::Warn);
    EXPECT_FALSE(warn->satisfied);
    // 报出来了，但不拦。
    EXPECT_FALSE(mc::interlocksBlock(results));
    EXPECT_EQ(mc::firstBlockingInterlock(results), nullptr);
}

// **不短路**：好几条同时不满足时，必须一次全列出来。
// 只报第一条会让人改一条、再点一次、再被告知下一条 —— 那正是这张表要消灭的东西。
TEST(InterlockTable, EveryUnmetRuleIsReportedAtOnce) {
    // 一份"什么都没有"的现场：没连上、没会话、没遥测、没选模式。
    // 注意有两项**反而满足**了 —— "没有任何报警"与"没有故障锁存"本来就是好状态。
    // 这也说明"什么都没连上"并不等于"每条规则都不满足"，规则读的是各自那一项。
    const std::vector<mc::InterlockResult> results =
        mc::evaluateInterlocks(mc::InterlockSnapshot{});

    const std::vector<std::uint16_t> unmet = unmetBlocks(results);
    EXPECT_EQ(unmet.size(), std::size_t{4});
    EXPECT_EQ(unmet, (std::vector<std::uint16_t>{mc::kInterlockLinkUp,
                                                 mc::kInterlockSessionReady,
                                                 mc::kInterlockDeviceReady,
                                                 mc::kInterlockModeSelected}));
    for (const std::uint16_t code : {mc::kInterlockNoUnacknowledgedAlarm,
                                     mc::kInterlockNoFaultLatch}) {
        const mc::InterlockResult* result = find(results, code);
        ASSERT_NE(result, nullptr);
        EXPECT_TRUE(result->satisfied) << "代码 " << code << " 读的不是它自己那一项";
    }

    // 报出来的第一条是表里的第一条（重要性顺序），而不是碰巧先判到的某一条。
    const mc::InterlockResult* first = mc::firstBlockingInterlock(results);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->code, mc::interlockRules()[0].code);
    EXPECT_EQ(first->code, mc::kInterlockLinkUp);
}

// 链路失效与"端口没开"是同一条规则：对操作员来说都是"发不出去"，
// 分成两条只会让人对着两份清单猜哪一条才算数。
TEST(InterlockTable, LinkFailureAndPortClosedAreTheSameRule) {
    mc::InterlockSnapshot closed = satisfiedSnapshot();
    closed.connected = false;
    mc::InterlockSnapshot down = satisfiedSnapshot();
    down.linkDown = true;

    EXPECT_EQ(unmetBlocks(mc::evaluateInterlocks(closed)),
              (std::vector<std::uint16_t>{mc::kInterlockLinkUp}));
    EXPECT_EQ(unmetBlocks(mc::evaluateInterlocks(down)),
              (std::vector<std::uint16_t>{mc::kInterlockLinkUp}));
}
