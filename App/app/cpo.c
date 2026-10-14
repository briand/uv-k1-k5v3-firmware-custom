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
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
#include "app/cpoqso.h"
#endif
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
#include "app/cwsynth.h"
#endif
#include "external/printf/printf.h"
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
#include "app/cwpopup.h"
#endif

#ifdef ENABLE_CODE_PRACTICE

bool gCW_CpoActive = false;
bool gCW_CpoBacklightOn = false;
static bool s_needs_redraw = false;
// Settings practice can change, as they were on entry: CPO_Exit saves only if one moved
static uint32_t s_entry_settings;   // the saved settings practice can change, at entry
#ifdef ENABLE_FLASHLIGHT
static bool s_flashlight_sending = false;
#endif

// The saved settings practice can change, packed into one value to compare
static uint32_t PracticeSettings(void)
{
	return gEeprom.CW_KEY_WPM                      // 6 bits
		| (uint32_t)gEeprom.CW_FARNSWORTH_WPM << 6     // 6
		| (uint32_t)gEeprom.CW_FARNSWORTH_AUTO << 12   // 1
		| (uint32_t)gEeprom.CW_PRACTICE_NOISE << 13    // 1
		| (uint32_t)gEeprom.CW_PRACTICE_NOISE_LEVEL << 14   // 4
		| (uint32_t)gEeprom.CW_QSO_DIFFICULTY << 18    // 2
		| (uint32_t)gEeprom.CW_QSO_PILE << 20          // 3
		| (uint32_t)gEeprom.CW_CALL_LEN << 23;         // 3
}
#ifdef ENABLE_CW_MODULATOR
static ModulationMode_t s_saved_modulation = MODULATION_CW;
#endif
static bool s_card = false;     // the controls card is up

// A setting's popup over the drill: see CPO_GetPopup
typedef enum {
	POPUP_NONE = 0,
	POPUP_DRILL,        // F: a drill to switch to, applied only on MENU
	POPUP_NOISE,        // 9 on or off, 3/6 its level
	POPUP_SPACING,      // 2/8, copy drill
	POPUP_AUTO,         // 0, copy drill
	POPUP_PILE,         // 1/7, QSO drill
	POPUP_DIFFICULTY,   // 4, QSO drill
	POPUP_FLASH,        // 4, the other drills
	POPUP_LIGHT,        // *
	POPUP_CALL_LEN,     // 1/7, callsign drills
} CPO_PopupKind_t;

static CPO_PopupKind_t s_popup = POPUP_NONE;
static uint16_t s_popup_10ms;   // idle time left, on the 10 ms tick like the CW popups'
static CPO_CallMode_t s_drill;  // the drill the picker shows

static bool QsoDrill(void)
{
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	return gCW_CpoCallMode == CPO_CALL_MODE_QSO;
#else
	return false;
#endif
}

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
// Sidetone level to DAC gain, out of 4096: as loud as the BK4819's sidetone at the
// same level up to where the DAC tops out at its full scale, just under level 9 on a
// UV-K1, and full scale above that. Full scale is fine for the sidetone alone: break-in
// silences the stations while it sounds.
static const uint16_t SIDETONE_DAC_GAIN[CW_SIDETONE_LEVEL_MAX + 1] = {
	0, 16, 32, 65, 130, 258, 516, 1029, 2053,
	4096, 4096, 4096, 4096, 4096, 4096, 4096,
};

static bool s_noise = false;    // band noise: the receiver on AF under the tones
static bool s_noise_muted = false;   // ...turned down for your sending
static bool s_tx_down = false;       // the sidetone is keyed, as the synthesizer has been told
static bool s_tx_transmit = false;   // ...for your sending, not a local cue


static bool s_synth = false;    // practice tones on the DAC

static uint16_t SidetoneDacGain(void)
{
	return SIDETONE_DAC_GAIN[MIN(gEeprom.CW_SIDETONE_LEVEL, CW_SIDETONE_LEVEL_MAX)];
}

