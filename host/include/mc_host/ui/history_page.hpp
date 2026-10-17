#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：历史数据页 —— 从 SQLite 里回看"当时到底发生了什么"。
//
// 它只读不写（唯一的写动作是导出 CSV）。库里写什么由应用服务层决定，
// 这一页不参与 —— 界面一旦自己决定"哪些数据值得存"，就会出现
// "屏幕上有的、历史里没有"这种最不该有的差别。
//
// 一个刻意的设计：**时间轴用会话相对毫秒**（与曲线页同一口径），
// 而会话本身用墙上时钟标示。绝对时间轴在仿真里毫无意义（时钟是调用方喂的），
// 但"这是哪一次会话"必须能被人认出来，所以两者都要有。
class HistoryPage : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshSessions();
    void runQuery();

protected:
    // 切到本页时**先用当下的计数刷一次**。
    //
    // 少了这一步，页面上的统计会停在上一次刷新的那一刻（实际是"刚连上"的时候）：
    // 屏幕上写着"已落盘 0 条"，而库里明明已经攒了一大段数据 ——
    // 这种"显示的数和真实状态不一致"恰好是最容易被当成"落库没生效"的那种误导。
    // 页面不自己打库：查询照旧只在切换本页、按下查询、或链路状态变化时发生。
    void showEvent(QShowEvent* event) override;

private:
    void exportCsv();
    qint64 currentSessionId() const;

    DeviceManager& manager_;

    QLabel* storeValue_ = nullptr;
    QLabel* storeErrorLabel_ = nullptr;
    QComboBox* sessionCombo_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QSpinBox* fromSpin_ = nullptr;
    QSpinBox* toSpin_ = nullptr;
    QPushButton* queryButton_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QTableWidget* sampleTable_ = nullptr;
    QTableWidget* alarmTable_ = nullptr;
    QTableWidget* eventTable_ = nullptr;
    QTableWidget* commandTable_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QLabel* exportLabel_ = nullptr;
};

}  // namespace mchost
