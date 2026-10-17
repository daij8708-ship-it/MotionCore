#include "mc_host/ui/history_page.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/device_state.hpp"
#include "mc_host/alarm_text.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

// 表格最多显示这么多行。库可以很大（几个小时就是几十万行），
// 一次性全塞进 QTableWidget 会把界面拖死 —— 但**必须把"没显示的那部分"说出来**，
// 否则"表里只有 2000 行"会被读成"总共就这么多"。
constexpr int kMaxTableRows = 2000;

QString wallText(qint64 wallMs) {
    return QDateTime::fromMSecsSinceEpoch(wallMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

}  // namespace

HistoryPage::HistoryPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* storeGroup = new QGroupBox(QStringLiteral("历史库（SQLite）"), this);
    auto* storeGrid = new QGridLayout(storeGroup);
    storeGrid->addWidget(new QLabel(QStringLiteral("位置："), this), 0, 0);
    storeValue_ = new QLabel(this);
    storeValue_->setObjectName(QStringLiteral("historyPathValue"));
    storeValue_->setWordWrap(true);
    storeValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    storeGrid->addWidget(storeValue_, 0, 1);
    storeErrorLabel_ = new QLabel(this);
    storeErrorLabel_->setObjectName(QStringLiteral("historyErrorLabel"));
    storeErrorLabel_->setWordWrap(true);
    storeErrorLabel_->setStyleSheet(QStringLiteral("color:#7a1f1f;"));
    storeGrid->addWidget(storeErrorLabel_, 1, 0, 1, 2);
    storeGrid->setColumnStretch(1, 1);
    root->addWidget(storeGroup);

    auto* queryGroup = new QGroupBox(QStringLiteral("查询"), this);
    auto* queryRow = new QHBoxLayout(queryGroup);
    queryRow->addWidget(new QLabel(QStringLiteral("会话："), this));
    sessionCombo_ = new QComboBox(this);
    sessionCombo_->setObjectName(QStringLiteral("sessionCombo"));
    queryRow->addWidget(sessionCombo_, 1);
    refreshButton_ = new QPushButton(QStringLiteral("刷新会话列表"), this);
    refreshButton_->setObjectName(QStringLiteral("refreshSessionsButton"));
    queryRow->addWidget(refreshButton_);

    queryRow->addWidget(new QLabel(QStringLiteral("时间窗(ms)："), this));
    fromSpin_ = new QSpinBox(this);
    fromSpin_->setObjectName(QStringLiteral("fromSpin"));
    fromSpin_->setRange(0, 24 * 60 * 60 * 1000);
    fromSpin_->setSingleStep(1000);
    fromSpin_->setValue(0);
    queryRow->addWidget(fromSpin_);
    toSpin_ = new QSpinBox(this);
    toSpin_->setObjectName(QStringLiteral("toSpin"));
    toSpin_->setRange(0, 24 * 60 * 60 * 1000);
    toSpin_->setSingleStep(1000);
    toSpin_->setValue(24 * 60 * 60 * 1000);
    queryRow->addWidget(toSpin_);

    queryButton_ = new QPushButton(QStringLiteral("查询"), this);
    queryButton_->setObjectName(QStringLiteral("queryButton"));
    queryRow->addWidget(queryButton_);
    root->addWidget(queryGroup);

    rangeLabel_ = new QLabel(this);
    rangeLabel_->setObjectName(QStringLiteral("historyRangeLabel"));
    rangeLabel_->setWordWrap(true);
    root->addWidget(rangeLabel_);

    auto* tables = new QGridLayout();

    auto* sampleGroup = new QGroupBox(QStringLiteral("遥测样本"), this);
    auto* sampleLayout = new QVBoxLayout(sampleGroup);
    sampleTable_ = new QTableWidget(0, 7, this);
    sampleTable_->setObjectName(QStringLiteral("historySampleTable"));
    sampleTable_->setHorizontalHeaderLabels(
        {QStringLiteral("时刻(ms)"), QStringLiteral("速度(deg/s)"), QStringLiteral("位置(deg)"),
         QStringLiteral("目标"), QStringLiteral("状态"), QStringLiteral("故障码"),
         QStringLiteral("PWM")});
    sampleTable_->verticalHeader()->setVisible(false);
    sampleTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sampleTable_->horizontalHeader()->setStretchLastSection(true);
    sampleLayout->addWidget(sampleTable_);

    auto* exportRow = new QHBoxLayout();
    exportButton_ = new QPushButton(QStringLiteral("导出 CSV（当前会话 + 时间窗）"), this);
    exportButton_->setObjectName(QStringLiteral("exportCsvButton"));
    exportRow->addWidget(exportButton_);
    exportLabel_ = new QLabel(this);
    exportLabel_->setObjectName(QStringLiteral("exportResultLabel"));
    exportLabel_->setWordWrap(true);
    exportRow->addWidget(exportLabel_, 1);
    sampleLayout->addLayout(exportRow);
    tables->addWidget(sampleGroup, 0, 0);

    auto* eventGroup = new QGroupBox(QStringLiteral("事件流水（按时间）"), this);
    auto* eventLayout = new QVBoxLayout(eventGroup);
    eventTable_ = new QTableWidget(0, 3, this);
    eventTable_->setObjectName(QStringLiteral("historyEventTable"));
    eventTable_->setHorizontalHeaderLabels(
        {QStringLiteral("会话内(ms)"), QStringLiteral("墙上时钟"), QStringLiteral("事件")});
    eventTable_->verticalHeader()->setVisible(false);
    eventTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    eventTable_->horizontalHeader()->setStretchLastSection(true);
    eventLayout->addWidget(eventTable_);

    // 分级报警单独一张表。它与上面那张"事件流水"不是一件事：
    // 流水回答"按时间先后发生了什么"（报警、配方下装混在同一条线上），
    // 这张回答"哪些是 Error 以上、确认了没有" —— 后者只有在保存了代码和等级
    // 的前提下才答得出来，而光存一句文本是答不了的。
    auto* alarmGroup = new QGroupBox(QStringLiteral("分级报警（代码 / 等级 / 确认）"), this);
    auto* alarmLayout = new QVBoxLayout(alarmGroup);
    alarmTable_ = new QTableWidget(0, 6, this);
    alarmTable_->setObjectName(QStringLiteral("historyAlarmTable"));
    alarmTable_->setHorizontalHeaderLabels(
        {QStringLiteral("首次(墙上)"), QStringLiteral("等级"), QStringLiteral("代码"),
         QStringLiteral("次数"), QStringLiteral("状态"), QStringLiteral("说明")});
    alarmTable_->verticalHeader()->setVisible(false);
    alarmTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    alarmTable_->horizontalHeader()->setStretchLastSection(true);
    alarmLayout->addWidget(alarmTable_);

    auto* commandGroup = new QGroupBox(QStringLiteral("命令记录"), this);
    auto* commandLayout = new QVBoxLayout(commandGroup);
    commandTable_ = new QTableWidget(0, 5, this);
    commandTable_->setObjectName(QStringLiteral("historyCommandTable"));
    commandTable_->setHorizontalHeaderLabels({QStringLiteral("时刻(ms)"), QStringLiteral("SEQ"),
                                              QStringLiteral("命令"), QStringLiteral("结果"),
                                              QStringLiteral("说明")});
    commandTable_->verticalHeader()->setVisible(false);
    commandTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    commandTable_->horizontalHeader()->setStretchLastSection(true);
    commandLayout->addWidget(commandTable_);

    // 上排看"这一段里数据与报警长什么样"，下排看"逐条发生了什么"。
    // 四张表都摆得下是因为每一张都有自己的滚动条 —— 页面本身不跟着变长。
    tables->addWidget(sampleGroup, 0, 0);
    tables->addWidget(alarmGroup, 0, 1);
    tables->addWidget(eventGroup, 1, 0);
    tables->addWidget(commandGroup, 1, 1);
    tables->setColumnStretch(0, 3);
    tables->setColumnStretch(1, 2);
    tables->setRowStretch(0, 3);
    tables->setRowStretch(1, 2);
    root->addLayout(tables, 1);

    connect(refreshButton_, &QPushButton::clicked, this, &HistoryPage::refreshSessions);
    connect(queryButton_, &QPushButton::clicked, this, &HistoryPage::runQuery);
    connect(sessionCombo_, &QComboBox::currentIndexChanged, this, [this](int) { runQuery(); });
    connect(exportButton_, &QPushButton::clicked, this, &HistoryPage::exportCsv);
    connect(&manager_, &DeviceManager::connectionChanged, this, [this](bool) { refresh(); });

    refreshSessions();
}

