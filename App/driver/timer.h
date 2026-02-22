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

// Millisecond timing API – uses HAL LPTIM as the backing hardware.
// The driver exposes a 32-bit millisecond counter by default.

// Initialize the timer hardware (configure and start LPTIM at ~1kHz tick rate).
void timer_init(void);

// Returns milliseconds since boot (32-bit). The LPTIM is configured to tick at 1 kHz
// so returned ticks equal milliseconds.
uint32_t timer_millis(void);

// Returns milliseconds elapsed since previous millis value with rollover protection.
// prev: Previous millis value from timer_millis()
uint32_t timer_millis_since(uint32_t prev);

#endif
