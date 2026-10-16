#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "mc/payload.hpp"

namespace {

using mc::PayloadError;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// 内存布局不参与线格式：线长由 kXxxSize 写死，与 sizeof 无关。
// 这里把关系写死，任何人把 sizeof 当成线长用都会立刻撞上。
static_assert(sizeof(mc::Telemetry) >= mc::kTelemetrySize);
static_assert(sizeof(mc::SetPid) >= mc::kSetPidSize);
static_assert(sizeof(mc::ClearFault) >= mc::kClearFaultSize);
static_assert(sizeof(mc::Nack) > mc::kNackSize,
              "Nack 的内存布局带 padding（u16 + u8 会被补齐到 4 字节），"
              "而线长是 3 —— 这正是不能对结构体做 memcpy 的实例");

}  // namespace

// ---------------------------------------------------------------------------
// 线格式
// ---------------------------------------------------------------------------

TEST(Payload, FloatIsEncodedAsIeee754LittleEndian) {
    // 1.0f  = 0x3F800000 → 小端 00 00 80 3F
    const std::vector<std::uint8_t> one = mc::encodeSetTarget(mc::SetTarget{1.0F});
    ASSERT_EQ(one.size(), std::size_t{4});
    EXPECT_EQ(one[0], 0x00U);
    EXPECT_EQ(one[1], 0x00U);
    EXPECT_EQ(one[2], 0x80U);
    EXPECT_EQ(one[3], 0x3FU);

    // -2.0f = 0xC0000000 → 小端 00 00 00 C0
    const std::vector<std::uint8_t> minusTwo = mc::encodeSetTarget(mc::SetTarget{-2.0F});
    ASSERT_EQ(minusTwo.size(), std::size_t{4});
    EXPECT_EQ(minusTwo[0], 0x00U);
    EXPECT_EQ(minusTwo[1], 0x00U);
    EXPECT_EQ(minusTwo[2], 0x00U);
    EXPECT_EQ(minusTwo[3], 0xC0U);
}

TEST(Payload, TelemetryByteLayoutIsPinned) {
    mc::Telemetry value;
    value.timestampMs = 0x11223344U;
    value.velocityDegPerSec = 1.0F;      // 0x3F800000
    value.positionDeg = -2.0F;           // 0xC0000000
    value.target = 0.0F;
    value.state = 0x03U;
    value.faultCode = 0x07U;
    value.pwmOutput = 0xAABBU;

    const std::vector<std::uint8_t> bytes = mc::encodeTelemetry(value);
    ASSERT_EQ(bytes.size(), mc::kTelemetrySize);

    EXPECT_EQ(bytes[0], 0x44U);   // timestamp 小端
    EXPECT_EQ(bytes[1], 0x33U);
    EXPECT_EQ(bytes[2], 0x22U);
    EXPECT_EQ(bytes[3], 0x11U);

    EXPECT_EQ(bytes[4], 0x00U);   // velocity = 1.0f
    EXPECT_EQ(bytes[5], 0x00U);
    EXPECT_EQ(bytes[6], 0x80U);
    EXPECT_EQ(bytes[7], 0x3FU);

    EXPECT_EQ(bytes[8], 0x00U);   // position = -2.0f
    EXPECT_EQ(bytes[9], 0x00U);
    EXPECT_EQ(bytes[10], 0x00U);
    EXPECT_EQ(bytes[11], 0xC0U);

    EXPECT_EQ(bytes[12], 0x00U);  // target = 0.0f
    EXPECT_EQ(bytes[13], 0x00U);
    EXPECT_EQ(bytes[14], 0x00U);
    EXPECT_EQ(bytes[15], 0x00U);

    EXPECT_EQ(bytes[16], 0x03U);  // state
    EXPECT_EQ(bytes[17], 0x07U);  // faultCode
    EXPECT_EQ(bytes[18], 0xBBU);  // pwmOutput 小端
    EXPECT_EQ(bytes[19], 0xAAU);
}

