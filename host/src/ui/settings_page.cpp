#include "mc_host/ui/settings_page.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc_host/audit_text.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

// 只列常用的几档。要更多可以自己加 —— 但**不**做成自由文本输入：
// 打错的波特率会静默地什么都收不到，是排查成本最高的一类错误。
constexpr int kBaudRates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

// 审计表最多显示多少行。表能滚，但没人会翻到一万行之前；而"库里一共多少条"
// 另有标签写着，所以这里截断**不会**让总量变成看不见的数。
constexpr int kAuditRowsShown = 200;
constexpr int kAuditColumns = 5;

QString formatAuditTime(qint64 wallMs) {
    if (wallMs <= 0) {
        return QStringLiteral("—");
    }
    return QDateTime::fromMSecsSinceEpoch(wallMs).toString(QStringLiteral("MM-dd HH:mm:ss"));
}

}  // namespace

SettingsPage::SettingsPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    simBanner_ = new QLabel(this);
    simBanner_->setObjectName(QStringLiteral("settingsBanner"));
    simBanner_->setWordWrap(true);
    simBanner_->setStyleSheet(
        QStringLiteral("background:#8a5a00; color:#fff6e0; padding:7px; border-radius:4px;"));
    root->addWidget(simBanner_);

    auto* sourceGroup = new QGroupBox(QStringLiteral("数据源"), this);
    auto* sourceGrid = new QGridLayout(sourceGroup);
    sourceGrid->addWidget(new QLabel(QStringLiteral("数据源："), this), 0, 0,
                          Qt::AlignRight | Qt::AlignTop);
    sourceCombo_ = new QComboBox(this);
    sourceCombo_->setObjectName(QStringLiteral("sourceCombo"));
    // 顺序 = 枚举顺序，代码里按下标取；这一行注释是为了让改顺序的人知道要一起改。
    sourceCombo_->insertItem(0, QStringLiteral("进程内模拟器（没有硬件时用这个）"));
    sourceCombo_->insertItem(1, QStringLiteral("串口（真实设备）"));
    sourceGrid->addWidget(sourceCombo_, 0, 1);

    sourceGrid->addWidget(new QLabel(QStringLiteral("当前："), this), 1, 0,
                          Qt::AlignRight | Qt::AlignTop);
    currentSourceValue_ = new QLabel(this);
    currentSourceValue_->setObjectName(QStringLiteral("currentSourceValue"));
    currentSourceValue_->setWordWrap(true);
    currentSourceValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceGrid->addWidget(currentSourceValue_, 1, 1);
    sourceGrid->setColumnStretch(1, 1);
    root->addWidget(sourceGroup);

    auto* serialGroup = new QGroupBox(QStringLiteral("串口参数（只在上面选“串口”时可改）"), this);
    auto* serialGrid = new QGridLayout(serialGroup);

    serialGrid->addWidget(new QLabel(QStringLiteral("端口："), this), 0, 0);
    portCombo_ = new QComboBox(this);
    portCombo_->setObjectName(QStringLiteral("portCombo"));
    portCombo_->setEditable(false);
    serialGrid->addWidget(portCombo_, 0, 1);
    refreshPortsButton_ = new QPushButton(QStringLiteral("重新扫描"), this);
    refreshPortsButton_->setObjectName(QStringLiteral("refreshPortsButton"));
    serialGrid->addWidget(refreshPortsButton_, 0, 2);

    serialGrid->addWidget(new QLabel(QStringLiteral("波特率："), this), 1, 0);
    baudCombo_ = new QComboBox(this);
    baudCombo_->setObjectName(QStringLiteral("baudCombo"));
    for (const int baud : kBaudRates) {
        baudCombo_->addItem(QString::number(baud), baud);
    }
    baudCombo_->setCurrentText(QStringLiteral("115200"));
    serialGrid->addWidget(baudCombo_, 1, 1);

    serialGrid->addWidget(new QLabel(QStringLiteral("数据位："), this), 2, 0);
    dataBitsCombo_ = new QComboBox(this);
    dataBitsCombo_->setObjectName(QStringLiteral("dataBitsCombo"));
    dataBitsCombo_->addItem(QStringLiteral("8"), 8);
    dataBitsCombo_->addItem(QStringLiteral("7"), 7);
    serialGrid->addWidget(dataBitsCombo_, 2, 1);

    serialGrid->addWidget(new QLabel(QStringLiteral("校验："), this), 3, 0);
    parityCombo_ = new QComboBox(this);
    parityCombo_->setObjectName(QStringLiteral("parityCombo"));
    parityCombo_->addItem(QStringLiteral("无 (N)"), 0);
    parityCombo_->addItem(QStringLiteral("偶 (E)"), 1);
    parityCombo_->addItem(QStringLiteral("奇 (O)"), 2);
    serialGrid->addWidget(parityCombo_, 3, 1);

    serialGrid->addWidget(new QLabel(QStringLiteral("停止位："), this), 4, 0);
    stopBitsCombo_ = new QComboBox(this);
    stopBitsCombo_->setObjectName(QStringLiteral("stopBitsCombo"));
    stopBitsCombo_->addItem(QStringLiteral("1"), 1);
    stopBitsCombo_->addItem(QStringLiteral("2"), 2);
    serialGrid->addWidget(stopBitsCombo_, 4, 1);

    serialGrid->addWidget(new QLabel(QStringLiteral("流控："), this), 5, 0);
    flowCombo_ = new QComboBox(this);
    flowCombo_->setObjectName(QStringLiteral("flowCombo"));
    flowCombo_->addItem(QStringLiteral("无"), 0);
    flowCombo_->addItem(QStringLiteral("硬件 RTS/CTS"), 1);
    serialGrid->addWidget(flowCombo_, 5, 1);

    portCountLabel_ = new QLabel(this);
    portCountLabel_->setObjectName(QStringLiteral("portCountLabel"));
    portCountLabel_->setWordWrap(true);
    portCountLabel_->setStyleSheet(QStringLiteral("color:#666;"));
    serialGrid->addWidget(portCountLabel_, 6, 0, 1, 3);
    serialGrid->setColumnStretch(1, 1);
    root->addWidget(serialGroup);

    auto* actions = new QGridLayout();
    applyButton_ = new QPushButton(QStringLiteral("应用数据源并连接"), this);
    applyButton_->setObjectName(QStringLiteral("applySourceButton"));
    disconnectButton_ = new QPushButton(QStringLiteral("断开"), this);
    disconnectButton_->setObjectName(QStringLiteral("settingsDisconnectButton"));
    actions->addWidget(applyButton_, 0, 0);
    actions->addWidget(disconnectButton_, 0, 1);
    root->addLayout(actions);

    // --- 操作员角色 -----------------------------------------------------------
    //
    // 它守住的是**误操作**，不是人：本软件不做账户认证，能改数据库文件的人也能改流水。
    // 所以这一页必须把这句话写在脸上，而不是做一个像保险柜的按钮。
    auto* roleGroup = new QGroupBox(QStringLiteral("操作员（软件权限，不是门禁）"), this);
    auto* roleGrid = new QGridLayout(roleGroup);
    roleGrid->addWidget(new QLabel(QStringLiteral("当前角色："), this), 0, 0,
                        Qt::AlignRight | Qt::AlignTop);
    roleValue_ = new QLabel(this);
    roleValue_->setObjectName(QStringLiteral("operatorRoleValue"));
    roleValue_->setWordWrap(true);
    roleGrid->addWidget(roleValue_, 0, 1, 1, 2);

    roleGrid->addWidget(new QLabel(QStringLiteral("切换为："), this), 1, 0,
                        Qt::AlignRight | Qt::AlignTop);
    roleCombo_ = new QComboBox(this);
    roleCombo_->setObjectName(QStringLiteral("roleCombo"));
    // 顺序 = 枚举顺序（Viewer/Operator/Engineer），下标即角色值。
    roleCombo_->addItem(operatorRoleText(mc::OperatorRole::Viewer),
                        static_cast<int>(mc::OperatorRole::Viewer));
    roleCombo_->addItem(operatorRoleText(mc::OperatorRole::Operator),
                        static_cast<int>(mc::OperatorRole::Operator));
    roleCombo_->addItem(operatorRoleText(mc::OperatorRole::Engineer),
                        static_cast<int>(mc::OperatorRole::Engineer));
    roleGrid->addWidget(roleCombo_, 1, 1);
    // 刻意做成"选完再点应用"，而不是选中就生效：下拉框是**一滑就变**的控件，
    // 而角色一改，紧接着的一串按钮就不一样了。多一次点击换"不会滑错"。
    applyRoleButton_ = new QPushButton(QStringLiteral("应用角色"), this);
    applyRoleButton_->setObjectName(QStringLiteral("applyRoleButton"));
    roleGrid->addWidget(applyRoleButton_, 1, 2);

    roleDutyLabel_ = new QLabel(this);
    roleDutyLabel_->setObjectName(QStringLiteral("roleDutyLabel"));
    roleDutyLabel_->setWordWrap(true);
    roleDutyLabel_->setStyleSheet(QStringLiteral("color:#444;"));
    roleGrid->addWidget(roleDutyLabel_, 2, 0, 1, 3);

    roleGrid->addWidget(
        new QLabel(QStringLiteral("说明：角色**只在本次运行内有效**，重启回到出厂角色（%1）——"
                                  "本软件不做账户认证，别把它当门禁用。每一次操作（包括被拒的）"
                                  "都会记进下面的审计流水，那才是它真正的用处：事后查得出"
                                  "「谁在什么时候想做什么、成没成、为什么没成」。")
                       .arg(operatorRoleText(DeviceManager::kDefaultRole)),
                   this),
        3, 0, 1, 3);
    roleGrid->setColumnStretch(1, 1);
    root->addWidget(roleGroup);

    roleHintLabel_ = new QLabel(this);
    roleHintLabel_->setObjectName(QStringLiteral("roleHintLabel"));
    roleHintLabel_->setWordWrap(true);
    root->addWidget(roleHintLabel_);

    // --- 审计流水 -------------------------------------------------------------
    auto* auditGroup = new QGroupBox(QStringLiteral("操作审计（谁、什么时候、做了什么、成没成）"), this);
    auto* auditLayout = new QVBoxLayout(auditGroup);
    auditCountLabel_ = new QLabel(this);
    auditCountLabel_->setObjectName(QStringLiteral("auditCountLabel"));
    auditCountLabel_->setWordWrap(true);
    auditLayout->addWidget(auditCountLabel_);

    auditTable_ = new QTableWidget(0, kAuditColumns, this);
    auditTable_->setObjectName(QStringLiteral("auditTable"));
    auditTable_->setHorizontalHeaderLabels({QStringLiteral("时刻"), QStringLiteral("角色"),
                                            QStringLiteral("动作"), QStringLiteral("结果"),
                                            QStringLiteral("对象／原因")});
    auditTable_->verticalHeader()->setVisible(false);
    auditTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auditTable_->horizontalHeader()->setStretchLastSection(true);
    auditTable_->setMaximumHeight(220);
    auditLayout->addWidget(auditTable_);

    refreshAuditButton_ = new QPushButton(QStringLiteral("重新读库"), this);
    refreshAuditButton_->setObjectName(QStringLiteral("refreshAuditButton"));
    auto* auditRow = new QHBoxLayout();
    auditRow->addWidget(refreshAuditButton_);
    auditRow->addStretch(1);
    auditLayout->addLayout(auditRow);
    root->addWidget(auditGroup);

    hintLabel_ = new QLabel(this);
    hintLabel_->setObjectName(QStringLiteral("settingsHint"));
    hintLabel_->setWordWrap(true);
    hintLabel_->setStyleSheet(QStringLiteral("color:#444;"));
    root->addWidget(hintLabel_);

    // 固定说明与"上一次动作的结果"分开写：合成一个标签的话，任何一次刷新
    // 都会把刚才那句结果盖掉 —— 而"打开失败的原因"恰恰是最需要留在屏幕上的。
    noteLabel_ = new QLabel(
        QStringLiteral("说明：换数据源会**重建**一条链路（新会话、重新握手）；正在跑的运动会"
                       "因此中断，这是刻意的 —— 同一时刻只能有一个数据源。进程内模拟器不需要"
                       "任何硬件，协议、状态机、界面与串口路径完全共用。本页不提供物理急停 / "
                       "断电的软件替代：安全回路始终是物理的。"),
        this);
    noteLabel_->setObjectName(QStringLiteral("settingsNote"));
    noteLabel_->setWordWrap(true);
    noteLabel_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(noteLabel_);
    root->addStretch(1);

    connect(sourceCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateSourceUi(); });
    connect(refreshPortsButton_, &QPushButton::clicked, this, &SettingsPage::refreshPorts);
    connect(applyButton_, &QPushButton::clicked, this, &SettingsPage::applySource);
    connect(disconnectButton_, &QPushButton::clicked, this, [this]() {
        if (!manager_.disconnectDevice()) {
            hintLabel_->setText(QStringLiteral("没有断开：%1").arg(manager_.lastRefusalText()));
        }
    });
    connect(applyRoleButton_, &QPushButton::clicked, this, &SettingsPage::applyRole);
    connect(roleCombo_, &QComboBox::currentIndexChanged, this, &SettingsPage::refreshRole);
    connect(refreshAuditButton_, &QPushButton::clicked, this, &SettingsPage::refreshAudit);
    // 审计是**人的动作频率**（不是 100 Hz 的遥测），所以来一条就重画一次：
    // 频率低到不值得做节流，而"点完立刻看见它出现在流水里"本身就是这一页的判据。
    connect(&manager_, &DeviceManager::auditChanged, this, &SettingsPage::refreshAudit);
    connect(&manager_, &DeviceManager::connectionChanged, this,
            &SettingsPage::onConnectionChanged);

    // 刻意**不**接 statsRefreshed：这一页显示的东西（数据源、端口列表、参数）
    // 只在换源时变，按 20 Hz 重建这些字符串是把 CPU 花在看不见的差别上。
    refreshPorts();
    updateSourceUi();
    onConnectionChanged(manager_.isConnected());
    refresh();
    refreshRole();
    refreshAudit();
}

