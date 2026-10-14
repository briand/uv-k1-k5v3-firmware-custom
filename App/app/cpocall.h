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

// Callsign drill for code practice. F picks off, send or copy:
//   send: a random callsign is shown and the operator keys it
//   copy: the callsign is played on the sidetone and the operator keys back what
//         they heard; a hit is answered with "dit dit", a miss replays the call,
//         and the third miss on one call reveals it and moves on. Sending part
//         of the call then ? (W1?) asks for a repeat and notes the part on screen.
// Every character is scored as it is decoded.
//
// Copy mode can play with Farnsworth spacing: characters at the keyer speed,
// with the gaps between them stretched to a slower effective speed (8 widens, 2
// narrows). Auto spacing (0) narrows the gaps a step after a run of calls copied on
// the first listen and widens them a step when a call has to be revealed.

#ifndef APP_CPOCALL_H
#define APP_CPOCALL_H

#include <stdbool.h>
#include <stdint.h>

#define CPO_CALL_MAX_LEN 7   // CW_CALL_LEN_MAX: a portable call such as K7ABC/P
#define CPO_CALL_LINE_SIZE (CPO_CALL_MAX_LEN + 2)   // notes plus '?' plus NUL
#define CPO_CALL_NOTICE_SIZE 15                     // "Spacing 12 WPM" plus NUL

typedef enum {
	CPO_CALL_MODE_OFF = 0,
	CPO_CALL_MODE_SEND,
	CPO_CALL_MODE_COPY,
#ifdef ENABLE_FEAT_NR7Y_CPO_QSO
	CPO_CALL_MODE_QSO,      // the QSO drill (cpoqso.c), after copy in F's list
#endif
	CPO_CALL_MODE_COUNT,
} CPO_CallMode_t;

typedef enum {
	CPO_CALL_RESULT_NONE = 0,
	CPO_CALL_RESULT_HIT,    // last callsign keyed correctly
	CPO_CALL_RESULT_MISS,   // last character keyed was wrong
} CPO_CallResult_t;

// Switch mode; resets the score and starts on a new callsign
void CPO_Call_SetMode(CPO_CallMode_t mode);

// A random callsign like the drill's: US, or DX from a list of common prefixes, no
// longer than CW_CALL_LEN. At CW_CALL_LEN_MAX some are portable: a DX call /P, a US
// call / another call area. Returns the DX prefix's index in that list, or -1 for a
// US call, whose call area is its last digit.
int8_t CPO_Call_Generate(char out[CPO_CALL_MAX_LEN + 1]);

// The drill's random numbers (xorshift32), reseeded with each new callsign
uint32_t CPO_Call_Rand(void);

// 5 key: clear the line and restart the current callsign unscored (replayed in copy)
void CPO_Call_Restart(void);

// True in the modes that play the callsign for the operator to copy
bool CPO_Call_IsCopyMode(void);

// Effective speed copy playback runs at (Farnsworth), or 0 for the keyer's own
// spacing. A stored speed at or above the keyer speed is off until the keyer
// speed passes it again.
uint8_t CPO_Call_EffectiveWPM(void);

// 2 and 8 keys, copy mode only: effective speed one step up (narrower gaps) or
// down (wider). Stepping up to the keyer speed turns the spacing off.
void CPO_Call_StepSpacing(bool narrower);

// 1 and 7 keys, the callsign drills: the longest call, from the next one on, up or
// down; an error dit past CW_CALL_LEN_MIN or CW_CALL_LEN_MAX
void CPO_Call_StepCallLen(bool longer);

// 0 key, copy mode only: auto spacing on/off
void CPO_Call_ToggleAutoSpacing(void);

// The line shown for a moment after auto spacing takes a step, e.g. "Spacing 12 WPM";
// false while there is none
bool CPO_Call_GetNotice(char out[CPO_CALL_NOTICE_SIZE]);

// Every character the CW encoder decodes, or CW_CHAR_UNKNOWN for keying that
// decodes to nothing (always a miss); a no-op while the drill is off
void CPO_Call_OnChar(char ch);

// 10ms tick from CPO_Tick
void CPO_Call_Tick10ms(void);

// Text for the callsign line: the call in send mode or once copied/revealed,
// otherwise the notes so far, e.g. "W1A?" (just "?" with none)
void CPO_Call_GetCallLine(char out[CPO_CALL_LINE_SIZE]);

extern CPO_CallMode_t gCW_CpoCallMode;
extern char gCW_CpoCall[CPO_CALL_MAX_LEN + 1];
extern uint16_t gCW_CpoCallHits;
extern uint16_t gCW_CpoCallMisses;
extern CPO_CallResult_t gCW_CpoCallResult;

#endif
