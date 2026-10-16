#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：故障报警页。
//
// 这一页有两块**必须分清**的东西，界面上也分开写：
//   ① 设备此刻的故障锁存 —— 一个状态。它只能由设备自己解除（人工确认 + 恢复校验）。
//   ② 报警记录 —— 一份带时刻的事件流水。清空它只是擦屏幕。
// 把两者合成一个"清除"按钮是安全设计上的错误：那会让人以为点一下屏幕，
// 设备就重新允许启动了。
//
// ⚠️ 与本页相关的红线：**软件确认不能代替物理处置**。
//    故障原因（超速、堵转、接线、CRC 连错）必须先由人在现场查清并排除，
//    本页的"确认"只是允许设备重新自检 —— 它不是复位按钮，更不是急停的反面。
//    急停/断电是**物理**回路，与软件无关，本页也不提供"恢复运行"。
class AlarmPage : public QWidget {
    Q_OBJECT

public:
    explicit AlarmPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshAlarms();
    void onConnectionChanged(bool connected);

private:
    void requestClearFault();

    DeviceManager& manager_;

    QLabel* stateValue_ = nullptr;
    QLabel* faultValue_ = nullptr;
    QLabel* latchValue_ = nullptr;
    QLabel* clearHint_ = nullptr;
    QPushButton* clearFaultButton_ = nullptr;
    QPushButton* clearListButton_ = nullptr;
    QLabel* listCountLabel_ = nullptr;
    QTableWidget* alarmTable_ = nullptr;

    bool lastReady_ = false;
};

}  // namespace mchost
