#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>

#include "mc/device_simulator.hpp"
#include "mc/device_state.hpp"
#include "mc/host_session.hpp"
#include "mc/link_monitor.hpp"
#include "mc/loopback_transport.hpp"
#include "mc/payload.hpp"

#include "mc_host/serial_transport.hpp"

namespace mchost {

// ---------------------------------------------------------------------------
// 层 4（设备抽象）
//
// 上位机**只认 IDevice**，「接真串口」与「接进程内模拟器」共用同一套 UI 与命令路径。
// 命令结算是本层唯一的"新增业务"，见下面的 CommandStatus。
//
// 层 3（通信）在这里**没有新代码**：`mc::HostSession` + `mc::ITransport` 就是通信层，
// 直接复用 libmc_core。刻意不为了"每层一个文件"再造一个转发壳 ——
// 那只会把已经测过的判据搬一层、再引入一处可能漂移的副本。
// ---------------------------------------------------------------------------

// 一条已下发命令的归宿。
//
// UI 必须把它显示出来：控件的"非装饰性"就体现在这里 —— 点了按钮之后
// 要能看到对端到底认没认。只把按钮接上 `sendXxx()` 而不显示结果，
// 等于把"命令是否生效"这件事藏起来。
enum class CommandStatus : std::uint8_t {
    NotSent = 0U,          // 根本没发出去（会话未就绪 / 登记冲突）
    Awaiting = 1U,         // 已发出，等应答
    Acked = 2U,            // 对端确认
    Nacked = 3U,           // 对端明确拒绝（带原因）
    TimedOut = 4U,         // 超时判 Fault —— 运动指令这里是**安全事件**，不是小毛病
    Unidirectional = 5U,   // 单向下行（STOP）：协议上就没有应答，不是"丢了"
};

const char* commandStatusName(CommandStatus status) noexcept;

// 打开设备为什么失败。接口返回的是**令牌**而不是一段话：
// 谁来措辞由界面决定，设备层只负责把原因分辨清楚（同一个理由见 NackReason 的处理）。
//
// 它存在的前提是"打开失败在真硬件上是常态"：端口被别的程序占着、线没插、
// 名字写错、Linux 上没进 dialout 组 —— 只回一个 false，等于把排查方向藏起来。
enum class OpenError : std::uint8_t {
    None = 0U,          // 还没试过 / 成功
    NoPortSelected,     // 参数里压根没给端口名
    NotFound,           // 设备不存在（没插上 / 名字错）
    BusyOrOccupied,     // 打不开：已被别的程序占用
    PermissionDenied,   // 权限不足
    Unsupported,        // 参数组合不被驱动支持
    TransportError,     // 打开后读写出错（线被拔了之类）
    Unknown,
};

const char* openErrorName(OpenError error) noexcept;

// NAK 原因的界面用词。放在设备层而不是各页面里：故障报警页随后也要用同一套措辞，
// 两处各写一份必然出现"同一件事两个说法"。
const char* nackReasonName(mc::NackReason reason) noexcept;

struct CommandRecord {
    std::string name;                    // 命令名（取自 mc::toString）
    std::uint16_t seq = 0U;
    CommandStatus status = CommandStatus::NotSent;
    mc::NackReason nackReason = mc::NackReason::Unspecified;
    std::uint64_t sentAtMs = 0U;         // 0 表示没发出去
};

// 一拍里发生的事。**刻意是事件而不是状态快照**：快照会让 UI 每拍重建，
// 而事件天然适合做报警与日志（"这一拍新增了几条 NAK"）。
// 累计量通过 IDevice 的只读访问器取。
struct DeviceTick {
    bool linkOpen = false;
    bool sessionReady = false;
    bool sessionLost = false;            // 本拍链路失效，会话被作废
    bool handshakeCompleted = false;     // 本拍握手完成
    std::uint64_t handshakeRounds = 0U;

    std::size_t telemetryAccepted = 0U;  // 本拍新增
    std::size_t telemetryRejected = 0U;
    bool hasTelemetry = false;
    mc::Telemetry telemetry{};           // 本拍最后一帧（hasTelemetry 为真时有效）

    std::size_t acksMatched = 0U;
    std::size_t naksReceived = 0U;
    std::size_t unexpectedFrames = 0U;
    std::size_t timeoutsFaulted = 0U;
    std::size_t timeoutsRetried = 0U;
    std::size_t droppedProbes = 0U;      // 心跳漏拍：不是故障

    bool linkDown = false;
    std::uint64_t ackTimeoutMs = 0U;

