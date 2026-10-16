#include "mc_host/ui/overview_page.hpp"

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "mc/device_state.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {

OverviewPage::OverviewPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    // 「模拟数据」横幅不是装饰 —— 任务书阶段八明确要求界面必须能显著区分
    // 模拟数据与真实数据。含糊其辞地显示一串数字，比不显示更坏。
    simBanner_ = new QLabel(this);
    simBanner_->setObjectName(QStringLiteral("simBanner"));
    simBanner_->setWordWrap(true);
    simBanner_->setAlignment(Qt::AlignCenter);
    QFont bannerFont = simBanner_->font();
    bannerFont.setBold(true);
    bannerFont.setPointSize(bannerFont.pointSize() + 1);
    simBanner_->setFont(bannerFont);
    simBanner_->setStyleSheet(
        QStringLiteral("background:#8a5a00; color:#fff6e0; padding:7px; border-radius:4px;"));
    root->addWidget(simBanner_);

    auto* sessionGroup = new QGroupBox(QStringLiteral("会话（上位机侧）"), this);
    auto* sessionGrid = new QGridLayout(sessionGroup);
    sessionValue_ = addRow(sessionGrid, 0, QStringLiteral("会话状态"));
    handshakeValue_ = addRow(sessionGrid, 1, QStringLiteral("握手轮次"));
    root->addWidget(sessionGroup);

    auto* deviceGroup = new QGroupBox(QStringLiteral("设备"), this);
    auto* deviceGrid = new QGridLayout(deviceGroup);
    deviceStateValue_ = addRow(deviceGrid, 0, QStringLiteral("设备状态"));
    faultValue_ = addRow(deviceGrid, 1, QStringLiteral("故障"));
    motionValue_ = addRow(deviceGrid, 2, QStringLiteral("运动"));
    root->addWidget(deviceGroup);

    auto* linkGroup = new QGroupBox(QStringLiteral("链路"), this);
    auto* linkGrid = new QGridLayout(linkGroup);
    linkValue_ = addRow(linkGrid, 0, QStringLiteral("链路判定"));
    rttValue_ = addRow(linkGrid, 1, QStringLiteral("实测 RTT(P95)"));
    windowValue_ = addRow(linkGrid, 2, QStringLiteral("生效超时窗口"));
    heartbeatValue_ = addRow(linkGrid, 3, QStringLiteral("心跳"));
    root->addWidget(linkGroup);

    auto* statsGroup = new QGroupBox(QStringLiteral("统计"), this);
    auto* statsGrid = new QGridLayout(statsGroup);
    framesValue_ = addRow(statsGrid, 0, QStringLiteral("发送 / 应答 / NAK"));
    telemetryValue_ = addRow(statsGrid, 1, QStringLiteral("遥测接收 / 拒绝"));
    decodeValue_ = addRow(statsGrid, 2, QStringLiteral("解码拒绝（CRC/长度/版本/重同步）"));
    timeoutValue_ = addRow(statsGrid, 3, QStringLiteral("超时（Fault / 重发 / 心跳漏拍）"));
    internalValue_ = addRow(statsGrid, 4, QStringLiteral("内部不变式（编码/发送失败）"));
    alarmValue_ = addRow(statsGrid, 5, QStringLiteral("最近报警"));
    root->addWidget(statsGroup);

    // 给取值标签统一起名字：界面自检要用 findChild 按名字找到它们并读文本。
    // "控件到底有没有变"只能通过读控件来断言 —— 去读内部成员就不叫界面测试了。
    sessionValue_->setObjectName(QStringLiteral("sessionValue"));
    handshakeValue_->setObjectName(QStringLiteral("handshakeValue"));
    deviceStateValue_->setObjectName(QStringLiteral("deviceStateValue"));
    faultValue_->setObjectName(QStringLiteral("faultValue"));
    motionValue_->setObjectName(QStringLiteral("motionValue"));
    linkValue_->setObjectName(QStringLiteral("linkValue"));
    rttValue_->setObjectName(QStringLiteral("rttValue"));
    windowValue_->setObjectName(QStringLiteral("windowValue"));
    heartbeatValue_->setObjectName(QStringLiteral("heartbeatValue"));
    framesValue_->setObjectName(QStringLiteral("framesValue"));
    telemetryValue_->setObjectName(QStringLiteral("telemetryValue"));
    decodeValue_->setObjectName(QStringLiteral("decodeValue"));
    timeoutValue_->setObjectName(QStringLiteral("timeoutValue"));
    internalValue_->setObjectName(QStringLiteral("internalValue"));
    alarmValue_->setObjectName(QStringLiteral("alarmValue"));

    auto* buttons = new QGridLayout();
    connectButton_ = new QPushButton(QStringLiteral("连接设备"), this);
    connectButton_->setObjectName(QStringLiteral("connectButton"));
    disconnectButton_ = new QPushButton(QStringLiteral("断开"), this);
    disconnectButton_->setObjectName(QStringLiteral("disconnectButton"));
    buttons->addWidget(connectButton_, 0, 0);
    buttons->addWidget(disconnectButton_, 0, 1);
    root->addLayout(buttons);

    // 故障注入按钮**不在这页**：它统一收在「通信调试」页。
    // 注入状态只能有一个真源 —— 两处按钮改同一个状态，迟早会出现"界面显示断着、
    // 实际没断"（或反过来），而这会让人得出完全反向的结论。
    auto* injectPointer = new QLabel(
        QStringLiteral("故障注入（断开上行 / 下行、上行延迟）在「通信调试」页 —— "
                       "那是唯一设置处，本页的统计会跟着它变。"),
        this);
    injectPointer->setWordWrap(true);
    injectPointer->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(injectPointer);
    root->addStretch(1);

    connect(connectButton_, &QPushButton::clicked, &manager_, &DeviceManager::connectDevice);
    connect(disconnectButton_, &QPushButton::clicked, &manager_,
            &DeviceManager::disconnectDevice);

    connect(&manager_, &DeviceManager::connectionChanged, this,
            &OverviewPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &OverviewPage::refresh);
    connect(&manager_, &DeviceManager::commandLogChanged, this, &OverviewPage::refresh);
    connect(&manager_, &DeviceManager::alarmRaised, this, [this](const QString&) { refresh(); });

    onConnectionChanged(manager_.isConnected());
    refresh();
}

