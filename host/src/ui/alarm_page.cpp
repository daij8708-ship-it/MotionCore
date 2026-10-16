#include "mc_host/ui/alarm_page.hpp"

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/device_state.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

constexpr int kAlarmColumns = 2;

QString formatTime(double tMs) {
    return QStringLiteral("%1 s").arg(tMs / 1000.0, 0, 'f', 2);
}

// 故障码 → 界面中文名。名词解释而不是另起一套术语：括号里保留协议里的英文名，
// 这样截图、日志、文档三处指的都是同一个东西。
QString faultText(mc::FaultCode code) {
    switch (code) {
        case mc::FaultCode::None:
            return QStringLiteral("无");
        case mc::FaultCode::CommunicationLoss:
            return QStringLiteral("设备侧通信丢失（CommunicationLoss：运行中太久没收到合法帧）");
        case mc::FaultCode::OverSpeed:
            return QStringLiteral("超速（OverSpeed）");
        case mc::FaultCode::OverLimit:
            return QStringLiteral("超出行程（OverLimit）");
        case mc::FaultCode::CrcBurst:
            return QStringLiteral("CRC 连续错（CrcBurst）");
        case mc::FaultCode::Forced:
            return QStringLiteral("人为触发（Forced，测试注入）");
    }
    return QStringLiteral("未知故障码");
}

}  // namespace

AlarmPage::AlarmPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* safetyBanner = new QLabel(
        QStringLiteral("⚠ 本页不提供“恢复运行”。故障原因必须在现场查清并排除后，"
                       "这里的“确认”才允许设备重新自检；急停 / 断电是**物理**回路，"
                       "软件的任何操作都不能替代它。"),
        this);
    safetyBanner->setObjectName(QStringLiteral("safetyBanner"));
    safetyBanner->setWordWrap(true);
    safetyBanner->setStyleSheet(
        QStringLiteral("background:#7a1f1f; color:#fff0f0; padding:7px; border-radius:4px;"));
    root->addWidget(safetyBanner);

    auto* currentGroup = new QGroupBox(QStringLiteral("当前故障（设备侧判定）"), this);
    auto* grid = new QGridLayout(currentGroup);
    const auto addRow = [this, grid](int row, const QString& title, const QString& objectName) {
        grid->addWidget(new QLabel(title + QStringLiteral("："), this), row, 0,
                        Qt::AlignRight | Qt::AlignTop);
        auto* value = new QLabel(QStringLiteral("—"), this);
        value->setObjectName(objectName);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        grid->addWidget(value, row, 1);
        grid->setColumnStretch(1, 1);
        return value;
    };
    stateValue_ = addRow(0, QStringLiteral("设备状态"), QStringLiteral("alarmStateValue"));
    faultValue_ = addRow(1, QStringLiteral("故障码"), QStringLiteral("alarmFaultValue"));
    latchValue_ = addRow(2, QStringLiteral("锁存"), QStringLiteral("alarmLatchValue"));

    clearFaultButton_ = new QPushButton(QStringLiteral("确认并请求清除故障"), this);
    clearFaultButton_->setObjectName(QStringLiteral("clearFaultButton"));
    clearFaultButton_->setEnabled(false);
    grid->addWidget(clearFaultButton_, 3, 1);

    clearHint_ = new QLabel(this);
    clearHint_->setObjectName(QStringLiteral("clearFaultHint"));
    clearHint_->setWordWrap(true);
    clearHint_->setStyleSheet(QStringLiteral("color:#444;"));
    grid->addWidget(clearHint_, 4, 0, 1, 2);
    root->addWidget(currentGroup);

    auto* listGroup = new QGroupBox(QStringLiteral("报警记录（本机收到的安全事件）"), this);
    auto* listLayout = new QVBoxLayout(listGroup);

    auto* listToolbar = new QHBoxLayout();
    listCountLabel_ = new QLabel(this);
    listCountLabel_->setObjectName(QStringLiteral("alarmCountLabel"));
    listToolbar->addWidget(listCountLabel_);
    listToolbar->addStretch(1);
    clearListButton_ = new QPushButton(QStringLiteral("清空列表（只擦屏幕，不动设备）"), this);
    clearListButton_->setObjectName(QStringLiteral("clearAlarmListButton"));
    listToolbar->addWidget(clearListButton_);
    listLayout->addLayout(listToolbar);

    alarmTable_ = new QTableWidget(0, kAlarmColumns, this);
    alarmTable_->setObjectName(QStringLiteral("alarmTable"));
    alarmTable_->setHorizontalHeaderLabels({QStringLiteral("时刻"), QStringLiteral("事件")});
    alarmTable_->verticalHeader()->setVisible(false);
    alarmTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    alarmTable_->horizontalHeader()->setStretchLastSection(true);
    listLayout->addWidget(alarmTable_, 1);
    root->addWidget(listGroup, 1);

    connect(clearFaultButton_, &QPushButton::clicked, this, &AlarmPage::requestClearFault);
    connect(clearListButton_, &QPushButton::clicked, &manager_, &DeviceManager::clearAlarms);
    connect(&manager_, &DeviceManager::connectionChanged, this, &AlarmPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &AlarmPage::refresh);
    connect(&manager_, &DeviceManager::alarmRaised, this, [this](const QString&) { refreshAlarms(); });
    connect(&manager_, &DeviceManager::alarmsCleared, this, &AlarmPage::refreshAlarms);
    connect(&manager_, &DeviceManager::commandLogChanged, this, [this]() { refresh(); });

    onConnectionChanged(manager_.isConnected());
    refreshAlarms();
}