void SettingsPage::refreshRole() {
    const mc::OperatorRole role = manager_.operatorRole();
    roleValue_->setText(QStringLiteral("%1（%2）")
                            .arg(operatorRoleText(role))
                            .arg(QString::fromLatin1(mc::operatorRoleToken(role))));

    // 职责说明跟着**下拉框里选中的那个**走，而不是跟着当前角色走：
    // 人在按下"应用角色"之前想知道的恰恰是"换成它会怎样"。
    const auto selected = static_cast<mc::OperatorRole>(roleCombo_->currentData().toInt());
    roleDutyLabel_->setText(QStringLiteral("【%1】%2")
                                .arg(operatorRoleText(selected), operatorRoleDutyText(selected)));

    // 换角色本身也要"管理角色"权限。一个观察者不该能把自己升上去 ——
    // 所以这里把按钮关掉**并说明为什么**，而不是让它点了没反应。
    const bool maySwitch = mc::roleHasPermission(role, mc::kPermissionManageRole);
    applyRoleButton_->setEnabled(maySwitch);
    applyRoleButton_->setToolTip(
        maySwitch ? QString()
                  : QStringLiteral("当前角色是「%1」，没有「管理角色」权限："
                                   "换角色这件事本身也要权限（重启可回到出厂角色 %2）")
                        .arg(operatorRoleText(role),
                             operatorRoleText(DeviceManager::kDefaultRole)));
}

