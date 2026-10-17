#include "mc_host/axis_group.hpp"

#include <string>
#include <vector>

#include <QStringList>

namespace mchost {
namespace {

const char* groupedPhaseText(AxisGroup::Phase phase) noexcept {
    switch (phase) {
        case AxisGroup::Phase::Idle: return "空闲";
        case AxisGroup::Phase::Starting: return "正在启动";
        case AxisGroup::Phase::Moving: return "运动中";
        case AxisGroup::Phase::Done: return "已走完并停止";
        case AxisGroup::Phase::Aborted: return "已停止";
        case AxisGroup::Phase::Failed: return "出错并停止";
    }
    return "未知";
}

}  // namespace

AxisGroup::AxisGroup(QObject* parent) : QObject(parent) {
    for (auto& axis : axes_) {
        // **不设 parent**：所有权在 unique_ptr 手里。两处都要删就是双重释放，
        // 而那种崩溃发生在退出那一刻，看起来像"关不掉"。
        axis = std::make_unique<DeviceManager>(DeviceManager::StorePolicy::Disabled);
    }
    status_.axisOutcomes = QVector<CommandStatus>(static_cast<int>(kAxisCount),
                                                 CommandStatus::NotSent);
    timeline_.setInterval(kGroupTickMs);
    timeline_.setTimerType(Qt::PreciseTimer);
    connect(&timeline_, &QTimer::timeout, this, &AxisGroup::onGroupTick);
}

AxisGroup::~AxisGroup() {
    timeline_.stop();
}

void AxisGroup::useSimulatedAxes() {
    for (auto& axis : axes_) {
        (void)axis->useSimulatedDevice();
    }
}

bool AxisGroup::useSerialAxis(std::size_t index, const SerialTransport::Config& config,
                              QString& error) {
    if (index >= kAxisCount) {
        error = QStringLiteral("没有第 %1 轴").arg(index + 1U);
        return false;
    }
    return axes_[index]->useSerialDevice(config, error);
}

DeviceManager& AxisGroup::axis(std::size_t index) {
    // 越界是编程错误，不是运行期状况；夹到第 1 轴而不是造一个全局兜底对象 ——
    // 一个"借来顶一下"的 DeviceManager 会带着自己的定时器和设备，
    // 那种东西出现在生产路径上比崩溃更难查。
    return *axes_[(index < kAxisCount) ? index : 0U];
}

const DeviceManager& AxisGroup::axis(std::size_t index) const {
    return *axes_[(index < kAxisCount) ? index : 0U];
}

bool AxisGroup::connectAll() {
    bool all = true;
    for (auto& axis : axes_) {
        if (axis->isConnected()) {
            continue;
        }
        all = axis->connectDevice() && all;
    }
    // 节拍器**连上就开**：安全闸（任一轴故障就停全组）必须是一直武装着的，
    // 不能等到"按下协调启动"才上膛 —— 手动启的那一次同样需要它。
    if (all && !timeline_.isActive()) {
        timeline_.start();
    }
    emit groupChanged();
    return all;
}

void AxisGroup::disconnectAll() {
    if (status_.phase == Phase::Starting || status_.phase == Phase::Moving) {
        stopAllAxes();
    }
    timeline_.stop();
    for (auto& axis : axes_) {
        if (axis->isConnected()) {
            (void)axis->disconnectDevice();
        }
    }
    safetyTripped_ = false;
    startStage_ = StartStage::None;
    status_ = Status{};
    status_.axisOutcomes = QVector<CommandStatus>(static_cast<int>(kAxisCount),
                                                  CommandStatus::NotSent);
    plan_ = mc::GroupMovePlan{};
    emit groupChanged();
}

bool AxisGroup::allReady() const {
    for (const auto& axis : axes_) {
        if (!axis->isConnected() || axis->device() == nullptr ||
            !axis->device()->sessionReady()) {
            return false;
        }
    }
    return true;
}

QString AxisGroup::startBlockReason() const {
    QStringList problems;
    for (std::size_t i = 0U; i < kAxisCount; ++i) {
        const DeviceManager& manager = *axes_[i];
        const QString axisName = QStringLiteral("轴%1").arg(i + 1U);
        if (!manager.isConnected()) {
            problems << QStringLiteral("%1 未连接").arg(axisName);
        } else if (manager.device() == nullptr || !manager.device()->sessionReady()) {
            problems << QStringLiteral("%1 的会话还没就绪（正在握手）").arg(axisName);
        }
    }
    if (problems.isEmpty()) {
        return QString();
    }
    return QStringLiteral("还不能开动：%1").arg(problems.join(QStringLiteral("；")));
}

mc::GroupMovePlan AxisGroup::previewMove(double targetXDeg, double targetYDeg,
                                         double maxSpeedDegPerSec) const {
    std::vector<mc::AxisMoveInput> inputs;
    inputs.reserve(kAxisCount);
    const double targets[kAxisCount] = {targetXDeg, targetYDeg};
    for (std::size_t i = 0U; i < kAxisCount; ++i) {
        const IDevice* device = axes_[i]->device();
        const double from = (device != nullptr) ? static_cast<double>(device->positionDeg()) : 0.0;
        inputs.push_back(mc::AxisMoveInput{from, targets[i]});
    }
    return mc::planSynchronizedMove(inputs, maxSpeedDegPerSec);
}

bool AxisGroup::beginStartSequence() {
    startingTicks_ = 0;
    bool allSent = true;
    for (std::size_t i = 0U; i < kAxisCount; ++i) {
        DeviceManager& manager = *axes_[i];
        const bool sent = manager.sendSetMode(kGroupMode);
        const auto& log = manager.device()->commandLog();
        modeSeq_[i] = log.empty() ? 0U : log.front().seq;
        status_.axisOutcomes[static_cast<int>(i)] = CommandStatus::Awaiting;
        allSent = sent && allSent;
    }
    if (!allSent) {
        stopAllAxes();
        return false;
    }
    startStage_ = StartStage::WaitMode;
    if (!timeline_.isActive()) {
        timeline_.start();
    }
    setPhase(Phase::Starting, QStringLiteral("正在把两轴切到速度环…（切换落地后才发启动）"));
    return true;
}

bool AxisGroup::allAxesInMode(std::uint8_t mode) const {
    for (const auto& axis : axes_) {
        const IDevice* device = axis->device();
        // 读的是**设备报回来的**模式，不是"我们发过了"。启动联锁读的也是这个。
        if (device == nullptr || static_cast<std::uint8_t>(device->runMode()) != mode) {
            return false;
        }
    }
    return true;
}

CommandStatus AxisGroup::modeOutcome(std::size_t index) const {
    if (const CommandRecord* record = findCommand(*axes_[index], modeSeq_[index])) {
        return record->status;
    }
    return CommandStatus::Awaiting;
}

bool AxisGroup::advanceStarting() {
    if (startStage_ == StartStage::WaitMode) {
        // 1) "切模式"那一批：被拒 / 超时当场收摊，不干等到超时兜底。
        for (std::size_t i = 0U; i < kAxisCount; ++i) {
            const CommandStatus outcome = modeOutcome(i);
            if (outcome == CommandStatus::Nacked || outcome == CommandStatus::TimedOut) {
                stopAllAxes();
                startStage_ = StartStage::None;
                setPhase(Phase::Failed,
                         QStringLiteral("轴%1 的「切速度环」被拒或超时 —— 已立刻发出全组停止")
                             .arg(i + 1U));
                timeline_.stop();
                return false;
            }
        }
        if (!allAxesInMode(kGroupMode)) {
            if (++startingTicks_ > kStartTimeoutTicks) {
                stopAllAxes();
                startStage_ = StartStage::None;
                setPhase(Phase::Failed,
                         QStringLiteral("等两轴报上新模式，但一直没等到 —— 已发出全组停止"));
                timeline_.stop();
                return false;
            }
            return false;   // 还在等设备把新模式报上来
        }

        // 2) 模式落地了，**现在**才发启动：启动联锁读的就是刚报上来的模式。
        bool allSent = true;
        for (std::size_t i = 0U; i < kAxisCount; ++i) {
            allSent = axes_[i]->sendStart() && allSent;
        }
        if (allSent) {
            for (std::size_t i = 0U; i < kAxisCount; ++i) {
                allSent = axes_[i]->sendSetTarget(plan_.axes[i].speedDegPerSec) && allSent;
            }
        }
        if (!allSent) {
            stopAllAxes();
            startStage_ = StartStage::None;
            setPhase(Phase::Failed,
                     QStringLiteral("有一条启动 / 给速度的命令没发出去 —— 已立刻发出全组停止"));
            timeline_.stop();
            return false;
        }
        for (std::size_t i = 0U; i < kAxisCount; ++i) {
            const auto& log = axes_[i]->device()->commandLog();
            outcomeSeq_[i] = log.empty() ? 0U : log.front().seq;
            status_.axisOutcomes[static_cast<int>(i)] = CommandStatus::Awaiting;
        }
        startStage_ = StartStage::WaitTarget;
        return false;   // 下一拍再结算目标那条
    }

    // 3) 目标那条命令：等两轴都被确认。
    settleOutcomes();
    if (anyAxisFailed()) {
        stopAllAxes();
        startStage_ = StartStage::None;
        setPhase(Phase::Failed, QStringLiteral("轴组的运动命令被拒或超时 —— 已立刻 STOP 全组"));
        timeline_.stop();
        return false;
    }
    for (const CommandStatus outcome : status_.axisOutcomes) {
        if (outcome != CommandStatus::Acked) {
            return false;   // 还没结算完，继续等
        }
    }
    startStage_ = StartStage::None;
    setPhase(Phase::Moving,
             QStringLiteral("两轴已同步启动（公共时长 %1 s）：轴1 %2 deg/s，轴2 %3 deg/s")
                 .arg(plan_.durationSec, 0, 'f', 3)
                 .arg(plan_.axes[0].speedDegPerSec, 0, 'f', 3)
                 .arg(plan_.axes[1].speedDegPerSec, 0, 'f', 3));
    return true;
}

bool AxisGroup::moveTo(double targetXDeg, double targetYDeg, double maxSpeedDegPerSec) {
    lastRefusal_.clear();
    if (status_.phase == Phase::Starting || status_.phase == Phase::Moving) {
        lastRefusal_ = QStringLiteral("已经在动了：先停下，再发新的协调移动");
        setPhase(status_.phase, lastRefusal_);
        return false;
    }
    const QString block = startBlockReason();
    if (!block.isEmpty()) {
        lastRefusal_ = block;
        setPhase(Phase::Idle, block);
        return false;
    }

    plan_ = previewMove(targetXDeg, targetYDeg, maxSpeedDegPerSec);
    if (!plan_.ok()) {
        // 规划失败**不给出可用的速度**（见 mc::planSynchronizedMove 的约定），
        // 所以这里只能拒发 —— 绝不能发一组速度为 0 的"运动指令"：
        // 那在界面上看起来是"协调启动成功"，而实际什么都没发生。
        lastRefusal_ = QStringLiteral("这一趟走不了：%1")
                           .arg(QString::fromUtf8(mc::groupPlanErrorText(plan_.error)));
        setPhase(Phase::Idle, lastRefusal_);
        return false;
    }

    safetyTripped_ = false;
    status_.safetyDetail.clear();
    plannedDurationMs_ = plan_.durationSec * 1000.0;
    elapsedMs_ = 0.0;
    status_.elapsedMs = 0.0;
    status_.plannedDurationMs = plannedDurationMs_;

    if (!beginStartSequence()) {
        setPhase(Phase::Failed,
                 QStringLiteral("有一条「切速度环」的命令没发出去 —— 已立刻发出全组停止"));
        timeline_.stop();
        return false;
    }
    return true;
}

bool AxisGroup::stopAllAxes() {
    bool all = true;
    for (auto& axis : axes_) {
        all = axis->sendStop() && all;
    }
    return all;
}

bool AxisGroup::stopAll() {
    const bool sent = stopAllAxes();
    startStage_ = StartStage::None;
    if (!safetyTripped_) {
        setPhase(Phase::Aborted,
                 QStringLiteral("已发出全组停止：各轴按自己的加减速限幅收住，"
                                "已经走过的行程收不回来"));
    }
    timeline_.stop();
    emit groupChanged();
    return sent;
}

const CommandRecord* AxisGroup::findCommand(const DeviceManager& manager,
                                            std::uint16_t seq) const {
    if (manager.device() == nullptr || seq == 0U) {
        return nullptr;
    }
    for (const CommandRecord& record : manager.device()->commandLog()) {
        if (record.seq == seq) {
            return &record;
        }
    }
    return nullptr;
}

void AxisGroup::settleOutcomes() {
    for (std::size_t i = 0U; i < kAxisCount; ++i) {
        CommandStatus status = CommandStatus::NotSent;
        if (const CommandRecord* record = findCommand(*axes_[i], outcomeSeq_[i])) {
            status = record->status;
        }
        status_.axisOutcomes[static_cast<int>(i)] = status;
    }
}

bool AxisGroup::anyAxisFaulted() const {
    for (const auto& axis : axes_) {
        const IDevice* device = axis->device();
        if (device == nullptr) {
            continue;
        }
        if (device->faultLatched() || device->linkDown()) {
            return true;
        }
    }
    return false;
}

bool AxisGroup::anyAxisFailed() const {
    for (const CommandStatus status : status_.axisOutcomes) {
        if (status == CommandStatus::Nacked || status == CommandStatus::TimedOut) {
            return true;
        }
    }
    return false;
}

void AxisGroup::onGroupTick() {
    const Phase phase = status_.phase;
    if (phase != Phase::Starting && phase != Phase::Moving) {
        return;
    }

    // 1) 安全闸**排在最前**：有轴出事时，晚一拍停下来都可能是机械被拧一下。
    if (anyAxisFaulted()) {
        stopAllAxes();
        startStage_ = StartStage::None;
        safetyTripped_ = true;
        status_.safetyDetail = QStringLiteral("有轴进入故障或链路失效");
        setPhase(Phase::Aborted,
                 QStringLiteral("安全闸：%1 —— 已立刻 STOP 全组。"
                                "一台轴停了另一台还在走，对机械就是被拧一下。")
                     .arg(status_.safetyDetail));
        timeline_.stop();
        emit groupChanged();
        return;
    }

    // 2) 启动是**跨拍**的：切模式 → 等设备报上新模式 → 启动 + 给速度。
    //    advanceStarting() 返回 true = 这一拍刚进入 Moving。
    if (phase == Phase::Starting) {
        if (!advanceStarting()) {
            return;   // 还在推进，或者已经判定失败并发了 STOP
        }
        emit groupChanged();
    }

    // 3) 结算这一拍各轴的命令结局
    const QVector<CommandStatus> before = status_.axisOutcomes;
    settleOutcomes();
    if (anyAxisFailed()) {
        stopAllAxes();
        setPhase(Phase::Failed,
                 QStringLiteral("有轴的命令被拒或超时 —— 已立刻 STOP 全组"));
        timeline_.stop();
        emit groupChanged();
        return;
    }

    // 4) 时间到：走完计划的那一段，自动全组停
    elapsedMs_ += static_cast<double>(kGroupTickMs);
    status_.elapsedMs = elapsedMs_;
    if (plannedDurationMs_ > 0.0 && elapsedMs_ >= plannedDurationMs_) {
        stopAllAxes();
        setPhase(Phase::Done,
                 QStringLiteral("按计划走完（公共时长 %1 s，实走 %2 s），已发出全组停止")
                     .arg(plannedDurationMs_ / 1000.0, 0, 'f', 2)
                     .arg(elapsedMs_ / 1000.0, 0, 'f', 2));
        timeline_.stop();
        emit groupChanged();
        return;
    }

    if (before != status_.axisOutcomes) {
        emit groupChanged();
    }
}

void AxisGroup::setPhase(Phase phase, const QString& detail) {
    status_.phase = phase;
    status_.detail = detail;
    emit groupChanged();
}

QString AxisGroup::phaseText() const {
    return QString::fromUtf8(groupedPhaseText(status_.phase));
}

}  // namespace mchost
