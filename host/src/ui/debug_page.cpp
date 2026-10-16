#include "mc_host/ui/debug_page.hpp"

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {

DebugPage::DebugPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* hint = new QLabel(
        QStringLiteral("这一页把通信层的**全部计数器**摊开：总览页只给几条汇总，"
                       "排查时要看的是具体是哪一项在涨（例如 CRC 错一直在涨，"
                       "说明线上有干扰；重同步丢字节在涨而 CRC 不涨，说明帧头被吃掉了）。\n"
                       "所有数字都直接来自核心库的统计结构，界面不做二次加工。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#444;"));
    root->addWidget(hint);

    auto* columns = new QGridLayout();

    auto* frameGroup = new QGroupBox(QStringLiteral("帧与解码"), this);
    auto* frameGrid = new QGridLayout(frameGroup);
    addRow(frameGrid, 0, QStringLiteral("framesSent"), QStringLiteral("发送帧"));
    addRow(frameGrid, 1, QStringLiteral("encodeFailures"), QStringLiteral("编码失败（内部不变式）"));
    addRow(frameGrid, 2, QStringLiteral("sendFailures"), QStringLiteral("发送失败"));
    addRow(frameGrid, 3, QStringLiteral("decodeOk"), QStringLiteral("解码成功"));
    addRow(frameGrid, 4, QStringLiteral("crcError"), QStringLiteral("CRC 错"));
    addRow(frameGrid, 5, QStringLiteral("badLength"), QStringLiteral("长度错"));
    addRow(frameGrid, 6, QStringLiteral("badVersion"), QStringLiteral("版本错"));
    addRow(frameGrid, 7, QStringLiteral("bytesDiscarded"), QStringLiteral("重同步丢弃字节"));
    columns->addWidget(frameGroup, 0, 0);

    auto* ackGroup = new QGroupBox(QStringLiteral("应答跟踪（AckTracker）"), this);
    auto* ackGrid = new QGridLayout(ackGroup);
    addRow(ackGrid, 0, QStringLiteral("ackRegistered"), QStringLiteral("登记"));
    addRow(ackGrid, 1, QStringLiteral("ackCompleted"), QStringLiteral("对上（收到应答）"));
    addRow(ackGrid, 2, QStringLiteral("ackRetried"), QStringLiteral("超时 → 重发"));
    addRow(ackGrid, 3, QStringLiteral("ackFaulted"), QStringLiteral("超时 → 判 Fault"));
    addRow(ackGrid, 4, QStringLiteral("ackDropped"), QStringLiteral("心跳漏拍（不计故障）"));
    addRow(ackGrid, 5, QStringLiteral("ackExhausted"), QStringLiteral("重试用尽"));
    addRow(ackGrid, 6, QStringLiteral("ackNaked"), QStringLiteral("被拒（收到 NAK）"));
    addRow(ackGrid, 7, QStringLiteral("ackStale"), QStringLiteral("陈旧应答"));
    addRow(ackGrid, 8, QStringLiteral("ackMismatch"), QStringLiteral("类型对不上"));
    addRow(ackGrid, 9, QStringLiteral("ackNotAwaitable"), QStringLiteral("不可应答类型被拒"));
    addRow(ackGrid, 10, QStringLiteral("ackDupSeq"), QStringLiteral("重复 SEQ 被拒"));
    columns->addWidget(ackGroup, 0, 1);

    auto* linkGroup = new QGroupBox(QStringLiteral("链路监控（LinkMonitor）"), this);
    auto* linkGrid = new QGridLayout(linkGroup);
    addRow(linkGrid, 0, QStringLiteral("linkVerdict"), QStringLiteral("链路判定"));
    addRow(linkGrid, 1, QStringLiteral("linkRtt"), QStringLiteral("实测 RTT(P95)"));
    addRow(linkGrid, 2, QStringLiteral("linkWindow"), QStringLiteral("生效超时窗口"));
    addRow(linkGrid, 3, QStringLiteral("hbSent"), QStringLiteral("心跳发出"));
    addRow(linkGrid, 4, QStringLiteral("hbAcked"), QStringLiteral("心跳已应答"));
    addRow(linkGrid, 5, QStringLiteral("hbStale"), QStringLiteral("陈旧心跳应答"));
    addRow(linkGrid, 6, QStringLiteral("hbSamples"), QStringLiteral("RTT 样本数"));
    addRow(linkGrid, 7, QStringLiteral("hbDown"), QStringLiteral("链路失效次数"));
    addRow(linkGrid, 8, QStringLiteral("handshakeRounds"), QStringLiteral("握手轮次"));
    addRow(linkGrid, 9, QStringLiteral("ticks"), QStringLiteral("tick / 时长"));
    columns->addWidget(linkGroup, 1, 0);

    auto* telemetryGroup = new QGroupBox(QStringLiteral("遥测与缓冲"), this);
    auto* telemetryGrid = new QGridLayout(telemetryGroup);
    addRow(telemetryGrid, 0, QStringLiteral("telAccepted"), QStringLiteral("接收（累计）"));
    addRow(telemetryGrid, 1, QStringLiteral("telRejected"), QStringLiteral("拒收（累计）"));
    addRow(telemetryGrid, 2, QStringLiteral("ringSize"), QStringLiteral("缓冲条数 / 容量"));
    addRow(telemetryGrid, 3, QStringLiteral("ringDiscarded"), QStringLiteral("因覆盖丢弃"));
    columns->addWidget(telemetryGroup, 1, 1);

    root->addLayout(columns);

    // --- 故障注入（本页是唯一设置处）-----------------------------------------
    auto* injectGroup = new QGroupBox(
        QStringLiteral("故障注入（演示与排查用；打在回环链路上，与用例用的是同一套注入）"),
        this);
    auto* injectGrid = new QGridLayout(injectGroup);

    const auto makeSpin = [this, injectGrid](QSpinBox*& spin, int row, const QString& title,
                                             int value, const QString& objectName) {
        injectGrid->addWidget(new QLabel(title, this), row, 0);
        spin = new QSpinBox(this);
        spin->setObjectName(objectName);
        // 0 = 关闭。注入量上限取 1000：再密的丢包等价于"整条链路断"，用不着更大。
        spin->setRange(0, 1000);
        spin->setValue(value);
        injectGrid->addWidget(spin, row, 1);
    };
    makeSpin(uplinkDropSpin_, 0, QStringLiteral("上行：每 N 字节丢 1（设备 → 上位机）"), 0,
             QStringLiteral("uplinkDropSpin"));
    makeSpin(downlinkDropSpin_, 1, QStringLiteral("下行：每 N 字节丢 1（上位机 → 设备）"), 0,
             QStringLiteral("downlinkDropSpin"));
    makeSpin(uplinkLatencySpin_, 2, QStringLiteral("上行：延迟 N 拍"), 0,
             QStringLiteral("uplinkLatencySpin"));

    applyInjectionButton_ = new QPushButton(QStringLiteral("应用注入"), this);
    applyInjectionButton_->setObjectName(QStringLiteral("applyInjectionButton"));
    clearInjectionButton_ = new QPushButton(QStringLiteral("清除全部注入"), this);
    clearInjectionButton_->setObjectName(QStringLiteral("clearInjectionButton"));
    injectGrid->addWidget(applyInjectionButton_, 3, 0);
    injectGrid->addWidget(clearInjectionButton_, 3, 1);

    injectionHint_ = new QLabel(this);
    injectionHint_->setObjectName(QStringLiteral("injectionHint"));
    injectionHint_->setWordWrap(true);
    injectionHint_->setStyleSheet(QStringLiteral("color:#666;"));
    injectGrid->addWidget(injectionHint_, 4, 0, 1, 2);
    root->addWidget(injectGroup);
    root->addStretch(1);

    connect(applyInjectionButton_, &QPushButton::clicked, this, &DebugPage::applyInjection);
    connect(clearInjectionButton_, &QPushButton::clicked, this, &DebugPage::clearInjection);
    connect(&manager_, &DeviceManager::connectionChanged, this, &DebugPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &DebugPage::refresh);

    onConnectionChanged(manager_.isConnected());
}

