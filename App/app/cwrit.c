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

// CW RIT/XIT offset and its adjust mode

#include <string.h>

#include "app/cwkeyer.h"
#include "app/cwrit.h"
#include "driver/bk4819.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/ui.h"

#include "external/printf/printf.h"

#define CW_RIT_LIMIT        999  // +/-9.99 kHz in 10 Hz units
#define CW_RIT_IDLE_10MS    500  // 5 s without a key closes adjust mode

static int16_t  s_offset;        // 10 Hz units, shared by RIT and XIT
static bool     s_rit_on;
static bool     s_xit_on;
static uint16_t s_adjust_10ms;   // non-zero while adjust mode is open; counts down to close it

uint32_t CW_RIT_RxFrequency(const VFO_Info_t *pVfo, uint32_t frequency)
{
	if (pVfo->Modulation != MODULATION_CW)
		return frequency;

	if (!gCW_CrossMode)
		frequency -= gEeprom.CW_TONE_FREQUENCY;  // CW BFO offset

	// RIT follows the main VFO only, the one the tag is drawn on
	if (s_rit_on && pVfo == gTxVfo)
		frequency = (uint32_t)((int32_t)frequency + s_offset);

	return frequency;
}

uint32_t CW_RIT_TxFrequency(const VFO_Info_t *pVfo, uint32_t frequency)
{
	if (pVfo->Modulation != MODULATION_CW)
		return frequency;

	if (gCW_CrossMode)
		frequency += gEeprom.CW_TONE_FREQUENCY;

	if (s_xit_on)
		frequency = (uint32_t)((int32_t)frequency + s_offset);

	return frequency;
}

// Retune the receiver in place, like a VFO step does, after the RIT frequency changes
static void Retune(void)
{
	if (gCurrentFunction == FUNCTION_TRANSMIT || gRxVfo != gTxVfo)
		return;  // picked up by the next RADIO_SetupRegisters()

	BK4819_SetFrequency(CW_RIT_RxFrequency(gRxVfo, gRxVfo->pRX->Frequency));
	BK4819_RX_TurnOn();
}

static void Nudge(int16_t delta)
{
	int16_t next = s_offset + delta;

	if (next > CW_RIT_LIMIT || next < -CW_RIT_LIMIT) {
		next = (next > 0) ? CW_RIT_LIMIT : -CW_RIT_LIMIT;
		CW_ErrorBeep();
	}

	s_offset = next;
	Retune();
}

// Retune the dial by the offset and zero it, so receive and transmit both land on the station
static void MoveToDial(void)
{
	const uint32_t dial = (uint32_t)((int32_t)gTxVfo->freq_config_RX.Frequency + s_offset);

	if (!IS_FREQ_CHANNEL(gEeprom.ScreenChannel[gEeprom.TX_VFO])   // memory channels keep their frequency
	    || s_offset == 0 || !(s_rit_on || s_xit_on)
	    || dial < frequencyBandTable[gTxVfo->Band].lower || dial > frequencyBandTable[gTxVfo->Band].upper
	    || RX_freq_check(dial) < 0) {
		CW_ErrorBeep();
		return;
	}

	gTxVfo->freq_config_RX.Frequency = dial;
	s_offset = 0;
	Retune();
	gRequestSaveChannel = 1;
}

bool CW_RIT_IsAdjusting(void)
{
	return s_adjust_10ms > 0;
}

static void ExitAdjust(void)
{
	s_adjust_10ms = 0;
	gUpdateDisplay = true;
}

void CW_RIT_EnterAdjust(void)
{
	if (gTxVfo->Modulation != MODULATION_CW || gScreenToDisplay != DISPLAY_MAIN) {
		CW_ErrorBeep();
		return;
	}

	// opening with both switches off means "listen off frequency"
	if (!s_rit_on && !s_xit_on) {
		s_rit_on = true;
		Retune();
	}

	s_adjust_10ms = CW_RIT_IDLE_10MS;
	gUpdateDisplay = true;
}


void CW_RIT_OnKeying(void)
{
	if (CW_RIT_IsAdjusting())
		ExitAdjust();
}

void CW_RIT_Tick10ms(void)
{
	if (s_adjust_10ms == 0)
		return;

	if (gTxVfo->Modulation != MODULATION_CW || gScreenToDisplay != DISPLAY_MAIN || --s_adjust_10ms == 0)
		ExitAdjust();
	else if (s_adjust_10ms % 50 == 0)
		gUpdateDisplay = true;  // keep the adjust modal's signal readout live, every 500 ms
}

bool CW_RIT_ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	// PTT keeps keying, side keys keep their actions (including closing this mode)
	if (!CW_RIT_IsAdjusting() || Key == KEY_PTT || Key == KEY_SIDE1 || Key == KEY_SIDE2)
		return false;

	const bool pressed = bKeyPressed && !bKeyHeld;

	s_adjust_10ms = CW_RIT_IDLE_10MS;
	gUpdateDisplay = true;

	switch (Key) {
		case KEY_UP:
		case KEY_DOWN:
			if (bKeyPressed) {  // first press and every auto-repeat while held
				int8_t direction = (Key == KEY_UP) ? 1 : -1;
				if (!gEeprom.SET_NAV)
					direction = -direction;  // same arrow orientation as dial tuning
				Nudge(direction);
			}
			break;

		// jog pad: columns are direction, rows are 10 Hz / 100 Hz / 1 kHz
		case KEY_1: if (pressed) Nudge(-1);   break;
		case KEY_3: if (pressed) Nudge(1);    break;
		case KEY_4: if (pressed) Nudge(-10);  break;
		case KEY_6: if (pressed) Nudge(10);   break;
		case KEY_7: if (pressed) Nudge(-100); break;
		case KEY_9: if (pressed) Nudge(100);  break;

		case KEY_2:
			if (pressed) {
				s_rit_on = !s_rit_on;
				Retune();
			}
			break;

		case KEY_8:
			if (pressed)
				s_xit_on = !s_xit_on;  // applied when the next transmission starts
			break;

		case KEY_5:
			if (pressed) {
				s_offset = 0;
				Retune();
			}
			break;

		case KEY_0:
			if (pressed)
				MoveToDial();
			break;

		case KEY_EXIT:
		case KEY_STAR:
			// close on release, so the release of * can't fall through and reopen it
			if (!bKeyPressed && !bKeyHeld)
				ExitAdjust();
			break;

		default:  // MENU, F
			if (pressed)
				CW_ErrorBeep();
			break;
	}

	return true;
}

int16_t CW_RIT_Offset(void)
{
	return s_offset;
}

bool CW_RIT_RitOn(void)
{
	return s_rit_on;
}

bool CW_RIT_XitOn(void)
{
	return s_xit_on;
}

void CW_RIT_FormatOffset(char *buf)
{
	const unsigned int magnitude = (s_offset < 0) ? -s_offset : s_offset;
	sprintf(buf, "%c%u.%02u", (s_offset < 0) ? '-' : '+', magnitude / 100, magnitude % 100);
}

bool CW_RIT_TagVisible(void)
{
	return gTxVfo->Modulation == MODULATION_CW && (s_rit_on || s_xit_on);
}

void CW_RIT_FormatTag(char *buf)
{
	strcpy(buf, s_rit_on ? (s_xit_on ? "RX" : "R") : "X");
	CW_RIT_FormatOffset(buf + strlen(buf));
}
