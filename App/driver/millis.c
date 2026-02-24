/* Millis implementation using TIM2 as the millisecond source.
 * Uses LL driver; no interrupts or DMA.
 */

#include <stdint.h>
#include <limits.h>
#include "py32f0xx.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_tim.h"
#include "millis.h"

void millis_init(void)
{
    // Enable TIM2 peripheral clock
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);

    // Compute prescaler so timer ticks at ~1 kHz
    uint32_t presc = (SystemCoreClock / 1000U);
    if (presc == 0)
        presc = 1;
    presc -= 1U;

    LL_TIM_SetPrescaler(TIM2, presc);
    LL_TIM_SetAutoReload(TIM2, 0xFFFFFFFFU);
    LL_TIM_EnableARRPreload(TIM2);
    LL_TIM_SetCounterMode(TIM2, LL_TIM_COUNTERMODE_UP);

    // Ensure the prescaler value is loaded immediately by generating an update event
    // and reset the counter to zero so we start from a known millisecond base.
    LL_TIM_SetCounter(TIM2, 0U);
    LL_TIM_GenerateEvent_UPDATE(TIM2);

    // Start counter
    LL_TIM_EnableCounter(TIM2);
}

uint32_t millis(void)
{
    return LL_TIM_GetCounter(TIM2);
}

uint32_t millis_since(uint32_t prev)
{
    uint32_t cur = millis();
    return (cur >= prev) ? (cur - prev) : (UINT32_MAX - prev + 1U + cur);
}
