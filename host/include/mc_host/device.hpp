#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/device_state.hpp"
#include "mc/host_session.hpp"
#include "mc/link_monitor.hpp"
#include "mc/loopback_transport.hpp"
#include "mc/payload.hpp"

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

    // 打开 / 关闭链路。打开后会话会自己发起握手（见 PROTOCOL.md §5.5）。
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const noexcept = 0;

    // 推进一拍：读传输 → 拆帧 → 分发 → 结算命令 → 驱动设备侧。
    virtual DeviceTick tick(std::uint64_t nowMs) = 0;

    // 返回 false = 命令**没有发出去**（会话未就绪 / 登记冲突）。
    // 对端的裁决不算在返回值里 —— 它随后以事件形式回来，落在命令行上。
    virtual bool setMode(std::uint8_t mode) = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool setTarget(float target) = 0;
    virtual bool setPid(const mc::SetPid& pid) = 0;

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
// 这是"没有硬件时唯一的对端"，也是本增量唯一实现 IDevice 的类。
// `SerialDevice`（QSerialPort）属后续增量：本机无硬件、也没有 PTY 对，
// 写了也验不了 —— 按任务书"不得宣称未验证"，不先写。
class SimulatedDevice final : public IDevice {
public:
    explicit SimulatedDevice(mc::SimulatorConfig simConfig = mc::SimulatorConfig{},
                             mc::LinkMonitor::Config monitorConfig = mc::LinkMonitor::Config{});
    ~SimulatedDevice() override;

    const char* kindName() const noexcept override { return "进程内模拟器"; }
    bool isSimulated() const noexcept override { return true; }

    void open() override;
    void close() override;
    bool isOpen() const noexcept override { return link_ != nullptr; }

    DeviceTick tick(std::uint64_t nowMs) override;

    bool setMode(std::uint8_t mode) override;
    bool start() override;
    bool stop() override;
    bool setTarget(float target) override;
    bool setPid(const mc::SetPid& pid) override;

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

}  // namespace mchost
