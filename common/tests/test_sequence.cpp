#include "mc/sequence.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mc/payload.hpp"   // 回零速度的上界与设备侧是同一个常量，用例直接引它

// 自动任务调度的用例。
//
// 重点不在"能按顺序把命令发出去"—— 那个写错了当场就能看见。重点是四条**规则**：
//   ① **一步在飞**：同一时刻只允许一条命令等结局（否则 NACK 说不清是谁的）；
//   ② **不确定就停**：被拒 / 超时 / 没发出去，整部程序当场停在那一-步；
//   ③ 终态只进一次：先到的原因才是真原因；
//   ④ 非法程序**整份**拒掉，半份程序不许跑。
namespace {

mc::SequenceStep makeStep(mc::SequenceStepKind kind) {
    mc::SequenceStep step;
    step.kind = kind;
    return step;
}

// 一步完整的动作程序：设参数 → 起转 → 等一段 → 停。
mc::SequenceProgram makeProgram() {
    mc::SequenceProgram program;
    program.name = "TASK-A";
    program.version = 2U;

    mc::SequenceStep mode = makeStep(mc::SequenceStepKind::SetMode);
    mode.mode = 1U;   // 速度环
    mode.note = "进速度环";

    mc::SequenceStep pid = makeStep(mc::SequenceStepKind::SetPid);
    pid.kp = 1.5F;
    pid.ki = 0.3F;
    pid.kd = 0.0F;

    mc::SequenceStep target = makeStep(mc::SequenceStepKind::SetTarget);
    target.target = 200.0F;   // deg/s 在速度环上界以内

    mc::SequenceStep start = makeStep(mc::SequenceStepKind::Start);
    mc::SequenceStep wait = makeStep(mc::SequenceStepKind::Wait);
    wait.waitMs = 500U;
    mc::SequenceStep stop = makeStep(mc::SequenceStepKind::Stop);

    program.steps = {mode, pid, target, start, wait, stop};
    return program;
}

// 模拟宿主的一拍：先推进引擎，再把该发的发出去。
// 返回是否真的发出了一条命令。
bool pumpDispatch(mc::Sequencer& sequencer, std::uint64_t nowMs,
                  mc::SequenceStepKind* kind = nullptr) {
    sequencer.tick(nowMs);
    if (!sequencer.awaitingDispatch()) {
        return false;
    }
    if (kind != nullptr) {
        *kind = sequencer.currentStep().kind;
    }
    sequencer.reportDispatched(true, "");
    return true;
}

// 把当前在飞的那一条确认掉。
void ack(mc::Sequencer& sequencer) {
    sequencer.reportOutcome(mc::StepOutcome::Acked, "");
}

}  // namespace

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

TEST(SequenceValidation, AWellFormedProgramPasses) {
    EXPECT_EQ(mc::validateSequence(makeProgram(), 1U), mc::SequenceError::None);
}

TEST(SequenceValidation, ProgramLevelFieldsAreChecked) {
    mc::SequenceProgram program = makeProgram();
    program.name.clear();
    EXPECT_EQ(mc::validateSequence(program, 1U), mc::SequenceError::EmptyName);

    program = makeProgram();
    program.version = 0U;   // 0 只可能是"没填"
    EXPECT_EQ(mc::validateSequence(program, 1U), mc::SequenceError::BadVersion);

    program = makeProgram();
    program.steps.clear();
    EXPECT_EQ(mc::validateSequence(program, 1U), mc::SequenceError::NoSteps);

    program = makeProgram();
    program.steps.assign(mc::kMaxSequenceSteps + 1U, makeStep(mc::SequenceStepKind::Stop));
    EXPECT_EQ(mc::validateSequence(program, 1U), mc::SequenceError::TooManySteps);

    program = makeProgram();
    program.steps = std::vector<mc::SequenceStep>(mc::kMaxSequenceSteps,
                                                  makeStep(mc::SequenceStepKind::Stop));
    EXPECT_EQ(mc::validateSequence(program, 1U), mc::SequenceError::None)
        << "恰好等于上限应当放行";
}