void AlarmPage::requestClearFault() {
    const IDevice* device = manager_.device();
    if (device == nullptr) {
        return;
    }

    // 确认的是**屏幕上显示的那个码**，不是"现在挂着的任何东西"。
    // 载荷里带着它，设备会比对自己锁存的故障码，不一致就拒 ——
    // 这正是"操作员确认的对象必须是他看到的那个"这条要求的实现。
    const mc::FaultCode code = device->faultCode();
    const bool sent = manager_.sendClearFault(code);

    clearHint_->setText(
        sent ? QStringLiteral("已发出确认（%1）。等设备自检结果 —— 状态回到 Ready 才算清掉。")
                   .arg(faultText(code))
             : QStringLiteral("没有发出去：会话未就绪（命令行里会留下一条“未发出”）。"));
    refresh();
}

void AlarmPage::onConnectionChanged(bool connected) {
    const IDevice* device = manager_.device();
    const bool ready = connected && device != nullptr && device->sessionReady();
    lastReady_ = ready;

    // 只有"会话就绪 + 确实停在故障态"才允许点。
    // 会话没就绪时发出去的确认到不了设备；不在故障态时设备会以状态非法拒掉 ——
    // 两种情况下把按钮亮着都是骗人。
    const bool canConfirm = ready && device->deviceState() == mc::DeviceState::Fault;
    clearFaultButton_->setEnabled(canConfirm);
    clearListButton_->setEnabled(true);
    refresh();
}

void AlarmPage::refresh() {
    const IDevice* device = manager_.device();

    // 会话就绪与设备状态都是**异步**变化的（握手几拍、故障要等设备报上来），
    // 这里得跟着变；不能只在 connectionChanged 时算一次，那个信号不会再来。
    const bool ready = manager_.isConnected() && device != nullptr && device->sessionReady();
    if (ready != lastReady_) {
        onConnectionChanged(manager_.isConnected());
        return;
    }

    if (device == nullptr) {
        stateValue_->setText(QStringLiteral("没有设备"));
        faultValue_->setText(QStringLiteral("—"));
        latchValue_->setText(QStringLiteral("—"));
        return;
    }

    stateValue_->setText(QStringLiteral("%1 | 会话 %2")
                             .arg(QString::fromUtf8(mc::deviceStateName(device->deviceState())))
                             .arg(ready ? QStringLiteral("就绪") : QStringLiteral("未就绪")));
    faultValue_->setText(faultText(device->faultCode()));

    const bool latched = device->faultLatched();
    latchValue_->setText(
        latched ? QStringLiteral("已锁存 —— 只能由人工确认清除；拔线重连**不会**自动清掉")
                : QStringLiteral("未锁存"));

    const bool canConfirm = ready && device->deviceState() == mc::DeviceState::Fault;
    clearFaultButton_->setEnabled(canConfirm);
    clearFaultButton_->setToolTip(
        canConfirm ? QStringLiteral("确认后会请求设备重新自检；通过后状态才回 Ready")
                   : QStringLiteral("仅当会话就绪且设备停在 Fault 时可用"));
}

void AlarmPage::refreshAlarms() {
    const QVector<AlarmRecord>& alarms = manager_.alarms();
    listCountLabel_->setText(QStringLiteral("共 %1 条（最多保留 %2 条，更早的会被挤掉）")
                                 .arg(alarms.size())
                                 .arg(DeviceManager::kMaxAlarms));

    alarmTable_->setRowCount(alarms.size());
    // 新的在上：排查时最关心的是"刚刚发生了什么"，而不是翻到最底下。
    int row = 0;
    for (int i = alarms.size() - 1; i >= 0; --i) {
        alarmTable_->setItem(row, 0, new QTableWidgetItem(formatTime(alarms.at(i).tMs)));
        alarmTable_->setItem(row, 1, new QTableWidgetItem(alarms.at(i).text));
        ++row;
    }
    alarmTable_->resizeColumnsToContents();
}

}  // namespace mchost
