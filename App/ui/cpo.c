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
		if (gCW_CpoCallResult == CPO_CALL_RESULT_HIT) {
			UI_PrintStringSmallNormal("OK", 0, 127, 5);
		} else if (gCW_CpoCallResult == CPO_CALL_RESULT_MISS) {
			UI_PrintStringSmallNormal("MISS", 0, 127, 5);
		}
		sprintf_(String, "%u/%u", gCW_CpoCallHits, gCW_CpoCallMisses);
		UI_PrintStringSmallNormal(String, 44, 104, 6);
	}
	sprintf_(String, "%u WPM", gEeprom.CW_KEY_WPM);
	UI_PrintStringSmallNormal(String, 2, 0, 6);
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