// The drill's station follows the sidetone pitch and level, which the CW menu sets
static void SetStationVoice(void)
{
	CWSYNTH_SetVoice(CWSYNTH_STATION, gEeprom.CW_TONE_FREQUENCY * 10, CPO_StationGain());
}

// Tones go on the DAC under band noise, and always in the QSO drill, whose station
// can only be heard there; the sidetone joins it so the two balance
static void UpdateSynth(void)
{
	const bool want = s_noise || QsoDrill();

	if (want && !s_synth) {
		CWSYNTH_Start();
		CWSYNTH_SetHang(gEeprom.CW_HANG_10MS * 10u);
		SetStationVoice();
	} else if (!want && s_synth) {
		CWSYNTH_Stop();
		s_tx_down = false;
	}
	s_synth = want;
}

static void NoiseStart(void)
{
	// The receiver back on AF on the VFO's frequency, through the CW filter. The
	// squelch stays open: practice doesn't service the BK4819's interrupts.
	RADIO_SetModulation(MODULATION_CW);
	BK4819_SetFilterBandwidth(RADIO_ResolveFilter(gTxVfo), true);
	BK4819_SetAfDacGain(gEeprom.CW_PRACTICE_NOISE_LEVEL);
	s_noise = true;
	s_noise_muted = false;
	UpdateSynth();
}

static void NoiseStop(void)
{
	s_noise = false;
	s_noise_muted = false;
	UpdateSynth();
	// Practice's quiet state, as CPO_Enter leaves it
	BK4819_SetAF(BK4819_AF_ALAM);
	BK4819_SetScrambleFrequencyControlWord(0);
}

static void StepNoiseLevel(bool louder)
{
	uint8_t level = gEeprom.CW_PRACTICE_NOISE_LEVEL;

	// With the noise off its popup says so, and names 9
	if (!s_noise || (louder ? level >= CW_PRACTICE_NOISE_LEVEL_MAX : level == 0)) {
		CW_ErrorBeep();
		return;
	}
	level = louder ? level + 1 : level - 1;
	gEeprom.CW_PRACTICE_NOISE_LEVEL = level;
	BK4819_SetAfDacGain(level);
}

bool CPO_SynthOn(void)
{
	return s_synth;
}

uint16_t CPO_StationGain(void)
{
	// stations keep headroom: several can sound at once
	const uint16_t gain = SidetoneDacGain();
	return (gain < CWSYNTH_GAIN_MAX) ? gain : CWSYNTH_GAIN_MAX;
}

void CPO_Sidetone(bool down, bool cue)
{
	if (down) {
		CWSYNTH_SetVoice(CWSYNTH_SIDETONE, cue ? CW_ERROR_TONE_HZ : gEeprom.CW_TONE_FREQUENCY * 10,
			SidetoneDacGain());
		s_tx_transmit = !cue;
		// Your sending drops the band noise at once, under the sidetone's start, by its
		// AF DAC gain to the bottom, some 30 dB down. The BK4819 stays on receive audio:
		// leaving its tone generator clicks, and its mute pulls the DAC's sidetone down
		// with it. The tick puts the noise back once the hang time has run out.
		if (!cue && s_noise && !s_noise_muted) {
			s_noise_muted = true;
			BK4819_SetAfDacGain(0);
		}
	}
	s_tx_down = down;
	// The error dit is local, not sent: it breaks nothing in
	CWSYNTH_Key(CWSYNTH_SIDETONE, down, s_tx_transmit);
}

#endif

void CPO_OnChar(char ch, bool word_gap)
{
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	if (QsoDrill()) {
		CPO_Qso_OnChar(ch, word_gap);
		return;
	}
#endif
	(void)word_gap;
	CPO_Call_OnChar(ch);
}

bool CPO_CardShowing(void)
{
	return s_card;
}