    // 命令日志本拍有没有变（新命令、或某条的结局变了）。
    // 由设备自己报，而不是让界面去比对快照 —— 比对要么漏要么每拍都算一遍。
    bool commandLogChanged = false;
};

class IDevice {
public:
    virtual ~IDevice() = default;

    virtual const char* kindName() const noexcept = 0;
    virtual bool isSimulated() const noexcept = 0;

    // "现在到底在跟什么说话"——一句话说清当前生效的参数（端口 @ 波特率 …）。
    // 让设备自己答，而不是让界面记住"我刚才应用了什么参数"：
    // 设备也可能由别的路径装上，那时界面手里的记录就是错的。
    virtual const char* configSummary() const noexcept = 0;

    // 打开 / 关闭链路。打开后会话会自己发起握手（见 PROTOCOL.md §5.5）。
    // 返回 false 时**没有打开**，原因见 openError()。
    virtual bool open() = 0;
    virtual OpenError openError() const noexcept = 0;
    virtual void close() = 0;
    virtual bool isOpen() const noexcept = 0;

    // 是否支持故障注入。真串口上"丢字节/加延迟"是做不到的 ——
    // 与其提供一个静默无效的开关（界面上会显示"已注入"而线上什么都没变），
    // 不如让界面据此把整组控件禁掉并说明原因。
    virtual bool supportsFaultInjection() const noexcept = 0;

    // 运行模式是"设备回读的"还是"上位机最后一次设定并被确认的"。
    //
    // 遥测帧里**没有**模式字段：实时回读模式要改协议（v1 不加字段），
    // 所以串口设备上的模式只能是上位机侧的设定值。这个差别必须显式暴露，
    // 否则界面上"模式：速度环"会被读成"设备现在确实在速度环"。
    virtual bool runModeIsReadBack() const noexcept = 0;

    // 推进一拍：读传输 → 拆帧 → 分发 → 结算命令 → 驱动设备侧。
    virtual DeviceTick tick(std::uint64_t nowMs) = 0;

    // 返回 false = 命令**没有发出去**（会话未就绪 / 登记冲突）。
    // 对端的裁决不算在返回值里 —— 它随后以事件形式回来，落在命令行上。
    virtual bool setMode(std::uint8_t mode) = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool setTarget(float target) = 0;
    virtual bool setPid(const mc::SetPid& pid) = 0;
    // 人工确认并请求清除故障锁存。**只发"确认"，不代表故障已被排除** ——
    // 物理原因（超速/堵转/接线）必须先由人查清，这条命令只是让设备重新自检。
    virtual bool clearFault(mc::FaultCode code) = 0;

    // 读回设备**存着**的那一组参数（模式 / 目标 / PID 增益 / 软限位）。
    //
    // 它不是运动指令，而是一次幂等读，因此超时后允许重传（协议层已这么定）。
    // 返回值只说明"有没有发出去"；读回来的值在 lastParamState() 里。
    virtual bool readParams() = 0;

    // 回零：设备以给定速度朝零点走，到位后把那里定义成零点。
    // 它是**运动指令**（电机会转）：超时按 Fault 处理、不自动重发。
    // 应答只说明"设备接受了"，到位与否看 deviceState()（Home → Ready/Fault）。
    virtual bool home(const mc::HomeParam& param) = 0;

    // 下发软限位（位置行程限制）。设备存下来并据此拦，不是一条显示用的设置。
    virtual bool setLimits(const mc::SoftLimits& limits) = 0;

    // --- 参数回读的观测 ---------------------------------------------------------
    virtual bool hasParamState() const noexcept = 0;
    virtual mc::ParamState lastParamState() const noexcept = 0;
    // 坏数据单独计数：把"设备回了一份坏数据"混同于"设备没回答"，排查方向会整个偏掉。
    virtual std::uint64_t paramStatesReceivedTotal() const noexcept = 0;
    virtual std::uint64_t paramStatesRejectedTotal() const noexcept = 0;

