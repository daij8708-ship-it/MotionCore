#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);
extern void SysTick_Handler(void);
extern void USART1_IRQHandler(void);

void Reset_Handler(void);
void Default_Handler(void);

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[] = {
    (vector_t)&_estack,
    Reset_Handler,
    Default_Handler,   // NMI
    Default_Handler,   // HardFault
    Default_Handler,   // MemManage
    Default_Handler,   // BusFault
    Default_Handler,   // UsageFault
    Default_Handler,
    Default_Handler,
    Default_Handler,
    Default_Handler,
    Default_Handler,   // SVCall
    Default_Handler,   // DebugMon
    Default_Handler,
    Default_Handler,   // PendSV
    SysTick_Handler,
    Default_Handler,   // WWDG
    Default_Handler,   // PVD
    Default_Handler,   // TAMPER
    Default_Handler,   // RTC
    Default_Handler,   // FLASH
    Default_Handler,   // RCC
    Default_Handler,   // EXTI0
    Default_Handler,   // EXTI1
    Default_Handler,   // EXTI2
    Default_Handler,   // EXTI3
    Default_Handler,   // EXTI4
    Default_Handler,   // DMA1_Channel1
    Default_Handler,   // DMA1_Channel2
    Default_Handler,   // DMA1_Channel3
    Default_Handler,   // DMA1_Channel4
    Default_Handler,   // DMA1_Channel5
    Default_Handler,   // DMA1_Channel6
    Default_Handler,   // DMA1_Channel7
    Default_Handler,   // ADC1_2
    Default_Handler,   // USB_HP_CAN1_TX
    Default_Handler,   // USB_LP_CAN1_RX0
    Default_Handler,   // CAN1_RX1
    Default_Handler,   // CAN1_SCE
    Default_Handler,   // EXTI9_5
    Default_Handler,   // TIM1_BRK
    Default_Handler,   // TIM1_UP
    Default_Handler,   // TIM1_TRG_COM
    Default_Handler,   // TIM1_CC
    Default_Handler,   // TIM2
    Default_Handler,   // TIM3
    Default_Handler,   // TIM4
    Default_Handler,   // I2C1_EV
    Default_Handler,   // I2C1_ER
    Default_Handler,   // I2C2_EV
    Default_Handler,   // I2C2_ER
    Default_Handler,   // SPI1
    Default_Handler,   // SPI2
    USART1_IRQHandler, // USART1
    Default_Handler,   // USART2
    Default_Handler,   // USART3
    Default_Handler,   // EXTI15_10
    Default_Handler,   // RTCAlarm
    Default_Handler,   // USBWakeup
};

void Reset_Handler(void) {
    uint32_t* source = &_sidata;
    for (uint32_t* destination = &_sdata; destination < &_edata;) {
        *destination++ = *source++;
    }
    for (uint32_t* destination = &_sbss; destination < &_ebss;) {
        *destination++ = 0U;
    }

    (void)main();
    for (;;) {
    }
}

void Default_Handler(void) {
    for (;;) {
    }
}