uint8_t CPO_GetCard(const char *lines[CPO_CARD_LINES])
{
	uint8_t n = 0;

	lines[n++] = "F drill  < > wpm";   // the speed keys' arrows: see CPO_GetCard
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	lines[n++] = "9 noise  3/6 level";
#endif
	if (CPO_Call_IsCopyMode()) {
		lines[n++] = "2/8 gaps  0 auto";
		lines[n++] = "1/7 len  5 replay";
	} else if (QsoDrill()) {
		// 4 is the difficulty here, not the flashlight
		lines[n++] = "1/7 pile-up size";
		lines[n++] = "4 difficulty";
		lines[n++] = "5 abandon  * light";
	} else if (gCW_CpoCallMode == CPO_CALL_MODE_SEND) {
		lines[n++] = "1/7 len  5 restart";
	} else {
		lines[n++] = "5 clear the text";
	}
	if (!QsoDrill()) {
#ifdef ENABLE_FLASHLIGHT
		lines[n++] = "4 flash  * light";
#else
		lines[n++] = "* backlight";
#endif
	}
	lines[n++] = "EXIT quit practice";
	return n;
}

static void ShowPopup(CPO_PopupKind_t kind)
{
	s_popup = kind;
	// The picker waits for MENU as long as the menu does, as the keyer mode popup does
	s_popup_10ms = (kind == POPUP_DRILL) ? menu_timeout_500ms * 50u : CW_POPUP_SHOW_10MS;
	gUpdateDisplay = true;
}

static void SetDrill(CPO_CallMode_t mode)
{
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	const bool was_qso = QsoDrill();
#endif
	CPO_Call_SetMode(mode);
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	if (was_qso) {
		CPO_Qso_Exit();
	}
	if (QsoDrill()) {
		CPO_Qso_Enter();
	}
	UpdateSynth();
#endif
}

// confirm is MENU, which switches to the picker's drill unless it's the one in use;
// anything else drops it. Every other popup's change is already made.
static void ClosePopup(bool confirm)
{
	if (confirm && s_popup == POPUP_DRILL && s_drill != gCW_CpoCallMode) {
		SetDrill(s_drill);
	}
	s_popup = POPUP_NONE;
	gUpdateDisplay = true;
}

// The picker's drill a step through F's list, wrapping
static void StepDrill(int8_t direction)
{
	s_drill = (CPO_CallMode_t)((s_drill + CPO_CALL_MODE_COUNT + direction) % CPO_CALL_MODE_COUNT);
	ShowPopup(POPUP_DRILL);
}

void CPO_OnKeying(void)
{
	if (s_popup != POPUP_NONE) {
		ClosePopup(false);
	}
}

