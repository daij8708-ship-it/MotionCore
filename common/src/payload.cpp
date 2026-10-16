#include "mc/payload.hpp"

#include <cmath>

namespace mc {
namespace {

bool withinAbs(float value, float bound) noexcept {
    return std::isfinite(value) && value >= -bound && value <= bound;
}

bool inGainRange(float gain) noexcept {
    return std::isfinite(gain) && gain >= 0.0F && gain <= kSanityMaxPidGain;
}

}  // namespace

// --- 遥测 -------------------------------------------------------------------

std::vector<std::uint8_t> encodeTelemetry(const Telemetry& value) {
    std::vector<std::uint8_t> out(kTelemetrySize, 0U);
    writeLe32(&out[0], value.timestampMs);
    writeLeFloat(&out[4], value.velocityDegPerSec);
    writeLeFloat(&out[8], value.positionDeg);
    writeLeFloat(&out[12], value.target);
    out[16] = value.state;
    out[17] = value.faultCode;
    writeLe16(&out[18], value.pwmOutput);
    return out;
}

PayloadError decodeTelemetry(const std::uint8_t* data, std::size_t len, Telemetry& out) noexcept {
    if (data == nullptr || len != kTelemetrySize) {
        return PayloadError::LengthMismatch;
    }

    Telemetry parsed;
    parsed.timestampMs = readLe32(&data[0]);
    parsed.velocityDegPerSec = readLeFloat(&data[4]);
    parsed.positionDeg = readLeFloat(&data[8]);
    parsed.target = readLeFloat(&data[12]);
    parsed.state = data[16];
    parsed.faultCode = data[17];
    parsed.pwmOutput = readLe16(&data[18]);

    out = parsed;   // 全部字段解析完才写出参，失败时不留半成品
    return PayloadError::None;
}

bool isTelemetryValid(const Telemetry& value) noexcept {
    // velocity / position 有明确的量纲上界；target 的量纲取决于当前模式
    // （速度环是度/秒、位置环是度），这里只要求它有限 ——
    // 量程检查留给知道模式的一方，避免用一个错的上界去卡另一半。
    return withinAbs(value.velocityDegPerSec, kSanityMaxAbsVelocityDegPerSec) &&
           withinAbs(value.positionDeg, kSanityMaxAbsPositionDeg) &&
           std::isfinite(value.target);
}

// --- PID 参数 ---------------------------------------------------------------

std::vector<std::uint8_t> encodeSetPid(const SetPid& value) {
    std::vector<std::uint8_t> out(kSetPidSize, 0U);
    writeLeFloat(&out[0], value.kp);
    writeLeFloat(&out[4], value.ki);
    writeLeFloat(&out[8], value.kd);
    return out;
}

PayloadError decodeSetPid(const std::uint8_t* data, std::size_t len, SetPid& out) noexcept {
    if (data == nullptr || len != kSetPidSize) {
        return PayloadError::LengthMismatch;
    }

    SetPid parsed;
    parsed.kp = readLeFloat(&data[0]);
    parsed.ki = readLeFloat(&data[4]);
    parsed.kd = readLeFloat(&data[8]);

    out = parsed;
    return PayloadError::None;
}

bool isSetPidValid(const SetPid& value) noexcept {
    return inGainRange(value.kp) && inGainRange(value.ki) && inGainRange(value.kd);
}

// --- 目标值 -----------------------------------------------------------------

std::vector<std::uint8_t> encodeSetTarget(const SetTarget& value) {
    std::vector<std::uint8_t> out(kSetTargetSize, 0U);
    writeLeFloat(&out[0], value.target);
    return out;
}

PayloadError decodeSetTarget(const std::uint8_t* data, std::size_t len, SetTarget& out) noexcept {
    if (data == nullptr || len != kSetTargetSize) {
        return PayloadError::LengthMismatch;
    }

    SetTarget parsed;
    parsed.target = readLeFloat(&data[0]);

    out = parsed;
    return PayloadError::None;
}

bool isSetTargetValid(const SetTarget& value) noexcept {
    // 量纲随模式变化，这里只挡 NaN / Inf（它们必然是无意义的命令）。
    return std::isfinite(value.target);
}

// --- 运行模式 ---------------------------------------------------------------

std::vector<std::uint8_t> encodeSetMode(const SetMode& value) {
    return std::vector<std::uint8_t>(1U, value.mode);
}

PayloadError decodeSetMode(const std::uint8_t* data, std::size_t len, SetMode& out) noexcept {
    if (data == nullptr || len != kSetModeSize) {
        return PayloadError::LengthMismatch;
    }

    SetMode parsed;
    parsed.mode = data[0];

    out = parsed;
    return PayloadError::None;
}

bool isSetModeValid(const SetMode& value) noexcept {
    return value.mode <= kMaxRunMode;
}

// --- 故障确认 ---------------------------------------------------------------

std::vector<std::uint8_t> encodeClearFault(const ClearFault& value) {
    return std::vector<std::uint8_t>(1U, value.faultCode);
}

PayloadError decodeClearFault(const std::uint8_t* data, std::size_t len, ClearFault& out) noexcept {
    if (data == nullptr || len != kClearFaultSize) {
        return PayloadError::LengthMismatch;
    }

    ClearFault parsed;
    parsed.faultCode = data[0];

    out = parsed;
    return PayloadError::None;
}

bool isClearFaultValid(const ClearFault& value) noexcept {
    // 0 = 遥测字段里的"无故障"（见 Telemetry::faultCode）。
    // 确认一个不存在的故障是无意义的空操作 —— 若放行，界面会显示"已确认清除"，
    // 而实际上什么都没发生。
    //
    // 上界不在这里卡：本层不认识设备的故障码枚举，且**码值对不对由设备比对**
    // （它要确认的正是"当前锁存的那个"），一个不存在的码在那边永远对不上，
    // 会以 BadParameter 被拒。在这里再抄一份枚举只会多一处会过期的副本。
    return value.faultCode != 0U;
}

// --- 拒绝 -------------------------------------------------------------------

std::vector<std::uint8_t> encodeNack(const Nack& value) {
    std::vector<std::uint8_t> out(kNackSize, 0U);
    writeLe16(&out[0], value.rejectedSeq);
    out[2] = static_cast<std::uint8_t>(value.reason);
    return out;
}

PayloadError decodeNack(const std::uint8_t* data, std::size_t len, Nack& out) noexcept {
    if (data == nullptr || len != kNackSize) {
        return PayloadError::LengthMismatch;
    }

    Nack parsed;
    parsed.rejectedSeq = readLe16(&data[0]);
    parsed.reason = static_cast<NackReason>(data[2]);

    out = parsed;
    return PayloadError::None;
}

bool isNackValid(const Nack& value) noexcept {
    switch (value.reason) {
        case NackReason::Unspecified:
        case NackReason::UnknownType:
        case NackReason::IllegalState:
        case NackReason::BadParameter:
        case NackReason::CrcError:
        case NackReason::Busy:
            return true;
    }
    return false;
}

}  // namespace mc
