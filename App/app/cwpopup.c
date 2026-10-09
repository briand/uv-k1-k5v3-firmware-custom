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

// CW quick-settings popup state and its key handling

#include "app/cwkeyer.h"
#include "app/cwpopup.h"
#include "audio.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/ui.h"

#define CW_POPUP_SHOW_500MS  4    // 2 s after the last key press
#define CW_WPM_MIN           10   // same range as the CWwpm menu
#define CW_WPM_MAX           45

static CW_PopupKind_t s_kind;
static uint8_t        s_500ms;
static uint8_t        s_key_input;      // input index shown while the key input popup is up
static bool           s_speed_changed;  // saved once on close instead of on every step

static void ApplyKeyInput(void)
{
	if (s_key_input == gEeprom.CW_KEY_INPUT_MENU)
		return;

	const uint8_t mode = CW_KEY_INPUT_menu_to_bitmap[s_key_input];

	gFlagReconfigureVfos = true;
	gRequestSaveSettings = true;

	if (CW_CheckKeyerInputs(mode)) {
		gEeprom.CW_KEY_INPUT      = mode;
		gEeprom.CW_KEY_INPUT_MENU = s_key_input;
		return;
	}

	// same fallback as the menu: a stuck key leaves the radio on the PTT handkey
	gEeprom.CW_KEY_INPUT      = CW_KEY_INPUT_HANDKEY;
	gEeprom.CW_KEY_INPUT_MENU = 0;
	s_key_input = 0;
	CW_Popup_Show(CW_POPUP_KEY_STUCK);
}

// confirm keeps what the popup was adjusting: speed is already live, a pending key
// input gets applied. Without it a pending key input is dropped.
static void Close(bool confirm)
{
	const CW_PopupKind_t kind = s_kind;

	s_kind = CW_POPUP_NONE;
	s_500ms = 0;
	gUpdateDisplay = true;

	if (kind == CW_POPUP_SPEED && s_speed_changed) {
		s_speed_changed = false;
		gRequestSaveSettings = true;
	}

	if (kind == CW_POPUP_KEY_INPUT && confirm)
		ApplyKeyInput();  // may reopen as CW_POPUP_KEY_STUCK
}

void CW_Popup_Show(CW_PopupKind_t kind)
{
	// switching to a different popup counts as moving on from the old one
	if (s_kind != CW_POPUP_NONE && s_kind != kind)
		Close(true);

	s_kind = kind;
	s_500ms = CW_POPUP_SHOW_500MS;
	gUpdateDisplay = true;
}

void CW_Popup_Dismiss(CW_PopupKind_t kind)
{
	if (s_kind == kind)
		Close(false);
}

CW_PopupKind_t CW_Popup_Kind(void)
{
	return s_kind;
}

uint8_t CW_Popup_KeyInput(void)
{
	return s_key_input;
}

void CW_Popup_Speed(void)
{
	if (s_kind == CW_POPUP_SPEED)
		Close(true);
	else if (gScreenToDisplay == DISPLAY_MAIN)
		CW_Popup_Show(CW_POPUP_SPEED);
	else
		gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;  // up/down only reach the main screen
}

// Move the pending key input one place through the CWkey menu's list, wrapping
static void StepKeyInput(int8_t direction)
{
	const uint8_t count = ARRAY_SIZE(CW_KEY_INPUT_menu_to_bitmap);

	s_key_input = (uint8_t)((s_key_input + count + direction) % count);
}

// Up/down as +1/-1, flipped like the menu and dial when SetNav is off
static int8_t KeyDirection(KEY_Code_t Key)
{
	const int8_t direction = (Key == KEY_UP) ? 1 : -1;

	return gEeprom.SET_NAV ? direction : -direction;
}

void CW_Popup_StepKeyInput(void)
{
	if (s_kind != CW_POPUP_KEY_INPUT) {
		if (gScreenToDisplay != DISPLAY_MAIN) {
			gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;  // nowhere to show the pending choice
			return;
		}
		s_key_input = gEeprom.CW_KEY_INPUT_MENU;
	}
	else {
		StepKeyInput(1);
	}

	CW_Popup_Show(CW_POPUP_KEY_INPUT);
}

bool CW_Popup_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	// PTT keeps keying, side keys keep their actions (including stepping or closing this popup)
	if (s_kind == CW_POPUP_NONE || Key == KEY_PTT || Key == KEY_SIDE1 || Key == KEY_SIDE2)
		return false;

	if (Key == KEY_EXIT) {
		// close on release so the release doesn't reach the main screen; EXIT keeps the
		// speed but drops a pending key input
		if (!bKeyPressed && !bKeyHeld)
			Close(s_kind != CW_POPUP_KEY_INPUT);
		return true;
	}

	if (s_kind == CW_POPUP_KEY_INPUT && (Key == KEY_UP || Key == KEY_DOWN)) {
		if (bKeyPressed) {  // first press and every auto-repeat while held
			StepKeyInput(KeyDirection(Key));
			s_500ms = CW_POPUP_SHOW_500MS;
			gUpdateDisplay = true;
		}
		return true;
	}

	if (s_kind == CW_POPUP_SPEED && (Key == KEY_UP || Key == KEY_DOWN)) {
		if (bKeyPressed) {  // first press and every auto-repeat while held
			const int wpm = gEeprom.CW_KEY_WPM + KeyDirection(Key);
			if (wpm < CW_WPM_MIN || wpm > CW_WPM_MAX) {
				gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
			} else {
				gEeprom.CW_KEY_WPM = wpm;
				CW_UpdateWPM();
				s_speed_changed = true;
			}

			s_500ms = CW_POPUP_SHOW_500MS;
			gUpdateDisplay = true;
		}
		return true;
	}

	// any other key keeps the new speed, closes the popup and then does its usual job
	if (s_kind == CW_POPUP_SPEED && bKeyPressed && !bKeyHeld)
		Close(true);

	return false;
}

void CW_Popup_OnKeying(void)
{
	// keying keeps a new speed; a pending key input is dropped, since keying means
	// the current input is the one in use (and a pressed key would fail its check)
	if (s_kind != CW_POPUP_NONE)
		Close(s_kind != CW_POPUP_KEY_INPUT);
}

void CW_Popup_Tick500ms(void)
{
	if (s_kind == CW_POPUP_NONE)
		return;

	if (gCurrentFunction == FUNCTION_TRANSMIT)
		CW_Popup_OnKeying();  // PTT in any mode
	else if (--s_500ms == 0)
		Close(true);
}