bool CPO_GetPopup(CPO_Popup_t *popup)
{
	static const char *const TITLES[] = {
		[POPUP_DRILL]      = "DRILL",
		[POPUP_NOISE]      = "BAND NOISE",
		[POPUP_SPACING]    = "SPACING",
		[POPUP_AUTO]       = "AUTO SPACING",
		[POPUP_PILE]       = "PILE-UP",
		[POPUP_DIFFICULTY] = "DIFFICULTY",
		[POPUP_FLASH]      = "FLASH SENDING",
		[POPUP_LIGHT]      = "BACKLIGHT",
		[POPUP_CALL_LEN]   = "CALL LENGTH",
	};
	static const char KEYS[][3] = {
		[POPUP_NOISE]      = "36",
		[POPUP_SPACING]    = "28",
		[POPUP_AUTO]       = "0",
		[POPUP_PILE]       = "17",
		[POPUP_DIFFICULTY] = "4",
		[POPUP_FLASH]      = "4",
		[POPUP_LIGHT]      = "*",
		[POPUP_CALL_LEN]   = "17",
	};
	static const char *const DRILLS[CPO_CALL_MODE_COUNT] = {
		"OFF", "SEND CALLS", "COPY CALLS",
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
		"QSO",
#endif
	};
	char *value = popup->value;
	bool on = false;

	if (s_popup == POPUP_NONE) {
		return false;
	}
	popup->title = TITLES[s_popup];
	memcpy(popup->keys, KEYS[s_popup], sizeof(popup->keys));
	popup->confirm = s_popup == POPUP_DRILL;

	switch (s_popup) {
	case POPUP_DRILL:
		strcpy(value, DRILLS[s_drill]);
		return true;
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	case POPUP_NOISE:
		if (s_noise) {
			sprintf_(value, "%u", gEeprom.CW_PRACTICE_NOISE_LEVEL + 1);
			return true;
		}
		strcpy(popup->keys, "9");   // off: the key that turns it on
		break;
#endif
	case POPUP_SPACING:
		if (CPO_Call_EffectiveWPM() != 0) {
			sprintf_(value, "%u WPM", CPO_Call_EffectiveWPM());
			return true;
		}
		break;
	case POPUP_AUTO:
		on = gEeprom.CW_FARNSWORTH_AUTO;
		break;
	case POPUP_CALL_LEN:
		sprintf_(value, "UP TO %u", gEeprom.CW_CALL_LEN);
		return true;
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	case POPUP_PILE:
		sprintf_(value, "%u CALLER%s", gEeprom.CW_QSO_PILE, (gEeprom.CW_QSO_PILE > 1) ? "S" : "");
		return true;
	case POPUP_DIFFICULTY:
		strcpy(value, CPO_Qso_DifficultyName());
		for (char *c = value; *c; c++) {
			if (*c >= 'a') {
				*c -= 'a' - 'A';
			}
		}
		return true;
#endif
#ifdef ENABLE_FLASHLIGHT
	case POPUP_FLASH:
		on = gCW_FlashlightSending;
		break;
#endif
	default:
		on = gCW_CpoBacklightOn;
		break;
	}
	strcpy(value, on ? "ON" : "OFF");
	return true;
}

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
	s_entry_settings = PracticeSettings();
#ifdef ENABLE_FLASHLIGHT
	gCW_FlashlightSending = s_flashlight_sending;
#endif

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

	s_card = false;
	s_popup = POPUP_NONE;
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
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	if (QsoDrill()) {
		CPO_Qso_Exit();
	}
#endif
	CPO_Call_SetMode(CPO_CALL_MODE_OFF);
	s_popup = POPUP_NONE;
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	s_noise = false;
	s_synth = false;
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
	if (PracticeSettings() != s_entry_settings) {
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
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	if (QsoDrill()) {
		CPO_Qso_Tick10ms();
	}
#endif
#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	CWSYNTH_Tick10ms();
	// The QSO drill sets its own station's pitch and level
	if (s_synth && !QsoDrill()) {
		SetStationVoice();
	}
	// Break-in follows the CW hang time, which the menu can change. Once it has run
	// out after your last element, the band noise comes back at its level.
	if (s_synth) {
		CWSYNTH_SetHang(gEeprom.CW_HANG_10MS * 10u);
		if (s_noise_muted && !s_tx_down && !CWSYNTH_Transmitting()) {
			s_noise_muted = false;
			BK4819_SetAfDacGain(gEeprom.CW_PRACTICE_NOISE_LEVEL);
		}
	}
#endif
	if (s_popup != POPUP_NONE && --s_popup_10ms == 0) {
		ClosePopup(false);
	}

	if (s_needs_redraw | gCW_TX_DisplayUpdated) {
		s_needs_redraw = false;
		gRequestDisplayScreen = DISPLAY_CPO;
		gUpdateDisplay = true;
	}
}

// Speed a step up or down, the way the CW speed popup goes (it follows SetNav), with
// the error dit at either end
static void StepWpm(int8_t direction)
{
	const int wpm = gEeprom.CW_KEY_WPM + direction;

	if (wpm < 10 || wpm > 45) {
		CW_ErrorBeep();
		return;
	}
	gEeprom.CW_KEY_WPM = wpm;
	CW_UpdateWPM();
	gUpdateDisplay = true;
}

