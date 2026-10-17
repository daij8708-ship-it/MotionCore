#include "mc/recipe.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "mc/crc16.hpp"
#include "mc/payload.hpp"

namespace {

mc::Recipe makeValid() {
    mc::Recipe r;
    r.name = "WAFER-A1";
    r.version = 3U;
    r.mode = 1U;
    r.target = 200.0F;
    r.kp = 1.6F;
    r.ki = 0.4F;
    r.kd = 0.02F;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// 线格式
// ---------------------------------------------------------------------------

TEST(Recipe, EncodingIsCanonicalAndByteExact) {
    // 金标准配方故意取"能心算"的数：增益 1.0 / 0.5 / 0.25 与目标 2.0 的
    // IEEE-754 位模式是确定的（0x3F800000 / 0x3F000000 / 0x3E800000 / 0x40000000），
    // 所以下面这张字节表可以逐字节核对 —— 而不是"跑一遍、把输出抄进来"。
    mc::Recipe r;
    r.name = "R1";
    r.version = 1U;
    r.mode = 1U;
    r.target = 2.0F;
    r.kp = 1.0F;
    r.ki = 0.5F;
    r.kd = 0.25F;

    const std::vector<std::uint8_t> expected = {
        0x02, 0x00,              // nameLen = 2
        'R',  '1',               // 名称
        0x01, 0x00,              // version = 1
        0x01,                    // mode = 1
        0x00, 0x00, 0x00, 0x40,  // target = 2.0
        0x00, 0x00, 0x80, 0x3F,  // kp = 1.0
        0x00, 0x00, 0x00, 0x3F,  // ki = 0.5
        0x00, 0x00, 0x80, 0x3E,  // kd = 0.25
    };

    const std::vector<std::uint8_t> bytes = mc::encodeRecipe(r);
    EXPECT_EQ(bytes, expected);
    // 长度公式本身也要钉住：格式一改，这两个数就得不一致。
    EXPECT_EQ(bytes.size(), mc::kRecipeFixedBytes + 2U);
}

TEST(Recipe, RoundTripPreservesEveryField) {
    const mc::Recipe source = makeValid();
    const std::vector<std::uint8_t> bytes = mc::encodeRecipe(source);

    mc::Recipe parsed;
    ASSERT_EQ(mc::decodeRecipe(bytes.data(), bytes.size(), parsed), mc::RecipeError::None);
    EXPECT_EQ(parsed.name, source.name);
    EXPECT_EQ(parsed.version, source.version);
    EXPECT_EQ(parsed.mode, source.mode);
    EXPECT_FLOAT_EQ(parsed.target, source.target);
    EXPECT_FLOAT_EQ(parsed.kp, source.kp);
    EXPECT_FLOAT_EQ(parsed.ki, source.ki);
    EXPECT_FLOAT_EQ(parsed.kd, source.kd);
}

TEST(Recipe, LongestNameStillRoundTrips) {
    // 边界上的一格：正好用满 kMaxRecipeNameBytes 时必须合法，
    // 再多一个字节才该被拒。这类"差一"只有刻意测才会被发现。
    mc::Recipe r = makeValid();
    r.name.assign(mc::kMaxRecipeNameBytes, 'x');

    const std::vector<std::uint8_t> bytes = mc::encodeRecipe(r);
    EXPECT_EQ(bytes.size(), mc::kMaxRecipeBytes);
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::None);

    mc::Recipe parsed;
    ASSERT_EQ(mc::decodeRecipe(bytes.data(), bytes.size(), parsed), mc::RecipeError::None);
    EXPECT_EQ(parsed.name, r.name);
}

// ---------------------------------------------------------------------------
// 校验和
// ---------------------------------------------------------------------------

TEST(Recipe, ChecksumMatchesTheIndependentBitwiseCrc) {
    // 用逐位实现（crc16Bitwise）当独立参照：查表实现若被改坏，
    // "自己和自己比"是发现不了的 —— 这与协议层用的是同一个手法。
    const mc::Recipe r = makeValid();
    const std::vector<std::uint8_t> bytes = mc::encodeRecipe(r);
    EXPECT_EQ(mc::recipeChecksum(r), mc::crc16Bitwise(bytes.data(), bytes.size()));
}

TEST(Recipe, ChecksumCoversEveryFieldIncludingTheNameAndVersion) {
    const mc::Recipe base = makeValid();
    const std::uint16_t reference = mc::recipeChecksum(base);

    // 名称也在校验范围内：操作员正是靠名字选配方的，
    // 若名字改了校验和不变，就等于"名字随便改都验得过"。
    mc::Recipe renamed = base;
    renamed.name = "WAFER-A2";
    EXPECT_NE(mc::recipeChecksum(renamed), reference);

    mc::Recipe reversioned = base;
    reversioned.version = 4U;
    EXPECT_NE(mc::recipeChecksum(reversioned), reference);

    mc::Recipe remoded = base;
    remoded.mode = 2U;
    EXPECT_NE(mc::recipeChecksum(remoded), reference);

    mc::Recipe retargeted = base;
    retargeted.target = 201.0F;
    EXPECT_NE(mc::recipeChecksum(retargeted), reference);

    mc::Recipe regained = base;
    regained.kp = 1.7F;
    EXPECT_NE(mc::recipeChecksum(regained), reference);
    regained = base;
    regained.ki = 0.5F;
    EXPECT_NE(mc::recipeChecksum(regained), reference);
    regained = base;
    regained.kd = 0.03F;
    EXPECT_NE(mc::recipeChecksum(regained), reference);

    // 反向对照：同一份配方算两次必须同值，否则上面那些"不等"证明不了任何事。
    EXPECT_EQ(mc::recipeChecksum(makeValid()), reference);
}

// ---------------------------------------------------------------------------
// 结构层拒绝
// ---------------------------------------------------------------------------

TEST(Recipe, EveryTruncatedLengthIsRejected) {
    const std::vector<std::uint8_t> bytes = mc::encodeRecipe(makeValid());
    for (std::size_t len = 0U; len < bytes.size(); ++len) {
        mc::Recipe parsed;
        EXPECT_EQ(mc::decodeRecipe(bytes.data(), len, parsed), mc::RecipeError::Truncated)
            << "长度 " << len << " 应当被拒";
    }
}

TEST(Recipe, TrailingBytesAreRejected) {
    std::vector<std::uint8_t> bytes = mc::encodeRecipe(makeValid());
    bytes.push_back(0x00U);
    mc::Recipe parsed;
    EXPECT_EQ(mc::decodeRecipe(bytes.data(), bytes.size(), parsed), mc::RecipeError::TrailingBytes);
}

TEST(Recipe, AnImpossibleNameLengthIsReportedAsSuchRatherThanTruncation) {
    // 名称长度字段声明 200 字节：真原因是"这个长度本格式不允许"，
    // 报 Truncated 会把操作员指向"文件是不是没传完"，方向是错的。
    const std::vector<std::uint8_t> bytes = {0xC8U, 0x00U, 'a', 'b'};
    mc::Recipe parsed;
    EXPECT_EQ(mc::decodeRecipe(bytes.data(), bytes.size(), parsed), mc::RecipeError::NameTooLong);
}

TEST(Recipe, FailedDecodeLeavesTheCallersRecipeUntouched) {
    mc::Recipe keeper = makeValid();
    const mc::Recipe before = keeper;

    const std::vector<std::uint8_t> shortBytes = {0x02U, 0x00U, 'R', '1'};
    ASSERT_NE(mc::decodeRecipe(shortBytes.data(), shortBytes.size(), keeper), mc::RecipeError::None);

    EXPECT_EQ(keeper.name, before.name);
    EXPECT_EQ(keeper.version, before.version);
    EXPECT_EQ(keeper.mode, before.mode);
    EXPECT_FLOAT_EQ(keeper.target, before.target);
    EXPECT_FLOAT_EQ(keeper.kp, before.kp);
}

// ---------------------------------------------------------------------------
// 语义层拒绝
// ---------------------------------------------------------------------------

TEST(Recipe, ValidRecipesProduceNoError) {
    EXPECT_EQ(mc::validateRecipe(makeValid()), mc::RecipeError::None);

    mc::Recipe stopMode;
    stopMode.name = "STOP";
    stopMode.version = 1U;
    stopMode.mode = 0U;      // 停止控制
    stopMode.target = 0.0F;
    EXPECT_EQ(mc::validateRecipe(stopMode), mc::RecipeError::None);

    mc::Recipe negativeZero = stopMode;
    negativeZero.target = -0.0F;
    EXPECT_EQ(mc::validateRecipe(negativeZero), mc::RecipeError::None)
        << "-0.0 在语义上就是零，不该被当成非零目标拒掉";
}

TEST(Recipe, EmptyNameIsRejected) {
    mc::Recipe r = makeValid();
    r.name.clear();
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::EmptyName);
}

