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

#ifdef disabled
extern uint8_t TIM0_CNT;
#endif

void TIM0_INIT(void);

#ifdef ENABLE_MILLIS
// Returns timer count in 100µs ticks (10kHz)
// WARNING: Rolls over every ~6.5 seconds (16-bit counter)
uint16_t timer_jiffies(void);

// Returns milliseconds since boot
// WARNING: Rolls over every ~6.5 seconds (16-bit counter at 10kHz = 6553ms)
uint16_t timer_millis();

// Returns ticks elapsed since previous jiffy value with rollover protection
// prev: Previous jiffy value from timer_jiffies()
uint16_t timer_jiffies_since(uint16_t prev);

// Returns milliseconds elapsed since previous millis value with rollover protection
// prev: Previous millis value from timer_millis()
uint16_t timer_millis_since(uint16_t prev);
#endif

#endif
