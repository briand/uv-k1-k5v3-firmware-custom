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

#define CPO_HINT_LINE_SIZE 19   // a full small-font line plus NUL
#define CPO_CARD_LINES     6

// Every character the CW encoder decodes, and whether a word gap came before it:
// to the QSO drill when it runs, else the callsign drill
void CPO_OnChar(char ch, bool word_gap);

// The hint line, which takes turns with the band noise level, its keys, and MENU
void CPO_GetHintLine(char out[CPO_HINT_LINE_SIZE]);

// The controls card MENU puts up, which any key takes down, and its lines for the
// drill in use
bool CPO_CardShowing(void);
uint8_t CPO_GetCard(const char *lines[CPO_CARD_LINES]);

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
// Practice tones play on the DAC: under band noise, where the receiver plays on AF,
// and in the QSO drill, whose station plays there
bool CPO_SynthOn(void);

// The sidetone on the DAC, keyed from the local sidetone path; cue is the error
// dit, at its own pitch
void CPO_NoiseSidetone(bool down, bool cue);
#endif

extern bool gCW_CpoActive;
extern bool gCW_CpoBacklightOn;
#endif
