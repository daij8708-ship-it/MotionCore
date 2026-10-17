#pragma once

#include <QString>

#include "mc/recipe.hpp"

namespace mchost {

// 配方校验失败的原因翻成给人看的一句话。
//
// 放在**一处**，而不是界面一份、应用服务一份：同一个原因在两处各写一套措辞，
// 迟早出现"界面说名称不能为空、下装提示说 EmptyName"这种情况，而使用者
// 会以为是两个不同的问题（见 device.hpp 里 nackReasonName 的同一条理由）。
//
// 同样刻意**不合并成"配方不合法"**：那句话没有一个字能告诉操作员该改哪个字段。
QString recipeErrorText(mc::RecipeError error);

}  // namespace mchost
