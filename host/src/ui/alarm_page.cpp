#include "mc_host/ui/alarm_page.hpp"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vector>

#include "mc/device_state.hpp"
#include "mc_host/alarm_text.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

constexpr int kAlarmColumns = 6;

// 等级对应的颜色。它不只是好看：现场的人先看颜色再读字，
// 而"要不要停下来处理"这件事必须在一眼之内看得出来。
QColor levelColor(mc::AlarmLevel level) {
    switch (level) {
        case mc::AlarmLevel::Info:    return QColor(QStringLiteral("#555555"));
        case mc::AlarmLevel::Warning: return QColor(QStringLiteral("#8a5a00"));
        case mc::AlarmLevel::Error:   return QColor(QStringLiteral("#b00020"));
        case mc::AlarmLevel::Fatal:   return QColor(QStringLiteral("#7a1f1f"));
    }
    return QColor();
}

QString formatTime(std::uint64_t tMs) {
    return QStringLiteral("%1 s").arg(static_cast<double>(tMs) / 1000.0, 0, 'f', 2);
}

QString stateText(const mc::AlarmEntry& entry, bool requiresAck) {
    QString state = entry.active ? QStringLiteral("挂着") : QStringLiteral("已恢复");
    if (requiresAck) {
        state += entry.acknowledged ? QStringLiteral("·已确认") : QStringLiteral("·未确认（拦启动）");
    } else {
        state += QStringLiteral("·不需确认");
    }
    return state;
}

// 筛选档位。存的是**等级下限**加一个"只看未确认"，因为界面要回答的问题
// 恰恰是"哪些我得看一眼"。
enum class Filter : int {
    All = 0,
    Unacknowledged = 1,
    FatalAndAbove = 2,
    ErrorAndAbove = 3,
    WarningAndAbove = 4,
};

bool passesFilter(const mc::AlarmEntry& entry, bool requiresAck, Filter filter) {
    switch (filter) {
        case Filter::All:
            return true;
        case Filter::Unacknowledged:
            return requiresAck && !entry.acknowledged;
        case Filter::FatalAndAbove:
            return entry.level >= mc::AlarmLevel::Fatal;
        case Filter::ErrorAndAbove:
            return entry.level >= mc::AlarmLevel::Error;
        case Filter::WarningAndAbove:
            return entry.level >= mc::AlarmLevel::Warning;
    }
    return true;
}

}  // namespace