void SettingsPage::applyRole() {
    const auto selected = static_cast<mc::OperatorRole>(roleCombo_->currentData().toInt());
    if (selected == manager_.operatorRole()) {
        roleHintLabel_->setText(QStringLiteral("角色没变：还是「%1」。")
                                    .arg(operatorRoleText(selected)));
        return;
    }
    if (!manager_.setOperatorRole(selected)) {
        roleHintLabel_->setText(QStringLiteral("没有换：%1").arg(manager_.lastRefusalText()));
        return;
    }
    roleHintLabel_->setText(QStringLiteral("已是「%1」。%2")
                                .arg(operatorRoleText(selected), operatorRoleDutyText(selected)));
    refreshRole();
}

void SettingsPage::refreshAudit() {
    const auto put = [this](int row, qint64 wallMs, mc::OperatorRole role,
                            mc::OperatorAction action, bool dispatched, const QString& subject,
                            const QString& reason) {
        auditTable_->setItem(row, 0, new QTableWidgetItem(formatAuditTime(wallMs)));
        auditTable_->setItem(row, 1, new QTableWidgetItem(operatorRoleText(role)));
        auditTable_->setItem(row, 2, new QTableWidgetItem(operatorActionText(action)));
        // 「结果」列写的是**发生没发生**，不是"允许不允许"：
        // 因此它有三种话要说 —— 执行了、被拦下了（理由在右列）、以及本来就是空的。
        auto* resultItem = new QTableWidgetItem(
            dispatched ? QStringLiteral("已执行") : QStringLiteral("未执行"));
        if (!dispatched) {
            resultItem->setForeground(QBrush(QColor(QStringLiteral("#b00020"))));
        }
        auditTable_->setItem(row, 3, resultItem);
        const QString detail = dispatched ? subject
                                          : QStringLiteral("%1｜%2").arg(subject, reason);
        auditTable_->setItem(row, 4, new QTableWidgetItem(detail));
    };

    const mc::AuditLog& log = manager_.audit();
    QString error;
    const bool fromDb = manager_.historyOpen();
    std::vector<HistoryStore::AuditRow> rows;
    if (fromDb) {
        rows = manager_.history()->audits(kAuditRowsShown, error);
    }

    if (!fromDb || !error.isEmpty()) {
        // 库读不到就退回内存里那一份，并且**说清楚现在看的是哪一份** ——
        // 不说明的话，"库里应该有、这里却没有"会被当成"那件事没发生过"。
        const std::deque<mc::AuditEntry>& entries = log.entries();
        auditTable_->setRowCount(static_cast<int>(entries.size()));
        int row = static_cast<int>(entries.size()) - 1;   // 最近的在上
        for (const mc::AuditEntry& entry : entries) {
            put(row, static_cast<qint64>(entry.wallMs), entry.role, entry.action, entry.dispatched,
                QString::fromStdString(entry.subject), QString::fromStdString(entry.reason));
            --row;
        }
        auditCountLabel_->setText(
            fromDb ? QStringLiteral("读库失败（%1），下面是本次运行的内存流水：共 %2 条"
                                    "（已执行 %3 / 未执行 %4）。")
                         .arg(error)
                         .arg(log.recordedTotal())
                         .arg(log.dispatchedTotal())
                         .arg(log.notDispatchedTotal())
                   : QStringLiteral("历史库没打开，下面是本次运行的内存流水：共 %1 条"
                                    "（已执行 %2 / 未执行 %3），窗口上限 %4 条、已挤掉 %5 条。")
                         .arg(log.recordedTotal())
                         .arg(log.dispatchedTotal())
                         .arg(log.notDispatchedTotal())
                         .arg(static_cast<int>(log.capacity()))
                         .arg(static_cast<int>(log.discarded())));
        auditTable_->resizeColumnsToContents();
        return;
    }

    auditTable_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const HistoryStore::AuditRow& entry = rows[static_cast<std::size_t>(row)];
        put(row, entry.wallMs, static_cast<mc::OperatorRole>(entry.role),
            static_cast<mc::OperatorAction>(entry.action), entry.dispatched, entry.subject,
            entry.reason);
    }
    auditTable_->resizeColumnsToContents();
    auditCountLabel_->setText(
        QStringLiteral("库里共 %1 条（本页显示最近 %2 条）；本次运行内存窗口 %3 条、已挤掉 %4 条。"
                       "流水是**追加**的：同一条操作做两次就是两条记录。")
            .arg(static_cast<int>(rows.size()))
            .arg(kAuditRowsShown)
            .arg(log.recordedTotal())
            .arg(static_cast<int>(log.discarded())));
}

