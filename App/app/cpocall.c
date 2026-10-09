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

// Callsign drill for code practice

#include <string.h>

#include "app/cpocall.h"
#include "app/cpoqth.h"
#include "app/cwkeyer.h"
#include "app/cwmacro.h"
#include "driver/bk4819.h"
#include "driver/millis.h"
#include "misc.h"
#ifdef ENABLE_FLASHLIGHT
#include "driver/gpio.h"
#include "py32f071_ll_gpio.h"
#endif

// How long OK/MISS stays up before the next attempt starts
#define CPO_CALL_RESULT_10MS 100
// Copy mode: pause after the operator's last character before answering "dit dit"
#define CPO_CALL_ACK_10MS 30
// Copy mode: how long a callsign revealed after the last allowed miss stays up
#define CPO_CALL_REVEAL_10MS 300
// Copy mode: misses allowed on one callsign before it is revealed
#define CPO_CALL_MAX_TRIES 3
// Keeps "999/999" inside the space the score gets on the WPM line
#define CPO_CALL_SCORE_MAX 999

// What the drill does next, once the operator stops keying and playback ends
typedef enum {
	CPO_STEP_NONE = 0,   // waiting on the operator
	CPO_STEP_RETRY,      // same callsign again (played in copy mode)
	CPO_STEP_NEXT,       // new callsign (played in copy mode)
	CPO_STEP_ACK,        // copy mode: "dit dit", then NEXT
} CPO_CallStep_t;

CPO_CallMode_t gCW_CpoCallMode = CPO_CALL_MODE_OFF;
char gCW_CpoCall[CPO_CALL_MAX_LEN + 1];
uint16_t gCW_CpoCallHits = 0;
uint16_t gCW_CpoCallMisses = 0;
CPO_CallResult_t gCW_CpoCallResult = CPO_CALL_RESULT_NONE;

static uint8_t s_pos = 0;          // index in gCW_CpoCall of the next expected character
static uint8_t s_tries = 0;        // misses on the current callsign
static uint8_t s_known = 0;        // copy mode: leading characters confirmed by a partial + ?
static bool s_revealed = false;    // copy mode: the call may be shown in full
static const char *s_name = NULL;  // copy QTH: what the code stands for
static CPO_CallStep_t s_step = CPO_STEP_NONE;
static uint16_t s_wait_10ms = 0;   // counts down only while the operator is idle
static bool s_paused = false;      // operator has paused since s_step was set
static uint32_t s_rng_state = 0;

// Common DX prefixes, two characters each (space-padded for one-letter prefixes)
static const char DX_PREFIXES[] =
	"VEVAG M DLDKF I EAJAJHVKZLPYLUONPAOHSMLAOKSPHAYOUA9AS5EI";

static uint32_t Rand(void)
{
	// xorshift32: tiny and plenty random for picking callsign characters
	s_rng_state ^= s_rng_state << 13;
	s_rng_state ^= s_rng_state >> 17;
	s_rng_state ^= s_rng_state << 5;
	return s_rng_state;
}

static char RandLetter(uint8_t count)
{
	return 'A' + (char)(Rand() % count);
}

static bool IsCopyMode(void)
{
	return gCW_CpoCallMode == CPO_CALL_MODE_COPY || gCW_CpoCallMode == CPO_CALL_MODE_COPY_QTH;
}

static void NewCallsign(void)
{
	char *p = gCW_CpoCall;
	uint8_t suffix_len;

	if (Rand() % 3 != 0) {
		// US: 1x2, 1x3, 2x1, 2x2 or 2x3
		static const char US_FIRST[] = "KNW";
		const uint8_t format = Rand() % 5;

		if (format < 2) {
			*p++ = US_FIRST[Rand() % 3];
			suffix_len = 2 + format;
		} else {
			if (Rand() % 4 == 0) {
				*p++ = 'A';
				*p++ = RandLetter(12);   // AA-AL
			} else {
				*p++ = US_FIRST[Rand() % 3];
				*p++ = RandLetter(26);
			}
			suffix_len = format - 1;
		}
	} else {
		const uint8_t i = (Rand() % ((sizeof(DX_PREFIXES) - 1) / 2)) * 2;

		*p++ = DX_PREFIXES[i];
		if (DX_PREFIXES[i + 1] != ' ') {
			*p++ = DX_PREFIXES[i + 1];
		}
		suffix_len = 2 + Rand() % 2;
	}

	*p++ = '0' + (char)(Rand() % 10);
	while (suffix_len--) {
		*p++ = RandLetter(26);
	}
	*p = '\0';
}

static void NewCall(void)
{
	// Fold in the time the operator finished the last call so the sequence
	// doesn't repeat from session to session. xorshift must never sit at zero.
	s_rng_state ^= millis();
	if (s_rng_state == 0) {
		s_rng_state = 0x2545F491;
	}

	if (gCW_CpoCallMode == CPO_CALL_MODE_COPY_QTH) {
		s_name = CPO_Qth_Pick(Rand(), gCW_CpoCall);
	} else {
		s_name = NULL;
		NewCallsign();
	}
}

static void StopPlayback(void)
{
	if (!gCW_PlaybackActive) {
		return;
	}
	// CW_StopPlayback leaves the sidetone (and flashlight) as they are, which is
	// on when it lands mid-element
	CW_StopPlayback();
	BK4819_SetScrambleFrequencyControlWord(0);
#ifdef ENABLE_FLASHLIGHT
	GPIO_ResetOutputPin(GPIO_PIN_FLASHLIGHT);
#endif
}

