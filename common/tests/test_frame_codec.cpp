#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <random>
#include <utility>
#include <vector>

#include "mc/crc16.hpp"
#include "mc/frame_codec.hpp"
#include "mc/protocol.hpp"

namespace {

using mc::DecodeError;
using mc::EncodeError;
using mc::Frame;
using mc::MessageType;
using mc::StreamDecoder;

constexpr MessageType kAllTypes[] = {
    MessageType::Hello,       MessageType::Heartbeat,     MessageType::SetMode,
    MessageType::Start,       MessageType::Stop,          MessageType::SetTarget,
    MessageType::SetPid,      MessageType::Telemetry,     MessageType::FaultEvent,
    MessageType::Nack,        MessageType::HelloAck,      MessageType::HeartbeatAck,
    MessageType::SetModeAck,  MessageType::StartAck,      MessageType::SetTargetAck,
    MessageType::SetPidAck,
};

std::vector<std::uint8_t> randomBytes(std::mt19937& rng, std::size_t len) {
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<std::uint8_t> out(len);
    for (std::uint8_t& byte : out) {
        byte = static_cast<std::uint8_t>(dist(rng));
    }
    return out;
}

Frame makeFrame(MessageType type, std::uint16_t seq, std::vector<std::uint8_t> payload) {
    Frame frame;
    frame.version = mc::kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = std::move(payload);
    return frame;
}

bool sameFrame(const Frame& a, const Frame& b) {
    return a.version == b.version && a.type == b.type && a.seq == b.seq && a.payload == b.payload;
}

std::vector<std::uint8_t> encodeOrDie(const Frame& frame) {
    mc::EncodeResult result = mc::encodeFrame(frame);
    EXPECT_TRUE(result) << "encode 失败：" << static_cast<int>(result.error);
    return result.bytes;
}

std::vector<std::uint8_t> concatenate(const std::vector<Frame>& frames) {
    std::vector<std::uint8_t> blob;
    for (const Frame& frame : frames) {
        const std::vector<std::uint8_t> bytes = encodeOrDie(frame);
        blob.insert(blob.end(), bytes.begin(), bytes.end());
    }
    return blob;
}

std::vector<Frame> makeFrameSet(std::mt19937& rng, std::size_t count) {
    std::uniform_int_distribution<std::size_t> typeDist(0, std::size(kAllTypes) - 1U);
    std::uniform_int_distribution<int> seqDist(0, 65535);
    std::uniform_int_distribution<std::size_t> lenDist(0, 64);

    std::vector<Frame> frames;
    frames.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        const MessageType type = kAllTypes[typeDist(rng)];
        const std::uint16_t seq = static_cast<std::uint16_t>(seqDist(rng));
        frames.push_back(makeFrame(type, seq, randomBytes(rng, lenDist(rng))));
    }
    return frames;
}

// 构造一帧但强制使用指定的版本号（用于模拟"对端说的是别的协议版本"）。
// CRC 仍然是正确的 —— 这样测的是版本分支而不是 CRC 分支。
std::vector<std::uint8_t> encodeWithVersion(const Frame& frame, std::uint8_t version) {
    std::vector<std::uint8_t> bytes = encodeOrDie(frame);
    bytes[mc::kOffsetVersion] = version;
    const std::uint16_t crc =
        mc::crc16(bytes.data() + mc::kOffsetVersion, mc::kHeaderSize - mc::kOffsetVersion + frame.payload.size());
    mc::writeLe16(bytes.data() + mc::kHeaderSize + frame.payload.size(), crc);
    return bytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// 单帧编码 / 解码
// ---------------------------------------------------------------------------

TEST(FrameCodec, EncodedLayoutMatchesSpec) {
    // 本用例钉住的是"线序"：偏移、小端、SOF。
    // CRC 数值本身的正确性由 test_crc16.cpp 的公开标准向量锚定，不在这里自证。
    const Frame frame = makeFrame(MessageType::SetTarget, 0x1234U, {0xDEU, 0xADU});
    const std::vector<std::uint8_t> bytes = encodeOrDie(frame);

    ASSERT_EQ(bytes.size(), std::size_t{12});   // 8 头部 + 2 载荷 + 2 CRC
    EXPECT_EQ(bytes[mc::kOffsetSof], mc::kSof0);
    EXPECT_EQ(bytes[mc::kOffsetSof + 1U], mc::kSof1);
    EXPECT_EQ(bytes[mc::kOffsetVersion], mc::kProtocolVersion);
    EXPECT_EQ(bytes[mc::kOffsetType], static_cast<std::uint8_t>(MessageType::SetTarget));
    EXPECT_EQ(mc::readLe16(bytes.data() + mc::kOffsetSeq), 0x1234U);
    EXPECT_EQ(mc::readLe16(bytes.data() + mc::kOffsetLen), 0x0002U);
    EXPECT_EQ(bytes[mc::kHeaderSize + 0U], 0xDEU);
    EXPECT_EQ(bytes[mc::kHeaderSize + 1U], 0xADU);

    // CRC 字段出现在正确的位置，并且解码时被当作小端读回。
    const std::uint16_t crcField = mc::readLe16(bytes.data() + mc::kHeaderSize + 2U);
    EXPECT_EQ(crcField, mc::crc16(bytes.data() + mc::kOffsetVersion, 8U));

    const mc::DecodeResult decoded = mc::decodeFrame(bytes.data(), bytes.size());
    ASSERT_TRUE(decoded) << "decode 失败：" << static_cast<int>(decoded.error);
    EXPECT_EQ(decoded.consumed, std::size_t{12});
    EXPECT_TRUE(sameFrame(decoded.frame, frame));
}

TEST(FrameCodec, EncodeRejectsOversizePayload) {
    const Frame frame = makeFrame(MessageType::Telemetry, 1U,
                                  std::vector<std::uint8_t>(mc::kMaxPayloadSize + 1U, 0x00U));
    const mc::EncodeResult result = mc::encodeFrame(frame);

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, EncodeError::PayloadTooLarge);
    EXPECT_TRUE(result.bytes.empty());
}