TEST(SequenceValidation, StepParametersAreChecked) {
    mc::SequenceStep mode = makeStep(mc::SequenceStepKind::SetMode);
    mode.mode = 3U;   // 超出 0..kMaxRunMode
    EXPECT_EQ(mc::validateSequenceStep(mode, 1U), mc::SequenceError::BadMode);

    mc::SequenceStep pid = makeStep(mc::SequenceStepKind::SetPid);
    pid.kp = -1.0F;
    EXPECT_EQ(mc::validateSequenceStep(pid, 1U), mc::SequenceError::GainNegative);
    pid.kp = 0.0F;
    pid.ki = 1.0e9F;   // 超过 kSanityMaxPidGain
    EXPECT_EQ(mc::validateSequenceStep(pid, 1U), mc::SequenceError::GainTooLarge);

    mc::SequenceStep target = makeStep(mc::SequenceStepKind::SetTarget);
    target.target = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(mc::validateSequenceStep(target, 1U), mc::SequenceError::TargetNotFinite);

    mc::SequenceStep wait = makeStep(mc::SequenceStepKind::Wait);
    EXPECT_EQ(mc::validateSequenceStep(wait, 1U), mc::SequenceError::WaitWithoutDuration)
        << "零等待是「写漏了」的典型样子，必须拒掉";
    wait.waitMs = mc::kMaxWaitMs + 1U;
    EXPECT_EQ(mc::validateSequenceStep(wait, 1U), mc::SequenceError::WaitTooLong);

    mc::SequenceStep note = makeStep(mc::SequenceStepKind::Stop);
    note.note.assign(mc::kMaxStepNoteBytes + 1U, 'x');
    EXPECT_EQ(mc::validateSequenceStep(note, 1U), mc::SequenceError::NoteTooLong);
}

// 同一个 200，在速度环里是 200 deg/s（合法），在位置环里是 200 deg（也合法）；
// 但 20000 在速度环里越界、在位置环里合法 —— 所以"合法区间"取决于**当时生效的模式**，
// 而模式是程序自己一步步设出来的。
TEST(SequenceValidation, TheTargetBoundFollowsTheModeTheProgramItselfSets) {
    const auto programWith = [](std::uint8_t mode, float target) {
        mc::SequenceProgram program;
        program.name = "X";
        mc::SequenceStep setMode = makeStep(mc::SequenceStepKind::SetMode);
        setMode.mode = mode;
        mc::SequenceStep setTarget = makeStep(mc::SequenceStepKind::SetTarget);
        setTarget.target = target;
        program.steps = {setMode, setTarget};
        return program;
    };

    // 20000 deg/s 超过速度环上界（10000），被拒。
    EXPECT_EQ(mc::validateSequence(programWith(1U, 20000.0F), 1U),
              mc::SequenceError::TargetOutOfRange);
    // 同一部程序里把模式改成位置环，同一个数字就合法了（位置环上界是 1e9 deg）。
    EXPECT_EQ(mc::validateSequence(programWith(2U, 20000.0F), 1U), mc::SequenceError::None);

    // 程序没设过模式时，用调用方给的初始模式判 —— 不能凭空猜。
    mc::SequenceProgram noMode;
    noMode.name = "Y";
    mc::SequenceStep setTarget = makeStep(mc::SequenceStepKind::SetTarget);
    setTarget.target = 20000.0F;
    noMode.steps = {setTarget};
    EXPECT_EQ(mc::validateSequence(noMode, 1U), mc::SequenceError::TargetOutOfRange);
    EXPECT_EQ(mc::validateSequence(noMode, 2U), mc::SequenceError::None);
}

