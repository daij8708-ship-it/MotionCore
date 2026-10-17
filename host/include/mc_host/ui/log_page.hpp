#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mc {
class Logger;
}

namespace mchost {

// 层 1：运行日志页（P1-8）。
//
// 它与**报警页**、**审计**三处都在"记录发生了什么"，但看的东西不同，所以是三页：
//   · 报警页：设备出了需要人处理的事（有级别、要确认、拦启动）；
//   · 审计：人做了 / 试了什么（追责用）；
//   · 本页：程序自己走过哪些路（排查用，最多、最细、可以随时丢）。
//
// 页面上必须能直接回答三个问题，否则这页等于没做：
//   ① 现在记到哪一级了（下拉框就是**当前生效**的那一级）；
//   ② 日志落在哪儿、滚动了没有（路径 / 当前大小 / 已轮转次数 / 保留份数）；
//   ③ 有没有"以为记下来了其实没记"（写失败次数、被级别挡掉的条数、环形丢掉的条数）。
class LogPage : public QWidget {
    Q_OBJECT

public:
    explicit LogPage(QWidget* parent = nullptr);

    // 日志器**不持有**：由应用程序建、这里只读。传 nullptr = 本页显示"未启用"。
    void attach(mc::Logger* logger);

public slots:
    void refresh();

private:
    void applyLevel();
    void clearView();

    mc::Logger* logger_ = nullptr;

    QComboBox* levelCombo_ = nullptr;
    QLabel* fileLabel_ = nullptr;
    QLabel* counterLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QTableWidget* table_ = nullptr;
};

}  // namespace mchost