AlarmPage::AlarmPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* safetyBanner = new QLabel(
        QStringLiteral("⚠ 本页不提供“恢复运行”。故障原因必须在现场查清并排除后，"
                       "这里的“确认”才允许设备重新自检、并让启动闸门重新打开；"
                       "急停 / 断电是**物理**回路，软件的任何操作都不能替代它。"),
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

    auto* listGroup = new QGroupBox(
        QStringLiteral("报警清单（带代码与等级；未确认的会拦住启动）"), this);
    auto* listLayout = new QVBoxLayout(listGroup);

    auto* listToolbar = new QHBoxLayout();
    listCountLabel_ = new QLabel(this);
    listCountLabel_->setObjectName(QStringLiteral("alarmCountLabel"));
    listToolbar->addWidget(listCountLabel_);

    listToolbar->addWidget(new QLabel(QStringLiteral("　筛选："), this));
    levelFilter_ = new QComboBox(this);
    levelFilter_->setObjectName(QStringLiteral("alarmFilterCombo"));
    levelFilter_->addItem(QStringLiteral("全部"), static_cast<int>(Filter::All));
    levelFilter_->addItem(QStringLiteral("只看未确认"), static_cast<int>(Filter::Unacknowledged));
    levelFilter_->addItem(QStringLiteral("严重及以上"), static_cast<int>(Filter::FatalAndAbove));
    levelFilter_->addItem(QStringLiteral("错误及以上"), static_cast<int>(Filter::ErrorAndAbove));
    levelFilter_->addItem(QStringLiteral("警告及以上"), static_cast<int>(Filter::WarningAndAbove));
    listToolbar->addWidget(levelFilter_);
    listToolbar->addStretch(1);

    acknowledgeButton_ = new QPushButton(QStringLiteral("确认选中"), this);
    acknowledgeButton_->setObjectName(QStringLiteral("acknowledgeAlarmButton"));
    acknowledgeAllButton_ = new QPushButton(QStringLiteral("全部确认"), this);
    acknowledgeAllButton_->setObjectName(QStringLiteral("acknowledgeAllButton"));
    clearListButton_ = new QPushButton(QStringLiteral("清空列表（只擦屏幕，不动设备）"), this);
    clearListButton_->setObjectName(QStringLiteral("clearAlarmListButton"));
    listToolbar->addWidget(acknowledgeButton_);
    listToolbar->addWidget(acknowledgeAllButton_);
    listToolbar->addWidget(clearListButton_);
    listLayout->addLayout(listToolbar);

    alarmTable_ = new QTableWidget(0, kAlarmColumns, this);
    alarmTable_->setObjectName(QStringLiteral("alarmTable"));
    alarmTable_->setHorizontalHeaderLabels({QStringLiteral("首次时刻"), QStringLiteral("等级"),
                                            QStringLiteral("代码"), QStringLiteral("事件"),
                                            QStringLiteral("次数"), QStringLiteral("状态")});
    alarmTable_->verticalHeader()->setVisible(false);
    alarmTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    alarmTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    alarmTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    alarmTable_->horizontalHeader()->setStretchLastSection(true);
    listLayout->addWidget(alarmTable_, 1);
    root->addWidget(listGroup, 1);

    connect(clearFaultButton_, &QPushButton::clicked, this, &AlarmPage::requestClearFault);
    connect(acknowledgeButton_, &QPushButton::clicked, this, &AlarmPage::acknowledgeSelected);
    connect(acknowledgeAllButton_, &QPushButton::clicked, this, &AlarmPage::acknowledgeAll);
    connect(clearListButton_, &QPushButton::clicked, this, [this]() {
        // 清清单与确认报警是两件事（一个擦屏幕、一个留凭据），所以它的结果
        // 也走同一条出口：**没清掉就说没清掉**。
        if (!manager_.clearAlarms()) {
            clearHint_->setText(manager_.lastRefusalText());
        }
    });
    connect(levelFilter_, &QComboBox::currentIndexChanged, this,
            [this]() { refreshAlarms(); });
    connect(&manager_, &DeviceManager::connectionChanged, this, &AlarmPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &AlarmPage::refresh);
    connect(&manager_, &DeviceManager::alarmRaised, this,
            [this](std::uint16_t, const QString&) { refreshAlarms(); });
    connect(&manager_, &DeviceManager::alarmsChanged, this, &AlarmPage::refreshAlarms);
    connect(&manager_, &DeviceManager::commandLogChanged, this, [this]() { refresh(); });

    onConnectionChanged(manager_.isConnected());
    refreshAlarms();
}

int AlarmPage::selectedCode() const {
    const int row = alarmTable_->currentRow();
    if (row < 0) {
        return -1;
    }
    const QTableWidgetItem* item = alarmTable_->item(row, 0);
    return (item != nullptr) ? item->data(Qt::UserRole).toInt() : -1;
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

    clearHint_->setText(sent ? QStringLiteral("已发出确认（%1）。等设备自检结果 —— 状态回到 Ready 才算清掉。")
                                   .arg(alarmText(mc::alarmCodeForFault(code)))
                             : manager_.lastRefusalText());
    refresh();
}

void AlarmPage::acknowledgeSelected() {
    const int code = selectedCode();
    if (code < 0) {
        return;
    }
    if (!manager_.acknowledgeAlarm(static_cast<std::uint16_t>(code))) {
        // 被拒（没权限 / 这个码不在清单里）要说出来：确认是"我看见了"的凭据，
        // 没确认成功却让人以为确认了，是最危险的一种沉默。
        clearHint_->setText(manager_.lastRefusalText());
    }
    refreshAlarms();
}

void AlarmPage::acknowledgeAll() {
    if (!manager_.acknowledgeAllAlarms()) {
        clearHint_->setText(manager_.lastRefusalText());
    }
    refreshAlarms();
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
    faultValue_->setText(device->faultCode() == mc::FaultCode::None
                             ? QStringLiteral("无")
                             : alarmText(mc::alarmCodeForFault(device->faultCode())));

    const bool latched = device->faultLatched();
    latchValue_->setText(
        latched ? QStringLiteral("已锁存 —— 只能由人工确认清除；拔线重连**不会**自动清掉")
                : QStringLiteral("未锁存"));

    const bool canConfirm = ready && device->deviceState() == mc::DeviceState::Fault;
    clearFaultButton_->setEnabled(canConfirm);
    clearFaultButton_->setToolTip(
        canConfirm ? QStringLiteral("确认后会请求设备重新自检；通过后状态才回 Ready")
                   : QStringLiteral("仅当会话就绪且设备停在 Fault 时可用"));

    refreshAlarms();
}

