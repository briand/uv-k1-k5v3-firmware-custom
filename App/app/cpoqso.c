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

// QSO drill for code practice

#include <string.h>

#include "app/cpo.h"
#include "app/cpocall.h"
#include "app/cpoqso.h"
#include "app/cwkeyer.h"
#include "app/cwmacro.h"
#include "app/cwsynth.h"
#include "driver/millis.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"

#define OVER_SIZE   96   // your over as decoded, its words split by single spaces
#define SAY_SIZE    200  // what the station sends next
#define TOKENS_MAX  24
#define CALL_SIZE   11   // room for a portable call such as VE3/W1ABC
#define NAME_SIZE   9
#define QTH_SIZE    16

// In 10 ms ticks
#define REPLY_MIN_10MS    20     // the station's reaction time, from 0.2 s...
#define REPLY_SPAN_10MS   40     // ...to 0.6 s, half that on a POTA or contest frequency
#define ANSWER_WAIT_10MS  400    // silence after its call before it calls again
#define CONFIRM_WAIT_10MS 700    // silence after its report before it asks
#define DITDIT_WAIT_10MS  300    // how long it listens for your dit dit
#define NOTICE_10MS       150
#define CALLS_MAX         3      // calls with no answer before it leaves
#define TAIL_MIN_10MS     40     // a tail-ender calls 0.4 s after the dit dit...
#define TAIL_SPAN_10MS    80     // ...to 1.2 s, half that on a POTA or contest frequency

typedef enum {
	QSO_IDLE = 0,   // waiting for a CQ
	QSO_ANSWER,     // it has called you, and waits for you to come back to it
	QSO_CONFIRM,    // it has sent its report, and waits for yours back and 73
	QSO_DITDIT,     // it has signed, and listens for your dit dit
	QSO_DONE,       // over, logged or not, and waiting for a CQ
} CPO_QsoState_t;

typedef enum {
	NOTICE_DOUBLED = 0,
	NOTICE_OP,
	NOTICE_NO_CALL,
	NOTICE_LEFT,
	NOTICE_DIFFICULTY,
	NOTICE_LOGGED,
	NOTICE_NOT_LOGGED,
} CPO_QsoNotice_t;

// What an over said, as far as the station cares
typedef struct {
	const char *de;          // the callsign after DE
	const char *other;       // the first callsign that isn't yours
	const char *op;          // your name, after NAME or OP
	const char *greet;       // GM, GA, GE or GD
	const char *end;         // K, KN or BK at the end, to answer in kind
	uint8_t best;            // edit distance from its call to your nearest callsign, 0xFF for none
	bool cq;                 // CQ, or QRZ between QSOs
	bool special;            // a CQ for POTA, SOTA, WWFF, a contest or field day
	bool ask;                // a request to repeat: ?, AGN or PSE RPT
	bool ask_name, ask_qth, ask_rst;
	bool rst;                // you sent a report
	bool rst_599;            // ...a contest one, 599 or 5NN
	bool cut;                // ...in cut numbers
	bool qth;                // you sent QTH
	bool sign;               // 73 or SK
	bool qrs, qrq;           // slower, faster
	bool chat;               // RIG, ANT, WX or PWR: you're ragchewing
	bool ditdit;             // it ends in EE or E E
} CPO_QsoOver_t;

// Names and QTHs it picks from. QTHs go by call area for US calls, and by prefix,
// in the order of cpocall.c's DX_PREFIXES, for DX.
static const char NAMES[] =
	"BOB JIM TOM BILL JOHN MIKE DAVE ED AL JOE DON KEN RON RICK STEVE GARY PAUL MARK "
	"DAN PETE SAM ANN SUE LIZ KAY DEB JAN PAT HANK BUD WALT FRED HAL GUS LOU ART RAY "
	"NED TED CHUCK";
static const char *const US_QTH[10] = {
	"DENVER CO|ST PAUL MN|OMAHA NE|WICHITA KS",
	"BOSTON MA|PORTLAND ME|HARTFORD CT|BURLINGTON VT",
	"ALBANY NY|BUFFALO NY|TRENTON NJ",
	"PITTSBURGH PA|ERIE PA|BALTIMORE MD|DOVER DE",
	"ATLANTA GA|RALEIGH NC|NASHVILLE TN|TAMPA FL|RICHMOND VA",
	"AUSTIN TX|TULSA OK|LITTLE ROCK AR|SANTA FE NM",
	"FRESNO CA|SAN DIEGO CA|SACRAMENTO CA",
	"BOISE ID|SEATTLE WA|TUCSON AZ|RENO NV|BOZEMAN MT",
	"DAYTON OH|DETROIT MI|CHARLESTON WV",
	"MADISON WI|PEORIA IL|GARY IN",
};
static const char DX_QTH[] =
	"TORONTO ON|OTTAWA ON|LONDON|LEEDS|MUNICH|BERLIN|LYON|ROME|MADRID|TOKYO|OSAKA|"
	"SYDNEY|AUCKLAND|SAO PAULO|CORDOBA|BRUSSELS|UTRECHT|HELSINKI|STOCKHOLM|OSLO|"
	"PRAGUE|WARSAW|BUDAPEST|BUCHAREST|MOSCOW|ZAGREB|LJUBLJANA|DUBLIN";
