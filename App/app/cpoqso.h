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

// QSO drill for code practice. Call CQ with your callsign after DE, and stations
// answer with their calls, each on its own synthesizer voice. A first CQ brings one,
// sometimes two; working them at a good rate brings more, up to the pile-up size
// (practice's Pileup), while slow QSOs and silence let the pile shrink. Whether anyone answers
// at all is down to the band's activity: CQs and QSOs build it, silence thins it,
// and a CQ may go unanswered. Come back to one
// with its call and your report; it sends its report, QTH and name; confirm them and
// sign with 73, and it signs too and sends dit dit for yours. Along the way:
//   - part of a call (K7?, XYZ?, 7XY), or one a character or two off, brings the
//     callers it could be back with theirs, often a touch slower or spaced wider
//     from then on, while the rest stand by
//   - a bare ?, QRZ? or AGN brings them all back
//   - the station repeats a field for NAME?, QTH? or RST?, and the lot for AGN or ?
//   - unanswered, the pile calls again until each runs out of patience and leaves:
//     unanswered calls use it up, as does a long wait through your QSO with another,
//     while being asked for its call or standing by doesn't; the rest call once your
//     QSO is over
//   - a lid calls over whoever you come back to, and a caller sending as you key
//     misses your over
// Answer it without your name or QTH and it keeps to the exchange, contest and POTA
// style: RST and its state, twice each for bad conditions, your 5NN and BK back, and
// a short TU or 73 to sign. CQ POTA, SOTA, WWFF, TEST or FD gets a quick single call,
// and QRZ between QSOs calls the next station like a CQ.
// Each station comes on with a personality the difficulty (Diffic) shapes: its speed and
// character spacing, how strong it is and whether it fades, how far off zero beat,
// its fist (a keyer, plain, weighted heavy or light, or with letters spaced by hand;
// a bug, straight key or cootie) and chirp, whether it's QSK and stops when you break in, and
// whether it ragchews (as it does when you mention RIG, ANT or WX), and in a pile how
// close to zero beat on the others, how patient, and whether it's a lid. QRS and QRQ
// change their speed. After the dit dit an impatient newcomer may join the pile.
// It learns your call from the DE in each CQ, so a CQ with another call hands the
// radio to the next operator. Prosigns count the same keyed run together or as
// letters, words run together without a word gap still count (UR599, 73EE,
// NAMEDREW), and a callsign keyed with a gap in it (K7X YZ) is joined back up. Whatever you send while it is sending, it never hears. The bottom line
// counts QSOs logged (the station signed) and failed (the station you came back to gave
// up waiting for your report, or you left it with 5 or a new CQ), from when the drill
// was entered. Callers you never came back to don't count.

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

const char *CPO_Qso_DifficultyName(void);

// Every character the CW encoder decodes, and whether a word gap came before it
void CPO_Qso_OnChar(char ch, bool word_gap);

// 10ms tick from CPO_Tick
void CPO_Qso_Tick10ms(void);

// How to start, until the drill has learned your call from a CQ; false after that,
// when the QSO goes by ear
bool CPO_Qso_GetHint(char out[CPO_QSO_LINE_SIZE]);


// A line shown for a moment on something worth knowing, e.g. "Doubled"; false while
// there is none
bool CPO_Qso_GetNotice(char out[CPO_QSO_LINE_SIZE]);

extern uint16_t gCW_CpoQsoLogged;
extern uint16_t gCW_CpoQsoFailed;

#endif
