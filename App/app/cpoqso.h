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

// QSO drill for code practice. Call CQ with your callsign after DE, and a station
// answers with its call on the synthesizer's station voice. Come back to it with its
// call and your report; it sends its report, QTH and name; confirm them and sign
// with 73, and it signs too and sends dit dit for yours. Along the way it:
//   - corrects its call when you send it one or two characters off
//   - repeats its call for a bare ?, QRZ? or AGN while you answer it
//   - repeats a field for NAME?, QTH? or RST?, and the lot for AGN or ? after
//   - calls again when you don't answer, and leaves after three tries
// It learns your call from the DE in each CQ, so a CQ with another call hands the
// radio to the next operator. Whatever you send while it is sending, it never hears.
// The score shows when the QSO ends.

#ifndef APP_CPOQSO_H
#define APP_CPOQSO_H

#include <stdbool.h>
#include <stdint.h>

#define CPO_QSO_LINE_SIZE 19   // a full small-font line plus NUL

// The drill selected and left; entry waits for a CQ
void CPO_Qso_Enter(void);
void CPO_Qso_Exit(void);

// 5 key: abandon the QSO and wait for a CQ
void CPO_Qso_Restart(void);

// Every character the CW encoder decodes, and whether a word gap came before it
void CPO_Qso_OnChar(char ch, bool word_gap);

// 10ms tick from CPO_Tick
void CPO_Qso_Tick10ms(void);

// Lines 1 and 2 of the screen during a QSO: what to do now, and who you're working
void CPO_Qso_GetStatus(char hint[CPO_QSO_LINE_SIZE], char info[CPO_QSO_LINE_SIZE]);

// Lines 1-5 once a QSO has ended, until the next one starts; false while there is
// no score to show
bool CPO_Qso_GetScore(char lines[5][CPO_QSO_LINE_SIZE]);

// A line shown for a moment on something worth knowing, e.g. "Doubled"; false while
// there is none
bool CPO_Qso_GetNotice(char out[CPO_QSO_LINE_SIZE]);

#endif
