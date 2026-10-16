#include "mc_host/ui/curve_plot.hpp"

#include <algorithm>
#include <cmath>

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>

namespace mchost {
namespace {

constexpr int kMarginLeft = 12;
constexpr int kMarginRight = 12;
constexpr int kMarginTop = 10;
constexpr int kMarginBottom = 26;
constexpr int kLegendLineHeight = 18;

const char* channelLabel(CurveChannel channel) {
    switch (channel) {
        case CurveChannel::Velocity: return "速度";
        case CurveChannel::Position: return "位置";
        case CurveChannel::Target:   return "目标";
    }
    return "?";
}

QColor channelColor(CurveChannel channel) {
    switch (channel) {
        case CurveChannel::Velocity: return QColor(64, 128, 255);
        case CurveChannel::Position: return QColor(40, 170, 90);
        case CurveChannel::Target:   return QColor(230, 130, 40);
    }
    return QColor(128, 128, 128);
}

QString formatValue(double value) {
    const double magnitude = std::fabs(value);
    if (magnitude >= 10000.0) {
        return QString::number(value, 'f', 0);
    }
    if (magnitude >= 100.0) {
        return QString::number(value, 'f', 1);
    }
    return QString::number(value, 'f', 2);
}

}  // namespace

CurvePlot::CurvePlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(220);
    setAutoFillBackground(false);
    for (std::size_t i = 0U; i < kCurveChannelCount; ++i) {
        const CurveChannel channel = static_cast<CurveChannel>(i);
        series_[i].color = channelColor(channel);
    }
}

void CurvePlot::setChannelVisible(CurveChannel channel, bool visible) {
    Series& series = series_[static_cast<std::size_t>(channel)];
    if (series.visible == visible) {
        return;
    }
    series.visible = visible;
    update();
}

bool CurvePlot::isChannelVisible(CurveChannel channel) const noexcept {
    return series_[static_cast<std::size_t>(channel)].visible;
}

void CurvePlot::setSeries(CurveChannel channel, std::vector<CurvePoint> points) {
    series_[static_cast<std::size_t>(channel)].points = std::move(points);
    update();
}

void CurvePlot::setTimeWindow(double fromMs, double toMs) {
    fromMs_ = fromMs;
    toMs_ = toMs;
}

void CurvePlot::clearAll() {
    for (Series& series : series_) {
        series.points.clear();
    }
    update();
}

void CurvePlot::drawSeries(QPainter& painter, const QRectF& area, const Series& series,
                           CurveChannel channel) const {
    if (series.points.size() < 2U) {
        return;
    }

    // 每个通道**用自己的纵轴刻度**。共用一根轴的话，0–300 deg/s 的速度
    // 在 0–10000 deg 的位置旁边就是一条贴底的直线，等于看不见。
    double valueMin = series.points.front().value;
    double valueMax = valueMin;
    for (const CurvePoint& point : series.points) {
        valueMin = std::min(valueMin, point.value);
        valueMax = std::max(valueMax, point.value);
    }
    double span = valueMax - valueMin;
    if (span <= 0.0) {
        // 完全平直：给它一条虚拟跨度，否则除以 0。
        span = std::max(1.0, std::fabs(valueMax) * 0.1);
        valueMin -= span / 2.0;
        valueMax += span / 2.0;
        span = valueMax - valueMin;
    }

    const double timeSpan = (toMs_ - fromMs_);
    if (timeSpan <= 0.0) {
        return;
    }

    QPainterPath path;
    bool started = false;
    for (const CurvePoint& point : series.points) {
        const double x = area.left() + (point.tMs - fromMs_) / timeSpan * area.width();
        const double y = area.bottom() - (point.value - valueMin) / span * area.height();
        const QPointF screen(std::clamp(x, area.left(), area.right()),
                             std::clamp(y, area.top(), area.bottom()));
        if (!started) {
            path.moveTo(screen);
            started = true;
        } else {
            path.lineTo(screen);
        }
    }

    QPen pen(series.color);
    pen.setWidthF(1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);

    // 图例里带上本通道的范围 —— 否则"两条线各自缩放"会让人误以为量级可比。
    const QString text = QStringLiteral("%1(%2)  当前 %3  范围 %4 ~ %5")
                             .arg(QString::fromUtf8(channelLabel(channel)))
                             .arg(QString::fromUtf8(channelUnit(channel)))
                             .arg(formatValue(series.points.back().value))
                             .arg(formatValue(valueMin))
                             .arg(formatValue(valueMax));
    const int index = static_cast<int>(channel);
    painter.setPen(series.color);
    painter.drawText(QPointF(area.left() + 4.0,
                             area.top() + static_cast<qreal>(kLegendLineHeight * (index + 1))),
                     text);
}

void CurvePlot::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(250, 250, 252));

    const QRectF area(static_cast<qreal>(kMarginLeft), static_cast<qreal>(kMarginTop),
                      static_cast<qreal>(width() - kMarginLeft - kMarginRight),
                      static_cast<qreal>(height() - kMarginTop - kMarginBottom));
    if (area.width() <= 1.0 || area.height() <= 1.0) {
        return;
    }

    // 网格
    QPen gridPen(QColor(225, 228, 234));
    gridPen.setWidth(1);
    painter.setPen(gridPen);
    for (int i = 0; i <= 4; ++i) {
        const double x = area.left() + area.width() * static_cast<double>(i) / 4.0;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        const double y = area.top() + area.height() * static_cast<double>(i) / 4.0;
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }

    // 横轴：以"现在"为 0，向左是过去多少秒 —— 滚动曲线这样读最自然。
    painter.setPen(QColor(90, 96, 106));
    const double windowSeconds = (toMs_ - fromMs_) / 1000.0;
    for (int i = 0; i <= 4; ++i) {
        const double x = area.left() + area.width() * static_cast<double>(i) / 4.0;
        const double secondsAgo = windowSeconds * (1.0 - static_cast<double>(i) / 4.0);
        const QString text = QStringLiteral("-%1s").arg(secondsAgo, 0, 'f', 1);
        painter.drawText(QPointF(x - 16.0, area.bottom() + 16.0), text);
    }

    lastDrawnPoints_ = 0U;
    bool anyVisible = false;
    for (std::size_t i = 0U; i < kCurveChannelCount; ++i) {
        if (!series_[i].visible) {
            continue;
        }
        anyVisible = true;
        drawSeries(painter, area, series_[i], static_cast<CurveChannel>(i));
        lastDrawnPoints_ += series_[i].points.size();
    }

    if (!anyVisible) {
        painter.setPen(QColor(120, 124, 132));
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("三个通道都被隐藏了"));
    } else if (lastDrawnPoints_ == 0U) {
        painter.setPen(QColor(120, 124, 132));
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("等待遥测数据……"));
    }

    ++repaints_;
}

}  // namespace mchost
