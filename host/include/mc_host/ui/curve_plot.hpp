#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <QColor>
#include <QWidget>

#include "mc_host/curve_model.hpp"

namespace mchost {

// 层 1（UI）：自绘曲线控件。
//
// 为什么自绘而不是引入绘图库：
//   ① 不引入第三方绘图库依赖 —— 产物零外部图库（`ldd` 可核对）；
//   ② 需要的能力很小 —— 三条折线 + 网格 + 图例，用 QPainter 一百多行就够；
//   ③ 任务书本就要展示"环形缓冲 + 抽稀 + 定频刷新"，自绘反而把这件事摆在明面上。
//
// 本控件**只画**：不采样、不抽稀（那些在 CurvePage 与 curve_model 里，且可无头测试）。
// 这样绘制逻辑与数据逻辑不会互相污染，数据逻辑才能在没显示器的地方被验证。
class CurvePlot : public QWidget {
    Q_OBJECT

public:
    explicit CurvePlot(QWidget* parent = nullptr);

    void setChannelVisible(CurveChannel channel, bool visible);
    bool isChannelVisible(CurveChannel channel) const noexcept;
    void setSeries(CurveChannel channel, std::vector<CurvePoint> points);
    void setTimeWindow(double fromMs, double toMs);
    void clearAll();

    // 重绘次数与本次实际画出的点数 —— 用来证明"定频刷新"真的在节流
    // （样本 100 Hz 进来，重绘只有 20 Hz）。这不是性能指标，只是机制证据。
    std::uint64_t repaintCount() const noexcept { return repaints_; }
    std::size_t lastDrawnPoints() const noexcept { return lastDrawnPoints_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Series {
        std::vector<CurvePoint> points;
        bool visible = true;
        QColor color;
    };

    void drawSeries(QPainter& painter, const QRectF& area, const Series& series,
                    CurveChannel channel) const;

    std::array<Series, kCurveChannelCount> series_;
    double fromMs_ = 0.0;
    double toMs_ = 1.0;
    std::uint64_t repaints_ = 0U;
    std::size_t lastDrawnPoints_ = 0U;
};

}  // namespace mchost
