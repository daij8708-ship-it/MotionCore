#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc_host/telemetry_ring.hpp"

namespace mchost {

// 曲线通道。三个通道**单位不同**（deg/s、deg、deg），因此绘制时各用自己的纵轴刻度 ——
// 共用一根轴的话，0–300 deg/s 的速度在 0–10000 deg 的位置旁边就是一条直线。
enum class CurveChannel : std::uint8_t {
    Velocity = 0U,
    Position = 1U,
    Target = 2U,
};
inline constexpr std::size_t kCurveChannelCount = 3U;

const char* channelName(CurveChannel channel) noexcept;
const char* channelUnit(CurveChannel channel) noexcept;

double channelValue(const TelemetrySample& sample, CurveChannel channel) noexcept;

struct CurvePoint {
    double tMs = 0.0;
    double value = 0.0;
};

struct CurveWindow {
    double fromMs = 0.0;   // 含
    double toMs = 0.0;     // 含
};

// 从环形缓冲里取出某通道在时间窗内的点（按时间升序）。
// 窗口为空（fromMs > toMs）时返回空 —— 不返回"全部"，那会把"没数据"伪装成"有数据"。
std::vector<CurvePoint> extractSeries(const TelemetryRing& ring, CurveChannel channel,
                                      const CurveWindow& window);

// 把点抽稀到至多 maxPoints 个。
//
// 用 **min/max 分桶**，而不是"每 N 个取一个"：后者会让一闪而过的尖峰从图上消失，
// 而尖峰恰恰是监控最该看见的东西（过速、限位、故障前的抖动）。
// 每个桶保留极小与极大，并按**时间顺序**输出（否则折线会自己打结）。
//
// 返回值大小恒 ≤ maxPoints。
std::vector<CurvePoint> decimateSeries(const std::vector<CurvePoint>& points,
                                       std::size_t maxPoints);

}  // namespace mchost