TEST(FrameCodec, EncodeRejectsUnsupportedVersion) {
    Frame frame = makeFrame(MessageType::Telemetry, 1U, {});
    frame.version = 0x02U;

    const mc::EncodeResult result = mc::encodeFrame(frame);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, EncodeError::UnsupportedVersion);
}

TEST(FrameCodec, DecodeRejectsBadSof) {
    std::vector<std::uint8_t> bytes = encodeOrDie(makeFrame(MessageType::Telemetry, 1U, {}));
    bytes[mc::kOffsetSof] = 0x00U;

    const mc::DecodeResult result = mc::decodeFrame(bytes.data(), bytes.size());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, DecodeError::BadSof);
}

TEST(FrameCodec, DecodeRejectsTruncatedBuffer) {
    const std::vector<std::uint8_t> bytes = encodeOrDie(makeFrame(MessageType::SetPid, 7U, {0x01U, 0x02U, 0x03U}));

    for (std::size_t len = 0U; len < bytes.size(); ++len) {
        const mc::DecodeResult result = mc::decodeFrame(bytes.data(), len);
        EXPECT_FALSE(result) << "len=" << len;
        EXPECT_EQ(result.error, DecodeError::BufferTooShort) << "len=" << len;
    }
}

TEST(FrameCodec, DecodeRejectsCorruptedCrc) {
    std::vector<std::uint8_t> bytes = encodeOrDie(makeFrame(MessageType::SetPid, 7U, {0x01U, 0x02U, 0x03U}));
    bytes[mc::kHeaderSize + 1U] ^= 0x01U;   // 翻转载荷中的一个比特

    const mc::DecodeResult result = mc::decodeFrame(bytes.data(), bytes.size());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, DecodeError::CrcMismatch);
}

TEST(FrameCodec, RoundTripEmptyAndMaxPayload) {
    const std::vector<std::size_t> sizes = {0U, 1U, mc::kMaxPayloadSize};
    std::mt19937 rng(0x1234U);

    for (const std::size_t size : sizes) {
        const Frame frame = makeFrame(MessageType::Telemetry, 42U, randomBytes(rng, size));
        const std::vector<std::uint8_t> bytes = encodeOrDie(frame);

        EXPECT_EQ(bytes.size(), mc::kHeaderSize + size + mc::kCrcSize) << "size=" << size;

        const mc::DecodeResult decoded = mc::decodeFrame(bytes.data(), bytes.size());
        ASSERT_TRUE(decoded) << "size=" << size;
        EXPECT_TRUE(sameFrame(decoded.frame, frame)) << "size=" << size;
    }
}

TEST(FrameCodec, RoundTripRandomFrames) {
    std::mt19937 rng(0xABCDEFU);
    const std::vector<Frame> frames = makeFrameSet(rng, 1000U);

    for (std::size_t i = 0U; i < frames.size(); ++i) {
        const std::vector<std::uint8_t> bytes = encodeOrDie(frames[i]);
        const mc::DecodeResult decoded = mc::decodeFrame(bytes.data(), bytes.size());

        ASSERT_TRUE(decoded) << "i=" << i;
        ASSERT_TRUE(sameFrame(decoded.frame, frames[i])) << "i=" << i;
    }
}

