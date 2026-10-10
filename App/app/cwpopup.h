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

// CW quick-settings popup on the main screen. It shows the setting a key action just
// changed, and for speed and key input it stays interactive while it is up. Any key
// other than MENU, EXIT and up/down closes it and then does its usual job.

#ifndef APP_CWPOPUP_H
#define APP_CWPOPUP_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"

typedef enum {
	CW_POPUP_NONE = 0,
	CW_POPUP_KEYER_MODE,
	CW_POPUP_SPEED,        // up/down change WPM live; the new speed stays however the popup closes
	CW_POPUP_KEY_INPUT,    // up/down or the action step a pending input; only MENU applies it, 5 s idle drops it
	CW_POPUP_KEY_STUCK,    // the pending input failed the stuck-key check
	CW_POPUP_TX_TIMEOUT,   // a stuck key tripped the transmit timeout
} CW_PopupKind_t;

void           CW_Popup_Show(CW_PopupKind_t kind);
void           CW_Popup_Dismiss(CW_PopupKind_t kind);  // close it if that kind is showing
CW_PopupKind_t CW_Popup_Kind(void);
uint8_t        CW_Popup_KeyInput(void);  // key input index the popup is showing

// Key actions
void CW_Popup_Speed(void);         // open speed adjust, or close it when already open
void CW_Popup_StepKeyInput(void);  // first press shows the current input, later presses step it

bool CW_Popup_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);  // true when the key was used
void CW_Popup_OnAction(uint8_t action);  // a side key action is about to run
void CW_Popup_OnKeying(void);      // the keyer or playback just keyed an element
void CW_Popup_Tick10ms(void);

#endif
