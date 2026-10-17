#include "mc_host/ui/axis_page.hpp"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "mc/axis_group.hpp"
#include "mc/device_state.hpp"
#include "mc_host/axis_group.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/log_text.hpp"

namespace mchost {
namespace {

constexpr int kAxisRows = 2;
constexpr int kAxisColumns = 5;

}  // namespace

AxisPage::AxisPage(AxisGroup& group, QWidget* parent) : QWidget(parent), group_(group) {
    auto* root = new QVBoxLayout(this);

    // --- 连接 ------------------------------------------------------------------
    auto* linkGroup = new QGroupBox(QStringLiteral("两轴的链路（各自独立的一台驱动器）"), this);
    auto* linkRow = new QHBoxLayout(linkGroup);
    connectButton_ = new QPushButton(QStringLiteral("连接两轴"), this);
    connectButton_->setObjectName(QStringLiteral("axisConnectButton"));
    linkRow->addWidget(connectButton_);
    connectLabel_ = new QLabel(QStringLiteral("未连接"), this);
    connectLabel_->setObjectName(QStringLiteral("axisConnectLabel"));
    linkRow->addWidget(connectLabel_, 1);
    auto* linkHint = new QLabel(
        QStringLiteral("轴组是**上位机侧**的一层编排：两轴各自是一条完整的链路、一台驱动器，"
                       "协议里没有\"轴组\"这个概念。所以它不要求任何一方多懂一件事，"
                       "换真设备时把某一轴的数据源换成串口即可。"),
        this);
    linkHint->setWordWrap(true);
    linkHint->setStyleSheet(QStringLiteral("color:#666;"));
    linkRow->addWidget(linkHint, 2);
    root->addWidget(linkGroup);

    // --- 协调移动 --------------------------------------------------------------
    auto* moveGroup = new QGroupBox(QStringLiteral("协调移动（速度缩放式同步）"), this);
    auto* grid = new QGridLayout(moveGroup);
    grid->addWidget(new QLabel(QStringLiteral("轴1 目标（deg）："), this), 0, 0);
    targetXSpin_ = new QDoubleSpinBox(this);
    targetXSpin_->setObjectName(QStringLiteral("axisTargetXSpin"));
    targetXSpin_->setRange(-100000.0, 100000.0);
    targetXSpin_->setDecimals(2);
    targetXSpin_->setValue(200.0);
    grid->addWidget(targetXSpin_, 0, 1);

    grid->addWidget(new QLabel(QStringLiteral("轴2 目标（deg）："), this), 0, 2);
    targetYSpin_ = new QDoubleSpinBox(this);
    targetYSpin_->setObjectName(QStringLiteral("axisTargetYSpin"));
    targetYSpin_->setRange(-100000.0, 100000.0);
    targetYSpin_->setDecimals(2);
    targetYSpin_->setValue(60.0);
    grid->addWidget(targetYSpin_, 0, 3);

    grid->addWidget(new QLabel(QStringLiteral("公共最大速度（deg/s）："), this), 1, 0);
    speedSpin_ = new QDoubleSpinBox(this);
    speedSpin_->setObjectName(QStringLiteral("axisSpeedSpin"));
    speedSpin_->setRange(0.1, 3000.0);
    speedSpin_->setDecimals(2);
    speedSpin_->setValue(100.0);
    grid->addWidget(speedSpin_, 1, 1);

    moveButton_ = new QPushButton(QStringLiteral("协调启动"), this);
    moveButton_->setObjectName(QStringLiteral("axisMoveButton"));
    grid->addWidget(moveButton_, 1, 2);
    stopButton_ = new QPushButton(QStringLiteral("全组停止"), this);
    stopButton_->setObjectName(QStringLiteral("axisStopButton"));
    grid->addWidget(stopButton_, 1, 3);

    planLabel_ = new QLabel(this);
    planLabel_->setObjectName(QStringLiteral("axisPlanLabel"));
    planLabel_->setWordWrap(true);
    grid->addWidget(planLabel_, 2, 0, 1, 4);
    root->addWidget(moveGroup);

    // --- 状态 ------------------------------------------------------------------
    auto* stateGroup = new QGroupBox(QStringLiteral("状态"), this);
    auto* stateLayout = new QVBoxLayout(stateGroup);
    statusLabel_ = new QLabel(QStringLiteral("空闲"), this);
    statusLabel_->setObjectName(QStringLiteral("axisStatusLabel"));
    statusLabel_->setWordWrap(true);
    stateLayout->addWidget(statusLabel_);
    safetyLabel_ = new QLabel(this);
    safetyLabel_->setObjectName(QStringLiteral("axisSafetyLabel"));
    safetyLabel_->setWordWrap(true);
    stateLayout->addWidget(safetyLabel_);

    axisTable_ = new QTableWidget(kAxisRows, kAxisColumns, this);
    axisTable_->setObjectName(QStringLiteral("axisTable"));
    axisTable_->setHorizontalHeaderLabels({QStringLiteral("轴"), QStringLiteral("设备状态"),
                                           QStringLiteral("位置(deg)"), QStringLiteral("速度(deg/s)"),
                                           QStringLiteral("这一步的结局")});
    axisTable_->verticalHeader()->setVisible(false);
    axisTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    axisTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    stateLayout->addWidget(axisTable_);
    root->addWidget(stateGroup);

    auto* footer = new QLabel(
        QStringLiteral("⚠️ 这不是插补：中间不约束两轴的轨迹关系，只保证**同时出发、同时到达**。"
                       "单线程下\"同时\"的含义是两轴命令落在同一拍（10 ms）里 —— 偏差不超过一拍，"
                       "不能说成\"严格同步\"。\n"
                       "⚠️ 安全闸：任一轴进入故障或链路失效，立刻 STOP 全组 —— "
                       "一台轴停了另一台还在走，对机械就是被拧一下。"),
        this);
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(footer);

    connect(connectButton_, &QPushButton::clicked, this, &AxisPage::applyConnect);
    connect(moveButton_, &QPushButton::clicked, this, &AxisPage::applyMove);
    connect(stopButton_, &QPushButton::clicked, this, &AxisPage::applyStop);
    connect(targetXSpin_, &QDoubleSpinBox::valueChanged, this, &AxisPage::updatePlanPreview);
    connect(targetYSpin_, &QDoubleSpinBox::valueChanged, this, &AxisPage::updatePlanPreview);
    connect(speedSpin_, &QDoubleSpinBox::valueChanged, this, &AxisPage::updatePlanPreview);
    connect(&group_, &AxisGroup::groupChanged, this, &AxisPage::refreshStatus);
    // 实时数值跟着两轴自己的刷新节拍走（20 Hz），不额外起一个定时器。
    // 这里连的是 refreshStatus 而不是 refresh：**"两轴握手好了没有"本身没有对应的
    // groupChanged**（就绪不是一次阶段变化），只有跟着节拍重算才看得见 ——
    // 否则连上之后按钮会一直灰/一直亮，状态栏一直停在"正在握手…"。
    // refreshStatus 里已经调了 refresh，所以不再单独连 refresh。
    connect(&group_.axis(0), &DeviceManager::statsRefreshed, this, &AxisPage::refreshStatus);
    connect(&group_.axis(1), &DeviceManager::statsRefreshed, this, &AxisPage::refreshStatus);

    refreshStatus();
    refresh();
}

void AxisPage::applyConnect() {
    group_.useSimulatedAxes();
    const bool ok = group_.connectAll();
    connectLabel_->setText(
        ok ? QStringLiteral("两轴已连接，正在握手…（握手完成后才能开动）")
           : QStringLiteral("有轴没连上：%1").arg(group_.startBlockReason()));
    refreshStatus();
}

void AxisPage::applyMove() {
    const bool started = group_.moveTo(targetXSpin_->value(), targetYSpin_->value(),
                                       speedSpin_->value());
    if (!started) {
        // 没开动就必须说清为什么 —— "点了没反应"是最难查的一种。
        statusLabel_->setText(QStringLiteral("没有开动：%1").arg(group_.lastRefusalText()));
    }
    refreshStatus();
}

void AxisPage::applyStop() {
    (void)group_.stopAll();
    refreshStatus();
}

void AxisPage::updatePlanPreview() {
    const mc::GroupMovePlan plan =
        group_.previewMove(targetXSpin_->value(), targetYSpin_->value(), speedSpin_->value());
    if (!plan.ok()) {
        planLabel_->setText(QStringLiteral("预览：这一趟走不了 —— %1")
                                .arg(QString::fromUtf8(mc::groupPlanErrorText(plan.error))));
        return;
    }
    planLabel_->setText(QStringLiteral("预览：公共时长 %1 s（轴1 %2 deg/s，轴2 %3 deg/s，"
                                       "行程 %4 / %5 deg）")
                            .arg(plan.durationSec, 0, 'f', 3)
                            .arg(plan.axes[0].speedDegPerSec, 0, 'f', 3)
                            .arg(plan.axes[1].speedDegPerSec, 0, 'f', 3)
                            .arg(plan.axes[0].deltaDeg, 0, 'f', 2)
                            .arg(plan.axes[1].deltaDeg, 0, 'f', 2));
}

void AxisPage::refreshStatus() {
    const AxisGroup::Status& status = group_.status();
    const QString block = group_.startBlockReason();

    statusLabel_->setText(QStringLiteral("阶段：%1 —— %2")
                              .arg(group_.phaseText(), status.detail.isEmpty()
                                                          ? QStringLiteral("（还没有动作）")
                                                          : status.detail));
    if (status.safetyDetail.isEmpty()) {
        safetyLabel_->setText(QStringLiteral("安全闸：待命（任一轴故障或链路失效即 STOP 全组）"));
        safetyLabel_->setStyleSheet(QStringLiteral("color:#666;"));
    } else {
        safetyLabel_->setText(QStringLiteral("安全闸已触发：%1").arg(status.safetyDetail));
        safetyLabel_->setStyleSheet(QStringLiteral("color:#b00;font-weight:bold;"));
    }

    moveButton_->setEnabled(block.isEmpty());
    connectButton_->setEnabled(!group_.allReady());
    if (!block.isEmpty() && group_.status().phase == AxisGroup::Phase::Idle) {
        connectLabel_->setText(block);
    } else if (group_.allReady()) {
        connectLabel_->setText(QStringLiteral("两轴会话就绪"));
    }
    refresh();
}

void AxisPage::refresh() {
    for (int row = 0; row < kAxisRows; ++row) {
        const DeviceManager& manager = group_.axis(static_cast<std::size_t>(row));
        const IDevice* device = manager.device();

        auto* nameItem = new QTableWidgetItem(QStringLiteral("轴%1").arg(row + 1));
        axisTable_->setItem(row, 0, nameItem);

        if (device == nullptr || !manager.isConnected()) {
            axisTable_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("未连接")));
            axisTable_->setItem(row, 2, new QTableWidgetItem(QStringLiteral("-")));
            axisTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("-")));
        } else {
            QString stateText = QString::fromUtf8(mc::deviceStateName(device->deviceState()));
            if (!device->sessionReady()) {
                stateText += QStringLiteral("（会话未就绪）");
            }
            axisTable_->setItem(row, 1, new QTableWidgetItem(stateText));
            axisTable_->setItem(row, 2, new QTableWidgetItem(
                                            QString::number(static_cast<double>(device->positionDeg()),
                                                             'f', 2)));
            axisTable_->setItem(row, 3,
                                new QTableWidgetItem(QString::number(
                                    static_cast<double>(device->velocityDegPerSec()), 'f', 2)));
        }

        const QVector<CommandStatus>& outcomes = group_.status().axisOutcomes;
        const CommandStatus outcome = (row < outcomes.size()) ? outcomes[row]
                                                             : CommandStatus::NotSent;
        auto* outcomeItem = new QTableWidgetItem(commandStatusText(outcome));
        if (outcome == CommandStatus::Nacked || outcome == CommandStatus::TimedOut) {
            outcomeItem->setForeground(QBrush(QColor(0xB0, 0x00, 0x00)));
        }
        axisTable_->setItem(row, 4, outcomeItem);
    }
    updatePlanPreview();
}

}  // namespace mchost
