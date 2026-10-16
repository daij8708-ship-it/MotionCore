#pragma once

#include <cstdint>
#include <deque>

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：PID 参数配置页。
//
// 这一页是 PID 的**唯一可写入口**（运动控制页原来那一组已经删掉）。
// 同一个参数有两个可写入口，迟早会出现"两处显示的值不一样、不知道该信哪个" ——
// 那正是原项目三个假下拉框的教训换来的规则：**一个参数只有一个真源**。
//
// ⚠️ 本页显示的"下发记录"，**不是设备的回读值**。协议 v1 里没有"读参数"的消息，
//    遥测载荷（20 B）也不带 PID，所以"设备现在到底用的多少"这件事在协议层面
//    根本答不出来。页面如实写这句话，而不是把输入框里的数字当成设备的值摆出来 ——
//    后者看起来更完整，但它是一个没有来源的数字。
class ParamPage : public QWidget {
    Q_OBJECT

public:
    explicit ParamPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshLog();
    void onConnectionChanged(bool connected);

private:
    struct PidSend {
        std::uint16_t seq = 0U;
        double kp = 0.0;
        double ki = 0.0;
        double kd = 0.0;
    };

    void applyPid();

    DeviceManager& manager_;
    std::deque<PidSend> sends_;   // 新→旧；只记本页发出去的，用来回填表格
    bool lastReady_ = false;      // 会话就绪是异步变的，用来决定要不要重算控件可用性

    QDoubleSpinBox* kpSpin_ = nullptr;
    QDoubleSpinBox* kiSpin_ = nullptr;
    QDoubleSpinBox* kdSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    QTableWidget* logTable_ = nullptr;

    static constexpr std::size_t kMaxSends = 32U;
};

}  // namespace mchost
