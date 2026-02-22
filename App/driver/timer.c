/* Timer implementation using LL LPTIM as the millisecond source.
 * Initialize LPTIM1 via LL, start continuous counting, and return the
 * 32-bit hardware counter directly. Adjust prescaler if board clock differs
 * so that 1 tick approximates 1 ms.
 */

#include <stdint.h>
#include "py32f0xx.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_lptim.h"

void timer_init(void)
{
    /* Enable LPTIM1 peripheral clock */
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_LPTIM1);

    /* Configure prescaler and update mode; use LL helpers when available */
#if defined(USE_FULL_LL_DRIVER)
    LL_LPTIM_InitTypeDef init;
    LL_LPTIM_StructInit(&init);
    init.Prescaler = LL_LPTIM_PRESCALER_DIV32; /* tune to board clock */
    init.UpdateMode = LL_LPTIM_UPDATE_MODE_IMMEDIATE;
    (void)LL_LPTIM_Init(LPTIM1, &init);
#else
    LL_LPTIM_SetPrescaler(LPTIM1, LL_LPTIM_PRESCALER_DIV32);
    LL_LPTIM_SetUpdateMode(LPTIM1, LL_LPTIM_UPDATE_MODE_IMMEDIATE);
#endif

    /* Set autoreload to maximum and enable/start the counter in continuous mode */
    LL_LPTIM_SetAutoReload(LPTIM1, 0xFFFFFFFFU);
    LL_LPTIM_Enable(LPTIM1);
    LL_LPTIM_StartCounter(LPTIM1, LL_LPTIM_OPERATING_MODE_CONTINUOUS);
}

uint32_t timer_millis(void)
{
    /* Read the hardware counter directly (32-bit) */
    return LL_LPTIM_GetCounter(LPTIM1);
}

uint32_t timer_millis_since(uint32_t prev)
{
    uint32_t cur = timer_millis();
    return (cur >= prev) ? (cur - prev) : (UINT32_MAX - prev + 1U + cur);
}