QLabel* DebugPage::addRow(QGridLayout* grid, int row, const QString& key, const QString& title) {
    auto* titleLabel = new QLabel(title + QStringLiteral("："), this);
    auto* valueLabel = new QLabel(QStringLiteral("—"), this);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setObjectName(QStringLiteral("dbg_") + key);
    grid->addWidget(titleLabel, row, 0, Qt::AlignRight | Qt::AlignTop);
    grid->addWidget(valueLabel, row, 1);
    grid->setColumnStretch(1, 1);
    rows_.insert(key, valueLabel);
    return valueLabel;
}

void DebugPage::setRow(const char* key, const QString& text) {
    QLabel* label = rows_.value(QString::fromLatin1(key));
    if (label != nullptr) {
        label->setText(text);
    }
}

void DebugPage::onConnectionChanged(bool connected) {
    const bool canInject = connected && manager_.device() != nullptr;
    uplinkDropSpin_->setEnabled(canInject);
    downlinkDropSpin_->setEnabled(canInject);
    uplinkLatencySpin_->setEnabled(canInject);
    applyInjectionButton_->setEnabled(canInject);
    clearInjectionButton_->setEnabled(canInject);
    injectionHint_->setText(
        canInject ? QStringLiteral("注入只在链路打开时有效。断开重连后注入不会自动恢复 —— "
                                  "要用就重新点“应用注入”。")
                  : QStringLiteral("未连接：没有链路可以注入。"));
    refresh();
}

