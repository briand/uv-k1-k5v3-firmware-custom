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

// CW RIT/XIT: one shared offset applied to receive (RIT), transmit (XIT) or both,
// and the main-screen adjust mode that edits it. Offset and switches live in RAM
// only, so every power-on starts back on the dial frequency.

#ifndef APP_CWRIT_H
#define APP_CWRIT_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"
#include "radio.h"

// Frequencies (10 Hz units) for a CW VFO: the pitch/BFO offset plus RIT or XIT.
// Non-CW VFOs pass through unchanged.
uint32_t CW_RIT_RxFrequency(const VFO_Info_t *pVfo, uint32_t frequency);
uint32_t CW_RIT_TxFrequency(const VFO_Info_t *pVfo, uint32_t frequency);

// Adjust mode: * in CW or the RIT/XIT key action opens it; EXIT, * or 5 s idle closes it
void CW_RIT_EnterAdjust(void);
void CW_RIT_ToggleAdjust(void);
bool CW_RIT_IsAdjusting(void);
bool CW_RIT_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);  // true when the key was used
void CW_RIT_Tick500ms(void);

// Info-line tag: "R+0.12", "X-0.05", "RX+0.12", or "OFF" while adjusting with both switches off
bool CW_RIT_TagVisible(void);
void CW_RIT_FormatTag(char *buf);

#endif
