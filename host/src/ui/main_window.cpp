#include "mc_host/ui/main_window.hpp"

#include <QFont>
#include <QLabel>
#include <QStatusBar>
#include <QTabWidget>

#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/ui/alarm_page.hpp"
#include "mc_host/ui/axis_page.hpp"
#include "mc_host/ui/curve_page.hpp"
#include "mc_host/ui/debug_page.hpp"
#include "mc_host/ui/history_page.hpp"
#include "mc_host/ui/log_page.hpp"
#include "mc_host/ui/motion_page.hpp"
#include "mc_host/ui/overview_page.hpp"
#include "mc_host/ui/param_page.hpp"
#include "mc_host/ui/recipe_page.hpp"
#include "mc_host/ui/sequence_page.hpp"
#include "mc_host/ui/settings_page.hpp"

namespace mchost {

MainWindow::MainWindow(DeviceManager& manager, QWidget* parent)
    : QMainWindow(parent), manager_(manager) {
    setWindowTitle(QStringLiteral("工业运动控制与实时监测系统 —— 上位机（mc_host）"));
    resize(1100, 760);

    tabs_ = new QTabWidget(this);
    overviewPage_ = new OverviewPage(manager_, tabs_);
    motionPage_ = new MotionPage(manager_, tabs_);
    axisPage_ = new AxisPage(axisGroup_, tabs_);
    paramPage_ = new ParamPage(manager_, tabs_);
    recipePage_ = new RecipePage(manager_, tabs_);
    sequencePage_ = new SequencePage(manager_, tabs_);
    curvePage_ = new CurvePage(manager_, tabs_);
    alarmPage_ = new AlarmPage(manager_, tabs_);
    historyPage_ = new HistoryPage(manager_, tabs_);
    settingsPage_ = new SettingsPage(manager_, tabs_);
    debugPage_ = new DebugPage(manager_, tabs_);
    logPage_ = new LogPage(tabs_);

    // 页序按"从看状态到动手调"排：总览 → 运动 → 双轴 → 参数 → 配方 → 程序 →
    // 曲线 → 报警 → 历史 → 设置 → 调试 → 日志。
    // 双轴页紧跟在运动页后面：它管的是"两台驱动器怎么一起动"，
    // 是运动页那件事的上一层（协议不变，多了"把两条链路当一个组来编排"）。
    // 参数页紧挨着运动页：它们改的是同一台设备的两种东西（运动 vs 控制器系数）。
    // 配方页紧跟在参数页后面：它管的也是"设备按什么参数干活"，只是高一层 ——
    // 参数页改的是此刻的系数，配方页改的是"按哪一份存档的参数干活"。
    // 程序页紧跟着配方页：配方回答"按什么参数"、程序回答"按什么顺序"，
    // 两者正交，摆在一起是因为它们一起构成"这台设备怎么干活"。
    // 报警页紧跟着曲线页：曲线看出异常之后，下一步就是去查报警记录与当前故障。
    // 历史页在报警页之后、设置页之前：先看"现在怎么了、刚才是怎么了"，再决定"跟谁说话"。
    // 日志页排在最后、紧挨调试页：两者都是排查用的，而日志比调试页更"底层"。
    tabs_->addTab(overviewPage_, QStringLiteral("设备总览"));
    tabs_->addTab(motionPage_, QStringLiteral("运动控制"));
    tabs_->addTab(axisPage_, QStringLiteral("双轴协调"));
    tabs_->addTab(paramPage_, QStringLiteral("PID 参数"));
    tabs_->addTab(recipePage_, QStringLiteral("工艺配方"));
    tabs_->addTab(sequencePage_, QStringLiteral("任务调度"));
    tabs_->addTab(curvePage_, QStringLiteral("数据曲线"));
    tabs_->addTab(alarmPage_, QStringLiteral("故障报警"));
    tabs_->addTab(historyPage_, QStringLiteral("历史数据"));
    tabs_->addTab(settingsPage_, QStringLiteral("系统设置"));
    tabs_->addTab(debugPage_, QStringLiteral("通信调试"));
    tabs_->addTab(logPage_, QStringLiteral("运行日志"));
    setCentralWidget(tabs_);

    // 状态栏里再放一份数据来源与连接状态。
    // 「模拟数据」这件事必须在**每一页**都看得见，不能只藏在总览页里 ——
    // 曲线页的数字最容易被误当成实测结果。
    dataSourceLabel_ = new QLabel(this);
    dataSourceLabel_->setObjectName(QStringLiteral("dataSourceLabel"));
    QFont sourceFont = dataSourceLabel_->font();
    sourceFont.setBold(true);
    dataSourceLabel_->setFont(sourceFont);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));

    statusBar()->addWidget(dataSourceLabel_);
    statusBar()->addPermanentWidget(statusLabel_);

    connect(&manager_, &DeviceManager::connectionChanged, this, &MainWindow::onConnectionChanged);
    connect(&manager_, &DeviceManager::alarmRaised, this, &MainWindow::onAlarmRaised);
    connect(&manager_, &DeviceManager::operationRefused, this, &MainWindow::onOperationRefused);
    connect(&manager_, &DeviceManager::statsRefreshed, this,
            [this]() { onConnectionChanged(manager_.isConnected()); });

    onConnectionChanged(manager_.isConnected());
}

