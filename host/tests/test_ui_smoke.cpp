#include <gtest/gtest.h>

#include <memory>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
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

TEST_F(UiSmokeTest, TheWindowOffersTheDesignedPagesAndRejectsAnOutOfRangeTab) {
    QTabWidget* tabs = window_->tabs();
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->count(), 6);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("设备总览"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("运动控制"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("PID 参数"));
    EXPECT_EQ(tabs->tabText(3), QStringLiteral("数据曲线"));
    EXPECT_EQ(tabs->tabText(4), QStringLiteral("故障报警"));
    EXPECT_EQ(tabs->tabText(5), QStringLiteral("通信调试"));

    EXPECT_FALSE(window_->showTab(-1));
    EXPECT_FALSE(window_->showTab(6));
    EXPECT_TRUE(window_->showTab(1));

    // PID 有两个写入口就会打架：Kp/Ki/Kd 在整个窗口里必须各只有一份，
    // 旧的故障注入按钮也必须彻底消失（注入状态只能有一个真源）。
    EXPECT_NE(find<QDoubleSpinBox>("kpSpin"), nullptr);
    EXPECT_EQ(window_->findChildren<QDoubleSpinBox*>("kpSpin").size(), 1);
    EXPECT_EQ(window_->findChildren<QDoubleSpinBox*>("kiSpin").size(), 1);
    EXPECT_EQ(window_->findChildren<QDoubleSpinBox*>("kdSpin").size(), 1);
    EXPECT_EQ(window_->findChildren<QPushButton*>("injectUplinkButton").size(), 0);
    EXPECT_EQ(window_->findChildren<QPushButton*>("injectDownlinkButton").size(), 0);
    EXPECT_EQ(window_->findChildren<QPushButton*>("injectLatencyButton").size(), 0);
    EXPECT_EQ(window_->findChildren<QPushButton*>("clearFaultsButton").size(), 0);
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
    ASSERT_TRUE(window_->showTab(3));   // 数据曲线

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

    // 注入只在通信调试页上。设成"每 1 字节丢 1"就是上行全丢。
    ASSERT_TRUE(window_->showTab(5));
    QSpinBox* spin = find<QSpinBox>("uplinkDropSpin");
    QPushButton* apply = find<QPushButton>("applyInjectionButton");
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_TRUE(spin->isEnabled());
    spin->setValue(1);
    apply->click();
    EXPECT_TRUE(textOf("injectionHint").contains(QStringLiteral("已应用")));

    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return textOf("alarmValue").contains(QStringLiteral("共")); }));

    // 收工：把注入撤掉并等它自己重新握上手，免得把链路留在断的状态。
    find<QPushButton>("clearInjectionButton")->click();
    EXPECT_TRUE(mchost_test::waitUntil([this]() { return manager_->device()->sessionReady(); }));
}

