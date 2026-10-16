#pragma once

#include <QMainWindow>

class QLabel;
class QTabWidget;

namespace mchost {

class CurvePage;
class DeviceManager;
class MotionPage;
class OverviewPage;

// 层 1：主窗口。只负责组装页面 + 状态栏，不含任何协议细节。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(DeviceManager& manager, QWidget* parent = nullptr);

    // 自检用：切到指定页（0..2）。返回是否切成功 —— 越界不静默。
    bool showTab(int index);

    QTabWidget* tabs() const noexcept { return tabs_; }

private slots:
    void onConnectionChanged(bool connected);
    void onAlarmRaised(const QString& text);

private:
    DeviceManager& manager_;
    QTabWidget* tabs_ = nullptr;
    OverviewPage* overviewPage_ = nullptr;
    MotionPage* motionPage_ = nullptr;
    CurvePage* curvePage_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* dataSourceLabel_ = nullptr;
};

}  // namespace mchost
