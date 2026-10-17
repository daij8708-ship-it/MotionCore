#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// 帧格式常量（权威定义见 docs/PROTOCOL.md §3）
//
//  偏移  长度  字段
//   0     2    SOF      0xAA 0x55
//   2     1    VER      协议版本
//   3     1    TYPE     消息类型（bit7 = 1 表示应答帧）
//   4     2    SEQ      序列号（小端）
//   6     2    LEN      载荷长度（小端），上限 kMaxPayloadSize
//   8     LEN  PAYLOAD
//   8+LEN 2    CRC16    CRC-16/CCITT-FALSE，覆盖 VER..PAYLOAD（不含 SOF）
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t kSof0 = 0xAAU;
inline constexpr std::uint8_t kSof1 = 0x55U;

// 协议版本。两端必须一致（不一致的帧按 UnsupportedVersion 拒）。
//
//   v1 → v2（P1-6）：PARAM_STATE 载荷增加了**软限位**一组字段（1 + 4 + 4 字节）。
//   v2 → v3（P1-6）：TELEMETRY 载荷增加 1 字节**状态位组**（目前只有
//        "本轮回零是否已建立零点"，见 payload.hpp 的 kTelemetryFlagHomingAttained）。
//
//   为什么走版本号而不是"两个字段凑合用"：这两个载荷都是定长，加字段就是改线格式；
//   而改线格式却不动版本号，等于让"版本号"这个字段从此不再表示任何东西 ——
//   下次真需要它挡一次不兼容时，就没有防线了。
//   遥测这一条的后果尤其直接：旧上位机收到长度不对的遥测，最"宽容"的做法是
//   猜着解析，而它猜出来的会是一条假的曲线。
inline constexpr std::uint8_t kProtocolVersion = 0x03U;

inline constexpr std::uint16_t kMaxPayloadSize = 1024U;

inline constexpr std::size_t kHeaderSize = 8U;
inline constexpr std::size_t kCrcSize = 2U;
inline constexpr std::size_t kMinFrameSize = kHeaderSize + kCrcSize;
inline constexpr std::size_t kMaxFrameSize = kHeaderSize + kMaxPayloadSize + kCrcSize;

// 帧内偏移
inline constexpr std::size_t kOffsetSof = 0U;
inline constexpr std::size_t kOffsetVersion = 2U;
inline constexpr std::size_t kOffsetType = 3U;
inline constexpr std::size_t kOffsetSeq = 4U;
inline constexpr std::size_t kOffsetLen = 6U;

// 应答帧的类型位
inline constexpr std::uint8_t kAckFlag = 0x80U;

// 小端读写（线序的一部分，因此放在协议头而不是某处实现细节里）
inline std::uint16_t readLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

inline void writeLe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
}

inline std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

