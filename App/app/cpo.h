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

// Code practice (CPO) app interface

#ifndef APP_CPO_H
#define APP_CPO_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/keyboard.h"

void CPO_Enter(void);
void CPO_Exit(void);
void CPO_Tick(void);
void CPO_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
#define CPO_NOISE_LINE_SIZE 19   // a full small-font line plus NUL

// Practice with band noise: the receiver plays on AF, and the sidetone and the
// drill's station play on the DAC over it
bool CPO_NoiseOn(void);

// The sidetone under band noise, keyed from the local sidetone path; cue is the
// error dit, at its own pitch
void CPO_NoiseSidetone(bool down, bool cue);

// The band noise line: its level and keys, or how to turn it on
void CPO_GetNoiseLine(char out[CPO_NOISE_LINE_SIZE]);
#endif

extern bool gCW_CpoActive;
extern bool gCW_CpoBacklightOn;
#endif