    // --- 会话与设备状态 ---------------------------------------------------------
    virtual bool sessionReady() const noexcept = 0;
    virtual std::uint64_t handshakeRounds() const noexcept = 0;
    virtual mc::DeviceState deviceState() const noexcept = 0;
    virtual bool faultLatched() const noexcept = 0;
    virtual mc::FaultCode faultCode() const noexcept = 0;
    virtual mc::SimRunMode runMode() const noexcept = 0;
    virtual bool enabled() const noexcept = 0;
    virtual float velocityDegPerSec() const noexcept = 0;
    virtual float positionDeg() const noexcept = 0;
    virtual float deviceTarget() const noexcept = 0;
    // **设备说的**"本轮回零已经建立了零点"（遥测状态位组里的那一位，v3 起）。
    //
    // 上位机不能自己推这件事：它能看到的只有状态，而"被 STOP 打断的回零"与
    // "走到位的回零"在状态上可能都表现为 Ready —— 打断那条路径里的 Stopping
    // 会在两拍之间过去（回零本来就是低速）。所以"零点建立没有"只能读设备报的位。
    //
    // 未收到过遥测时返回 false（= "还没有证据"）：这与"设备说没建立"在字面上
    // 无法区分，是刻意的 —— 调用方该先看 hasTelemetryReceived()，别把"没收到"
    // 当成一条结论。
    virtual bool homingAttained() const noexcept = 0;
    virtual bool hasTelemetryReceived() const noexcept = 0;

    // --- 链路 -------------------------------------------------------------------
    virtual bool linkDown() const noexcept = 0;
    virtual std::uint64_t ackTimeoutMs() const noexcept = 0;
    virtual bool hasRttSample() const noexcept = 0;
    virtual std::uint64_t rttP95Ms() const noexcept = 0;
    virtual const mc::LinkMonitor::Stats* linkStats() const noexcept = 0;

    // --- 统计（上位机侧）--------------------------------------------------------
    virtual std::uint64_t framesSent() const noexcept = 0;
    virtual std::uint64_t hostEncodeFailures() const noexcept = 0;
    virtual std::uint64_t hostSendFailures() const noexcept = 0;
    virtual std::uint64_t telemetryAcceptedTotal() const noexcept = 0;
    virtual std::uint64_t telemetryRejectedTotal() const noexcept = 0;
    virtual const mc::StreamDecoder::Stats& decoderStats() const noexcept = 0;
    virtual const mc::AckStats& ackStats() const noexcept = 0;

    // 命令行（新→旧）。UI 用它证明每个按钮都真的走了协议。
    virtual const std::deque<CommandRecord>& commandLog() const noexcept = 0;

    // --- 故障注入（演示与测试用，不是产品功能）----------------------------------
    virtual void setUplinkDropEveryNthByte(std::uint32_t n) = 0;
    virtual void setDownlinkDropEveryNthByte(std::uint32_t n) = 0;
    virtual void setUplinkLatencyPolls(std::uint32_t n) = 0;
    virtual void clearInjectedFaults() = 0;
};

// 进程内模拟设备：把 DeviceSimulator 接在回环链路的另一端。
//
// 这是"没有硬件时唯一的对端"。它和 SerialDevice 共用同一套 UI 与命令路径 ——
// 界面上没有一处 `if (模拟器)` 的分支（需要区别的地方用 isSimulated() /
// runModeIsReadBack() / supportsFaultInjection() 这几个显式能力位表达）。
class SimulatedDevice final : public IDevice {
public:
    explicit SimulatedDevice(mc::SimulatorConfig simConfig = mc::SimulatorConfig{},
                             mc::LinkMonitor::Config monitorConfig = mc::LinkMonitor::Config{});
    ~SimulatedDevice() override;

    const char* kindName() const noexcept override { return "进程内模拟器"; }
    bool isSimulated() const noexcept override { return true; }
    const char* configSummary() const noexcept override {
        return "内置仿真模型（无外部参数：加速度限幅 + 一阶速率限制）";
    }

    bool open() override;
    OpenError openError() const noexcept override { return OpenError::None; }
    void close() override;
    bool isOpen() const noexcept override { return link_ != nullptr; }
    bool supportsFaultInjection() const noexcept override { return true; }
    // 模拟器把状态机装在自己身上，"模式"是它自己的字段 —— 这就是回读。
    bool runModeIsReadBack() const noexcept override { return true; }

    DeviceTick tick(std::uint64_t nowMs) override;

    bool setMode(std::uint8_t mode) override;
    bool start() override;
    bool stop() override;
    bool setTarget(float target) override;
    bool setPid(const mc::SetPid& pid) override;
    bool clearFault(mc::FaultCode code) override;
    bool readParams() override;
    bool home(const mc::HomeParam& param) override;
    bool setLimits(const mc::SoftLimits& limits) override;

    bool sessionReady() const noexcept override;
    std::uint64_t handshakeRounds() const noexcept override;
    mc::DeviceState deviceState() const noexcept override;
    bool faultLatched() const noexcept override;
    mc::FaultCode faultCode() const noexcept override;
    mc::SimRunMode runMode() const noexcept override;
    bool enabled() const noexcept override;
    float velocityDegPerSec() const noexcept override;
    float positionDeg() const noexcept override;
    float deviceTarget() const noexcept override;
    bool homingAttained() const noexcept override;
    bool hasTelemetryReceived() const noexcept override;

