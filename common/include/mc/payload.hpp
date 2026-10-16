#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 载荷编解码
//
// 关键设计：**逐字段显式序列化，不对结构体做 memcpy**。
// 编译器会在结构体里插 padding，且不同 ABI / 编译选项下 padding 可能不同 ——
// memcpy 一个结构体上线，等于把本机的 ABI 细节写进协议，两端迟早对不上。
// 因此每个类型都有一个与之对应的、写死的线长常量（kXxxSize），
// 并有 static_assert(sizeof(X) >= kXxxSize) 提醒"内存布局 ≥ 线长"。
//
// 结构体 → 线（encode）与 线 → 结构体（decode）的职责边界：
//   decodeXxx 只做**结构性**校验（长度对不对）。长度不符 = 线格式错。
//   isValidXxx 做**语义**校验（有限性、取值范围）。数值荒谬 = 业务层要拒的。
// 分开的理由：NaN 的 4 个字节是完全合法的 float32，结构上没毛病，
// 但它是传感器坏掉/除零的信号，**绝不能画到曲线上**。
// ---------------------------------------------------------------------------

static_assert(sizeof(float) == 4U, "协议假设 float 为 32 位");
static_assert(std::numeric_limits<float>::is_iec559, "协议假设 float 遵循 IEEE-754");

// 语义校验的合理性上界。
// 注意：这是**协议层的防荒谬上界**，不是硬件限值 ——
// 真正的速度/位置软限位属于设备参数表，在阶段 4 定义。
inline constexpr float kSanityMaxAbsVelocityDegPerSec = 10000.0F;
inline constexpr float kSanityMaxAbsPositionDeg = 1.0e9F;

// PID 增益的防荒谬上界（同样不是整定限值，只是不让 1e30 这种值穿透到设备）。
inline constexpr float kSanityMaxPidGain = 1.0e6F;

// 允许的运行模式编号（与 SetMode::mode 对应）。
inline constexpr std::uint8_t kMaxRunMode = 2U;

enum class PayloadError : std::uint8_t {
    None = 0U,
    LengthMismatch,
};

// 编码是**全函数**：载荷都是定长的，不存在"编不出来"的情况。
// 因此 encodeXxx 返回定长缓冲（长度恒等于对应的 kXxxSize），不返回错误码 ——
// 不给调用方一个永远为 None 的错误分支去判断。
// 数值是否合理不属于编码的职责，那是 isValidXxx 的事。

// --- 遥测（设备 → 上位机，100 Hz 周期上报）-----------------------------------
struct Telemetry {
    std::uint32_t timestampMs = 0U;        // 设备侧时间戳（不是上位机收到时刻）
    float velocityDegPerSec = 0.0F;
    float positionDeg = 0.0F;
    float target = 0.0F;
    std::uint8_t state = 0U;               // 设备状态机枚举值（阶段 4 定义）
    std::uint8_t faultCode = 0U;           // 0 = 无故障
    std::uint16_t pwmOutput = 0U;          // 实际输出
};
inline constexpr std::size_t kTelemetrySize = 20U;

std::vector<std::uint8_t> encodeTelemetry(const Telemetry& value);
PayloadError decodeTelemetry(const std::uint8_t* data, std::size_t len, Telemetry& out) noexcept;
bool isTelemetryValid(const Telemetry& value) noexcept;

// --- PID 参数（上位机 → 设备）----------------------------------------------
struct SetPid {
    float kp = 0.0F;
    float ki = 0.0F;
    float kd = 0.0F;
};
inline constexpr std::size_t kSetPidSize = 12U;

std::vector<std::uint8_t> encodeSetPid(const SetPid& value);
PayloadError decodeSetPid(const std::uint8_t* data, std::size_t len, SetPid& out) noexcept;
bool isSetPidValid(const SetPid& value) noexcept;

// --- 目标值（上位机 → 设备）-------------------------------------------------
struct SetTarget {
    float target = 0.0F;                   // 速度环为度/秒，位置环为度
};
inline constexpr std::size_t kSetTargetSize = 4U;

std::vector<std::uint8_t> encodeSetTarget(const SetTarget& value);
PayloadError decodeSetTarget(const std::uint8_t* data, std::size_t len, SetTarget& out) noexcept;
bool isSetTargetValid(const SetTarget& value) noexcept;

// --- 运行模式（上位机 → 设备）-----------------------------------------------
struct SetMode {
    std::uint8_t mode = 0U;                // 0 = 停止控制，1 = 速度环，2 = 位置环
};
inline constexpr std::size_t kSetModeSize = 1U;

std::vector<std::uint8_t> encodeSetMode(const SetMode& value);
PayloadError decodeSetMode(const std::uint8_t* data, std::size_t len, SetMode& out) noexcept;
bool isSetModeValid(const SetMode& value) noexcept;

// --- 故障确认（上位机 → 设备）-----------------------------------------------
//
// 载荷只有一字节：操作员**确认的那个故障码**。要求它与设备当前锁存的故障一致，
// 不一致就拒（理由见 protocol.hpp 里 ClearFault 的说明：防止在旧屏幕上
// 把操作员没看见的新故障清掉）。
//
// 注意 `faultCode == 0`（None）不是合法确认：确认"没有故障"没有意义，
// 那只会是一处静默的空操作。所以 isValid 会拒它。
struct ClearFault {
    std::uint8_t faultCode = 0U;
};
inline constexpr std::size_t kClearFaultSize = 1U;

std::vector<std::uint8_t> encodeClearFault(const ClearFault& value);
PayloadError decodeClearFault(const std::uint8_t* data, std::size_t len, ClearFault& out) noexcept;
bool isClearFaultValid(const ClearFault& value) noexcept;

// --- 拒绝（双向）------------------------------------------------------------
enum class NackReason : std::uint8_t {
    Unspecified = 0U,
    UnknownType = 1U,
    IllegalState = 2U,      // 当前设备状态下不允许该命令（阶段 4 的状态机裁决）
    BadParameter = 3U,
    CrcError = 4U,
    Busy = 5U,
};

struct Nack {
    std::uint16_t rejectedSeq = 0U;
    NackReason reason = NackReason::Unspecified;
};
inline constexpr std::size_t kNackSize = 3U;

std::vector<std::uint8_t> encodeNack(const Nack& value);
PayloadError decodeNack(const std::uint8_t* data, std::size_t len, Nack& out) noexcept;
bool isNackValid(const Nack& value) noexcept;

}  // namespace mc
