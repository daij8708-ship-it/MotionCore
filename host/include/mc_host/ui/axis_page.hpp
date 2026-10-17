#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class AxisGroup;

// 层 1：双轴协调页（P1-7）。
//
// 它显式地把"轴组是**上位机侧**的一层编排"这件事摆出来：两轴各自是一个完整的
// 单轴设备（各有一条链路、各有一台驱动器），这一页只是把两条命令的下发与收尾
// 绑在一起 —— 协议没有为它改过一个字节。
//
// 判据按页面的规矩来：每个按钮点下去都要能看到结果（每轴这一步的结局），
// 而不是"按下去了就算成功"。规划不出来时也要说清是为什么（行程为零？速度上限
// 没给正值？），因为"点了没反应"是最难查的一种。
class AxisPage : public QWidget {
    Q_OBJECT

public:
    explicit AxisPage(AxisGroup& group, QWidget* parent = nullptr);

public slots:
    // 20 Hz：两轴的实时数值（状态 / 位置 / 速度）。
    void refresh();
    // 组状态变化时：阶段、结局、安全闸。
    void refreshStatus();

private:
    void applyConnect();
    void applyMove();
    void applyStop();
    void updatePlanPreview();

    AxisGroup& group_;

    QPushButton* connectButton_ = nullptr;
    QLabel* connectLabel_ = nullptr;

    QDoubleSpinBox* targetXSpin_ = nullptr;
    QDoubleSpinBox* targetYSpin_ = nullptr;
    QDoubleSpinBox* speedSpin_ = nullptr;
    QPushButton* moveButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* planLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* safetyLabel_ = nullptr;

    QTableWidget* axisTable_ = nullptr;
};

}  // namespace mchost
