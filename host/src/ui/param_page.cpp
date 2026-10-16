#include "mc_host/ui/param_page.hpp"

#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/payload.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {
namespace {

constexpr int kLogColumns = 7;

QString formatTime(double tMs) {
    return QStringLiteral("%1 s").arg(tMs / 1000.0, 0, 'f', 2);
}

}  // namespace

ParamPage::ParamPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral(
            "这一页是 PID 的**唯一可写入口**：同一个参数有两个写入口，迟早会出现"
            "“两处显示的值不一样、不知道该信哪个”。\n"
            "⚠️ 下面那张表是**下发记录**，不是设备的回读值 —— 协议 v1 里没有“读参数”的消息，"
            "遥测帧也不带 PID，所以“设备现在实际用的是多少”在协议层面答不出来。"
            "要知道它有没有生效，看的是“结果”列：对端确认了才算落地。"),
        this);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:#444;"));
    root->addWidget(intro);

    auto* pidGroup = new QGroupBox(
        QStringLiteral("PID 参数（SET_PID —— 幂等命令，超时后允许重传）"), this);
    auto* pidRow = new QHBoxLayout(pidGroup);

    const auto makeGain = [this, pidRow](QDoubleSpinBox*& spin, const QString& name,
                                         double value, const QString& objectName) {
        pidRow->addWidget(new QLabel(name, this));
        spin = new QDoubleSpinBox(this);
        spin->setObjectName(objectName);
        // 上下限直接取自**协议校验用的那个常量**，界面不另立一套限制：
        // 两套限制必然有一处先过期，而先过期的那一处通常是界面（没人会去改它）。
        spin->setRange(0.0, static_cast<double>(mc::kSanityMaxPidGain));
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setValue(value);
        pidRow->addWidget(spin);
    };
    makeGain(kpSpin_, QStringLiteral("Kp"), 1.6, QStringLiteral("kpSpin"));
    makeGain(kiSpin_, QStringLiteral("Ki"), 0.4, QStringLiteral("kiSpin"));
    makeGain(kdSpin_, QStringLiteral("Kd"), 0.02, QStringLiteral("kdSpin"));

    applyButton_ = new QPushButton(QStringLiteral("下发 PID"), this);
    applyButton_->setObjectName(QStringLiteral("applyPidButton"));
    pidRow->addWidget(applyButton_);
    pidRow->addStretch(1);
    root->addWidget(pidGroup);

    auto* limitHint = new QLabel(
        QStringLiteral("取值范围 0 … %1（协议的判定只挡负数与非有限值，上限就是这条常量）。"
                       "Kp/Ki/Kd 都是**无量纲系数**，设备侧怎么用它们取决于当前是速度环还是位置环。")
            .arg(static_cast<double>(mc::kSanityMaxPidGain), 0, 'g', 6),
        this);
    limitHint->setWordWrap(true);
    limitHint->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(limitHint);

    resultLabel_ = new QLabel(QStringLiteral("尚未下发过 PID"), this);
    resultLabel_->setObjectName(QStringLiteral("pidResultLabel"));
    QFont resultFont = resultLabel_->font();
    resultFont.setBold(true);
    resultLabel_->setFont(resultFont);
    resultLabel_->setWordWrap(true);
    root->addWidget(resultLabel_);

    logTable_ = new QTableWidget(0, kLogColumns, this);
    logTable_->setObjectName(QStringLiteral("pidLogTable"));
    logTable_->setHorizontalHeaderLabels(
        {QStringLiteral("时刻"), QStringLiteral("SEQ"), QStringLiteral("Kp"), QStringLiteral("Ki"),
         QStringLiteral("Kd"), QStringLiteral("结果"), QStringLiteral("说明")});
    logTable_->verticalHeader()->setVisible(false);
    logTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logTable_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(logTable_, 1);

    connect(applyButton_, &QPushButton::clicked, this, &ParamPage::applyPid);
    connect(&manager_, &DeviceManager::connectionChanged, this, &ParamPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::commandLogChanged, this, &ParamPage::refreshLog);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &ParamPage::refresh);

    onConnectionChanged(manager_.isConnected());
    refreshLog();
}

