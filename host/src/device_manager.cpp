#include "mc_host/device_manager.hpp"

#include <utility>

#include "mc/device_state.hpp"

namespace mchost {

DeviceManager::DeviceManager(QObject* parent) : QObject(parent) {
    device_ = std::make_unique<SimulatedDevice>();

    timer_.setInterval(kTickIntervalMs);
    // 精确定时器：默认的粗粒度定时器在 Windows/WSL 上有 ±十几毫秒的抖动，
    // 而它直接就是仿真的积分步长（dt），抖动会变成速度曲线的毛刺。
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &DeviceManager::onTick);
}

DeviceManager::~DeviceManager() = default;

void DeviceManager::setDevice(std::unique_ptr<IDevice> device) {
    if (connected_) {
        // 先断开：否则旧设备的链路会被悄悄丢掉（而且测试里注入的假设备也一样）。
        disconnectDevice();
    }
    device_ = std::move(device);
}

void DeviceManager::connectDevice() {
    if (connected_ || device_ == nullptr) {
        return;
    }
    device_->open();
    clock_.start();
    timingClock_.start();
    lastTimingTickMs_ = -1;
    elapsedMs_ = 0.0;
    ticks_ = 0U;
    lastFaultLatched_ = false;
    connected_ = true;
    timer_.start();
    emit connectionChanged(true);
}

void DeviceManager::disconnectDevice() {
    if (!connected_) {
        return;
    }
    timer_.stop();
    if (device_ != nullptr) {
        device_->close();
    }
    connected_ = false;
    emit connectionChanged(false);
    emit commandLogChanged();
}

void DeviceManager::clearTelemetry() {
    telemetry_.clear();
}

bool DeviceManager::sendSetMode(int mode) {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    const bool sent = device_->setMode(static_cast<std::uint8_t>(mode));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendStart() {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    const bool sent = device_->start();
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendStop() {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    // STOP 是最不该被本地条件拦住的一条：链路开着就发。
    // （设备侧的状态机规定 STOP 在任何状态都接受。）
    const bool sent = device_->stop();
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendSetTarget(double target) {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    const bool sent = device_->setTarget(static_cast<float>(target));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendSetPid(double kp, double ki, double kd) {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    mc::SetPid pid;
    pid.kp = static_cast<float>(kp);
    pid.ki = static_cast<float>(ki);
    pid.kd = static_cast<float>(kd);
    const bool sent = device_->setPid(pid);
    emit commandLogChanged();
    return sent;
}

void DeviceManager::injectUplinkDrop(bool enabled) {
    if (device_ != nullptr) {
        device_->setUplinkDropEveryNthByte(enabled ? 1U : 0U);
    }
}

void DeviceManager::injectDownlinkDrop(bool enabled) {
    if (device_ != nullptr) {
        device_->setDownlinkDropEveryNthByte(enabled ? 1U : 0U);
    }
}

void DeviceManager::injectUplinkLatency(int polls) {
    if (device_ != nullptr) {
        device_->setUplinkLatencyPolls(static_cast<std::uint32_t>(polls < 0 ? 0 : polls));
    }
}

void DeviceManager::enableTimingDiagnostics(bool enabled, std::size_t capacity) {
    tickBusyUs_.clear();
    tickIntervalUs_.clear();
    timingSampleCount_ = 0U;
    lastTimingTickMs_ = -1;
    timingEnabled_ = enabled;

    if (!enabled || capacity == 0U) {
        timingCapacity_ = 0U;
        return;
    }

    timingCapacity_ = capacity;
    // 预置容量：记样本时用下标写入而不是 append，避免"采样本身在分配内存"
    // 这件事污染被测量的那一拍。
    tickBusyUs_.resize(static_cast<int>(capacity));
    tickIntervalUs_.resize(static_cast<int>(capacity));
}

void DeviceManager::recordTimingSample(qint64 busyUs, qint64 intervalUs) {
    if (timingCapacity_ == 0U) {
        return;
    }
    const int index = static_cast<int>(timingSampleCount_ % timingCapacity_);
    tickBusyUs_[index] = busyUs;
    tickIntervalUs_[index] = intervalUs;
    ++timingSampleCount_;
}

void DeviceManager::raiseAlarm(const QString& text) {
    AlarmRecord record;
    record.tMs = elapsedMs_;
    record.text = text;
    alarms_.push_back(record);
    while (alarms_.size() > kMaxAlarms) {
        alarms_.removeFirst();
    }
    emit alarmRaised(text);
}

void DeviceManager::onTick() {
    if (device_ == nullptr) {
        return;
    }

    if (!timingEnabled_) {
        // 产品路径：一层都不多包。诊断关闭时这里与以前完全一样
        // —— 一个 nullptr 判断，不是"看起来关了其实还在测"。
        tickOnce();
        return;
    }

    const qint64 t0 = timingClock_.nsecsElapsed();
    tickOnce();
    const qint64 t1 = timingClock_.nsecsElapsed();

    const qint64 nowMs = clock_.elapsed();
    if (lastTimingTickMs_ >= 0) {
        // 第一拍没有"上一拍"，间隔样本必须从第二拍才开始记 —— 否则会凭空录进
        // 一个"自连接以来的时长"，把 P99 拉成一个毫无意义的数字。
        recordTimingSample((t1 - t0) / 1000, (nowMs - lastTimingTickMs_) * 1000);
    }
    lastTimingTickMs_ = nowMs;
}

void DeviceManager::tickOnce() {
    const double nowMs = static_cast<double>(clock_.elapsed());
    const DeviceTick tick = device_->tick(static_cast<std::uint64_t>(nowMs));
    elapsedMs_ = nowMs;
    ++ticks_;

    // 链路在下面被关掉（例如传输断开）时，这里就收摊 —— 不停在一个"连着但没数据"的假状态。
    if (!tick.linkOpen) {
        timer_.stop();
        connected_ = false;
        emit connectionChanged(false);
        emit commandLogChanged();
        return;
    }

    if (tick.hasTelemetry) {
        TelemetrySample sample;
        sample.tMs = nowMs;
        sample.velocityDegPerSec = tick.telemetry.velocityDegPerSec;
        sample.positionDeg = tick.telemetry.positionDeg;
        sample.target = tick.telemetry.target;
        sample.state = tick.telemetry.state;
        sample.faultCode = tick.telemetry.faultCode;
        sample.pwmOutput = tick.telemetry.pwmOutput;
        telemetry_.push(sample);
    }

    // --- 报警：只报"安全事件"，不把每一件小事都变成报警 --------------------
    if (tick.sessionLost) {
        raiseAlarm(QStringLiteral("链路失效：会话已作废，正在重新握手"));
    }
    if (tick.timeoutsFaulted > 0U) {
        raiseAlarm(QStringLiteral("%1 条命令超时（判 Fault，需人工确认）")
                       .arg(static_cast<qulonglong>(tick.timeoutsFaulted)));
    }
    const bool latched = (device_->faultLatched());
    if (latched && !lastFaultLatched_) {
        raiseAlarm(QStringLiteral("设备故障：%1（已锁存，须人工确认才能清除）")
                       .arg(QString::fromLatin1(mc::faultCodeName(device_->faultCode()))));
    }
    lastFaultLatched_ = latched;

    if (tick.commandLogChanged) {
        emit commandLogChanged();
    }

    if ((ticks_ % static_cast<std::uint64_t>(kUiRefreshDivider)) == 0U) {
        emit statsRefreshed();
    }
}

}  // namespace mchost
