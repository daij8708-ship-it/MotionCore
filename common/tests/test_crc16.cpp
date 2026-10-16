#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "mc/crc16.hpp"

namespace {

std::vector<std::uint8_t> bytesOf(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> randomBytes(std::mt19937& rng, std::size_t len) {
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<std::uint8_t> out(len);
    for (std::uint8_t& byte : out) {
        byte = static_cast<std::uint8_t>(dist(rng));
    }
    return out;
}

}  // namespace

// CRC-16/CCITT-FALSE 的公开标准校验向量。
// 这是本文件里唯一一个"外部权威值"，其余断言都是内部自洽性检查。
TEST(Crc16, StandardCheckVector) {
    const std::vector<std::uint8_t> data = bytesOf("123456789");

    EXPECT_EQ(mc::crc16(data.data(), data.size()), 0x29B1U);
    EXPECT_EQ(mc::crc16Bitwise(data.data(), data.size()), 0x29B1U);
}

// 正对照：证明上面那个断言不是"对任何输入都成立"的空断言 ——
// 改动一个字符，CRC 必须变。
TEST(Crc16, ValueChangesWhenInputChanges) {
    const std::vector<std::uint8_t> a = bytesOf("123456789");
    const std::vector<std::uint8_t> b = bytesOf("123456780");

    EXPECT_NE(mc::crc16(a.data(), a.size()), mc::crc16(b.data(), b.size()));
}

TEST(Crc16, EmptyInputEqualsInitValue) {
    EXPECT_EQ(mc::crc16(nullptr, 0U), mc::kCrc16Init);
    EXPECT_EQ(mc::crc16Bitwise(nullptr, 0U), mc::kCrc16Init);
}

// 查表实现与逐位实现是两份独立代码，必须处处一致。
TEST(Crc16, TableImplementationMatchesBitwiseOnRandomData) {
    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> lenDist(0, 300);

    for (int iteration = 0; iteration < 2000; ++iteration) {
        const std::size_t len = static_cast<std::size_t>(lenDist(rng));
        const std::vector<std::uint8_t> data = randomBytes(rng, len);

        ASSERT_EQ(mc::crc16(data.data(), data.size()),
                  mc::crc16Bitwise(data.data(), data.size()))
            << "iteration=" << iteration << " len=" << len;
    }
}

// 按任意大小分块流式累计，结果必须等于一次性计算。
TEST(Crc16, StreamingMatchesOneShot) {
    std::mt19937 rng(0x5EEDU);
    std::uniform_int_distribution<std::size_t> chunkDist(1, 17);
    std::uniform_int_distribution<int> lenDist(0, 400);

    for (int iteration = 0; iteration < 500; ++iteration) {
        const std::size_t len = static_cast<std::size_t>(lenDist(rng));
        const std::vector<std::uint8_t> data = randomBytes(rng, len);

        mc::Crc16 streaming;
        std::size_t offset = 0U;
        while (offset < data.size()) {
            const std::size_t chunk = std::min(chunkDist(rng), data.size() - offset);
            streaming.update(data.data() + offset, chunk);
            offset += chunk;
        }

        ASSERT_EQ(streaming.value(), mc::crc16(data.data(), data.size()))
            << "iteration=" << iteration << " len=" << len;
    }
}

TEST(Crc16, ResetRestoresInitValue) {
    mc::Crc16 crc;
    const std::vector<std::uint8_t> data = bytesOf("payload");

    crc.update(data.data(), data.size());
    const std::uint16_t afterUpdate = crc.value();

    crc.reset();
    EXPECT_EQ(crc.value(), mc::kCrc16Init);

    crc.update(data.data(), data.size());
    EXPECT_EQ(crc.value(), afterUpdate);
}
