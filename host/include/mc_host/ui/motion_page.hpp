#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：运动控制页。
//
// 每个控件都直接对应一条协议命令，而且**都必须显示结果**：命令行表格里能看到
// 它是不是被确认、被拒（原因）、还是超时。任务书说"禁止仅实现界面而没有业务逻辑"，
// 这一页的判据就是 —— 每个按钮点下去都会在链路上留下一条可查的记录。
//
// 刻意**不按设备状态禁用控制控件**（只按"会话是否就绪"禁用）：门禁的权威在设备侧，
// 把按钮灰掉会让人永远看不到"为什么被拒"。让拒绝发生、并把原因显示出来，信息量更大。
class MotionPage : public QWidget {
    Q_OBJECT

public:
    explicit MotionPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshLog();
    void onConnectionChanged(bool connected);

private:
    void applyMode();
    void applyTarget();
    void updateTargetRange();

    DeviceManager& manager_;
    bool lastSessionReady_ = false;

    QComboBox* modeCombo_ = nullptr;
    QPushButton* applyModeButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QDoubleSpinBox* targetSpin_ = nullptr;
    QLabel* targetUnitLabel_ = nullptr;
    QPushButton* applyTargetButton_ = nullptr;
    QLabel* lastResultLabel_ = nullptr;
    QTableWidget* logTable_ = nullptr;
};

}  // namespace mchost
