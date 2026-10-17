#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QCheckBox>

#include "mc/audit.hpp"
#include "mc/device_simulator.hpp"
#include "mc/interlock.hpp"
#include "mc/link_monitor.hpp"
#include "mc/log.hpp"
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
    EXPECT_EQ(tabs->count(), 12);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("设备总览"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("运动控制"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("双轴协调"));
    EXPECT_EQ(tabs->tabText(3), QStringLiteral("PID 参数"));
    EXPECT_EQ(tabs->tabText(4), QStringLiteral("工艺配方"));
    EXPECT_EQ(tabs->tabText(5), QStringLiteral("任务调度"));
    EXPECT_EQ(tabs->tabText(6), QStringLiteral("数据曲线"));
    EXPECT_EQ(tabs->tabText(7), QStringLiteral("故障报警"));
    EXPECT_EQ(tabs->tabText(8), QStringLiteral("历史数据"));
    EXPECT_EQ(tabs->tabText(9), QStringLiteral("系统设置"));
    EXPECT_EQ(tabs->tabText(10), QStringLiteral("通信调试"));
    EXPECT_EQ(tabs->tabText(11), QStringLiteral("运行日志"));

    EXPECT_FALSE(window_->showTab(-1));
    EXPECT_FALSE(window_->showTab(12));
    EXPECT_TRUE(window_->showTab(1));
    // 页名定位：别的用例靠它找页，插一页也不会指到别处去。
    EXPECT_EQ(window_->tabIndexFor(QStringLiteral("数据曲线")), 6);
    EXPECT_EQ(window_->tabIndexFor(QStringLiteral("不存在的页")), -1);
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

// 运动控制页上的启动联锁清单。
//
// 判据是"按启动被拦下时，界面当场说得出是被哪一条拦的"，而不是"按钮变灰了"：
// 灰按钮不会告诉操作员该去干什么。这一页刻意不按联锁禁用启动按钮 ——
// 门禁的权威在设备侧，让拒绝发生、并把原因说出来，信息量更大。
TEST_F(UiSmokeTest, TheMotionPageSaysWhichInterlockIsBlockingStart) {
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("运动控制"))));
    ASSERT_TRUE(connectFromUiAndWaitReady());

    QTableWidget* interlocks = find<QTableWidget>("interlockTable");
    QLabel* summary = find<QLabel>("interlockSummaryLabel");
    QPushButton* start = find<QPushButton>("startButton");
    QLabel* result = find<QLabel>("lastResultLabel");
    QTableWidget* log = find<QTableWidget>("commandLogTable");
    ASSERT_NE(interlocks, nullptr);
    ASSERT_NE(summary, nullptr);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(result, nullptr);
    ASSERT_NE(log, nullptr);

    // 清单一条不少：规则表住在上位机核心里，这一页只负责把它显示出来。
    EXPECT_EQ(interlocks->rowCount(), static_cast<int>(mc::interlockRules().size()));
    EXPECT_EQ(interlocks->columnCount(), 4);

    // 会读某一格的状态列：清单是"条件/等级/状态/现在是什么情况"四列。
    const auto stateOf = [interlocks](const QString& rule) -> QString {
        for (int row = 0; row < interlocks->rowCount(); ++row) {
            const QTableWidgetItem* name = interlocks->item(row, 0);
            const QTableWidgetItem* state = interlocks->item(row, 2);
            if (name != nullptr && state != nullptr && name->text() == rule) {
                return state->text();
            }
        }
        return QString();
    };

    // 还没选环：汇总要说"按不动启动"，按下去要说清是哪一条没满足。
    ASSERT_TRUE(mchost_test::waitUntil(
        [summary]() { return summary->text().contains(QStringLiteral("按不动启动")); }))
        << summary->text().toStdString();
    EXPECT_EQ(stateOf(QStringLiteral("已选控制模式")), QStringLiteral("没满足"));
    start->click();
    EXPECT_TRUE(result->text().contains(QStringLiteral("还没选控制模式")))
        << result->text().toStdString();
    // 拦在本地 = 命令日志里一条 START 都没有（不然就变成"设备替我们兜底"了）。
    EXPECT_FALSE(rowWith(log, "START", "已确认"));
    EXPECT_FALSE(rowWith(log, "START", "被拒绝"));

    // 选了环：清单上那一格翻成"满足"，汇总也跟着翻。
    QComboBox* mode = find<QComboBox>("modeCombo");
    QPushButton* applyMode = find<QPushButton>("applyModeButton");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(applyMode, nullptr);
    mode->setCurrentIndex(1);   // 速度环
    applyMode->click();
    ASSERT_TRUE(mchost_test::waitUntil([&stateOf]() {
        return stateOf(QStringLiteral("已选控制模式")) == QStringLiteral("满足");
    })) << "选了环，清单上那一格还没翻过来";
    EXPECT_TRUE(mchost_test::waitUntil(
        [summary]() { return summary->text().contains(QStringLiteral("都满足")); }))
        << summary->text().toStdString();

    // 拦不拦得住、理由说没说清，都要有"确实按下去了"的收尾：现在按启动该发得出去。
    start->click();
    EXPECT_TRUE(mchost_test::waitUntil([log]() { return rowWith(log, "START", "已确认"); }));
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
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("数据曲线"))));

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
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("通信调试"))));
    QSpinBox* spin = find<QSpinBox>("uplinkDropSpin");
    QPushButton* apply = find<QPushButton>("applyInjectionButton");
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_TRUE(spin->isEnabled());
    spin->setValue(1);
    apply->click();
    EXPECT_TRUE(textOf("injectionHint").contains(QStringLiteral("已应用")));

    EXPECT_TRUE(mchost_test::waitUntil(
        [this]() { return textOf("alarmValue").contains(QStringLiteral("最近")); }));

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
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("通信调试"))));
    find<QSpinBox>("downlinkDropSpin")->setValue(1);
    find<QPushButton>("applyInjectionButton")->click();

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->deviceState() == mc::DeviceState::Fault;
    }));
    EXPECT_TRUE(manager_->device()->faultLatched());
    EXPECT_EQ(manager_->device()->faultCode(), mc::FaultCode::CommunicationLoss);

    // 恢复链路。设备会自己重新握上手，但**故障不许被握手顺手洗掉** ——
    // "设备出事 → 人跑过去拔线 → 重新上电 → 界面显示一切正常"正是最危险的那条路径。
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("通信调试"))));
    find<QPushButton>("clearInjectionButton")->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->sessionReady() &&
               manager_->device()->deviceState() == mc::DeviceState::Fault;
    }));
    EXPECT_TRUE(manager_->device()->faultLatched());

    // 报警页上要看得见"故障是什么、锁着没锁"，确认按钮此时才可用。
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("故障报警"))));
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
    // 报警清单里两条边沿都在：故障来的时候一条、人工清掉的时候一条。
    // 事件在第 4 列（0 起算是 3）：0 首次时刻 / 1 等级 / 2 代码 / 3 事件 / 4 次数 / 5 状态。
    QTableWidget* table = find<QTableWidget>("alarmTable");
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(mchost_test::waitUntil([table]() { return table->rowCount() >= 2; }));
    const auto cellText = [table](int row, int column) {
        const QTableWidgetItem* item = table->item(row, column);
        return (item != nullptr) ? item->text() : QString();
    };
    bool sawFault = false;
    bool sawFaultCode = false;
    bool sawCleared = false;
    bool faultMarkedAcknowledged = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString event = cellText(row, 3);
        if (event.contains(QStringLiteral("通信丢失"))) {
            sawFault = true;
            // 代码列写成 "0x0001"：十六进制大写、前缀的小写 x 不被 toUpper 吃掉。
            // 截图、日志、文档三处必须指的是同一种写法。
            if (cellText(row, 2) == QStringLiteral("0x0001")) {
                sawFaultCode = true;
            }
            // 锁存是被**人工确认**才解开的：解开它的那一下，就是这条报警的确认动作。
            // 所以它现在必须显示成"已确认" —— 否则界面会一边让它恢复
            // 一边继续拿它拦着启动，看起来像"确认没生效"。
            if (cellText(row, 5).contains(QStringLiteral("已确认"))) {
                faultMarkedAcknowledged = true;
            }
        }
        if (event.contains(QStringLiteral("故障锁存已清除"))) {
            sawCleared = true;
        }
    }
    EXPECT_TRUE(sawFault);
    EXPECT_TRUE(sawFaultCode);
    EXPECT_TRUE(sawCleared);
    EXPECT_TRUE(faultMarkedAcknowledged);
}