void AlarmPage::refreshAlarms() {
    const mc::AlarmLog& log = manager_.alarms();
    const Filter filter = static_cast<Filter>(levelFilter_->currentData().toInt());

    QString countText = QStringLiteral("共 %1 条（上限 %2 条；挤掉过 %3 条）｜未确认 %4 条")
                            .arg(static_cast<int>(log.entries().size()))
                            .arg(static_cast<int>(log.capacity()))
                            .arg(static_cast<int>(log.discarded()))
                            .arg(static_cast<int>(log.unacknowledgedRequiringAck()));
    if (log.blocksStart()) {
        countText += QStringLiteral("（**拦住启动**）");
    }
    if (log.raisedTotal() > static_cast<std::uint64_t>(log.entries().size())) {
        countText += QStringLiteral("｜累计发生 %1 次").arg(static_cast<qulonglong>(log.raisedTotal()));
    }
    listCountLabel_->setText(countText);

    // 新的在上：排查时最关心"刚刚发生了什么"，而不是翻到最底下。
    const std::vector<mc::AlarmEntry>& entries = log.entries();
    std::vector<int> visible;
    for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
        const mc::AlarmEntry& entry = entries[static_cast<std::size_t>(i)];
        const mc::AlarmSpec* spec = mc::alarmSpec(entry.code);
        // 不认识的代码按"需要确认"处理（与报警簿同一套判据，不另立一份）。
        const bool requiresAck = (spec == nullptr) ? true : spec->requiresAck;
        if (passesFilter(entry, requiresAck, filter)) {
            visible.push_back(i);
        }
    }

    alarmTable_->setRowCount(visible.size());
    int row = 0;
    for (const int index : visible) {
        const mc::AlarmEntry& entry = entries[static_cast<std::size_t>(index)];
        const mc::AlarmSpec* spec = mc::alarmSpec(entry.code);
        const bool requiresAck = (spec == nullptr) ? true : spec->requiresAck;

        auto* timeItem = new QTableWidgetItem(formatTime(entry.firstTMs));
        // 代码存在这一列里：选中哪一行要能直接读出它，而不是从文本反解。
        timeItem->setData(Qt::UserRole, static_cast<int>(entry.code));
        alarmTable_->setItem(row, 0, timeItem);

        auto* levelItem = new QTableWidgetItem(alarmLevelText(entry.level));
        levelItem->setForeground(QBrush(levelColor(entry.level)));
        alarmTable_->setItem(row, 1, levelItem);

        auto* codeItem = new QTableWidgetItem(alarmCodeText(entry.code));
        codeItem->setForeground(QBrush(levelColor(entry.level)));
        alarmTable_->setItem(row, 2, codeItem);

        QString text = alarmText(entry.code);
        if (!entry.detail.empty()) {
            text += QStringLiteral("（%1）").arg(QString::fromStdString(entry.detail));
        }
        alarmTable_->setItem(row, 3, new QTableWidgetItem(text));
        alarmTable_->setItem(row, 4,
                             new QTableWidgetItem(QString::number(entry.occurrences)));

        auto* stateItem = new QTableWidgetItem(stateText(entry, requiresAck));
        if (requiresAck && !entry.acknowledged) {
            // 未确认的条目加粗：这是"现在按不动启动"的直接原因。
            QFont font = stateItem->font();
            font.setBold(true);
            stateItem->setFont(font);
        }
        alarmTable_->setItem(row, 5, stateItem);
        ++row;
    }
    alarmTable_->resizeColumnsToContents();

    const std::size_t pending = log.unacknowledgedRequiringAck();
    acknowledgeButton_->setEnabled(pending > 0U && selectedCode() >= 0);
    acknowledgeAllButton_->setEnabled(pending > 0U);
    acknowledgeAllButton_->setToolTip(
        pending > 0U ? QStringLiteral("把 %1 条未确认的一次性确认掉").arg(static_cast<int>(pending))
                     : QStringLiteral("没有需要确认的报警"));
}

}  // namespace mchost