// 非法程序**整份**拒掉，而且不许把上一份好程序弄坏：
// 半份程序比没有程序更危险 —— 它看起来是能跑的。
TEST(SequenceValidation, AnInvalidProgramIsRefusedWholeAndLeavesTheOldOneIntact) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 1U)) << sequencer.loadErrorDetail();

    mc::SequenceProgram broken = makeProgram();
    broken.version = 0U;
    EXPECT_FALSE(sequencer.load(broken, 1U));
    EXPECT_EQ(sequencer.loadError(), mc::SequenceError::BadVersion);
    EXPECT_FALSE(sequencer.loadErrorDetail().empty());

    // 原来那份仍然在、仍然能跑。
    EXPECT_TRUE(sequencer.loaded());
    EXPECT_EQ(sequencer.program().name, "TASK-A");
    EXPECT_EQ(sequencer.program().steps.size(), std::size_t{6});
    EXPECT_TRUE(sequencer.start(0U));
}

// ---------------------------------------------------------------------------
// 存库用的规范字节
// ---------------------------------------------------------------------------

// "同一部程序永远得到同一串字节"必须成立，否则校验和只能证明字节没变、
// 证明不了语义没变 —— 而后者才是库里那一列存在的理由。
TEST(SequenceEncoding, RoundTripsAndIsDeterministic) {
    const mc::SequenceProgram program = makeProgram();
    const std::vector<std::uint8_t> bytes = mc::encodeSequence(program);

    mc::SequenceProgram decoded;
    ASSERT_EQ(mc::decodeSequence(bytes.data(), bytes.size(), decoded), mc::SequenceError::None);

    EXPECT_EQ(decoded.name, program.name);
    EXPECT_EQ(decoded.version, program.version);
    ASSERT_EQ(decoded.steps.size(), program.steps.size());
    for (std::size_t i = 0U; i < program.steps.size(); ++i) {
        EXPECT_EQ(decoded.steps[i].kind, program.steps[i].kind);
        EXPECT_EQ(decoded.steps[i].mode, program.steps[i].mode);
        EXPECT_EQ(decoded.steps[i].waitMs, program.steps[i].waitMs);
        EXPECT_FLOAT_EQ(decoded.steps[i].target, program.steps[i].target);
        EXPECT_FLOAT_EQ(decoded.steps[i].kp, program.steps[i].kp);
        EXPECT_EQ(decoded.steps[i].note, program.steps[i].note);
    }

    // 编两遍、再解一遍再编：三串字节必须完全一样。
    EXPECT_EQ(mc::encodeSequence(program), bytes);
    EXPECT_EQ(mc::encodeSequence(decoded), bytes);
}

TEST(SequenceEncoding, TheChecksumCoversEveryField) {
    const mc::SequenceProgram base = makeProgram();
    const std::uint16_t checksum = mc::sequenceChecksum(base);

    // 每一个可能被改坏的地方都要让校验和变。
    mc::SequenceProgram program = base;
    program.name = "TASK-B";
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改名";

    program = base;
    program.version = 3U;
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改版本";

    program = base;
    program.steps[2].target = 250.0F;
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改目标值";

    program = base;
    program.steps[1].ki = 0.5F;
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改增益";

    program = base;
    program.steps[4].waitMs = 501U;
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改等待时长";

    program = base;
    program.steps[0].note = "换个说法";
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "改说明";

    program = base;
    program.steps.pop_back();
    EXPECT_NE(mc::sequenceChecksum(program), checksum) << "少一步";

    EXPECT_EQ(mc::sequenceChecksum(base), checksum) << "没动它就不该变";
}