// 故障从"真的发生"到"被人工确认清除"的整条路。
//
// 刻意不用 forcedFaultCode（那是把一个状态位设上去）：走的是**F7 设备侧通信丢失保护**
// 的真实路径 —— 设备在 Running 中再也收不到帧，到点自己撤目标进 Fault。
TEST_F(UiSmokeTest, ALatchedFaultSurvivesReconnectAndIsClearedOnlyByExplicitConfirmation) {
    // 通信丢失保护窗口取短一点：不用等 3 秒就能让它自己走进 Fault。
    mc::SimulatorConfig simConfig;
    simConfig.commLossTimeoutMs = 150U;
    manager_->setDevice(std::make_unique<mchost::SimulatedDevice>(simConfig, fastMonitorConfig()));

    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(selectVelocityModeAndStart());

    // 下行全丢（上位机 → 设备一个字节都到不了）：设备迟早发现自己被"静音"了。
    ASSERT_TRUE(window_->showTab(5));
    find<QSpinBox>("downlinkDropSpin")->setValue(1);
    find<QPushButton>("applyInjectionButton")->click();

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->deviceState() == mc::DeviceState::Fault;
    }));
    EXPECT_TRUE(manager_->device()->faultLatched());
    EXPECT_EQ(manager_->device()->faultCode(), mc::FaultCode::CommunicationLoss);

    // 恢复链路。设备会自己重新握上手，但**故障不许被握手顺手洗掉** ——
    // "设备出事 → 人跑过去拔线 → 重新上电 → 界面显示一切正常"正是最危险的那条路径。
    ASSERT_TRUE(window_->showTab(5));
    find<QPushButton>("clearInjectionButton")->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->sessionReady() &&
               manager_->device()->deviceState() == mc::DeviceState::Fault;
    }));
    EXPECT_TRUE(manager_->device()->faultLatched());

    // 报警页上要看得见"故障是什么、锁着没锁"，确认按钮此时才可用。
    ASSERT_TRUE(window_->showTab(4));
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return textOf("alarmFaultValue").contains(QStringLiteral("通信丢失")); }));
    EXPECT_TRUE(textOf("alarmLatchValue").contains(QStringLiteral("已锁存")));
    // 界面上必须把"软件确认 ≠ 物理处置"写出来，不能只靠使用者自觉。
    EXPECT_TRUE(textOf("safetyBanner").contains(QStringLiteral("物理")));

    QPushButton* confirm = find<QPushButton>("clearFaultButton");
    ASSERT_NE(confirm, nullptr);
    // 等它变可用而不是立刻断言：链路刚恢复时会话可能还在"就绪→失效→再就绪"之间
    // 晃一下（心跳漏拍的余波），按钮的可用性本来就该跟着会话走。
    ASSERT_TRUE(mchost_test::waitUntil([confirm]() { return confirm->isEnabled(); }));
    confirm->click();
    EXPECT_TRUE(textOf("clearFaultHint").contains(QStringLiteral("已发出")));

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return !manager_->device()->faultLatched() &&
               manager_->device()->deviceState() == mc::DeviceState::Ready;
    }));
    EXPECT_EQ(manager_->device()->faultCode(), mc::FaultCode::None);

    // "清掉了"必须是一趟真实的协议往返：命令行里这条要有"已确认"的结局。
    // 它比设备状态**晚一拍**：设备读到帧就改了状态，而上位机要到下一拍才读到应答 ——
    // 所以这里等的是日志，不是设备状态位。
    const auto clearFaultAcked = [this]() {
        for (const mchost::CommandRecord& record : manager_->device()->commandLog()) {
            if (record.name == "CLEAR_FAULT" && record.status == mchost::CommandStatus::Acked) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(mchost_test::waitUntil(clearFaultAcked)) << "命令行（新→旧）: " << [this]() {
        std::string dump;
        for (const mchost::CommandRecord& record : manager_->device()->commandLog()) {
            dump += record.name;
            dump += "=";
            dump += mchost::commandStatusName(record.status);
            dump += " ";
        }
        return dump;
    }();

    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return textOf("alarmLatchValue").contains(QStringLiteral("未锁存")); }));
    EXPECT_TRUE(textOf("alarmStateValue").contains(QStringLiteral("Ready")));
    // 报警流水里两条边沿都在：故障来的时候一条、人工清掉的时候一条。
    QTableWidget* table = find<QTableWidget>("alarmTable");
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(mchost_test::waitUntil([table]() { return table->rowCount() >= 2; }));
    bool sawFault = false;
    bool sawCleared = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem* item = table->item(row, 1);
        if (item == nullptr) {
            continue;
        }
        if (item->text().contains(QStringLiteral("设备故障"))) {
            sawFault = true;
        }
        if (item->text().contains(QStringLiteral("清除"))) {
            sawCleared = true;
        }
    }
    EXPECT_TRUE(sawFault);
    EXPECT_TRUE(sawCleared);
}

}  // namespace
