#include "mc_host/ui/motion_page.hpp"

#include <vector>

#include <QBrush>
#include <QColor>
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
#include "mc_host/interlock_text.hpp"

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

    // --- 启动联锁 ---------------------------------------------------------------
    //
    // 表里是"启动之前必须成立什么"，一次判完、按重要性列出没满足的。
    // 它不把启动按钮灰掉（理由见头文件）：按钮灰掉等于把人堵在门外，
    // 而清单告诉人"现在是哪一条没过、要把它变成什么样"。
    auto* interlockGroup = new QGroupBox(QStringLiteral("启动联锁（软件联锁，不是安全回路）"), this);
    auto* interlockLayout = new QVBoxLayout(interlockGroup);
    interlockSummary_ = new QLabel(this);
    interlockSummary_->setObjectName(QStringLiteral("interlockSummaryLabel"));
    interlockSummary_->setWordWrap(true);
    interlockLayout->addWidget(interlockSummary_);

    interlockTable_ = new QTableWidget(0, 4, this);
    interlockTable_->setObjectName(QStringLiteral("interlockTable"));
    interlockTable_->setHorizontalHeaderLabels({QStringLiteral("联锁条件"), QStringLiteral("等级"),
                                                QStringLiteral("状态"),
                                                QStringLiteral("现在是什么情况 / 要怎么做")});
    interlockTable_->verticalHeader()->setVisible(false);
    interlockTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    interlockTable_->horizontalHeader()->setStretchLastSection(true);
    interlockTable_->setMaximumHeight(160);
    interlockLayout->addWidget(interlockTable_);
    root->addWidget(interlockGroup);

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

    // --- 回零（HOME）----------------------------------------------------------
    //
    // 这一组的写法与上面三组不同：**按钮的结果不是"成功"**。
    // HOME 的应答只说明"回零开始了"，而零点有没有建立只能从设备状态看出来
    // （Home → Ready 才算成，进了 Fault 就是没走到）。所以这里下面挂的是一个
    // 状态标签，而不是"下发成功"那句话 —— 后者会把"开始了"说成"做完了"。
    auto* homeGroup = new QGroupBox(QStringLiteral("回零（HOME）"), this);
    auto* homeLayout = new QVBoxLayout(homeGroup);
    auto* homeRow = new QHBoxLayout();
    homeSpeedSpin_ = new QDoubleSpinBox(this);
    homeSpeedSpin_->setObjectName(QStringLiteral("homeSpeedSpin"));
    homeSpeedSpin_->setRange(0.1, 360.0);   // 上界与协议层的防荒谬上界同口径
    homeSpeedSpin_->setDecimals(1);
    homeSpeedSpin_->setSingleStep(5.0);
    homeSpeedSpin_->setValue(DeviceManager::kDefaultHomeSpeedDegPerSec);
    homeTimeoutSpin_ = new QDoubleSpinBox(this);
    homeTimeoutSpin_->setObjectName(QStringLiteral("homeTimeoutSpin"));
    homeTimeoutSpin_->setRange(0.1, 600.0);  // 10 分钟，与协议里的回零时限上界一致
    homeTimeoutSpin_->setDecimals(1);
    homeTimeoutSpin_->setSingleStep(5.0);
    homeTimeoutSpin_->setValue(30.0);
    homeButton_ = new QPushButton(QStringLiteral("回零（朝零点走并把它定义成 0）"), this);
    homeButton_->setObjectName(QStringLiteral("homeButton"));
    homeRow->addWidget(new QLabel(QStringLiteral("速度"), this));
    homeRow->addWidget(homeSpeedSpin_);
    homeRow->addWidget(new QLabel(QStringLiteral("deg/s，时限"), this));
    homeRow->addWidget(homeTimeoutSpin_);
    homeRow->addWidget(new QLabel(QStringLiteral("s"), this));
    homeRow->addWidget(homeButton_, 1);
    homeLayout->addLayout(homeRow);
    homeStatusLabel_ = new QLabel(this);
    homeStatusLabel_->setObjectName(QStringLiteral("homeStatusLabel"));
    homeStatusLabel_->setWordWrap(true);
    homeLayout->addWidget(homeStatusLabel_);
    auto* homeHint = new QLabel(
        QStringLiteral("回零速度是**幅值**：朝哪边走由设备按自己的坐标判断"
                       "（真设备上是行程开关 / 编码器 Z 相的位置）—— 让上位机指定方向，"
                       "等于让一个看不见坐标系的人指挥电机往哪边撞。回零中 STOP 有效，"
                       "但那样**零点不会建立**。"),
        this);
    homeHint->setWordWrap(true);
    homeHint->setStyleSheet(QStringLiteral("color:#666;"));
    homeLayout->addWidget(homeHint);
    root->addWidget(homeGroup);

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
    connect(homeButton_, &QPushButton::clicked, this, &MotionPage::applyHome);
    connect(startButton_, &QPushButton::clicked, this, [this]() {
        // 发没发出去必须如实说：门禁拦下时按返回值为假，此时再写一句
        // "已下发 START"就是句假话 —— 而这句话恰好是操作员下一步的依据。
        // 被拦的原因有三类（没权限 / 没连上 / 联锁没满足），
        // lastRefusalText() 把三类都收好了，不必在这里分辨。
        const bool sent = manager_.sendStart();
        lastResultLabel_->setText(sent ? QStringLiteral("已下发 START，等结果…")
                                       : manager_.lastRefusalText());
        refreshInterlocks();
    });
    connect(stopButton_, &QPushButton::clicked, this, [this]() {
        if (manager_.sendStop()) {
            lastResultLabel_->setText(QStringLiteral("已下发 STOP（单向，无应答）"));
        } else {
            lastResultLabel_->setText(manager_.lastRefusalText());
        }
    });
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &MotionPage::updateTargetRange);

    connect(&manager_, &DeviceManager::connectionChanged, this, &MotionPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &MotionPage::refresh);
    connect(&manager_, &DeviceManager::commandLogChanged, this, &MotionPage::refreshLog);
    // 回零的进展**不在 statsRefreshed 那条节流里**：它由状态变化驱动（进 Home、
    // 回 Ready、进 Fault），而 20 Hz 的重绘会把这些一次性事件刷成同一句话。
    connect(&manager_, &DeviceManager::homeChanged, this, &MotionPage::refreshHomeStatus);

    onConnectionChanged(manager_.isConnected());
    refreshLog();
    refreshInterlocks();
    refreshHomeStatus();
}

