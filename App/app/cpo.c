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

// Code practice (CPO) app skeleton

#include "app/cpo.h"
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/ui.h"
#ifdef ENABLE_FLASHLIGHT
#include "driver/gpio.h"
#include "py32f071_ll_gpio.h"
#endif
#ifdef ENABLE_CW_MODULATOR
#include "app/cwkeyer.h"
#include "app/cwmacro.h"
#endif

#ifdef ENABLE_CODE_PRACTICE

bool gCW_CpoActive = false;
bool gCW_CpoBacklightOn = false;
static bool s_needs_redraw = false;
bool wpm_changed = false;
static bool s_flashlight_sending = false;

void CPO_Enter(void)
{
    CW_KeyerReconfigure(true);
	gCW_CpoActive = true;
	s_needs_redraw = true;
	gRequestDisplayScreen = DISPLAY_CPO;
	gUpdateDisplay = true;
	gMonitor = false;
    wpm_changed = false;
    gCW_FlashlightSending = s_flashlight_sending;
	BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
	BK4819_WriteRegister(BK4819_REG_3F, 0x0000);        // Disable interrupts
	BK4819_SetAF(BK4819_AF_ALAM);
	AUDIO_AudioPathOn();
}

void CPO_Exit(void)
{
#ifdef ENABLE_FLASHLIGHT
	gCW_FlashlightSending = false;
	GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
#endif
	gCW_CpoActive = false;
	gRequestDisplayScreen = DISPLAY_MAIN;
	gUpdateDisplay = true;
	gUpdateStatus = true;
	// Reconfigure radio/UI back to normal path, but do not force a keyer deinit here.
	// This avoids a brief window where generic PTT can race before CW keyer resumes ownership.
	gFlagReconfigureVfos = true;
	CW_KeyerResetRuntime();
    if( wpm_changed ) {
        gRequestSaveSettings = true;
    }
}

void CPO_Tick(void)
{
	if (!gCW_CpoActive) {
		return;
	}

	if (gCW_CpoBacklightOn) {
		gBacklightCountdown_500ms = 2;
	}

	if (s_needs_redraw | gCW_TX_DisplayUpdated) {
		s_needs_redraw = false;
		gRequestDisplayScreen = DISPLAY_CPO;
		gUpdateDisplay = true;
	}
}

void CPO_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	if (!bKeyPressed || bKeyHeld) {
		return;
	}

	switch (Key) {

	case KEY_UP:
		if (gEeprom.CW_KEY_WPM < 30) {
			gEeprom.CW_KEY_WPM++;
#ifdef ENABLE_CW_MODULATOR
			CW_UpdateWPM();
#endif
			gUpdateDisplay = true;
            wpm_changed = true;
		}
		break;

	case KEY_DOWN:
		if (gEeprom.CW_KEY_WPM > 10) {
			gEeprom.CW_KEY_WPM--;
#ifdef ENABLE_CW_MODULATOR
			CW_UpdateWPM();
#endif
			gUpdateDisplay = true;
            wpm_changed = true;
        }
		break;

	case KEY_STAR:
		gCW_CpoBacklightOn = !gCW_CpoBacklightOn;
		if (gCW_CpoBacklightOn) {
			BACKLIGHT_TurnOn();
			gBacklightCountdown_500ms = 2;
            gUpdateDisplay = true;
		} else {
			BACKLIGHT_TurnOff();
            gUpdateDisplay = true;
		}
		break;

	case KEY_4:
#ifdef ENABLE_FLASHLIGHT
		gCW_FlashlightSending = !gCW_FlashlightSending;
        s_flashlight_sending = gCW_FlashlightSending;
		gUpdateDisplay = true;
#endif
		break;

	default:
		break;
	}
}

#endif