qint64 HistoryPage::currentSessionId() const {
    return sessionCombo_->currentData().toLongLong();
}

void HistoryPage::refreshSessions() {
    HistoryStore* store = manager_.history();
    if (store == nullptr) {
        return;
    }

    const qint64 previous = currentSessionId();
    sessionCombo_->clear();

    QString error;
    const std::vector<HistoryStore::SessionRow> rows = store->sessions(error);
    if (!error.isEmpty()) {
        storeErrorLabel_->setText(QStringLiteral("读会话列表失败：%1").arg(error));
        return;
    }

    for (const HistoryStore::SessionRow& row : rows) {
        const QString label =
            QStringLiteral("#%1  %2  %3  %4 条样本%5")
                .arg(row.id)
                .arg(wallText(row.startedWallMs), row.deviceKind)
                .arg(row.sampleCount)
                .arg(row.endedWallMs < 0 ? QStringLiteral("  （未正常结束）") : QString());
        sessionCombo_->addItem(label, row.id);
    }

    const int restored = sessionCombo_->findData(previous);
    if (restored >= 0) {
        sessionCombo_->setCurrentIndex(restored);
    }
    runQuery();
}

void HistoryPage::runQuery() {
    HistoryStore* store = manager_.history();

    if (store == nullptr || !store->isOpen()) {
        storeValue_->setText(QStringLiteral("未启用"));
        storeErrorLabel_->setText(
            store != nullptr ? QStringLiteral("历史库不可用：%1").arg(store->error())
                             : QStringLiteral("没有历史库"));
        rangeLabel_->setText(QStringLiteral("没有可查询的数据。"));
        sampleTable_->setRowCount(0);
        alarmTable_->setRowCount(0);
        eventTable_->setRowCount(0);
        commandTable_->setRowCount(0);
        return;
    }

    QString error;
    const qint64 total = store->totalSamples(error);
    storeValue_->setText(QStringLiteral("%1（共 %2 条样本 / 已落盘 %3 条 / %4 个事务）")
                             .arg(store->path())
                             .arg(total)
                             .arg(store->samplesWritten())
                             .arg(store->transactions()));
    storeErrorLabel_->setText(error.isEmpty() ? QString() : QStringLiteral("查询出错：%1").arg(error));

    const qint64 sessionId = currentSessionId();
    const qint64 from = fromSpin_->value();
    const qint64 to = toSpin_->value();

    const std::vector<HistoryStore::SampleRow> samples =
        store->samples(sessionId, from, to, kMaxTableRows, error);
    const qint64 matched = store->countSamples(sessionId, from, to, error);

    sampleTable_->setRowCount(static_cast<int>(samples.size()));
    int row = 0;
    for (const HistoryStore::SampleRow& sample : samples) {
        const auto set = [this, row](int column, const QString& text) {
            sampleTable_->setItem(row, column, new QTableWidgetItem(text));
        };
        set(0, QString::number(sample.tMs));
        set(1, QString::number(sample.velocityDegPerSec, 'f', 2));
        set(2, QString::number(sample.positionDeg, 'f', 2));
        set(3, QString::number(sample.target, 'f', 2));
        set(4, QString::fromUtf8(
                   mc::deviceStateName(static_cast<mc::DeviceState>(sample.state))));
        set(5, QString::number(sample.faultCode));
        set(6, QString::number(sample.pwmOutput));
        ++row;
    }
    sampleTable_->resizeColumnsToContents();

    const std::vector<HistoryStore::AlarmRow> alarms = store->alarms(sessionId, error);
    alarmTable_->setRowCount(static_cast<int>(alarms.size()));
    row = 0;
    for (const HistoryStore::AlarmRow& alarm : alarms) {
        alarmTable_->setItem(row, 0,
                             new QTableWidgetItem(wallText(alarm.firstWallMs)));
        alarmTable_->setItem(
            row, 1,
            new QTableWidgetItem(alarmLevelText(static_cast<mc::AlarmLevel>(alarm.level))));
        alarmTable_->setItem(row, 2,
                             new QTableWidgetItem(alarmCodeText(
                                 static_cast<std::uint16_t>(alarm.code))));
        alarmTable_->setItem(row, 3, new QTableWidgetItem(QString::number(alarm.occurrences)));
        // 状态必须是"确认了没有"**和**"现在还挂着没有"两件事，缺一件都不够判断：
        // 已恢复但没确认的报警仍然拦着启动，只写"已恢复"会让人以为可以不管了。
        QString state = alarm.active ? QStringLiteral("挂着") : QStringLiteral("已恢复");
        state += alarm.acknowledged ? QStringLiteral("·已确认")
                                    : QStringLiteral("·未确认（当时拦启动）");
        alarmTable_->setItem(row, 4, new QTableWidgetItem(state));
        alarmTable_->setItem(row, 5, new QTableWidgetItem(alarmText(
                                                 static_cast<std::uint16_t>(alarm.code))));
        ++row;
    }
    alarmTable_->resizeColumnsToContents();

    const std::vector<HistoryStore::EventRow> events = store->events(sessionId, error);
    eventTable_->setRowCount(static_cast<int>(events.size()));
    row = 0;
    for (const HistoryStore::EventRow& event : events) {
        eventTable_->setItem(row, 0, new QTableWidgetItem(QString::number(event.tMs)));
        eventTable_->setItem(row, 1, new QTableWidgetItem(wallText(event.wallMs)));
        eventTable_->setItem(row, 2, new QTableWidgetItem(event.text));
        ++row;
    }
    eventTable_->resizeColumnsToContents();

    const std::vector<HistoryStore::CommandRow> commands = store->commands(sessionId, error);
    commandTable_->setRowCount(static_cast<int>(commands.size()));
    row = 0;
    for (const HistoryStore::CommandRow& command : commands) {
        commandTable_->setItem(row, 0, new QTableWidgetItem(QString::number(command.sentMs)));
        commandTable_->setItem(row, 1, new QTableWidgetItem(QString::number(command.seq)));
        commandTable_->setItem(row, 2, new QTableWidgetItem(command.name));
        commandTable_->setItem(
            row, 3,
            new QTableWidgetItem(QString::fromUtf8(
                commandStatusName(static_cast<CommandStatus>(command.status)))));
        commandTable_->setItem(
            row, 4,
            new QTableWidgetItem(command.status == static_cast<int>(CommandStatus::Nacked)
                                     ? QString::fromUtf8(nackReasonName(
                                           static_cast<mc::NackReason>(command.nackReason)))
                                     : QString()));
        ++row;
    }
    commandTable_->resizeColumnsToContents();

    // 行数必须说清是"显示了多少"还是"一共有多少"：这两个数不一样的时候，
    // 把它们混起来正好会让人以为"数据只有这么点"。
    rangeLabel_->setText(
        QStringLiteral("会话 #%1：时间窗 %2–%3 ms 内共 %4 条样本（表里显示前 %5 条）、"
                       "%6 条分级报警、%7 条事件流水、%8 条命令。")
            .arg(sessionId)
            .arg(from)
            .arg(to)
            .arg(matched)
            .arg(samples.size())
            .arg(alarms.size())
            .arg(events.size())
            .arg(commands.size()));
}

