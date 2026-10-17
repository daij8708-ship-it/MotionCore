#include "mc_host/ui/log_page.hpp"

#include <deque>

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "mc/log.hpp"

namespace mchost {
namespace {

constexpr int kColumns = 4;

QColor levelColor(mc::LogLevel level) {
    switch (level) {
        case mc::LogLevel::Trace:
        case mc::LogLevel::Debug:
            return QColor(0x66, 0x66, 0x66);
        case mc::LogLevel::Info:
            return QColor(0x22, 0x22, 0x22);
        case mc::LogLevel::Warn:
            return QColor(0xB0, 0x6A, 0x00);
        case mc::LogLevel::Error:
            return QColor(0xB0, 0x00, 0x00);
    }
    return QColor(0x22, 0x22, 0x22);
}

}  // namespace

LogPage::LogPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("记录级别（低于它的不记）："), this));
    levelCombo_ = new QComboBox(this);
    levelCombo_->setObjectName(QStringLiteral("logLevelCombo"));
    for (std::size_t i = 0U; i < mc::kLogLevelCount; ++i) {
        const auto level = static_cast<mc::LogLevel>(i);
        levelCombo_->addItem(QString::fromUtf8(mc::logLevelText(level)),
                             static_cast<int>(level));
    }
    levelCombo_->setCurrentIndex(static_cast<int>(mc::LogLevel::Info));
    top->addWidget(levelCombo_);
    refreshButton_ = new QPushButton(QStringLiteral("刷新"), this);
    refreshButton_->setObjectName(QStringLiteral("logRefreshButton"));
    top->addWidget(refreshButton_);
    clearButton_ = new QPushButton(QStringLiteral("清空视图"), this);
    clearButton_->setObjectName(QStringLiteral("logClearButton"));
    top->addWidget(clearButton_);
    top->addStretch(1);
    root->addLayout(top);

    fileLabel_ = new QLabel(this);
    fileLabel_->setObjectName(QStringLiteral("logFileLabel"));
    fileLabel_->setWordWrap(true);
    root->addWidget(fileLabel_);

    counterLabel_ = new QLabel(this);
    counterLabel_->setObjectName(QStringLiteral("logCounterLabel"));
    counterLabel_->setWordWrap(true);
    root->addWidget(counterLabel_);

    hintLabel_ = new QLabel(this);
    hintLabel_->setObjectName(QStringLiteral("logHintLabel"));
    hintLabel_->setWordWrap(true);
    hintLabel_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(hintLabel_);

    table_ = new QTableWidget(0, kColumns, this);
    table_->setObjectName(QStringLiteral("logTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("时刻"), QStringLiteral("级别"),
                                       QStringLiteral("分类"), QStringLiteral("内容")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    root->addWidget(table_, 1);

    auto* footer = new QLabel(
        QStringLiteral("日志与报警、审计是**三件事**：把这里调到\"只记错误\"，"
                       "不会让任何一条报警消失，也不会少一条审计流水；反过来"
                       "这边开到跟踪级，也不会多出一条要人确认的报警。"),
        this);
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(footer);

    connect(levelCombo_, &QComboBox::currentIndexChanged, this, &LogPage::applyLevel);
    connect(refreshButton_, &QPushButton::clicked, this, &LogPage::refresh);
    connect(clearButton_, &QPushButton::clicked, this, &LogPage::clearView);

    refresh();
}

void LogPage::attach(mc::Logger* logger) {
    logger_ = logger;
    refresh();
}

void LogPage::applyLevel() {
    if (logger_ == nullptr) {
        return;
    }
    const auto level = static_cast<mc::LogLevel>(levelCombo_->currentData().toInt());
    logger_->setMinLevel(level);
    // 改级别这件事本身值得留一条：排查时"为什么这段日志这么少"的答案往往就在这里。
    logger_->info("log", QStringLiteral("记录级别改为 %1").arg(QString::fromUtf8(
                                                            mc::logLevelText(level)))
                            .toStdString());
    refresh();
}

void LogPage::clearView() {
    if (logger_ != nullptr) {
        logger_->ring().clear();
    }
    table_->setRowCount(0);
    refresh();
}

void LogPage::refresh() {
    if (logger_ == nullptr) {
        hintLabel_->setText(QStringLiteral("运行日志未启用（没有挂日志器）。"));
        levelCombo_->setEnabled(false);
        fileLabel_->setText(QString());
        counterLabel_->setText(QString());
        table_->setRowCount(0);
        return;
    }
    levelCombo_->setEnabled(true);

    // 下拉框显示**当前生效**的那一级，而不是"我上次点了什么"。
    const int levelIndex = static_cast<int>(logger_->minLevel());
    if (levelCombo_->currentIndex() != levelIndex) {
        const QSignalBlocker blocker(levelCombo_);
        levelCombo_->setCurrentIndex(levelIndex);
    }

    const mc::RotatingFileWriter* file = logger_->file();
    if (file != nullptr && file->isOpen()) {
        fileLabel_->setText(
            QStringLiteral("日志文件：%1 ｜ 当前 %2 / %3 字节 ｜ 已轮转 %4 次，保留 %5 份")
                .arg(QString::fromStdString(file->path()))
                .arg(static_cast<qulonglong>(file->currentBytes()))
                .arg(static_cast<qulonglong>(file->maxBytes()))
                .arg(static_cast<qulonglong>(file->rotations()))
                .arg(static_cast<qulonglong>(file->keepFiles())));
    } else if (file != nullptr) {
        fileLabel_->setText(QStringLiteral("日志文件打不开（%1）—— 目前只留在内存里，"
                                           "进程退出就没了")
                                .arg(QString::fromStdString(file->lastError())));
    } else {
        fileLabel_->setText(QStringLiteral("没有挂日志文件：只留在内存环形里（进程退出即丢）。"));
    }

    counterLabel_->setText(
        QStringLiteral("已记录 %1 条 ｜ 被级别挡掉 %2 条 ｜ 环形丢弃 %3 条 ｜ 文件写失败 %4 次")
            .arg(static_cast<qulonglong>(logger_->totalWritten()))
            .arg(static_cast<qulonglong>(logger_->totalFiltered()))
            .arg(static_cast<qulonglong>(logger_->ring().dropped()))
            .arg(static_cast<qulonglong>(logger_->fileWriteFailures())));

    hintLabel_->setText(QStringLiteral("环形只留最近 %1 条（满了丢最旧的）—— "
                                       "界面上看到的不是全部，丢了多少在上面那行里。")
                            .arg(static_cast<qulonglong>(logger_->ring().capacity())));

    const std::deque<mc::LogRecord>& records = logger_->ring().records();
    table_->setRowCount(static_cast<int>(records.size()));
    int row = 0;
    for (const mc::LogRecord& record : records) {
        const QString stamp =
            QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(record.timestampMs))
                .toString(QStringLiteral("HH:mm:ss.zzz"));
        const QColor color = levelColor(record.level);

        auto* timeItem = new QTableWidgetItem(stamp);
        auto* levelItem = new QTableWidgetItem(QString::fromUtf8(mc::logLevelName(record.level)));
        auto* tagItem = new QTableWidgetItem(QString::fromStdString(record.tag));
        auto* textItem = new QTableWidgetItem(QString::fromStdString(record.text));
        levelItem->setForeground(QBrush(color));
        if (record.level == mc::LogLevel::Warn || record.level == mc::LogLevel::Error) {
            textItem->setForeground(QBrush(color));
        }
        table_->setItem(row, 0, timeItem);
        table_->setItem(row, 1, levelItem);
        table_->setItem(row, 2, tagItem);
        table_->setItem(row, 3, textItem);
        ++row;
    }
    if (table_->rowCount() > 0) {
        table_->scrollToBottom();
    }
}

}  // namespace mchost
