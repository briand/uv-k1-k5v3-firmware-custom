/* Copyright 2026 NR7Y
 * https://github.com/briand
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

// Audio out of the MCU's own DAC (PA4, the voice-prompt path): TIM6 triggers a
// conversion at the sample rate and DMA feeds it from a double buffer, so the
// caller's fill function runs each time 80 samples have played (10 ms at 8 kHz) to
// make the next 80. Whatever it writes is 12-bit, 0-4095; 2048 is silence once the
// output has ramped to mid-scale.

#ifndef DRIVER_DACAUDIO_H
#define DRIVER_DACAUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define DACAUDIO_HALF_LEN  80     // samples per fill
#define DACAUDIO_MID       2048

// Called from the DMA interrupt for each half of the buffer as it frees up
typedef void (*DACAUDIO_Fill_t)(uint16_t *buf, uint16_t n);

// Fills both halves, then starts the timer; a no-op while already running
void DACAUDIO_Start(DACAUDIO_Fill_t fill, uint32_t rate_hz);

// New sample rate from the next sample on, while running or not
void DACAUDIO_SetRate(uint32_t rate_hz);

// Stops the timer and DMA and turns the DAC off
void DACAUDIO_Stop(void);

bool DACAUDIO_IsRunning(void);

// Since the last call: fill time as a share of all CPU time, and the longest single
// fill as a share of the time a half buffer plays, both in tenths of a percent
void DACAUDIO_TakeLoad(uint16_t *avg_permille, uint16_t *peak_permille);

#endif
