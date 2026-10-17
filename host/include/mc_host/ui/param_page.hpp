#pragma once

#include <cstdint>
#include <deque>

#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：设备参数配置页（PID + 软限位）。
//
// 这一页是这两组设备参数的**唯一可写入口**（运动控制页原来那一组已经删掉）。
// 同一个参数有两个可写入口，迟早会出现"两处显示的值不一样、不知道该信哪个" ——
// 那正是原项目三个假下拉框的教训换来的规则：**一个参数只有一个真源**。
//
// ⚠️ 本页的"下发记录"是**上位机发出去的东西**，不是设备的实存值。
//    要回答"设备现在到底用的哪一组"，只有一条路：按下回读，看设备回过来的那一段
//    （协议 v2 的 READ_PARAMS）。页面把这两件事分开显示，而不是把输入框里的数字
//    摆成设备的值 —— 后者看起来更完整，但它是一个没有来源的数字。
class ParamPage : public QWidget {
    Q_OBJECT

public:
    explicit ParamPage(DeviceManager& manager, QWidget* parent = nullptr);

public slots:
    void refresh();
    void refreshLog();
    void refreshReadback();
    void onConnectionChanged(bool connected);

private:
    struct PidSend {
        std::uint16_t seq = 0U;
        double kp = 0.0;
        double ki = 0.0;
        double kd = 0.0;
    };

    void applyPid();
    void applyLimits();

    DeviceManager& manager_;
    std::deque<PidSend> sends_;   // 新→旧；只记本页发出去的，用来回填表格
    bool lastReady_ = false;      // 会话就绪是异步变的，用来决定要不要重算控件可用性

    QDoubleSpinBox* kpSpin_ = nullptr;
    QDoubleSpinBox* kiSpin_ = nullptr;
    QDoubleSpinBox* kdSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    QTableWidget* logTable_ = nullptr;

    // 软限位（P1-6）与参数回读。两者放在同一页是因为它们回答的是同一个问题：
    // "设备侧现在被设成了什么、我设的落地了没有"。
    QCheckBox* limitEnabledCheck_ = nullptr;
    QDoubleSpinBox* limitMinSpin_ = nullptr;
    QDoubleSpinBox* limitMaxSpin_ = nullptr;
    QPushButton* applyLimitsButton_ = nullptr;
    QLabel* limitsResultLabel_ = nullptr;
    QPushButton* readbackButton_ = nullptr;
    QLabel* readbackLabel_ = nullptr;

    static constexpr std::size_t kMaxSends = 32U;
};

}  // namespace mchost
