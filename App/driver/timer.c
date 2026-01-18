//
// Originally created by RUPC on 2024/1/8.
//
#include "bsp/dp32g030/timer.h"
#include "bsp/dp32g030/syscon.h"
#include "ARMCM0.h"
#include <stdbool.h>

#ifdef disabled
uint8_t TIM0_CNT = 0;

__attribute__((weak)) void HandlerTIMER_BASE0()
{
    TIMERBASE0_IF |= (1 << 0); // clear timer interrupt status first
    TIM0_CNT++;
}
#endif

static void TIM0_SET_PSC(uint16_t prescaler) {
    // Clear the DIV field and set the prescaler (16-bit register)
    TIMERBASE0_DIV &= ~(0xFFFF); // Clear DIV field contents via AND
    TIMERBASE0_DIV |= prescaler;
}

static void TIM0_SET_ARR(uint16_t targetval) {
    // Clear the LOW_LOAD field and set the auto-reload value (16-bit register)
    TIMERBASE0_LOW_LOAD &= ~(0xFFFF); // Clear field contents via AND
    TIMERBASE0_LOW_LOAD |= targetval;
}

void TIM0_INIT(void)
{
    // Enable TIMERBASE0 clock gate
    SYSCON_DEV_CLK_GATE |= SYSCON_DEV_CLK_GATE_TIMER_BASE0_BITS_ENABLE;
    
    // Configure for 10 kHz tick (48 MHz / 4800)
    // 16-bit counter rolls over at 0xFFFF (~6553 ms)
    TIM0_SET_PSC(4800U - 1U);
    TIM0_SET_ARR(0xFFFFU);

    TIMERBASE0_IF |= (1 << 0); // Write 1 to clear: clear timer interrupt status
    
    // Enable low counter only (HIGH counter unused - not cascaded)
    TIMERBASE0_EN |= (1 << 0);
}

#ifdef ENABLE_MILLIS

// Returns timer count in 100µs ticks (10kHz)
// WARNING: Rolls over every ~6.5 seconds (16-bit counter)
inline uint16_t timer_jiffies(void)
{
    return (uint16_t)TIMERBASE0_LOW_CNT;
}

// Returns milliseconds since boot
// WARNING: Rolls over every ~6.5 seconds (16-bit counter at 10kHz = 6553ms)
inline uint16_t timer_millis(void)
{
    // Read low counter and convert to milliseconds: 10 ticks = 1ms
    return (uint16_t)(TIMERBASE0_LOW_CNT / 10);
}

// Returns ticks elapsed since previous jiffy value with rollover protection
// prev: Previous jiffy value from timer_jiffies()
uint16_t timer_jiffies_since(uint16_t prev)
{
    uint16_t cur = (uint16_t)TIMERBASE0_LOW_CNT;
    return (cur >= prev) ? (uint16_t)(cur - prev) : (uint16_t)(UINT16_MAX - prev + 1U + cur);
}

// Returns milliseconds elapsed since previous millis value with rollover protection
// prev: Previous millis value from timer_millis()
uint16_t timer_millis_since(uint16_t prev)
{
    uint16_t cur = timer_millis();
    return (cur >= prev) ? (uint16_t)(cur - prev) : (uint16_t)(UINT16_MAX - prev + 1U + cur);
}

#endif