static const char RSTS[] = "599 589 579 579 569 559 559 549 449 459";
static const char RIGS[] = "IC7300|K3|FT991|KX2|TS590|FTDX10|IC705|HOMEBREW";
static const char ANTS[] = "DIPOLE|VERTICAL|YAGI|EFHW|LOOP|G5RV|HEXBEAM";
static const char WX[] = "SUNNY|CLOUDY|RAIN|SNOW|WINDY|FOGGY";
static const char *const DIFFICULTY_NAMES[CW_QSO_DIFFICULTIES] = {"Easy", "Medium", "Hard"};

// What each difficulty puts on the air. Each station rolls its own within these.
typedef struct {
	uint8_t wpm_spread;      // WPM either side of yours
	uint8_t level_db;        // dB under full level at the strongest...
	uint8_t level_span_db;   // ...and how much weaker it can be
	uint8_t qsb_pct;         // chance it fades
	uint8_t qsb_db;          // as deep as this at most
	uint8_t pitch_spread;    // 10s of Hz either side of zero beat
	uint8_t hand_pct;        // chance of a bug, straight key or cootie over a paddle
	uint8_t chirp_pct;
	uint8_t tail_pct;        // chance another calls straight after the dit dit
} CPO_QsoDifficulty_t;

static const CPO_QsoDifficulty_t DIFFICULTY[CW_QSO_DIFFICULTIES] = {
	{ 0,  0,  0,  0,  0,  0,  0,  0, 20 },   // easy: strong, your speed, a paddle
	{ 3,  0, 15, 40, 10,  6, 40, 10, 35 },   // medium
	{ 8, 10, 14, 70, 20, 15, 70, 20, 50 },   // hard: down near the noise
};

static CPO_QsoState_t s_state = QSO_IDLE;
static CPO_QsoState_t s_after;     // the state once the station has done sending
static bool s_reply_due = false;   // what it says next is ready, and waits out its reaction time
static bool s_speaking = false;
static uint16_t s_reply_10ms;
static uint16_t s_wait_10ms;       // silence in a waiting state
static uint8_t s_calls;            // calls it has made since you last answered
static bool s_prompted;            // it has already asked once while waiting on your report
static bool s_quick;               // a POTA, SOTA, contest or field day CQ: it answers fast
static bool s_brief;               // you answered without name or QTH: it keeps to RST and state
static bool s_cut;                 // it sends its report in cut numbers
static char s_end[3];              // how you ended your over: K, KN, BK or nothing

// The station's personality, rolled when it comes on
static uint8_t s_wpm;
static int16_t s_pitch;            // Hz off your sidetone
static uint8_t s_level_db;         // under full level
static uint8_t s_qsb_db;           // how deep it fades, 0 for not at all
static uint16_t s_qsb_10ms;        // and how slowly
static uint16_t s_qsb_at;
static bool s_qsk;                 // it stops when you break in, and hears you
static bool s_talker;              // it ragchews whatever you send
static uint16_t s_tail_10ms;       // until a tail-ender calls, 0 for none coming

static char s_over[OVER_SIZE];
static uint8_t s_over_len = 0;
static bool s_over_heard = false;  // the over has been acted on; more keying adds to it
static bool s_word_done = false;   // a prosign ended the word: the next character starts one
static uint32_t s_last_char_ms;
static const char *s_tok[TOKENS_MAX];
static uint8_t s_tok_count;

static char s_say[SAY_SIZE];
static uint8_t s_say_len;

static char s_my[CALL_SIZE];       // yours, from your last CQ
static char s_call[CALL_SIZE];     // the station's
static char s_name[NAME_SIZE];
static char s_qth[QTH_SIZE];
static char s_rst[4];              // its report to you
static char s_op[NAME_SIZE];       // your name, if you sent it
static char s_greet[3];

static CPO_QsoNotice_t s_notice;
static char s_logged[CALL_SIZE];   // the call a "Logged" notice names, kept: a tail-ender
                                   // coming on changes s_call while the notice is still up
static uint8_t s_notice_10ms = 0;


// QSOs logged (the station signed) and failed (it left, gave up on you, or you left it)
uint16_t gCW_CpoQsoLogged = 0;
uint16_t gCW_CpoQsoFailed = 0;

static uint32_t Rnd(uint32_t n)
{
	return CPO_Call_Rand() % n;
}