    bool linkDown() const noexcept override;
    std::uint64_t ackTimeoutMs() const noexcept override;
    bool hasRttSample() const noexcept override;
    std::uint64_t rttP95Ms() const noexcept override;
    const mc::LinkMonitor::Stats* linkStats() const noexcept override;

    std::uint64_t framesSent() const noexcept override;
    std::uint64_t hostEncodeFailures() const noexcept override;
    std::uint64_t hostSendFailures() const noexcept override;
    std::uint64_t telemetryAcceptedTotal() const noexcept override;
    std::uint64_t telemetryRejectedTotal() const noexcept override;
    bool hasParamState() const noexcept override;
    mc::ParamState lastParamState() const noexcept override;
    std::uint64_t paramStatesReceivedTotal() const noexcept override;
    std::uint64_t paramStatesRejectedTotal() const noexcept override;
    const mc::StreamDecoder::Stats& decoderStats() const noexcept override;
    const mc::AckStats& ackStats() const noexcept override;

    const std::deque<CommandRecord>& commandLog() const noexcept override { return log_; }

    // 设备侧"欠发"计数（该产但超过单拍上限而丢弃的遥测帧数）。
    // **刻意不放进 IDevice**：那是模拟器的内部计数器，真实设备不会把它塞进
    // 遥测帧，放进接口就等于逼所有实现返回一个自己拿不到的数。
    std::uint64_t telemetryBacklogDropped() const noexcept;

    void setUplinkDropEveryNthByte(std::uint32_t n) override;
    void setDownlinkDropEveryNthByte(std::uint32_t n) override;
    void setUplinkLatencyPolls(std::uint32_t n) override;
    void clearInjectedFaults() override;

private:
    // 一次"打开链路"的全部家当。
    //
    // 做成可整体重建的对象，是因为回环端点**一旦 close 就不能重开**
    // （LoopbackTransport 没有 open），而 HostSession 又持有传输的引用 ——
    // 所以"重新连接"只能整组换新，不能原地复位。真串口设备同理（重开端口）。
    struct Link {
        Link(const mc::SimulatorConfig& simConfig, const mc::LinkMonitor::Config& monitorConfig);

        mc::LoopbackPair pair = mc::makeLoopbackPair();
        std::unique_ptr<mc::HostSession> host;   // 引用 pair.host，必须后于 pair 构造
        mc::DeviceSimulator sim;
        std::uint16_t seq = 0U;                  // 心跳 / 握手 / 命令共用的唯一序列号来源
    };

    // 记录一条命令；返回它是否被发出。
    bool recordCommand(const char* name, std::uint16_t seq, bool sent);
    // 按本拍的 NAK / 超时 / 在途表把命令状态推进一格。
    void settleCommands(const mc::HostSession::PumpResult& hostSide);
    CommandRecord* findAwaiting(std::uint16_t seq);

    mc::SimulatorConfig simConfig_;
    mc::LinkMonitor::Config monitorConfig_;
    std::unique_ptr<Link> link_;
    std::deque<CommandRecord> log_;
    std::size_t nacksSeen_ = 0U;
    std::uint64_t lastTickMs_ = 0U;   // 最近一拍的时钟，命令登记超时窗口用
    bool logDirty_ = false;           // 命令日志有变化 → 下一次 tick 上报
    static constexpr std::size_t kMaxLogEntries = 64U;
};

// 真串口设备：把 SerialTransport（QSerialPort）接上 HostSession。
//
// 与 SimulatedDevice 最大的不同不在传输，而在**它什么都看不到**：
// 对面是一块只会按协议回帧的板子，没有本地对象可以问状态。
// 因此本类的一切观测值都只有两个来源：
//   ① 遥测帧（速度/位置/目标/PWM/状态/故障码）—— 这是**回读**；
//   ② 上位机自己发出去、并**被确认过**的设定值（运行模式）—— 这是**设定值**。
// 两者绝不可混为一谈，runModeIsReadBack() 就是把这个差别交给界面去说明的开关。
//
// 另一个后果：**锁存位只能从遥测里推**。设备进 Ready 时会把自己的 faultCode
// 清成 0（见 device_state.cpp 的 applyEvent），所以"faultCode 非 0 且状态不是
// Ready"就是锁存着。这不是第二套规则，而是设备状态机那条不变式在遥测上的投影。
class SerialDevice final : public IDevice {
public:
    SerialDevice(SerialTransport::Config config, mc::LinkMonitor::Config monitorConfig);
    ~SerialDevice() override;

