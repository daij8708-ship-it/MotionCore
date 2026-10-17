#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：故障报警页。
//
// 这一页上有**三件必须分清**的东西，界面上也分开写：
//   ① 设备此刻的故障锁存 —— 一个状态。它只能由设备自己解除（人工确认 + 恢复校验）。
//   ② 报警清单 —— 一份带代码、等级、确认状态的流水。清空它只是擦屏幕。
//   ③ 启动门禁 —— "还有几条报警没确认"。它拦的是启动按钮，不是屏幕。
// 把前两者合成一个"清除"按钮是安全设计上的错误：那会让人以为点一下屏幕，
// 设备就重新允许启动了。
//
// ⚠️ 与本页相关的红线：**软件确认不能代替物理处置**。
//    故障原因（超速、堵转、接线、CRC 连错）必须先由人在现场查清并排除，
//    本页的"确认"只是允许设备重新自检、并让启动闸门重新打开 ——
//    它不是复位按钮，更不是急停的反面。
//    急停/断电是**物理**回路，与软件无关，本页也不提供"恢复运行"。
class AlarmPage : public QWidget {
    Q_OBJECT

public:
    explicit AlarmPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshAlarms();
    void onConnectionChanged(bool connected);

private slots:
    void requestClearFault();
    void acknowledgeSelected();
    void acknowledgeAll();

private:
    // 当前选中的那一行是哪条报警（-1 = 没选中）。代码存在行的 UserRole 里，
    // 而不是从显示文本里反解 —— 文本是给人看的，随时可能改。
    int selectedCode() const;

    DeviceManager& manager_;

    QLabel* stateValue_ = nullptr;
    QLabel* faultValue_ = nullptr;
    QLabel* latchValue_ = nullptr;
    QLabel* clearHint_ = nullptr;
    QPushButton* clearFaultButton_ = nullptr;
    QPushButton* clearListButton_ = nullptr;
    QComboBox* levelFilter_ = nullptr;
    QPushButton* acknowledgeButton_ = nullptr;
    QPushButton* acknowledgeAllButton_ = nullptr;
    QLabel* listCountLabel_ = nullptr;
    QTableWidget* alarmTable_ = nullptr;

    bool lastReady_ = false;
};

}  // namespace mchost
