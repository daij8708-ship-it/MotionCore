#pragma once

#include <cstddef>
#include <cstdint>

#include "mc/ring_buffer.hpp"

namespace mchost {

// 一条遥测样本。
//
// tMs 用**上位机收到它的时刻**，不是设备时间戳：两端时钟不同源（设备跑的是
// 上电后的毫秒数），混用会把曲线画歪。设备时间戳另有用途，要显示就单独显示。
struct TelemetrySample {
    double tMs = 0.0;
    float velocityDegPerSec = 0.0F;
    float positionDeg = 0.0F;
    float target = 0.0F;
    std::uint8_t state = 0U;
    std::uint8_t faultCode = 0U;
    std::uint16_t pwmOutput = 0U;
};

// 层 5（数据）：实时遥测缓存。
//
// 复用 mc::RingBuffer 而不是手写一个 —— 它已经有用例钉住"覆盖计数精确""持续写入有界"。
// 语义是**允许丢**：显示跟不上时宁可丢最旧的，也不能无界增长。
// （协议解析用的 StreamDecoder 暂存区则不允许丢，两者刻意不同。）
class TelemetryRing {
public:
    // 4096 条 ≈ 40 s @100 Hz；单条约 28 B → 约 115 KB，恒定不增长。
    static constexpr std::size_t kCapacity = 4096U;

    void push(const TelemetrySample& sample) { ring_.push(sample); }

    std::size_t size() const noexcept { return ring_.size(); }
    bool empty() const noexcept { return ring_.empty(); }
    std::uint64_t discarded() const noexcept { return ring_.discarded(); }

    // index == 0 取最旧。时间戳单调递增，因此按时间二分查找是合法的。
    const TelemetrySample& at(std::size_t index) const { return ring_.at(index); }

    // 只清元素，不清"因覆盖而丢弃"的累计计数。
    void clear() noexcept { ring_.clear(); }

private:
    mc::RingBuffer<TelemetrySample, kCapacity> ring_{};
};

}  // namespace mchost
