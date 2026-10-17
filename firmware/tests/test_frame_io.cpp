#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mc/crc16.hpp"
#include "mc/frame_codec.hpp"
#include "mc_fw/frame_io.hpp"

namespace {

std::vector<std::uint8_t> makeFrameBytes(mc::MessageType type, std::uint16_t seq,
                                         const std::vector<std::uint8_t>& payload) {
    mc::Frame frame;
    frame.version = mc::kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;
    const mc::EncodeResult encoded = mc::encodeFrame(frame);
    EXPECT_TRUE(encoded);
    return encoded.bytes;
}

}  // namespace

TEST(FirmwareFrameIo, CrcMatchesTheHostImplementation) {
    const std::uint8_t data[] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
    EXPECT_EQ(mcfw::crc16CcittFalse(data, sizeof(data)), mc::crc16(data, sizeof(data)));
    EXPECT_EQ(mcfw::crc16CcittFalse(nullptr, 0U), mc::kCrc16Init);
}

TEST(FirmwareFrameIo, EncodesExactlyWhatTheHostDecoderExpects) {
    const std::vector<std::uint8_t> payload = {1U, 2U, 3U, 4U};
    const std::vector<std::uint8_t> bytes = makeFrameBytes(mc::MessageType::SetPid, 7U, payload);

    std::uint8_t out[mcfw::kMaxFrameBytes]{};
    const std::size_t written = mcfw::encodeFrame(mc::MessageType::SetPid, 7U,
                                                  payload.data(),
                                                  static_cast<std::uint16_t>(payload.size()),
                                                  out, sizeof(out));
    ASSERT_EQ(written, bytes.size());
    EXPECT_EQ(std::vector<std::uint8_t>(out, out + written), bytes);

    const mc::DecodeResult decoded = mc::decodeFrame(out, written);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.frame.type, mc::MessageType::SetPid);
    EXPECT_EQ(decoded.frame.seq, 7U);
    EXPECT_EQ(decoded.frame.payload, payload);
}

TEST(FirmwareFrameIo, SplitsAndMergesWithoutLosingFrames) {
    const std::vector<std::uint8_t> first = makeFrameBytes(mc::MessageType::Heartbeat, 1U, {});
    const std::vector<std::uint8_t> second = makeFrameBytes(mc::MessageType::Start, 2U, {});

    std::vector<std::uint8_t> merged = first;
    merged.insert(merged.end(), second.begin(), second.end());

    mcfw::FrameParser parser;
    std::vector<mcfw::ParsedFrame> seen;
    for (std::uint8_t byte : merged) {
        mcfw::ParsedFrame frame;
        if (parser.push(byte, frame)) {
            seen.push_back(frame);
        }
    }

    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0].type, mc::MessageType::Heartbeat);
    EXPECT_EQ(seen[0].seq, 1U);
    EXPECT_EQ(seen[1].type, mc::MessageType::Start);
    EXPECT_EQ(seen[1].seq, 2U);
}

TEST(FirmwareFrameIo, DropsACorruptedFrameAndResynchronizes) {
    std::vector<std::uint8_t> bad = makeFrameBytes(mc::MessageType::SetMode, 3U, {1U});
    bad[8] ^= 0xFFU;   // 载荷被改，CRC 必然对不上
    const std::vector<std::uint8_t> good = makeFrameBytes(mc::MessageType::Heartbeat, 4U, {});

    mcfw::FrameParser parser;
    std::vector<mcfw::ParsedFrame> seen;
    for (std::uint8_t byte : bad) {
        mcfw::ParsedFrame frame;
        if (parser.push(byte, frame)) {
            seen.push_back(frame);
        }
    }
    for (std::uint8_t byte : good) {
        mcfw::ParsedFrame frame;
        if (parser.push(byte, frame)) {
            seen.push_back(frame);
        }
    }

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].type, mc::MessageType::Heartbeat);
    EXPECT_EQ(seen[0].seq, 4U);
    EXPECT_GE(parser.stats().crcErrors, 1U);
}

TEST(FirmwareFrameIo, RefusesAnOversizePayloadAndKeepsLooking) {
    const std::vector<std::uint8_t> oversize(mcfw::kMaxCommandPayload + 1U, 0x5AU);
    const std::vector<std::uint8_t> bad = makeFrameBytes(mc::MessageType::Telemetry, 5U, oversize);
    const std::vector<std::uint8_t> good = makeFrameBytes(mc::MessageType::Heartbeat, 6U, {});

    mcfw::FrameParser parser;
    std::vector<mcfw::ParsedFrame> seen;
    for (std::uint8_t byte : bad) {
        mcfw::ParsedFrame frame;
        if (parser.push(byte, frame)) {
            seen.push_back(frame);
        }
    }
    for (std::uint8_t byte : good) {
        mcfw::ParsedFrame frame;
        if (parser.push(byte, frame)) {
            seen.push_back(frame);
        }
    }

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].type, mc::MessageType::Heartbeat);
    EXPECT_GE(parser.stats().badLength, 1U);
}

TEST(FirmwareFrameIo, RefusesAVersionMismatch) {
    mc::Frame frame;
    frame.type = mc::MessageType::Heartbeat;
    frame.seq = 9U;
    const mc::EncodeResult encoded = mc::encodeFrame(frame);
    ASSERT_TRUE(encoded);

    // 宿主编码器会拒绝写一个自己都不支持的版本，所以先在合法帧上
    // 把版本字节改掉。解析器必须在看 CRC 之前就按"整帧跳过"处理它。
    std::vector<std::uint8_t> bytes = encoded.bytes;
    bytes[mc::kOffsetVersion] = static_cast<std::uint8_t>(mc::kProtocolVersion + 1U);

    mcfw::FrameParser parser;
    bool decoded = false;
    for (std::uint8_t byte : bytes) {
        mcfw::ParsedFrame parsed;
        decoded = parser.push(byte, parsed) || decoded;
    }

    EXPECT_FALSE(decoded);
    EXPECT_GE(parser.stats().badVersion, 1U);
}
