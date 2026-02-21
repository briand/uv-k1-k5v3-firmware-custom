 /*
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

#ifndef DRIVER_TIMER_H
#define DRIVER_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef ENABLE_MILLIS
	// 1 ms ISR flag – set by TIMERBASE0 interrupt, consumed in main loop
	extern volatile bool gNextTimeslice_1ms;
#endif

void TIM0_INIT(void);

// Returns low 16-bit milliseconds from TIMERBASE0 low counter.
uint16_t timer_millis_low16(void);

// Returns elapsed milliseconds based on low 16-bit counter (wrap-safe modulo 16 bits).
uint16_t timer_millis_low16_since(uint16_t prev);

#ifdef ENABLE_MILLIS
	// Returns 32-bit milliseconds:
	// lower 16 bits are TIMERBASE0 low counter,
	// upper 16 bits are maintained by TIMERBASE0 overflow ISR.
	uint32_t timer_millis(void);

	// Returns milliseconds elapsed since previous millis value with rollover protection
	// prev: Previous millis value from timer_millis()
	uint32_t timer_millis_since(uint32_t prev);
#endif

#endif
