#include "mc/crc16.hpp"

#include <array>

namespace mc {
namespace {

std::array<std::uint16_t, 256> buildTable() noexcept {
    std::array<std::uint16_t, 256> table{};
    for (std::uint32_t i = 0; i < 256U; ++i) {
        std::uint16_t crc = static_cast<std::uint16_t>(i << 8);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ kCrc16Poly);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
        table[i] = crc;
    }
    return table;
}

const std::array<std::uint16_t, 256>& table() noexcept {
    static const std::array<std::uint16_t, 256> instance = buildTable();
    return instance;
}

}  // namespace

std::uint16_t crc16Bitwise(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint16_t crc = kCrc16Init;
    for (std::size_t i = 0; i < len; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ static_cast<std::uint16_t>(data[i] << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ kCrc16Poly);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t len) noexcept {
    const std::array<std::uint16_t, 256>& t = table();
    std::uint16_t crc = kCrc16Init;
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>((crc >> 8) ^ data[i]);
        crc = static_cast<std::uint16_t>((crc << 8) ^ t[index]);
    }
    return crc;
}

void Crc16::update(std::uint8_t byte) noexcept {
    const std::array<std::uint16_t, 256>& t = table();
    const std::uint8_t index = static_cast<std::uint8_t>((value_ >> 8) ^ byte);
    value_ = static_cast<std::uint16_t>((value_ << 8) ^ t[index]);
}

void Crc16::update(const std::uint8_t* data, std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        update(data[i]);
    }
}

}  // namespace mc
