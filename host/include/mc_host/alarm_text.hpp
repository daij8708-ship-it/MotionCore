#pragma once

#include <cstdint>

#include <QString>

#include "mc/alarm.hpp"

namespace mchost {

// 报警代码 → 给人看的一句话。**代码表本身住在核心库**（那里只有英文标识符，
// 因为固件与日志要共用同一份编号）；中文措辞属于界面这一侧，与 recipeErrorText
// 同一个安排：同一条原因在两处各写一套措辞，迟早会说成两件事。
//
// 未知代码返回"未知报警（代码 XXXX）"，**不编一句像样的文本** ——
// 编出来的文本会让人以为设备报的就是那个意思。等级另外显示，见 alarmLevelText。
QString alarmText(std::uint16_t code);

// 等级的中文名（信息 / 警告 / 错误 / 严重）。
QString alarmLevelText(mc::AlarmLevel level);

// 报警代码的书写形式：固定 "0x" + 4 位十六进制（例如 0x0102）。
// 单独一个函数是因为它有两个必须同时成立的要求，随手拼很容易只做到一个：
//   ① 十六进制**必须大写**（0x0102，不是 0x0102 里混着小写）；
//   ② 前缀 "0x" 里的那个 x **必须是小写** —— 对整个字符串调 toUpper() 会把
//      它顶成 "0X0102"，而截图、日志、文档三处指的就成了两种写法。
QString alarmCodeText(std::uint16_t code);

}  // namespace mchost