static void Schedule(CPO_CallStep_t step, uint16_t wait_10ms)
{
	s_step = step;
	s_wait_10ms = wait_10ms;
	s_paused = false;
}

// Start an attempt on a new callsign (NEXT) or the same one (RETRY). play is false
// when the operator is already keying, so playback doesn't talk over them.
static void Begin(CPO_CallStep_t step, bool play)
{
	s_step = CPO_STEP_NONE;
	if (step == CPO_STEP_NEXT) {
		NewCall();
		s_tries = 0;
		s_known = 0;
		s_revealed = false;
	}
	gCW_CpoCallResult = CPO_CALL_RESULT_NONE;
	s_pos = 0;
	CW_ClearTxDisplay();
	if (play && IsCopyMode()) {
		CW_StartTextPlayback(gCW_CpoCall, false);
	}
	gUpdateDisplay = true;
}

void CPO_Call_SetMode(CPO_CallMode_t mode)
{
	StopPlayback();
	gCW_CpoCallMode = mode;
	gCW_CpoCallHits = 0;
	gCW_CpoCallMisses = 0;
	Begin(CPO_STEP_NEXT, false);
	if (IsCopyMode()) {
		Schedule(CPO_STEP_RETRY, 0);   // first play, held until the keyer is idle
	}
}

void CPO_Call_NextMode(void)
{
	CPO_Call_SetMode(gCW_CpoCallMode == CPO_CALL_MODE_COPY_QTH
		? CPO_CALL_MODE_OFF
		: (CPO_CallMode_t)(gCW_CpoCallMode + 1));
}

void CPO_Call_Restart(void)
{
	// After a hit or a reveal the next callsign is already on its way
	if (gCW_CpoCallMode == CPO_CALL_MODE_OFF || s_step == CPO_STEP_NEXT || s_step == CPO_STEP_ACK) {
		return;
	}
	StopPlayback();
	Schedule(CPO_STEP_RETRY, 0);
}

void CPO_Call_OnChar(char ch)
{
	if (gCW_CpoCallMode == CPO_CALL_MODE_OFF) {
		return;
	}

	if (s_step != CPO_STEP_NONE) {
		// Only a retry can be cut short, and only by a fresh attempt after a pause.
		// Characters that finish off the word that missed are ignored.
		if (s_step != CPO_STEP_RETRY || !s_paused) {
			return;
		}
		Begin(CPO_STEP_RETRY, false);
		CW_AddToTxDisplay(ch, false);   // the encoder put it on the old line
	}

	if (ch == '?' && IsCopyMode()) {
		// Asking for a repeat, as on the air: no penalty. Everything keyed before
		// the ? has already matched, so it goes on the notes (which only grow).
		if (s_pos > s_known) {
			s_known = s_pos;
		}
		Schedule(CPO_STEP_RETRY, 0);
		gUpdateDisplay = true;
		return;
	}

	if (ch != gCW_CpoCall[s_pos]) {
		if (gCW_CpoCallMisses < CPO_CALL_SCORE_MAX) {
			gCW_CpoCallMisses++;
		}
		gCW_CpoCallResult = CPO_CALL_RESULT_MISS;
		if (IsCopyMode() && ++s_tries >= CPO_CALL_MAX_TRIES) {
			s_revealed = true;
			Schedule(CPO_STEP_NEXT, CPO_CALL_REVEAL_10MS);
		} else {
			Schedule(CPO_STEP_RETRY, CPO_CALL_RESULT_10MS);
		}
	} else if (gCW_CpoCall[++s_pos] == '\0') {
		if (gCW_CpoCallHits < CPO_CALL_SCORE_MAX) {
			gCW_CpoCallHits++;
		}
		gCW_CpoCallResult = CPO_CALL_RESULT_HIT;
		if (IsCopyMode()) {
			s_revealed = true;
			Schedule(CPO_STEP_ACK, CPO_CALL_ACK_10MS);
		} else {
			Schedule(CPO_STEP_NEXT, CPO_CALL_RESULT_10MS);
		}
	}
	gUpdateDisplay = true;
}

void CPO_Call_Tick10ms(void)
{
	// Steps wait for the operator to stop keying and for playback to finish, so
	// the drill never plays over them or scores the tail of a missed word
	if (s_step == CPO_STEP_NONE || gCW_PlaybackActive || !CW_KeyerIsIdle()) {
		return;
	}
	s_paused = true;
	if (s_wait_10ms > 0) {
		s_wait_10ms--;
		return;
	}
	if (s_step == CPO_STEP_ACK) {
		CW_StartTextPlayback("EE", false);
		Schedule(CPO_STEP_NEXT, CPO_CALL_RESULT_10MS);
		return;
	}
	Begin(s_step, true);
}

void CPO_Call_GetCallLine(char out[CPO_CALL_LINE_SIZE])
{
	if (gCW_CpoCallMode == CPO_CALL_MODE_SEND || s_revealed) {
		strcpy(out, gCW_CpoCall);
		return;
	}
	// s_known stops short of the full call: keying the last character is a hit
	memcpy(out, gCW_CpoCall, s_known);
	out[s_known] = '?';
	out[s_known + 1] = '\0';
}

const char *CPO_Call_GetName(void)
{
	return s_revealed ? s_name : NULL;
}
