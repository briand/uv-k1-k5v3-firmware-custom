/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */
#include "py32f0xx.h"
#include "systick.h"
#include "misc.h"
// 0x20000324
static uint32_t gTickMultiplier;
void SYSTICK_Init(void)
{
    SysTick_Config(SystemCoreClock / 100); // 10 ms (1/100 sec) systick interrupt
    gTickMultiplier = SystemCoreClock / 1000000;

    NVIC_SetPriority(SysTick_IRQn, 0);
}
void SYSTICK_DelayUs(uint32_t Delay)
{
	const uint32_t ticks = Delay * gTickMultiplier; // target CPU cycles
	const uint32_t reload = SysTick->LOAD;          // current reload value (period = reload + 1)
	uint32_t elapsed = 0;
	uint32_t prev = SysTick->VAL;

	while (elapsed < ticks) {
		uint32_t cur = SysTick->VAL;
		if (cur != prev) {
			// SysTick counts down; handle wrap: when counter reloads, cur > prev
			uint32_t delta = (cur <= prev)
							   ? (prev - cur)
							   : (prev + ((reload + 1U) - cur));
			elapsed += delta;
			prev = cur;
		}
	}
}