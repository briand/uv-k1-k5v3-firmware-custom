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

// CW application-level update loop and end-of-transmission handling
// Extracted from app.c to give CW its own independent PTT / EOT path.

#include <stdint.h>
#include <stdbool.h>

#include "app/cwapp.h"
#include "app/cwkeyer.h"
#include "app/cwmacro.h"
#include "app/app.h"
#include "app/menu.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "py32f071_ll_gpio.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "driver/system.h"
#include "driver/timer.h"
#ifdef ENABLE_CODE_PRACTICE
#include "app/cpo.h"
#endif
#ifdef ENABLE_FLASHLIGHT
#include "app/flashlight.h"
#endif

// ---------------------------------------------------------------------------
// CW_EndTxNow  –  end CW transmission immediately and return to monitor
// ---------------------------------------------------------------------------
void CW_EndTxNow(void)
{
    // Clear CW state when ending transmission entirely
    gCW_State = CW_INACTIVE;

	// Call the common end-of-transmission (sends tail, resets TX regs)
	APP_EndTransmission();

	// Go straight to FOREGROUND
	FUNCTION_Select(FUNCTION_FOREGROUND);

	gFlagEndTransmission = false;

#ifdef ENABLE_VOX
	gVOX_NoiseDetected = false;
#endif

	RADIO_SetVfoState(VFO_STATE_NORMAL);  // only variables
    RADIO_SelectVfos();                   // only variables
    APP_StartListening(FUNCTION_MONITOR);  // does AudioPathOn
}

// ---------------------------------------------------------------------------
// CW_AppUpdate  –  called from main.c at approx 1ms cadence (sometimes a little longer)
// ---------------------------------------------------------------------------
void CW_AppUpdate(void)
{
	if (gF_LOCK)  // don't init or run the keyer in "hidden menu" tech mode
		return;

	if (!(gTxVfo->Modulation == MODULATION_CW
#ifdef ENABLE_CODE_PRACTICE
		|| gCW_CpoActive
#endif
	))
	{
		// Not in CW mode – paranoid check to end CW TX
		if (gCW_State != CW_INACTIVE)
		{
			CW_EndTxNow();
		}
        return;  // not in CW mode, nothing else to do
	}

	// ---- poll the keyer / playback engine for the next action ----
	CW_Action_t action;
	if (gCW_PlaybackActive)
		action = CW_PlaybackHandleState();
	else
		action = CW_HandleState();

	// ---- local-only sidetone path (no RF) ----
	// Used when recording a macro, reading ADC, breakin disabled, or code practice
	if (gCW_Recording || gCW_AdcReadActive || !gEeprom.CW_BREAKIN_ENABLE
#ifdef ENABLE_CODE_PRACTICE
		|| gCW_CpoActive
#endif
		) {
		switch (action)
		{
			case CW_ACTION_CARRIER_ON:
				BK4819_SetAF(BK4819_AF_ALAM);
				BK4819_WriteRegister(BK4819_REG_70,
					BK4819_REG_70_ENABLE_TONE1 |
					(gEeprom.CW_SIDETONE_LEVEL << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
				BK4819_SetScrambleFrequencyControlWord(gEeprom.CW_TONE_FREQUENCY * 10);
				#ifdef ENABLE_FLASHLIGHT
				if (gCW_FlashlightSending) {
					GPIO_SetOutputPin(GPIO_PIN_FLASHLIGHT);
				}
				#endif
				gCW_TxDisplayHoldoff_10ms = 200;
			break;

			case CW_ACTION_CARRIER_OFF:
				BK4819_SetScrambleFrequencyControlWord(0);
				#ifdef ENABLE_FLASHLIGHT
				if (gCW_FlashlightSending) {
					GPIO_ResetOutputPin(GPIO_PIN_FLASHLIGHT);
				}
				#endif
				#ifdef ENABLE_CODE_PRACTICE
				if (!gCW_CpoActive)  // just stay in ALAM for CPO
				#endif
					RADIO_SetModulation(gRxVfo->Modulation);
				gCW_TxDisplayHoldoff_10ms = 200;
			break;

			default:
			break;
		}
		// don't let RF happen
		action = CW_ACTION_NONE;
	}

	// ---- RF transmit path ----
	switch (action)
	{
		case CW_ACTION_CARRIER_ON:
			gTxTimerCountdown_500ms = 0;
			gCW_TxDisplayHoldoff_10ms = 200;
			gPttIsPressed = true;  // makes backlight come on, among other things

			if (gCW_State == CW_INACTIVE)
			{
				RADIO_PrepareTX();
			}
			else if (gCW_State == CW_SUSPENDED) {
				RADIO_CW_BeginResume();
                gCW_SuspendCounter_1ms = 0;
			}
			// if already CW_TRANSMITTING: no-op
		break;

		case CW_ACTION_CARRIER_OFF:
			// only suspend once, from active TX
			if (gCW_State == CW_TRANSMITTING) {
				RADIO_CW_Suspend();
				gCW_SuspendCounter_1ms = timer_millis();
			}
			gCW_TxDisplayHoldoff_10ms = 200;
		break;

		case CW_ACTION_CARRIER_HOLD_ON:
			gPttIsPressed = true;
			gTxTimerCountdown_500ms = 0;
			gCW_TxDisplayHoldoff_10ms = 200;

			// if hold arrives while suspended (shouldn't happen), resume once
			if (gCW_State == CW_SUSPENDED) {
				RADIO_CW_BeginResume();
			}
			gCW_SuspendCounter_1ms = timer_millis();
		break;

		case CW_ACTION_NONE:
			if(gCW_State == CW_TRANSMITTING) {
				// if we've been transmitting but now have no carrier, suspend
				RADIO_CW_Suspend();
				gCW_SuspendCounter_1ms = timer_millis();
			}
		default:
		break;
	}

	// ---- suspend timeout → end TX ----
	if (gCW_State == CW_SUSPENDED)
	{
		if (timer_millis_since(gCW_SuspendCounter_1ms) >= cw_suspend_limit_1ms) {
            gCW_SuspendCounter_1ms = 0;
            gCW_TxDisplayHoldoff_10ms = 200;
            gPttIsPressed = false;
			CW_EndTxNow();
		}
	}
}

// Perform CW-derived 10ms timeslice work previously located in app.c
void CW_TimeSlice10ms(void)
{
	// Handle CW macro recording updates
	if (gCW_Recording && gCW_RecordNewChar) {
		gCW_RecordNewChar = false;
		gUpdateDisplay = true;
	}

	// Update playback indicator
	CW_PlaybackIndicatorDeadline();

	// Decrement TX display holdoff timer (runs every 10ms tick regardless of TX state)
	if (gCW_TxDisplayHoldoff_10ms > 0) {
		if (--gCW_TxDisplayHoldoff_10ms == 0)
			gUpdateDisplay = true;  // Trigger screen refresh to switch away from CW display
	}
}