// 浮点按 IEEE-754 单精度的位模式走线。两端都是 IEEE-754 平台这一点由
// payload.hpp 里的 static_assert 钉住 —— 不做假定，让它编译期就失败。
inline float readLeFloat(const std::uint8_t* p) noexcept {
    const std::uint32_t bits = readLe32(p);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void writeLeFloat(std::uint8_t* p, float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    writeLe32(p, bits);
}

// ---------------------------------------------------------------------------
// 消息类型
// ---------------------------------------------------------------------------
enum class MessageType : std::uint8_t {
    Hello = 0x01U,
    Heartbeat = 0x02U,

    SetMode = 0x10U,
    Start = 0x11U,
    Stop = 0x12U,      // 单向：无 ACK、不重传（docs/PROTOCOL.md §5）
    SetTarget = 0x13U,
    SetPid = 0x14U,
    // 人工确认故障、请求清除锁存。载荷里带**被确认的故障码**：
    // "确认"只有针对一个具体观察到的故障才有意义，否则它等于"把现在挂着的
    // 任何东西清掉"—— 屏幕停在旧故障上、设备又新出了另一个故障时，
    // 这一下就会把操作员**没看见**的那个故障悄悄清掉。
    ClearFault = 0x15U,

    // 读回设备**存着**的参数组（模式 / 目标 / 三个 PID 增益）。
    //
    // 为什么需要它：SET_* 的应答只说明"设备收到并通过了校验"，不说明"它存下来
    // 的就是我们要的那个数"。配方下装是"照一份存档的参数去配置设备"，配置完
    // 不核对一遍，就只剩"我相信它"这一种说法。
    //
    // 注意它与遥测里的 target **不是一回事**：遥测报的是"此刻在追什么"
    // （非 Running 时恒为 0，因为那时设备确实没在追任何值），而本条报的是
    // "被设成了多少"。两者在 Ready 下必然不同，正是这个差别让回读有意义。
    ReadParams = 0x16U,

    // 回零：设备以**给定的低速**朝零点走，到位之后把那里**定义成**零点。
    //
    // 为什么它必须在设备侧完成、而不是上位机用 SET_MODE + SET_TARGET + START 拼出来：
    // "零点"是设备坐标系的定义，它是设备侧的知识（行程开关在哪、编码器 Z 相在哪）。
    // 上位机拼出来的只是"走去 0 度"，而如果此刻的坐标系本来就是错的，
    // 那等于朝一个错误的原点走 —— 拼出来的回零看起来一模一样，含义完全不同。
    //
    // 载荷带速度与超时（见 payload.hpp 的 HomeParam）：回零速度是**每台机器**
    // 的事，写死在协议里等于替现场做决定。
    Home = 0x17U,

    // 软限位（位置行程限制）。设备**存下来并据此拦**，不是一条显示用的设置：
    // 上位机可以因为"它以为是这个值"而少发一条命令，但真正拦住电机的必须是设备。
    SetLimits = 0x18U,

    Telemetry = 0x20U,   // 周期上报：无 ACK，允许丢帧
    FaultEvent = 0x21U,
    Nack = 0x7FU,

    HelloAck = 0x81U,
    HeartbeatAck = 0x82U,
    SetModeAck = 0x90U,
    StartAck = 0x91U,
    SetTargetAck = 0x93U,
    SetPidAck = 0x94U,
    ClearFaultAck = 0x95U,
    // 应答携带**设备侧的实存值**（载荷见 payload.hpp 的 ParamState）。
    ReadParamsAck = 0x96U,
    // 应答只表示"回零**开始了**"：到位与否由**遥测里的
    // 「本轮回零是否已建立零点」位**报（见 payload.hpp 的 kTelemetryFlagHomingAttained），
    // 与 START 同一套分层（命令的结果是"接受了"，行为的结局在状态与状态位里）。
    //
    // ⚠️ 单看设备状态是**判不出来**的：被 STOP 打断的回零是 Home → Stopping → Ready，
    // 而回零本来是低速，那一拍的 |v| 常常已经在停机门限之内 —— Stopping 会在同一拍里
    // 过去，上位机采不到。所以"零点建立没有"这件事必须由设备用一个**持续**的位来说。
    HomeAck = 0x97U,
    SetLimitsAck = 0x98U,
};

// 已知类型返回其名称；未知类型返回 "UNKNOWN"。
const char* toString(MessageType type) noexcept;

// 该值是否是本协议定义过的类型。
bool isKnown(MessageType type) noexcept;

// 应答帧（bit7 置位且是已知的 Ack 类型）。
bool isAck(MessageType type) noexcept;

// 是否需要应答：HELLO / HEARTBEAT / SET_MODE / START / SET_TARGET / SET_PID / CLEAR_FAULT
// 为 true；STOP（ADR-004）、TELEMETRY（ADR-005）、FAULT_EVENT（周期通知，R6）与各类 ACK 为 false。
//
// CLEAR_FAULT 必须应答：它是**唯一**能清掉故障锁存的报文，"清没清掉"这件事
// 不能靠猜。没确认就显示"已清除"，等于把一次未生效的安全动作说成生效了。
bool requiresAck(MessageType type) noexcept;

// 是否允许在超时后自动重传：只有幂等命令才为 true。
// START 与 SET_TARGET 明确返回 false —— 运动指令不得盲目自动重试（ADR-003）。
bool isRetryable(MessageType type) noexcept;

// 是否是"期待应答、但丢了不算故障"的周期性探测帧（当前只有 HEARTBEAT）。
//
// 必须与"超时即 Fault"分开：运动指令超时是**安全事件**，心跳超时只是**漏了一拍**。
// 若把两者混为一谈，健康链路上丢一个心跳就会立刻报 Fault —— 而链路是否失效
// 本应由"连续丢 N 拍"累计判定（见 LinkMonitor）。
bool isPeriodicProbe(MessageType type) noexcept;

// 请求 → 对应 ACK 类型。无 ACK 的请求（如 Stop / Telemetry）返回 false，
// 由调用方显式处理，不使用"哨兵值"表示缺失。
bool tryAckOf(MessageType request, MessageType& out) noexcept;

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------
struct Frame {
    std::uint8_t version = kProtocolVersion;
    MessageType type = MessageType::Nack;
    std::uint16_t seq = 0;
    std::vector<std::uint8_t> payload;
};

// ---------------------------------------------------------------------------
// 编解码错误：显式枚举，不用"空返回值"表示失败（失败原因必须可分辨）
// ---------------------------------------------------------------------------
enum class EncodeError : std::uint8_t {
    None = 0U,
    PayloadTooLarge,
    UnsupportedVersion,
};

enum class DecodeError : std::uint8_t {
    None = 0U,
    BufferTooShort,
    BadSof,
    UnsupportedVersion,
    PayloadTooLarge,
    CrcMismatch,
};

struct EncodeResult {
    EncodeError error = EncodeError::None;
    std::vector<std::uint8_t> bytes;   // 仅当 error == None 时非空
    explicit operator bool() const noexcept { return error == EncodeError::None; }
};

struct DecodeResult {
    DecodeError error = DecodeError::None;
    Frame frame;                 // 仅当 error == None 时有效
    std::size_t consumed = 0;    // 仅当 error == None 时有效：本帧占用的字节数
    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

}  // namespace mc