void SettingsPage::refreshPorts() {
    const QString previous = portCombo_->currentText();
    portCombo_->clear();

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        // 描述与厂商一起放进显示文本：同一个 /dev/ttyUSB0 上换一块板子是常事，
        // 光看设备名分辨不出来。实际值仍是端口名（userData）。
        QString label = info.portName();
        const QString description = info.description().trimmed();
        if (!description.isEmpty()) {
            label += QStringLiteral(" — %1").arg(description);
        }
        portCombo_->addItem(label, info.portName());
    }

    // 扫描会清空列表，尽量把刚才选中的那个还选回来（否则每次点"重新扫描"
    // 都要重新找一遍端口，而人往往是"先选好端口再换线"）。
    const int restored = portCombo_->findText(previous, Qt::MatchStartsWith);
    if (restored >= 0) {
        portCombo_->setCurrentIndex(restored);
    }

    portCountLabel_->setText(
        ports.isEmpty()
            ? QStringLiteral("本机没有扫描到任何串口。没有硬件时可以先用进程内模拟器 —— "
                             "协议、状态机、界面完全是同一套。")
            : QStringLiteral("扫描到 %1 个串口。").arg(ports.size()));
    updateSourceUi();
}

void SettingsPage::updateSourceUi() {
    const bool serial = sourceCombo_->currentIndex() == 1;
    for (QComboBox* combo : {portCombo_, baudCombo_, dataBitsCombo_, parityCombo_, stopBitsCombo_,
                             flowCombo_}) {
        combo->setEnabled(serial);
    }
    refreshPortsButton_->setEnabled(serial);
}

