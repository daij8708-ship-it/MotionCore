#pragma once

#include <cstddef>
#include <cstdint>

namespace mc {

// CRC-16/CCITT-FALSE
//   poly   = 0x1021
//   init   = 0xFFFF
//   refin  = false
//   refout = false
//   xorout = 0x0000
//
// 标准校验向量：crc16("123456789") == 0x29B1
inline constexpr std::uint16_t kCrc16Init = 0xFFFFU;
inline constexpr std::uint16_t kCrc16Poly = 0x1021U;

// 逐位实现。慢，但它是查表实现的独立参照 —— 两者必须给出相同结果。
std::uint16_t crc16Bitwise(const std::uint8_t* data, std::size_t len) noexcept;

// 查表实现，用于实际收发路径。data 可以为 nullptr（当 len == 0 时）。
std::uint16_t crc16(const std::uint8_t* data, std::size_t len) noexcept;

// 流式计算，用于"边收边算"而不用先把整帧拼起来。
class Crc16 {
public:
    void reset() noexcept { value_ = kCrc16Init; }

    void update(std::uint8_t byte) noexcept;
    void update(const std::uint8_t* data, std::size_t len) noexcept;

    std::uint16_t value() const noexcept { return value_; }

private:
    std::uint16_t value_ = kCrc16Init;
};

}  // namespace mc