QLabel* OverviewPage::addRow(QGridLayout* grid, int row, const QString& title) {
    auto* titleLabel = new QLabel(title + QStringLiteral("："), this);
    auto* valueLabel = new QLabel(QStringLiteral("—"), this);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(titleLabel, row, 0, Qt::AlignRight | Qt::AlignTop);
    grid->addWidget(valueLabel, row, 1);
    grid->setColumnStretch(1, 1);
    return valueLabel;
}

void OverviewPage::onConnectionChanged(bool connected) {
    connectButton_->setEnabled(!connected);
    disconnectButton_->setEnabled(connected);
    refresh();
}

void OverviewPage::refresh() {
    const IDevice* device = manager_.device();

    if (device == nullptr) {
        simBanner_->setText(QStringLiteral("未接入任何设备"));
        return;
    }

    simBanner_->setText(
        device->isSimulated()
            ? QStringLiteral("★ 模拟数据 —— 当前由「%1」驱动，不是真实设备，也不代表任何硬件指标")
                  .arg(QString::fromUtf8(device->kindName()))
            : QStringLiteral("■ 真实设备 —— 数据来自「%1」").arg(QString::fromUtf8(device->kindName())));

    refreshDeviceSection();
    refreshLinkSection();
    refreshStatsSection();
}

void OverviewPage::refreshDeviceSection() {
    const IDevice* device = manager_.device();
    const bool ready = device->sessionReady();

    if (!manager_.isConnected()) {
        sessionValue_->setText(QStringLiteral("未连接"));
        handshakeValue_->setText(QStringLiteral("—"));
    } else if (ready) {
        sessionValue_->setText(QStringLiteral("就绪（已握手，命令可下发）"));
        handshakeValue_->setText(QStringLiteral("共 %1 轮").arg(device->handshakeRounds()));
    } else {
        // 未就绪时必须说清"在干什么"，否则用户看到的就是"连上了但什么都没反应"。
        sessionValue_->setText(QStringLiteral("未就绪（正在握手 / 等待链路恢复）"));
        handshakeValue_->setText(QStringLiteral("已开 %1 轮").arg(device->handshakeRounds()));
    }

    deviceStateValue_->setText(QString::fromUtf8(mc::deviceStateName(device->deviceState())));
    faultValue_->setText(
        device->faultLatched()
            ? QStringLiteral("%1（已锁存，须人工确认才能清除）")
                  .arg(QString::fromUtf8(mc::faultCodeName(device->faultCode())))
            : QStringLiteral("无"));

    const char* modeName = "停止";
    if (device->runMode() == mc::SimRunMode::Velocity) {
        modeName = "速度环";
    } else if (device->runMode() == mc::SimRunMode::Position) {
        modeName = "位置环";
    }
    motionValue_->setText(
        QStringLiteral("模式 %1 | 输出 %2 | 速度 %3 deg/s | 位置 %4 deg | 目标 %5")
            .arg(QString::fromUtf8(modeName))
            .arg(device->enabled() ? QStringLiteral("使能中") : QStringLiteral("未使能"))
            .arg(static_cast<double>(device->velocityDegPerSec()), 0, 'f', 1)
            .arg(static_cast<double>(device->positionDeg()), 0, 'f', 1)
            .arg(static_cast<double>(device->deviceTarget()), 0, 'f', 1));
}