// ---------------------------------------------------------------------------
// 流式解码：粘包 / 拆包 / 重同步 / 坏帧
// ---------------------------------------------------------------------------

TEST(StreamDecoderTest, WholeFeedEqualsBytewiseFeed) {
    std::mt19937 rng(0xFEEDU);
    const std::vector<Frame> expected = makeFrameSet(rng, 1000U);
    const std::vector<std::uint8_t> blob = concatenate(expected);

    StreamDecoder wholeDecoder;
    const std::vector<Frame> whole = wholeDecoder.feed(blob.data(), blob.size());

    StreamDecoder byteDecoder;
    std::vector<Frame> bytewise;
    for (const std::uint8_t byte : blob) {
        const std::vector<Frame> part = byteDecoder.feedByte(byte);
        bytewise.insert(bytewise.end(), part.begin(), part.end());
    }

    ASSERT_EQ(whole.size(), expected.size());
    ASSERT_EQ(bytewise.size(), expected.size());
    for (std::size_t i = 0U; i < expected.size(); ++i) {
        EXPECT_TRUE(sameFrame(whole[i], expected[i])) << "whole i=" << i;
        EXPECT_TRUE(sameFrame(bytewise[i], expected[i])) << "bytewise i=" << i;
    }

    EXPECT_EQ(wholeDecoder.stats().framesDecoded, 1000ULL);
    EXPECT_EQ(byteDecoder.stats().framesDecoded, 1000ULL);
    EXPECT_EQ(wholeDecoder.pending(), std::size_t{0});
    EXPECT_EQ(byteDecoder.pending(), std::size_t{0});
}

TEST(StreamDecoderTest, RandomChunkingEqualsWholeFeed) {
    std::mt19937 rng(0x0DDBA11U);
    const std::vector<Frame> expected = makeFrameSet(rng, 500U);
    const std::vector<std::uint8_t> blob = concatenate(expected);

    StreamDecoder decoder;
    std::vector<Frame> produced;
    std::uniform_int_distribution<std::size_t> chunkDist(1U, 7U);

    std::size_t offset = 0U;
    while (offset < blob.size()) {
        const std::size_t chunk = std::min(chunkDist(rng), blob.size() - offset);
        const std::vector<Frame> part = decoder.feed(blob.data() + offset, chunk);
        produced.insert(produced.end(), part.begin(), part.end());
        offset += chunk;
    }

    ASSERT_EQ(produced.size(), expected.size());
    for (std::size_t i = 0U; i < expected.size(); ++i) {
        EXPECT_TRUE(sameFrame(produced[i], expected[i])) << "i=" << i;
    }
    EXPECT_EQ(decoder.pending(), std::size_t{0});
}

TEST(StreamDecoderTest, IncompleteFrameIsHeldUntilCompleted) {
    const Frame frame = makeFrame(MessageType::SetTarget, 5U, {0x01U, 0x02U, 0x03U, 0x04U});
    const std::vector<std::uint8_t> bytes = encodeOrDie(frame);
    const std::size_t half = bytes.size() / 2U;

    StreamDecoder decoder;

    const std::vector<Frame> firstHalf = decoder.feed(bytes.data(), half);
    EXPECT_TRUE(firstHalf.empty());
    EXPECT_EQ(decoder.pending(), half);

    const std::vector<Frame> secondHalf = decoder.feed(bytes.data() + half, bytes.size() - half);
    ASSERT_EQ(secondHalf.size(), std::size_t{1});
    EXPECT_TRUE(sameFrame(secondHalf[0], frame));
    EXPECT_EQ(decoder.pending(), std::size_t{0});
}

TEST(StreamDecoderTest, CrcCorruptedFrameIsDroppedAndDecoderKeepsWorking) {
    const Frame good = makeFrame(MessageType::Telemetry, 9U, {0xAAU, 0xBBU, 0xCCU, 0xDDU});
    std::vector<std::uint8_t> corrupted = encodeOrDie(good);
    corrupted[mc::kHeaderSize] ^= 0x40U;   // 载荷比特翻转 → CRC 必然不符

    StreamDecoder decoder;

    // 负向：坏帧不产出任何帧。
    const std::vector<Frame> fromBad = decoder.feed(corrupted.data(), corrupted.size());
    EXPECT_TRUE(fromBad.empty());
    EXPECT_EQ(decoder.stats().framesCrcError, 1ULL);
    EXPECT_EQ(decoder.stats().framesDecoded, 0ULL);

    // 正向对照：同一解码器紧接着仍能正确解析一个好帧。
    // （没有这一条，"不产出任何帧"的断言可能只是因为解码器压根不工作。）
    const std::vector<std::uint8_t> goodBytes = encodeOrDie(good);
    const std::vector<Frame> fromGood = decoder.feed(goodBytes.data(), goodBytes.size());
    ASSERT_EQ(fromGood.size(), std::size_t{1});
    EXPECT_TRUE(sameFrame(fromGood[0], good));
    EXPECT_EQ(decoder.stats().framesDecoded, 1ULL);
}

