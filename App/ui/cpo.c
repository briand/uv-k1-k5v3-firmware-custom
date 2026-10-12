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
#include "settings.h"
#include "app/cpo.h"
#include "app/cpocall.h"
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
#include "app/cpoqso.h"
#endif
#include "ui/cpo.h"
#include "ui/helper.h"

// MENU's controls card, in place of the whole screen
static void DisplayCard(void)
{
	const char *lines[CPO_CARD_LINES];
	const uint8_t n = CPO_GetCard(lines);

	UI_PrintStringSmallBold("Controls", 0, 127, 0);
	for (uint8_t i = 0; i < n; i++) {
		UI_PrintStringSmallNormal(lines[i], 2, 0, i + 1);
	}
}

// Plain practice and the callsign drills: your sending, and the drill's call, result
// and score. True when the drill has line 5.
static bool DisplayDrill(char *text, uint8_t text_len)
{
	const bool drill = gCW_CpoCallMode != CPO_CALL_MODE_OFF;

	if (text_len > 0) {
		if (drill)
			UI_PrintStringCWLarge(text, 0, 3);
		else
			UI_PrintStringCW(text, 0, 0, 3);
	}
	if (!drill) {
		return false;
	}

	// Left-aligned like the keyed line so each character sits above its copy
	char call_line[CPO_CALL_LINE_SIZE];
	CPO_Call_GetCallLine(call_line);
	UI_PrintStringCWLarge(call_line, 0, 1);
	sprintf_(text, "%u/%u", gCW_CpoCallHits, gCW_CpoCallMisses);
	UI_PrintStringSmallNormal(text, 44, 104, 6);

	char notice[CPO_CALL_NOTICE_SIZE];
	if (CPO_Call_GetNotice(notice)) {
		UI_PrintStringSmallNormal(notice, 0, 127, 5);
	} else if (gCW_CpoCallResult == CPO_CALL_RESULT_HIT) {
		UI_PrintStringSmallNormal("OK", 0, 127, 5);
	} else if (gCW_CpoCallResult == CPO_CALL_RESULT_MISS) {
		UI_PrintStringSmallNormal("MISS", 0, 127, 5);
	} else {
		return false;
	}
	return true;
}

#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
// The QSO drill: what to do and who you're working above your sending, or the score
// once the QSO has ended. True when it has line 5.
static bool DisplayQso(const char *text, uint8_t text_len)
{
	char lines[5][CPO_QSO_LINE_SIZE];

	if (CPO_Qso_GetScore(lines)) {
		for (uint8_t i = 0; i < 5; i++) {
			UI_PrintStringSmallNormal(lines[i], 0, 127, i + 1);
		}
		return true;
	}
	CPO_Qso_GetStatus(lines[0], lines[1]);
	UI_PrintStringSmallNormal(lines[0], 0, 127, 1);
	UI_PrintStringSmallNormal(lines[1], 0, 127, 2);
	if (text_len > 0) {
		UI_PrintStringCWLarge(text, 0, 3);
	}
	if (CPO_Qso_GetNotice(lines[2])) {
		UI_PrintStringSmallNormal(lines[2], 0, 127, 5);
		return true;
	}
	return false;
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
		"QSO",
#endif
	};

	UI_DisplayClear();
	if (CPO_CardShowing()) {
		DisplayCard();
		ST7565_BlitFullScreen();
		return;
	}
	UI_PrintStringSmallNormal(titles[gCW_CpoCallMode], 0, 127, 0);

	bool line5_taken;
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	if (gCW_CpoCallMode == CPO_CALL_MODE_QSO) {
		line5_taken = DisplayQso(String, tx_len);
	} else {
		line5_taken = DisplayDrill(String, tx_len);
	}
#else
	line5_taken = DisplayDrill(String, tx_len);
#endif

	// Farnsworth spacing shows as character speed then effective speed, "25(12)", in
	// the space "25 WPM" takes. A slash would read like the score beside it.
	const bool copy = CPO_Call_IsCopyMode();
	const uint8_t eff_wpm = copy ? CPO_Call_EffectiveWPM() : 0;
	if (eff_wpm) {
		sprintf_(String, "%u(%u)", gEeprom.CW_KEY_WPM, eff_wpm);
	} else {
		sprintf_(String, "%u WPM", gEeprom.CW_KEY_WPM);
	}
	UI_PrintStringSmallNormal(String, 2, 0, 6);
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
	if (!line5_taken) {
		char hint[CPO_HINT_LINE_SIZE];
		CPO_GetHintLine(hint);
		UI_PrintStringSmallNormal(hint, 0, 127, 5);
	}
	ST7565_BlitFullScreen();
}
