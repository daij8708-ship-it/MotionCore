#include "mc/frame_codec.hpp"

#include <utility>

#include "mc/crc16.hpp"

namespace mc {
namespace {

// CRC 覆盖 VER..PAYLOAD，因此长度 = (头部长度 - VER 偏移) + 载荷长度。
constexpr std::size_t crcRangeLength(std::size_t payloadLen) noexcept {
    return kHeaderSize - kOffsetVersion + payloadLen;
}

}  // namespace

EncodeResult encodeFrame(const Frame& frame) {
    EncodeResult result;

    if (frame.version != kProtocolVersion) {
        result.error = EncodeError::UnsupportedVersion;
        return result;
    }
    if (frame.payload.size() > kMaxPayloadSize) {
        result.error = EncodeError::PayloadTooLarge;
        return result;
    }

    const std::size_t payloadLen = frame.payload.size();
    const std::size_t totalSize = kHeaderSize + payloadLen + kCrcSize;

    std::vector<std::uint8_t> out(totalSize, 0U);
    out[kOffsetSof] = kSof0;
    out[kOffsetSof + 1U] = kSof1;
    out[kOffsetVersion] = frame.version;
    out[kOffsetType] = static_cast<std::uint8_t>(frame.type);
    writeLe16(&out[kOffsetSeq], frame.seq);
    writeLe16(&out[kOffsetLen], static_cast<std::uint16_t>(payloadLen));
    for (std::size_t i = 0U; i < payloadLen; ++i) {
        out[kHeaderSize + i] = frame.payload[i];
    }

    const std::uint16_t crc = crc16(&out[kOffsetVersion], crcRangeLength(payloadLen));
    writeLe16(&out[kHeaderSize + payloadLen], crc);

    result.bytes = std::move(out);
    return result;
}

DecodeResult decodeFrame(const std::uint8_t* data, std::size_t len) {
    DecodeResult result;

    if (data == nullptr || len < kMinFrameSize) {
        result.error = DecodeError::BufferTooShort;
        return result;
    }
    if (data[kOffsetSof] != kSof0 || data[kOffsetSof + 1U] != kSof1) {
        result.error = DecodeError::BadSof;
        return result;
    }

    const std::uint16_t payloadLen = readLe16(&data[kOffsetLen]);
    if (payloadLen > kMaxPayloadSize) {
        result.error = DecodeError::PayloadTooLarge;
        return result;
    }

    const std::size_t frameSize = kHeaderSize + payloadLen + kCrcSize;
    if (len < frameSize) {
        result.error = DecodeError::BufferTooShort;
        return result;
    }

    // 先校验完整性，再看语义（版本）。
    const std::uint16_t crcCalculated = crc16(&data[kOffsetVersion], crcRangeLength(payloadLen));
    const std::uint16_t crcReceived = readLe16(&data[kHeaderSize + payloadLen]);
    if (crcCalculated != crcReceived) {
        result.error = DecodeError::CrcMismatch;
        return result;
    }
    if (data[kOffsetVersion] != kProtocolVersion) {
        result.error = DecodeError::UnsupportedVersion;
        return result;
    }

    result.frame.version = data[kOffsetVersion];
    result.frame.type = static_cast<MessageType>(data[kOffsetType]);
    result.frame.seq = readLe16(&data[kOffsetSeq]);
    result.frame.payload.assign(data + kHeaderSize, data + kHeaderSize + payloadLen);
    result.consumed = frameSize;
    return result;
}

std::vector<Frame> StreamDecoder::feed(const std::uint8_t* data, std::size_t len) {
    if (data != nullptr && len > 0U) {
        buffer_.insert(buffer_.end(), data, data + len);
    }

    std::vector<Frame> frames;
    std::size_t pos = 0U;

    while (true) {
        const std::size_t available = buffer_.size() - pos;

        // (1) 定位 SOF。不足 2 字节无法判断，保留待续。
        if (available < 2U) {
            break;
        }
        if (buffer_[pos] != kSof0 || buffer_[pos + 1U] != kSof1) {
            ++pos;
            ++stats_.bytesDiscarded;
            continue;
        }

        // (2) 头部到齐才能读出 LEN。
        if (available < kHeaderSize) {
            break;
        }
        const std::uint16_t payloadLen = readLe16(&buffer_[pos + kOffsetLen]);
        if (payloadLen > kMaxPayloadSize) {
            // 拒绝而不分配：此处直接放弃这一字节，绝不按 LEN 去申请内存。
            ++stats_.framesBadLength;
            ++pos;
            ++stats_.bytesDiscarded;
            continue;
        }

        // (3) 整帧到齐才能校验 CRC。
        const std::size_t frameSize = kHeaderSize + payloadLen + kCrcSize;
        if (available < frameSize) {
            break;
        }

        const std::uint8_t* p = &buffer_[pos];
        const std::uint16_t crcCalculated = crc16(p + kOffsetVersion, crcRangeLength(payloadLen));
        const std::uint16_t crcReceived = readLe16(p + kHeaderSize + payloadLen);
        if (crcCalculated != crcReceived) {
            // 坏帧：可能只是字节错位，逐字节向前重同步。
            ++stats_.framesCrcError;
            ++pos;
            ++stats_.bytesDiscarded;
            continue;
        }
        if (p[kOffsetVersion] != kProtocolVersion) {
            // CRC 通过 ⇒ 这确实是一帧，只是版本不兼容，整帧跳过。
            ++stats_.framesBadVersion;
            pos += frameSize;
            continue;
        }

        Frame frame;
        frame.version = p[kOffsetVersion];
        frame.type = static_cast<MessageType>(p[kOffsetType]);
        frame.seq = readLe16(p + kOffsetSeq);
        frame.payload.assign(p + kHeaderSize, p + kHeaderSize + payloadLen);
        frames.push_back(std::move(frame));
        ++stats_.framesDecoded;
        pos += frameSize;
    }

    if (pos > 0U) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    return frames;
}

std::vector<Frame> StreamDecoder::feedByte(std::uint8_t byte) {
    return feed(&byte, 1U);
}

void StreamDecoder::reset() {
    buffer_.clear();
    stats_ = Stats{};
}

}  // namespace mc
