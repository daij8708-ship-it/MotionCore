#include "mc_fw/frame_io.hpp"

#include <cstring>

#include "mc/crc16.hpp"

namespace mcfw {
namespace {

constexpr std::size_t kNoSof = static_cast<std::size_t>(-1);

}  // namespace

std::uint16_t crc16CcittFalse(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint16_t crc = mc::kCrc16Init;
    if (data == nullptr) {
        return crc;
    }
    for (std::size_t i = 0; i < len; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ static_cast<std::uint16_t>(data[i] << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ mc::kCrc16Poly);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

void FrameParser::reset() noexcept {
    len_ = 0U;
    stats_ = Stats{};
}

bool FrameParser::push(std::uint8_t byte, ParsedFrame& out) noexcept {
    // 正常路径下不会走到这里：最长的合法帧正好填满 buffer_。
    // 保底丢掉最老的一个字节，宁可重同步也不能越界写。
    if (len_ >= sizeof(buffer_)) {
        std::memmove(buffer_, buffer_ + 1, len_ - 1U);
        --len_;
        ++stats_.bytesDiscarded;
    }
    buffer_[len_++] = byte;

    for (;;) {
        if (len_ < 2U) {
            return false;
        }

        std::size_t sof = kNoSof;
        for (std::size_t i = 0; i + 1U < len_; ++i) {
            if (buffer_[i] == mc::kSof0 && buffer_[i + 1U] == mc::kSof1) {
                sof = i;
                break;
            }
        }

        if (sof == kNoSof) {
            // 末尾可能正好是 SOF 的前半个字节，留着等下一拍。
            const std::size_t keep = (buffer_[len_ - 1U] == mc::kSof0) ? 1U : 0U;
            stats_.bytesDiscarded += static_cast<std::uint32_t>(len_ - keep);
            if (keep != 0U) {
                buffer_[0] = buffer_[len_ - 1U];
            }
            len_ = keep;
            return false;
        }

        if (sof > 0U) {
            std::memmove(buffer_, buffer_ + sof, len_ - sof);
            stats_.bytesDiscarded += static_cast<std::uint32_t>(sof);
            len_ -= sof;
        }

        if (len_ < mc::kHeaderSize) {
            return false;
        }

        const std::uint16_t payloadLen = mc::readLe16(buffer_ + mc::kOffsetLen);
        if (payloadLen > kMaxCommandPayload) {
            ++stats_.badLength;
            std::memmove(buffer_, buffer_ + 1, len_ - 1U);
            --len_;
            ++stats_.bytesDiscarded;
            continue;
        }

        const std::size_t total = mc::kHeaderSize + payloadLen + mc::kCrcSize;
        if (len_ < total) {
            return false;
        }

        if (buffer_[mc::kOffsetVersion] != mc::kProtocolVersion) {
            ++stats_.badVersion;
            std::memmove(buffer_, buffer_ + total, len_ - total);
            len_ -= total;
            continue;
        }

        const std::uint16_t expected =
            mc::readLe16(buffer_ + mc::kHeaderSize + payloadLen);
        const std::uint16_t actual = crc16CcittFalse(
            buffer_ + mc::kOffsetVersion,
            mc::kHeaderSize - mc::kOffsetVersion + payloadLen);
        if (actual != expected) {
            ++stats_.crcErrors;
            std::memmove(buffer_, buffer_ + 1, len_ - 1U);
            --len_;
            ++stats_.bytesDiscarded;
            continue;
        }

        std::memcpy(payload_, buffer_ + mc::kHeaderSize, payloadLen);
        out.type = static_cast<mc::MessageType>(buffer_[mc::kOffsetType]);
        out.seq = mc::readLe16(buffer_ + mc::kOffsetSeq);
        out.payload = payload_;
        out.payloadLen = payloadLen;
        ++stats_.framesDecoded;

        std::memmove(buffer_, buffer_ + total, len_ - total);
        len_ -= total;
        return true;
    }
}

std::size_t encodeFrame(mc::MessageType type, std::uint16_t seq,
                        const std::uint8_t* payload, std::uint16_t payloadLen,
                        std::uint8_t* out, std::size_t outCapacity) noexcept {
    if (out == nullptr || payloadLen > kMaxCommandPayload) {
        return 0U;
    }
    if (payloadLen > 0U && payload == nullptr) {
        return 0U;
    }

    const std::size_t total = mc::kHeaderSize + payloadLen + mc::kCrcSize;
    if (outCapacity < total) {
        return 0U;
    }

    out[0] = mc::kSof0;
    out[1] = mc::kSof1;
    out[mc::kOffsetVersion] = mc::kProtocolVersion;
    out[mc::kOffsetType] = static_cast<std::uint8_t>(type);
    mc::writeLe16(out + mc::kOffsetSeq, seq);
    mc::writeLe16(out + mc::kOffsetLen, payloadLen);
    if (payloadLen > 0U) {
        std::memcpy(out + mc::kHeaderSize, payload, payloadLen);
    }
    const std::uint16_t crc = crc16CcittFalse(
        out + mc::kOffsetVersion, mc::kHeaderSize - mc::kOffsetVersion + payloadLen);
    mc::writeLe16(out + mc::kHeaderSize + payloadLen, crc);
    return total;
}

}  // namespace mcfw