TEST(Payload, EncodedSizesMatchDeclaredWireSizes) {
    EXPECT_EQ(mc::encodeTelemetry(mc::Telemetry{}).size(), mc::kTelemetrySize);
    EXPECT_EQ(mc::encodeSetPid(mc::SetPid{}).size(), mc::kSetPidSize);
    EXPECT_EQ(mc::encodeSetTarget(mc::SetTarget{}).size(), mc::kSetTargetSize);
    EXPECT_EQ(mc::encodeSetMode(mc::SetMode{}).size(), mc::kSetModeSize);
    EXPECT_EQ(mc::encodeNack(mc::Nack{}).size(), mc::kNackSize);
}

// ---------------------------------------------------------------------------
// 往返
// ---------------------------------------------------------------------------

TEST(Payload, TelemetryRoundTrip) {
    mc::Telemetry value;
    value.timestampMs = 4294967295U;
    value.velocityDegPerSec = -384.25F;
    value.positionDeg = 123456.5F;
    value.target = 0.125F;
    value.state = 4U;
    value.faultCode = 9U;
    value.pwmOutput = 65535U;

    mc::Telemetry decoded;
    ASSERT_EQ(mc::decodeTelemetry(mc::encodeTelemetry(value).data(), mc::kTelemetrySize, decoded),
              PayloadError::None);
    EXPECT_EQ(decoded.timestampMs, value.timestampMs);
    EXPECT_FLOAT_EQ(decoded.velocityDegPerSec, value.velocityDegPerSec);
    EXPECT_FLOAT_EQ(decoded.positionDeg, value.positionDeg);
    EXPECT_FLOAT_EQ(decoded.target, value.target);
    EXPECT_EQ(decoded.state, value.state);
    EXPECT_EQ(decoded.faultCode, value.faultCode);
    EXPECT_EQ(decoded.pwmOutput, value.pwmOutput);
}

TEST(Payload, SetPidRoundTrip) {
    const mc::SetPid value{1.5F, 0.25F, 0.03125F};

    mc::SetPid decoded;
    ASSERT_EQ(mc::decodeSetPid(mc::encodeSetPid(value).data(), mc::kSetPidSize, decoded), PayloadError::None);
    EXPECT_FLOAT_EQ(decoded.kp, value.kp);
    EXPECT_FLOAT_EQ(decoded.ki, value.ki);
    EXPECT_FLOAT_EQ(decoded.kd, value.kd);
}

TEST(Payload, SetModeRoundTrip) {
    mc::SetMode decoded;
    const std::vector<std::uint8_t> bytes = mc::encodeSetMode(mc::SetMode{2U});
    ASSERT_EQ(mc::decodeSetMode(bytes.data(), bytes.size(), decoded), PayloadError::None);
    EXPECT_EQ(decoded.mode, 2U);
}

TEST(Payload, NackRoundTrip) {
    const mc::Nack value{0xBEEFU, mc::NackReason::IllegalState};

    mc::Nack decoded;
    ASSERT_EQ(mc::decodeNack(mc::encodeNack(value).data(), mc::kNackSize, decoded), PayloadError::None);
    EXPECT_EQ(decoded.rejectedSeq, value.rejectedSeq);
    EXPECT_EQ(decoded.reason, mc::NackReason::IllegalState);
}

// ---------------------------------------------------------------------------
// 结构性校验（长度）
// ---------------------------------------------------------------------------

