#include "mc_host/ui/param_page.hpp"

#include <QCheckBox>
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

// 软限位在界面上的说法只有两种：一组区间，或者"未启用"。
// **不给"关掉但留着旧数值"的第三种写法**：关掉就是关掉，设备一条都不看，
// 而界面上多一个"好像还设着什么"的显示只会让人以为它在起作用。
QString limitsText(const mc::SoftLimits& limits) {
    if (!limits.enabled) {
        return QStringLiteral("未启用（设备在这条上一律放行）");
    }
    return QStringLiteral("启用：[%1, %2] deg")
        .arg(limits.minDeg, 0, 'f', 1)
        .arg(limits.maxDeg, 0, 'f', 1);
}

}  // namespace

ParamPage::ParamPage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral(
            "这一页是 PID 与软限位的**唯一可写入口**：同一个参数有两个写入口，"
            "迟早会出现“两处显示的值不一样、不知道该信哪个”。\n"
            "⚠️ 下面的“下发记录”是**上位机发出去的东西**，不是设备的实存值。"
            "要回答“设备现在到底用的是哪一组”，只有按下回读、看设备回过来的那一段 —— "
            "协议里的 READ_PARAMS 就是为这一问存在的。"),
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

    // --- 软限位（SET_LIMITS）---------------------------------------------------
    //
    // 写在这一页而不是运动页：软限位是**设备侧的约束参数**，与增益同属"配置"，
    // 而"现在往哪走"才是运动页的事。
    auto* limitsGroup = new QGroupBox(
        QStringLiteral("软限位（SET_LIMITS —— 位置行程限制，定义的是“哪里算越界”）"), this);
    auto* limitsRow = new QHBoxLayout(limitsGroup);
    limitEnabledCheck_ = new QCheckBox(QStringLiteral("启用"), this);
    limitEnabledCheck_->setObjectName(QStringLiteral("limitsEnabledCheck"));
    limitsRow->addWidget(limitEnabledCheck_);
    limitsRow->addWidget(new QLabel(QStringLiteral("最小"), this));
    limitMinSpin_ = new QDoubleSpinBox(this);
    limitMinSpin_->setObjectName(QStringLiteral("limitsMinSpin"));
    limitMinSpin_->setRange(-100000.0, 100000.0);
    limitMinSpin_->setDecimals(1);
    limitMinSpin_->setSingleStep(30.0);
    limitMinSpin_->setValue(-180.0);
    limitsRow->addWidget(limitMinSpin_);
    limitsRow->addWidget(new QLabel(QStringLiteral("deg   最大"), this));
    limitMaxSpin_ = new QDoubleSpinBox(this);
    limitMaxSpin_->setObjectName(QStringLiteral("limitsMaxSpin"));
    limitMaxSpin_->setRange(-100000.0, 100000.0);
    limitMaxSpin_->setDecimals(1);
    limitMaxSpin_->setSingleStep(30.0);
    limitMaxSpin_->setValue(180.0);
    limitsRow->addWidget(limitMaxSpin_);
    limitsRow->addWidget(new QLabel(QStringLiteral("deg"), this));
    applyLimitsButton_ = new QPushButton(QStringLiteral("下发软限位"), this);
    applyLimitsButton_->setObjectName(QStringLiteral("applyLimitsButton"));
    limitsRow->addWidget(applyLimitsButton_);
    limitsRow->addStretch(1);
    root->addWidget(limitsGroup);

    auto* limitsNote = new QLabel(
        QStringLiteral(
            "“启用”是关键：关掉时两个数值**不参与任何判定**，设备一条都不看。\n"
            "设备是执法方（它才是那个能让电机停下的），所以它还会多判一层："
            "新限位若把**当前位置**或**当前目标**关在门外，报文会以“参数不合法”被拒 —— "
            "那种限位设下去等于下一拍就报超限故障。"),
        this);
    limitsNote->setWordWrap(true);
    limitsNote->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(limitsNote);

    limitsResultLabel_ = new QLabel(QStringLiteral("尚未下发过软限位"), this);
    limitsResultLabel_->setObjectName(QStringLiteral("limitsResultLabel"));
    limitsResultLabel_->setWordWrap(true);
    root->addWidget(limitsResultLabel_);

    // --- 回读设备实存参数（READ_PARAMS）----------------------------------------
    auto* readbackGroup =
        new QGroupBox(QStringLiteral("回读设备实存参数（READ_PARAMS —— 设备回的是什么就是什么）"),
                      this);
    auto* readbackLayout = new QVBoxLayout(readbackGroup);
    auto* readbackRow = new QHBoxLayout();
    readbackButton_ = new QPushButton(QStringLiteral("回读一次"), this);
    readbackButton_->setObjectName(QStringLiteral("readbackButton"));
    readbackRow->addWidget(readbackButton_);
    readbackRow->addStretch(1);
    readbackLayout->addLayout(readbackRow);
    readbackLabel_ = new QLabel(this);
    readbackLabel_->setObjectName(QStringLiteral("readbackLabel"));
    readbackLabel_->setWordWrap(true);
    readbackLayout->addWidget(readbackLabel_);
    root->addWidget(readbackGroup);

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
    connect(applyLimitsButton_, &QPushButton::clicked, this, &ParamPage::applyLimits);
    connect(readbackButton_, &QPushButton::clicked, this, [this]() {
        if (!manager_.requestParamReadback()) {
            limitsResultLabel_->setText(manager_.lastRefusalText());
            return;
        }
        limitsResultLabel_->setText(
            QStringLiteral("已请求回读（READ_PARAMS），等设备回话…"));
    });
    // 回读值是**异步**到达的：命令发出去与设备回话之间隔着几拍，
    // 所以这里跟着 20 Hz 那条刷新走，而不是发完就读（那时读到的还是上一次的结果）。
    connect(&manager_, &DeviceManager::statsRefreshed, this, &ParamPage::refreshReadback);
    connect(&manager_, &DeviceManager::connectionChanged, this, &ParamPage::onConnectionChanged);
    connect(&manager_, &DeviceManager::commandLogChanged, this, &ParamPage::refreshLog);
    connect(&manager_, &DeviceManager::statsRefreshed, this, &ParamPage::refresh);

    onConnectionChanged(manager_.isConnected());
    refreshLog();
    refreshReadback();
}

