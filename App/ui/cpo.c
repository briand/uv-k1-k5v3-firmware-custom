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
#include "app/cpoqth.h"
#include "ui/cpo.h"
#include "ui/helper.h"

// Clear of a two-letter code on the large callsign line, with room for
// CPO_QTH_NAME_LINE_LEN small characters to the right edge
#define QTH_NAME_X 28

// Beside the code on the callsign line, wrapped at a space onto its second line
static void DrawQthName(const char *name)
{
	char line[CPO_QTH_NAME_LINE_LEN + 1];
	size_t cut = strlen(name);

	if (cut > CPO_QTH_NAME_LINE_LEN) {
		cut = CPO_QTH_NAME_LINE_LEN;
		while (cut > 0 && name[cut] != ' ') {
			cut--;
		}
	}
	memcpy(line, name, cut);
	line[cut] = '\0';
	UI_PrintStringSmallNormal(line, QTH_NAME_X, 0, 1);
	if (name[cut] == ' ') {
		UI_PrintStringSmallNormal(name + cut + 1, QTH_NAME_X, 0, 2);
	}
}

void UI_DisplayCPO(void)
{
	char String[24];
	const bool drill = gCW_CpoCallMode != CPO_CALL_MODE_OFF;
	// Drills use the frequency-sized font, which a callsign and its copy fit in easily;
	// plain practice keeps the narrower one so more free-form sending stays in view
	const uint8_t tx_len = CW_GetTxDisplayTail(String, drill ? UI_CW_LARGE_MAX_LEN + 1 : 17);

	static const char *const titles[] = {"Code Practice", "Send Callsigns", "Copy Callsigns", "Copy State/Country"};

	UI_DisplayClear();
	UI_PrintStringSmallNormal(titles[gCW_CpoCallMode], 0, 127, 0);
	if (tx_len > 0) {
		if (drill)
			UI_PrintStringCWLarge(String, 0, 3);
		else
			UI_PrintStringCW(String, 0, 0, 3);
	}
	if (drill) {
		// Left-aligned like the keyed line so each character sits above its copy
		char call_line[CPO_CALL_LINE_SIZE];
		CPO_Call_GetCallLine(call_line);
		UI_PrintStringCWLarge(call_line, 0, 1);
		const char *name = CPO_Call_GetName();
		if (name) {
			DrawQthName(name);
		}
		char notice[CPO_CALL_NOTICE_SIZE];
		if (CPO_Call_GetNotice(notice)) {
			UI_PrintStringSmallNormal(notice, 0, 127, 5);
		} else if (gCW_CpoCallResult == CPO_CALL_RESULT_HIT) {
			UI_PrintStringSmallNormal("OK", 0, 127, 5);
		} else if (gCW_CpoCallResult == CPO_CALL_RESULT_MISS) {
			UI_PrintStringSmallNormal("MISS", 0, 127, 5);
		}
		sprintf_(String, "%u/%u", gCW_CpoCallHits, gCW_CpoCallMisses);
		UI_PrintStringSmallNormal(String, 44, 104, 6);
	}
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
	ST7565_BlitFullScreen();
}
