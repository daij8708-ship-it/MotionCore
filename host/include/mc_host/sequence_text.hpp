#pragma once

#include <QString>

#include "mc/sequence.hpp"

namespace mchost {

// 任务调度相关的 token → 给人看的中文。**代码表住在核心库**（那里只有英文标识符，
// 因为固件、日志、库文件要共用同一份编号），措辞属于界面这一侧 —— 与 recipeErrorText、
// alarmText 同一个安排：同一条原因在两处各写一套措辞，迟早会说成两件事。
QString sequenceErrorText(mc::SequenceError error);
QString sequenceStepKindText(mc::SequenceStepKind kind);
QString stepOutcomeText(mc::StepOutcome outcome);
QString sequencePhaseText(mc::SequencePhase phase);

// 一步参数的一句话摘要（"模式=速度环"、"等待 500 ms"…）。
// 表格里一格放不下几个字段，而**人复核程序时看的就是这一列** ——
// 所以它必须把这一步真正会发出去的那个值写出来，不能只写个类型名。
QString sequenceStepSummary(const mc::SequenceStep& step);

}  // namespace mchost
