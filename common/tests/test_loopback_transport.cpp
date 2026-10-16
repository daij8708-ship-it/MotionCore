#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mc/loopback_transport.hpp"

namespace {

std::vector<std::uint8_t> countTo(std::size_t len, std::uint8_t first = 1U) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0U; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(first + i);
    }
    return out;
}

std::vector<std::uint8_t> drain(mc::LoopbackTransport& endpoint, std::size_t maxLen = 1024U) {
    std::vector<std::uint8_t> out(maxLen);
    const std::size_t got = endpoint.receive(out.data(), maxLen);
    out.resize(got);
    return out;
}

}  // namespace

TEST(LoopbackTransportTest, DeliversBytesFromOneEndToTheOther) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    const std::vector<std::uint8_t> payload = countTo(5U);

    ASSERT_TRUE(pair.host->send(payload.data(), payload.size()));

    // 发出去的字节不该出现在发送端自己的读方向。
    EXPECT_TRUE(drain(*pair.host).empty());

    const std::vector<std::uint8_t> received = drain(*pair.device);
    EXPECT_EQ(received, payload);
}

// 默认配置就必须是"无故障"：不能有任何隐含有意义的缺省值。
TEST(LoopbackTransportTest, DefaultConfigurationInjectsNoFaults) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    const std::vector<std::uint8_t> payload = countTo(200U);

    ASSERT_TRUE(pair.host->send(payload.data(), payload.size()));
    const std::vector<std::uint8_t> received = drain(*pair.device, 256U);

    EXPECT_EQ(received, payload);
    EXPECT_EQ(pair.host->outgoingStats().bytesDropped, 0ULL);
    EXPECT_EQ(pair.host->outgoingStats().bytesCorrupted, 0ULL);
}

TEST(LoopbackTransportTest, DirectionsAreIndependent) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();

    const std::vector<std::uint8_t> up = countTo(3U, 0x10U);
    const std::vector<std::uint8_t> down = countTo(3U, 0x20U);

    ASSERT_TRUE(pair.host->send(up.data(), up.size()));
    ASSERT_TRUE(pair.device->send(down.data(), down.size()));

    EXPECT_EQ(drain(*pair.device), up);
    EXPECT_EQ(drain(*pair.host), down);
}

TEST(LoopbackTransportTest, CloseIsVisibleOnBothEnds) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    ASSERT_TRUE(pair.host->isOpen());
    ASSERT_TRUE(pair.device->isOpen());

    pair.host->close();

    EXPECT_FALSE(pair.host->isOpen());
    EXPECT_FALSE(pair.device->isOpen()) << "对端共享同一条通道，必须立刻看到断开";

    const std::vector<std::uint8_t> payload = countTo(4U);
    EXPECT_FALSE(pair.device->send(payload.data(), payload.size()));
    EXPECT_TRUE(drain(*pair.device).empty());
}

TEST(LoopbackTransportTest, LatencyDelaysVisibilityByGivenPolls) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    pair.host->outgoingFaults().latencyPolls = 2U;

    const std::vector<std::uint8_t> payload = countTo(1U);
    ASSERT_TRUE(pair.host->send(payload.data(), payload.size()));

    EXPECT_TRUE(drain(*pair.device).empty()) << "第 1 次 receive 还看不到";
    EXPECT_TRUE(drain(*pair.device).empty()) << "第 2 次 receive 还看不到";
    EXPECT_EQ(drain(*pair.device), payload) << "第 3 次 receive 才可见";
}

TEST(LoopbackTransportTest, DropInjectionLosesExactBytesAndCountsThem) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    pair.host->outgoingFaults().dropEveryNthByte = 3U;

    const std::vector<std::uint8_t> payload = countTo(9U);   // 1..9
    ASSERT_TRUE(pair.host->send(payload.data(), payload.size()));

    const std::vector<std::uint8_t> received = drain(*pair.device);

    // 每第 3 个（3、6、9）被丢弃。
    const std::vector<std::uint8_t> expected = {1U, 2U, 4U, 5U, 7U, 8U};
    EXPECT_EQ(received, expected);
    EXPECT_EQ(pair.host->outgoingStats().bytesDropped, 3ULL);
    EXPECT_EQ(pair.host->outgoingStats().bytesSent, 9ULL);
}

TEST(LoopbackTransportTest, CorruptionInjectionFlipsExactBytesAndCountsThem) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    pair.device->outgoingFaults().corruptEveryNthByte = 2U;

    const std::vector<std::uint8_t> payload = countTo(4U, 0x00U);   // 00 01 02 03
    ASSERT_TRUE(pair.device->send(payload.data(), payload.size()));

    const std::vector<std::uint8_t> received = drain(*pair.host);

    // 每第 2 个（第 2、4 个）取反。
    const std::vector<std::uint8_t> expected = {0x00U, 0xFEU, 0x02U, 0xFCU};
    EXPECT_EQ(received, expected);
    EXPECT_EQ(pair.device->outgoingStats().bytesCorrupted, 2ULL);
}

TEST(LoopbackTransportTest, BoundedQueueDropsAndReportsInsteadOfGrowingForever) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    pair.host->outgoingFaults().maxQueueBytes = 10U;

    const std::vector<std::uint8_t> payload = countTo(25U);
    const bool complete = pair.host->send(payload.data(), payload.size());

    EXPECT_FALSE(complete) << "有字节被丢时 send 必须返回 false，不能假装成功";
    EXPECT_EQ(pair.host->outgoingStats().bytesLostToOverflow, 15ULL);

    const std::vector<std::uint8_t> received = drain(*pair.device, 64U);
    EXPECT_EQ(received.size(), std::size_t{10});

    // 队列没有继续增长：再读一次也是空的。
    EXPECT_TRUE(drain(*pair.device).empty());
}

TEST(LoopbackTransportTest, StatsTrackEachDirectionSeparately) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();

    const std::vector<std::uint8_t> up = countTo(7U);
    ASSERT_TRUE(pair.host->send(up.data(), up.size()));
    (void)drain(*pair.device);

    EXPECT_EQ(pair.host->outgoingStats().bytesSent, 7ULL);
    EXPECT_EQ(pair.device->incomingStats().bytesReceived, 7ULL);
    EXPECT_EQ(pair.device->outgoingStats().bytesSent, 0ULL);
    EXPECT_EQ(pair.host->incomingStats().bytesReceived, 0ULL);
}

TEST(LoopbackTransportTest, ChannelIsSharedByExactlyTwoEndpoints) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    // 每个端点持有两条通道（写一条、读一条），每条被两个端点共享。
    EXPECT_EQ(mc::LoopbackTransport::channelUseCount(*pair.host), std::size_t{4});
    EXPECT_EQ(mc::LoopbackTransport::channelUseCount(*pair.device), std::size_t{4});
}
