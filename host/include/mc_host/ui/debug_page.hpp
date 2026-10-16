#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QGridLayout;
class QLabel;
class QPushButton;
class QSpinBox;

namespace mchost {

class DeviceManager;

// 层 1：通信调试页。
//
// 与总览页的分工：总览页回答"现在好不好"（会话、设备、链路、几条汇总统计），
// 本页回答"**具体是什么数字**"——把解码器、应答跟踪、链路监控三张统计表全部展开，
// 并给出参数的**故障注入**入口（总览页原来那三个开关已删除：注入状态只能有一个真源，
// 两处按钮改同一个状态，迟早出现"界面显示断着、实际没断"）。
//
// 本页不含任何协议判断：所有数字都直接来自 libmc_core 的统计结构。
// 界面只负责如实显示，包括显示"不健康"的数字（CRC 错、重同步丢字节、超时判 Fault）。
class DebugPage : public QWidget {
    Q_OBJECT

public:
    explicit DebugPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void onConnectionChanged(bool connected);

private:
    QLabel* addRow(QGridLayout* grid, int row, const QString& key, const QString& title);
    void setRow(const char* key, const QString& text);
    void applyInjection();
    void clearInjection();

    DeviceManager& manager_;
    QHash<QString, QLabel*> rows_;

    QSpinBox* uplinkDropSpin_ = nullptr;
    QSpinBox* downlinkDropSpin_ = nullptr;
    QSpinBox* uplinkLatencySpin_ = nullptr;
    QPushButton* applyInjectionButton_ = nullptr;
    QPushButton* clearInjectionButton_ = nullptr;
    QLabel* injectionHint_ = nullptr;
};

}  // namespace mchost