TEST(Payload, DecodeRejectsEveryWrongLength) {
    const std::vector<std::uint8_t> telemetry = mc::encodeTelemetry(mc::Telemetry{});
    mc::Telemetry out;

    for (std::size_t len = 0U; len <= mc::kTelemetrySize + 1U; ++len) {
        if (len == mc::kTelemetrySize) {
            continue;
        }
        EXPECT_EQ(mc::decodeTelemetry(telemetry.data(), len, out), PayloadError::LengthMismatch)
            << "len=" << len;
    }

    // 逐类型再各测一次"短一字节 / 长一字节"，避免只覆盖了 Telemetry。
    const std::vector<std::uint8_t> pid = mc::encodeSetPid(mc::SetPid{});
    mc::SetPid pidOut;
    EXPECT_EQ(mc::decodeSetPid(pid.data(), mc::kSetPidSize - 1U, pidOut), PayloadError::LengthMismatch);
    EXPECT_EQ(mc::decodeSetPid(pid.data(), mc::kSetPidSize + 1U, pidOut), PayloadError::LengthMismatch);

    const std::vector<std::uint8_t> target = mc::encodeSetTarget(mc::SetTarget{});
    mc::SetTarget targetOut;
    EXPECT_EQ(mc::decodeSetTarget(target.data(), mc::kSetTargetSize + 1U, targetOut), PayloadError::LengthMismatch);

    const std::vector<std::uint8_t> mode = mc::encodeSetMode(mc::SetMode{});
    mc::SetMode modeOut;
    EXPECT_EQ(mc::decodeSetMode(mode.data(), 0U, modeOut), PayloadError::LengthMismatch);

    const std::vector<std::uint8_t> nack = mc::encodeNack(mc::Nack{});
    mc::Nack nackOut;
    EXPECT_EQ(mc::decodeNack(nack.data(), mc::kNackSize - 1U, nackOut), PayloadError::LengthMismatch);
}

TEST(Payload, DecodeRejectsNullPointer) {
    mc::Telemetry out;
    EXPECT_EQ(mc::decodeTelemetry(nullptr, mc::kTelemetrySize, out), PayloadError::LengthMismatch);
}

// 解码失败时不得给调用方留下半成品。
TEST(Payload, FailedDecodeLeavesOutputUntouched) {
    mc::Telemetry out;
    out.timestampMs = 0xDEADBEEFU;
    out.velocityDegPerSec = 42.0F;
    out.state = 7U;

    const std::vector<std::uint8_t> bytes = mc::encodeTelemetry(mc::Telemetry{});
    ASSERT_EQ(mc::decodeTelemetry(bytes.data(), 5U, out), PayloadError::LengthMismatch);

    EXPECT_EQ(out.timestampMs, 0xDEADBEEFU);
    EXPECT_FLOAT_EQ(out.velocityDegPerSec, 42.0F);
    EXPECT_EQ(out.state, 7U);
}

// ---------------------------------------------------------------------------
// 语义校验（有限性与取值范围）—— 与结构性校验刻意分开
// ---------------------------------------------------------------------------

// 核心用例：NaN 的 4 个字节是完全合法的 float32，结构上没毛病，
// 但它是传感器坏掉/除零的信号，绝不能被当成正常数据画到曲线上。
TEST(Payload, NonFiniteTelemetryDecodesButFailsValidation) {
    mc::Telemetry nanVelocity;
    nanVelocity.velocityDegPerSec = kNaN;

    const std::vector<std::uint8_t> bytes = mc::encodeTelemetry(nanVelocity);

    mc::Telemetry decoded;
    EXPECT_EQ(mc::decodeTelemetry(bytes.data(), bytes.size(), decoded), PayloadError::None)
        << "结构性校验不该拦 NaN —— 长度是对的";
    EXPECT_FALSE(mc::isTelemetryValid(decoded)) << "语义校验必须拦住 NaN";

    // 正对照：同一探针在正常值上给出"有效"。
    mc::Telemetry good;
    good.velocityDegPerSec = -384.0F;
    good.positionDeg = 720.0F;
    EXPECT_TRUE(mc::isTelemetryValid(good));
}

TEST(Payload, TelemetryRejectsNonFinitePositionAndTarget) {
    mc::Telemetry value;
    EXPECT_TRUE(mc::isTelemetryValid(value));

    value.positionDeg = kInf;
    EXPECT_FALSE(mc::isTelemetryValid(value));
    value.positionDeg = 0.0F;

    value.target = kNaN;
    EXPECT_FALSE(mc::isTelemetryValid(value));
}

TEST(Payload, TelemetryRejectsAbsurdVelocityAndPosition) {
    mc::Telemetry value;
    value.velocityDegPerSec = mc::kSanityMaxAbsVelocityDegPerSec + 1.0F;
    EXPECT_FALSE(mc::isTelemetryValid(value));
    value.velocityDegPerSec = -mc::kSanityMaxAbsVelocityDegPerSec;
    EXPECT_TRUE(mc::isTelemetryValid(value));

    value.velocityDegPerSec = 0.0F;
    value.positionDeg = mc::kSanityMaxAbsPositionDeg * 2.0F;
    EXPECT_FALSE(mc::isTelemetryValid(value));
}

