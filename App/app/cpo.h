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

#define CPO_CARD_LINE_SIZE 19   // a full small-font line plus NUL
#define CPO_CARD_LINES     6

// Every character the CW encoder decodes, and whether a word gap came before it:
// to the QSO drill when it runs, else the callsign drill
void CPO_OnChar(char ch, bool word_gap);

// Your keying closes a setting's popup, as it does the CW popups, dropping a drill the
// picker hasn't switched to
void CPO_OnKeying(void);

// A setting's popup over the drill, in the CW popups' box, for 2 s after its key. F's
// drill picker is the confirm kind: F or up/down step it, only MENU switches to it, and
// EXIT, any other key, keying or the menu's idle timeout drop it. Up/down are always
// the speed otherwise, on line 6 with no popup.
#define CPO_POPUP_VALUE_SIZE 12   // 13 big-font characters fit the box
typedef struct {
	const char *title;
	char value[CPO_POPUP_VALUE_SIZE];
	// The key that changes it, or a pair drawn one over the other as on the keypad,
	// the upper one raising it; the picker draws the step arrows instead
	char keys[3];
	bool confirm;   // the drill picker: a choice only MENU applies
} CPO_Popup_t;

bool CPO_GetPopup(CPO_Popup_t *popup);   // false while none is up

// The controls card MENU puts up, which any key takes down, and its lines for the
// drill in use. In them < and > stand for the left and right speed keys, which the
// card draws as arrows the way SetNav names them.
bool CPO_CardShowing(void);
uint8_t CPO_GetCard(const char *lines[CPO_CARD_LINES]);

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
// Practice tones play on the DAC: under band noise, where the receiver plays on AF,
// and in the QSO drill, whose station plays there
bool CPO_SynthOn(void);

// A station at full strength: as loud as the sidetone at its level
uint16_t CPO_StationGain(void);

// The sidetone on the DAC, keyed from the local sidetone path; cue is the error dit,
// at its own pitch. Your sending breaks in like a transceiver: the stations and the
// band noise drop out while you key and come back after the CW hang time. The cue is
// local and breaks nothing in.
void CPO_Sidetone(bool down, bool cue);
#endif

extern bool gCW_CpoActive;
extern bool gCW_CpoBacklightOn;
#endif