// 系统设置页：换数据源这件事必须真的换掉、真的重连，而不是只改一个显示值。
TEST_F(UiSmokeTest, TheSettingsPageSwitchesTheDataSourceAndSaysWhichOneIsLive) {
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("系统设置"))));

    QComboBox* source = find<QComboBox>("sourceCombo");
    QPushButton* apply = find<QPushButton>("applySourceButton");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(apply, nullptr);
    EXPECT_EQ(source->count(), 2);   // 进程内模拟器 / 串口
    // 未连接时可选：换源必须先断开，这是刻意的（同一时刻只能有一个数据源）。
    ASSERT_TRUE(apply->isEnabled());

    source->setCurrentIndex(0);   // 进程内模拟器
    apply->click();

    EXPECT_TRUE(manager_->isConnected());
    EXPECT_TRUE(manager_->device()->isSimulated());
    EXPECT_TRUE(textOf("currentSourceValue").contains(QStringLiteral("进程内模拟器")));
    EXPECT_TRUE(textOf("settingsBanner").contains(QStringLiteral("模拟数据")));
    // 已连接时不许换源，而且要说清理由 —— 禁用一个按钮而不解释等于让人干瞪眼。
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_FALSE(apply->toolTip().isEmpty());

    // 断开后又能换了，换回串口时把错误的端口名填进去应当**明确失败**，
    // 而不是"看起来连上了"。这里用一个必然不存在的端口名。
    find<QPushButton>("settingsDisconnectButton")->click();
    EXPECT_FALSE(manager_->isConnected());
    ASSERT_TRUE(apply->isEnabled());

    source->setCurrentIndex(1);
    QComboBox* ports = find<QComboBox>("portCombo");
    ASSERT_NE(ports, nullptr);
    ports->addItem(QStringLiteral("__no_such_port__"), QStringLiteral("__no_such_port__"));
    ports->setCurrentIndex(ports->count() - 1);
    apply->click();

    EXPECT_FALSE(manager_->isConnected()) << "打不开的端口绝不能显示成已连接";
    EXPECT_NE(manager_->device()->openError(), mchost::OpenError::None);
    EXPECT_TRUE(textOf("settingsHint").contains(QStringLiteral("打开失败")))
        << "实际是：" << textOf("settingsHint").toStdString();
    EXPECT_TRUE(textOf("settingsBanner").contains(QStringLiteral("串口")));
}