TEST(SequenceEncoding, MalformedBytesAreRejectedInsteadOfGuessed) {
    const std::vector<std::uint8_t> good = mc::encodeSequence(makeProgram());
    mc::SequenceProgram out;

    EXPECT_EQ(mc::decodeSequence(good.data(), good.size() - 1U, out), mc::SequenceError::Truncated);
    EXPECT_EQ(mc::decodeSequence(good.data(), 3U, out), mc::SequenceError::Truncated);

    // 尾部多一个字节：字节流与这个结构对不上，不当成"没关系"。
    std::vector<std::uint8_t> longer = good;
    longer.push_back(0U);
    EXPECT_EQ(mc::decodeSequence(longer.data(), longer.size(), out),
              mc::SequenceError::TrailingBytes);

    // 头部布局：名称长度(2) + 名称 + 程序版本(2) + 步数(2)，之后才是第一步。
    const std::size_t stepCountOffset = 2U + std::string("TASK-A").size() + 2U;
    const std::size_t firstStepOffset = stepCountOffset + 2U;

    // 步类型字节改成不认识的值：**不许猜成一个默认类型**。
    std::vector<std::uint8_t> badKind = good;
    badKind[firstStepOffset] = 0x7FU;
    EXPECT_EQ(mc::decodeSequence(badKind.data(), badKind.size(), out), mc::SequenceError::BadKind);

    // 步数字段被改成一个离谱的值：它是格式层面的界，得当场拒掉，
    // 不能按那个数去 reserve 一大块内存。
    std::vector<std::uint8_t> badCount = good;
    badCount[stepCountOffset] = 0xFFU;
    badCount[stepCountOffset + 1U] = 0xFFU;
    EXPECT_EQ(mc::decodeSequence(badCount.data(), badCount.size(), out),
              mc::SequenceError::BadStepCount);
}

TEST(SequenceEncoding, ADecodedProgramStillGoesThroughTheSameValidation) {
    // 结构解码与语义校验是两层，谁都不替谁。
    mc::SequenceProgram program = makeProgram();
    program.steps[4].waitMs = 0U;   // 结构好好的，语义不合法
    const std::vector<std::uint8_t> bytes = mc::encodeSequence(program);

    mc::SequenceProgram decoded;
    ASSERT_EQ(mc::decodeSequence(bytes.data(), bytes.size(), decoded), mc::SequenceError::None);
    EXPECT_EQ(mc::validateSequence(decoded, 1U), mc::SequenceError::WaitWithoutDuration);
}

// ---------------------------------------------------------------------------
// 执行
// ---------------------------------------------------------------------------

TEST(SequencerRun, StepsRunInOrderAndEveryOneMustBeAcknowledged) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));

    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Idle);
    ASSERT_TRUE(sequencer.start(0U));
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Running);

    // 前四步都是设备命令：一条一条来，每条都要等到"已确认"才轮到下一步。
    const mc::SequenceStepKind expected[] = {
        mc::SequenceStepKind::SetMode, mc::SequenceStepKind::SetPid,
        mc::SequenceStepKind::SetTarget, mc::SequenceStepKind::Start,
    };
    for (const mc::SequenceStepKind kind : expected) {
        mc::SequenceStepKind dispatched = mc::SequenceStepKind::Wait;
        ASSERT_TRUE(pumpDispatch(sequencer, 0U, &dispatched)) << "该发的那一步没发出来";
        EXPECT_EQ(dispatched, kind);
        // 在飞的时候不许再摆出下一步 —— 铁律①。
        EXPECT_FALSE(sequencer.awaitingDispatch());
        EXPECT_TRUE(sequencer.status().waitingOutcome);
        ack(sequencer);
    }

    // 第五步是等待：它不产生下行报文，只由时间推进。
    EXPECT_FALSE(pumpDispatch(sequencer, 0U));
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{4});
    EXPECT_GT(sequencer.status().waitUntilMs, 0U);
    EXPECT_FALSE(sequencer.awaitingDispatch());

    // 还没到点：不动。
    sequencer.tick(100U);
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{4});
    // 到点了：往下走。
    sequencer.tick(600U);

    mc::SequenceStepKind last = mc::SequenceStepKind::Wait;
    ASSERT_TRUE(pumpDispatch(sequencer, 600U, &last));
    EXPECT_EQ(last, mc::SequenceStepKind::Stop);
    ack(sequencer);

    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Succeeded);
    EXPECT_EQ(sequencer.status().completedSteps, std::size_t{6});
    // 每一步都留下一条"已确认"的流水：这是"整部程序确实跑完了"的证据。
    ASSERT_EQ(sequencer.reports().size(), std::size_t{6});
    for (const mc::StepReport& report : sequencer.reports()) {
        EXPECT_EQ(report.outcome, mc::StepOutcome::Acked);
    }
    EXPECT_FALSE(sequencer.awaitingDispatch());
    EXPECT_FALSE(sequencer.status().waitingOutcome);
}

