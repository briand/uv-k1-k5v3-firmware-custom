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

#include <string.h>

#include "app/cpo.h"
#include "app/cpocall.h"
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
#include "app/cwsynth.h"
#include "external/printf/printf.h"
#endif
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "functions.h"
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
// Settings practice can change, as they were on entry: CPO_Exit saves only if one moved
static uint8_t s_entry_wpm;
static uint8_t s_entry_farnsworth_wpm;
static bool s_entry_farnsworth_auto;
static bool s_entry_noise;
static uint8_t s_entry_noise_level;
static bool s_flashlight_sending = false;
#ifdef ENABLE_CW_MODULATOR
static ModulationMode_t s_saved_modulation = MODULATION_CW;
#endif

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
// How long each line of the band noise hint stays up
#define CPO_NOISE_HINT_10MS 200

// Sidetone level to DAC gain: as loud as the BK4819's sidetone at the same level up
// to where the DAC tops out, about level 9 on a UV-K1, and the DAC's top above that
static const uint16_t SIDETONE_DAC_GAIN[CW_SIDETONE_LEVEL_MAX + 1] = {
	0, 12, 23, 46, 92, 183, 365, 728, 1453,
	CWSYNTH_GAIN_MAX, CWSYNTH_GAIN_MAX, CWSYNTH_GAIN_MAX, CWSYNTH_GAIN_MAX,
	CWSYNTH_GAIN_MAX, CWSYNTH_GAIN_MAX, CWSYNTH_GAIN_MAX,
};

static bool s_noise = false;    // the receiver on AF, practice tones on the DAC
static uint8_t s_hint = 0;      // which line of the hint is up
static uint8_t s_hint_10ms = 0;

static uint16_t SidetoneDacGain(void)
{
	return SIDETONE_DAC_GAIN[MIN(gEeprom.CW_SIDETONE_LEVEL, CW_SIDETONE_LEVEL_MAX)];
}

// The drill's station follows the sidetone pitch and level, which the CW menu sets
static void SetStationVoice(void)
{
	CWSYNTH_SetVoice(CWSYNTH_STATION, gEeprom.CW_TONE_FREQUENCY * 10, SidetoneDacGain());
}

static void NoiseStart(void)
{
	// The receiver back on AF on the VFO's frequency, through the CW filter. The
	// squelch stays open: practice doesn't service the BK4819's interrupts.
	RADIO_SetModulation(MODULATION_CW);
	BK4819_SetFilterBandwidth(RADIO_ResolveFilter(gTxVfo), true);
	BK4819_SetAfDacGain(gEeprom.CW_PRACTICE_NOISE_LEVEL);
	CWSYNTH_Start();
	SetStationVoice();
	s_noise = true;
	s_hint = 0;
	s_hint_10ms = 0;
}

static void NoiseStop(void)
{
	s_noise = false;
	CWSYNTH_Stop();
	// Practice's quiet state, as CPO_Enter leaves it
	BK4819_SetAF(BK4819_AF_ALAM);
	BK4819_SetScrambleFrequencyControlWord(0);
}

static void StepNoiseLevel(bool louder)
{
	uint8_t level = gEeprom.CW_PRACTICE_NOISE_LEVEL;

	if (!s_noise) {
		return;
	}
	if (louder ? level >= CW_PRACTICE_NOISE_LEVEL_MAX : level == 0) {
		CW_ErrorBeep();
		return;
	}
	level = louder ? level + 1 : level - 1;
	gEeprom.CW_PRACTICE_NOISE_LEVEL = level;
	BK4819_SetAfDacGain(level);
	s_hint = 0;   // show the level
	s_hint_10ms = 0;
	gUpdateDisplay = true;
}

bool CPO_NoiseOn(void)
{
	return s_noise;
}

void CPO_NoiseSidetone(bool down, bool cue)
{
	if (down) {
		CWSYNTH_SetVoice(CWSYNTH_SIDETONE, cue ? CW_ERROR_TONE_HZ : gEeprom.CW_TONE_FREQUENCY * 10,
			SidetoneDacGain());
	}
	CWSYNTH_Key(CWSYNTH_SIDETONE, down);
}

void CPO_GetNoiseLine(char out[CPO_NOISE_LINE_SIZE])
{
	if (!s_noise) {
		strcpy(out, "9 band noise");
	} else if (s_hint == 0) {
		sprintf_(out, "Band noise %u", gEeprom.CW_PRACTICE_NOISE_LEVEL + 1);
	} else {
		strcpy(out, "3/6 level  9 off");
	}
}
#endif