// 操作员角色与审计：切成观察者之后，"使能并启动"必须被**本地**拦住并说清为什么，
// 而且这一次没做成的尝试本身要出现在审计流水里 —— 那才是这一页的用处。
TEST_F(UiSmokeTest, TheSettingsPageSwitchesTheOperatorRoleAndTheRefusalLandsInTheAuditTrail) {
    // 临时库：审计要写库，测试不该往开发者的真实库里写字。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(manager_->openHistory(dir.filePath(QStringLiteral("ui-audit.db"))))
        << manager_->history()->error().toStdString();

    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("系统设置"))));
    ASSERT_TRUE(connectFromUiAndWaitReady());

    QComboBox* roleCombo = find<QComboBox>("roleCombo");
    QPushButton* applyRole = find<QPushButton>("applyRoleButton");
    QTableWidget* audit = find<QTableWidget>("auditTable");
    ASSERT_NE(roleCombo, nullptr);
    ASSERT_NE(applyRole, nullptr);
    ASSERT_NE(audit, nullptr);
    EXPECT_EQ(roleCombo->count(), 3);   // 观察者 / 操作员 / 工程师

    // 出厂角色是工程师：能换角色，而且角色值上写着稳定标识（与库里那一列同一套词）。
    EXPECT_TRUE(textOf("operatorRoleValue").contains(QStringLiteral("工程师")));
    EXPECT_TRUE(textOf("operatorRoleValue").contains(QStringLiteral("ENGINEER")));
    EXPECT_TRUE(applyRole->isEnabled());

    // 审计表里**已经**有记录：刚才那次连接就是一条（表的列也要对上）。
    EXPECT_EQ(audit->columnCount(), 5);
    const auto hasRow = [audit](const QString& action, const QString& result) {
        for (int row = 0; row < audit->rowCount(); ++row) {
            const QTableWidgetItem* actionItem = audit->item(row, 2);
            const QTableWidgetItem* resultItem = audit->item(row, 3);
            if (actionItem != nullptr && resultItem != nullptr &&
                actionItem->text() == action && resultItem->text() == result) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(mchost_test::waitUntil([&hasRow]() {
        return hasRow(QStringLiteral("连接设备"), QStringLiteral("已执行"));
    })) << "连接这件事没进审计流水";

    // 切成观察者。用 userData 找下标而不是写死数字：枚举加了值也不会指错人。
    roleCombo->setCurrentIndex(roleCombo->findData(static_cast<int>(mc::OperatorRole::Viewer)));
    // 下拉框里选中谁，职责说明就说谁 —— 按下应用之前就该知道换成它会怎样。
    EXPECT_TRUE(textOf("roleDutyLabel").contains(QStringLiteral("观察者")));
    applyRole->click();

    EXPECT_TRUE(textOf("operatorRoleValue").contains(QStringLiteral("观察者")));
    // 换了角色，「应用角色」就不该再亮着：观察者没有「管理角色」权限。
    EXPECT_FALSE(applyRole->isEnabled());
    EXPECT_FALSE(applyRole->toolTip().isEmpty()) << "禁用而不说理由，等于让人干瞪眼";
    ASSERT_TRUE(mchost_test::waitUntil(
        [&hasRow]() { return hasRow(QStringLiteral("切换角色"), QStringLiteral("已执行")); }))
        << "换角色本身没进流水";

    // 去运动控制页按启动：被**本地**拦住，理由说清了角色与缺的权限。
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("运动控制"))));
    QPushButton* start = find<QPushButton>("startButton");
    QTableWidget* log = find<QTableWidget>("commandLogTable");
    ASSERT_NE(start, nullptr);
    ASSERT_NE(log, nullptr);
    start->click();
    EXPECT_TRUE(textOf("lastResultLabel").contains(QStringLiteral("观察者")))
        << textOf("lastResultLabel").toStdString();
    EXPECT_TRUE(textOf("lastResultLabel").contains(QStringLiteral("运动控制")))
        << textOf("lastResultLabel").toStdString();
    // 拦在本地 = 链路上一条 START 都没有（连"被拒绝"都不该有）。
    EXPECT_FALSE(rowWith(log, "START", "已确认"));
    EXPECT_FALSE(rowWith(log, "START", "被拒绝"));

    // 回到设置页：这一次没做成的尝试也进了流水。
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("系统设置"))));
    ASSERT_TRUE(mchost_test::waitUntil(
        [&hasRow]() { return hasRow(QStringLiteral("使能并启动"), QStringLiteral("未执行")); }))
        << "被拦下的启动没进流水";
    EXPECT_TRUE(textOf("auditCountLabel").contains(QStringLiteral("条")));
    EXPECT_GT(audit->rowCount(), 0);
}

