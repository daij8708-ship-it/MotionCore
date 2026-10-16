#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "mc/ring_buffer.hpp"

namespace {

using IntRing = mc::RingBuffer<int, 4U>;

}  // namespace

TEST(RingBuffer, CapacityIsCompileTimeConstant) {
    EXPECT_EQ(IntRing::capacity(), std::size_t{4});
    EXPECT_TRUE(IntRing{}.empty());
    EXPECT_FALSE(IntRing{}.full());
}

TEST(RingBuffer, PopFromEmptyReturnsFalse) {
    IntRing ring;
    int out = -1;

    EXPECT_FALSE(ring.pop(out));
    EXPECT_EQ(out, -1);          // 失败时不修改出参
    EXPECT_EQ(ring.size(), std::size_t{0});
}

TEST(RingBuffer, FifoOrderIsPreserved) {
    IntRing ring;
    ring.push(1);
    ring.push(2);
    ring.push(3);

    int out = 0;
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 2);
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_FALSE(ring.pop(out));
}

TEST(RingBuffer, AtReturnsOldestFirst) {
    IntRing ring;
    ring.push(10);
    ring.push(20);
    ring.push(30);

    EXPECT_EQ(ring.at(0U), 10);
    EXPECT_EQ(ring.at(1U), 20);
    EXPECT_EQ(ring.at(2U), 30);
}

TEST(RingBuffer, AtThrowsWhenOutOfRange) {
    IntRing ring;
    ring.push(10);

    EXPECT_THROW(ring.at(1U), std::out_of_range);
    EXPECT_THROW(IntRing{}.at(0U), std::out_of_range);
}

// 核心行为：写满后覆盖最旧元素，丢弃量可精确核对。
TEST(RingBuffer, OverflowOverwritesOldestAndCountsExactDiscards) {
    constexpr std::size_t kPushes = 1000U;
    constexpr std::size_t kCapacity = IntRing::capacity();

    IntRing ring;
    for (std::size_t i = 0U; i < kPushes; ++i) {
        ring.push(static_cast<int>(i));
    }

    EXPECT_EQ(ring.size(), kCapacity);
    EXPECT_TRUE(ring.full());
    EXPECT_EQ(ring.discarded(), static_cast<std::uint64_t>(kPushes - kCapacity));

    // 保留的是最后 kCapacity 个值，且顺序仍然是从旧到新。
    for (std::size_t i = 0U; i < kCapacity; ++i) {
        EXPECT_EQ(ring.at(i), static_cast<int>(kPushes - kCapacity + i)) << "i=" << i;
    }
}

// "内存占用恒定"的可观测代理：持续写入后 size() 始终被夹在容量上，
// 底层是 std::array（编译期定容），运行期不分配堆内存。
TEST(RingBuffer, SizeStaysBoundedUnderSustainedWrites) {
    constexpr std::size_t kCapacity = IntRing::capacity();

    IntRing ring;
    for (std::size_t i = 0U; i < 1000000U; ++i) {
        ring.push(static_cast<int>(i % 7U));
        ASSERT_LE(ring.size(), kCapacity) << "i=" << i;
    }

    EXPECT_EQ(ring.size(), kCapacity);
    EXPECT_EQ(ring.discarded(), 1000000ULL - static_cast<std::uint64_t>(kCapacity));
}

// 显式语义：清空元素不重置统计量，需要重置时另行调用。
TEST(RingBuffer, ClearKeepsDiscardCounterAndResetCountersZeroesIt) {
    IntRing ring;
    for (int i = 0; i < 10; ++i) {
        ring.push(i);
    }
    ASSERT_GT(ring.discarded(), 0ULL);

    ring.clear();
    EXPECT_EQ(ring.size(), std::size_t{0});
    EXPECT_TRUE(ring.empty());
    EXPECT_GT(ring.discarded(), 0ULL);      // 累计统计不随清空归零

    ring.resetCounters();
    EXPECT_EQ(ring.discarded(), 0ULL);
}

TEST(RingBuffer, PopFreesSlotForNewPushes) {
    IntRing ring;
    ring.push(1);
    ring.push(2);
    ring.push(3);
    ring.push(4);
    EXPECT_TRUE(ring.full());
    EXPECT_EQ(ring.discarded(), 0ULL);

    int out = 0;
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 1);
    EXPECT_FALSE(ring.full());

    ring.push(5);
    EXPECT_TRUE(ring.full());
    EXPECT_EQ(ring.discarded(), 0ULL);      // 有空间时不应计入丢弃

    EXPECT_EQ(ring.at(0U), 2);
    EXPECT_EQ(ring.at(3U), 5);
}