void MotionPage::refreshInterlocks() {
    const std::vector<DeviceManager::InterlockView> view = manager_.interlockView();

    int unmetBlocks = 0;
    int unmetWarns = 0;
    for (const DeviceManager::InterlockView& row : view) {
        if (!row.satisfied) {
            if (row.severity == mc::InterlockSeverity::Block) {
                ++unmetBlocks;
            } else {
                ++unmetWarns;
            }
        }
    }
    // 汇总必须说清"拦不拦"，而不能只说"有几条没满足"：
    // 一条提示级的没满足和一条拦启动的没满足，处置完全不同。
    if (unmetBlocks > 0) {
        interlockSummary_->setText(
            QStringLiteral("现在**按不动启动**：%1 条拦启动的条件没满足（另有 %2 条提示）"
                           "—— 按下面的清单逐条处理。这是**软件**联锁，"
                           "它代替不了急停/断电那样的物理安全回路。")
                .arg(unmetBlocks)
                .arg(unmetWarns));
        interlockSummary_->setStyleSheet(QStringLiteral("color:#b00020; font-weight:bold;"));
    } else {
        interlockSummary_->setText(
            unmetWarns > 0
                ? QStringLiteral("启动联锁都满足（另有 %1 条提示，不拦启动）").arg(unmetWarns)
                : QStringLiteral("启动联锁都满足"));
        interlockSummary_->setStyleSheet(QStringLiteral("color:#1b5e20;"));
    }

    interlockTable_->setRowCount(static_cast<int>(view.size()));
    for (int row = 0; row < static_cast<int>(view.size()); ++row) {
        const DeviceManager::InterlockView& result = view[static_cast<std::size_t>(row)];
        interlockTable_->setItem(row, 0,
                                 new QTableWidgetItem(interlockRuleText(result.code)));
        interlockTable_->setItem(
            row, 1, new QTableWidgetItem(interlockSeverityText(result.severity)));
        // 状态列用「满足 / 没满足」而不是「是 / 否」：这一个字就是操作员要找的那一行。
        auto* stateItem =
            new QTableWidgetItem(result.satisfied ? QStringLiteral("满足")
                                                  : QStringLiteral("没满足"));
        if (!result.satisfied) {
            stateItem->setForeground(
                QBrush(result.severity == mc::InterlockSeverity::Block
                           ? QColor(QStringLiteral("#b00020"))
                           : QColor(QStringLiteral("#8a5a00"))));
        }
        interlockTable_->setItem(row, 2, stateItem);
        interlockTable_->setItem(row, 3, new QTableWidgetItem(result.text));
    }
    interlockTable_->resizeColumnsToContents();
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
    if (!manager_.sendSetMode(modeCombo_->currentIndex())) {
        lastResultLabel_->setText(manager_.lastRefusalText());
        return;
    }
    lastResultLabel_->setText(QStringLiteral("已下发 SET_MODE(%1)，等结果…")
                                  .arg(modeCombo_->currentIndex()));
}