// 历史页：接上、跑一段、断开，然后从库里把刚才那一段查回来。
//
// 判据是"表里真的有行"，而不是"数据库文件存在" —— 文件存在说明不了写进去过什么。
TEST_F(UiSmokeTest, TheHistoryPageReadsBackWhatWasRecordedWhileConnected) {
    // 用一个临时库：测试不该往开发者的真实历史库里写字（也不该依赖它是否可写）。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("ui-history.db"));
    ASSERT_TRUE(manager_->openHistory(dbPath)) << manager_->history()->error().toStdString();

    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(selectVelocityModeAndStart());
    ASSERT_TRUE(mchost_test::waitUntil([this]() { return manager_->telemetry().size() > 50U; }));

    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("历史数据"))));
    // 切到本页时**不必先按查询**：页面自己会用当下的计数刷新一次。
    // 少了这一步，屏幕上的统计会停在"刚连上"那一刻 —— 写着"已落盘 0 条"，
    // 而库里其实已经攒了一大段（见 TEST_REPORT 的 C42）。
    EXPECT_FALSE(textOf("historyPathValue").contains(QStringLiteral("已落盘 0 条")))
        << "实际是：" << textOf("historyPathValue").toStdString();
    find<QPushButton>("refreshSessionsButton")->click();

    QTableWidget* samples = find<QTableWidget>("historySampleTable");
    QTableWidget* commands = find<QTableWidget>("historyCommandTable");
    ASSERT_NE(samples, nullptr);
    ASSERT_NE(commands, nullptr);
    EXPECT_NE(find<QTableWidget>("historyEventTable"), nullptr);

    // 数据点是**攒批**写的（默认 256 条 / 500 ms 一批），所以查到之前必然先落了一次盘。
    ASSERT_TRUE(mchost_test::waitUntil([samples]() { return samples->rowCount() > 0; }));
    EXPECT_TRUE(textOf("historyPathValue").contains(QStringLiteral("ui-history.db")));
    EXPECT_TRUE(textOf("historyRangeLabel").contains(QStringLiteral("条样本")))
        << "实际是：" << textOf("historyRangeLabel").toStdString();
    EXPECT_TRUE(manager_->history()->samplesWritten() > 0ULL);
    // 批量落盘在界面上也是可查的：样本数远大于事务数。
    EXPECT_LT(manager_->history()->transactions(), manager_->history()->samplesWritten());

    // 命令记录也落了库：界面上的按钮确实被记下来了，而且带着结局。
    ASSERT_TRUE(mchost_test::waitUntil([commands]() { return commands->rowCount() > 0; }));
    bool sawAcked = false;
    for (int row = 0; row < commands->rowCount(); ++row) {
        const QTableWidgetItem* result = commands->item(row, 3);
        if (result != nullptr && result->text() == QStringLiteral("已确认")) {
            sawAcked = true;
        }
    }
    EXPECT_TRUE(sawAcked);

    // 报警也落库，而且**带上代码与等级**回来了。
    // 这一段刻意真的制造一次链路失效（上行全丢），而不是往库里塞一行假数据 ——
    // 塞假数据只能证明"表能显示两列字"，证明不了整条路（报警 → 落库 → 查询 → 渲染）。
    QTableWidget* alarms = find<QTableWidget>("historyAlarmTable");
    ASSERT_NE(alarms, nullptr);
    // 注入之前库里不该有"链路失效"这一条。用"某一行在不在"做判据，而不是
    // "表是空的" —— 握手期间可能已经有过别的记录，那个不该让这条用例变红。
    const auto hasRow0102 = [alarms]() {
        for (int row = 0; row < alarms->rowCount(); ++row) {
            const QTableWidgetItem* code = alarms->item(row, 2);
            if (code != nullptr && code->text() == QStringLiteral("0x0102")) {
                return true;
            }
        }
        return false;
    };
    EXPECT_FALSE(hasRow0102()) << "还没断链路，库里就有了链路失效报警";

    manager_->setUplinkDropEveryNthByte(1);
    const auto hasLinkLost = [this]() {
        for (const mc::AlarmEntry& entry : manager_->alarms().entries()) {
            if (entry.code == mc::kAlarmCodeLinkLost) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(mchost_test::waitUntil(hasLinkLost));

    find<QPushButton>("queryButton")->click();
    const auto describeAlarmTable = [alarms]() {
        std::string dump;
        for (int row = 0; row < alarms->rowCount(); ++row) {
            for (int column = 0; column < alarms->columnCount(); ++column) {
                const QTableWidgetItem* item = alarms->item(row, column);
                dump += (item != nullptr) ? item->text().toStdString() : std::string("-");
                dump += " | ";
            }
            dump += "\n";
        }
        return dump;
    };
    ASSERT_TRUE(mchost_test::waitUntil(hasRow0102))
        << "报了警却没在历史库里查到。表里现有：\n"
        << describeAlarmTable() << "\n范围标签：" << textOf("historyRangeLabel").toStdString();
    for (int row = 0; row < alarms->rowCount(); ++row) {
        const QTableWidgetItem* code = alarms->item(row, 2);
        if (code == nullptr || code->text() != QStringLiteral("0x0102")) {
            continue;
        }
        // 等级与确认状态都从库里回来了 —— 这正是 alarm 表单独存在的理由：
        // 只存一句人话的话，这两个问题的答案就永远查不出来了。
        EXPECT_EQ(alarms->item(row, 1)->text(), QStringLiteral("错误"));
        EXPECT_TRUE(alarms->item(row, 4)->text().contains(QStringLiteral("未确认")));
    }
    EXPECT_TRUE(textOf("historyRangeLabel").contains(QStringLiteral("条分级报警")));

    manager_->setUplinkDropEveryNthByte(0);
}

// 任务调度页：从界面上编一部程序 → 存库 → 执行 → 每一步都拿到"已确认"。
//
// 判据一律读控件上的字：界面上没显示出来的东西，对操作员等于不存在。
TEST_F(UiSmokeTest, TheSequencePageSavesAProgramAndRunsEveryStepToAnAcknowledgedOutcome) {
    // 临时程序库：用例既不依赖开发机上那份真实程序库是否为空，也不往里面写。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(manager_->openSequences(dir.filePath(QStringLiteral("ui-sequences.db"))))
        << manager_->sequences()->error().toStdString();

    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("任务调度"))));

    QPushButton* run = find<QPushButton>("runProgramButton");
    QPushButton* save = find<QPushButton>("programSaveButton");
    QTableWidget* steps = find<QTableWidget>("sequenceStepTable");
    QTableWidget* programs = find<QTableWidget>("sequenceProgramTable");
    QComboBox* kind = find<QComboBox>("stepKindCombo");
    QPushButton* addStep = find<QPushButton>("addStepButton");
    ASSERT_NE(run, nullptr);
    ASSERT_NE(save, nullptr);
    ASSERT_NE(steps, nullptr);
    ASSERT_NE(programs, nullptr);
    ASSERT_NE(kind, nullptr);
    ASSERT_NE(addStep, nullptr);

    // 空程序既不能执行也不能保存，而且要说清为什么 ——
    // 一个亮着的按钮配上一句"没反应"是最难查的那种。
    EXPECT_EQ(steps->rowCount(), 0);
    EXPECT_FALSE(run->isEnabled());
    EXPECT_FALSE(save->isEnabled());
    EXPECT_TRUE(textOf("programHintLabel").contains(QStringLiteral("不能为空")))
        << textOf("programHintLabel").toStdString();

    find<QLineEdit>("programNameEdit")->setText(QStringLiteral("流程一号"));
    find<QSpinBox>("programVersionSpin")->setValue(2);
    // 填了名字之后，提示要换成下一条真正缺的东西（而不是停在原来那句上）。
    EXPECT_TRUE(textOf("programHintLabel").contains(QStringLiteral("至少要一步")))
        << textOf("programHintLabel").toStdString();

    // 编一部五步的程序：设模式 → 设目标 → 启动 → 等待 → 停止。
    // 只有用得上的输入才可编辑：一列全亮着的输入框会让人以为"设模式"这一步
    // 也要填 PID。
    kind->setCurrentIndex(0);   // 设置模式
    find<QComboBox>("stepModeCombo")->setCurrentIndex(1);   // 速度环
    EXPECT_FALSE(find<QDoubleSpinBox>("stepKpSpin")->isEnabled());
    addStep->click();

    kind->setCurrentIndex(2);   // 设置目标值
    find<QDoubleSpinBox>("stepTargetSpin")->setValue(200.0);
    EXPECT_FALSE(find<QDoubleSpinBox>("stepKpSpin")->isEnabled());
    addStep->click();

    kind->setCurrentIndex(1);   // 设置 PID
    EXPECT_TRUE(find<QDoubleSpinBox>("stepKpSpin")->isEnabled())
        << "切到设置 PID 之后增益输入该亮起来";
    addStep->click();

    kind->setCurrentIndex(3);   // 启动
    addStep->click();

    kind->setCurrentIndex(5);   // 等待
    find<QSpinBox>("stepWaitSpin")->setValue(300);
    EXPECT_TRUE(find<QSpinBox>("stepWaitSpin")->isEnabled());
    addStep->click();

    kind->setCurrentIndex(4);   // 停止
    addStep->click();
    EXPECT_EQ(steps->rowCount(), 6);

    // 上移/下移要真的换顺序：顺序就是这部程序的全部意义。
    steps->selectRow(5);
    find<QPushButton>("stepUpButton")->click();
    EXPECT_EQ(steps->item(4, 1)->text(), QStringLiteral("停止"));
    find<QPushButton>("stepDownButton")->click();
    EXPECT_EQ(steps->item(4, 1)->text(), QStringLiteral("等待"));
    EXPECT_EQ(steps->item(5, 1)->text(), QStringLiteral("停止"));
    // 删掉"设置 PID"那一步（第 3 步），回到五步。
    steps->selectRow(2);
    find<QPushButton>("stepRemoveButton")->click();
    EXPECT_EQ(steps->rowCount(), 5);
    EXPECT_EQ(steps->item(2, 1)->text(), QStringLiteral("启动"));

    EXPECT_TRUE(run->isEnabled()) << textOf("programHintLabel").toStdString();
    EXPECT_TRUE(save->isEnabled()) << textOf("programHintLabel").toStdString();

    // 存库：列表里要出现这一部，而且校验和是对的（状态列写"可用"）。
    save->click();
    EXPECT_EQ(programs->rowCount(), 1);
    // 保存结果单独一个标签：校验提示是按编辑状态随时重算的，
    // 两者挤在一个标签里的话，"已保存"会在刷新列表那一瞬间被覆盖掉。
    EXPECT_TRUE(textOf("sequenceResultLabel").contains(QStringLiteral("已保存")))
        << textOf("sequenceResultLabel").toStdString();
    EXPECT_EQ(programs->item(0, 0)->text(), QStringLiteral("流程一号"));
    EXPECT_EQ(programs->item(0, 2)->text(), QStringLiteral("5"));
    EXPECT_EQ(programs->item(0, 3)->text(), QStringLiteral("可用"));

    // 执行：先连上（设备停在 Ready 才允许跑程序）。
    ASSERT_TRUE(connectFromUiAndWaitReady());
    run->click();

    const auto phaseText = [this]() {
        return textOf("runStatusLabel");
    };
    ASSERT_TRUE(mchost_test::waitUntil([&phaseText]() {
        return phaseText().contains(QStringLiteral("全部完成"));
    })) << "实际状态：" << phaseText().toStdString()
        << " 详情：" << textOf("runDetailLabel").toStdString();

    QTableWidget* report = find<QTableWidget>("runReportTable");
    ASSERT_NE(report, nullptr);
    ASSERT_EQ(report->rowCount(), 5);
    for (int row = 0; row < report->rowCount(); ++row) {
        EXPECT_EQ(report->item(row, 2)->text(), QStringLiteral("已确认"))
            << "第 " << row + 1 << " 步的结局不是已确认";
    }
    // 结束时"中止"必须点不动、并且不再有什么在跑。
    EXPECT_FALSE(find<QPushButton>("abortProgramButton")->isEnabled());
    EXPECT_TRUE(textOf("sequenceBanner").contains(QStringLiteral("物理")));
}