void ParamPage::applyLimits() {
    mc::SoftLimits limits;
    limits.enabled = limitEnabledCheck_->isChecked();
    limits.minDeg = static_cast<float>(limitMinSpin_->value());
    limits.maxDeg = static_cast<float>(limitMaxSpin_->value());

    if (!manager_.sendSoftLimits(limits)) {
        limitsResultLabel_->setText(manager_.lastRefusalText());
        return;
    }
    limitsResultLabel_->setText(QStringLiteral("已下发 SET_LIMITS（%1），等结果…")
                                    .arg(limitsText(limits)));
}

void ParamPage::refreshReadback() {
    mc::ParamState state;
    if (!manager_.deviceParamState(state)) {
        readbackLabel_->setText(
            QStringLiteral("还没回读过 —— 这一段是空的，因为**设备没有说过**它现在存的是什么。"
                           "上面输入框里的数字是我发出去的，不是它存着的。"));
        readbackLabel_->setStyleSheet(QStringLiteral("color:#8a5a00;"));
        return;
    }

    readbackLabel_->setText(QStringLiteral("设备侧实存：模式 %1、目标 %2、Kp %3、Ki %4、Kd %5\n软限位：%6")
                                .arg(static_cast<int>(state.mode))
                                .arg(static_cast<double>(state.target), 0, 'f', 3)
                                .arg(static_cast<double>(state.kp), 0, 'f', 4)
                                .arg(static_cast<double>(state.ki), 0, 'f', 4)
                                .arg(static_cast<double>(state.kd), 0, 'f', 4)
                                .arg(limitsText(state.limits)));
    readbackLabel_->setStyleSheet(QStringLiteral("color:#1b5e20;"));
}

void ParamPage::applyPid() {
    const double kp = kpSpin_->value();
    const double ki = kiSpin_->value();
    const double kd = kdSpin_->value();

    if (!manager_.sendSetPid(kp, ki, kd)) {
        // 返回 false 有两种来路：没权限，或本地就没发出去（会话未就绪 / 没连上）。
        // 两者混为一谈会把排查方向指错，所以显示的是**管理层给的原话** ——
        // 它已经分好了类（见 DeviceManager::lastRefusalText）。
        resultLabel_->setText(manager_.lastRefusalText());
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
    applyLimitsButton_->setEnabled(ready);
    limitEnabledCheck_->setEnabled(ready);
    limitMinSpin_->setEnabled(ready);
    limitMaxSpin_->setEnabled(ready);
    readbackButton_->setEnabled(ready);
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
