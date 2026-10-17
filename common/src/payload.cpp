#include "mc/payload.hpp"

#include <algorithm>
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
    out[20] = value.statusFlags;
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
    // 位组原样收下：**不认识的位不清零、也不判错**（见 payload.hpp 的说明）。
    // 清零会让"设备说了一件我们还不懂的事"变成"设备什么都没说"。
    parsed.statusFlags = data[20];

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

// --- 参数回读 ---------------------------------------------------------------

std::vector<std::uint8_t> encodeParamState(const ParamState& value) {
    std::vector<std::uint8_t> out(kParamStateSize, 0U);
    out[0] = value.mode;
    writeLeFloat(&out[1], value.target);
    writeLeFloat(&out[5], value.kp);
    writeLeFloat(&out[9], value.ki);
    writeLeFloat(&out[13], value.kd);
    const std::vector<std::uint8_t> limits = encodeSoftLimits(value.limits);
    std::copy(limits.begin(), limits.end(), out.begin() + 17);
    return out;
}

PayloadError decodeParamState(const std::uint8_t* data, std::size_t len, ParamState& out) noexcept {
    if (data == nullptr || len != kParamStateSize) {
        return PayloadError::LengthMismatch;
    }

    ParamState parsed;
    parsed.mode = data[0];
    parsed.target = readLeFloat(&data[1]);
    parsed.kp = readLeFloat(&data[5]);
    parsed.ki = readLeFloat(&data[9]);
    parsed.kd = readLeFloat(&data[13]);
    // 软限位那一组的编解码复用同一对函数：两处各写一遍偏移，迟早写歪一个。
    if (decodeSoftLimits(&data[17], kSoftLimitsSize, parsed.limits) != PayloadError::None) {
        return PayloadError::LengthMismatch;
    }

    out = parsed;
    return PayloadError::None;
}

bool isParamStateValid(const ParamState& value) noexcept {
    if (value.mode > kMaxRunMode) {
        return false;
    }
    if (!std::isfinite(value.target)) {
        return false;
    }
    // 这里**刻意不按模式分量纲**上界。配方校验（mc::validateRecipe）判的是
    // "要下发的设定值合不合理"，必须严格；而这一条判的是"设备报回来的现值像不像话"，
    // 一个足够宽的防荒谬上界就够了。两处严厉程度不同，是因为它们管的事不同：
    // 拿宽的那个去挡下装会放过该拒的，拿严的那个去判回读会把正常值误判成坏数据。
    if (std::fabs(value.target) > kSanityMaxAbsPositionDeg) {
        return false;
    }
    return inGainRange(value.kp) && inGainRange(value.ki) && inGainRange(value.kd) &&
           isSoftLimitsValid(value.limits);
}

// --- 回零 -------------------------------------------------------------------

std::vector<std::uint8_t> encodeHomeParam(const HomeParam& value) {
    std::vector<std::uint8_t> out(kHomeParamSize, 0U);
    writeLeFloat(&out[0], value.speedDegPerSec);
    writeLe32(&out[4], value.timeoutMs);
    return out;
}

PayloadError decodeHomeParam(const std::uint8_t* data, std::size_t len, HomeParam& out) noexcept {
    if (data == nullptr || len != kHomeParamSize) {
        return PayloadError::LengthMismatch;
    }

    HomeParam parsed;
    parsed.speedDegPerSec = readLeFloat(&data[0]);
    parsed.timeoutMs = readLe32(&data[4]);

    out = parsed;
    return PayloadError::None;
}

bool isHomeParamValid(const HomeParam& value) noexcept {
    // 速度 0 不是"不用动"，而是一个**永远走不到**的回零：判据是"到位"，
    // 而速度为 0 时位置永远不变。这种参数在界面上看起来完全正常。
    if (!std::isfinite(value.speedDegPerSec) || value.speedDegPerSec <= 0.0F ||
        value.speedDegPerSec > kSanityMaxHomeSpeedDegPerSec) {
        return false;
    }
    // 超时 0 同理：没有时限的回零会在传感器坏掉时一直挂着，
    // 而"一直挂着"从界面上看和"还在走"分不出来。
    return value.timeoutMs > 0U && value.timeoutMs <= kMaxHomeTimeoutMs;
}

// --- 软限位 -----------------------------------------------------------------

std::vector<std::uint8_t> encodeSoftLimits(const SoftLimits& value) {
    std::vector<std::uint8_t> out(kSoftLimitsSize, 0U);
    out[0] = value.enabled ? 1U : 0U;
    writeLeFloat(&out[1], value.minDeg);
    writeLeFloat(&out[5], value.maxDeg);
    return out;
}

PayloadError decodeSoftLimits(const std::uint8_t* data, std::size_t len, SoftLimits& out) noexcept {
    if (data == nullptr || len != kSoftLimitsSize) {
        return PayloadError::LengthMismatch;
    }

    SoftLimits parsed;
    parsed.enabled = (data[0] != 0U);
    parsed.minDeg = readLeFloat(&data[1]);
    parsed.maxDeg = readLeFloat(&data[5]);

    out = parsed;
    return PayloadError::None;
}

bool isSoftLimitsValid(const SoftLimits& value) noexcept {
    if (!std::isfinite(value.minDeg) || !std::isfinite(value.maxDeg)) {
        return false;
    }
    if (std::fabs(value.minDeg) > kSanityMaxAbsPositionDeg ||
        std::fabs(value.maxDeg) > kSanityMaxAbsPositionDeg) {
        return false;
    }
    // 关掉时两个数值不参与任何判定，因此不要求它们构成一个区间：
    // "未配置"的自然表示就是两个 0。而**打开**这条命令总是带着两个数值一起来
    // （SET_LIMITS 的载荷是整组），所以不存在"存了一组荒唐值、哪天一点就中招"的路径 ——
    // 打开的那一刻会走同一次检查。
    if (!value.enabled) {
        return true;
    }
    return value.minDeg < value.maxDeg;
}

bool softLimitAllows(const SoftLimits& limits, float positionDeg) noexcept {
    if (!limits.enabled) {
        return true;
    }
    if (!std::isfinite(positionDeg)) {
        // 位置读不出来时**不算通过**：判不了的一律不放行，
        // 与联锁表里"一条判不了的规则不能算通过"同一条原则。
        return false;
    }
    return positionDeg >= limits.minDeg && positionDeg <= limits.maxDeg;
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