// 配方页：新建 → 存库 → 下装，整条路都从界面上走。
//
// 判据一律读控件上的字：界面上没显示出来的东西，对操作员等于不存在。
TEST_F(UiSmokeTest, TheRecipePageSavesDownloadsAndReportsEveryStep) {
    // 临时配方库：用例既不依赖开发机上那份真实配方库是否为空，也不往里面写。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(manager_->openRecipes(dir.filePath(QStringLiteral("ui-recipes.db"))))
        << manager_->recipes()->error().toStdString();

    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("工艺配方"))));

    QPushButton* download = find<QPushButton>("recipeDownloadButton");
    QTableWidget* table = find<QTableWidget>("recipeTable");
    ASSERT_NE(download, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 0);

    // 一上来是一份"还没填完"的配方：校验面板要说清缺什么，下装必须点不动。
    // 给一个像模像样的默认值反而更容易让人不去核对它。
    EXPECT_TRUE(textOf("recipeValidationLabel").contains(QStringLiteral("名称不能为空")))
        << textOf("recipeValidationLabel").toStdString();
    EXPECT_FALSE(download->isEnabled());

    find<QLineEdit>("recipeNameEdit")->setText(QStringLiteral("WAFER-A1"));
    find<QSpinBox>("recipeVersionSpin")->setValue(3);
    find<QComboBox>("recipeModeCombo")->setCurrentIndex(1);   // 速度环
    find<QDoubleSpinBox>("recipeTargetSpin")->setValue(200.0);
    find<QDoubleSpinBox>("recipeKpSpin")->setValue(1.6);
    find<QDoubleSpinBox>("recipeKiSpin")->setValue(0.4);
    find<QDoubleSpinBox>("recipeKdSpin")->setValue(0.02);
    EXPECT_TRUE(textOf("recipeValidationLabel").contains(QStringLiteral("校验通过")))
        << textOf("recipeValidationLabel").toStdString();

    find<QPushButton>("recipeSaveButton")->click();
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 0)->text(), QStringLiteral("WAFER-A1"));
    EXPECT_EQ(table->item(0, 2)->text(), QStringLiteral("1 速度环"));
    EXPECT_EQ(table->item(0, 8)->text(), QStringLiteral("正常"));
    EXPECT_TRUE(textOf("recipeStoreValue").contains(QStringLiteral("1 条配方")))
        << textOf("recipeStoreValue").toStdString();

    // 同名同版本再存一次要被拒，而且理由要显示出来 —— 拒绝路径不能只存在于代码里。
    find<QPushButton>("recipeNewButton")->click();
    find<QLineEdit>("recipeNameEdit")->setText(QStringLiteral("WAFER-A1"));
    find<QSpinBox>("recipeVersionSpin")->setValue(3);
    find<QComboBox>("recipeModeCombo")->setCurrentIndex(1);
    find<QDoubleSpinBox>("recipeTargetSpin")->setValue(200.0);
    find<QPushButton>("recipeSaveButton")->click();
    EXPECT_TRUE(textOf("recipeResultLabel").contains(QStringLiteral("已存在")))
        << textOf("recipeResultLabel").toStdString();
    EXPECT_EQ(table->rowCount(), 1) << "被拒的那一份不该落库";

    // 选中清单里那一行 → 编辑器被载入（这就是"配方库"的意义：关掉窗口也还在）。
    table->setCurrentCell(0, 0);
    EXPECT_EQ(find<QLineEdit>("recipeNameEdit")->text(), QStringLiteral("WAFER-A1"));
    EXPECT_DOUBLE_EQ(find<QDoubleSpinBox>("recipeTargetSpin")->value(), 200.0);

    // 下装：三条命令 + 一次回读核对，每一步的结果都要在界面上看得见。
    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(mchost_test::waitUntil([download]() { return download->isEnabled(); }));
    download->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->recipeDownload().phase == mchost::RecipeDownloadPhase::Succeeded;
    })) << manager_->recipeDownload().detail.toStdString();

    QTableWidget* steps = find<QTableWidget>("recipeDownloadTable");
    ASSERT_NE(steps, nullptr);
    ASSERT_EQ(steps->rowCount(), 4);
    for (int row = 0; row < 3; ++row) {
        EXPECT_EQ(steps->item(row, 2)->text(), QStringLiteral("已确认")) << "第 " << row << " 行";
    }
    EXPECT_TRUE(steps->item(3, 2)->text().contains(QStringLiteral("一致")))
        << steps->item(3, 2)->text().toStdString();
    EXPECT_TRUE(textOf("recipeDownloadLabel").contains(QStringLiteral("下装成功")))
        << textOf("recipeDownloadLabel").toStdString();

    // 设备侧真的收到了那个目标值 —— 不是界面自己写了个"成功"。
    EXPECT_FLOAT_EQ(manager_->device()->deviceTarget(), 200.0F);
}