    const char* kindName() const noexcept override { return "串口设备"; }
    bool isSimulated() const noexcept override { return false; }
    const char* configSummary() const noexcept override;

    bool open() override;
    OpenError openError() const noexcept override { return openError_; }
    void close() override;
    bool isOpen() const noexcept override { return host_ != nullptr; }
    bool supportsFaultInjection() const noexcept override { return false; }
    bool runModeIsReadBack() const noexcept override { return false; }

    DeviceTick tick(std::uint64_t nowMs) override;

    bool setMode(std::uint8_t mode) override;
    bool start() override;
    bool stop() override;
    bool setTarget(float target) override;
    bool setPid(const mc::SetPid& pid) override;
    bool clearFault(mc::FaultCode code) override;
    bool readParams() override;
    bool home(const mc::HomeParam& param) override;
    bool setLimits(const mc::SoftLimits& limits) override;

    bool sessionReady() const noexcept override;
    std::uint64_t handshakeRounds() const noexcept override;
    mc::DeviceState deviceState() const noexcept override;
    bool faultLatched() const noexcept override;
    mc::FaultCode faultCode() const noexcept override;
    mc::SimRunMode runMode() const noexcept override;
    bool enabled() const noexcept override;
    float velocityDegPerSec() const noexcept override;
    float positionDeg() const noexcept override;
    float deviceTarget() const noexcept override;
    bool homingAttained() const noexcept override;
    bool hasTelemetryReceived() const noexcept override;

    bool linkDown() const noexcept override;
    std::uint64_t ackTimeoutMs() const noexcept override;
    bool hasRttSample() const noexcept override;
    std::uint64_t rttP95Ms() const noexcept override;
    const mc::LinkMonitor::Stats* linkStats() const noexcept override;

    std::uint64_t framesSent() const noexcept override;
    std::uint64_t hostEncodeFailures() const noexcept override;
    std::uint64_t hostSendFailures() const noexcept override;
    std::uint64_t telemetryAcceptedTotal() const noexcept override;
    std::uint64_t telemetryRejectedTotal() const noexcept override;
    bool hasParamState() const noexcept override;
    mc::ParamState lastParamState() const noexcept override;
    std::uint64_t paramStatesReceivedTotal() const noexcept override;
    std::uint64_t paramStatesRejectedTotal() const noexcept override;
    const mc::StreamDecoder::Stats& decoderStats() const noexcept override;
    const mc::AckStats& ackStats() const noexcept override;

    const std::deque<CommandRecord>& commandLog() const noexcept override { return log_; }

    // 注入：串口上做不到，**明确不支持**（supportsFaultInjection() == false）。
    // 这里不做任何事，但也不假装成功 —— 界面根本不会给机会调它们。
    void setUplinkDropEveryNthByte(std::uint32_t n) override;
    void setDownlinkDropEveryNthByte(std::uint32_t n) override;
    void setUplinkLatencyPolls(std::uint32_t n) override;
    void clearInjectedFaults() override;

private:
    // 一条已发出的 SET_MODE。用来在它**被确认**之后记住模式。
    // 为什么不直接记"我发过的最后一个模式"：没被确认的设定值等于没生效，
    // 拿它去回答"设备现在是什么模式"就是把一次可能丢失的命令说成事实。
    struct ModeRequest {
        std::uint16_t seq = 0U;
        std::uint8_t mode = 0U;
    };

    bool recordCommand(const char* name, std::uint16_t seq, bool sent, std::uint8_t mode = 0xFFU);
    void settleCommands(const mc::HostSession::PumpResult& hostSide);
    CommandRecord* findAwaiting(std::uint16_t seq);

    SerialTransport::Config transportConfig_;
    mc::LinkMonitor::Config monitorConfig_;
    std::unique_ptr<SerialTransport> transport_;
    std::unique_ptr<mc::HostSession> host_;
    std::uint16_t seq_ = 0U;

    std::deque<CommandRecord> log_;
    std::vector<ModeRequest> modeRequests_;
    std::uint8_t ackedMode_ = 0U;
    bool hasAckedMode_ = false;

    std::size_t nacksSeen_ = 0U;
    std::uint64_t lastTickMs_ = 0U;
    bool logDirty_ = false;

    mc::Telemetry lastTelemetry_{};
    bool hasTelemetry_ = false;
    OpenError openError_ = OpenError::None;
    mutable QByteArray summaryUtf8_;

    static constexpr std::size_t kMaxLogEntries = 64U;
};

}  // namespace mchost
