#include <gtest/gtest.h>

#include <memory>

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>

#include "mc/device_simulator.hpp"
#include "mc/link_monitor.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/ui/curve_plot.hpp"
#include "mc_host/ui/main_window.hpp"

#include "host_test_util.hpp"

// 界面冒烟用例：真的建窗口、真的点按钮、真的读控件上的字。
//
// 判据一律**读控件**（findChild + text()），不读内部成员：
// 去读成员就等于绕开了本层，测的还是应用服务层，界面错了也照样绿。
//
// 这是冒烟，不是视觉验收：无头环境下"画得对不对"没法自动判定，
// 只能断言"确实画了、画出的点数不是 0"。像素级正确性靠人看截图。
namespace {

using mchost::DeviceManager;
using mchost::MainWindow;

mc::LinkMonitor::Config fastMonitorConfig() {
    mc::LinkMonitor::Config config;
    config.heartbeatPeriodMs = 50U;
    config.missedBeatsToDeclareLinkDown = 3U;
    config.minAckTimeoutMs = 50U;
    return config;
}

// 命令行表格里有没有"这条命令是这个结果"的一行。
bool rowWith(const QTableWidget* table, const QString& name, const QString& result) {
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem* itemName = table->item(row, 1);
        const QTableWidgetItem* itemResult = table->item(row, 3);
        if (itemName != nullptr && itemResult != nullptr && itemName->text() == name &&
            itemResult->text() == result) {
            return true;
        }
    }
    return false;
}

class UiSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<DeviceManager>();
        manager_->setDevice(
            std::make_unique<mchost::SimulatedDevice>(mc::SimulatorConfig{}, fastMonitorConfig()));
        window_ = std::make_unique<MainWindow>(*manager_);
        window_->resize(1000, 700);
        window_->show();
        mchost_test::pumpFor(30);
    }

    template <typename T>
    T* find(const char* name) const {
        return window_->findChild<T*>(QString::fromLatin1(name));
    }

    QString textOf(const char* name) const {
        QLabel* label = find<QLabel>(name);
        return label != nullptr ? label->text() : QString();
    }

    bool connectFromUiAndWaitReady() {
        QPushButton* connect = find<QPushButton>("connectButton");
        if (connect == nullptr) {
            return false;
        }
        connect->click();
        // ⚠ 判 startsWith 而不是 contains："未就绪（正在握手…）"里**也含**"就绪"两个字，
        // 用 contains 会在刚点下连接时就立刻为真 —— 那样的断言等于没等。
        return mchost_test::waitUntil(
            [this]() { return textOf("sessionValue").startsWith(QStringLiteral("就绪")); });
    }

    // 选好控制模式再启动。顺序不能反：设备会拒绝"没选模式就使能"（语义上不成立），
    // 这是设备侧的门禁，不是界面该替它遮掉的。
    bool selectVelocityModeAndStart() {
        QComboBox* mode = find<QComboBox>("modeCombo");
        QPushButton* applyMode = find<QPushButton>("applyModeButton");
        QPushButton* start = find<QPushButton>("startButton");
        QTableWidget* table = find<QTableWidget>("commandLogTable");
        if (mode == nullptr || applyMode == nullptr || start == nullptr || table == nullptr) {
            return false;
        }
        if (!mchost_test::waitUntil([applyMode, start]() {
                return applyMode->isEnabled() && start->isEnabled();
            })) {
            return false;
        }
        mode->setCurrentIndex(1);   // 速度环
        applyMode->click();
        if (!mchost_test::waitUntil([table]() { return rowWith(table, "SET_MODE", "已确认"); })) {
            return false;
        }
        start->click();
        return mchost_test::waitUntil([table]() { return rowWith(table, "START", "已确认"); });
    }

    std::unique_ptr<DeviceManager> manager_;
    std::unique_ptr<MainWindow> window_;
};

TEST_F(UiSmokeTest, TheSimulatedDataSourceIsLabelledInTheBannerAndTheStatusBar) {
    // 「模拟数据」必须在页面上显著可见（任务书阶段八）：含糊地显示一串数字比不显示更坏 ——
    // 曲线页的数字最容易被当成本次实测结果。
    EXPECT_TRUE(textOf("simBanner").contains(QStringLiteral("模拟数据")));
    EXPECT_TRUE(textOf("dataSourceLabel").contains(QStringLiteral("模拟数据")));
}