TEST(SequencerRun, ANackedStepStopsTheWholeProgramRightThere) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));

    // 第一步就被拒（设备以状态非法之类的原因拒掉）。
    ASSERT_TRUE(pumpDispatch(sequencer, 0U));
    sequencer.reportOutcome(mc::StepOutcome::Nacked, "设备拒绝：状态非法");

    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{0});
    EXPECT_EQ(sequencer.status().completedSteps, std::size_t{0});
    EXPECT_NE(sequencer.status().detail.find("状态非法"), std::string::npos)
        << "失败原因必须带上设备给的说明，不能只说「失败了」";
    // 停在原地：不许偷偷往下走。
    EXPECT_FALSE(sequencer.awaitingDispatch());
    EXPECT_FALSE(sequencer.status().waitingOutcome);
    ASSERT_EQ(sequencer.reports().size(), std::size_t{1});
    EXPECT_EQ(sequencer.reports()[0].outcome, mc::StepOutcome::Nacked);
}

// 运动指令不自动重发（ADR-003）：超时之后引擎**不会**再摆出同一步。
TEST(SequencerRun, ATimedOutStepIsNeverAutomaticallyRetried) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));

    ASSERT_TRUE(pumpDispatch(sequencer, 0U));
    sequencer.reportOutcome(mc::StepOutcome::TimedOut, "等超时了");

    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    ASSERT_EQ(sequencer.reports().size(), std::size_t{1});
    EXPECT_EQ(sequencer.reports()[0].outcome, mc::StepOutcome::TimedOut);

    // 再推几拍也不该有任何东西被摆出来重发。
    for (std::uint64_t t = 10U; t <= 1000U; t += 10U) {
        sequencer.tick(t);
        EXPECT_FALSE(sequencer.awaitingDispatch()) << "超时之后又摆了重发";
    }
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
}

TEST(SequencerRun, ALocallyRefusedStepFailsWithItsReason) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));

    ASSERT_TRUE(sequencer.awaitingDispatch());
    // 宿主那一侧的门禁拦下了（例如"有未确认的报警"或"会话未就绪"）。
    sequencer.reportDispatched(false, "有 1 条报警未确认");

    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    EXPECT_NE(sequencer.status().detail.find("未确认"), std::string::npos);
    ASSERT_EQ(sequencer.reports().size(), std::size_t{1});
    EXPECT_EQ(sequencer.reports()[0].outcome, mc::StepOutcome::NotSent);
}

TEST(SequencerRun, StartIsRefusedWithoutAProgramAndWhileOneIsRunning) {
    mc::Sequencer sequencer;
    EXPECT_FALSE(sequencer.start(0U));
    EXPECT_FALSE(sequencer.status().detail.empty());

    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));
    EXPECT_FALSE(sequencer.start(0U)) << "一部没跑完就再启一部 = 两条命令同时在飞";
    EXPECT_FALSE(sequencer.status().detail.empty());
}

// 终态只进一次：先到的原因才是真原因。同一条命令的超时之后紧跟着链路失效，
// 不该把"超时"改写成"中止"—— 那会让人以为是别人按了停。
TEST(SequencerRun, TheFirstConclusiveReasonWinsAndLaterOnesAreIgnored) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));

    ASSERT_TRUE(pumpDispatch(sequencer, 0U));
    sequencer.reportOutcome(mc::StepOutcome::TimedOut, "等超时了");
    const std::string detail = sequencer.status().detail;

    sequencer.abort("链路失效");
    sequencer.reportOutcome(mc::StepOutcome::Nacked, "不该出现");
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    EXPECT_EQ(sequencer.status().detail, detail);
    EXPECT_EQ(sequencer.reports().size(), std::size_t{1});
}

