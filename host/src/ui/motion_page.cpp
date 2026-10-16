#include "mc_host/ui/motion_page.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/device_simulator.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

constexpr int kLogColumns = 5;

QString formatTime(double tMs) {
    return QStringLiteral("%1 s").arg(tMs / 1000.0, 0, 'f', 2);
}

}  // namespace

MotionPage::MotionPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    // --- 运行模式 -------------------------------------------------------------
    auto* modeGroup = new QGroupBox(QStringLiteral("运行模式（SET_MODE）"), this);
    auto* modeRow = new QHBoxLayout(modeGroup);
    modeCombo_ = new QComboBox(this);
    modeCombo_->setObjectName(QStringLiteral("modeCombo"));
    modeCombo_->addItem(QStringLiteral("停止控制（0）"));
    modeCombo_->addItem(QStringLiteral("速度环（1）"));
    modeCombo_->addItem(QStringLiteral("位置环（2）"));
    modeCombo_->setCurrentIndex(1);
    applyModeButton_ = new QPushButton(QStringLiteral("下发模式"), this);
    applyModeButton_->setObjectName(QStringLiteral("applyModeButton"));
    modeRow->addWidget(modeCombo_);
    modeRow->addWidget(applyModeButton_);
    auto* modeHint = new QLabel(
        QStringLiteral("设备只在 Ready 状态接受改模式：它改的是目标值的**单位语义**"
                       "（deg/s ↔ deg），运动中切换会把同一个数字换成另一种含义。"
                       "运行中下发会收到 NAK，原因就显示在下面的命令行里。"),
        this);
    modeHint->setWordWrap(true);
    modeHint->setStyleSheet(QStringLiteral("color:#666;"));
    modeRow->addWidget(modeHint, 1);
    root->addWidget(modeGroup);

    // --- 使能 / 停止 ----------------------------------------------------------
    auto* runGroup = new QGroupBox(QStringLiteral("使能与停止"), this);
    auto* runRow = new QHBoxLayout(runGroup);
    startButton_ = new QPushButton(QStringLiteral("使能并启动（START）"), this);
    startButton_->setObjectName(QStringLiteral("startButton"));
    stopButton_ = new QPushButton(QStringLiteral("停止（STOP，单向不等应答）"), this);
    stopButton_->setObjectName(QStringLiteral("stopButton"));
    stopButton_->setStyleSheet(
        QStringLiteral("background:#b3261e; color:white; font-weight:bold; padding:8px;"));
    runRow->addWidget(startButton_, 1);
    runRow->addWidget(stopButton_, 2);
    root->addWidget(runGroup);

    // --- 目标值 ---------------------------------------------------------------
    auto* targetGroup = new QGroupBox(QStringLiteral("目标值（SET_TARGET）"), this);
    auto* targetRow = new QHBoxLayout(targetGroup);
    targetSpin_ = new QDoubleSpinBox(this);
    targetSpin_->setObjectName(QStringLiteral("targetSpin"));
    targetSpin_->setRange(-384.0, 384.0);
    targetSpin_->setDecimals(1);
    targetSpin_->setSingleStep(10.0);
    targetSpin_->setValue(200.0);
    targetUnitLabel_ = new QLabel(QStringLiteral("deg/s"), this);
    applyTargetButton_ = new QPushButton(QStringLiteral("下发目标"), this);
    applyTargetButton_->setObjectName(QStringLiteral("applyTargetButton"));
    targetRow->addWidget(targetSpin_);
    targetRow->addWidget(targetUnitLabel_);
    targetRow->addWidget(applyTargetButton_);
    targetRow->addStretch(1);
    root->addWidget(targetGroup);

    // --- 上一次命令的结果 ------------------------------------------------------
    lastResultLabel_ = new QLabel(QStringLiteral("尚未下发任何命令"), this);
    lastResultLabel_->setObjectName(QStringLiteral("lastResultLabel"));
    QFont resultFont = lastResultLabel_->font();
    resultFont.setBold(true);
    lastResultLabel_->setFont(resultFont);
    lastResultLabel_->setWordWrap(true);
    root->addWidget(lastResultLabel_);

    // --- 命令行 -----------------------------------------------------------------
    logTable_ = new QTableWidget(0, kLogColumns, this);
    logTable_->setObjectName(QStringLiteral("commandLogTable"));
    logTable_->setHorizontalHeaderLabels({QStringLiteral("时刻"), QStringLiteral("命令"),
                                          QStringLiteral("SEQ"), QStringLiteral("结果"),
                                          QStringLiteral("说明")});
    logTable_->verticalHeader()->setVisible(false);
    logTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(logTable_, 1);

    connect(applyModeButton_, &QPushButton::clicked, this, &MotionPage::applyMode);
    connect(applyTargetButton_, &QPushButton::clicked, this, &MotionPage::applyTarget);
    connect(startButton_, &QPushButton::clicked, this,
            [this]() { manager_.sendStart(); lastResultLabel_->setText(QStringLiteral("已下发 START，等结果…")); });
    connect(stopButton_, &QPushButton::clicked, this,
            [this]() { manager_.sendStop(); lastResultLabel_->setText(QStringLiteral("已下发 STOP（单向，无应答）")); });
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &MotionPage::updateTargetRange);

    connect(&manager_, &DeviceManager::connectionChanged, this, &MotionPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &MotionPage::refresh);
    connect(&manager_, &DeviceManager::commandLogChanged, this, &MotionPage::refreshLog);

    onConnectionChanged(manager_.isConnected());
    refreshLog();
}

