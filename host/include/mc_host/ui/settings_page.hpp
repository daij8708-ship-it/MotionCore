#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：系统设置页 —— 选数据源并把它接上，顺便管操作员角色与审计流水。
//
// 这一页存在的理由很具体：**换数据源必须有一个地方可做，而且必须只有一处**。
// "用模拟器演示"与"接真实串口"如果各有一套入口，迟早出现"界面以为自己在说串口、
// 实际还连着模拟器"—— 那种情况下所有数字都不可信，且看不出问题在哪。
//
// 它刻意不提供"预填几个常用波特率之外的自由输入"之类的小聪明，
// 但**必须**把边界说清楚：串口设备上哪些值是回读、哪些只是上位机设定值。
//
// 角色与审计也放在这一页：它们都是"这台终端由谁在用、用过什么"，
// 与数据源同属"关于这套软件本身的设置"，另开一页只会把页数堆上去。
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshPorts();
    void refreshRole();
    void refreshAudit();
    void onConnectionChanged(bool connected);

private:
    void updateSourceUi();
    void applySource();
    void applyRole();

    DeviceManager& manager_;

    QComboBox* sourceCombo_ = nullptr;
    QComboBox* portCombo_ = nullptr;
    QComboBox* baudCombo_ = nullptr;
    QComboBox* dataBitsCombo_ = nullptr;
    QComboBox* parityCombo_ = nullptr;
    QComboBox* stopBitsCombo_ = nullptr;
    QComboBox* flowCombo_ = nullptr;
    QPushButton* refreshPortsButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QLabel* currentSourceValue_ = nullptr;
    QLabel* portCountLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;      // 上一次动作的结果（换源/开端口成没成）
    QLabel* noteLabel_ = nullptr;      // 固定说明：这一页能证明什么、不能证明什么
    QLabel* simBanner_ = nullptr;

    QComboBox* roleCombo_ = nullptr;
    QPushButton* applyRoleButton_ = nullptr;
    QLabel* roleValue_ = nullptr;
    QLabel* roleDutyLabel_ = nullptr;
    QLabel* roleHintLabel_ = nullptr;   // 上一次改角色的结果
    QLabel* auditCountLabel_ = nullptr;
    QTableWidget* auditTable_ = nullptr;
    QPushButton* refreshAuditButton_ = nullptr;
};

}  // namespace mchost