void CPO_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	// Up/down step on the press and on every repeat while held, as the CW popups' do:
	// the drill in the picker, else the speed, which line 6 shows clear of any popup
	if ((Key == KEY_UP || Key == KEY_DOWN) && bKeyPressed && !s_card) {
		const int8_t direction = CW_Popup_KeyDirection(Key);
		if (s_popup == POPUP_DRILL) {
			StepDrill(direction);
		} else {
			ClosePopup(false);
			StepWpm(direction);
		}
		return;
	}

	if (!bKeyPressed || bKeyHeld) {
		return;
	}

	if (s_popup != POPUP_NONE) {
		// MENU switches to the picker's drill and EXIT drops it; either only closes it
		if (Key == KEY_MENU || Key == KEY_EXIT) {
			ClosePopup(Key == KEY_MENU);
			return;
		}
		if (Key == KEY_F && s_popup == POPUP_DRILL) {
			StepDrill(1);
			return;
		}
		// Any other key closes it, dropping a pending drill, and then does its usual job
		ClosePopup(false);
	}

	// Any key closes the controls card, EXIT too, and does only that
	if (s_card) {
		s_card = false;
		gUpdateDisplay = true;
		return;
	}

	if (Key == KEY_EXIT) {
		CPO_Exit();
		return;
	}

	switch (Key) {

	case KEY_MENU:
		s_card = true;
		gUpdateDisplay = true;
		break;

	case KEY_STAR:
		gCW_CpoBacklightOn = !gCW_CpoBacklightOn;
		if (gCW_CpoBacklightOn) {
			BACKLIGHT_TurnOn();
			gBacklightCountdown_500ms = 2;
		} else {
			BACKLIGHT_TurnOff();
		}
		ShowPopup(POPUP_LIGHT);
		break;

	case KEY_4:
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
		if (QsoDrill()) {
			CPO_Qso_NextDifficulty();
			ShowPopup(POPUP_DIFFICULTY);
			break;
		}
#endif
#ifdef ENABLE_FLASHLIGHT
		gCW_FlashlightSending = !gCW_FlashlightSending;
        s_flashlight_sending = gCW_FlashlightSending;
		ShowPopup(POPUP_FLASH);
#endif
		break;
	case KEY_1:
	case KEY_7:
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
		if (QsoDrill()) {
			CPO_Qso_StepPile(Key == KEY_1);
			ShowPopup(POPUP_PILE);
			break;
		}
#endif
		// The callsign drills' longest call, which the QSO drill's callers keep to too
		if (gCW_CpoCallMode != CPO_CALL_MODE_OFF) {
			CPO_Call_StepCallLen(Key == KEY_1);
			ShowPopup(POPUP_CALL_LEN);
		}
		break;

	case KEY_5:
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
		if (QsoDrill()) {
			CPO_Qso_Restart();
			break;
		}
#endif
		CW_ClearTxDisplay();
		CPO_Call_Restart();
		gUpdateDisplay = true;
		break;

	case KEY_F:
		// The picker opens on the drill in use
		s_drill = gCW_CpoCallMode;
		ShowPopup(POPUP_DRILL);
		break;

	case KEY_2:
	case KEY_8:
		if (CPO_Call_IsCopyMode()) {
			CPO_Call_StepSpacing(Key == KEY_2);
			ShowPopup(POPUP_SPACING);
		}
		break;

	case KEY_0:
		if (CPO_Call_IsCopyMode()) {
			CPO_Call_ToggleAutoSpacing();
			ShowPopup(POPUP_AUTO);
		}
		break;

#ifdef ENABLE_FEAT_NR7Y_CPO_SYNTH
	case KEY_9:
		gEeprom.CW_PRACTICE_NOISE = !gEeprom.CW_PRACTICE_NOISE;
		if (gEeprom.CW_PRACTICE_NOISE) {
			NoiseStart();
		} else {
			NoiseStop();
		}
		ShowPopup(POPUP_NOISE);
		break;

	case KEY_3:
	case KEY_6:
		StepNoiseLevel(Key == KEY_3);
		ShowPopup(POPUP_NOISE);
		break;
#endif

	default:
		break;
	}
}

#endif