void MotionPage::updateTargetRange() {
    // 目标值的单位随模式变化 —— 这是协议里的语义（§5.3），界面必须跟着走，
    // 否则同一个数字在不同模式下会被当成不同的东西。
    const int mode = modeCombo_->currentIndex();
    if (mode == 2) {
        targetUnitLabel_->setText(QStringLiteral("deg"));
        targetSpin_->setRange(-100000.0, 100000.0);
        targetSpin_->setSingleStep(90.0);
    } else {
        targetUnitLabel_->setText(QStringLiteral("deg/s"));
        targetSpin_->setRange(-384.0, 384.0);
        targetSpin_->setSingleStep(10.0);
    }
}

void MotionPage::applyMode() {
    manager_.sendSetMode(modeCombo_->currentIndex());
    lastResultLabel_->setText(QStringLiteral("已下发 SET_MODE(%1)，等结果…")
                                  .arg(modeCombo_->currentIndex()));
}

void MotionPage::applyTarget() {
    manager_.sendSetTarget(targetSpin_->value());
    lastResultLabel_->setText(
        QStringLiteral("已下发 SET_TARGET(%1 %2)，等结果…")
            .arg(targetSpin_->value(), 0, 'f', 1)
            .arg(targetUnitLabel_->text()));
}

void MotionPage::onConnectionChanged(bool connected) {
    const IDevice* device = manager_.device();
    const bool ready = connected && device != nullptr && device->sessionReady();
    lastSessionReady_ = ready;

    applyModeButton_->setEnabled(ready);
    startButton_->setEnabled(ready);
    applyTargetButton_->setEnabled(ready);
    modeCombo_->setEnabled(ready);
    targetSpin_->setEnabled(ready);

    // STOP 只在"连接着"上做限制：会话没就绪也要能停。
    // 把手停在一个"未就绪"的会话后面，恰恰是最危险的时刻。
    stopButton_->setEnabled(connected);

    refresh();
}

void MotionPage::refresh() {
    // 会话就绪状态是**异步**变化的（握手要几拍），所以这里要跟着变，
    // 不能只在 connectionChanged 时算一次。
    const IDevice* device = manager_.device();
    const bool ready = manager_.isConnected() && device != nullptr && device->sessionReady();
    if (ready != lastSessionReady_) {
        onConnectionChanged(manager_.isConnected());
        return;
    }

    if (!ready) {
        startButton_->setToolTip(QStringLiteral("会话未就绪：握手完成前，设备会把命令拒掉"));
    } else {
        startButton_->setToolTip(QString());
    }
}

void MotionPage::refreshLog() {
    const IDevice* device = manager_.device();
    if (device == nullptr) {
        logTable_->setRowCount(0);
        return;
    }

    const std::deque<CommandRecord>& log = device->commandLog();
    logTable_->setRowCount(static_cast<int>(log.size()));

    int row = 0;
    for (const CommandRecord& record : log) {
        const auto set = [this, row](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            logTable_->setItem(row, column, item);
        };
        set(0, record.sentAtMs > 0.0 ? formatTime(record.sentAtMs) : QStringLiteral("—"));
        set(1, QString::fromStdString(record.name));
        set(2, QString::number(record.seq));
        set(3, QString::fromUtf8(commandStatusName(record.status)));

        QString detail;
        if (record.status == CommandStatus::Nacked) {
            detail = QString::fromUtf8(nackReasonName(record.nackReason));
        } else if (record.status == CommandStatus::TimedOut) {
            detail = QStringLiteral("未确认即超时 —— 运动指令按 Fault 处理，须人工确认");
        } else if (record.status == CommandStatus::NotSent) {
            detail = QStringLiteral("会话未就绪，本地就没有发出去");
        } else if (record.status == CommandStatus::Unidirectional) {
            detail = QStringLiteral("协议上无应答（ADR-004），已发出即终点");
        }
        set(4, detail);
        ++row;
    }
    logTable_->resizeColumnsToContents();

    if (!log.empty()) {
        const CommandRecord& newest = log.front();
        QString text = QStringLiteral("最近一条：%1 #%2 → %3")
                           .arg(QString::fromStdString(newest.name))
                           .arg(newest.seq)
                           .arg(QString::fromUtf8(commandStatusName(newest.status)));
        if (newest.status == CommandStatus::Nacked) {
            text += QStringLiteral("（%1）").arg(QString::fromUtf8(nackReasonName(newest.nackReason)));
        }
        lastResultLabel_->setText(text);
    }
}

}  // namespace mchost