bool MainWindow::showTab(int index) {
    if (index < 0 || index >= tabs_->count()) {
        return false;
    }
    tabs_->setCurrentIndex(index);
    return true;
}

void MainWindow::attachLogger(mc::Logger* logger) {
    if (logPage_ != nullptr) {
        logPage_->attach(logger);
    }
}

int MainWindow::tabIndexFor(const QString& title) const {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabText(i) == title) {
            return i;
        }
    }
    return -1;
}

void MainWindow::onConnectionChanged(bool connected) {
    const IDevice* device = manager_.device();

    if (device != nullptr && device->isSimulated()) {
        dataSourceLabel_->setText(QStringLiteral("★ 模拟数据（%1）")
                                      .arg(QString::fromUtf8(device->kindName())));
        dataSourceLabel_->setStyleSheet(QStringLiteral("color:#8a5a00;"));
    } else if (device != nullptr) {
        dataSourceLabel_->setText(QStringLiteral("■ 真实设备（%1）")
                                      .arg(QString::fromUtf8(device->kindName())));
        dataSourceLabel_->setStyleSheet(QStringLiteral("color:#1b5e20;"));
    }

    if (!connected) {
        statusLabel_->setText(QStringLiteral("未连接"));
        return;
    }

    const bool ready = device != nullptr && device->sessionReady();
    const QString session = ready ? QStringLiteral("会话就绪")
                                  : QStringLiteral("未就绪（握手中 / 等链路恢复）");
    statusLabel_->setText(QStringLiteral("%1 | tick %2 | 遥测 %3 条")
                              .arg(session)
                              .arg(manager_.ticks())
                              .arg(manager_.telemetry().size()));
}

void MainWindow::onAlarmRaised(std::uint16_t code, const QString& text) {
    // 代码暂时不用（状态栏只报文本），但参数必须接：信号的形状要能撑住
    // 后面"按代码跳到报警页定位"这件事，不能等到那时再改一遍信号。
    Q_UNUSED(code);
    statusBar()->showMessage(text, 6000);
}

void MainWindow::onOperationRefused(const QString& text) {
    // 被拒的提示**留得比报警久一点**：报警是"发生了什么"，被拒是"你想做的事没做成、
    // 而且原因是这个"—— 后者往往还要求人去另一个页面改点什么，六秒不够读完。
    // 只在状态栏显示而不弹窗，是因为绝大多数的拒绝都已经被页面自己的结果标签
    // 说清了；状态栏这一份是兜底，不是唯一出口。
    statusBar()->showMessage(QStringLiteral("⛔ %1").arg(text), 12000);
}

}  // namespace mchost
