#pragma once

#include <cstdint>

#include <QMainWindow>

#include "mc_host/axis_group.hpp"

class QLabel;
class QTabWidget;

namespace mc {
class Logger;
}

namespace mchost {

class AlarmPage;
class AxisPage;
class CurvePage;
class DebugPage;
class DeviceManager;
class HistoryPage;
class LogPage;
class MotionPage;
class OverviewPage;
class ParamPage;
class RecipePage;
class SequencePage;
class SettingsPage;

// 层 1：主窗口。只负责组装页面 + 状态栏，不含任何协议细节。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(DeviceManager& manager, QWidget* parent = nullptr);

    // 把应用程序建好的日志器接给"运行日志"页。**必须在这里接而不是构造时**：
    // 日志的去处（文件路径、滚动参数）是**应用程序**的决定，主窗口不该知道
    // 日志写到哪儿 —— 它只负责把那一份显示出来。
    // 传 nullptr（或从不调用）= 该页显示"未启用"，其余功能不受影响。
    void attachLogger(mc::Logger* logger);

    // 自检用：切到指定页。返回是否切成功 —— 越界不静默。
    bool showTab(int index);

    // 按页名找它是第几页（找不到返回 -1）。
    //
    // 它是给测试用的：用例想表达的是"切到数据曲线页"，而不是"切到第 3 页"。
    // 页序一旦插进一页，写死数字的用例会**指到另一页上而照样通过** ——
    // 那种失败最难查，因为它看起来是绿的。
    int tabIndexFor(const QString& title) const;

    QTabWidget* tabs() const noexcept { return tabs_; }

private slots:
    void onConnectionChanged(bool connected);
    // 报警信号带代码与文本。状态栏只拿文本，代码留着：将来要按代码做跳转
    // （"点这条提示直接去故障报警页定位那一条"）时不用再改信号。
    void onAlarmRaised(std::uint16_t code, const QString& text);
    // 一次操作被拒（没权限 / 没连上 / 联锁没满足）。状态栏是**兜底**：
    // 有结果标签的页面会显示在自己的标签上，这里保证"任何被拒都至少看得见一次"。
    void onOperationRefused(const QString& text);

private:
    DeviceManager& manager_;
    QTabWidget* tabs_ = nullptr;
    OverviewPage* overviewPage_ = nullptr;
    MotionPage* motionPage_ = nullptr;
    // 轴组由主窗口持有：它是一条**独立的**业务（两台驱动器），
    // 与"当前数据源那一台设备"没有耦合 —— 不共用 manager_，也不共用它的定时器。
    AxisGroup axisGroup_;
    AxisPage* axisPage_ = nullptr;
    ParamPage* paramPage_ = nullptr;
    RecipePage* recipePage_ = nullptr;
    SequencePage* sequencePage_ = nullptr;
    CurvePage* curvePage_ = nullptr;
    AlarmPage* alarmPage_ = nullptr;
    HistoryPage* historyPage_ = nullptr;
    SettingsPage* settingsPage_ = nullptr;
    DebugPage* debugPage_ = nullptr;
    LogPage* logPage_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* dataSourceLabel_ = nullptr;
};

}  // namespace mchost