TEST(SequencerRun, AbortStopsTheProgramAndKeepsTheReason) {
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));
    ASSERT_TRUE(pumpDispatch(sequencer, 0U));
    ack(sequencer);

    sequencer.abort("操作员按了中止");
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Aborted);
    EXPECT_NE(sequencer.status().detail.find("操作员按了中止"), std::string::npos);

    // 中止之后不再有任何一步被摆出来。
    for (std::uint64_t t = 0U; t <= 500U; t += 50U) {
        sequencer.tick(t);
        EXPECT_FALSE(sequencer.awaitingDispatch());
    }
    // 而且它不改变"已经完成了几步"这个事实：中止不是撤销。
    EXPECT_EQ(sequencer.status().completedSteps, std::size_t{1});
}

TEST(SequencerRun, TheOutcomeOfACommandThatWasNeverSentIsIgnored) {
    // 宿主可能在链路断了之后才把结局报上来（或者报了两次）。没有在飞的东西时，
    // 这个报告必须被忽略 —— 否则一条迟到的"已确认"会把刚失败的程序改写成成功。
    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(makeProgram(), 0U));
    ASSERT_TRUE(sequencer.start(0U));

    sequencer.reportOutcome(mc::StepOutcome::Acked, "");
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Running);
    EXPECT_EQ(sequencer.status().completedSteps, std::size_t{0});
}

// "等设备就绪"这一步的要害不是"能等"，而是**两件相反的事都要做对**：
// 设备已经停了就立刻往下走（不等满超时），设备一直不就绪就得停（不许无限等）。
TEST(SequencerRun, TheWaitReadyStepProceedsAsSoonAsTheDeviceIsReady) {
    mc::SequenceProgram program;
    program.name = "SETTLE";
    mc::SequenceStep stop = makeStep(mc::SequenceStepKind::Stop);
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 5000U;
    mc::SequenceStep ready = makeStep(mc::SequenceStepKind::WaitReady);
    ready.waitMs = 5000U;
    program.steps = {stop, settle, ready};

    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(program, 0U));
    ASSERT_TRUE(sequencer.start(0U));

    ASSERT_TRUE(pumpDispatch(sequencer, 0U));
    ack(sequencer);   // STOP 被接受
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{1});
    EXPECT_FALSE(sequencer.awaitingDispatch()) << "等待类步骤不产生下行报文";

    // 设备还在减速：不动。
    sequencer.reportDeviceReady(false);
    sequencer.tick(100U);
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{1});

    // 设备停了：**立刻**往下走，不等满那 5 秒上限。
    sequencer.reportDeviceReady(true);
    sequencer.tick(120U);
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{2});
    ASSERT_EQ(sequencer.reports().size(), std::size_t{2});
    EXPECT_EQ(sequencer.reports()[1].outcome, mc::StepOutcome::Acked);
    EXPECT_NE(sequencer.reports()[1].detail.find("就绪"), std::string::npos);
}

TEST(SequencerRun, TheWaitReadyStepStopsTheProgramWhenTheDeviceNeverSettles) {
    mc::SequenceProgram program;
    program.name = "SETTLE-TIMEOUT";
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 1000U;
    program.steps = {settle};

    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(program, 0U));
    ASSERT_TRUE(sequencer.start(0U));

    sequencer.reportDeviceReady(false);
    sequencer.tick(0U);
    sequencer.tick(500U);
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Running);

    // 到点还没就绪：**停下并说清原因**，而不是无限等下去。
    sequencer.tick(1001U);
    EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    EXPECT_NE(sequencer.status().detail.find("超时"), std::string::npos)
        << sequencer.status().detail;

    for (std::uint64_t t = 1100U; t <= 2000U; t += 100U) {
        sequencer.tick(t);
        EXPECT_EQ(sequencer.status().phase, mc::SequencePhase::Failed);
    }
}

