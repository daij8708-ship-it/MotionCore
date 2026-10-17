#pragma once

#include <QString>

#include "mc/interlock.hpp"

namespace mchost {

// 联锁规则的中文。**规则表住在核心库**（那里只有英文标识符，因为日志与将来的
// 固件要共用同一份编号），措辞属于界面这一侧 —— 与 alarmText / recipeErrorText
// 同一个安排：同一条理由在两处各写一套措辞，迟早会说成两件事。
QString interlockRuleText(std::uint16_t code);
QString interlockSeverityText(mc::InterlockSeverity severity);

// 把一条判定结果连同现场快照写成一句能读懂的话。**满足与不满足都要有话说**：
// 只说"这条没满足"，操作员还是不知道现在是什么样、要变成什么样。
//
// `worstUnacknowledgedAlarm` 是"最严重的那条未确认报警"的文本 —— 只有宿主侧
// 知道它（报警簿在那边），所以从这里传进来；只有报警那条规则用得到。
QString interlockSentence(const mc::InterlockResult& result, const mc::InterlockSnapshot& snapshot,
                          const QString& worstUnacknowledgedAlarm);

}  // namespace mchost