TEST(StreamDecoderTest, OversizeLengthIsRejectedWithoutBuffering) {
    constexpr std::size_t kHeaderCount = 100U;

    // 100 个"声称 LEN = 8192"的假头部（超过 kMaxPayloadSize = 1024）。
    std::vector<std::uint8_t> evil;
    for (std::size_t i = 0U; i < kHeaderCount; ++i) {
        const std::vector<std::uint8_t> header = {mc::kSof0, mc::kSof1, mc::kProtocolVersion,
                                                  static_cast<std::uint8_t>(MessageType::Telemetry),
                                                  0x00U, 0x00U, 0x00U, 0x20U};
        evil.insert(evil.end(), header.begin(), header.end());
    }

    StreamDecoder decoder;
    const std::vector<Frame> produced = decoder.feed(evil.data(), evil.size());

    EXPECT_TRUE(produced.empty());
    EXPECT_EQ(decoder.stats().framesBadLength, static_cast<std::uint64_t>(kHeaderCount));
    EXPECT_EQ(decoder.stats().framesDecoded, 0ULL);

    // 关键断言：暂存区没有按 LEN 增长 —— 证明"拒绝"发生在分配之前。
    EXPECT_LE(decoder.pending(), mc::kHeaderSize);

    // 正向对照：随后仍是可用状态。
    const Frame good = makeFrame(MessageType::SetTarget, 3U, {0x11U});
    const std::vector<std::uint8_t> goodBytes = encodeOrDie(good);
    const std::vector<Frame> after = decoder.feed(goodBytes.data(), goodBytes.size());
    ASSERT_EQ(after.size(), std::size_t{1});
    EXPECT_TRUE(sameFrame(after[0], good));
}

TEST(StreamDecoderTest, GarbagePrefixIsCountedAndResynced) {
    constexpr std::size_t kGarbageLen = 37U;

    // 刻意避开 0xAA / 0x55，使"丢弃字节数"可以精确断言。
    std::vector<std::uint8_t> garbage(kGarbageLen, 0x10U);
    for (std::size_t i = 0U; i < kGarbageLen; ++i) {
        garbage[i] = static_cast<std::uint8_t>(0x10U + (i % 0x40U));
    }

    const Frame good = makeFrame(MessageType::Start, 0x0001U, {0x42U});
    const std::vector<std::uint8_t> goodBytes = encodeOrDie(good);

    std::vector<std::uint8_t> stream = garbage;
    stream.insert(stream.end(), goodBytes.begin(), goodBytes.end());

    StreamDecoder decoder;
    const std::vector<Frame> produced = decoder.feed(stream.data(), stream.size());

    ASSERT_EQ(produced.size(), std::size_t{1});
    EXPECT_TRUE(sameFrame(produced[0], good));
    EXPECT_EQ(decoder.stats().bytesDiscarded, static_cast<std::uint64_t>(kGarbageLen));
    EXPECT_EQ(decoder.pending(), std::size_t{0});
}

TEST(StreamDecoderTest, UnknownTypeIsPassedThroughToApplicationLayer) {
    constexpr std::uint8_t kUnknownType = 0x5AU;
    Frame frame = makeFrame(MessageType::Telemetry, 1U, {0x01U});
    frame.type = static_cast<MessageType>(kUnknownType);

    const std::vector<std::uint8_t> bytes = encodeOrDie(frame);

    StreamDecoder decoder;
    const std::vector<Frame> produced = decoder.feed(bytes.data(), bytes.size());

    // 编解码层不做类型白名单 —— 未知类型要交给应用层回 NAK。
    ASSERT_EQ(produced.size(), std::size_t{1});
    EXPECT_EQ(static_cast<std::uint8_t>(produced[0].type), kUnknownType);

    // 负向对照：同一探针（isKnown）对这种类型确实会给出"未知"。
    EXPECT_FALSE(mc::isKnown(static_cast<MessageType>(kUnknownType)));
    EXPECT_TRUE(mc::isKnown(MessageType::Telemetry));
}

