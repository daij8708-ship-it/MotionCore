#include "board.hpp"

#include "registers.hpp"

namespace mcfw::board {
namespace {

constexpr std::uint16_t kRingSize = 256U;
volatile std::uint8_t g_ring[kRingSize]{};
volatile std::uint16_t g_head = 0U;
volatile std::uint16_t g_tail = 0U;
volatile std::uint32_t g_overflow = 0U;
volatile std::uint32_t g_uartErrors = 0U;
volatile std::uint32_t g_millis = 0U;

void configPin(volatile std::uint32_t& cr, unsigned pin, std::uint32_t value) {
    const unsigned shift = (pin % 8U) * 4U;
    cr = (cr & ~(0xFUL << shift)) | ((value & 0xFUL) << shift);
}

void writePinLow(stm32f103::Gpio* gpio, unsigned pin) {
    gpio->BSRR = 1UL << (pin + 16U);
}

void writePinHigh(stm32f103::Gpio* gpio, unsigned pin) {
    gpio->BSRR = 1UL << pin;
}

void ringPush(std::uint8_t byte) {
    const std::uint16_t next = static_cast<std::uint16_t>((g_head + 1U) % kRingSize);
    if (next == g_tail) {
        g_tail = static_cast<std::uint16_t>((g_tail + 1U) % kRingSize);
        ++g_overflow;
    }
    g_ring[g_head] = byte;
    g_head = next;
}

}  // namespace

extern "C" void SysTick_Handler(void) {
    ++g_millis;
}

extern "C" void USART1_IRQHandler(void) {
    const std::uint32_t sr = stm32f103::kUsart1->SR;
    const bool hasData = (sr & stm32f103::kUsartSrRxne) != 0U;
    const bool hasError = (sr & (stm32f103::kUsartSrOre | stm32f103::kUsartSrFe |
                                 stm32f103::kUsartSrNe | stm32f103::kUsartSrPe)) != 0U;
    if (!hasData && !hasError) {
        return;
    }

    const std::uint8_t data = static_cast<std::uint8_t>(stm32f103::kUsart1->DR);
    if (hasData) {
        ringPush(data);
    }
    if (hasError) {
        ++g_uartErrors;
    }
}

void init() {
    // 时钟：AFIO + GPIOA + GPIOC + USART1 + TIM3。
    stm32f103::kRcc->APB2ENR |= stm32f103::kRccApb2Afio | stm32f103::kRccApb2Iopa |
                                stm32f103::kRccApb2Iopc | stm32f103::kRccApb2Usart1;
    stm32f103::kRcc->APB1ENR |= stm32f103::kRccApb1Tim3;

    // PA9 = USART1_TX（复用推挽 50 MHz），PA10 = USART1_RX（浮空输入）。
    configPin(stm32f103::kGpioA->CRH, 9U, 0xBU);
    configPin(stm32f103::kGpioA->CRH, 10U, 0x4U);

    // PA5 = 驱动器使能，PA6 = TIM3_CH1 PWM，PA7 = 方向。
    configPin(stm32f103::kGpioA->CRL, 5U, 0x3U);
    configPin(stm32f103::kGpioA->CRL, 6U, 0x3U);
    configPin(stm32f103::kGpioA->CRL, 7U, 0x3U);

    // PC13 = 板载 LED（低电平点亮）。
    configPin(stm32f103::kGpioC->CRH, 13U, 0x2U);
    writePinHigh(stm32f103::kGpioC, 13U);

    // USART1：8 MHz / 115200，8N1，开接收中断。
    stm32f103::kUsart1->BRR = stm32f103::kUsartBrr115200;
    stm32f103::kUsart1->CR1 = stm32f103::kUsartCr1Ue | stm32f103::kUsartCr1Te |
                              stm32f103::kUsartCr1Re | stm32f103::kUsartCr1Rxneie;

    // USART1 中断号 37，落在 NVIC ISER1 的 bit 5。
    *stm32f103::kNvicIser1 = 1UL << 5U;

    // TIM3：1 MHz 计数，ARR=99 → 10 kHz PWM，占空比 0..99。
    stm32f103::kTim3->PSC = 71U;
    stm32f103::kTim3->ARR = 99U;
    stm32f103::kTim3->CCMR1 = stm32f103::kTimCcmr1Oc1pe | stm32f103::kTimCcmr1Oc1mPwm1;
    stm32f103::kTim3->CCER = stm32f103::kTimCcerCc1e;
    stm32f103::kTim3->EGR = stm32f103::kTimEgrUg;
    stm32f103::kTim3->CCR1 = 0U;
    stm32f103::kTim3->CR1 = stm32f103::kTimCr1Cen;

    writePinLow(stm32f103::kGpioA, 5U);
    writePinLow(stm32f103::kGpioA, 7U);

    // SysTick：8 MHz 内核时钟，1 ms 中断。
    stm32f103::kSystick->LOAD = stm32f103::kSystickReload1ms;
    stm32f103::kSystick->VAL = 0U;
    stm32f103::kSystick->CTRL = stm32f103::kSystickCtrlClksource |
                               stm32f103::kSystickCtrlTickint |
                               stm32f103::kSystickCtrlEnable;
}

std::uint32_t millis() {
    return g_millis;
}

bool readByte(std::uint8_t& out) {
    if (g_tail == g_head) {
        return false;
    }
    out = g_ring[g_tail];
    g_tail = static_cast<std::uint16_t>((g_tail + 1U) % kRingSize);
    return true;
}

void write(const std::uint8_t* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        std::uint32_t guard = 0U;
        while ((stm32f103::kUsart1->SR & stm32f103::kUsartSrTxe) == 0U) {
            if (++guard > 1000000U) {
                return;
            }
        }
        stm32f103::kUsart1->DR = data[i];
    }

    std::uint32_t guard = 0U;
    while ((stm32f103::kUsart1->SR & stm32f103::kUsartSrTc) == 0U) {
        if (++guard > 1000000U) {
            return;
        }
    }
}

void setLed(bool on) {
    if (on) {
        writePinLow(stm32f103::kGpioC, 13U);
    } else {
        writePinHigh(stm32f103::kGpioC, 13U);
    }
}

void setMotor(std::uint16_t pwm, bool forward, bool enable) {
    const std::uint16_t clamped = pwm > 99U ? 99U : pwm;
    stm32f103::kTim3->CCR1 = enable ? clamped : 0U;

    if (enable) {
        writePinHigh(stm32f103::kGpioA, 5U);
    } else {
        writePinLow(stm32f103::kGpioA, 5U);
    }

    if (forward) {
        writePinHigh(stm32f103::kGpioA, 7U);
    } else {
        writePinLow(stm32f103::kGpioA, 7U);
    }
}

std::uint32_t uartErrorCount() {
    return g_uartErrors;
}

}  // namespace mcfw::board
