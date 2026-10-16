#pragma once

#include <cstdint>

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace mchost {

class CurvePage;
class CurvePlot;
class DeviceManager;

// 层 1：实时数据曲线页。
//
// 刷新链路的三个节拍刻意是分开的，这也是 ADR-006 的可执行形态：
//   遥测 100 Hz 进环形缓冲 → 本页按 20 Hz（statsRefreshed）取窗、抽稀、重绘。
// 让 100 Hz 的数据直接驱动 100 Hz 的重绘，就是在给看不见的差别烧 CPU。
// 页面把"样本数 / 画出的点数 / 重绘次数"都显示出来，节流有没有生效一眼可查。
class CurvePage : public QWidget {
    Q_OBJECT

public:
    explicit CurvePage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();

private:
    void pullAndDraw();
    void updateStatsLabel();

    DeviceManager& manager_;
    CurvePlot* plot_ = nullptr;
    QCheckBox* velocityBox_ = nullptr;
    QCheckBox* positionBox_ = nullptr;
    QCheckBox* targetBox_ = nullptr;
    QComboBox* windowCombo_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;

    bool paused_ = false;
    std::size_t drawnPoints_ = 0U;
};

}  // namespace mchost