void DebugPage::applyInjection() {
    manager_.setUplinkDropEveryNthByte(uplinkDropSpin_->value());
    manager_.setDownlinkDropEveryNthByte(downlinkDropSpin_->value());
    manager_.setUplinkLatencyPolls(uplinkLatencySpin_->value());

    if (uplinkDropSpin_->value() == 0 && downlinkDropSpin_->value() == 0 &&
        uplinkLatencySpin_->value() == 0) {
        injectionHint_->setText(QStringLiteral("当前没有任何注入。"));
    } else {
        injectionHint_->setText(QStringLiteral("已应用：上行每 %1 字节丢 1 / 下行每 %2 字节丢 1 / "
                                               "上行延迟 %3 拍。")
                                    .arg(uplinkDropSpin_->value())
                                    .arg(downlinkDropSpin_->value())
                                    .arg(uplinkLatencySpin_->value()));
    }
}

void DebugPage::clearInjection() {
    manager_.clearInjectedFaults();
    uplinkDropSpin_->setValue(0);
    downlinkDropSpin_->setValue(0);
    uplinkLatencySpin_->setValue(0);
    injectionHint_->setText(QStringLiteral("注入已全部清除。"));
}

void DebugPage::refresh() {
    const IDevice* device = manager_.device();
    if (device == nullptr) {
        return;
    }

    const mc::StreamDecoder::Stats& decode = device->decoderStats();
    setRow("framesSent", QString::number(device->framesSent()));
    setRow("encodeFailures", QString::number(device->hostEncodeFailures()));
    setRow("sendFailures", QString::number(device->hostSendFailures()));
    setRow("decodeOk", QString::number(decode.framesDecoded));
    setRow("crcError", QString::number(decode.framesCrcError));
    setRow("badLength", QString::number(decode.framesBadLength));
    setRow("badVersion", QString::number(decode.framesBadVersion));
    setRow("bytesDiscarded", QString::number(decode.bytesDiscarded));

    const mc::AckStats& ack = device->ackStats();
    setRow("ackRegistered", QString::number(ack.registered));
    setRow("ackCompleted", QString::number(ack.completed));
    setRow("ackRetried", QString::number(ack.timedOutRetried));
    setRow("ackFaulted", QString::number(ack.timedOutFaulted));
    setRow("ackDropped", QString::number(ack.timedOutDropped));
    setRow("ackExhausted", QString::number(ack.retriesExhausted));
    setRow("ackNaked", QString::number(ack.rejectedByPeer));
    setRow("ackStale", QString::number(ack.staleAcks));
    setRow("ackMismatch", QString::number(ack.mismatchedAcks));
    setRow("ackNotAwaitable", QString::number(ack.rejectedNotAwaitable));
    setRow("ackDupSeq", QString::number(ack.rejectedDuplicateSeq));

    if (!manager_.isConnected()) {
        setRow("linkVerdict", QStringLiteral("未连接"));
    } else if (device->linkDown()) {
        setRow("linkVerdict", QStringLiteral("判失效（连续丢拍）"));
    } else {
        setRow("linkVerdict", device->linkStats() != nullptr ? QStringLiteral("正常（监控中）")
                                                            : QStringLiteral("未监控"));
    }
    setRow("linkRtt", device->hasRttSample()
                          ? QStringLiteral("%1 ms").arg(device->rttP95Ms())
                          : QStringLiteral("无样本"));
    setRow("linkWindow", QStringLiteral("%1 ms").arg(device->ackTimeoutMs()));

    const mc::LinkMonitor::Stats* link = device->linkStats();
    setRow("hbSent", link != nullptr ? QString::number(link->heartbeatsSent)
                                     : QStringLiteral("未启用链路监控"));
    setRow("hbAcked", link != nullptr ? QString::number(link->heartbeatsAcked)
                                      : QStringLiteral("—"));
    setRow("hbStale", link != nullptr ? QString::number(link->staleHeartbeatAcks)
                                      : QStringLiteral("—"));
    setRow("hbSamples", link != nullptr ? QString::number(link->rttSamples) : QStringLiteral("—"));
    setRow("hbDown", link != nullptr ? QString::number(link->linkDownTransitions)
                                     : QStringLiteral("—"));
    setRow("handshakeRounds", QString::number(device->handshakeRounds()));
    setRow("ticks", QStringLiteral("%1 拍 / %2 s")
                        .arg(manager_.ticks())
                        .arg(manager_.elapsedMs() / 1000.0, 0, 'f', 1));

    setRow("telAccepted", QString::number(device->telemetryAcceptedTotal()));
    setRow("telRejected", QString::number(device->telemetryRejectedTotal()));
    setRow("ringSize", QStringLiteral("%1 / %2")
                           .arg(manager_.telemetry().size())
                           .arg(TelemetryRing::kCapacity));
    setRow("ringDiscarded", QString::number(manager_.telemetry().discarded()));
}

}  // namespace mchost
