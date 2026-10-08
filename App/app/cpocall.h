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

// Callsign drill for code practice. F cycles off -> send -> copy:
//   send: a random callsign is shown and the operator keys it
//   copy: the callsign is played on the sidetone and the operator keys back what
//         they heard; a hit is answered with "dit dit", a miss replays the call,
//         and the third miss on one call reveals it and moves on. Sending part
//         of the call then ? (W1?) asks for a repeat and notes the part on screen.
// Every character is scored as it is decoded.

#ifndef APP_CPOCALL_H
#define APP_CPOCALL_H

#include <stdbool.h>
#include <stdint.h>

#define CPO_CALL_MAX_LEN 6
#define CPO_CALL_LINE_SIZE (CPO_CALL_MAX_LEN + 2)   // notes plus '?' plus NUL

typedef enum {
	CPO_CALL_MODE_OFF = 0,
	CPO_CALL_MODE_SEND,
	CPO_CALL_MODE_COPY,
} CPO_CallMode_t;

typedef enum {
	CPO_CALL_RESULT_NONE = 0,
	CPO_CALL_RESULT_HIT,    // last callsign keyed correctly
	CPO_CALL_RESULT_MISS,   // last character keyed was wrong
} CPO_CallResult_t;

// Switch mode; resets the score and starts on a new callsign
void CPO_Call_SetMode(CPO_CallMode_t mode);

// F key: off -> send -> copy -> off
void CPO_Call_NextMode(void);

// 5 key: clear the line and restart the current callsign unscored (replayed in copy)
void CPO_Call_Restart(void);

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