TEST(StreamDecoderTest, VersionMismatchSkipsWholeFrameWithoutResync) {
    const Frame frame = makeFrame(MessageType::Telemetry, 2U, {0xAAU, 0xBBU});
    const std::vector<std::uint8_t> bytes = encodeWithVersion(frame, 0x02U);

    StreamDecoder decoder;
    const std::vector<Frame> produced = decoder.feed(bytes.data(), bytes.size());

    EXPECT_TRUE(produced.empty());
    EXPECT_EQ(decoder.stats().framesBadVersion, 1ULL);
    EXPECT_EQ(decoder.stats().framesCrcError, 0ULL);
    // CRC 通过 ⇒ 认得出这是一整帧，不需要逐字节重同步。
    EXPECT_EQ(decoder.stats().bytesDiscarded, 0ULL);
    EXPECT_EQ(decoder.pending(), std::size_t{0});
}

TEST(StreamDecoderTest, ResetClearsPendingAndStats) {
    const Frame frame = makeFrame(MessageType::Telemetry, 1U, {0x01U, 0x02U});
    const std::vector<std::uint8_t> bytes = encodeOrDie(frame);

    StreamDecoder decoder;
    decoder.feed(bytes.data(), bytes.size() / 2U);
    EXPECT_GT(decoder.pending(), std::size_t{0});

    decoder.reset();

    EXPECT_EQ(decoder.pending(), std::size_t{0});
    EXPECT_EQ(decoder.stats().framesDecoded, 0ULL);
    EXPECT_EQ(decoder.stats().bytesDiscarded, 0ULL);

    // 复位后仍能正常解析整帧。
    const std::vector<Frame> produced = decoder.feed(bytes.data(), bytes.size());
    ASSERT_EQ(produced.size(), std::size_t{1});
    EXPECT_TRUE(sameFrame(produced[0], frame));
}

// ---------------------------------------------------------------------------
// 应答与重传策略（ADR-003 / ADR-004 / ADR-005 的可执行表述）
// ---------------------------------------------------------------------------

TEST(ProtocolPolicy, StopIsSingleDirectionalAndNeverRetried) {
    EXPECT_FALSE(mc::requiresAck(MessageType::Stop));
    EXPECT_FALSE(mc::isRetryable(MessageType::Stop));
}

TEST(ProtocolPolicy, MotionCommandsNeedAckButAreNeverAutoRetried) {
    EXPECT_TRUE(mc::requiresAck(MessageType::Start));
    EXPECT_TRUE(mc::requiresAck(MessageType::SetTarget));
    EXPECT_FALSE(mc::isRetryable(MessageType::Start));
    EXPECT_FALSE(mc::isRetryable(MessageType::SetTarget));
}

TEST(ProtocolPolicy, IdempotentCommandsAreRetryable) {
    EXPECT_TRUE(mc::isRetryable(MessageType::SetPid));
    EXPECT_TRUE(mc::isRetryable(MessageType::SetMode));
    EXPECT_TRUE(mc::isRetryable(MessageType::Hello));
}

TEST(ProtocolPolicy, HeartbeatExpectsReplyButIsNotRetried) {
    EXPECT_TRUE(mc::requiresAck(MessageType::Heartbeat));
    EXPECT_FALSE(mc::isRetryable(MessageType::Heartbeat));
}

TEST(ProtocolPolicy, TelemetryNeedsNoAck) {
    EXPECT_FALSE(mc::requiresAck(MessageType::Telemetry));
    EXPECT_FALSE(mc::isRetryable(MessageType::Telemetry));
}

TEST(ProtocolPolicy, TryAckOfMapsRequestsAndReportsNoPairing) {
    MessageType ack = MessageType::Nack;

    EXPECT_TRUE(mc::tryAckOf(MessageType::Start, ack));
    EXPECT_EQ(ack, MessageType::StartAck);

    EXPECT_TRUE(mc::tryAckOf(MessageType::SetPid, ack));
    EXPECT_EQ(ack, MessageType::SetPidAck);

    // 负向对照：Stop / Telemetry 没有配对的 ACK，必须显式返回 false，
    // 而不是靠一个"哨兵类型"蒙混过去。
    EXPECT_FALSE(mc::tryAckOf(MessageType::Stop, ack));
    EXPECT_FALSE(mc::tryAckOf(MessageType::Telemetry, ack));
}

TEST(ProtocolPolicy, AckFlagDistinguishesAcksFromRequests) {
    EXPECT_TRUE(mc::isAck(MessageType::StartAck));
    EXPECT_TRUE(mc::isAck(MessageType::HelloAck));
    EXPECT_FALSE(mc::isAck(MessageType::Start));
    EXPECT_FALSE(mc::isAck(MessageType::Nack));
}