void SettingsPage::applySource() {
    QString error;
    bool ok = false;

    if (sourceCombo_->currentIndex() == 0) {
        ok = manager_.useSimulatedDevice();
        if (!ok) {
            // 失败原因不止一种（没权限 / 已连接中），所以**问管理层的原话**，
            // 不在这里硬写一句"请先断开"—— 那会把权限问题说成链路问题。
            error = manager_.lastRefusalText();
        }
    } else {
        SerialTransport::Config config;
        config.portName = portCombo_->currentData().toString();
        config.baudRate = baudCombo_->currentData().toInt();
        config.dataBits = dataBitsCombo_->currentData().toInt();
        config.parity = static_cast<SerialTransport::Config::Parity>(
            parityCombo_->currentData().toInt());
        config.stopBits = stopBitsCombo_->currentData().toInt();
        config.hardwareFlowControl = flowCombo_->currentData().toInt() != 0;
        ok = manager_.useSerialDevice(config, error);
    }

    if (!ok) {
        hintLabel_->setText(QStringLiteral("没有换：%1").arg(error));
        return;
    }

    // 换好之后**立刻尝试连接**：这一页的动作就是"把数据源接上"，
    // 分两步（换源、再点连接）只会让人忘了第二步，然后对着一个空界面找原因。
    manager_.connectDevice();

    const IDevice* device = manager_.device();
    if (manager_.isConnected()) {
        hintLabel_->setText(QStringLiteral("已连接：%1（%2）")
                                .arg(QString::fromUtf8(device->kindName()),
                                     QString::fromUtf8(device->isSimulated()
                                                           ? "模拟数据"
                                                           : "真实串口")));
    } else if (device != nullptr && device->openError() != OpenError::None) {
        // 打开失败的原文在 openErrorName() 里，报警流水里也有一条。
        hintLabel_->setText(QStringLiteral("打开失败：%1")
                                .arg(QString::fromUtf8(openErrorName(device->openError()))));
    } else {
        hintLabel_->setText(QStringLiteral("已切换数据源，但还没连上。"));
    }
    refresh();
}

