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
#ifdef ENABLE_FEAT_NR7Y_DAC_SPIKE
#include "app/cpodac.h"
#endif
#include "ui/cpo.h"
#include "ui/helper.h"

void UI_DisplayCPO(void)
{
	char String[24];
	const bool drill = gCW_CpoCallMode != CPO_CALL_MODE_OFF;
	// Drills use the frequency-sized font, which a callsign and its copy fit in easily;
	// plain practice keeps the narrower one so more free-form sending stays in view
	const uint8_t tx_len = CW_GetTxDisplayTail(String, drill ? UI_CW_LARGE_MAX_LEN + 1 : 17);

	static const char *const titles[] = {"Code Practice", "Send Callsigns", "Copy Callsigns"};

	const char *title = titles[gCW_CpoCallMode];
#ifdef ENABLE_FEAT_NR7Y_DAC_SPIKE
	char dac_status[CPO_DAC_STATUS_SIZE];
	if (CPO_Dac_GetStatus(dac_status)) {
		title = dac_status;
	}
#endif

	UI_DisplayClear();
	UI_PrintStringSmallNormal(title, 0, 127, 0);
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
#ifdef ENABLE_FEAT_NR7Y_DAC_SPIKE
	// Key hint on line 5 whenever a drill's notice or result isn't using it
	char notice_check[CPO_CALL_NOTICE_SIZE];
	if (!drill || (!CPO_Call_GetNotice(notice_check) && gCW_CpoCallResult == CPO_CALL_RESULT_NONE)) {
		UI_PrintStringSmallNormal(CPO_Dac_GetHint(), 0, 127, 5);
	}
	// Synthesis settings and load on lines 1-2, which only drills use
	if (!drill && CPO_Dac_IsActive()) {
		char line[CPO_DAC_STATUS_SIZE];
		CPO_Dac_GetSynth(line);
		UI_PrintStringSmallNormal(line, 0, 127, 1);
		CPO_Dac_GetLoad(line);
		UI_PrintStringSmallNormal(line, 0, 127, 2);
	}
#endif
	ST7565_BlitFullScreen();
}