void CPO_Enter(void)
{
	if (gCW_CpoActive) {
		return;
	}

#ifdef ENABLE_CW_MODULATOR
	// A reconfigure would only hand the paddle and PTT inputs to the keyer while
	// the VFO reads as CW, so borrow CW modulation for the session and put the real
	// one back in CPO_Exit. This is what lets practice work from FM/AM/USB. The
	// borrowed value cannot reach EEPROM: only gRequestSaveChannel persists
	// Modulation, and ProcessKey routes every key to CPO_ProcessKeys while active.
	s_saved_modulation = gTxVfo->Modulation;
	gTxVfo->Modulation = MODULATION_CW;
#endif

	// Set this before touching the radio: CheckRadioInterrupts bails out on it, so
	// squelch and RX events stop being serviced from here on.
	gCW_CpoActive = true;
	CW_KeyerReconfigure(true);

	s_needs_redraw = true;
	gRequestDisplayScreen = DISPLAY_CPO;
	gUpdateDisplay = true;
	s_entry_wpm = gEeprom.CW_KEY_WPM;
	s_entry_farnsworth_wpm = gEeprom.CW_FARNSWORTH_WPM;
	s_entry_farnsworth_auto = gEeprom.CW_FARNSWORTH_AUTO;
	s_entry_noise = gEeprom.CW_PRACTICE_NOISE;
	s_entry_noise_level = gEeprom.CW_PRACTICE_NOISE_LEVEL;
    gCW_FlashlightSending = s_flashlight_sending;

	// Park the radio for the session rather than asking for a VFO reconfigure --
	// a reconfigure ends in RADIO_SetupRegisters/ACTION_Monitor, which would put
	// the demodulator back on the audio path after we set ALAM below, leaving
	// over-the-air audio until the first element muted it again.
	//
	// FUNCTION_MONITOR is the quiet state to park in: its HandleFunction entry is
	// a no-op, and the 10ms scheduler only runs power save from FUNCTION_FOREGROUND
	// and dual watch outside FUNCTION_MONITOR. Power save in particular would call
	// BK4819_Sleep and switch the screen away mid-practice. FUNCTION_Select does
	// not touch the audio path for this state, so it is safe to call here.
	if (gCurrentFunction != FUNCTION_MONITOR) {
		FUNCTION_Select(FUNCTION_MONITOR);
	}
	gMonitor = false;   // parked, not actually monitoring

	// Go straight to the state practice settles into after the first element: the
	// alarm tone generator owns the audio path instead of the demodulator, so
	// nothing off the air is audible, and a zero control word keeps it silent until
	// the keyer speaks.
	BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
	BK4819_WriteRegister(BK4819_REG_3F, 0x0000);        // Disable interrupts
	BK4819_SetAF(BK4819_AF_ALAM);
	BK4819_SetScrambleFrequencyControlWord(0);
	AUDIO_AudioPathOn();
	gEnableSpeaker = true;
    CW_ClearTxDisplay();

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	if (gEeprom.CW_PRACTICE_NOISE) {
		NoiseStart();
	}
#endif
}

void CPO_Exit(void)
{
	// Paired with the guard in CPO_Enter: without this an unmatched exit would
	// write a stale s_saved_modulation over the live VFO.
	if (!gCW_CpoActive) {
		return;
	}

#ifdef ENABLE_FLASHLIGHT
	gCW_FlashlightSending = false;
	GPIO_ResetOutputPin(GPIO_PIN_FLASHLIGHT);
#endif
	CPO_Call_SetMode(CPO_CALL_MODE_OFF);
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	s_noise = false;
	CWSYNTH_StopNow();
#endif
	gCW_CpoActive = false;
	gRequestDisplayScreen = DISPLAY_MAIN;
	gUpdateDisplay = true;
	gUpdateStatus = true;
#ifdef ENABLE_CW_MODULATOR
	// Hand back the modulation CPO_Enter borrowed; the reconfigure below applies it
	// and drops the keyer's claim on PTT if we are no longer in CW.
	gTxVfo->Modulation = s_saved_modulation;
#endif
	// Leave the monitor state CPO_Enter parked in. The reconfigure below ends with
	// `if (gMonitor) ACTION_Monitor()`, and ACTION_Monitor toggles: called while
	// gCurrentFunction is still FUNCTION_MONITOR it would turn monitor off and
	// close the squelch instead of re-establishing normal RX for the restored mode.
	FUNCTION_Select(FUNCTION_FOREGROUND);

	// Reconfigure radio/UI back to normal path, but do not force a keyer deinit here.
	// This avoids a brief window where generic PTT can race before CW keyer resumes ownership.
	gFlagReconfigureVfos = true;
	CW_KeyerResetRuntime();
	if (gEeprom.CW_KEY_WPM != s_entry_wpm
		|| gEeprom.CW_FARNSWORTH_WPM != s_entry_farnsworth_wpm
		|| gEeprom.CW_FARNSWORTH_AUTO != s_entry_farnsworth_auto
		|| gEeprom.CW_PRACTICE_NOISE != s_entry_noise
		|| gEeprom.CW_PRACTICE_NOISE_LEVEL != s_entry_noise_level) {
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

	CPO_Call_Tick10ms();
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	CWSYNTH_Tick10ms();
	if (s_noise) {
		SetStationVoice();
		if (++s_hint_10ms >= CPO_NOISE_HINT_10MS) {
			s_hint_10ms = 0;
			s_hint ^= 1;
			gUpdateDisplay = true;
		}
	}
#endif

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
		if (gEeprom.CW_KEY_WPM < 45) {
			gEeprom.CW_KEY_WPM++;
#ifdef ENABLE_CW_MODULATOR
			CW_UpdateWPM();
#endif
			gUpdateDisplay = true;
		}
		break;

	case KEY_DOWN:
		if (gEeprom.CW_KEY_WPM > 10) {
			gEeprom.CW_KEY_WPM--;
#ifdef ENABLE_CW_MODULATOR
			CW_UpdateWPM();
#endif
			gUpdateDisplay = true;
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
	case KEY_5:
		CW_ClearTxDisplay();
		CPO_Call_Restart();
		gUpdateDisplay = true;
		break;

	case KEY_F:
		CPO_Call_NextMode();
		break;

	case KEY_2:
	case KEY_8:
		CPO_Call_StepSpacing(Key == KEY_2);
		break;

	case KEY_0:
		CPO_Call_ToggleAutoSpacing();
		break;

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	case KEY_9:
		gEeprom.CW_PRACTICE_NOISE = !gEeprom.CW_PRACTICE_NOISE;
		if (gEeprom.CW_PRACTICE_NOISE) {
			NoiseStart();
		} else {
			NoiseStop();
		}
		gUpdateDisplay = true;
		break;

	case KEY_3:
	case KEY_6:
		StepNoiseLevel(Key == KEY_3);
		break;
#endif

	default:
		break;
	}
}

#endif