// 回零按钮：点下去要**真的**走协议，而且结果那一行说的是设备报的那一位，
// 而不是"命令发出去了所以成功"。
TEST_F(UiSmokeTest, TheMotionPageHomesAndReportsTheOriginFromTheDeviceBit) {
    QPushButton* home = find<QPushButton>("homeButton");
    QDoubleSpinBox* speed = find<QDoubleSpinBox>("homeSpeedSpin");
    QDoubleSpinBox* timeout = find<QDoubleSpinBox>("homeTimeoutSpin");
    ASSERT_NE(home, nullptr);
    ASSERT_NE(speed, nullptr);
    ASSERT_NE(timeout, nullptr);

    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(selectVelocityModeAndStart());

    // 先开出去一段，否则"原点就在脚下"，回零没有可观察的行程。
    QDoubleSpinBox* target = find<QDoubleSpinBox>("targetSpin");
    QPushButton* applyTarget = find<QPushButton>("applyTargetButton");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(applyTarget, nullptr);
    target->setValue(200.0);
    applyTarget->click();
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return std::fabs(manager_->device()->positionDeg()) > 15.0F; }));
    QPushButton* stop = find<QPushButton>("stopButton");
    ASSERT_NE(stop, nullptr);
    stop->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->deviceState() == mc::DeviceState::Ready;
    }));

    speed->setValue(60.0);
    timeout->setValue(5.0);
    ASSERT_TRUE(mchost_test::waitUntil([home]() { return home->isEnabled(); }));
    home->click();

    // 一点下去**不能**说成功：应答只说明回零开始了。
    EXPECT_TRUE(textOf("homeStatusLabel").contains(QStringLiteral("回零进行中")))
        << textOf("homeStatusLabel").toStdString();

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("homeStatusLabel").contains(QStringLiteral("零点已经建立"));
    })) << textOf("homeStatusLabel").toStdString();

    // 判据的另一半：设备侧的坐标确实被搬到了 0，而且 HOME 真的上过链路。
    EXPECT_NEAR(manager_->device()->positionDeg(), 0.0F, 1.0F);
    EXPECT_TRUE(rowWith(find<QTableWidget>("commandLogTable"), "HOME", QStringLiteral("已确认")));
}

// "这一轮回零没有走完"必须与"成功了"在界面上分得开 —— 两者在曲线上都是"停下来了"。
TEST_F(UiSmokeTest, TheMotionPageReportsAStoppedHomeAsNotEstablished) {
    QPushButton* home = find<QPushButton>("homeButton");
    QDoubleSpinBox* speed = find<QDoubleSpinBox>("homeSpeedSpin");
    QDoubleSpinBox* timeout = find<QDoubleSpinBox>("homeTimeoutSpin");
    ASSERT_NE(home, nullptr);

    ASSERT_TRUE(connectFromUiAndWaitReady());
    ASSERT_TRUE(selectVelocityModeAndStart());

    QDoubleSpinBox* target = find<QDoubleSpinBox>("targetSpin");
    QPushButton* applyTarget = find<QPushButton>("applyTargetButton");
    target->setValue(200.0);
    applyTarget->click();
    ASSERT_TRUE(mchost_test::waitUntil(
        [this]() { return std::fabs(manager_->device()->positionDeg()) > 25.0F; }));
    QPushButton* stop = find<QPushButton>("stopButton");
    stop->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->deviceState() == mc::DeviceState::Ready;
    }));

    // 慢速回零，留出"半路被停住"的窗口。
    speed->setValue(5.0);
    timeout->setValue(30.0);
    home->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return manager_->device()->deviceState() == mc::DeviceState::Home;
    }));
    stop->click();

    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("homeStatusLabel").contains(QStringLiteral("没有"));
    })) << textOf("homeStatusLabel").toStdString();
    // 与成功那一格分清：这句话里不能出现"零点已经建立"。
    EXPECT_FALSE(textOf("homeStatusLabel").contains(QStringLiteral("零点已经建立")));
    // 坐标没有被搬走 —— "没有建立零点"的证据。
    EXPECT_GT(std::fabs(manager_->device()->positionDeg()), 1.0F);
}

// 软限位：下发要真的落到设备上，回读要读**设备报回来的**那一组。
TEST_F(UiSmokeTest, TheParamPageDownloadsSoftLimitsAndReadsThemBack) {
    QCheckBox* enabled = find<QCheckBox>("limitsEnabledCheck");
    QDoubleSpinBox* min = find<QDoubleSpinBox>("limitsMinSpin");
    QDoubleSpinBox* max = find<QDoubleSpinBox>("limitsMaxSpin");
    QPushButton* apply = find<QPushButton>("applyLimitsButton");
    QPushButton* readback = find<QPushButton>("readbackButton");
    ASSERT_NE(enabled, nullptr);
    ASSERT_NE(min, nullptr);
    ASSERT_NE(max, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(readback, nullptr);

    ASSERT_TRUE(connectFromUiAndWaitReady());
    // 回读之前"设备存了什么"是不知道的，界面必须这么说，而不是拿输入框里的数字充数。
    EXPECT_TRUE(textOf("readbackLabel").contains(QStringLiteral("还没回读过")))
        << textOf("readbackLabel").toStdString();

    ASSERT_TRUE(mchost_test::waitUntil([apply]() { return apply->isEnabled(); }));
    enabled->setChecked(true);
    min->setValue(-120.0);
    max->setValue(120.0);
    apply->click();
    EXPECT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("limitsResultLabel").contains(QStringLiteral("已下发"));
    })) << textOf("limitsResultLabel").toStdString();

    // 判据不是"界面说自己发出去了"，而是**回读**：那一组必须能从设备问回来。
    // 设备才是执法的那一端，界面显示的数字若与它存的不一致，这条会红。
    ASSERT_TRUE(mchost_test::waitUntil([readback]() { return readback->isEnabled(); }));
    readback->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("readbackLabel").contains(QStringLiteral("启用"));
    })) << textOf("readbackLabel").toStdString();
    EXPECT_TRUE(textOf("readbackLabel").contains(QStringLiteral("-120")))
        << textOf("readbackLabel").toStdString();
    EXPECT_TRUE(textOf("readbackLabel").contains(QStringLiteral("120")))
        << textOf("readbackLabel").toStdString();
}