TEST_F(UiSmokeTest, TheWindowOffersTheThreePagesAndRejectsAnOutOfRangeTab) {
    QTabWidget* tabs = window_->tabs();
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->count(), 3);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("设备总览"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("运动控制"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("数据曲线"));

    EXPECT_FALSE(window_->showTab(-1));
    EXPECT_FALSE(window_->showTab(3));
    EXPECT_TRUE(window_->showTab(1));
}

TEST_F(UiSmokeTest, ConnectingFromTheButtonReachesAReadySessionVisibleOnTheOverview) {
    ASSERT_TRUE(connectFromUiAndWaitReady());

    EXPECT_TRUE(manager_->isConnected());
    EXPECT_TRUE(manager_->device()->sessionReady());
    // 链路监控开着，"正常"才不是一句没有来源的话（界面自己会区分"未监控"）。
    EXPECT_TRUE(textOf("linkValue").contains(QStringLiteral("正常")));
    EXPECT_TRUE(textOf("handshakeValue").contains(QStringLiteral("轮")));
    EXPECT_TRUE(mchost_test::waitUntil([this]() { return manager_->telemetry().size() > 0U; }));
    // 设备状态用的是协议里的状态名（与文档、状态表同一套词），不是另起的中文别名。
    EXPECT_TRUE(textOf("deviceStateValue").contains(QStringLiteral("Ready")));
}

TEST_F(UiSmokeTest, MotionCommandsShowUpInTheTableWithTheirOutcome) {
    ASSERT_TRUE(connectFromUiAndWaitReady());

    // 按钮点下去必须在链路上留下一条可查的记录 —— 这就是"界面不是装饰"的判据。
    EXPECT_TRUE(selectVelocityModeAndStart());
}

TEST_F(UiSmokeTest, AnIllegalStateCommandShowsUpAsNackedWithItsReason) {
    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(selectVelocityModeAndStart());

    // 运行中改模式：门禁在设备侧，界面要做的不是把按钮灰掉，而是把拒绝和原因显示出来。
    QComboBox* mode = find<QComboBox>("modeCombo");
    QPushButton* applyMode = find<QPushButton>("applyModeButton");
    QTableWidget* table = find<QTableWidget>("commandLogTable");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(applyMode, nullptr);
    ASSERT_NE(table, nullptr);

    mode->setCurrentIndex(2);   // 位置环
    applyMode->click();

    ASSERT_TRUE(mchost_test::waitUntil([table]() { return rowWith(table, "SET_MODE", "被拒绝"); }));
    bool reasonShown = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem* result = table->item(row, 3);
        const QTableWidgetItem* detail = table->item(row, 4);
        if (result != nullptr && detail != nullptr && result->text() == QStringLiteral("被拒绝") &&
            detail->text() == QStringLiteral("当前设备状态不允许该命令")) {
            reasonShown = true;
        }
    }
    EXPECT_TRUE(reasonShown);
}

TEST_F(UiSmokeTest, TheCurvePageDrawsAndReportsItsCounters) {
    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return manager_->telemetry().size() > 20U; }));
    ASSERT_TRUE(window_->showTab(2));

    mchost::CurvePlot* plot = find<mchost::CurvePlot>("curvePlot");
    ASSERT_NE(plot, nullptr);

    // 抓一张图 = 强制走一次 paintEvent。无头环境里没人看它，"不抓就没人画"。
    window_->grab();

    EXPECT_GT(plot->repaintCount(), 0U);
    EXPECT_GT(plot->lastDrawnPoints(), 0U);
    EXPECT_TRUE(textOf("curveStatsLabel").contains(QStringLiteral("tick")));
    EXPECT_FALSE(textOf("curveStatsLabel").contains(QStringLiteral("缓冲 0 条")));
}

TEST_F(UiSmokeTest, InjectingAnUplinkDropRaisesAnAlarmThatShowsUpOnTheOverview) {
    ASSERT_TRUE(connectFromUiAndWaitReady());

    QPushButton* inject = find<QPushButton>("injectUplinkButton");
    ASSERT_NE(inject, nullptr);
    ASSERT_TRUE(inject->isEnabled());
    inject->click();
    ASSERT_TRUE(inject->isChecked());

    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return textOf("alarmValue").contains(QStringLiteral("共")); }));

    // 收工：把注入撤掉并等它自己重新握上手，免得把链路留在断的状态。
    inject->click();
    EXPECT_TRUE(mchost_test::waitUntil([this]() { return manager_->device()->sessionReady(); }));
}

}  // namespace
