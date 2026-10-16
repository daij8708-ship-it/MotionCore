#include "mc_host/ui/curve_page.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "mc_host/curve_model.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/ui/curve_plot.hpp"

namespace mchost {
namespace {

// 一次重绘最多画多少个点。超出的部分用 min/max 分桶抽稀掉。
// 取 900 的理由：控件宽度按 1200 px 算，每像素不到一个点，再多画的就是重复像素。
constexpr std::size_t kMaxPointsPerRepaint = 900U;

}  // namespace

CurvePage::CurvePage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* controls = new QHBoxLayout();
    velocityBox_ = new QCheckBox(QStringLiteral("速度"), this);
    velocityBox_->setObjectName(QStringLiteral("velocityCheck"));
    velocityBox_->setChecked(true);
    positionBox_ = new QCheckBox(QStringLiteral("位置"), this);
    positionBox_->setObjectName(QStringLiteral("positionCheck"));
    positionBox_->setChecked(true);
    targetBox_ = new QCheckBox(QStringLiteral("目标"), this);
    targetBox_->setObjectName(QStringLiteral("targetCheck"));
    targetBox_->setChecked(true);

    windowCombo_ = new QComboBox(this);
    windowCombo_->setObjectName(QStringLiteral("windowCombo"));
    windowCombo_->addItem(QStringLiteral("最近 1 s"), 1000);
    windowCombo_->addItem(QStringLiteral("最近 5 s"), 5000);
    windowCombo_->addItem(QStringLiteral("最近 10 s"), 10000);
    windowCombo_->addItem(QStringLiteral("最近 30 s"), 30000);
    windowCombo_->setCurrentIndex(1);

    pauseButton_ = new QPushButton(QStringLiteral("暂停"), this);
    pauseButton_->setObjectName(QStringLiteral("pauseButton"));
    pauseButton_->setCheckable(true);
    clearButton_ = new QPushButton(QStringLiteral("清空缓冲"), this);
    clearButton_->setObjectName(QStringLiteral("clearButton"));

    controls->addWidget(new QLabel(QStringLiteral("通道："), this));
    controls->addWidget(velocityBox_);
    controls->addWidget(positionBox_);
    controls->addWidget(targetBox_);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(QStringLiteral("时间窗："), this));
    controls->addWidget(windowCombo_);
    controls->addStretch(1);
    controls->addWidget(pauseButton_);
    controls->addWidget(clearButton_);
    root->addLayout(controls);

    plot_ = new CurvePlot(this);
    plot_->setObjectName(QStringLiteral("curvePlot"));
    root->addWidget(plot_, 1);

    statsLabel_ = new QLabel(this);
    statsLabel_->setObjectName(QStringLiteral("curveStatsLabel"));
    root->addWidget(statsLabel_);

    hintLabel_ = new QLabel(
        QStringLiteral("三条曲线**各用自己的纵轴刻度** —— 速度是 deg/s、位置是 deg，"
                       "共用一根轴的话 0–300 的速度在 0–10000 的位置旁边就是一条贴底的直线。"
                       "图例里给出了每条线的当前值与范围，量级请以图例为准。"),
        this);
    hintLabel_->setWordWrap(true);
    hintLabel_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(hintLabel_);

    connect(velocityBox_, &QCheckBox::toggled, this, [this](bool on) {
        plot_->setChannelVisible(CurveChannel::Velocity, on);
        updateStatsLabel();
    });
    connect(positionBox_, &QCheckBox::toggled, this, [this](bool on) {
        plot_->setChannelVisible(CurveChannel::Position, on);
        updateStatsLabel();
    });
    connect(targetBox_, &QCheckBox::toggled, this, [this](bool on) {
        plot_->setChannelVisible(CurveChannel::Target, on);
        updateStatsLabel();
    });
    connect(windowCombo_, &QComboBox::currentIndexChanged, this, [this]() { pullAndDraw(); });
    connect(pauseButton_, &QPushButton::toggled, this, [this](bool checked) {
        paused_ = checked;
        pauseButton_->setText(checked ? QStringLiteral("继续") : QStringLiteral("暂停"));
    });
    connect(clearButton_, &QPushButton::clicked, this, [this]() {
        manager_.clearTelemetry();
        plot_->clearAll();
        updateStatsLabel();
    });

    connect(&manager_, &DeviceManager::statsRefreshed, this, &CurvePage::refresh);
    refresh();
}

void CurvePage::refresh() {
    if (!paused_) {
        pullAndDraw();
    }
    updateStatsLabel();
}

void CurvePage::pullAndDraw() {
    const IDevice* device = manager_.device();
    if (device == nullptr || !manager_.isConnected()) {
        return;
    }
    const TelemetryRing& ring = manager_.telemetry();
    if (ring.empty()) {
        return;
    }

    const double nowMs = manager_.elapsedMs();
    const double windowMs = static_cast<double>(windowCombo_->currentData().toInt());
    CurveWindow window;
    window.fromMs = nowMs - windowMs;
    window.toMs = nowMs;

    plot_->setTimeWindow(window.fromMs, window.toMs);
    drawnPoints_ = 0U;
    for (std::size_t i = 0U; i < kCurveChannelCount; ++i) {
        const CurveChannel channel = static_cast<CurveChannel>(i);
        if (!plot_->isChannelVisible(channel)) {
            plot_->setSeries(channel, {});
            continue;
        }
        std::vector<CurvePoint> points = extractSeries(ring, channel, window);
        points = decimateSeries(points, kMaxPointsPerRepaint);
        drawnPoints_ += points.size();
        plot_->setSeries(channel, std::move(points));
    }
}

void CurvePage::updateStatsLabel() {
    const TelemetryRing& ring = manager_.telemetry();

    // 把 tick 数与重绘数并排显示，节流有没有生效一眼可查：
    // tick 是 100 Hz，重绘应该只有它的五分之一左右。
    statsLabel_->setText(
        QStringLiteral("缓冲 %1 条（容量 %2，因覆盖丢弃 %3） | 本拍画出 %4 点 | "
                       "tick %5 次 / 控件重绘 %6 次（遥测 100 Hz 进入，界面 20 Hz 刷新）")
            .arg(ring.size())
            .arg(TelemetryRing::kCapacity)
            .arg(ring.discarded())
            .arg(drawnPoints_)
            .arg(manager_.ticks())
            .arg(plot_->repaintCount()));
}

}  // namespace mchost
