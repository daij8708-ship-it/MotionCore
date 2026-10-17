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
//
// 同一个道理适用于「启动联锁」清单：它**不**把启动按钮灰掉，而是把每一条的前因后果
// 列在按钮下面。理由与上面那句一样 —— 灰一个按钮不解释，等于让人干瞪眼；
// 而清单是"现在缺什么、要变成什么样"，照着做就行。
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
    void applyHome();
    void updateTargetRange();
    void refreshInterlocks();
    void refreshHomeStatus();

    DeviceManager& manager_;
    bool lastSessionReady_ = false;

    QComboBox* modeCombo_ = nullptr;
    QPushButton* applyModeButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QTableWidget* interlockTable_ = nullptr;
    QLabel* interlockSummary_ = nullptr;
    QDoubleSpinBox* targetSpin_ = nullptr;
    QLabel* targetUnitLabel_ = nullptr;
    QPushButton* applyTargetButton_ = nullptr;
    QLabel* lastResultLabel_ = nullptr;
    QTableWidget* logTable_ = nullptr;

    // 回零（P1-6）。速度与时限在这一页给：它们是**每台机器**的事
    // （丝杠导程、负载惯量、行程开关位置），写死在协议或界面里等于替现场做决定。
    QDoubleSpinBox* homeSpeedSpin_ = nullptr;
    QDoubleSpinBox* homeTimeoutSpin_ = nullptr;
    QPushButton* homeButton_ = nullptr;
    QLabel* homeStatusLabel_ = nullptr;
};

}  // namespace mchost