void SettingsPage::onConnectionChanged(bool connected) {
    // 已连接时不许换源：换设备会把一条正在跑的链路的关闭责任丢掉。
    // 按钮**禁用并且给出理由**，而不是点下去什么都不发生。
    applyButton_->setEnabled(!connected);
    disconnectButton_->setEnabled(connected);
    sourceCombo_->setEnabled(!connected);
    portCombo_->setEnabled(!connected && sourceCombo_->currentIndex() == 1);
    applyButton_->setToolTip(connected ? QStringLiteral("已连接：请先断开再换数据源") : QString());
    refresh();
}

void SettingsPage::refresh() {
    const IDevice* device = manager_.device();
    if (device == nullptr) {
        currentSourceValue_->setText(QStringLiteral("没有设备"));
        simBanner_->setText(QStringLiteral("未接入任何设备"));
        return;
    }

    const bool simulated = device->isSimulated();
    currentSourceValue_->setText(
        QStringLiteral("%1（%2）")
            .arg(QString::fromUtf8(device->kindName()),
                 simulated ? QStringLiteral("模拟数据，不代表任何硬件指标")
                           : QString::fromUtf8(device->configSummary())));
    simBanner_->setText(
        simulated
            ? QStringLiteral("★ 当前数据源是进程内模拟器（模拟数据）：界面上的曲线与数字是**仿真"
                             "模型**的输出，不是任何真实设备或硬件的测量结果。")
            : QStringLiteral("■ 当前数据源是串口设备。速度/位置/目标来自遥测帧（回读）；"
                             "运行模式协议里没有回读字段，显示的是上位机最后一次被确认的"
                             "设定值 —— 总览页上会标出来。"));
}

}  // namespace mchost