void OverviewPage::refreshLinkSection() {
    const IDevice* device = manager_.device();

    if (!manager_.isConnected()) {
        linkValue_->setText(QStringLiteral("未连接"));
    } else if (device->linkDown()) {
        linkValue_->setText(QStringLiteral("判失效（连续丢拍）"));
    } else {
        // 链路监控没启用时 linkDown 恒为 false —— 含义是"没有在监控"，不是"链路正常"。
        // 界面必须把这个差别说出来，否则"绿灯"会被读成一种保证。
        linkValue_->setText(device->linkStats() != nullptr ? QStringLiteral("正常（监控中）")
                                                          : QStringLiteral("未监控"));
    }

    rttValue_->setText(device->hasRttSample()
                           ? QStringLiteral("%1 ms").arg(device->rttP95Ms())
                           : QStringLiteral("无样本（尚未量到往返）"));
    windowValue_->setText(
        QStringLiteral("%1 ms").arg(device->ackTimeoutMs()));

    const mc::LinkMonitor::Stats* stats = device->linkStats();
    heartbeatValue_->setText(
        stats != nullptr
            ? QStringLiteral("发出 %1 / 已应答 %2 / 链路失效次数 %3")
                  .arg(stats->heartbeatsSent)
                  .arg(stats->heartbeatsAcked)
                  .arg(stats->linkDownTransitions)
            : QStringLiteral("未启用链路监控"));
}

void OverviewPage::refreshStatsSection() {
    const IDevice* device = manager_.device();
    const mc::AckStats& ack = device->ackStats();
    const mc::StreamDecoder::Stats& decode = device->decoderStats();

    framesValue_->setText(QStringLiteral("发送 %1 | 应答对上 %2 | NAK 收到 %3")
                              .arg(device->framesSent())
                              .arg(ack.completed)
                              .arg(ack.rejectedByPeer));
    telemetryValue_->setText(QStringLiteral("接收 %1 / 拒绝 %2 | 缓冲 %3 条（丢弃 %4）")
                                 .arg(device->telemetryAcceptedTotal())
                                 .arg(device->telemetryRejectedTotal())
                                 .arg(manager_.telemetry().size())
                                 .arg(manager_.telemetry().discarded()));
    decodeValue_->setText(QStringLiteral("%1 / %2 / %3 / %4 | 解码成功 %5")
                              .arg(decode.framesCrcError)
                              .arg(decode.framesBadLength)
                              .arg(decode.framesBadVersion)
                              .arg(decode.bytesDiscarded)
                              .arg(decode.framesDecoded));
    timeoutValue_->setText(QStringLiteral("Fault %1 | 重发 %2 | 心跳漏拍 %3 | 重试用尽 %4")
                               .arg(ack.timedOutFaulted)
                               .arg(ack.timedOutRetried)
                               .arg(ack.timedOutDropped)
                               .arg(ack.retriesExhausted));
    // 这两项是**内部不变式**：非 0 就说明本模块有 bug。摆在界面上比藏在日志里好。
    internalValue_->setText(QStringLiteral("编码失败 %1 | 发送失败 %2")
                                .arg(device->hostEncodeFailures())
                                .arg(device->hostSendFailures()));

    const QVector<AlarmRecord>& alarms = manager_.alarms();
    alarmValue_->setText(alarms.isEmpty()
                             ? QStringLiteral("无（累计 0 条）")
                             : QStringLiteral("共 %1 条，最近：%2")
                                   .arg(alarms.size())
                                   .arg(alarms.back().text));
}

}  // namespace mchost