TEST(Recipe, NameOverTheLimitIsRejected) {
    mc::Recipe r = makeValid();
    r.name.assign(mc::kMaxRecipeNameBytes + 1U, 'x');
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::NameTooLong);
}

TEST(Recipe, VersionZeroIsRejectedBecauseItOnlyMeansUnset) {
    mc::Recipe r = makeValid();
    r.version = 0U;
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::BadVersion);
}

TEST(Recipe, ModeBeyondTheRunModeRangeIsRejected) {
    mc::Recipe r = makeValid();
    r.mode = mc::kMaxRunMode + 1U;
    r.target = 1.0F;
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::BadMode);
}

TEST(Recipe, TargetBoundFollowsTheModeBecauseTheUnitsDiffer) {
    // 同一个数值在速度环里是"度每秒"、在位置环里是"度"，两条量纲的合理上界
    // 差着五个数量级。用一条界去卡两种模式，必然有一边是错的。
    mc::Recipe velocity = makeValid();          // mode = 1
    velocity.target = mc::kSanityMaxAbsVelocityDegPerSec;
    EXPECT_EQ(mc::validateRecipe(velocity), mc::RecipeError::None);
    velocity.target = mc::kSanityMaxAbsVelocityDegPerSec * 2.0F;
    EXPECT_EQ(mc::validateRecipe(velocity), mc::RecipeError::TargetOutOfRange);

    mc::Recipe position = makeValid();
    position.mode = 2U;
    position.target = mc::kSanityMaxAbsPositionDeg;
    EXPECT_EQ(mc::validateRecipe(position), mc::RecipeError::None);
    position.target = mc::kSanityMaxAbsPositionDeg * 2.0F;
    EXPECT_EQ(mc::validateRecipe(position), mc::RecipeError::TargetOutOfRange);

    // 反向对照：位置环的上界放到速度环里必须**也**被拒（否则说明两条界写反了）。
    velocity.target = mc::kSanityMaxAbsPositionDeg;
    EXPECT_EQ(mc::validateRecipe(velocity), mc::RecipeError::TargetOutOfRange);
}

