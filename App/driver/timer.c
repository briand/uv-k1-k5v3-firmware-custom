//
// Originally created by RUPC on 2024/1/8.
//
#include "bsp/dp32g030/timer.h"
#ifdef ENABLE_MILLIS
#include "bsp/dp32g030/irq.h"
#include "ARMCM0.h"
#endif
#include "bsp/dp32g030/syscon.h"
#include <stdbool.h>

#ifdef ENABLE_MILLIS
    // 1 ms ISR flag – set by TIMERBASE0 overflow, consumed by main loop
    volatile bool gNextTimeslice_1ms = false;
    // Upper 16 bits of a software 32-bit millisecond counter.
    // Lower 16 bits come directly from TIMERBASE0_LOW_CNT.
    static volatile uint16_t s_millis_upper16 = 0;
#endif

#ifdef ENABLE_MILLIS
void HandlerTIMER_BASE0(void)
{
    TIMERBASE0_IF = (1 << 0); // write-1-to-clear: acknowledge interrupt
    s_millis_upper16++;
    gNextTimeslice_1ms = true;
}
#endif

void TIM0_INIT(void)
{
    // Enable TIMERBASE0 clock gate
    SYSCON_DEV_CLK_GATE |= SYSCON_DEV_CLK_GATE_TIMER_BASE0_BITS_ENABLE;
    
    // 1 kHz tick (48 MHz / 48000), overflow every 65536 ms.
    TIMERBASE0_DIV = (48000U - 1U);     // Prescaler: 48MHz / 48000 = 1kHz
    TIMERBASE0_LOW_LOAD = 0xFFFFU;      // 16-bit wrap period at 1 kHz: 65536 ms

#ifdef ENABLE_MILLIS
    TIMERBASE0_IF = (1 << 0);           // Clear any pending interrupt
    TIMERBASE0_IE = (1 << 0);           // Enable overflow interrupt
#else
    TIMERBASE0_IE = 0;                  // Polling-only mode (no interrupt support)
#endif

    TIMERBASE0_EN = (1 << 0);           // Enable low counter

#ifdef ENABLE_MILLIS
    NVIC_EnableIRQ((IRQn_Type)DP32_TIMER_BASE0_IRQn);
#endif
}

// Returns low 16-bit milliseconds directly from TIMERBASE0 low counter.
uint16_t timer_millis_low16(void)
{
    return (uint16_t)(TIMERBASE0_LOW_CNT & 0xFFFFU);
}

// Returns low 16-bit milliseconds elapsed since previous low16 value.
uint16_t timer_millis_low16_since(uint16_t prev)
{
    const uint16_t cur = timer_millis_low16();
    return (uint16_t)(cur - prev);
}

#ifdef ENABLE_MILLIS
// Returns 32-bit milliseconds:
// upper 16 bits from ISR-maintained software counter,
// lower 16 bits from TIMERBASE0 low counter.
uint32_t timer_millis(void)
{
    uint16_t hi1;
    uint16_t hi2;
    uint16_t lo;

    // Ensure upper/lower halves are sampled consistently across overflow.
    do {
        hi1 = s_millis_upper16;
        lo  = (uint16_t)(TIMERBASE0_LOW_CNT & 0xFFFFU);
        hi2 = s_millis_upper16;
    } while (hi1 != hi2);

    return (((uint32_t)hi1) << 16) | (uint32_t)lo;
}

// Returns milliseconds elapsed since previous millis value with rollover protection
// prev: Previous millis value from timer_millis()
uint32_t timer_millis_since(uint32_t prev)
{
    uint32_t cur = timer_millis();
    return (cur >= prev) ? (cur - prev) : (UINT32_MAX - prev + 1U + cur);
}
#endif