TEST(Payload, SetPidRejectsNegativeAndNonFiniteGains) {
    EXPECT_TRUE(mc::isSetPidValid(mc::SetPid{0.0F, 0.0F, 0.0F}));
    EXPECT_TRUE(mc::isSetPidValid(mc::SetPid{1.0F, 2.0F, 3.0F}));

    EXPECT_FALSE(mc::isSetPidValid(mc::SetPid{-0.001F, 0.0F, 0.0F}));
    EXPECT_FALSE(mc::isSetPidValid(mc::SetPid{0.0F, kNaN, 0.0F}));
    EXPECT_FALSE(mc::isSetPidValid(mc::SetPid{0.0F, 0.0F, kInf}));
    EXPECT_FALSE(mc::isSetPidValid(mc::SetPid{mc::kSanityMaxPidGain * 2.0F, 0.0F, 0.0F}));
}

TEST(Payload, SetTargetRejectsOnlyNonFinite) {
    EXPECT_TRUE(mc::isSetTargetValid(mc::SetTarget{0.0F}));
    EXPECT_TRUE(mc::isSetTargetValid(mc::SetTarget{-99999.0F}));
    EXPECT_FALSE(mc::isSetTargetValid(mc::SetTarget{kNaN}));
    EXPECT_FALSE(mc::isSetTargetValid(mc::SetTarget{kInf}));
}

TEST(Payload, SetModeRejectsUnknownModeNumber) {
    EXPECT_TRUE(mc::isSetModeValid(mc::SetMode{0U}));
    EXPECT_TRUE(mc::isSetModeValid(mc::SetMode{mc::kMaxRunMode}));
    EXPECT_FALSE(mc::isSetModeValid(mc::SetMode{static_cast<std::uint8_t>(mc::kMaxRunMode + 1U)}));
    EXPECT_FALSE(mc::isSetModeValid(mc::SetMode{0xFFU}));
}

TEST(Payload, NackRejectsUnknownReasonValue) {
    EXPECT_TRUE(mc::isNackValid(mc::Nack{1U, mc::NackReason::Busy}));
    EXPECT_FALSE(mc::isNackValid(mc::Nack{1U, static_cast<mc::NackReason>(200U)}));
}

TEST(Payload, ClearFaultCarriesTheAcknowledgedCodeByteForByte) {
    EXPECT_EQ(mc::encodeClearFault(mc::ClearFault{}).size(), mc::kClearFaultSize);

    const std::vector<std::uint8_t> bytes = mc::encodeClearFault(mc::ClearFault{4U});
    mc::ClearFault decoded;
    ASSERT_EQ(mc::decodeClearFault(bytes.data(), bytes.size(), decoded), PayloadError::None);
    EXPECT_EQ(decoded.faultCode, 4U);

    // 长度不符 = 线格式错（结构性），与"码值不对"（语义性）分开报。
    EXPECT_EQ(mc::decodeClearFault(bytes.data(), 0U, decoded), PayloadError::LengthMismatch);
    EXPECT_EQ(mc::decodeClearFault(bytes.data(), mc::kClearFaultSize + 1U, decoded),
              PayloadError::LengthMismatch);
    EXPECT_EQ(mc::decodeClearFault(nullptr, mc::kClearFaultSize, decoded),
              PayloadError::LengthMismatch);
}

TEST(Payload, ClearFaultRejectsTheNoFaultCode) {
    // 0 = "无故障"。确认一个不存在的故障是无意义的空操作，放行它
    // 只会让界面显示一句"已确认清除"而实际什么都没发生。
    EXPECT_FALSE(mc::isClearFaultValid(mc::ClearFault{0U}));
    EXPECT_TRUE(mc::isClearFaultValid(mc::ClearFault{1U}));

    // 上界不在这里卡：本层不认识设备的故障码枚举，码值对不对由设备比对
    // 当前锁存的那个码来决定 —— 不存在的码在那边永远对不上。
    EXPECT_TRUE(mc::isClearFaultValid(mc::ClearFault{200U}));
}