// 等待类步骤的超时上限也要被校验：0 或超过上限都得拒 ——
// 一部"最多等 0 毫秒"的程序读起来就像个笔误。
TEST(SequenceValidation, WaitReadyUsesItsDurationAsABoundedTimeout) {
    mc::SequenceStep step = makeStep(mc::SequenceStepKind::WaitReady);
    EXPECT_EQ(mc::validateSequenceStep(step, 1U), mc::SequenceError::WaitWithoutDuration);
    step.waitMs = mc::kMaxWaitMs + 1U;
    EXPECT_EQ(mc::validateSequenceStep(step, 1U), mc::SequenceError::WaitTooLong);
    step.waitMs = 5000U;
    EXPECT_EQ(mc::validateSequenceStep(step, 1U), mc::SequenceError::None);
}

// ---------------------------------------------------------------------------
// 回零步（P1-6）：两个字段的含义与别的步**不一样**（target = 速度、waitMs = 时限），
// 而且它有一个跨步规则（后面必须跟"等设备就绪"）。
// ---------------------------------------------------------------------------
namespace {

mc::SequenceStep homeStep(float speedDegPerSec, std::uint64_t timeoutMs) {
    mc::SequenceStep step = makeStep(mc::SequenceStepKind::Home);
    step.target = speedDegPerSec;
    step.waitMs = timeoutMs;
    return step;
}

}  // namespace

TEST(SequenceHome, SpeedAndTimeoutMustBothBeGivenAndSane) {
    EXPECT_EQ(mc::validateSequenceStep(homeStep(60.0F, 30000U), 1U), mc::SequenceError::None);

    // 速度为 0 不是"慢慢走"，是永远走不到：判据是"到位"，而速度为 0 时位置不变。
    EXPECT_EQ(mc::validateSequenceStep(homeStep(0.0F, 30000U), 1U),
              mc::SequenceError::HomeSpeedMissing);
    EXPECT_EQ(mc::validateSequenceStep(homeStep(-60.0F, 30000U), 1U),
              mc::SequenceError::HomeSpeedMissing)
        << "方向由设备按自己的坐标判断，报文里的速度只能是幅值";
    EXPECT_EQ(mc::validateSequenceStep(homeStep(60.0F, 30000U), 1U), mc::SequenceError::None);

    EXPECT_EQ(mc::validateSequenceStep(homeStep(mc::kSanityMaxHomeSpeedDegPerSec + 1.0F, 30000U), 1U),
              mc::SequenceError::HomeSpeedTooLarge);

    // 时限：0 是"没有时限"，走不到的时候会一直挂着，而"挂着"看起来和"还在走"一模一样。
    EXPECT_EQ(mc::validateSequenceStep(homeStep(60.0F, 0U), 1U),
              mc::SequenceError::HomeTimeoutMissing);
    EXPECT_EQ(mc::validateSequenceStep(homeStep(60.0F, mc::kMaxWaitMs + 1U), 1U),
              mc::SequenceError::HomeTimeoutTooLarge);

    // 复用字段**不等于复用错误码**：这一步报的是"回零的时限"，不是"等待步的时长" ——
    // 两者指向的输入框不是同一个，错误码混用会把人引到错的字段上。
    EXPECT_STRNE(mc::sequenceErrorName(mc::SequenceError::HomeTimeoutMissing),
                 mc::sequenceErrorName(mc::SequenceError::WaitWithoutDuration));
}

