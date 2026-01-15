//
// Originally created by RUPC on 2024/1/8.
//
#include "bsp/dp32g030/timer.h"
#include "ARMCM0.h"
#include <stdbool.h>

uint8_t TIM0_CNT = 0;

__attribute__((weak)) void HandlerTIMER_BASE0()
{
    TIMERBASE0_IF |= (1 << 0); // clear timer interrupt status first
    TIM0_CNT++;
}

void TIM0_SET_PSC(uint16_t prescaler) {
    // Clear the DIV field and set the prescaler (16-bit register)
    TIMERBASE0_DIV &= ~(0xFFFF); // Clear DIV field contents via AND
    TIMERBASE0_DIV |= prescaler;
}

void TIM0_SET_ARR(uint16_t targetval) {
    // Clear the LOW_LOAD field and set the auto-reload value (16-bit register)
    TIMERBASE0_LOW_LOAD &= ~(0xFFFF); // Clear field contents via AND
    TIMERBASE0_LOW_LOAD |= targetval;
}

void TIM0_INIT(uint16_t prescaler, uint16_t targetval, bool enableInterrupt)
{
    TIM0_SET_PSC(prescaler);
    TIM0_SET_ARR(targetval);

    TIMERBASE0_IF |= (1 << 1) | (1 << 0); // Write 1 to clear: clear timer interrupt status

    // Enable Timer0 interrupt (LOW_IE)
    if (enableInterrupt) {
        TIMERBASE0_IE |= (1 << 0); // Enable LOW_IE
    }

    // Enable TIMERBASE0 (LOW_EN)
    TIMERBASE0_EN |= 0x1; // Enable LOW_EN
}

// Example configuration (commented out):
//    TIM0_SET_PSC(480 - 1);           // 48,000,000 / 480 / 100 = 1000
//    TIM0_SET_ARR(1000);              // 10 ms
//    TIMERBASE0_IF |= (1 << 1) | (1 << 0); // Write 1 to clear: clear timer interrupt status
//    TIMERBASE0_IE |= (1 << 1) | (1 << 0); // Enable interrupts for both high and low timers
//    TIMERBASE0_EN |= (1 << 1) | (1 << 0); // Enable both high and low timers
//
//    __enable_irq();