void HistoryPage::refresh() {
    // 采集进行中时，历史页上的"当前会话"在长大 —— 但**不按 20 Hz 重查**：
    // 每 50 ms 全表重查一次既浪费又会让正在看的那一页不停跳动。
    // 只在"连接状态变了"时刷新会话列表；要看最新数据，点查询按钮。
    refreshSessions();
}

void HistoryPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 只在"真的切到这一页"时刷新，不在每拍刷新（理由见 refresh()）。
    refresh();
}

void HistoryPage::exportCsv() {
    HistoryStore* store = manager_.history();
    if (store == nullptr || !store->isOpen()) {
        exportLabel_->setText(QStringLiteral("历史库不可用，无法导出。"));
        return;
    }

    const qint64 sessionId = currentSessionId();
    if (sessionId <= 0) {
        exportLabel_->setText(QStringLiteral("没有可导出的会话。"));
        return;
    }

    const QString suggested =
        QStringLiteral("session%1_%2.csv")
            .arg(sessionId)
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 CSV"), suggested, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) {
        exportLabel_->setText(QStringLiteral("已取消。"));
        return;
    }

    QString error;
    const bool ok = store->exportSamplesCsv(path, sessionId, fromSpin_->value(),
                                            toSpin_->value(), error);
    exportLabel_->setText(ok ? QStringLiteral("已导出到 %1").arg(path)
                             : QStringLiteral("导出失败：%1").arg(error));
}

}  // namespace mchost