// 任务页的 HOME 步：类型下拉里要有它，而且**两个字段的含义是换过的**
// （target 是速度、waitMs 是时限）—— 标签必须跟着换，否则填进去的数看着正常、含义全错。
TEST_F(UiSmokeTest, TheSequencePageOffersAHomeStepAndRequiresASettleStep) {
    QComboBox* kind = find<QComboBox>("stepKindCombo");
    QPushButton* add = find<QPushButton>("addStepButton");
    QDoubleSpinBox* target = find<QDoubleSpinBox>("stepTargetSpin");
    QSpinBox* wait = find<QSpinBox>("stepWaitSpin");
    QTableWidget* steps = find<QTableWidget>("sequenceStepTable");
    ASSERT_NE(kind, nullptr);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(wait, nullptr);
    ASSERT_NE(steps, nullptr);

    const int homeIndex = kind->findText(QStringLiteral("回零"));
    ASSERT_GE(homeIndex, 0) << "任务页的类型下拉里没有「回零」";
    kind->setCurrentIndex(homeIndex);

    QLineEdit* name = find<QLineEdit>("programNameEdit");
    ASSERT_NE(name, nullptr);
    name->setText(QStringLiteral("UI-HOME"));

    target->setValue(60.0);
    wait->setValue(5000);
    add->click();
    ASSERT_EQ(steps->rowCount(), 1);
    EXPECT_TRUE(steps->item(0, 1)->text().contains(QStringLiteral("回零")));
    // 参数列要写清两个字段各自的含义（"以 60.0 deg/s 朝零点走，最多 5.0 s"）。
    EXPECT_TRUE(steps->item(0, 2)->text().contains(QStringLiteral("deg/s")))
        << steps->item(0, 2)->text().toStdString();
    EXPECT_TRUE(steps->item(0, 2)->text().contains(QStringLiteral("朝零点走")))
        << steps->item(0, 2)->text().toStdString();

    // 只加一个回零步：**不合法**，并且理由要说清为什么要跟一步"等设备就绪"。
    EXPECT_TRUE(textOf("programHintLabel").contains(QStringLiteral("等设备就绪")))
        << textOf("programHintLabel").toStdString();

    // 补上那一步之后就合法了 —— 反向对照，说明拦下它的正是那条规则。
    const int settleIndex = kind->findText(QStringLiteral("等设备就绪"));
    ASSERT_GE(settleIndex, 0);
    kind->setCurrentIndex(settleIndex);
    wait->setValue(10000);
    add->click();
    ASSERT_EQ(steps->rowCount(), 2);
    EXPECT_TRUE(textOf("programHintLabel").contains(QStringLiteral("合法")))
        << textOf("programHintLabel").toStdString();
}

// 双轴协调页：从界面上连两轴、按协调启动，两轴都拿到命令、都走完。
//
// 判据一律读控件：界面没显示出来的东西，对操作员等于不存在。
TEST_F(UiSmokeTest, TheAxisPageConnectsBothAxesAndCompletesASynchronizedMove) {
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("双轴协调"))));

    QPushButton* connect = find<QPushButton>("axisConnectButton");
    QPushButton* move = find<QPushButton>("axisMoveButton");
    QPushButton* stop = find<QPushButton>("axisStopButton");
    QDoubleSpinBox* targetX = find<QDoubleSpinBox>("axisTargetXSpin");
    QDoubleSpinBox* targetY = find<QDoubleSpinBox>("axisTargetYSpin");
    QDoubleSpinBox* speed = find<QDoubleSpinBox>("axisSpeedSpin");
    QTableWidget* table = find<QTableWidget>("axisTable");
    ASSERT_NE(connect, nullptr);
    ASSERT_NE(move, nullptr);
    ASSERT_NE(stop, nullptr);
    ASSERT_NE(targetX, nullptr);
    ASSERT_NE(targetY, nullptr);
    ASSERT_NE(speed, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 2);

    // 还没连：开不动，而且按钮是灰的 —— 但理由也要写在标签上（灰按钮不解释等于让人干瞪眼）。
    EXPECT_FALSE(move->isEnabled());
    EXPECT_TRUE(textOf("axisConnectLabel").contains(QStringLiteral("未连接")))
        << textOf("axisConnectLabel").toStdString();

    connect->click();
    // 两轴各自握手：连上按钮变灰 = 两轴都真的就绪了。
    ASSERT_TRUE(mchost_test::waitUntil([connect]() { return !connect->isEnabled(); }, 8000))
        << textOf("axisConnectLabel").toStdString();
    EXPECT_TRUE(textOf("axisConnectLabel").contains(QStringLiteral("就绪")))
        << textOf("axisConnectLabel").toStdString();

    // 预览：按下之前就能看出会怎么走（公共时长 + 各轴速度）。
    targetX->setValue(20.0);
    targetY->setValue(6.0);
    speed->setValue(100.0);
    EXPECT_TRUE(textOf("axisPlanLabel").contains(QStringLiteral("公共时长")))
        << textOf("axisPlanLabel").toStdString();

    ASSERT_TRUE(mchost_test::waitUntil([move]() { return move->isEnabled(); }));
    move->click();
    ASSERT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("axisStatusLabel").contains(QStringLiteral("按计划走完"));
    })) << textOf("axisStatusLabel").toStdString();

    // 两行的"这一步的结局"都必须是已确认 —— 界面上看得见的那一份证据。
    for (int row = 0; row < table->rowCount(); ++row) {
        ASSERT_NE(table->item(row, 4), nullptr);
        EXPECT_EQ(table->item(row, 4)->text(), QStringLiteral("已确认")) << "第 " << row + 1 << " 行";
    }
    // 安全闸这一栏也要在：它没触发时说的是"待命"，不是空白。
    EXPECT_TRUE(textOf("axisSafetyLabel").contains(QStringLiteral("待命")))
        << textOf("axisSafetyLabel").toStdString();

    // 「全组停止」按钮：按下去必须真的让状态栏改口。按钮没接到编排上，这一栏
    // 会永远停在"按计划走完" —— 那正是"假按钮"。判据读控件，不读内部成员。
    //
    // ⚠️ 谓词不能用"已发出全组停止"：**走完那一刻的文案里也有这五个字**
    // （"…按计划走完…，已发出全组停止"），拿它当判据等于按下之前就已经成立。
    // 这里钉的是**只有人工停止那条路才会说的话**（"停不等于撤销"）。
    stop->click();
    EXPECT_TRUE(mchost_test::waitUntil([this]() {
        return textOf("axisStatusLabel").contains(QStringLiteral("行程收不回来"));
    })) << textOf("axisStatusLabel").toStdString();
    EXPECT_TRUE(textOf("axisStatusLabel").contains(QStringLiteral("阶段：已停止")))
        << textOf("axisStatusLabel").toStdString();
}

