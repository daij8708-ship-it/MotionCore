#include "mc_host/curve_model.hpp"

#include <algorithm>

namespace mchost {

const char* channelName(CurveChannel channel) noexcept {
    switch (channel) {
        case CurveChannel::Velocity: return "速度";
        case CurveChannel::Position: return "位置";
        case CurveChannel::Target:   return "目标";
    }
    return "?";
}

const char* channelUnit(CurveChannel channel) noexcept {
    switch (channel) {
        case CurveChannel::Velocity: return "deg/s";
        case CurveChannel::Position: return "deg";
        case CurveChannel::Target:   return "deg/s";
    }
    return "";
}

double channelValue(const TelemetrySample& sample, CurveChannel channel) noexcept {
    switch (channel) {
        case CurveChannel::Velocity: return static_cast<double>(sample.velocityDegPerSec);
        case CurveChannel::Position: return static_cast<double>(sample.positionDeg);
        case CurveChannel::Target:   return static_cast<double>(sample.target);
    }
    return 0.0;
}

std::vector<CurvePoint> extractSeries(const TelemetryRing& ring, CurveChannel channel,
                                      const CurveWindow& window) {
    std::vector<CurvePoint> points;
    if (ring.empty() || window.fromMs > window.toMs) {
        return points;
    }

    const auto timeAt = [&ring](std::size_t index) { return ring.at(index).tMs; };

    // 时间戳单调 → 二分找窗内第一点。不做线性扫描：窗口 1 s 而缓冲 40 s 时，
    // 扫描 4000 条里只有 100 条有用。
    std::size_t low = 0U;
    std::size_t high = ring.size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2U;
        if (timeAt(mid) < window.fromMs) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }

    for (std::size_t i = low; i < ring.size(); ++i) {
        const TelemetrySample& sample = ring.at(i);
        if (sample.tMs > window.toMs) {
            break;
        }
        points.push_back(CurvePoint{sample.tMs, channelValue(sample, channel)});
    }
    return points;
}

std::vector<CurvePoint> decimateSeries(const std::vector<CurvePoint>& points,
                                       std::size_t maxPoints) {
    if (maxPoints == 0U) {
        return {};
    }
    if (points.size() <= maxPoints) {
        // 本来就没超上限：原样返回，不做任何"聪明"的改动 ——
        // 抽稀只该在真的需要时发生，否则等于凭空引入误差。
        return points;
    }

    // 每桶最多贡献 2 个点（极小 + 极大），所以桶数最多取一半。
    const std::size_t buckets = std::max<std::size_t>(1U, maxPoints / 2U);
    const std::size_t total = points.size();

    std::vector<CurvePoint> out;
    out.reserve(maxPoints);

    for (std::size_t bucket = 0U; bucket < buckets; ++bucket) {
        const std::size_t begin = (bucket * total) / buckets;
        const std::size_t end = ((bucket + 1U) * total) / buckets;
        if (begin >= end) {
            continue;
        }

        std::size_t indexMin = begin;
        std::size_t indexMax = begin;
        for (std::size_t i = begin; i < end; ++i) {
            if (points[i].value < points[indexMin].value) {
                indexMin = i;
            }
            if (points[i].value > points[indexMax].value) {
                indexMax = i;
            }
        }

        if (indexMin <= indexMax) {
            out.push_back(points[indexMin]);
            if (indexMax != indexMin) {
                out.push_back(points[indexMax]);
            }
        } else {
            out.push_back(points[indexMax]);
            out.push_back(points[indexMin]);
        }
    }

    // buckets 取的是 maxPoints/2 的下取整，正常不会超；这里只是兜住 maxPoints 很小的边界。
    if (out.size() > maxPoints) {
        out.resize(maxPoints);
    }
    return out;
}

}  // namespace mchost