TEST(Recipe, StopModeCarryingANonZeroTargetIsRejectedAsSelfContradictory) {
    mc::Recipe r = makeValid();
    r.mode = 0U;
    r.target = 200.0F;
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::TargetMustBeZero);
}

TEST(Recipe, NonFiniteTargetIsReportedBeforeTheZeroCheck) {
    // 顺序有意义：NaN 与 0 比较恒为"不等"，若先判"必须为零"，
    // 一个 NaN 目标会被说成"停止控制不该带目标值"，把真正的原因盖掉。
    mc::Recipe r = makeValid();
    r.mode = 0U;
    r.target = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::TargetNotFinite);

    r.target = std::numeric_limits<float>::infinity();
    EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::TargetNotFinite);
}

TEST(Recipe, AllThreeGainsShareTheSameRule) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float tooLarge = mc::kSanityMaxPidGain * 2.0F;

    for (int which = 0; which < 3; ++which) {
        const auto assign = [&](mc::Recipe& r, float v) {
            if (which == 0) {
                r.kp = v;
            } else if (which == 1) {
                r.ki = v;
            } else {
                r.kd = v;
            }
        };

        mc::Recipe r = makeValid();
        assign(r, nan);
        EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::GainNotFinite) << "第 " << which << " 个增益";

        r = makeValid();
        assign(r, -1.0F);
        EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::GainNegative) << "第 " << which << " 个增益";

        r = makeValid();
        assign(r, tooLarge);
        EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::GainTooLarge) << "第 " << which << " 个增益";

        // 边界：正好等于上界必须放行，否则"上限"就成了"上限减一"。
        r = makeValid();
        assign(r, mc::kSanityMaxPidGain);
        EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::None) << "第 " << which << " 个增益";

        r = makeValid();
        assign(r, 0.0F);
        EXPECT_EQ(mc::validateRecipe(r), mc::RecipeError::None) << "第 " << which << " 个增益";
    }
}

// ---------------------------------------------------------------------------
// 回读载荷（ParamState）
//
// 配方下装的最后一步会把设备**存着的**参数读回来逐字段核对，所以这个载荷
// 必须能被独立验证：它坏了的话，"回读一致"这句话就没有意义了。
// ---------------------------------------------------------------------------