void ParamPage::applyPid() {
    const double kp = kpSpin_->value();
    const double ki = kiSpin_->value();
    const double kd = kdSpin_->value();

    if (!manager_.sendSetPid(kp, ki, kd)) {
        // 返回 false = 本地就没发出去（会话未就绪）。这不是"设备拒绝了"，
        // 两者混为一谈会把排查方向指错。
        resultLabel_->setText(QStringLiteral("没有发出去：会话未就绪（命令行里会留下一条“未发出”）"));
        return;
    }

    // 刚发出去的那条命令就是命令行里最新的一条 —— 当场取它的 seq 来关联结果。
    // 不去猜 DeviceManager 内部的序号分配，也不自己维护一个平行的计数器。
    const IDevice* device = manager_.device();
    if (device != nullptr && !device->commandLog().empty()) {
        const CommandRecord& newest = device->commandLog().front();
        sends_.push_front(PidSend{newest.seq, kp, ki, kd});
        while (sends_.size() > kMaxSends) {
            sends_.pop_back();
        }
    }

    resultLabel_->setText(QStringLiteral("已下发 SET_PID(%1, %2, %3)，等结果…")
                              .arg(kp, 0, 'f', 3)
                              .arg(ki, 0, 'f', 3)
                              .arg(kd, 0, 'f', 3));
    refreshLog();
}

void ParamPage::onConnectionChanged(bool connected) {
    const IDevice* device = manager_.device();
    const bool ready = connected && device != nullptr && device->sessionReady();
    lastReady_ = ready;

    applyButton_->setEnabled(ready);
    kpSpin_->setEnabled(ready);
    kiSpin_->setEnabled(ready);
    kdSpin_->setEnabled(ready);
    refresh();
}

void ParamPage::refresh() {
    // 会话就绪状态是**异步**变化的（握手要几拍），这里要跟着变，
    // 不能只在 connectionChanged 时算一次 —— 那个信号在"握手完成"时不会再发。
    const IDevice* device = manager_.device();
    const bool ready = manager_.isConnected() && device != nullptr && device->sessionReady();
    if (ready != lastReady_) {
        onConnectionChanged(manager_.isConnected());
        return;
    }

    applyButton_->setToolTip(ready ? QString()
                                   : QStringLiteral("会话未就绪：握手完成前下发，设备会把命令拒掉"));
}

void ParamPage::refreshLog() {
    const IDevice* device = manager_.device();
    logTable_->setRowCount(static_cast<int>(sends_.size()));
    if (device == nullptr) {
        return;
    }

    const std::deque<CommandRecord>& log = device->commandLog();

    int row = 0;
    for (const PidSend& send : sends_) {
        const CommandRecord* record = nullptr;
        for (const CommandRecord& candidate : log) {
            if (candidate.seq == send.seq && candidate.name == "SET_PID") {
                record = &candidate;
                break;
            }
        }

        const auto set = [this, row](int column, const QString& text) {
            logTable_->setItem(row, column, new QTableWidgetItem(text));
        };
        set(0, (record != nullptr && record->sentAtMs != 0U)
                   ? formatTime(static_cast<double>(record->sentAtMs))
                   : QStringLiteral("—"));
        set(1, QString::number(send.seq));
        set(2, QString::number(send.kp, 'f', 3));
        set(3, QString::number(send.ki, 'f', 3));
        set(4, QString::number(send.kd, 'f', 3));

        QString status = QStringLiteral("记录已滚出");
        QString detail;
        if (record != nullptr) {
            status = QString::fromUtf8(commandStatusName(record->status));
            if (record->status == CommandStatus::Nacked) {
                detail = QString::fromUtf8(nackReasonName(record->nackReason));
            } else if (record->status == CommandStatus::TimedOut) {
                detail = QStringLiteral("未确认即超时（幂等命令，允许重传）");
            } else if (record->status == CommandStatus::NotSent) {
                detail = QStringLiteral("会话未就绪，本地就没有发出去");
            }
        }
        set(5, status);
        set(6, detail);
        ++row;
    }
    logTable_->resizeColumnsToContents();
}

}  // namespace mchost
