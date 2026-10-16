#include "mc_host/device_manager.hpp"

#include <utility>

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

#include "mc/device_state.hpp"

namespace mchost {

QString DeviceManager::defaultHistoryPath() {
    // 用户数据目录而不是当前目录：双击启动时"当前目录"是哪儿只有天知道，
    // 历史文件散落在下载目录/桌面/安装目录里是查不出来的。
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.db");
}

DeviceManager::DeviceManager(QObject* parent) : QObject(parent) {
    device_ = std::make_unique<SimulatedDevice>();

    // 默认就把历史库打开。打不开只是"没有历史"，不是"不能用"：
    // 这种情况下 historyOpen() 为假、historyError() 有原因，历史页会照实显示。
    history_ = std::make_unique<HistoryStore>();
    (void)history_->open(defaultHistoryPath());

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

bool DeviceManager::openHistory(const QString& dbPath) {
    if (history_ == nullptr) {
        history_ = std::make_unique<HistoryStore>();
    }
    return history_->open(dbPath);
}

void DeviceManager::connectDevice() {
    if (connected_ || device_ == nullptr) {
        return;
    }
    if (!device_->open()) {
        // 打开失败在真硬件上是常态（端口被占用、线没插、权限不够）。
        // 这里**不启动定时器、也不进入"已连接"**：一个打不开的端口被显示成
        // "已连接"，会让所有后续判断都建立在假前提上。
        raiseAlarm(QStringLiteral("打开设备失败：%1")
                       .arg(QString::fromUtf8(openErrorName(device_->openError()))));
        emit connectionChanged(false);
        return;
    }
    clock_.start();
    timingClock_.start();
    lastTimingTickMs_ = -1;
    elapsedMs_ = 0.0;
    ticks_ = 0U;
    lastFaultLatched_ = false;
    connected_ = true;

    if (history_ != nullptr && history_->isOpen()) {
        (void)history_->beginSession(QString::fromUtf8(device_->kindName()), device_->isSimulated(),
                                     QDateTime::currentMSecsSinceEpoch());
    }

    timer_.start();
    emit connectionChanged(true);
}

bool DeviceManager::useSimulatedDevice() {
    if (connected_) {
        return false;
    }
    setDevice(std::make_unique<SimulatedDevice>());
    emit connectionChanged(false);
    return true;
}

bool DeviceManager::useSerialDevice(const SerialTransport::Config& config, QString& error) {
    if (connected_) {
        error = QStringLiteral("已连接中：请先断开再换数据源");
        return false;
    }
    if (config.portName.isEmpty()) {
        error = QStringLiteral("没有选择端口");
        return false;
    }
    setDevice(std::make_unique<SerialDevice>(config, mc::LinkMonitor::Config{}));
    emit connectionChanged(false);
    return true;
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

    // 收摊前把这一段的尾巴落盘并结束会话。少了这一步，最后不到半个刷写周期的
    // 样本会永远留在内存里，而历史页上那一段会显示成"数据到此为止"。
    if (history_ != nullptr && history_->isOpen()) {
        history_->endSession(QDateTime::currentMSecsSinceEpoch());
    }

    emit connectionChanged(false);
    emit commandLogChanged();
}

void DeviceManager::clearTelemetry() {
    telemetry_.clear();
}

void DeviceManager::clearAlarms() {
    alarms_.clear();
    emit alarmsCleared();
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

bool DeviceManager::sendClearFault(mc::FaultCode code) {
    if (!connected_ || device_ == nullptr) {
        return false;
    }
    const bool sent = device_->clearFault(code);
    emit commandLogChanged();
    return sent;
}

void DeviceManager::setUplinkDropEveryNthByte(int n) {
    if (device_ != nullptr) {
        device_->setUplinkDropEveryNthByte(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
}

void DeviceManager::setDownlinkDropEveryNthByte(int n) {
    if (device_ != nullptr) {
        device_->setDownlinkDropEveryNthByte(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
}

void DeviceManager::setUplinkLatencyPolls(int n) {
    if (device_ != nullptr) {
        device_->setUplinkLatencyPolls(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
}

void DeviceManager::clearInjectedFaults() {
    if (device_ != nullptr) {
        device_->clearInjectedFaults();
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
    // 报警也要进历史：实时列表只有最近 200 条，而"上周三那次断链是什么时候"
    // 只能从库里查。addEvent 会先把待写的样本落盘，保证它与样本的先后顺序正确。
    if (history_ != nullptr) {
        history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                           QDateTime::currentMSecsSinceEpoch(), text);
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
        // 落库走**同一条**数据（不是另取一份遥测）：历史里的曲线与当时屏幕上
        // 看到的曲线必须是同一组数字，否则事后追查会追到一个界面从没显示过的值。
        if (history_ != nullptr) {
            history_->addSample(sample);
        }
    }

    if (history_ != nullptr) {
        history_->tick(QDateTime::currentMSecsSinceEpoch());
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
    } else if (!latched && lastFaultLatched_) {
        // 降沿也要报：故障"什么时候没的"和"什么时候来的"一样是排查依据，
        // 而且这是**人工动作**的结果 —— 不说的话，日志里只剩一条"设备故障"，
        // 后来的人会以为故障还挂着。
        raiseAlarm(QStringLiteral("故障锁存已清除（人工确认 + 恢复校验通过）"));
    }
    lastFaultLatched_ = latched;

    if (tick.commandLogChanged) {
        // 命令记录会**变**（等待应答 → 已确认/被拒/超时），所以按 (会话, seq) 覆盖写。
        // 整本日志都过一遍（上限 64 条）：只处理"新增的那几条"要额外维护一套
        // "见到过哪些 seq"的状态，而那套状态一旦与日志的淘汰策略不一致就会漏记。
        if (history_ != nullptr) {
            for (const CommandRecord& record : device_->commandLog()) {
                history_->upsertCommand(record.sentAtMs, record.seq,
                                        QString::fromStdString(record.name),
                                        static_cast<int>(record.status),
                                        static_cast<int>(record.nackReason));
            }
        }
        emit commandLogChanged();
    }

    if ((ticks_ % static_cast<std::uint64_t>(kUiRefreshDivider)) == 0U) {
        emit statsRefreshed();
    }
}

}  // namespace mchost