TEST(ParamState, RoundTripPreservesEveryField) {
    mc::ParamState state;
    state.mode = 1U;
    state.target = 200.0F;
    state.kp = 1.6F;
    state.ki = 0.4F;
    state.kd = 0.02F;
    // 软限位是 v2 长出来的那一组。**开着**测：关着的时候两个数值不参与判定，
    // 万一编解码把 min/max 弄丢了也看不出来 —— 而"关掉再打开"时它们要还在。
    state.limits.enabled = true;
    state.limits.minDeg = -120.0F;
    state.limits.maxDeg = 120.0F;

    const std::vector<std::uint8_t> bytes = mc::encodeParamState(state);
    EXPECT_EQ(bytes.size(), mc::kParamStateSize);

    mc::ParamState parsed;
    ASSERT_EQ(mc::decodeParamState(bytes.data(), bytes.size(), parsed), mc::PayloadError::None);
    EXPECT_EQ(parsed.mode, state.mode);
    EXPECT_FLOAT_EQ(parsed.target, state.target);
    EXPECT_FLOAT_EQ(parsed.kp, state.kp);
    EXPECT_FLOAT_EQ(parsed.ki, state.ki);
    EXPECT_FLOAT_EQ(parsed.kd, state.kd);
    EXPECT_EQ(parsed.limits.enabled, state.limits.enabled);
    EXPECT_FLOAT_EQ(parsed.limits.minDeg, state.limits.minDeg);
    EXPECT_FLOAT_EQ(parsed.limits.maxDeg, state.limits.maxDeg);
}

TEST(ParamState, LengthMustMatchExactly) {
    const std::vector<std::uint8_t> bytes = mc::encodeParamState(mc::ParamState{});
    mc::ParamState parsed;
    for (std::size_t len = 0U; len < bytes.size(); ++len) {
        EXPECT_EQ(mc::decodeParamState(bytes.data(), len, parsed), mc::PayloadError::LengthMismatch)
            << "长度 " << len;
    }
    EXPECT_EQ(mc::decodeParamState(bytes.data(), bytes.size() + 1U, parsed),
              mc::PayloadError::LengthMismatch);
}

TEST(ParamState, NonsenseValuesAreRejectedSoTheyCannotBeMistakenForAValidReadBack) {
    mc::ParamState state;
    state.mode = 1U;
    state.target = 200.0F;
    state.kp = 1.6F;
    state.ki = 0.4F;
    state.kd = 0.02F;
    EXPECT_TRUE(mc::isParamStateValid(state));

    state.mode = mc::kMaxRunMode + 1U;
    EXPECT_FALSE(mc::isParamStateValid(state));

    state.mode = 1U;
    state.target = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(mc::isParamStateValid(state));

    state.target = 200.0F;
    state.kp = -1.0F;
    EXPECT_FALSE(mc::isParamStateValid(state));

    state.kp = mc::kSanityMaxPidGain * 2.0F;
    EXPECT_FALSE(mc::isParamStateValid(state));

    // 反向对照：上界本身必须放行，否则"上限"就成了"上限减一"。
    state.kp = mc::kSanityMaxPidGain;
    EXPECT_TRUE(mc::isParamStateValid(state));

    // 回读里捎带的软限位同样要过语义校验：一组 min >= max 的限位画在界面上
    // 会是一条空区间 —— 它看起来像"限位已设置"，而实际上什么位置都不在范围内。
    state.limits.enabled = true;
    state.limits.minDeg = 100.0F;
    state.limits.maxDeg = -100.0F;
    EXPECT_FALSE(mc::isParamStateValid(state));

    state.limits.minDeg = -100.0F;
    state.limits.maxDeg = 100.0F;
    EXPECT_TRUE(mc::isParamStateValid(state));

    state.limits.maxDeg = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(mc::isParamStateValid(state));
}

TEST(Recipe, EveryErrorCodeHasAToken) {
    // 漏掉一个 case 会让界面显示出一个 "?"，而 "?" 没法指导任何人改配方。
    for (int i = 0; i <= 12; ++i) {
        const char* name = mc::recipeErrorName(static_cast<mc::RecipeError>(i));
        ASSERT_NE(name, nullptr);
        EXPECT_STRNE(name, "?") << "错误码 " << i << " 没有名字";
    }
}
