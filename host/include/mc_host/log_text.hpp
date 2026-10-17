#pragma once

#include <QString>

#include "mc_host/device.hpp"

namespace mchost {

// 命令归宿的中文措辞。
//
// `commandStatusName()`（设备层）给的是英文标识符 —— 它是要进日志、进导出、
// 进将来固件的那一份；中文措辞属于界面这一侧，与 alarm_text / recipe_text
// 同一个安排：同一条原因在两处各写一套话，迟早会说成两件事。
QString commandStatusText(CommandStatus status);

// 一条命令行在日志里的样子："SET_TARGET #7 已确认"。
// 单独一个函数是为了让日志与界面**用同一句话**：两处各拼一遍，
// 出了分歧就得靠人去比对两边有什么不同。
QString commandEventText(const CommandRecord& record);

}  // namespace mchost