// 这条规则不是"保险起见"：HOME 的应答只表示**回零开始了**（见 protocol.hpp），
// "零点建立"只能从设备状态看出来。后面不跟一等，程序下一步就会被设备以
// "状态不允许"拒掉 —— 而那个报错指向的是下一步，不是真正的病因。
TEST(SequenceHome, AHomeStepMustBeFollowedByWaitReady) {
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 60000U;

    mc::SequenceProgram orphan;
    orphan.name = "HOME-ONLY";
    orphan.steps = {homeStep(60.0F, 30000U)};
    EXPECT_EQ(mc::validateSequence(orphan, 1U), mc::SequenceError::HomeWithoutWaitReady);

    mc::SequenceProgram good;
    good.name = "HOME-THEN-SETTLE";
    good.steps = {homeStep(60.0F, 30000U), settle};
    EXPECT_EQ(mc::validateSequence(good, 1U), mc::SequenceError::None);

    // 紧跟的那一步必须是"等设备就绪"：中间插一个盲等不算 ——
    // 盲等是"给它一点时间"，而不是"等到它真的到了"，两者不能互相顶替。
    mc::SequenceStep blindWait = makeStep(mc::SequenceStepKind::Wait);
    blindWait.waitMs = 5000U;
    mc::SequenceProgram wrongWaiter;
    wrongWaiter.name = "HOME-THEN-WAIT";
    wrongWaiter.steps = {homeStep(60.0F, 30000U), blindWait, settle};
    EXPECT_EQ(mc::validateSequence(wrongWaiter, 1U), mc::SequenceError::HomeWithoutWaitReady);
}

// 回零步是**命令步**：它要真的发一条报文出去，而不是"自己按时间推进"。
// 这一格错了的后果很隐蔽 —— 程序会显示"回零已确认"，而链路上什么都没有。
TEST(SequenceHome, TheHomeStepIsDispatchedAsACommandAndThenSettles) {
    mc::SequenceProgram program;
    program.name = "HOME";
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 60000U;
    const mc::SequenceStep home = homeStep(45.0F, 20000U);
    program.steps = {home, settle};

    mc::Sequencer sequencer;
    ASSERT_TRUE(sequencer.load(program, 1U));
    ASSERT_TRUE(sequencer.start(0U));

    mc::SequenceStepKind dispatched = mc::SequenceStepKind::Wait;
    ASSERT_TRUE(pumpDispatch(sequencer, 0U, &dispatched));
    EXPECT_EQ(dispatched, mc::SequenceStepKind::Home);

    // 参数**原样**留在这一步里：发命令的人（宿主）要照它填 HOME 报文的
    // 速度与时限，少一个都编不出载荷。
    EXPECT_FLOAT_EQ(sequencer.currentStep().target, 45.0F);
    EXPECT_EQ(sequencer.currentStep().waitMs, std::uint64_t{20000});

    ack(sequencer);
    EXPECT_EQ(sequencer.status().stepIndex, std::size_t{1});
    EXPECT_EQ(sequencer.reports().at(0).kind, mc::SequenceStepKind::Home);
}

// 存库用的规范字节里，类型码是**线外的约定**（只有主机库读它），但同样不能漂：
// 换了码值等于把已经存好的程序全部读成另一种步。
TEST(SequenceHome, TheStoredKindByteIsSevenAndSurvivesTheRoundTrip) {
    mc::SequenceProgram program;
    program.name = "HOME";
    mc::SequenceStep settle = makeStep(mc::SequenceStepKind::WaitReady);
    settle.waitMs = 60000U;
    program.steps = {homeStep(45.0F, 20000U), settle};

    const std::vector<std::uint8_t> bytes = mc::encodeSequence(program);
    // 布局：2 字节 nameLen + 名字 + 2 版本 + 2 步数 ⇒ 第一步的类型字节在 6+n 处。
    const std::size_t kindOffset = 6U + program.name.size();
    ASSERT_GT(bytes.size(), kindOffset);
    EXPECT_EQ(bytes[kindOffset], 7U);

    mc::SequenceProgram decoded;
    ASSERT_EQ(mc::decodeSequence(bytes.data(), bytes.size(), decoded), mc::SequenceError::None);
    ASSERT_EQ(decoded.steps.size(), std::size_t{2});
    EXPECT_EQ(decoded.steps[0].kind, mc::SequenceStepKind::Home);
    EXPECT_FLOAT_EQ(decoded.steps[0].target, 45.0F);
    EXPECT_EQ(decoded.steps[0].waitMs, std::uint64_t{20000});
    EXPECT_FLOAT_EQ(decoded.steps[1].target, 0.0F)
        << "等就绪那一步不该被回零的速度污染";
}
