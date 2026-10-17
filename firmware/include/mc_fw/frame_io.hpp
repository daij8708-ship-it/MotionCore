#pragma once

#include <cstddef>
#include <cstdint>

#include "mc/protocol.hpp"

namespace mcfw {

// 固件侧只接受命令载荷不超过 64 字节的帧。
//
// 现存命令里最大的是 PARAM_STATE 回读（26 字节），64 已经留足余量；
// 上限写小是为了在 20 KB RAM 的片子上给出**有界**的收帧缓冲。
// 超限的长帧在解析层就被丢弃并计数，不会把缓冲区撑爆。
inline constexpr std::size_t kMaxCommandPayload = 64U;
inline constexpr std::size_t kMaxFrameBytes =
    mc::kHeaderSize + kMaxCommandPayload + mc::kCrcSize;

// 一帧解析结果。payload 指向解析器内部缓冲，只在本次 push 返回后、
// 下一次 push 之前有效；调用方必须在这段时间内把需要的字段取走。
struct ParsedFrame {
    mc::MessageType type = mc::MessageType::Nack;
    std::uint16_t seq = 0U;
    const std::uint8_t* payload = nullptr;
    std::uint16_t payloadLen = 0U;
};

// CRC-16/CCITT-FALSE。固件侧用逐位实现：100 Hz、每帧几十字节的负载下，
// 这点运算量远小于一次串口中断的开销，换来的是不占 512 字节查表空间、
// 也不依赖运行期静态初始化。
std::uint16_t crc16CcittFalse(const std::uint8_t* data, std::size_t len) noexcept;

// 流式拆帧：粘包、拆包、坏帧重同步都在这里完成。
//
// 与宿主侧 mc::StreamDecoder 的职责边界一致：这里只回答"有没有一帧完整的、
// CRC 正确的帧"，不解释消息类型，也不做去重。
class FrameParser {
public:
    struct Stats {
        std::uint32_t framesDecoded = 0U;
        std::uint32_t crcErrors = 0U;
        std::uint32_t badLength = 0U;
        std::uint32_t badVersion = 0U;
        std::uint32_t bytesDiscarded = 0U;
    };

    // 喂入一个字节。凑满一帧且 CRC 正确时返回 true 并填 out。
    bool push(std::uint8_t byte, ParsedFrame& out) noexcept;

    void reset() noexcept;

    const Stats& stats() const noexcept { return stats_; }
    std::size_t pending() const noexcept { return len_; }

private:
    std::uint8_t buffer_[kMaxFrameBytes]{};
    // 解析成功后把载荷拷到这里，再丢弃原始帧 —— 否则调用方拿到的指针
    // 会在下一次 push 时被 memmove 擦掉。
    std::uint8_t payload_[kMaxCommandPayload]{};
    std::size_t len_ = 0U;
    Stats stats_{};
};

// 编码一帧。容量不足或载荷超限时返回 0；成功返回写入的字节数。
std::size_t encodeFrame(mc::MessageType type, std::uint16_t seq,
                        const std::uint8_t* payload, std::uint16_t payloadLen,
                        std::uint8_t* out, std::size_t outCapacity) noexcept;

}  // namespace mcfw
