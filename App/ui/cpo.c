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

#include <string.h>

#include "app/cwmacro.h"
#ifdef ENABLE_FLASHLIGHT
#include "app/cwkeyer.h"
#endif
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "font.h"
#include "settings.h"
#include "app/cpo.h"
#include "app/cpocall.h"
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
#include "app/cpoqso.h"
#endif
#include "ui/cpo.h"
#include "ui/helper.h"

// MENU's settings menu over the whole screen, laid out as the main menu: the row's
// name big in the left pane between its neighbours' small ones, its value big in the
// right pane, and the row count under the dotted rule, with a PRACTICE badge against
// the divider beside it that tells it from the main menu. Editing, the name moves to the
// top and its neighbours go, as in the main menu. On the key list the speed keys are
// arrows, drawn the way SetNav names the keys as the CW speed popup draws them: up and
// down on a UV-K5 (the left key is UP), left and right on a UV-K1.
static void DisplayMenu(const CPO_Menu_t *menu)
{
	char text[12];

	// The main menu's rounded badge at the foot of the value pane, so it doesn't read as
	// the value's title, its left edge on the divider (x 48-106). It goes down first: it
	// inverts what's under it, and the divider drawn over it stays whole.
	UI_PrintStringSmallNormalInverse("PRACTICE", 49, 0, 6);
	UI_DrawLineBuffer(gFrameBuffer, 48, 0, 48, 55, 1);
	for (uint8_t x = 0; x < 48; x += 2) {
		gFrameBuffer[5][x] = 0x40;
	}
	if (menu->editing) {
		UI_PrintString(menu->name, 0, 0, 0, 8);
	} else {
		UI_PrintStringSmallNormal(menu->prev, 0, 0, 1);
		UI_PrintString(menu->name, 0, 0, 2, 8);
		UI_PrintStringSmallNormal(menu->next, 0, 0, 4);
	}
	sprintf_(text, "%02u/%02u", menu->index + 1, menu->count);
	UI_PrintStringSmallNormal(text, 6, 0, 6);

	if (!menu->keys) {
		UI_PrintString(menu->value, 50, 127, 2, 8);
		return;
	}
	sprintf_(text, "5  %s", menu->again);
	UI_PrintStringSmallNormal("F  drill", 52, 0, 1);
	UI_PrintStringSmallNormal(text, 52, 0, 2);
	UI_PrintStringSmallNormal("*  light", 52, 0, 3);
	UI_PrintStringSmallNormal("    speed", 52, 0, 4);
	// in the first and third character cells, at their middles
	for (int8_t way = -1; way <= 1; way += 2) {
		const uint8_t x = 52 + 3 + 7 * (way + 1), y = 8 * 4 + 3;
		if (gEeprom.SET_NAV) {
			UI_DrawArrow(x, y + 2 * way, 0, way);
		} else {
			UI_DrawArrow(x + 2 * way, y, way, 0);
		}
	}
}

// Line 6, left to right: MENU's key cap (x 0-9), the speed, the score centered in the
// space up to the marks ("999/999" and "25(12)" fit), and the marks
#define SPEED_START 14   // 5 px clear of the key cap
#define SCORE_START 55
#define SCORE_END   107

// A key's name in the small font with its top row at pixel row y
static void DrawKey(char key, uint8_t x, uint8_t y)
{
	const uint8_t *glyph = gFontSmall[key - ' ' - 1];

	for (uint8_t i = 0; i < sizeof(gFontSmall[0]); i++) {
		const uint16_t column = (uint16_t)glyph[i] << (y % 8);
		gFrameBuffer[y / 8][x + i]     |= column;
		gFrameBuffer[y / 8 + 1][x + i] |= column >> 8;
	}
}

// A popup in the CW popups' box, over everything but the title and line 6. The key
// that changes it goes where the CW popups' arrows do; the drill picker gets the M,
// check mark and arrows of the keyer mode popup.
static void DisplayPopup(void)
{
	CPO_Popup_t popup;

	if (!CPO_GetPopup(&popup)) {
		return;
	}
	// clear the sides the box leaves, where the drill's big text would show
	for (uint8_t line = 1; line <= 5; line++) {
		memset(gFrameBuffer[line], 0, sizeof(gFrameBuffer[line]));
	}
	UI_DrawCWPopupBox(popup.title, popup.confirm);
	UI_PrintString(popup.value, 9, 118, 3, 8);
	if (popup.confirm) {
		UI_DrawCWPopupArrows();
		return;
	}
	DrawKey(popup.key, 12, 28);   // level with the value
}

