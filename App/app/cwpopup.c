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

#define CW_POPUP_SHOW_10MS     200  // 2 s after the last key press
#define CW_POPUP_CONFIRM_10MS  500  // 5 s to confirm a key input with MENU before it's dropped
#define CW_WPM_MIN             10   // same range as the CWwpm menu
#define CW_WPM_MAX             45

static CW_PopupKind_t s_kind;
static uint16_t       s_10ms;           // idle time left, counted on the 10 ms tick so it's exact
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

// A new speed is already live, so it's kept (and saved) however the popup closes. A
// pending key input is applied only on confirm, which is MENU; everything else drops it.
static void Close(bool confirm)
{
	const CW_PopupKind_t kind = s_kind;

	s_kind = CW_POPUP_NONE;
	s_10ms = 0;
	gUpdateDisplay = true;

	if (kind == CW_POPUP_SPEED && s_speed_changed) {
		s_speed_changed = false;
		gRequestSaveSettings = true;
	}

	if (kind == CW_POPUP_KEY_INPUT && confirm)
		ApplyKeyInput();  // may reopen as CW_POPUP_KEY_STUCK
}

// Also restarts the idle time of the popup already showing
void CW_Popup_Show(CW_PopupKind_t kind)
{
	// switching to a different popup drops a pending key input
	if (s_kind != CW_POPUP_NONE && s_kind != kind)
		Close(false);

	s_kind = kind;
	s_10ms = (kind == CW_POPUP_KEY_INPUT) ? CW_POPUP_CONFIRM_10MS : CW_POPUP_SHOW_10MS;
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
		Close(false);
	else if (gScreenToDisplay == DISPLAY_MAIN)
		CW_Popup_Show(CW_POPUP_SPEED);
	else
		gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;  // up/down only reach the main screen
}

static void StepSpeed(int8_t direction)
{
	const int wpm = gEeprom.CW_KEY_WPM + direction;

	if (wpm < CW_WPM_MIN || wpm > CW_WPM_MAX) {
		gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	gEeprom.CW_KEY_WPM = wpm;
	CW_UpdateWPM();
	s_speed_changed = true;
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
	// PTT keeps keying; side keys go to their actions, see CW_Popup_OnAction
	if (s_kind == CW_POPUP_NONE || Key == KEY_PTT || Key == KEY_SIDE1 || Key == KEY_SIDE2)
		return false;

	if (Key == KEY_MENU || Key == KEY_EXIT) {
		// close on release so the release doesn't reach the main screen. Releasing a long
		// press does nothing, so an M long action that opened the popup leaves it up.
		if (!bKeyPressed && !bKeyHeld)
			Close(Key == KEY_MENU);
		return true;
	}

	if ((s_kind == CW_POPUP_SPEED || s_kind == CW_POPUP_KEY_INPUT) && (Key == KEY_UP || Key == KEY_DOWN)) {
		if (bKeyPressed) {  // first press and every auto-repeat while held
			if (s_kind == CW_POPUP_SPEED)
				StepSpeed(KeyDirection(Key));
			else
				StepKeyInput(KeyDirection(Key));
			CW_Popup_Show(s_kind);
		}
		return true;
	}

	// any other key closes the popup, dropping a pending key input, then does its usual job
	if (bKeyPressed && !bKeyHeld)
		Close(false);

	return false;
}

void CW_Popup_OnAction(uint8_t action)
{
	// the popup's own actions step, switch or close it themselves
	if (s_kind != CW_POPUP_NONE &&
	    action != ACTION_OPT_CW_KEYER_MODE &&
	    action != ACTION_OPT_CW_SPEED &&
	    action != ACTION_OPT_CW_KEY_INPUT)
		Close(false);
}

void CW_Popup_OnKeying(void)
{
	// keying keeps a new speed and drops a pending key input, since keying means the
	// current input is the one in use (and a pressed key would fail its check)
	if (s_kind != CW_POPUP_NONE)
		Close(false);
}

void CW_Popup_Tick10ms(void)
{
	if (s_kind == CW_POPUP_NONE)
		return;

	// PTT in any mode closes it like keying does
	if (gCurrentFunction == FUNCTION_TRANSMIT || --s_10ms == 0)
		Close(false);
}