// 运行日志页：挂上日志器 → 记录进表；级别过滤在**写入那一刻**生效。
//
// 过滤是"入口过滤"，不是"显示时过滤"：被级别挡掉的记录根本不进环形，
// 所以环形里留下的就是当时该留的那些（改级别不会追溯删掉旧记录）。
// 顺带验一份真的 RotatingFileWriter 在页面上是看得见的。
TEST_F(UiSmokeTest, TheLogPageFiltersByLevelAndReportsItsFile) {
    ASSERT_TRUE(window_->showTab(window_->tabIndexFor(QStringLiteral("运行日志"))));

    QTableWidget* table = find<QTableWidget>("logTable");
    QComboBox* combo = find<QComboBox>("logLevelCombo");
    ASSERT_NE(table, nullptr);
    ASSERT_NE(combo, nullptr);

    // 没有挂日志器：页面要说"未启用"，而不是摆一张永远空着的表。
    EXPECT_TRUE(textOf("logHintLabel").contains(QStringLiteral("未启用")))
        << textOf("logHintLabel").toStdString();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    mc::RotatingFileWriter writer;
    ASSERT_TRUE(writer.open(dir.filePath(QStringLiteral("ui.log")).toStdString(), 1U << 16, 3U))
        << writer.lastError();
    mc::Logger logger(64U);
    logger.setMinLevel(mc::LogLevel::Warn);
    logger.attachFile(&writer);
    window_->attachLogger(&logger);

    // 挂上之后，下拉框显示的是**当前生效**的那一级（不是"我上次点了什么"）。
    EXPECT_EQ(combo->currentIndex(), static_cast<int>(mc::LogLevel::Warn));

    logger.info("cmd", "启动命令已发出");   // 低于 Warn：挡掉
    logger.warn("link", "链路失效");         // 留下
    logger.debug("cmd", "也不该出现");       // 挡掉
    logger.error("alarm", "故障锁存");       // 留下

    find<QPushButton>("logRefreshButton")->click();
    EXPECT_EQ(table->rowCount(), 2) << textOf("logCounterLabel").toStdString();
    EXPECT_TRUE(textOf("logCounterLabel").contains(QStringLiteral("被级别挡掉 2 条")))
        << textOf("logCounterLabel").toStdString();

    bool sawWarn = false;
    bool sawError = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        ASSERT_NE(table->item(row, 1), nullptr);
        const QString level = table->item(row, 1)->text();
        sawWarn = sawWarn || (level == QStringLiteral("WARN"));
        sawError = sawError || (level == QStringLiteral("ERROR"));
    }
    EXPECT_TRUE(sawWarn);
    EXPECT_TRUE(sawError);

    // 文件那一行：路径、离轮转还有多远、保留几份 —— 都要看得见。
    EXPECT_TRUE(textOf("logFileLabel").contains(QStringLiteral("ui.log")))
        << textOf("logFileLabel").toStdString();
    EXPECT_TRUE(textOf("logFileLabel").contains(QStringLiteral("保留 3 份")))
        << textOf("logFileLabel").toStdString();

    // 调到调试档：此后 Info 也记（改级别这一下自己会记一条 Info "记录级别改为…"）。
    combo->setCurrentIndex(static_cast<int>(mc::LogLevel::Debug));
    EXPECT_EQ(logger.minLevel(), mc::LogLevel::Debug);
    EXPECT_TRUE(textOf("logCounterLabel").contains(QStringLiteral("被级别挡掉")))
        << textOf("logCounterLabel").toStdString();

    // 「清空视图」按钮：只清"现在还剩几条"，**不抹掉"一共丢过多少"** ——
    // 后者是证据，点一下按钮就把它抹掉正是要避免的（核心侧
    // `LogRing.ClearingKeepsTheDroppedAccount` 守的是同一条）。换一个容量只有 2 的
    // 日志器，写 5 条好让"丢弃计数"非零，这样"没被抹掉"才判得出来。
    mc::Logger tiny(2U);
    tiny.setMinLevel(mc::LogLevel::Warn);
    window_->attachLogger(&tiny);
    for (int i = 0; i < 5; ++i) {
        tiny.warn("t", "第 " + std::to_string(i) + " 条");
    }
    find<QPushButton>("logRefreshButton")->click();
    EXPECT_EQ(table->rowCount(), 2) << "容量 2 只留最近两条";
    ASSERT_TRUE(textOf("logCounterLabel").contains(QStringLiteral("环形丢弃 3 条")))
        << textOf("logCounterLabel").toStdString();

    find<QPushButton>("logClearButton")->click();
    EXPECT_EQ(table->rowCount(), 0) << "清屏之后表里不该还有行";
    EXPECT_TRUE(textOf("logCounterLabel").contains(QStringLiteral("环形丢弃 3 条")))
        << "清屏不该把丢弃计数一起抹掉：" << textOf("logCounterLabel").toStdString();
}

}  // namespace
