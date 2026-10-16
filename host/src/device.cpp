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

void SimulatedDevice::open() {
    if (link_ != nullptr) {
        return;
    }
    link_ = std::make_unique<Link>(simConfig_, monitorConfig_);
    nacksSeen_ = 0U;
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

const mc::StreamDecoder::Stats& SimulatedDevice::decoderStats() const noexcept {
    static const mc::StreamDecoder::Stats kEmpty{};
    return (link_ != nullptr) ? link_->host->decoder().stats() : kEmpty;
}

const mc::AckStats& SimulatedDevice::ackStats() const noexcept {
    static const mc::AckStats kEmpty{};
    return (link_ != nullptr) ? link_->host->tracker().stats() : kEmpty;
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

}  // namespace mchost
