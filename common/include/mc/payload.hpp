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

// 回零超时的上界（10 分钟）。它与 Wait 步的上限同口径：回零是**低速**动作，
// 慢是正常的；但"永远走不到"必须有个头 —— 否则设备会一直挂在那儿。
inline constexpr std::uint32_t kMaxHomeTimeoutMs = 10U * 60U * 1000U;

// 回零速度的防荒谬上界。它比速度环的上界小一个量级，因为回零是
// **对着一个机械零点撞过去**的动作：速度在这里不是性能，是风险。
inline constexpr float kSanityMaxHomeSpeedDegPerSec = 360.0F;

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
//
// 遥测里有一个**状态位组**（statusFlags）。为什么不塞进 state：state 是"设备在哪
// 一步"，位组回答的是"某个过程做完没有" —— 两件事的生命周期不同。把位硬塞进
// 状态枚举会让状态数随每加一个过程翻倍，而且状态之间本来就不该互相排斥。
//
// 目前只定义一位：**本轮回零是否已建立零点**（见下面的说明）。
// 未定义的位一律**忽略**（与 state / faultCode 一样不参与语义校验）：
// 将来设备多发一位，不该让整帧遥测在旧上位机那里被判成坏数据 —— 那是 100 Hz
// 的帧，判坏等于把整条曲线的数据源掐断。
inline constexpr std::uint8_t kTelemetryFlagHomingAttained = 0x01U;

struct Telemetry {
    std::uint32_t timestampMs = 0U;        // 设备侧时间戳（不是上位机收到时刻）
    float velocityDegPerSec = 0.0F;
    float positionDeg = 0.0F;
    float target = 0.0F;
    std::uint8_t state = 0U;               // 设备状态机枚举值（阶段 4 定义）
    std::uint8_t faultCode = 0U;           // 0 = 无故障
    std::uint16_t pwmOutput = 0U;          // 实际输出
    // 状态位组（v3 起）。未知位忽略。
    //
    // **为什么回零需要它**：HOME 的应答只说明"回零开始了"，而"零点建立"这件事
    // 必须由设备来说，不能由上位机推。上位机能看到的只有状态，而状态分不出下面
    // 这两种"回到 Ready"：
    //   · 走到位 → 设备把那里定义成 0 → Ready（HomeCompleted）
    //   · 中途被 STOP 打断 → Stopping → Ready
    // 后者只要那一拍的 |v| 本来就在停机门限内（回零本来就是低速），Stopping
    // 可以在**同一拍**里过去 —— 上位机根本采不到那一格，于是"被打断"会被读成
    // "回零成功"。这正是本项目最不能接受的那类错：把没发生的事说成发生了。
    //
    // 位的语义与 CiA402（CANopen 驱动器子协议）的 *Homing attained* 一致：
    // **进入回零时清掉、零点建立时置位**。所以它说的是"**这一轮**回零成没成"，
    // 而不是"这台机器历史上有没有回过零" —— 两者在"上一次回过、这一次又被停住"
    // 的现场完全不同，而后者恰恰是最容易发生的那一种。
    std::uint8_t statusFlags = 0U;
};
inline constexpr std::size_t kTelemetrySize = 21U;

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

// --- 回零（上位机 → 设备）----------------------------------------------------
//
// 回零速度由**上位机**给，不由设备写死：它是每台机器的事（丝杠导程、负载惯量、
// 行程开关的位置），而"谁更接近现场"这个问题只有一个答案。
//
// 速度取**幅值**，方向不在这条报文里：朝零点的方向由设备按自己的坐标判断。
// 让上位机指定方向，等于让一个看不见坐标系的人指挥电机往哪边撞。
struct HomeParam {
    float speedDegPerSec = 0.0F;    // > 0，幅值
    std::uint32_t timeoutMs = 0U;   // 超过它就判回零失败（进 Fault）
};
inline constexpr std::size_t kHomeParamSize = 8U;

std::vector<std::uint8_t> encodeHomeParam(const HomeParam& value);
PayloadError decodeHomeParam(const std::uint8_t* data, std::size_t len, HomeParam& out) noexcept;
bool isHomeParamValid(const HomeParam& value) noexcept;

// --- 软限位（上位机 → 设备，并且随参数回读返回）--------------------------------
//
// 它是**位置**行程限制，不是速度限制（速度的上界在配置里，见 SimulatorConfig）。
// enabled = false 时两个数值仍然有意义吗？不 —— 关掉就是关掉，两个数值不参与判定。
// 所以这里不给"关掉但留个默认值"的折中：关掉时设备一条都不看，界面上也照实写「未启用」。
struct SoftLimits {
    bool enabled = false;
    float minDeg = 0.0F;
    float maxDeg = 0.0F;
};
inline constexpr std::size_t kSoftLimitsSize = 9U;

std::vector<std::uint8_t> encodeSoftLimits(const SoftLimits& value);
PayloadError decodeSoftLimits(const std::uint8_t* data, std::size_t len, SoftLimits& out) noexcept;
bool isSoftLimitsValid(const SoftLimits& value) noexcept;
// 这个位置是否在限位之内。**未启用时恒为 true**，而且这个函数是唯一的判定处：
// 上位机的预判与设备的实际执法都调它，两边的口径不会漂。
bool softLimitAllows(const SoftLimits& limits, float positionDeg) noexcept;

// --- 参数回读（设备 → 上位机）------------------------------------------------
//
// 设备**存着**的那一组参数：模式 / 目标 / 三个 PID 增益 / 软限位。它是
// SET_MODE + SET_PID + SET_TARGET + SET_LIMITS 四条命令落地的结果。
//
// ⚠️ 它与遥测帧里的 target 字段**不是一回事**：那里报的是"此刻在追什么"
// （设备没在 Running 时恒为 0，因为那时它确实没在追任何值），这里报的是
// "被设成了多少"。要核对"下装到底存进去了什么"，只能看这一条。
struct ParamState {
    std::uint8_t mode = 0U;
    float target = 0.0F;
    float kp = 0.0F;
    float ki = 0.0F;
    float kd = 0.0F;
    // v2 起带上软限位：它必须回得出来，否则上位机只能显示"我以为设成了什么"，
    // 而"设备到底拿哪一组在拦"这件事就永远是猜的。
    SoftLimits limits{};
};
inline constexpr std::size_t kParamStateSize = 26U;

std::vector<std::uint8_t> encodeParamState(const ParamState& value);
PayloadError decodeParamState(const std::uint8_t* data, std::size_t len, ParamState& out) noexcept;
bool isParamStateValid(const ParamState& value) noexcept;

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
