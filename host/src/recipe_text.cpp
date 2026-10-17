#include "mc_host/recipe_text.hpp"

#include "mc/payload.hpp"

namespace mchost {

QString recipeErrorText(mc::RecipeError error) {
    switch (error) {
        case mc::RecipeError::None:
            return QStringLiteral("校验通过");
        case mc::RecipeError::Truncated:
            return QStringLiteral("字节流不完整");
        case mc::RecipeError::NameTooLong:
            return QStringLiteral("名称太长（上限 %1 字节，按 UTF-8 计）")
                .arg(static_cast<int>(mc::kMaxRecipeNameBytes));
        case mc::RecipeError::TrailingBytes:
            return QStringLiteral("字节流尾部有多余内容");
        case mc::RecipeError::EmptyName:
            return QStringLiteral("名称不能为空 —— 名字是操作员挑配方的唯一依据");
        case mc::RecipeError::BadVersion:
            return QStringLiteral("版本号不能为 0（0 只可能表示“没填”）");
        case mc::RecipeError::BadMode:
            return QStringLiteral("模式超出 0…%1").arg(static_cast<int>(mc::kMaxRunMode));
        case mc::RecipeError::TargetNotFinite:
            return QStringLiteral("目标值不是有限数（NaN 或无穷）");
        case mc::RecipeError::TargetOutOfRange:
            return QStringLiteral("目标值超出该模式的量程上限");
        case mc::RecipeError::TargetMustBeZero:
            return QStringLiteral("模式是「停止控制」，目标值必须为 0");
        case mc::RecipeError::GainNotFinite:
            return QStringLiteral("PID 增益不是有限数");
        case mc::RecipeError::GainNegative:
            return QStringLiteral("PID 增益不能为负");
        case mc::RecipeError::GainTooLarge:
            return QStringLiteral("PID 增益超过上限 %1")
                .arg(static_cast<double>(mc::kSanityMaxPidGain));
    }
    return QStringLiteral("未知原因");
}

}  // namespace mchost
