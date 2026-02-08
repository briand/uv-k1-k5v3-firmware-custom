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

// CW Iambic Keyer interface
// a small FSM to handle CW dit/dah timing and iambic A/B semantics

#ifndef APP_CWKEYER_H
#define APP_CWKEYER_H

#include <stdint.h>
#include <stdbool.h>

// Actions emitted by the keyer FSM; mutually exclusive carrier state transitions
typedef enum {
    CW_ACTION_NONE        = 0,  // no carrier active (idle, inter-element gap)
    CW_ACTION_CARRIER_ON  = 1,  // transition carrier from off to on (start element)
    CW_ACTION_CARRIER_OFF = 2,  // transition carrier from on to off (end element)
    CW_ACTION_CARRIER_HOLD_ON = 3, // carrier is on and should remain on (during element)
} CW_Action_t;

// Periodic handler: drive CW state machine and return actions to apply
CW_Action_t CW_HandleState(void);

// Set CW keyer speed from EEPROM; updates internal timing parameters
void CW_UpdateWPM();

// Optional: request reconfigure at next safe boundary (applied at gap or idle)
void CW_KeyerReconfigure(bool enable);

// Check keyer inputs before mode change: returns true if inputs valid, false to abort
// new_mode: The CW_KeyInputType_t mode to validate
bool CW_CheckKeyerInputs(uint8_t new_mode);

// Macro playback API
// Start playback of macro (0-3). Sets playback active flag and loads macro.
// If repeat is true, playback will restart after CW_MESSAGE_REPEAT_DELAY expires.
void CW_StartMacroPlayback(uint8_t macroIndex, bool repeat);

// Stop playback and cancel any pending repeat
void CW_StopPlayback(void);

// Periodic handler for playback FSM - returns CW_Action_t (carrier on/off/hold)
CW_Action_t CW_PlaybackHandleState(void);

// Periodic deadline handler to refresh CW playback UI indicator (blinker).
// Call from a periodic context (e.g., APP_TimeSlice10ms). Uses timer_jiffies/millis.
void CW_PlaybackIndicatorDeadline(void);

#ifdef ENABLE_FLASHLIGHT
extern bool gCW_FlashlightSending;
#endif

#endif // APP_CWKEYER_H