void MotionPage::applyTarget() {
    if (!manager_.sendSetTarget(targetSpin_->value())) {
        lastResultLabel_->setText(manager_.lastRefusalText());
        return;
    }
    lastResultLabel_->setText(
        QStringLiteral("已下发 SET_TARGET(%1 %2)，等结果…")
            .arg(targetSpin_->value(), 0, 'f', 1)
            .arg(targetUnitLabel_->text()));
}

void MotionPage::applyHome() {
    mc::HomeParam param;
    param.speedDegPerSec = static_cast<float>(homeSpeedSpin_->value());
    param.timeoutMs = static_cast<std::uint32_t>(homeTimeoutSpin_->value() * 1000.0);

    if (!manager_.sendHome(param)) {
        // 发没发出去如实说（没权限 / 没连上 / 联锁没满足 / 参数不合法）。
        // 在这里补一句"回零已开始"是最不能做的事：那会让操作员按一个假前提离开。
        lastResultLabel_->setText(manager_.lastRefusalText());
        return;
    }
    lastResultLabel_->setText(QStringLiteral("已下发 HOME（%1 deg/s，时限 %2 s）—— "
                                            "应答只说明回零**开始了**，"
                                            "零点有没有建立看下面那行状态")
                                  .arg(homeSpeedSpin_->value(), 0, 'f', 1)
                                  .arg(homeTimeoutSpin_->value(), 0, 'f', 1));
    refreshHomeStatus();
}

void MotionPage::refreshHomeStatus() {
    const HomeStatus& status = manager_.homeStatus();
    const IDevice* device = manager_.device();
    const QString stateName =
        QString::fromUtf8(mc::deviceStateName(device != nullptr
                                                 ? device->deviceState()
                                                 : mc::DeviceState::Disconnected));

    QString headline;
    QString color;
    switch (status.phase) {
        case HomePhase::Idle:
            headline = QStringLiteral("本次连接还没有回过零");
            color = QStringLiteral("#666");
            break;
        case HomePhase::Started:
            headline = QStringLiteral("回零进行中（还没结束，别走开）");
            color = QStringLiteral("#8a5a00");
            break;
        case HomePhase::Succeeded:
            headline = QStringLiteral("回零成功：零点已经建立");
            color = QStringLiteral("#1b5e20");
            break;
        case HomePhase::Aborted:
            // 这一格与"成功"在曲线页上都表现为"停下来了"，含义却相反 ——
            // 所以它必须自己占一行，而且要说清零点**没有**建立。
            headline = QStringLiteral("回零被中止：零点**没有**建立，现在的坐标还不是基准");
            color = QStringLiteral("#8a5a00");
            break;
        case HomePhase::Failed:
            headline = QStringLiteral("回零失败：零点**没有**建立");
            color = QStringLiteral("#b00020");
            break;
    }

    QString text = QStringLiteral("%1（设备状态 %2）").arg(headline).arg(stateName);
    if (!status.detail.isEmpty()) {
        text += QStringLiteral("\n") + status.detail;
    }
    homeStatusLabel_->setText(text);
    homeStatusLabel_->setStyleSheet(QStringLiteral("color:%1;").arg(color));
}

void MotionPage::onConnectionChanged(bool connected) {
    const IDevice* device = manager_.device();
    const bool ready = connected && device != nullptr && device->sessionReady();
    lastSessionReady_ = ready;

    applyModeButton_->setEnabled(ready);
    startButton_->setEnabled(ready);
    applyTargetButton_->setEnabled(ready);
    homeButton_->setEnabled(ready);
    homeSpeedSpin_->setEnabled(ready);
    homeTimeoutSpin_->setEnabled(ready);
    modeCombo_->setEnabled(ready);
    targetSpin_->setEnabled(ready);

    // STOP 只在"连接着"上做限制：会话没就绪也要能停。
    // 把手停在一个"未就绪"的会话后面，恰恰是最危险的时刻。
    stopButton_->setEnabled(connected);

    refresh();
    refreshHomeStatus();
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
    refreshInterlocks();
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
