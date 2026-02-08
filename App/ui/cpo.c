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
#include "ui/cpo.h"
#include "ui/helper.h"

void UI_DisplayCPO(void)
{
	char String[24];
	const uint8_t tx_len = CW_GetTxDisplayTail(String, 17);

	UI_DisplayClear();
	UI_PrintStringSmallNormal("Code Practice", 0, 127, 0);
	if (tx_len > 0) {
		UI_PrintString(String, 0, 0, 3, 8);
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
