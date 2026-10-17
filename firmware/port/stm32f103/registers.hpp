#pragma once

#include <cstdint>

namespace stm32f103 {

struct Rcc {
    volatile std::uint32_t CR;
    volatile std::uint32_t CFGR;
    volatile std::uint32_t CIR;
    volatile std::uint32_t APB2RSTR;
    volatile std::uint32_t APB1RSTR;
    volatile std::uint32_t AHBENR;
    volatile std::uint32_t APB2ENR;
    volatile std::uint32_t APB1ENR;
    volatile std::uint32_t BDCR;
    volatile std::uint32_t CSR;
};

struct Gpio {
    volatile std::uint32_t CRL;
    volatile std::uint32_t CRH;
    volatile std::uint32_t IDR;
    volatile std::uint32_t ODR;
    volatile std::uint32_t BSRR;
    volatile std::uint32_t BRR;
    volatile std::uint32_t LCKR;
};

struct Usart {
    volatile std::uint32_t SR;
    volatile std::uint32_t DR;
    volatile std::uint32_t BRR;
    volatile std::uint32_t CR1;
    volatile std::uint32_t CR2;
    volatile std::uint32_t CR3;
    volatile std::uint32_t GTPR;
};

struct Tim {
    volatile std::uint32_t CR1;
    volatile std::uint32_t CR2;
    volatile std::uint32_t SMCR;
    volatile std::uint32_t DIER;
    volatile std::uint32_t SR;
    volatile std::uint32_t EGR;
    volatile std::uint32_t CCMR1;
    volatile std::uint32_t CCMR2;
    volatile std::uint32_t CCER;
    volatile std::uint32_t CNT;
    volatile std::uint32_t PSC;
    volatile std::uint32_t ARR;
    volatile std::uint32_t RCR;
    volatile std::uint32_t CCR1;
    volatile std::uint32_t CCR2;
    volatile std::uint32_t CCR3;
    volatile std::uint32_t CCR4;
};

struct Systick {
    volatile std::uint32_t CTRL;
    volatile std::uint32_t LOAD;
    volatile std::uint32_t VAL;
    volatile std::uint32_t CALIB;
};

inline Rcc* const kRcc = reinterpret_cast<Rcc*>(0x40021000UL);
inline Gpio* const kGpioA = reinterpret_cast<Gpio*>(0x40010800UL);
inline Gpio* const kGpioC = reinterpret_cast<Gpio*>(0x40011000UL);
inline Usart* const kUsart1 = reinterpret_cast<Usart*>(0x40013800UL);
inline Tim* const kTim3 = reinterpret_cast<Tim*>(0x40000400UL);
inline Systick* const kSystick = reinterpret_cast<Systick*>(0xE000E010UL);
inline volatile std::uint32_t* const kNvicIser1 =
    reinterpret_cast<volatile std::uint32_t*>(0xE000E104UL);

inline constexpr std::uint32_t kRccApb2Afio = 1UL << 0U;
inline constexpr std::uint32_t kRccApb2Iopa = 1UL << 2U;
inline constexpr std::uint32_t kRccApb2Iopc = 1UL << 4U;
inline constexpr std::uint32_t kRccApb2Usart1 = 1UL << 14U;
inline constexpr std::uint32_t kRccApb1Tim3 = 1UL << 1U;

inline constexpr std::uint32_t kUsartSrPe = 1UL << 0U;
inline constexpr std::uint32_t kUsartSrFe = 1UL << 1U;
inline constexpr std::uint32_t kUsartSrNe = 1UL << 2U;
inline constexpr std::uint32_t kUsartSrOre = 1UL << 3U;
inline constexpr std::uint32_t kUsartSrRxne = 1UL << 5U;
inline constexpr std::uint32_t kUsartSrTc = 1UL << 6U;
inline constexpr std::uint32_t kUsartSrTxe = 1UL << 7U;

inline constexpr std::uint32_t kUsartCr1Re = 1UL << 2U;
inline constexpr std::uint32_t kUsartCr1Te = 1UL << 3U;
inline constexpr std::uint32_t kUsartCr1Rxneie = 1UL << 5U;
inline constexpr std::uint32_t kUsartCr1Ue = 1UL << 13U;

inline constexpr std::uint32_t kTimCr1Cen = 1UL << 0U;
inline constexpr std::uint32_t kTimEgrUg = 1UL << 0U;
inline constexpr std::uint32_t kTimCcmr1Oc1pe = 1UL << 3U;
inline constexpr std::uint32_t kTimCcmr1Oc1mPwm1 = 0x60UL;
inline constexpr std::uint32_t kTimCcerCc1e = 1UL << 0U;

inline constexpr std::uint32_t kSystickCtrlEnable = 1UL << 0U;
inline constexpr std::uint32_t kSystickCtrlTickint = 1UL << 1U;
inline constexpr std::uint32_t kSystickCtrlClksource = 1UL << 2U;

// 8 MHz 内部 RC、115200 8N1：BRR = 69（实际 115942 bps，误差 0.64%）。
inline constexpr std::uint32_t kUsartBrr115200 = 69UL;
// 8 MHz / 1000 = 8000 个时钟一拍。
inline constexpr std::uint32_t kSystickReload1ms = 7999UL;

}  // namespace stm32f103
