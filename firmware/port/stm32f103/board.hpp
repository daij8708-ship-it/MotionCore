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

// TIM4 正交解码：PB6 = CH1，PB7 = CH2，四倍频计数。
void initEncoder();
std::uint16_t encoderCount();

}  // namespace mcfw::board
