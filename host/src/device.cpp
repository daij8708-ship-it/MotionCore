#include "mc_host/device.hpp"

#include <optional>
#include <utility>

namespace mchost {
namespace {

constexpr std::size_t kMaxBytesPerExchange = 512U;

}  // namespace

const char* commandStatusName(CommandStatus status) noexcept {
    switch (status) {
        case CommandStatus::NotSent:        return "未发出";
        case CommandStatus::Awaiting:       return "等待应答";
        case CommandStatus::Acked:          return "已确认";
        case CommandStatus::Nacked:         return "被拒绝";
        case CommandStatus::TimedOut:       return "超时";
        case CommandStatus::Unidirectional: return "单向（无应答）";
    }
    return "?";
}

const char* nackReasonName(mc::NackReason reason) noexcept {
    switch (reason) {
        case mc::NackReason::Unspecified:   return "未说明";
        case mc::NackReason::UnknownType:   return "类型不认识";
        case mc::NackReason::IllegalState:  return "当前设备状态不允许该命令";
        case mc::NackReason::BadParameter:  return "参数非法";
        case mc::NackReason::CrcError:      return "校验错";
        case mc::NackReason::Busy:          return "设备忙";
    }
    return "?";
}

const char* openErrorName(OpenError error) noexcept {
    switch (error) {
        case OpenError::None:             return "无";
        case OpenError::NoPortSelected:   return "没有选择端口";
        case OpenError::NotFound:         return "找不到端口（设备没插上，或名字写错了）";
        case OpenError::BusyOrOccupied:   return "端口打不开或已被占用（另一个程序正开着它？）";
        case OpenError::PermissionDenied: return "权限不足（Linux 上通常要把用户加进 dialout 组）";
        case OpenError::Unsupported:      return "本平台/驱动不支持该串口参数组合";
        case OpenError::TransportError:   return "端口读写出错（线被拔了？）";
        case OpenError::Unknown:          return "未知原因";
    }
    return "?";
}

SimulatedDevice::Link::Link(const mc::SimulatorConfig& simConfig,
                            const mc::LinkMonitor::Config& monitorConfig)
    : host(std::make_unique<mc::HostSession>(*pair.host)), sim(simConfig) {
    // 心跳、握手、命令**必须**共用一个计数器：AckTracker 只按 seq 判重、不看类型，
    // 两套计数器必然撞车，撞上之后命令会被 canRegister 拦截、静默发不出去。
    host->enableLinkMonitor(monitorConfig, [this]() { return seq++; });
    host->enableHandshake([this]() { return seq++; });
}

SimulatedDevice::SimulatedDevice(mc::SimulatorConfig simConfig,
                                 mc::LinkMonitor::Config monitorConfig)
    : simConfig_(simConfig), monitorConfig_(monitorConfig) {}

SimulatedDevice::~SimulatedDevice() = default;

bool SimulatedDevice::open() {
    if (link_ != nullptr) {
        return true;
    }
    link_ = std::make_unique<Link>(simConfig_, monitorConfig_);
    nacksSeen_ = 0U;
    return true;
}

void SimulatedDevice::close() {
    if (link_ == nullptr) {
        return;
    }

    // 在途命令一律按**超时**结清。
    // ADR-003：一条没被确认的运动指令必须当成安全事件 —— 断开时更不能假设
    // "它大概没生效"。留在"等待应答"里等于把这件事从界面上藏起来。
    for (CommandRecord& record : log_) {
        if (record.status == CommandStatus::Awaiting) {
            record.status = CommandStatus::TimedOut;
        }
    }

    link_.reset();
    nacksSeen_ = 0U;
}

DeviceTick SimulatedDevice::tick(std::uint64_t nowMs) {
    DeviceTick out;
    if (link_ == nullptr) {
        return out;   // linkOpen 恒为 false：没打开就是没打开，不假装
    }

    // 命令登记要用的时间就是这个时钟。UI 在两次 tick 之间下命令，
    // 用最近一拍的时刻与真实时刻最多差一个 tick（10 ms），对超时窗口没有影响。
    lastTickMs_ = nowMs;

    const mc::HostSession::PumpResult hostSide = link_->host->pump(nowMs);

    // 命令结算必须在 pump 之后：本拍的应答正是在 pump 里被分发的。
    settleCommands(hostSide);
    out.commandLogChanged = logDirty_;
    logDirty_ = false;

    out.linkOpen = link_->pair.host->isOpen();
    out.sessionReady = link_->host->sessionReady();
    out.sessionLost = hostSide.sessionLost;
    out.handshakeCompleted = hostSide.handshakeCompleted;
    out.handshakeRounds = link_->host->handshakeRounds();

    out.telemetryAccepted = hostSide.telemetryAccepted;
    out.telemetryRejected = hostSide.telemetryRejected;
    if (hostSide.telemetryAccepted > 0U) {
        out.hasTelemetry = true;
        out.telemetry = link_->host->lastTelemetry();
    }

    out.acksMatched = hostSide.acksMatched;
    out.naksReceived = hostSide.naksReceived;
    out.unexpectedFrames = hostSide.unexpectedFrames;
    out.droppedProbes = hostSide.droppedProbes;
    for (const mc::TimeoutEvent& event : hostSide.timeouts) {
        if (event.outcome == mc::TxOutcome::Fault) {
            ++out.timeoutsFaulted;
        } else if (event.outcome == mc::TxOutcome::Retry) {
            ++out.timeoutsRetried;
        }
    }

    out.linkDown = hostSide.linkDown;
    out.ackTimeoutMs = hostSide.ackTimeoutMs;

    // --- 设备侧：收上位机的字节 → 推进 → 把待发字节送回链路 ---------------
    std::uint8_t toDevice[kMaxBytesPerExchange];
    const std::size_t down = link_->pair.device->receive(toDevice, sizeof(toDevice));
    (void)link_->sim.tick(toDevice, down, nowMs);

    std::uint8_t fromDevice[kMaxBytesPerExchange];
    const std::size_t up = link_->sim.readOutgoing(fromDevice, sizeof(fromDevice));
    if (up > 0U) {
        (void)link_->pair.device->send(fromDevice, up);
    }

    return out;
}

bool SimulatedDevice::recordCommand(const char* name, std::uint16_t seq, bool sent) {
    CommandRecord record;
    record.name = name;
    record.seq = seq;
    record.sentAtMs = sent ? lastTickMs_ : 0U;
    record.status = sent ? CommandStatus::Awaiting : CommandStatus::NotSent;
    log_.push_front(record);
    if (log_.size() > kMaxLogEntries) {
        log_.pop_back();
    }
    logDirty_ = true;
    return sent;
}

CommandRecord* SimulatedDevice::findAwaiting(std::uint16_t seq) {
    for (CommandRecord& record : log_) {
        if (record.seq == seq && record.status == CommandStatus::Awaiting) {
            return &record;
        }
    }
    return nullptr;
}

void SimulatedDevice::settleCommands(const mc::HostSession::PumpResult& hostSide) {
    // ① 新到的 NAK：对端明确拒绝。
    //    只处理**没见过的**那几条，否则每拍都会把同一条 NAK 重新算一次。
    const std::vector<mc::Nack>& nacks = link_->host->receivedNacks();
    for (; nacksSeen_ < nacks.size(); ++nacksSeen_) {
        CommandRecord* record = findAwaiting(nacks[nacksSeen_].rejectedSeq);
        if (record != nullptr) {
            record->status = CommandStatus::Nacked;
            record->nackReason = nacks[nacksSeen_].reason;
            logDirty_ = true;
        }
    }

    // ② 本拍的超时事件。只认 Fault：
    //    Dropped 是心跳漏拍（不在命令日志里），Retry 表示还会重发、命令仍在途。
    for (const mc::TimeoutEvent& event : hostSide.timeouts) {
        if (event.outcome != mc::TxOutcome::Fault) {
            continue;
        }
        CommandRecord* record = findAwaiting(event.seq);
        if (record != nullptr) {
            record->status = CommandStatus::TimedOut;
            logDirty_ = true;
        }
    }

    // ③ 剩下的仍标"等待应答"的：已经不在等待表里、也没判超时/被拒 → 它是被应答了。
    for (CommandRecord& record : log_) {
        if (record.status != CommandStatus::Awaiting) {
            continue;
        }
        if (!link_->host->tracker().isPending(record.seq)) {
            record.status = CommandStatus::Acked;
            logDirty_ = true;
        }
    }
}

// --- 命令 -------------------------------------------------------------------
//
// 控制类命令带一个**本层前置条件**：会话未就绪就不发。
// 这不是替设备做门禁（设备那边照样会拒），而是"别发一条注定被拒的东西"
// —— 否则命令日志里会塞满噪声，真正的拒绝反而被淹掉。
// STOP 例外：它是安全动作，协议规定任何状态都接受，链路开着就允许发。

bool SimulatedDevice::setMode(std::uint8_t mode) {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("SET_MODE", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("SET_MODE", seq, link_->host->sendSetMode(mode, seq, lastTickMs_));
}

bool SimulatedDevice::start() {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("START", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("START", seq, link_->host->sendStart(seq, lastTickMs_));
}

bool SimulatedDevice::stop() {
    if (link_ == nullptr) {
        return recordCommand("STOP", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    const bool sent = link_->host->sendStop(seq);

    CommandRecord record;
    record.name = "STOP";
    record.seq = seq;
    record.sentAtMs = lastTickMs_;
    // STOP 是单向的（ADR-004）：协议上就没有应答，所以"已发出"就是它的终点。
    // 若标成"等待应答"，界面会永远停在等待上 —— 把协议设计当成了丢包。
    record.status = sent ? CommandStatus::Unidirectional : CommandStatus::NotSent;
    log_.push_front(record);
    if (log_.size() > kMaxLogEntries) {
        log_.pop_back();
    }
    return sent;
}

bool SimulatedDevice::setTarget(float target) {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("SET_TARGET", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("SET_TARGET", seq, link_->host->sendSetTarget(target, seq, lastTickMs_));
}

bool SimulatedDevice::setPid(const mc::SetPid& pid) {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("SET_PID", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("SET_PID", seq, link_->host->sendSetPid(pid, seq, lastTickMs_));
}

bool SimulatedDevice::readParams() {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("READ_PARAMS", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("READ_PARAMS", seq, link_->host->sendReadParams(seq, lastTickMs_));
}

bool SimulatedDevice::home(const mc::HomeParam& param) {
    // 与其余运动命令同一档：会话未就绪就不发（此时命令到不了设备）。
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("HOME", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("HOME", seq, link_->host->sendHome(param, seq, lastTickMs_));
}

bool SimulatedDevice::setLimits(const mc::SoftLimits& limits) {
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("SET_LIMITS", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand("SET_LIMITS", seq, link_->host->sendSetLimits(limits, seq, lastTickMs_));
}

bool SimulatedDevice::clearFault(mc::FaultCode code) {
    // 会话未就绪就不发。局部条件比设备门禁更严一档是**故意**的：故障锁存期间
    // 链路一旦断了（重连会被直接带回 Fault），此时发出去的确认连门禁都到不了，
    // 日志里只会多一条噪声。
    if (link_ == nullptr || !link_->host->sessionReady()) {
        return recordCommand("CLEAR_FAULT", 0U, false);
    }
    const std::uint16_t seq = link_->seq++;
    return recordCommand(
        "CLEAR_FAULT", seq,
        link_->host->sendClearFault(static_cast<std::uint8_t>(code), seq, lastTickMs_));
}

// --- 只读观测 ---------------------------------------------------------------

bool SimulatedDevice::sessionReady() const noexcept {
    return link_ != nullptr && link_->host->sessionReady();
}

std::uint64_t SimulatedDevice::handshakeRounds() const noexcept {
    return (link_ != nullptr) ? link_->host->handshakeRounds() : 0U;
}

mc::DeviceState SimulatedDevice::deviceState() const noexcept {
    return (link_ != nullptr) ? link_->sim.state() : mc::DeviceState::Disconnected;
}

bool SimulatedDevice::faultLatched() const noexcept {
    return link_ != nullptr && link_->sim.faultLatched();
}

mc::FaultCode SimulatedDevice::faultCode() const noexcept {
    return (link_ != nullptr) ? link_->sim.faultCode() : mc::FaultCode::None;
}

mc::SimRunMode SimulatedDevice::runMode() const noexcept {
    return (link_ != nullptr) ? link_->sim.runMode() : mc::SimRunMode::Stopped;
}

bool SimulatedDevice::enabled() const noexcept {
    return link_ != nullptr && link_->sim.isEnabled();
}

float SimulatedDevice::velocityDegPerSec() const noexcept {
    return (link_ != nullptr) ? link_->sim.velocityDegPerSec() : 0.0F;
}

float SimulatedDevice::positionDeg() const noexcept {
    return (link_ != nullptr) ? link_->sim.positionDeg() : 0.0F;
}

float SimulatedDevice::deviceTarget() const noexcept {
    return (link_ != nullptr) ? link_->sim.target() : 0.0F;
}

// 进程内模拟器：这一位直接问设备（它就在同一个进程里）。
// 真链路上它来自遥测帧的 statusFlags —— 见下面的 SerialDevice 版本。
bool SimulatedDevice::homingAttained() const noexcept {
    return link_ != nullptr && link_->sim.homingAttained();
}

bool SimulatedDevice::hasTelemetryReceived() const noexcept {
    return link_ != nullptr && link_->host->telemetryAccepted() > 0U;
}

bool SimulatedDevice::linkDown() const noexcept {
    return link_ != nullptr && link_->host->linkMonitor() != nullptr &&
           link_->host->linkMonitor()->linkDown();
}

std::uint64_t SimulatedDevice::ackTimeoutMs() const noexcept {
    return (link_ != nullptr) ? link_->host->tracker().timeoutMs() : 0U;
}

bool SimulatedDevice::hasRttSample() const noexcept {
    return link_ != nullptr && link_->host->linkMonitor() != nullptr &&
           link_->host->linkMonitor()->hasRttSample();
}

std::uint64_t SimulatedDevice::rttP95Ms() const noexcept {
    if (link_ == nullptr || link_->host->linkMonitor() == nullptr) {
        return 0U;
    }
    const std::optional<std::uint64_t> rtt = link_->host->linkMonitor()->rttP95Ms();
    return rtt.has_value() ? *rtt : 0U;
}

const mc::LinkMonitor::Stats* SimulatedDevice::linkStats() const noexcept {
    if (link_ == nullptr || link_->host->linkMonitor() == nullptr) {
        return nullptr;
    }
    return &link_->host->linkMonitor()->stats();
}

std::uint64_t SimulatedDevice::framesSent() const noexcept {
    return (link_ != nullptr) ? link_->host->framesSent() : 0U;
}

std::uint64_t SimulatedDevice::hostEncodeFailures() const noexcept {
    return (link_ != nullptr) ? link_->host->encodeFailures() : 0U;
}

std::uint64_t SimulatedDevice::hostSendFailures() const noexcept {
    return (link_ != nullptr) ? link_->host->sendFailures() : 0U;
}

std::uint64_t SimulatedDevice::telemetryAcceptedTotal() const noexcept {
    return (link_ != nullptr) ? link_->host->telemetryAccepted() : 0U;
}

std::uint64_t SimulatedDevice::telemetryRejectedTotal() const noexcept {
    return (link_ != nullptr) ? link_->host->telemetryRejected() : 0U;
}

bool SimulatedDevice::hasParamState() const noexcept {
    return link_ != nullptr && link_->host->hasParamState();
}

mc::ParamState SimulatedDevice::lastParamState() const noexcept {
    return (link_ != nullptr) ? link_->host->lastParamState() : mc::ParamState{};
}

std::uint64_t SimulatedDevice::paramStatesReceivedTotal() const noexcept {
    return (link_ != nullptr) ? link_->host->paramStatesReceived() : 0U;
}

std::uint64_t SimulatedDevice::paramStatesRejectedTotal() const noexcept {
    return (link_ != nullptr) ? link_->host->paramStatesRejected() : 0U;
}

const mc::StreamDecoder::Stats& SimulatedDevice::decoderStats() const noexcept {
    static const mc::StreamDecoder::Stats kEmpty{};
    return (link_ != nullptr) ? link_->host->decoder().stats() : kEmpty;
}

const mc::AckStats& SimulatedDevice::ackStats() const noexcept {
    static const mc::AckStats kEmpty{};
    return (link_ != nullptr) ? link_->host->tracker().stats() : kEmpty;
}

std::uint64_t SimulatedDevice::telemetryBacklogDropped() const noexcept {
    return (link_ != nullptr) ? link_->sim.telemetryBacklogDropped() : 0U;
}

// --- 故障注入 ---------------------------------------------------------------

void SimulatedDevice::setUplinkDropEveryNthByte(std::uint32_t n) {
    if (link_ != nullptr) {
        link_->pair.device->outgoingFaults().dropEveryNthByte = n;
    }
}

void SimulatedDevice::setDownlinkDropEveryNthByte(std::uint32_t n) {
    if (link_ != nullptr) {
        link_->pair.host->outgoingFaults().dropEveryNthByte = n;
    }
}

void SimulatedDevice::setUplinkLatencyPolls(std::uint32_t n) {
    if (link_ != nullptr) {
        link_->pair.device->outgoingFaults().latencyPolls = n;
    }
}

void SimulatedDevice::clearInjectedFaults() {
    if (link_ != nullptr) {
        link_->pair.host->outgoingFaults() = mc::LinkFaultConfig{};
        link_->pair.device->outgoingFaults() = mc::LinkFaultConfig{};
    }
}

// ===========================================================================
// SerialDevice
// ===========================================================================

SerialDevice::SerialDevice(SerialTransport::Config config, mc::LinkMonitor::Config monitorConfig)
    : transportConfig_(std::move(config)), monitorConfig_(monitorConfig) {}

SerialDevice::~SerialDevice() = default;

const char* SerialDevice::configSummary() const noexcept {
    summaryUtf8_ = QStringLiteral("%1 @ %2 baud, %3%4%5%6")
                       .arg(transportConfig_.portName, QString::number(transportConfig_.baudRate),
                            QString::number(transportConfig_.dataBits),
                            transportConfig_.parity == SerialTransport::Config::Parity::None
                                ? QStringLiteral("N")
                            : transportConfig_.parity == SerialTransport::Config::Parity::Even
                                ? QStringLiteral("E")
                                : QStringLiteral("O"),
                            QString::number(transportConfig_.stopBits),
                            transportConfig_.hardwareFlowControl ? QStringLiteral("+RTS/CTS")
                                                                 : QString())
                       .toUtf8();
    return summaryUtf8_.constData();
}

bool SerialDevice::open() {
    if (host_ != nullptr) {
        return true;
    }

    openError_ = OpenError::None;

    transport_ = std::make_unique<SerialTransport>(transportConfig_);
    if (!transport_->open()) {
        // 把 QSerialPort 的错误码翻成本层的令牌：界面只需要知道"该去查哪一类原因"，
        // 不需要认识 Qt 的错误枚举（那是本层的实现细节）。
        switch (transport_->lastSystemError()) {
            case QSerialPort::DeviceNotFoundError: openError_ = OpenError::NotFound; break;
            case QSerialPort::PermissionError:     openError_ = OpenError::PermissionDenied; break;
            case QSerialPort::OpenError:
            case QSerialPort::ResourceError:       openError_ = OpenError::BusyOrOccupied; break;
            case QSerialPort::UnsupportedOperationError: openError_ = OpenError::Unsupported; break;
            case QSerialPort::WriteError:
            case QSerialPort::ReadError:           openError_ = OpenError::TransportError; break;
            default:                               openError_ = OpenError::Unknown; break;
        }
        transport_.reset();
        return false;
    }

    host_ = std::make_unique<mc::HostSession>(*transport_);
    // 心跳 / 握手 / 命令共用一个计数器（与模拟器同一条理由：AckTracker 只按 seq 判重）。
    host_->enableLinkMonitor(monitorConfig_, [this]() { return seq_++; });
    host_->enableHandshake([this]() { return seq_++; });

    nacksSeen_ = 0U;
    hasTelemetry_ = false;
    log_.clear();
    modeRequests_.clear();
    hasAckedMode_ = false;   // 新会话还没确认过任何模式
    return true;
}

void SerialDevice::close() {
    if (host_ == nullptr) {
        return;
    }

    // 与模拟器同样的规则：断开时把在途命令按超时结清。
    // 一条没被确认的运动指令不能因为"我关了端口"就变成没发生过。
    for (CommandRecord& record : log_) {
        if (record.status == CommandStatus::Awaiting) {
            record.status = CommandStatus::TimedOut;
        }
    }

    host_.reset();
    if (transport_ != nullptr) {
        transport_->close();
        transport_.reset();
    }
    nacksSeen_ = 0U;
    hasAckedMode_ = false;
}

DeviceTick SerialDevice::tick(std::uint64_t nowMs) {
    DeviceTick out;
    if (host_ == nullptr) {
        return out;   // linkOpen 恒为 false
    }

    lastTickMs_ = nowMs;

    const mc::HostSession::PumpResult hostSide = host_->pump(nowMs);
    settleCommands(hostSide);

    out.commandLogChanged = logDirty_;
    logDirty_ = false;

    out.linkOpen = transport_ != nullptr && transport_->isOpen();
    out.sessionReady = host_->sessionReady();
    out.sessionLost = hostSide.sessionLost;
    out.handshakeCompleted = hostSide.handshakeCompleted;
    out.handshakeRounds = host_->handshakeRounds();

    out.telemetryAccepted = hostSide.telemetryAccepted;
    out.telemetryRejected = hostSide.telemetryRejected;
    if (hostSide.telemetryAccepted > 0U) {
        // 遥测是**唯一的**回读来源，因此本类把它留下来自己也要用一份。
        lastTelemetry_ = host_->lastTelemetry();
        hasTelemetry_ = true;
        out.hasTelemetry = true;
        out.telemetry = lastTelemetry_;
    }

    out.acksMatched = hostSide.acksMatched;
    out.naksReceived = hostSide.naksReceived;
    out.unexpectedFrames = hostSide.unexpectedFrames;
    out.droppedProbes = hostSide.droppedProbes;
    for (const mc::TimeoutEvent& event : hostSide.timeouts) {
        if (event.outcome == mc::TxOutcome::Fault) {
            ++out.timeoutsFaulted;
        } else if (event.outcome == mc::TxOutcome::Retry) {
            ++out.timeoutsRetried;
        }
    }

    out.linkDown = hostSide.linkDown;
    out.ackTimeoutMs = hostSide.ackTimeoutMs;
    return out;
}

// --- 命令 -------------------------------------------------------------------
//
// 前置条件与模拟器一致：会话未就绪就不发（那不是替设备做门禁，而是别发一条
// 注定被拒的东西）；STOP 例外，链路开着就发。

bool SerialDevice::recordCommand(const char* name, std::uint16_t seq, bool sent,
                                 std::uint8_t mode) {
    CommandRecord record;
    record.name = name;
    record.seq = seq;
    record.sentAtMs = sent ? lastTickMs_ : 0U;
    record.status = sent ? CommandStatus::Awaiting : CommandStatus::NotSent;
    log_.push_front(record);
    if (log_.size() > kMaxLogEntries) {
        log_.pop_back();
    }
    if (sent && mode != 0xFFU) {
        modeRequests_.push_back(ModeRequest{seq, mode});
    }
    logDirty_ = true;
    return sent;
}

bool SerialDevice::setMode(std::uint8_t mode) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("SET_MODE", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("SET_MODE", seq, host_->sendSetMode(mode, seq, lastTickMs_), mode);
}

bool SerialDevice::start() {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("START", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("START", seq, host_->sendStart(seq, lastTickMs_));
}

bool SerialDevice::stop() {
    if (host_ == nullptr) {
        return recordCommand("STOP", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    const bool sent = host_->sendStop(seq);

    CommandRecord record;
    record.name = "STOP";
    record.seq = seq;
    record.sentAtMs = lastTickMs_;
    // STOP 单向（ADR-004）：没有应答可等，"已发出"就是它的终点。
    record.status = sent ? CommandStatus::Unidirectional : CommandStatus::NotSent;
    log_.push_front(record);
    if (log_.size() > kMaxLogEntries) {
        log_.pop_back();
    }
    logDirty_ = true;
    return sent;
}

bool SerialDevice::setTarget(float target) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("SET_TARGET", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("SET_TARGET", seq, host_->sendSetTarget(target, seq, lastTickMs_));
}

bool SerialDevice::setPid(const mc::SetPid& pid) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("SET_PID", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("SET_PID", seq, host_->sendSetPid(pid, seq, lastTickMs_));
}

bool SerialDevice::clearFault(mc::FaultCode code) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("CLEAR_FAULT", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand(
        "CLEAR_FAULT", seq,
        host_->sendClearFault(static_cast<std::uint8_t>(code), seq, lastTickMs_));
}

bool SerialDevice::readParams() {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("READ_PARAMS", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("READ_PARAMS", seq, host_->sendReadParams(seq, lastTickMs_));
}

bool SerialDevice::home(const mc::HomeParam& param) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("HOME", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("HOME", seq, host_->sendHome(param, seq, lastTickMs_));
}

bool SerialDevice::setLimits(const mc::SoftLimits& limits) {
    if (host_ == nullptr || !host_->sessionReady()) {
        return recordCommand("SET_LIMITS", 0U, false);
    }
    const std::uint16_t seq = seq_++;
    return recordCommand("SET_LIMITS", seq, host_->sendSetLimits(limits, seq, lastTickMs_));
}

CommandRecord* SerialDevice::findAwaiting(std::uint16_t seq) {
    for (CommandRecord& record : log_) {
        if (record.seq == seq && record.status == CommandStatus::Awaiting) {
            return &record;
        }
    }
    return nullptr;
}

void SerialDevice::settleCommands(const mc::HostSession::PumpResult& hostSide) {
    // ① 新到的 NAK —— 对端明确拒绝，命令到此为止。
    const std::vector<mc::Nack>& nacks = host_->receivedNacks();
    for (; nacksSeen_ < nacks.size(); ++nacksSeen_) {
        CommandRecord* record = findAwaiting(nacks[nacksSeen_].rejectedSeq);
        if (record != nullptr) {
            record->status = CommandStatus::Nacked;
            record->nackReason = nacks[nacksSeen_].reason;
            logDirty_ = true;
        }
        // 被拒的 SET_MODE：它没有生效，也就不该留下任何"当前模式"的痕迹。
        for (auto it = modeRequests_.begin(); it != modeRequests_.end();) {
            it = (it->seq == nacks[nacksSeen_].rejectedSeq) ? modeRequests_.erase(it) : it + 1;
        }
    }

    // ② 本拍的超时事件（只认 Fault；Retry 表示还会重发，Dropped 是心跳漏拍）。
    for (const mc::TimeoutEvent& event : hostSide.timeouts) {
        if (event.outcome != mc::TxOutcome::Fault) {
            continue;
        }
        CommandRecord* record = findAwaiting(event.seq);
        if (record != nullptr) {
            record->status = CommandStatus::TimedOut;
            logDirty_ = true;
        }
        for (auto it = modeRequests_.begin(); it != modeRequests_.end();) {
            it = (it->seq == event.seq) ? modeRequests_.erase(it) : it + 1;
        }
    }

    // ③ 剩下的仍在"等待应答"、却已经不在等待表里的 —— 它是被应答了。
    for (CommandRecord& record : log_) {
        if (record.status != CommandStatus::Awaiting) {
            continue;
        }
        if (!host_->tracker().isPending(record.seq)) {
            record.status = CommandStatus::Acked;
            logDirty_ = true;

            // SET_MODE 被确认 = 设定值真的落地了，此时它才可以回答"设备是什么模式"。
            for (auto it = modeRequests_.begin(); it != modeRequests_.end();) {
                if (it->seq == record.seq) {
                    ackedMode_ = it->mode;
                    hasAckedMode_ = true;
                    it = modeRequests_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

// --- 只读观测（全部来自遥测或"已确认的设定值"）------------------------------

bool SerialDevice::sessionReady() const noexcept {
    return host_ != nullptr && host_->sessionReady();
}

std::uint64_t SerialDevice::handshakeRounds() const noexcept {
    return (host_ != nullptr) ? host_->handshakeRounds() : 0U;
}

mc::DeviceState SerialDevice::deviceState() const noexcept {
    if (!hasTelemetry_) {
        // 还没收到过遥测就不能猜：链路开着但设备状态未知，如实报 Disconnected
        // （= 没有可用会话），而不是报一个看起来正常的 Ready。
        return mc::DeviceState::Disconnected;
    }
    return static_cast<mc::DeviceState>(lastTelemetry_.state);
}

bool SerialDevice::faultLatched() const noexcept {
    if (!hasTelemetry_) {
        return false;
    }
    // 设备在回 Ready 时会把 faultCode 清成 0，所以"码非 0 且不在 Ready"
    // 就是锁存着 —— 这是设备状态机的不变式在遥测上的投影，不是第二套规则。
    return lastTelemetry_.faultCode != 0U &&
           lastTelemetry_.state != static_cast<std::uint8_t>(mc::DeviceState::Ready);
}

mc::FaultCode SerialDevice::faultCode() const noexcept {
    return static_cast<mc::FaultCode>(lastTelemetry_.faultCode);
}

mc::SimRunMode SerialDevice::runMode() const noexcept {
    // 没有回读就是没有回读：只报**被确认过**的设定值，一次都没确认过就报"停止控制"。
    return hasAckedMode_ ? static_cast<mc::SimRunMode>(ackedMode_) : mc::SimRunMode::Stopped;
}

bool SerialDevice::enabled() const noexcept {
    const mc::DeviceState state = deviceState();
    return state == mc::DeviceState::Running || state == mc::DeviceState::Stopping;
}

float SerialDevice::velocityDegPerSec() const noexcept { return lastTelemetry_.velocityDegPerSec; }
float SerialDevice::positionDeg() const noexcept { return lastTelemetry_.positionDeg; }
float SerialDevice::deviceTarget() const noexcept { return lastTelemetry_.target; }

// 这一位**只能**从设备报的遥测里来（v3 起的状态位组）。上位机没有别的路能知道
// "这一轮回零有没有建立零点"：被 STOP 打断的那条路径可以在两拍之间从 Stopping
// 回到 Ready（见 payload.hpp 的说明），状态采不到。
bool SerialDevice::homingAttained() const noexcept {
    return (lastTelemetry_.statusFlags & mc::kTelemetryFlagHomingAttained) != 0U;
}

bool SerialDevice::hasTelemetryReceived() const noexcept {
    return host_ != nullptr && host_->telemetryAccepted() > 0U;
}

bool SerialDevice::linkDown() const noexcept {
    return host_ != nullptr && host_->linkMonitor() != nullptr && host_->linkMonitor()->linkDown();
}

std::uint64_t SerialDevice::ackTimeoutMs() const noexcept {
    return (host_ != nullptr) ? host_->tracker().timeoutMs() : 0U;
}

bool SerialDevice::hasRttSample() const noexcept {
    return linkStats() != nullptr && linkStats()->rttSamples > 0U;
}

std::uint64_t SerialDevice::rttP95Ms() const noexcept {
    const mc::LinkMonitor* monitor = (host_ != nullptr) ? host_->linkMonitor() : nullptr;
    if (monitor == nullptr) {
        return 0U;
    }
    const std::optional<std::uint64_t> rtt = monitor->rttP95Ms();
    return rtt.has_value() ? *rtt : 0U;
}

const mc::LinkMonitor::Stats* SerialDevice::linkStats() const noexcept {
    const mc::LinkMonitor* monitor = (host_ != nullptr) ? host_->linkMonitor() : nullptr;
    return (monitor != nullptr) ? &monitor->stats() : nullptr;
}

std::uint64_t SerialDevice::framesSent() const noexcept {
    return (host_ != nullptr) ? host_->framesSent() : 0U;
}

std::uint64_t SerialDevice::hostEncodeFailures() const noexcept {
    return (host_ != nullptr) ? host_->encodeFailures() : 0U;
}

std::uint64_t SerialDevice::hostSendFailures() const noexcept {
    return (host_ != nullptr) ? host_->sendFailures() : 0U;
}

std::uint64_t SerialDevice::telemetryAcceptedTotal() const noexcept {
    return (host_ != nullptr) ? host_->telemetryAccepted() : 0U;
}

std::uint64_t SerialDevice::telemetryRejectedTotal() const noexcept {
    return (host_ != nullptr) ? host_->telemetryRejected() : 0U;
}

bool SerialDevice::hasParamState() const noexcept {
    return host_ != nullptr && host_->hasParamState();
}

mc::ParamState SerialDevice::lastParamState() const noexcept {
    return (host_ != nullptr) ? host_->lastParamState() : mc::ParamState{};
}

std::uint64_t SerialDevice::paramStatesReceivedTotal() const noexcept {
    return (host_ != nullptr) ? host_->paramStatesReceived() : 0U;
}

std::uint64_t SerialDevice::paramStatesRejectedTotal() const noexcept {
    return (host_ != nullptr) ? host_->paramStatesRejected() : 0U;
}

const mc::StreamDecoder::Stats& SerialDevice::decoderStats() const noexcept {
    static const mc::StreamDecoder::Stats kEmpty{};
    return (host_ != nullptr) ? host_->decoder().stats() : kEmpty;
}

const mc::AckStats& SerialDevice::ackStats() const noexcept {
    static const mc::AckStats kEmpty{};
    return (host_ != nullptr) ? host_->tracker().stats() : kEmpty;
}

void SerialDevice::setUplinkDropEveryNthByte(std::uint32_t) {}
void SerialDevice::setDownlinkDropEveryNthByte(std::uint32_t) {}
void SerialDevice::setUplinkLatencyPolls(std::uint32_t) {}
void SerialDevice::clearInjectedFaults() {}

}  // namespace mchost