// Plain practice and the callsign drills: your sending, and the drill's call, result
// and score
static void DisplayDrill(char *text, uint8_t text_len)
{
	const bool drill = gCW_CpoCallMode != CPO_CALL_MODE_OFF;

	if (text_len > 0) {
		if (drill)
			UI_PrintStringCWLarge(text, 0, 3);
		else
			UI_PrintStringCW(text, 0, 0, 3);
	}
	if (!drill) {
		return;
	}

	// Left-aligned like the keyed line so each character sits above its copy
	char call_line[CPO_CALL_LINE_SIZE];
	CPO_Call_GetCallLine(call_line);
	UI_PrintStringCWLarge(call_line, 0, 1);
	sprintf_(text, "%u/%u", gCW_CpoCallHits, gCW_CpoCallMisses);
	UI_PrintStringSmallNormal(text, SCORE_START, SCORE_END, 6);

	char notice[CPO_CALL_NOTICE_SIZE];
	if (CPO_Call_GetNotice(notice)) {
		UI_PrintStringSmallNormal(notice, 0, 127, 5);
	} else if (gCW_CpoCallResult == CPO_CALL_RESULT_HIT) {
		UI_PrintStringSmallNormal("OK", 0, 127, 5);
	} else if (gCW_CpoCallResult == CPO_CALL_RESULT_MISS) {
		UI_PrintStringSmallNormal("MISS", 0, 127, 5);
	}
}

#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
// The QSO drill: your sending, how to start until it knows your call, and QSOs logged
// and failed where the drills keep their score
static void DisplayQso(char *text, uint8_t text_len)
{
	char line[CPO_QSO_LINE_SIZE];

	if (CPO_Qso_GetHint(line)) {
		UI_PrintStringSmallNormal(line, 0, 127, 1);
	}
	if (text_len > 0) {
		UI_PrintStringCWLarge(text, 0, 3);
	}
	sprintf_(text, "%u/%u", gCW_CpoQsoLogged, gCW_CpoQsoFailed);
	UI_PrintStringSmallNormal(text, SCORE_START, SCORE_END, 6);
	if (CPO_Qso_GetNotice(line)) {
		UI_PrintStringSmallNormal(line, 0, 127, 5);
	}
}
#endif

void UI_DisplayCPO(void)
{
	char String[24];
	const bool drill = gCW_CpoCallMode != CPO_CALL_MODE_OFF;
	// Drills use the frequency-sized font, which a callsign and its copy fit in easily;
	// plain practice keeps the narrower one so more free-form sending stays in view
	const uint8_t tx_len = CW_GetTxDisplayTail(String, drill ? UI_CW_LARGE_MAX_LEN + 1 : 17);

	static const char *const titles[CPO_CALL_MODE_COUNT] = {
		"Code Practice", "Send Callsigns", "Copy Callsigns",
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
		"QSO Drill (%s)",   // its difficulty
#endif
	};

	CPO_Menu_t menu;

	UI_DisplayClear();
	if (CPO_GetMenu(&menu)) {
		DisplayMenu(&menu);
		ST7565_BlitFullScreen();
		return;
	}
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	char title[CPO_QSO_LINE_SIZE];
	sprintf_(title, titles[gCW_CpoCallMode], CPO_Qso_DifficultyName());
	UI_PrintStringSmallNormal(title, 0, 127, 0);
	if (gCW_CpoCallMode == CPO_CALL_MODE_QSO) {
		DisplayQso(String, tx_len);
	} else {
		DisplayDrill(String, tx_len);
	}
#else
	UI_PrintStringSmallNormal(titles[gCW_CpoCallMode], 0, 127, 0);
	DisplayDrill(String, tx_len);
#endif
	DisplayPopup();

	// Farnsworth spacing shows as character speed then effective speed, "25(12)", in
	// the space "25 WPM" takes. A slash would read like the score beside it.
	const bool copy = CPO_Call_IsCopyMode();
	const uint8_t eff_wpm = copy ? CPO_Call_EffectiveWPM() : 0;
	if (eff_wpm) {
		sprintf_(String, "%u(%u)", gEeprom.CW_KEY_WPM, eff_wpm);
	} else {
		sprintf_(String, "%u WPM", gEeprom.CW_KEY_WPM);
	}
	UI_PrintStringSmallNormal(String, SPEED_START, 0, 6);
	if (copy && gEeprom.CW_FARNSWORTH_AUTO) {
		UI_PrintStringSmallNormal("A", 114, 0, 6);
	}
    if (gCW_CpoBacklightOn) {
		UI_PrintStringSmallNormal("*", 107, 0, 6);
	}
#ifdef ENABLE_FLASHLIGHT
	if (gCW_FlashlightSending) {
		UI_PrintStringSmallNormal("^", 121, 0, 6);
	}
#endif
	// MENU's settings menu, as a key cap at the bottom left, by the MENU key on the radio
	UI_PrintStringSmallNormalInverse("M", 1, 0, 6);
	ST7565_BlitFullScreen();
}
