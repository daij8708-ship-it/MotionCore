#pragma once

#include <cstddef>
#include <cstdint>

namespace mcfw::board {

void init();
std::uint32_t millis();
bool readByte(std::uint8_t& out);
void write(const std::uint8_t* data, std::size_t len);
void setLed(bool on);
void setMotor(std::uint16_t pwm, bool forward, bool enable);
std::uint32_t uartErrorCount();

}  // namespace mcfw::board
