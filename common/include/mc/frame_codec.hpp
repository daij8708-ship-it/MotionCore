#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 单帧编码
// ---------------------------------------------------------------------------
// 失败原因放在 EncodeResult::error 里；失败时 bytes 为空。
// 不做"超长就静默截断"这种事 —— 载荷超限必须是一个显式错误。
EncodeResult encodeFrame(const Frame& frame);

// ---------------------------------------------------------------------------
// 单帧解码（输入缓冲区必须以帧首 SOF 开始）
// ---------------------------------------------------------------------------
// 用于测试与"已经确定帧边界"的场合。流式场景请用 StreamDecoder。
DecodeResult decodeFrame(const std::uint8_t* data, std::size_t len);

// ---------------------------------------------------------------------------
// 流式解码器：字节流 → 帧
//
// 职责：
//   - 粘包/拆包：任意切分方式喂入，产出的帧序列必须一致
//   - 重同步：SOF 丢失或坏帧后，向前找到下一个 SOF 继续
//   - 坏帧丢弃：CRC 错 / 长度超限的帧不产出，且计入统计
//   - 有界内存：内部暂存区不会超过 kMaxFrameSize + 单次喂入量
//
// 不负责的事（归属应用层）：
//   - 未知 TYPE 的处置（这里原样透传，由应用层回 NAK）
//   - 序列号去重、超时判定、重传
// ---------------------------------------------------------------------------
class StreamDecoder {
public:
    struct Stats {
        std::uint64_t framesDecoded = 0;
        std::uint64_t framesCrcError = 0;
        std::uint64_t framesBadLength = 0;
        std::uint64_t framesBadVersion = 0;
        std::uint64_t bytesDiscarded = 0;   // 重同步过程中丢弃的字节数
    };

    // 喂入一段数据，返回本次能解析出的全部完整帧（可能为空）。
    std::vector<Frame> feed(const std::uint8_t* data, std::size_t len);

    // 单字节喂入。与整块喂入必须产出相同结果。
    std::vector<Frame> feedByte(std::uint8_t byte);

    const Stats& stats() const noexcept { return stats_; }

    // 当前暂存区里尚未构成完整帧的字节数。
    std::size_t pending() const noexcept { return buffer_.size(); }

    // 清空暂存区与统计量，回到初始状态。
    void reset();

private:
    std::vector<std::uint8_t> buffer_;
    Stats stats_{};
};

}  // namespace mc
