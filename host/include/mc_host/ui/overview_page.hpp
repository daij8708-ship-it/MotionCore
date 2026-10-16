#pragma once

#include <QWidget>

// 前置声明一律放在 namespace 之外。写在参数表里的 `class QGridLayout*` 会把
// **mchost::QGridLayout** 声明成一个新类型，而不是 Qt 那个 —— 报错是一串
// "invalid use of incomplete type"，且直到取成员时才炸。
class QGridLayout;
class QLabel;
class QPushButton;

namespace mchost {

class DeviceManager;

// 层 1：设备总览页。
//
// 这一页只做一件事：把**已经判定好的事实**如实显示出来。它不推导状态、
// 不算链路是否正常、不猜命令有没有生效 —— 那些都在 libmc_core 里判定过。
// 界面自己再算一遍就会出现第二套规则，两边迟早不一致。
class OverviewPage : public QWidget {
    Q_OBJECT

public:
    explicit OverviewPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void onConnectionChanged(bool connected);

private:
    QLabel* addRow(QGridLayout* grid, int row, const QString& title);
    void refreshDeviceSection();
    void refreshLinkSection();
    void refreshStatsSection();

    DeviceManager& manager_;

    QLabel* simBanner_ = nullptr;
    QLabel* sessionValue_ = nullptr;
    QLabel* handshakeValue_ = nullptr;
    QLabel* deviceStateValue_ = nullptr;
    QLabel* faultValue_ = nullptr;
    QLabel* motionValue_ = nullptr;
    QLabel* linkValue_ = nullptr;
    QLabel* rttValue_ = nullptr;
    QLabel* windowValue_ = nullptr;
    QLabel* heartbeatValue_ = nullptr;
    QLabel* framesValue_ = nullptr;
    QLabel* telemetryValue_ = nullptr;
    QLabel* decodeValue_ = nullptr;
    QLabel* timeoutValue_ = nullptr;
    QLabel* alarmValue_ = nullptr;
    QLabel* internalValue_ = nullptr;

    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QPushButton* injectUplinkButton_ = nullptr;
    QPushButton* injectDownlinkButton_ = nullptr;
    QPushButton* injectLatencyButton_ = nullptr;
    QPushButton* clearFaultsButton_ = nullptr;
};

}  // namespace mchost