static bool Is(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

static uint8_t CountItems(const char *list, char sep)
{
	uint8_t n = 1;
	for (; *list; list++) {
		n += (*list == sep);
	}
	return n;
}

// Item n of a list split by sep, into out
static void PickItem(const char *list, char sep, uint8_t n, char *out, uint8_t size)
{
	uint8_t i = 0;

	while (n--) {
		list = strchr(list, sep) + 1;
	}
	while (list[i] && list[i] != sep && i + 1 < size) {
		out[i] = list[i];
		i++;
	}
	out[i] = '\0';
}

static void PickRandom(const char *list, char sep, char *out, uint8_t size)
{
	PickItem(list, sep, Rnd(CountItems(list, sep)), out, size);
}

static void Notice(CPO_QsoNotice_t notice)
{
	s_notice = notice;
	s_notice_10ms = NOTICE_10MS;
	gUpdateDisplay = true;
}

// ---- what the station sends ----

static void SayStart(void)
{
	s_say_len = 0;
	s_say[0] = '\0';
}

static void Say(const char *words)
{
	const uint8_t n = strlen(words);

	if (n == 0 || s_say_len + n + 2 > SAY_SIZE) {
		return;
	}
	if (s_say_len) {
		s_say[s_say_len++] = ' ';
	}
	memcpy(s_say + s_say_len, words, n + 1);
	s_say_len += n;
}

static void SayTwice(const char *words)
{
	Say(words);
	Say(words);
}

// One of a list split by |; an empty item says nothing
static void SayOneOf(const char *list)
{
	char words[20];
	PickRandom(list, '|', words, sizeof(words));
	Say(words);
}

static void SayCall(uint8_t times)
{
	while (times--) {
		Say(s_call);
	}
}

static void SayRst(void)
{
	char rst[4];

	strcpy(rst, s_rst);
	if (s_cut) {
		// cut numbers: 9 as N
		for (char *c = rst; *c; c++) {
			if (*c == '9') {
				*c = 'N';
			}
		}
	}
	if (!s_brief) {
		SayOneOf("UR RST|UR|RST");
	}
	SayTwice(rst);
}

// The two-letter state or province ending its QTH, if it has one
static bool State(char out[3])
{
	const char *gap = strrchr(s_qth, ' ');

	if (!gap || strlen(gap + 1) != 2) {
		return false;
	}
	strcpy(out, gap + 1);
	return true;
}

// BK and KN go out as prosigns, run together, as most send them
static void SayEnd(const char *end)
{
	Say(Is(end, "BK") ? "\x02" : Is(end, "KN") ? "(" : end);
}

static void SayName(void)
{
	SayOneOf("NAME|OP|NAME HR");
	SayTwice(s_name);
}

static void SayQth(bool twice)
{
	Say("QTH");
	Say(s_qth);
	if (twice) {
		Say(s_qth);
	}
}

static void SayChat(void)
{
	char temp[5];

	Say("= RIG");
	SayOneOf(RIGS);
	Say("ES ANT");
	SayOneOf(ANTS);
	Say("= WX");
	SayOneOf(WX);
	sprintf_(temp, "%uF", 20 + (unsigned)Rnd(76));
	Say(temp);
}

static void SayOver(void)
{
	Say(s_my);
	Say("DE");
	Say(s_call);
	if (s_end[0]) {
		SayEnd(s_end);
	} else {
		SayOneOf("(|(|K|\x02");
	}
}

static void Reply(CPO_QsoState_t after)
{
	s_after = after;
	s_reply_due = true;
	// quicker on a POTA, SOTA or contest frequency
	s_reply_10ms = s_quick ? REPLY_MIN_10MS / 2 + Rnd(REPLY_SPAN_10MS / 2)
		: REPLY_MIN_10MS + Rnd(REPLY_SPAN_10MS);
}

// ---- your over ----

static void ClearOver(void)
{
	s_over_len = 0;
	s_over[0] = '\0';
	s_over_heard = false;
	s_word_done = false;
}

static void AddToOver(char ch)
{
	if (s_over_len + 2 > OVER_SIZE) {
		// full: drop the first word
		char *gap = strchr(s_over, ' ');
		const uint8_t drop = gap ? (uint8_t)(gap - s_over + 1) : s_over_len;
		memmove(s_over, s_over + drop, s_over_len - drop + 1);
		s_over_len -= drop;
	}
	s_over[s_over_len++] = ch;
	s_over[s_over_len] = '\0';
}

static bool IsRst(const char *t)
{
	return strlen(t) == 3 && t[0] >= '1' && t[0] <= '5'
		&& ((t[1] >= '1' && t[1] <= '9') || t[1] == 'N')
		&& ((t[2] >= '1' && t[2] <= '9') || t[2] == 'N');
}

// A callsign: letters and digits, its longest part between any /s ending in a letter
// with a digit inside that has a letter before it, as K7XYZ, 2E0ABC, 9A1AB, W1ABC/P or
// VE3/W1ABC have, and 5NN, UR599, 73 and 73EE don't
static bool IsCall(const char *t)
{
	const char *home = t, *part = t;
	size_t home_len = 0;

	if (strlen(t) < 3 || strlen(t) >= CALL_SIZE) {
		return false;
	}
	for (const char *c = t; ; c++) {
		if (*c == '/' || *c == '\0') {
			if ((size_t)(c - part) > home_len) {
				home = part;
				home_len = c - part;
			}
			if (*c == '\0') {
				break;
			}
			part = c + 1;
		} else if ((*c < 'A' || *c > 'Z') && (*c < '0' || *c > '9')) {
			return false;
		}
	}
	if (home_len < 3 || home[home_len - 1] < 'A') {
		return false;
	}
	bool letter = home[0] >= 'A';
	for (size_t i = 1; i + 1 < home_len; i++) {
		if (home[i] >= 'A') {
			letter = true;
		} else if (letter) {
			return true;
		}
	}
	return false;
}

static bool EndsWith(const char *t, const char *word)
{
	const size_t n = strlen(t), w = strlen(word);
	return n >= w && strcmp(t + n - w, word) == 0;
}

// A report alone, or run on to the word before it as many key it: 599, UR599,
// TU5NN, RST579, 5NN5NN
static bool HasRst(const char *t)
{
	const size_t n = strlen(t);
	char head[4];

	if (n < 3 || n > 6 || !IsRst(t + n - 3)) {
		return false;
	}
	memcpy(head, t, n - 3);
	head[n - 3] = '\0';
	return n == 3 || IsRst(head) || Is(head, "UR") || Is(head, "RST") || Is(head, "TU")
		|| Is(head, "R") || Is(head, "RR") || Is(head, "GM") || Is(head, "GA")
		|| Is(head, "GE") || Is(head, "BK") || Is(head, "FB");
}

// K, KN or BK ending an over, alone or run on to the word before it (BOSTONKN), but
// not a callsign that happens to end that way
static const char *EndMark(const char *last)
{
	if (Is(last, "K")) {
		return "K";
	}
	if (IsCall(last)) {
		return NULL;
	}
	return EndsWith(last, "KN") ? "KN" : EndsWith(last, "BK") ? "BK" : NULL;
}

// Words i to i + count - 1 run together into out, if that makes a callsign: a call
// keyed with a gap in it, as K7X YZ or W1 ABC
static const char *Join(uint8_t i, uint8_t count, char out[CALL_SIZE])
{
	size_t len = 0;

	if (i + count > s_tok_count) {
		return NULL;
	}
	while (count--) {
		const size_t n = strlen(s_tok[i]);
		if (len + n >= CALL_SIZE) {
			return NULL;
		}
		memcpy(out + len, s_tok[i++], n);
		len += n;
	}
	out[len] = '\0';
	return (len >= 4 && IsCall(out)) ? out : NULL;
}

static bool IsName(const char *t)
{
	const size_t n = strlen(t);

	if (n < 2 || n >= NAME_SIZE || Is(t, "IS") || Is(t, "HR")) {
		return false;
	}
	for (; *t; t++) {
		if (*t < 'A' || *t > 'Z') {
			return false;
		}
	}
	return true;
}

// Edit distance, for a callsign sent a character or two off
static uint8_t Distance(const char *a, const char *b)
{
	uint8_t row[CALL_SIZE + 1];
	const uint8_t nb = strlen(b);

	for (uint8_t j = 0; j <= nb; j++) {
		row[j] = j;
	}
	for (uint8_t i = 1; *a; a++, i++) {
		uint8_t diag = row[0];
		row[0] = i;
		for (uint8_t j = 1; j <= nb; j++) {
			const uint8_t up = row[j];
			uint8_t d = diag + (*a != b[j - 1]);
			if (up + 1 < d) {
				d = up + 1;
			}
			if (row[j - 1] + 1 < d) {
				d = row[j - 1] + 1;
			}
			row[j] = d;
			diag = up;
		}
	}
	return row[nb];
}

static bool StartsWith(const char *t, const char *word)
{
	return strncmp(t, word, strlen(word)) == 0;
}

// Splits the over in place; Unsplit puts its spaces back, so more keying can add to it
static void Split(void)
{
	char *p = s_over;

	s_tok_count = 0;
	while (*p && s_tok_count < TOKENS_MAX) {
		s_tok[s_tok_count++] = p;
		p = strchr(p, ' ');
		if (!p) {
			break;
		}
		*p++ = '\0';
	}
}

static void Unsplit(void)
{
	for (uint8_t i = 1; i < s_tok_count; i++) {
		((char *)s_tok[i])[-1] = ' ';
	}
}

static void Parse(CPO_QsoOver_t *o)
{
	static char de_call[CALL_SIZE];   // your call after DE, joined back if keyed with a gap

	memset(o, 0, sizeof(*o));
	o->best = 0xFF;

	for (uint8_t i = 0; i < s_tok_count; i++) {
		const char *t = s_tok[i];
		const char *prev = i ? s_tok[i - 1] : "";
		const char *next = (i + 1 < s_tok_count) ? s_tok[i + 1] : "";
		const size_t n = strlen(t);
		const bool call = IsCall(t);

		// Words also count run on to others without a word gap: CQCQ, DEW1ABC,
		// NAMEDREW, QTHBOSTON, UR599, 73EE
		if (StartsWith(t, "CQ") || (Is(t, "QRZ") && (s_state == QSO_IDLE || s_state == QSO_DONE))) {
			o->cq = true;
		}
		if (strstr(t, "POTA") || strstr(t, "SOTA") || strstr(t, "WWFF") || strstr(t, "TEST")
			|| Is(t, "FD")) {
			o->special = true;
		}
		if (StartsWith(t, "QTH") && t[n - 1] != '?') {
			o->qth = true;
		}
		if (StartsWith(t, "QRS")) {
			o->qrs = true;
		} else if (StartsWith(t, "QRQ")) {
			o->qrq = true;
		}
		if (Is(t, "RIG") || Is(t, "ANT") || Is(t, "WX") || Is(t, "PWR")) {
			o->chat = true;
		}
		if (Is(t, "DE")) {
			if (IsCall(next)) {
				o->de = next;
			} else if (Join(i + 1, 2, de_call)) {
				o->de = de_call;
			}
		} else if (StartsWith(t, "DE") && IsCall(t + 2)) {
			o->de = t + 2;
		}
		if (HasRst(t)) {
			o->rst = true;
			o->rst_599 |= EndsWith(t, "599") || EndsWith(t, "5NN");
			o->cut |= strchr(t, 'N') != NULL;
		} else if (call && !Is(t, s_my) && !o->other) {
			o->other = t;
		}
		// its call, as keyed or with a gap or two in it
		for (uint8_t words = 1; words <= 3; words++) {
			char joined[CALL_SIZE];
			const char *c = (words == 1) ? (call && !HasRst(t) ? t : NULL) : Join(i, words, joined);
			if (c && !Is(c, s_my)) {
				const uint8_t d = Distance(c, s_call);
				if (d < o->best) {
					o->best = d;
				}
			}
		}
		if (Is(t, "NAME") || Is(t, "OP") || Is(t, "NM")) {
			if (Is(next, "IS") || Is(next, "HR")) {
				next = (i + 2 < s_tok_count) ? s_tok[i + 2] : "";
			}
			if (IsName(next)) {
				o->op = next;
			}
		} else if (StartsWith(t, "NAME") && IsName(t + 4)) {
			o->op = t + 4;
		}
		if (Is(t, "GM") || Is(t, "GA") || Is(t, "GE") || Is(t, "GD")) {
			o->greet = t;
		}
		if (StartsWith(t, "AGN") || (Is(t, "RPT") && (Is(prev, "PSE") || Is(prev, "PLS")))
			|| (Is(t, "QRZ") && s_state == QSO_ANSWER)) {
			// RPT on its own is a report, as in TNX FER RPT
			o->ask = true;
		}

		if (t[n - 1] == '?' && !StartsWith(t, "HW")) {
			// a bare ? asks about the word before it
			const char *what = (n == 1) ? prev : t;
			o->ask = true;
			if (StartsWith(what, "NAME") || StartsWith(what, "OP")) {
				o->ask_name = true;
			} else if (StartsWith(what, "QTH")) {
				o->ask_qth = true;
			} else if (StartsWith(what, "RST")) {
				o->ask_rst = true;
			}
		}
		// 73, also run on to another word, and SK
		if ((strstr(t, "73") && !HasRst(t) && !call) || Is(t, "SK")) {
			o->sign = true;
		}
	}

	if (s_tok_count) {
		// how it ends: K, KN or BK, alone or run on (BOSTONKN); dit dit, as EE, E E or 73EE
		const char *last = s_tok[s_tok_count - 1];
		o->end = EndMark(last);
		o->ditdit = (EndsWith(last, "EE") && !IsCall(last))
			|| (Is(last, "E") && s_tok_count > 1 && EndsWith(s_tok[s_tok_count - 2], "E"));
	}
}

// ---- the QSO ----

static void Tally(uint16_t *count)
{
	if (*count < 999) {
		(*count)++;
	}
	gUpdateDisplay = true;
}

// The QSO is over: logged when the station signed, failed otherwise, with notice
// saying which (or that the station left)
static void Done(bool logged, CPO_QsoNotice_t notice)
{
	CWSYNTH_Cancel(CWSYNTH_STATION);
	s_speaking = false;
	s_reply_due = false;
	s_state = QSO_DONE;
	Tally(logged ? &gCW_CpoQsoLogged : &gCW_CpoQsoFailed);
	strcpy(s_logged, s_call);
	Notice(notice);

	// An impatient station may call straight after the dit dit, without your CQ: more
	// often on a POTA or contest frequency, and sooner
	const CPO_QsoDifficulty_t *d = &DIFFICULTY[gEeprom.CW_QSO_DIFFICULTY % CW_QSO_DIFFICULTIES];
	s_tail_10ms = 0;
	if (logged && Rnd(100) < (uint32_t)d->tail_pct * (s_quick ? 2 : 1)) {
		s_tail_10ms = s_quick ? TAIL_MIN_10MS / 2 + Rnd(TAIL_SPAN_10MS / 2)
			: TAIL_MIN_10MS + Rnd(TAIL_SPAN_10MS);
	}
	gUpdateDisplay = true;
}

static void NewStation(void)
{
	const int8_t dx = CPO_Call_Generate(s_call);

	if (dx < 0) {
		const char *digit = s_call;
		while (*digit < '0' || *digit > '9') {
			digit++;
		}
		PickRandom(US_QTH[*digit - '0'], '|', s_qth, sizeof(s_qth));
	} else {
		PickItem(DX_QTH, '|', dx, s_qth, sizeof(s_qth));
	}
	PickRandom(NAMES, ' ', s_name, sizeof(s_name));
	PickRandom(RSTS, ' ', s_rst, sizeof(s_rst));

	// Its personality, within what the difficulty allows
	const CPO_QsoDifficulty_t *d = &DIFFICULTY[gEeprom.CW_QSO_DIFFICULTY % CW_QSO_DIFFICULTIES];
	int16_t wpm = gEeprom.CW_KEY_WPM + (int16_t)Rnd(2u * d->wpm_spread + 1u) - d->wpm_spread;
	s_wpm = (wpm < 10) ? 10 : (wpm > 45) ? 45 : (uint8_t)wpm;
	s_pitch = ((int16_t)Rnd(2u * d->pitch_spread + 1u) - d->pitch_spread) * 10;
	s_level_db = d->level_db + Rnd(d->level_span_db + 1u);
	s_qsb_db = (Rnd(100) < d->qsb_pct) ? 4 + Rnd(d->qsb_db - 3u) : 0;
	s_qsb_10ms = 300 + Rnd(900);   // fading over 3 to 12 s
	s_qsb_at = Rnd(s_qsb_10ms);
	CWSYNTH_SetFist(CWSYNTH_STATION,
		(Rnd(100) < d->hand_pct) ? (CWSYNTH_Fist_t)(CWSYNTH_FIST_BUG + Rnd(3)) : CWSYNTH_FIST_PADDLE,
		(Rnd(100) < d->chirp_pct) ? 20 + Rnd(40) : 0);
	s_qsk = Rnd(10) < 3;
	s_talker = Rnd(100) < 15;
}

// The station's pitch and level, every tick: off zero beat, down from full level by
// its strength, and fading through its QSB, deepest halfway through each cycle
static void UpdateVoice(void)
{
	uint32_t db = s_level_db;

	if (s_qsb_db) {
		if (++s_qsb_at >= s_qsb_10ms) {
			s_qsb_at = 0;
		}
		const uint32_t half = s_qsb_10ms / 2;
		const uint32_t off_mid = (s_qsb_at > half) ? s_qsb_at - half : half - s_qsb_at;
		db += s_qsb_db - s_qsb_db * off_mid / half;
	}
	uint32_t gain = CPO_StationGain();
	while (db--) {
		gain = gain * 3652u >> 12;   // 1 dB down
	}
	CWSYNTH_SetVoice(CWSYNTH_STATION, gEeprom.CW_TONE_FREQUENCY * 10 + s_pitch, gain);
}

// A station comes on, answering your CQ or tail-ending, and calls with its call: once
// on a POTA, SOTA or contest frequency, once or twice otherwise
static void NewQso(void)
{
	NewStation();
	s_op[0] = '\0';
	s_greet[0] = '\0';
	s_end[0] = '\0';
	s_calls = 1;
	s_prompted = false;
	s_brief = false;
	s_tail_10ms = 0;

	SayStart();
	if (!s_quick && Rnd(3) == 0) {
		Say("DE");
	}
	SayCall(s_quick ? 1 : 1 + Rnd(2));
	Reply(QSO_ANSWER);
	s_state = QSO_ANSWER;
}

// Ends a QSO under way: logged if the station had signed, else failed
static void Abandon(void)
{
	if (s_state == QSO_ANSWER || s_state == QSO_CONFIRM) {
		Done(false, NOTICE_NOT_LOGGED);
	} else if (s_state == QSO_DITDIT) {
		Done(true, NOTICE_LOGGED);
	}
}

static void HearCq(const CPO_QsoOver_t *o)
{
	const char *mine = o->de ? o->de : o->other;

	Abandon();
	if (mine && !Is(mine, s_my)) {
		strcpy(s_my, mine);
		Notice(NOTICE_OP);
	}
	if (!s_my[0]) {
		Notice(NOTICE_NO_CALL);
		s_state = QSO_IDLE;
		return;
	}

	s_quick = o->special;
	NewQso();
}

static void HearAnswer(const CPO_QsoOver_t *o)
{
	s_calls = 0;
	SayStart();
	if (o->best == 0) {
		// you have its call right: its report
		if (o->op) {
			strcpy(s_op, o->op);
		}
		if (o->greet) {
			strcpy(s_greet, o->greet);
		}
		strcpy(s_end, o->end ? o->end : "");
		// Without your name or QTH it keeps to the exchange, contest and POTA style:
		// a contest report for a contest report, and its cut numbers if you used them
		s_brief = !o->op && !o->qth;
		s_cut = o->cut || (!s_brief && Rnd(3) == 0);
		if (s_brief && o->rst_599) {
			strcpy(s_rst, "599");
		}
		if (s_brief) {
			// e.g. BK R R TU 5NN 5NN ID ID BK, its state twice for bad conditions
			char state[3];
			if (Is(s_end, "BK")) {
				SayEnd(s_end);
			}
			if (o->rst) {
				SayOneOf("R R|R|");
			}
			Say(s_greet);
			Say("TU");
			SayRst();
			if (State(state)) {
				SayTwice(state);
			}
			SayEnd(s_end);
			Reply(QSO_CONFIRM);
			return;
		}
		Say(s_my);
		Say("DE");
		Say(s_call);
		if (o->rst) {
			Say("R");
		}
		if (s_greet[0]) {
			Say(s_greet);
		} else {
			SayOneOf("GM|GA|GE|");
		}
		if (s_op[0]) {
			Say(s_op);
		} else {
			SayOneOf("OM|");
		}
		SayOneOf(o->rst ? "TNX FER CALL|TNX FER RPT|TU FER CALL" : "TNX FER CALL|TU FER CALL");
		SayOneOf("=|");
		const bool name_first = Rnd(2);
		if (name_first) {
			SayName();
		}
		SayRst();
		SayQth(Rnd(2));
		if (!name_first) {
			SayName();
		}
		if (o->chat || s_talker) {
			SayChat();
		}
		if (s_wpm + 5 < gEeprom.CW_KEY_WPM) {
			Say("PSE QRS");   // you're too fast for it
		}
		SayOneOf("HW?|HW CPY?|=");
		SayOver();
		Reply(QSO_CONFIRM);
		return;
	}
	// Asked for, nothing it could take for its call, or a call one or two off:
	// its call twice. Something further off: once, as if it heard someone else.
	SayCall((o->best <= 2 || o->best == 0xFF || o->ask) ? 2 : 1);
	Reply(QSO_ANSWER);
}

static void HearConfirm(const CPO_QsoOver_t *o)
{
	SayStart();

	if (o->ask) {
		const bool all = !(o->ask_name || o->ask_qth || o->ask_rst);
		if (all || o->ask_rst) {
			SayRst();
		}
		if (all || o->ask_name) {
			SayName();
		}
		if (all || o->ask_qth) {
			char state[3];
			if (s_brief && State(state)) {
				SayTwice(state);
			} else {
				SayQth(true);
			}
		}
		if (s_brief || s_end[0]) {
			SayEnd(s_end);
		} else {
			SayOneOf("(|K|\x02");
		}
		Reply(QSO_CONFIRM);
		return;
	}

	if (s_brief) {
		// A short sign: TU or 73 and dit dit, or just yours back
		SayOneOf(o->ditdit ? "EE|E E|73 EE|TU EE" : "73 EE|TU 73 EE|TU EE|73 TU EE|E E");
		Reply(o->ditdit ? QSO_DONE : QSO_DITDIT);
		return;
	}

	// It signs, rogering your 73, and ends with dit dit
	SayOneOf(o->sign ? "R R|R|R R TU" : "R");
	SayOneOf("TNX FER QSO|TNX FER FB QSO|TU FER QSO|FB");
	if (s_op[0]) {
		Say(s_op);
	} else {
		SayOneOf("OM|");
	}
	SayOneOf("73 GL|73 ES GL|73 CUL|HPE CUAGN 73|GL 73");
	if (Rnd(2)) {
		Say(s_my);
		Say("DE");
		Say(s_call);
	}
	Say("\x01");
	SayOneOf("EE|E E|TU EE|EE");
	Reply(o->ditdit ? QSO_DONE : QSO_DITDIT);
}

static void Hear(void)
{
	CPO_QsoOver_t o;

	Split();
	Parse(&o);
	if (o.qrs || o.qrq) {
		// it changes speed and says it again
		s_wpm = o.qrs ? ((s_wpm > 15) ? s_wpm - 5 : 10) : ((s_wpm < 40) ? s_wpm + 5 : 45);
		o.ask = true;
	}
	if (o.cq) {
		HearCq(&o);
	} else if (s_state == QSO_ANSWER) {
		HearAnswer(&o);
	} else if (s_state == QSO_CONFIRM) {
		HearConfirm(&o);
	} else if (s_state == QSO_DITDIT) {
		Done(true, NOTICE_LOGGED);
	}
	Unsplit();
}

// How long a pause ends your over: a few dits after K, KN, BK, SK, AR or a question,
// as an operator copying well comes straight back; a little over a second otherwise
static uint32_t OverGapMs(void)
{
	const uint32_t dit_ms = 1200u / gEeprom.CW_KEY_WPM;
	const char *gap = strrchr(s_over, ' ');
	const char *last = gap ? gap + 1 : s_over;
	const size_t n = strlen(last);
	const bool end = EndMark(last) || Is(last, "AR") || Is(last, "SK") || (n && last[n - 1] == '?')
		|| s_state == QSO_DITDIT;
	const uint32_t dits = end ? 4 * dit_ms : 15 * dit_ms;
	const uint32_t floor = end ? 250 : 1200;

	return dits > floor ? dits : floor;
}

static void Reset(void)
{
	CWSYNTH_Cancel(CWSYNTH_STATION);
	s_state = QSO_IDLE;
	s_reply_due = false;
	s_speaking = false;
	s_tail_10ms = 0;
	ClearOver();
	CW_ClearTxDisplay();
	gUpdateDisplay = true;
}

void CPO_Qso_Enter(void)
{
	gCW_CpoQsoLogged = 0;
	gCW_CpoQsoFailed = 0;
	s_notice_10ms = 0;
	Reset();
}

void CPO_Qso_Exit(void)
{
	Reset();
}

void CPO_Qso_Restart(void)
{
	Abandon();
	Reset();
}

void CPO_Qso_NextDifficulty(void)
{
	gEeprom.CW_QSO_DIFFICULTY = (gEeprom.CW_QSO_DIFFICULTY + 1) % CW_QSO_DIFFICULTIES;
	Notice(NOTICE_DIFFICULTY);
}

const char *CPO_Qso_DifficultyName(void)
{
	return DIFFICULTY_NAMES[gEeprom.CW_QSO_DIFFICULTY % CW_QSO_DIFFICULTIES];
}

// A prosign keyed run together reads as its letters, so BK, KN, UR or SK count the
// same however they were keyed; -.--. and .-.-. are KN and AR here, not ( and +
static const char *ProsignWord(char ch)
{
	switch (ch) {
	case CW_PROSIGN_SK:  return "SK";
	case CW_PROSIGN_BK:  return "BK";
	case CW_PROSIGN_KA:  return "KA";
	case CW_PROSIGN_SN:  return "SN";
	case CW_PROSIGN_UR:  return "UR";
	case CW_PROSIGN_SOS: return "SOS";
	case '(':            return "KN";
	case '+':            return "AR";
	case '=':            return "BT";
	case '&':            return "AS";
	default:             return NULL;
	}
}

void CPO_Qso_OnChar(char ch, bool word_gap)
{
	if (s_speaking) {
		if (!s_qsk) {
			// it can't hear you over its own sending
			Notice(NOTICE_DOUBLED);
			return;
		}
		// A QSK station hears you between its own elements: it stops and listens
		CWSYNTH_Cancel(CWSYNTH_STATION);
		s_speaking = false;
		s_state = s_after;
		if (s_state == QSO_DONE) {
			Done(true, NOTICE_LOGGED);
		}
	}
	if (ch == CW_CHAR_UNKNOWN) {
		return;
	}
	// Keying keeps a tail-ender off; more after the pause that ended the over holds
	// the station's reply, and it hears the whole over again
	s_tail_10ms = 0;
	s_over_heard = false;
	s_reply_due = false;

	// and a word of its own, even with no word gap around it
	const char *prosign = ProsignWord(ch);
	if ((word_gap || s_word_done || prosign) && s_over_len) {
		AddToOver(' ');
	}
	if (prosign) {
		while (*prosign) {
			AddToOver(*prosign++);
		}
	} else {
		AddToOver(ch);
	}
	s_word_done = prosign != NULL;
	s_last_char_ms = millis();
}

void CPO_Qso_Tick10ms(void)
{
	if (s_notice_10ms > 0 && --s_notice_10ms == 0) {
		gUpdateDisplay = true;
	}
	if (s_state != QSO_IDLE && s_state != QSO_DONE) {
		UpdateVoice();
	}

	if (s_speaking) {
		if (CWSYNTH_IsSending(CWSYNTH_STATION)) {
			return;
		}
		s_speaking = false;
		s_state = s_after;
		s_wait_10ms = 0;
		if (s_state == QSO_DONE) {
			Done(true, NOTICE_LOGGED);
		}
		gUpdateDisplay = true;
		return;
	}

	const bool idle = CW_KeyerIsIdle();

	if (s_over_len && !s_over_heard && idle && millis_since(s_last_char_ms) >= OverGapMs()) {
		s_over_heard = true;
		Hear();
		if (!s_reply_due) {
			ClearOver();
		}
		gUpdateDisplay = true;
	}

	if (s_reply_due) {
		// its reaction time runs while you're quiet
		if (!idle) {
			return;
		}
		if (s_reply_10ms > 0) {
			s_reply_10ms--;
			return;
		}
		s_reply_due = false;
		ClearOver();
		CW_ClearTxDisplay();
		CWSYNTH_Send(CWSYNTH_STATION, s_say, s_wpm, 0);
		s_speaking = CWSYNTH_IsSending(CWSYNTH_STATION);
		if (!s_speaking) {
			s_state = s_after;   // no synthesizer: on as if it had sent
		}
		gUpdateDisplay = true;
		return;
	}

	// Silence while it waits on you
	if (!idle || s_over_len) {
		s_wait_10ms = 0;
		return;
	}
	s_wait_10ms++;
	if (s_state == QSO_DONE && s_tail_10ms > 0 && --s_tail_10ms == 0) {
		NewQso();
		gUpdateDisplay = true;
	} else if (s_state == QSO_ANSWER && s_wait_10ms >= ANSWER_WAIT_10MS) {
		if (++s_calls > CALLS_MAX) {
			Done(false, NOTICE_LEFT);
		} else {
			SayStart();
			SayCall(1 + Rnd(2));
			Reply(QSO_ANSWER);
		}
		s_wait_10ms = 0;
	} else if (s_state == QSO_CONFIRM && s_wait_10ms >= CONFIRM_WAIT_10MS) {
		if (s_prompted) {
			Done(false, NOTICE_NOT_LOGGED);
		} else {
			s_prompted = true;
			SayStart();
			Say("?");
			Reply(QSO_CONFIRM);
		}
		s_wait_10ms = 0;
	} else if (s_state == QSO_DITDIT && s_wait_10ms >= DITDIT_WAIT_10MS) {
		Done(true, NOTICE_LOGGED);
	}
}

bool CPO_Qso_GetHint(char out[CPO_QSO_LINE_SIZE])
{
	// Only how to start: the rest goes by ear, as on the air
	if (s_my[0]) {
		return false;
	}
	strcpy(out, "CQ DE your call K");
	return true;
}

bool CPO_Qso_GetNotice(char out[CPO_QSO_LINE_SIZE])
{
	if (s_notice_10ms == 0) {
		return false;
	}
	switch (s_notice) {
	case NOTICE_DOUBLED:
		strcpy(out, "Doubled: unheard");
		break;
	case NOTICE_OP:
		sprintf_(out, "Op %s", s_my);
		break;
	case NOTICE_NO_CALL:
		strcpy(out, "DE your call?");
		break;
	case NOTICE_DIFFICULTY:
		sprintf_(out, "%s stations", CPO_Qso_DifficultyName());
		break;
	case NOTICE_LOGGED:
		sprintf_(out, "Logged %s", s_logged);
		break;
	case NOTICE_NOT_LOGGED:
		strcpy(out, "Not logged");
		break;
	default:
		strcpy(out, "Station left");
		break;
	}
	return true;
}
