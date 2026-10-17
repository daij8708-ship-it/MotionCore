#include "mc/recipe.hpp"

#include <cmath>
#include <utility>

#include "mc/crc16.hpp"
#include "mc/payload.hpp"

namespace mc {
namespace {

// 三个增益共用一条判定。抽出来不是因为"重复三遍太长"，
// 而是因为**它们必须用同一条规则**：只给 Kp 加上界、给 Ki/Kd 忘了加，
// 是这类校验最常见的漏法。
RecipeError validateGain(float gain) noexcept {
    if (!std::isfinite(gain)) {
        return RecipeError::GainNotFinite;
    }
    if (gain < 0.0F) {
        return RecipeError::GainNegative;
    }
    if (gain > kSanityMaxPidGain) {
        return RecipeError::GainTooLarge;
    }
    return RecipeError::None;
}

}  // namespace

const char* recipeErrorName(RecipeError error) noexcept {
    switch (error) {
        case RecipeError::None:             return "None";
        case RecipeError::Truncated:        return "Truncated";
        case RecipeError::NameTooLong:      return "NameTooLong";
        case RecipeError::TrailingBytes:    return "TrailingBytes";
        case RecipeError::EmptyName:        return "EmptyName";
        case RecipeError::BadVersion:       return "BadVersion";
        case RecipeError::BadMode:          return "BadMode";
        case RecipeError::TargetNotFinite:  return "TargetNotFinite";
        case RecipeError::TargetOutOfRange: return "TargetOutOfRange";
        case RecipeError::TargetMustBeZero: return "TargetMustBeZero";
        case RecipeError::GainNotFinite:    return "GainNotFinite";
        case RecipeError::GainNegative:     return "GainNegative";
        case RecipeError::GainTooLarge:     return "GainTooLarge";
    }
    return "?";
}

std::vector<std::uint8_t> encodeRecipe(const Recipe& value) {
    const std::size_t nameLen = value.name.size();
    std::vector<std::uint8_t> bytes(kRecipeFixedBytes + nameLen, 0U);

    writeLe16(bytes.data(), static_cast<std::uint16_t>(nameLen));
    for (std::size_t i = 0U; i < nameLen; ++i) {
        bytes[2U + i] = static_cast<std::uint8_t>(value.name[i]);
    }

    std::size_t p = 2U + nameLen;
    writeLe16(bytes.data() + p, value.version);
    p += 2U;
    bytes[p] = value.mode;
    p += 1U;
    writeLeFloat(bytes.data() + p, value.target);
    p += 4U;
    writeLeFloat(bytes.data() + p, value.kp);
    p += 4U;
    writeLeFloat(bytes.data() + p, value.ki);
    p += 4U;
    writeLeFloat(bytes.data() + p, value.kd);
    return bytes;
}

RecipeError decodeRecipe(const std::uint8_t* data, std::size_t len, Recipe& out) noexcept {
    if (data == nullptr) {
        return RecipeError::Truncated;
    }
    if (len < 2U) {
        return RecipeError::Truncated;
    }

    const std::size_t nameLen = static_cast<std::size_t>(readLe16(data));
    // 先报 NameTooLong 再报 Truncated：名称长度本身就是错的（声明了一个
    // 本格式不允许的长度），这时"字节不够"只是它的后果，报长度问题才是真原因。
    if (nameLen > kMaxRecipeNameBytes) {
        return RecipeError::NameTooLong;
    }

    const std::size_t expected = kRecipeFixedBytes + nameLen;
    if (len < expected) {
        return RecipeError::Truncated;
    }
    if (len > expected) {
        return RecipeError::TrailingBytes;
    }

    // 先解到一个局部量：失败时**不碰 out**，调用方手里的旧值不会被写坏一半。
    Recipe parsed;
    parsed.name.assign(reinterpret_cast<const char*>(data + 2U), nameLen);

    std::size_t p = 2U + nameLen;
    parsed.version = readLe16(data + p);
    p += 2U;
    parsed.mode = data[p];
    p += 1U;
    parsed.target = readLeFloat(data + p);
    p += 4U;
    parsed.kp = readLeFloat(data + p);
    p += 4U;
    parsed.ki = readLeFloat(data + p);
    p += 4U;
    parsed.kd = readLeFloat(data + p);

    out = std::move(parsed);
    return RecipeError::None;
}

RecipeError validateRecipe(const Recipe& value) noexcept {
    // 命中即返回：**只报第一个错**。一处错会连带引发后面的判定（模式非法 ⇒
    // 量纲上界无从谈起），逐个都报只会让操作员看到一串互相矛盾的提示。
    if (value.name.empty()) {
        return RecipeError::EmptyName;
    }
    if (value.name.size() > kMaxRecipeNameBytes) {
        return RecipeError::NameTooLong;
    }
    if (value.version == 0U) {
        return RecipeError::BadVersion;
    }
    if (value.mode > kMaxRunMode) {
        return RecipeError::BadMode;
    }
    if (!std::isfinite(value.target)) {
        return RecipeError::TargetNotFinite;
    }

    switch (value.mode) {
        case 0U:
            // 模式 0 是"停止控制"：目标值不参与任何运算，所以它必须**恰好是零**。
            // 带着 200 的"停止控制"配方只有两种下场 —— 执行不了，或者被别的
            // 环节解释成别的意思；两种都不是我们想要的。
            if (value.target != 0.0F) {
                return RecipeError::TargetMustBeZero;
            }
            break;
        case 1U:
            if (std::fabs(value.target) > kSanityMaxAbsVelocityDegPerSec) {
                return RecipeError::TargetOutOfRange;
            }
            break;
        case 2U:
            if (std::fabs(value.target) > kSanityMaxAbsPositionDeg) {
                return RecipeError::TargetOutOfRange;
            }
            break;
        default:
            break;   // mode 越界已在上面的 BadMode 拦掉
    }

    for (const float gain : {value.kp, value.ki, value.kd}) {
        const RecipeError error = validateGain(gain);
        if (error != RecipeError::None) {
            return error;
        }
    }
    return RecipeError::None;
}

std::uint16_t recipeChecksum(const Recipe& value) noexcept {
    const std::vector<std::uint8_t> bytes = encodeRecipe(value);
    return crc16(bytes.data(), bytes.size());
}

}  // namespace mc
