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

// Bench spike for the QSO simulator: can the MCU DAC play a keyed tone while the
// BK4819 keeps receiving? In code practice, 9 cycles where the audio comes from:
//   off       practice as usual, the BK4819 tone generator only
//   DAC+RX    "CQ" keyed in a loop on the DAC, over live receive audio from the VFO
//   RX only   receive audio alone, with the DAC running silent at mid-scale
//   DAC only  the DAC alone, the BK4819 as silent as practice leaves it
// 3 and 6 step the DAC level up and down 3 dB, and 7 sets 1-5 voices: callers keying
// CQ at their own pitch, staggered, one a zero-beater. 1 cycles the mixer through the
// ways of summing them, and the load line times the DMA interrupt, to size the QSO
// simulator's pile-up. Output is 32 kHz at the sidetone pitch and keyer speed.
// Keying still sounds the sidetone on the BK4819, which takes the audio path over;
// receive audio comes back once the keyer has been idle 300 ms.

#ifndef APP_CPODAC_H
#define APP_CPODAC_H

#include <stdbool.h>
#include <stdint.h>

#define CPO_DAC_STATUS_SIZE 19   // a full small-font line, 18 characters, plus NUL
#define CPO_DAC_MAX_VOICES  5

typedef enum {
	CPO_DAC_OFF = 0,
	CPO_DAC_MIX,
	CPO_DAC_RX,
	CPO_DAC_ONLY,
	CPO_DAC_MODE_COUNT,
} CPO_DacMode_t;

// 9 key: off -> DAC+RX -> RX only -> DAC only -> off
void CPO_Dac_NextMode(void);

// 3 and 6 keys: DAC level 3 dB up or down, 0 to -36 dB
void CPO_Dac_StepLevel(bool louder);

// 1 key: mixer naive -> fast -> fast from RAM
void CPO_Dac_NextMixer(void);

// 7 key: 1 -> 2 -> ... -> 5 voices
void CPO_Dac_NextVoices(void);

// Practice exit: DAC off at once, no ramp
void CPO_Dac_Stop(void);

// 10ms tick from CPO_Tick
void CPO_Dac_Tick10ms(void);

bool CPO_Dac_IsActive(void);

// Title-line text while active, e.g. "DAC+RX -9dB"; false while off
bool CPO_Dac_GetStatus(char out[CPO_DAC_STATUS_SIZE]);

// The synthesis settings, e.g. "32k 5v fast-ram"
void CPO_Dac_GetSynth(char out[CPO_DAC_STATUS_SIZE]);

// The interrupt's average share of the CPU and its longest fill as a share of the
// time one half buffer plays, e.g. "cpu 6.1% pk 9%"
void CPO_Dac_GetLoad(char out[CPO_DAC_STATUS_SIZE]);

// The key hint line, rotating every 2 s while active
const char *CPO_Dac_GetHint(void);

#endif
