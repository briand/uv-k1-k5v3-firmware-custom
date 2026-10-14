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
#define PILE_MAX    CW_QSO_PILE_MAX   // a synthesizer voice each, after the sidetone
#define PILE_ALL    ((1u << PILE_MAX) - 1)
#define CALLING_SIZE (2 * CALL_SIZE + 3)   // DE and a call twice

// In 10 ms ticks
#define REPLY_MIN_10MS    20     // the station's reaction time, from 0.2 s...
#define REPLY_SPAN_10MS   40     // ...to 0.6 s, half that on a POTA or contest frequency
#define ANSWER_WAIT_10MS  400    // silence after its call before it calls again
#define CONFIRM_WAIT_10MS 700    // silence after its report before it asks
#define DITDIT_WAIT_10MS  300    // how long it listens for your dit dit
#define NOTICE_10MS       150
#define TAIL_MIN_10MS     40     // the pile calls 0.4 s after the dit dit...
#define TAIL_SPAN_10MS    80     // ...to 1.2 s, half that on a POTA or contest frequency
#define STAGGER_MIN_10MS  20     // in a pile, each caller starts 0.2 s after the one before...
#define STAGGER_SPAN_10MS 40     // ...to 0.6 s, half that on a POTA or contest frequency
#define CAREFUL_PCT       60     // a caller asked for its call again makes it easier to copy
#define LATE_PCT          20     // a caller after the first may hold back to call in the clear...
#define LATE_MIN_10MS     150    // ...1.5 s later...
#define LATE_SPAN_10MS    100    // ...to 2.5 s
#define SPREAD_10HZ       30     // callers spread up to 300 Hz either side of zero beat
#define DRAIN_10MS        2000   // each 20 s waiting through your QSO with another costs a caller patience
#define QUIET_10MS        200    // each 2 s of silence, the band's activity drops a point

// The band's activity, 0-100: how many hear your CQ and call
#define ACTIVITY_START    60
#define ACTIVITY_CQ       15
#define ACTIVITY_QSO      10

// Your QSO rate as the band hears it, 0-255. A logged QSO adds RATE_QSO and a failed
// one takes half that; it fades a point a second for each RATE_QSO it holds. A pile
// earns one caller, and one more for each RATE_QSO: quick QSOs build it up, and
// slow ones and silence let it fade.
#define RATE_QSO          64
#define RATE_MAX          255
#define BONUS_PCT         35     // a chance of one caller more than earned

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
	uint8_t best[PILE_MAX];  // each caller: edit distance from its call to your nearest callsign, 0xFF for none
	uint8_t partial;         // the callers whose call holds part of one you sent: K7?, XYZ?, 7XY
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
	uint8_t wpm_down;        // WPM under yours at the slowest...
	uint8_t wpm_up;          // ...and over at the fastest
	uint8_t spacing_pct;     // chance it spaces its characters out (Farnsworth)
	uint8_t level_db;        // dB under full level at the strongest...
	uint8_t level_span_db;   // ...and how much weaker it can be
	uint8_t qsb_pct;         // chance it fades
	uint8_t qsb_db;          // as deep as this at most
	uint8_t pitch_spread;    // 10s of Hz either side of zero beat
	uint8_t hand_pct;        // chance of a bug, straight key or cootie over a keyer
	uint8_t chirp_pct;
	uint8_t tail_pct;        // chance a newcomer calls straight after the dit dit
	uint8_t sep_10hz;        // callers at least this far apart in pitch, 10s of Hz; 0 brings zero-beaters
	uint8_t lid_pct;         // chance a caller calls over whoever you come back to
	uint8_t patience;        // unanswered calls a caller makes before it leaves, at least; up to 3 more
} CPO_QsoDifficulty_t;

static const CPO_QsoDifficulty_t DIFFICULTY[CW_QSO_DIFFICULTIES] = {
	{ 4,  1, 30,  0,  6, 15,  6,  3, 10,  0, 20, 12,  0, 5 },   // easy: strong, about your speed or slower, clean keyers
	{ 6,  5, 20,  0, 18, 40, 12,  6, 40, 10, 35,  6, 10, 4 },   // medium
	{ 8, 10, 15,  4, 24, 70, 20, 15, 70, 25, 50,  0, 25, 3 },   // hard: some loud, many down near the noise
};

// A station on frequency: calling you, waiting its turn, or the one you're working.
// Its personality is rolled when it comes on.
typedef struct {
	char call[CALL_SIZE];
	char calling[CALLING_SIZE];   // what it sends calling you, unchanged until sent
	int8_t dx;               // its DX prefix, for its QTH; -1 for a US call
	uint8_t wpm;
	uint8_t eff;             // the speed its characters are spaced for, 0 for its own (Farnsworth)
	int16_t pitch;           // Hz off your sidetone
	uint8_t level_db;        // under full level
	uint8_t qsb_db;          // how deep it fades, 0 for not at all
	uint16_t qsb_10ms;       // and how slowly
	uint16_t qsb_at;
	uint16_t call_10ms;      // until it calls, 0 for no call coming
	uint8_t patience;        // unanswered calls, or 20 s waits through another's QSO, before it leaves
	uint8_t slot;            // its place in the pile
	bool on;
	bool qsk;                // it stops when you break in, and hears you
	bool talker;             // it ragchews whatever you send
	bool lid;                // it calls over whoever you come back to
} CPO_QsoStation_t;

static CPO_QsoState_t s_state = QSO_IDLE;
static CPO_QsoState_t s_after;     // the state once the station has done sending
static bool s_reply_due = false;   // what it says next is ready, and waits out its reaction time
static bool s_speaking = false;
static uint16_t s_reply_10ms;
static uint16_t s_wait_10ms;       // silence in a waiting state
static bool s_prompted;            // it has already asked once while waiting on your report
static bool s_quick;               // a POTA, SOTA, contest or field day CQ: it answers fast
static bool s_brief;               // you answered without name or QTH: it keeps to RST and state
static bool s_cut;                 // it sends its report in cut numbers
static char s_end[3];              // how you ended your over: K, KN, BK or nothing

static CPO_QsoStation_t s_pile[PILE_MAX];   // on station i + 1's synthesizer voice
static CPO_QsoStation_t *s_st;     // the station you're working, NULL until you come back to one
static uint16_t s_tail_10ms;       // after a QSO, the silence before the pile calls
static uint16_t s_drain_10ms;
static uint8_t s_missed;           // the callers that were sending over your over
static uint8_t s_activity;
static uint8_t s_quiet_10ms;
static uint8_t s_rate;
static uint8_t s_rate_10ms;

#define VOICE(st) (CWSYNTH_STATION + (st)->slot)
#define BIT(st)   (1u << (st)->slot)

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
static char s_name[NAME_SIZE];     // the worked station's
static char s_qth[QTH_SIZE];
static char s_rst[4];              // its report to you
static char s_op[NAME_SIZE];       // your name, if you sent it
static char s_greet[3];

static CPO_QsoNotice_t s_notice;
static char s_logged[CALL_SIZE];   // the call a "Logged" notice names, kept: the station
                                   // has gone, and its slot may take a newcomer
static uint8_t s_notice_10ms = 0;


// QSOs logged (the station signed) and failed (it left, gave up on you, or you left it)
uint16_t gCW_CpoQsoLogged = 0;
uint16_t gCW_CpoQsoFailed = 0;

static uint32_t Rnd(uint32_t n)
{
	return CPO_Call_Rand() % n;
}

// True pct times in a hundred
static bool Chance(uint8_t pct)
{
	return Rnd(100) < pct;
}

static bool Is(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

// How OneOf matches a word against its list
enum { WORD = 0, PREFIX, WITHIN };

// t is one of the words in list (split by |), starts with one, or holds one within it
static bool OneOf(const char *t, const char *list, uint8_t how)
{
	char item[8];

	for (;;) {
		uint8_t len = 0;
		while (*list && *list != '|') {
			item[len++] = *list++;
		}
		item[len] = '\0';
		if (how == WORD ? strcmp(t, item) == 0 : how == PREFIX ? strncmp(t, item, len) == 0
			: strstr(t, item) != NULL) {
			return true;
		}
		if (!*list++) {
			return false;
		}
	}
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

// Your call DE its call
static void SayCalls(void)
{
	Say(s_my);
	Say("DE");
	Say(s_st->call);
}

static void SayOver(void)
{
	SayCalls();
	if (s_end[0]) {
		SayEnd(s_end);
	} else {
		SayOneOf("(|(|K|\x02");
	}
}

// A station's reaction time: quicker on a POTA, SOTA or contest frequency
static uint8_t Reaction(void)
{
	return s_quick ? REPLY_MIN_10MS / 2 + Rnd(REPLY_SPAN_10MS / 2) : REPLY_MIN_10MS + Rnd(REPLY_SPAN_10MS);
}

static void Reply(CPO_QsoState_t after)
{
	s_after = after;
	s_reply_due = true;
	s_reply_10ms = Reaction();
}

// ---- your over ----

static void ClearOver(void)
{
	s_over_len = 0;
	s_over[0] = '\0';
	s_over_heard = false;
	s_word_done = false;
	s_missed = 0;
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
	return n == 3 || IsRst(head) || OneOf(head, "UR|RST|TU|R|RR|GM|GA|GE|BK|FB", WORD);
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

	if (n < 2 || n >= NAME_SIZE || OneOf(t, "IS|HR", WORD)) {
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
	memset(o->best, 0xFF, sizeof(o->best));

	for (uint8_t i = 0; i < s_tok_count; i++) {
		const char *t = s_tok[i];
		const char *prev = i ? s_tok[i - 1] : "";
		const char *next = (i + 1 < s_tok_count) ? s_tok[i + 1] : "";
		const size_t n = strlen(t);
		const bool call = IsCall(t);
		const bool rst = HasRst(t);

		// Words also count run on to others without a word gap: CQCQ, DEW1ABC,
		// NAMEDREW, QTHBOSTON, UR599, 73EE
		if (StartsWith(t, "CQ") || (Is(t, "QRZ") && (s_state == QSO_IDLE || s_state == QSO_DONE))) {
			o->cq = true;
		}
		if (OneOf(t, "POTA|SOTA|WWFF|TEST", WITHIN) || Is(t, "FD")) {
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
		if (OneOf(t, "RIG|ANT|WX|PWR", WORD)) {
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
		if (rst) {
			o->rst = true;
			o->rst_599 |= EndsWith(t, "599") || EndsWith(t, "5NN");
			o->cut |= strchr(t, 'N') != NULL;
		} else if (call && !Is(t, s_my) && !o->other) {
			o->other = t;
		}
		// a caller's call, as keyed or with a gap or two in it...
		for (uint8_t words = 1; words <= 3; words++) {
			char joined[CALL_SIZE];
			const char *c = (words == 1) ? (call && !rst ? t : NULL) : Join(i, words, joined);
			for (uint8_t k = 0; c && k < PILE_MAX; k++) {
				const uint8_t d = (s_pile[k].on && !Is(c, s_my)) ? Distance(c, s_pile[k].call) : 0xFF;
				if (d < o->best[k]) {
					o->best[k] = d;
				}
			}
		}
		// ...or part of one: with a ?, as K7? or XYZ?, or with a digit, as 7XY
		const size_t part_len = n - (t[n - 1] == '?');
		if (part_len >= 2 && part_len < CALL_SIZE && (part_len < n || strpbrk(t, "0123456789")) && !rst) {
			char part[CALL_SIZE];
			memcpy(part, t, part_len);
			part[part_len] = '\0';
			for (uint8_t k = 0; k < PILE_MAX; k++) {
				if (s_pile[k].on && strstr(s_pile[k].call, part) && !Is(s_pile[k].call, part)) {
					o->partial |= 1u << k;
				}
			}
		}
		if (OneOf(t, "NAME|OP|NM", WORD)) {
			if (OneOf(next, "IS|HR", WORD)) {
				next = (i + 2 < s_tok_count) ? s_tok[i + 2] : "";
			}
			if (IsName(next)) {
				o->op = next;
			}
		} else if (StartsWith(t, "NAME") && IsName(t + 4)) {
			o->op = t + 4;
		}
		if (OneOf(t, "GM|GA|GE|GD", WORD)) {
			o->greet = t;
		}
		if (StartsWith(t, "AGN") || (Is(t, "RPT") && OneOf(prev, "PSE|PLS", WORD))
			|| (Is(t, "QRZ") && s_state == QSO_ANSWER)) {
			// RPT on its own is a report, as in TNX FER RPT
			o->ask = true;
		}

		if (t[n - 1] == '?' && !StartsWith(t, "HW")) {
			// a bare ? asks about the word before it
			const char *what = (n == 1) ? prev : t;
			o->ask = true;
			if (OneOf(what, "NAME|OP", PREFIX)) {
				o->ask_name = true;
			} else if (StartsWith(what, "QTH")) {
				o->ask_qth = true;
			} else if (StartsWith(what, "RST")) {
				o->ask_rst = true;
			}
		}
		// 73, also run on to another word, and SK
		if ((strstr(t, "73") && !rst && !call) || Is(t, "SK")) {
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

static const CPO_QsoDifficulty_t *Difficulty(void)
{
	return &DIFFICULTY[gEeprom.CW_QSO_DIFFICULTY % CW_QSO_DIFFICULTIES];
}

static void Tally(uint16_t *count)
{
	if (*count < 999) {
		(*count)++;
	}
	gUpdateDisplay = true;
}

static void Busier(uint8_t points)
{
	s_activity = (s_activity + points < 100) ? s_activity + points : 100;
}

static uint8_t PileCount(void)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < PILE_MAX; i++) {
		n += s_pile[i].on;
	}
	return n;
}

static void Leave(CPO_QsoStation_t *st)
{
	st->on = false;
	st->call_10ms = 0;
	CWSYNTH_Cancel(VOICE(st));
}

// A round of waiting on you: false once a caller has run out of patience, and left
static bool Wait(CPO_QsoStation_t *st)
{
	if (st->patience == 0) {
		Leave(st);
		return false;
	}
	st->patience--;
	return true;
}

static bool CallTaken(const CPO_QsoStation_t *st)
{
	if (Is(st->call, s_my)) {
		return true;
	}
	for (const CPO_QsoStation_t *o = s_pile; o < s_pile + PILE_MAX; o++) {
		if (o != st && o->on && Is(o->call, st->call)) {
			return true;
		}
	}
	return false;
}

// How far a pitch is from the nearest other station on frequency
static int16_t Gap(const CPO_QsoStation_t *st, int16_t pitch)
{
	int16_t gap = 0x7FFF;
	for (const CPO_QsoStation_t *o = s_pile; o < s_pile + PILE_MAX; o++) {
		const int16_t d = (o->pitch > pitch) ? o->pitch - pitch : pitch - o->pitch;
		if (o != st && o->on && d < gap) {
			gap = d;
		}
	}
	return gap;
}

// Wider spaces between its characters: a few WPM under its speed, or under what it
// spaced them for already
static void Spaced(CPO_QsoStation_t *st)
{
	const int16_t eff = (st->eff ? st->eff : st->wpm) - 3 - (int16_t)Rnd(3);
	st->eff = (eff > 5) ? eff : 5;
}

// A station comes on frequency to call you, if the pile has room for it, with its own
// call and a personality within what the difficulty allows
static void Arrive(void)
{
	const CPO_QsoDifficulty_t *d = Difficulty();
	CPO_QsoStation_t *st = s_pile;
	uint8_t tries = 4;

	if (PileCount() >= gEeprom.CW_QSO_PILE) {
		return;
	}
	while (st->on) {
		st++;
	}
	do {
		st->dx = CPO_Call_Generate(st->call);
	} while (CallTaken(st) && --tries);

	int16_t wpm = gEeprom.CW_KEY_WPM + (int16_t)Rnd(d->wpm_down + d->wpm_up + 1u) - d->wpm_down;
	st->wpm = (wpm < 10) ? 10 : (wpm > 45) ? 45 : (uint8_t)wpm;
	st->eff = 0;
	if (Chance(d->spacing_pct)) {
		Spaced(st);
	}
	// Off zero beat, and in a pile far enough from the others to pick out, unless the
	// difficulty brings zero-beaters: the first pitch clear enough from a random
	// start across the spread, else the clearest
	int16_t pitch = ((int16_t)Rnd(2u * d->pitch_spread + 1u) - d->pitch_spread) * 10;
	int16_t gap = Gap(st, pitch);
	const uint8_t from = Rnd(2u * SPREAD_10HZ + 1u);
	for (uint8_t i = 0; i <= 2 * SPREAD_10HZ && gap < d->sep_10hz * 10; i++) {
		const int16_t other = ((int16_t)((from + i) % (2 * SPREAD_10HZ + 1)) - SPREAD_10HZ) * 10;
		if (Gap(st, other) > gap) {
			pitch = other;
			gap = Gap(st, other);
		}
	}
	st->pitch = pitch;
	st->level_db = d->level_db + Rnd(d->level_span_db + 1u);
	st->qsb_db = Chance(d->qsb_pct) ? 4 + Rnd(d->qsb_db - 3u) : 0;
	st->qsb_10ms = 300 + Rnd(900);   // fading over 3 to 12 s
	st->qsb_at = Rnd(st->qsb_10ms);
	// a keyer, plain half the time, else weighted or (above Easy) ragged; or a hand key
	CWSYNTH_SetFist(VOICE(st),
		Chance(d->hand_pct) ? (CWSYNTH_Fist_t)(CWSYNTH_FIST_BUG + Rnd(3))
		: Rnd(2) ? CWSYNTH_FIST_PADDLE
		: (CWSYNTH_Fist_t)(CWSYNTH_FIST_WEIGHTED + Rnd(gEeprom.CW_QSO_DIFFICULTY == CW_QSO_EASY ? 2 : 3)),
		Chance(d->chirp_pct) ? 20 + Rnd(40) : 0);
	st->qsk = Rnd(10) < 3;
	st->talker = Chance(15);
	st->lid = Chance(d->lid_pct);
	st->patience = d->patience + Rnd(4);
	st->call_10ms = 0;
	st->on = true;
}

// Newcomers join until the pile holds what your rate has earned: one, sometimes two,
// from a standing start, up to the pile-up size as you work them quickly
static void Fill(void)
{
	uint8_t want = 1 + s_rate / RATE_QSO + Chance(BONUS_PCT);

	if (want > gEeprom.CW_QSO_PILE) {
		want = gEeprom.CW_QSO_PILE;
	}
	while (PileCount() < want) {
		Arrive();
	}
}

// A caller calls you with its call, times over (0: once or twice, as it likes), in
// at_10ms. One still calling goes on.
static void Call(CPO_QsoStation_t *st, uint8_t times, uint16_t at_10ms)
{
	if (CWSYNTH_IsSending(VOICE(st))) {
		return;
	}
	st->calling[0] = '\0';
	if (!s_quick && Rnd(3) == 0) {
		strcpy(st->calling, "DE ");
	}
	if (!times) {
		times = 1 + Rnd(2);
	}
	while (times--) {
		strcat(st->calling, st->call);
		if (times) {
			strcat(st->calling, " ");
		}
	}
	st->call_10ms = 1 + at_10ms;
}

// How long after one caller the next comes in
static uint16_t Stagger(void)
{
	const uint16_t gap = STAGGER_MIN_10MS + Rnd(STAGGER_SPAN_10MS);
	return s_quick ? gap / 2 : gap;
}

// The callers in mask call, and the lids whoever you came back to; the rest stand by.
// They come in, in no set order, one after another a moment apart from the first's
// reaction time, and now and then one holds back to call in the clear once the rest
// are done. A call that may go unanswered costs a caller some patience, and one out of
// it leaves instead; standing by costs nothing. Asked for its call again (careful), a
// caller is hopeful, and often makes it easier to copy from then on: a touch slower,
// or its characters spaced wider.
static void Round(uint8_t mask, uint8_t times, bool careful)
{
	const uint8_t from = Rnd(PILE_MAX);
	uint16_t at = Reaction();
	uint8_t late = 0;
	bool first = true;
	CPO_QsoStation_t *st;

	for (uint8_t i = 0; i < PILE_MAX; i++) {
		st = &s_pile[(from + i) % PILE_MAX];
		const bool asked = careful && (mask & BIT(st));
		if (st->on && st != s_st && ((mask & BIT(st)) || st->lid) && (asked || Wait(st))) {
			if (asked && Chance(CAREFUL_PCT)) {
				if (Rnd(2)) {
					st->wpm = (st->wpm > 12) ? st->wpm - 2 - Rnd(2) : 10;
				} else {
					Spaced(st);
				}
			}
			if (!first && Chance(LATE_PCT)) {
				late |= BIT(st);
			} else {
				Call(st, times, at);
				at += Stagger();
				first = false;
			}
		}
	}
	at += LATE_MIN_10MS + Rnd(LATE_SPAN_10MS);
	for (st = s_pile; st < s_pile + PILE_MAX; st++) {
		if (late & BIT(st)) {
			Call(st, times, at);
			at += Stagger();
		}
	}
}

// The QSO is over: logged when the station signed, failed otherwise, with a notice
// saying which. The station goes, and the pile calls once you've been quiet a moment.
// A logged QSO builds your rate, and those who heard it join the pile up to what the
// rate has earned; with none left waiting, an impatient one may call anyway, more
// often (and sooner) on a POTA or contest frequency and the higher your rate.
static void Done(bool logged, CPO_QsoNotice_t notice)
{
	s_speaking = false;
	s_reply_due = false;
	s_state = QSO_DONE;
	Tally(logged ? &gCW_CpoQsoLogged : &gCW_CpoQsoFailed);
	strcpy(s_logged, s_st->call);
	Leave(s_st);
	s_st = NULL;
	Notice(notice);

	if (logged) {
		Busier(ACTIVITY_QSO);
		s_rate = (s_rate + RATE_QSO < RATE_MAX) ? s_rate + RATE_QSO : RATE_MAX;
		if (PileCount() || Chance(Difficulty()->tail_pct * (s_quick ? 2u : 1u) + s_rate / 4u)) {
			Fill();
		}
	} else {
		s_rate -= (s_rate < RATE_QSO / 2) ? s_rate : RATE_QSO / 2;
	}
	s_tail_10ms = s_quick ? TAIL_MIN_10MS / 2 + Rnd(TAIL_SPAN_10MS / 2)
		: TAIL_MIN_10MS + Rnd(TAIL_SPAN_10MS);
	s_wait_10ms = 0;
}

// You came back to a caller with its call: the QSO is with it now
static void Pick(CPO_QsoStation_t *st)
{
	s_st = st;
	st->call_10ms = 0;
	if (st->dx < 0) {
		// Its call area: the last digit, which a portable call's / puts after its home one
		char area = '0';
		for (const char *c = st->call; *c; c++) {
			if (*c >= '0' && *c <= '9') {
				area = *c;
			}
		}
		PickRandom(US_QTH[area - '0'], '|', s_qth, sizeof(s_qth));
	} else {
		PickItem(DX_QTH, '|', st->dx, s_qth, sizeof(s_qth));
	}
	PickRandom(NAMES, ' ', s_name, sizeof(s_name));
	PickRandom(RSTS, ' ', s_rst, sizeof(s_rst));
	s_op[0] = '\0';
	s_greet[0] = '\0';
	s_prompted = false;
	s_drain_10ms = 0;
}

// A station's pitch and level, every tick: off zero beat, down from top by its
// strength, and fading through its QSB, deepest halfway through each cycle
static void UpdateVoice(CPO_QsoStation_t *st, uint32_t top)
{
	uint32_t db = st->level_db;

	if (st->qsb_db) {
		if (++st->qsb_at >= st->qsb_10ms) {
			st->qsb_at = 0;
		}
		const uint32_t half = st->qsb_10ms / 2;
		const uint32_t off_mid = (st->qsb_at > half) ? st->qsb_at - half : half - st->qsb_at;
		db += st->qsb_db - st->qsb_db * off_mid / half;
	}
	while (db--) {
		top = top * 3652u >> 12;   // 1 dB down
	}
	CWSYNTH_SetVoice(VOICE(st), gEeprom.CW_TONE_FREQUENCY * 10 + st->pitch, top);
}

// Ends a QSO under way: logged if the station had signed, else failed
static void Abandon(void)
{
	if (s_st) {
		const bool signed_off = s_state == QSO_DITDIT;
		Done(signed_off, signed_off ? NOTICE_LOGGED : NOTICE_NOT_LOGGED);
	}
}

// A CQ: the more you call, the more hear you. The callers still on frequency call
// again, and newcomers join as the band's activity allows; with nobody, CQ again.
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

	// Someone comes to an empty frequency as the band's activity allows (30% even when
	// it's dead); how many is down to your rate
	s_quick = o->special;
	Busier(ACTIVITY_CQ);
	if (PileCount() || Chance(30u + s_activity * 7u / 10u)) {
		Fill();
	}
	Round(PILE_ALL, s_quick ? 1 : 0, false);
	s_state = PileCount() ? QSO_ANSWER : QSO_IDLE;
}

// You came back to the station with its call: its report
static void HearAnswer(const CPO_QsoOver_t *o)
{
	SayStart();
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
	SayCalls();
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
	if (o->chat || s_st->talker) {
		SayChat();
	}
	if (s_st->wpm + 5 < gEeprom.CW_KEY_WPM) {
		Say("PSE QRS");   // you're too fast for it
	}
	SayOneOf("HW?|HW CPY?|=");
	SayOver();
	Reply(QSO_CONFIRM);
}

// The pile called you. Come back to one with its call and it sends its report while
// the others stand by. Part of a call (K7?, XYZ?, 7XY), or one a character or two
// off, and the callers it could be (the closest) send theirs twice. Asked for, or
// nothing like a call, and they all call again; a call none of them has, once. A
// caller sending over your over didn't hear it.
static void HearPile(const CPO_QsoOver_t *o)
{
	uint8_t heard = 0, near = 0, closest = 0xFF;
	CPO_QsoStation_t *st;

	for (st = s_pile; st < s_pile + PILE_MAX; st++) {
		if (st->on && !(s_missed & BIT(st))) {
			heard |= BIT(st);
			if (o->best[st->slot] < closest) {
				closest = o->best[st->slot];
			}
		}
	}
	for (st = s_pile; st < s_pile + PILE_MAX; st++) {
		if (!(heard & BIT(st))) {
			continue;
		}
		const bool nearest = o->best[st->slot] == closest;
		if (nearest && closest == 0) {
			Pick(st);
			Round(0, 1, false);   // the rest stand by, but a lid calls over it
			HearAnswer(o);
			return;
		}
		if ((nearest && closest <= 2) || (o->partial & BIT(st))) {
			near |= BIT(st);
		}
	}
	if (!heard) {
		Notice(NOTICE_DOUBLED);
		return;
	}
	Round(near ? near : heard, near ? 2 : (o->other && !o->ask) ? 1 : 0, near != 0);
	if (!PileCount()) {
		Notice(NOTICE_LEFT);
		s_state = QSO_DONE;
	}
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
		SayCalls();
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
		// they change speed and say it again
		for (CPO_QsoStation_t *st = s_pile; st < s_pile + PILE_MAX; st++) {
			st->wpm = o.qrs ? ((st->wpm > 15) ? st->wpm - 5 : 10) : ((st->wpm < 40) ? st->wpm + 5 : 45);
		}
		o.ask = true;
	}
	if (o.cq) {
		HearCq(&o);
	} else if (s_state == QSO_ANSWER) {
		HearPile(&o);
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
	const bool end = EndMark(last) || OneOf(last, "AR|SK", WORD) || (n && last[n - 1] == '?')
		|| s_state == QSO_DITDIT;
	const uint32_t dits = end ? 4 * dit_ms : 15 * dit_ms;
	const uint32_t floor = end ? 250 : 1200;

	return dits > floor ? dits : floor;
}

static void Reset(void)
{
	for (CPO_QsoStation_t *st = s_pile; st < s_pile + PILE_MAX; st++) {
		Leave(st);
	}
	s_st = NULL;
	s_state = QSO_IDLE;
	s_reply_due = false;
	s_speaking = false;
	ClearOver();
	CW_ClearTxDisplay();
	gUpdateDisplay = true;
}

void CPO_Qso_Enter(void)
{
	for (uint8_t i = 0; i < PILE_MAX; i++) {
		s_pile[i].slot = i;
	}
	gCW_CpoQsoLogged = 0;
	gCW_CpoQsoFailed = 0;
	s_notice_10ms = 0;
	s_activity = ACTIVITY_START;
	s_rate = 0;
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

const char *CPO_Qso_DifficultyName(void)
{
	return DIFFICULTY_NAMES[gEeprom.CW_QSO_DIFFICULTY % CW_QSO_DIFFICULTIES];
}

// A prosign keyed run together reads as its letters, so BK, KN, UR or SK count the
// same however they were keyed; -.--. and .-.-. are KN and AR here, not ( and +
static const char *ProsignWord(char ch)
{
	// each prosign's code, then its letters
	static const char WORDS[] = "\x01" "SK\0" "\x02" "BK\0" "\x03" "KA\0" "\x04" "SN\0" "\x05" "UR\0"
		"\x06" "SOS\0" "(KN\0" "+AR\0" "=BT\0" "&AS";

	for (const char *w = WORDS; w < WORDS + sizeof(WORDS); w += strlen(w) + 1) {
		if (*w == ch) {
			return w + 1;
		}
	}
	return NULL;
}

void CPO_Qso_OnChar(char ch, bool word_gap)
{
	// Callers about to call hold off while you send. One calling as you key stops and
	// listens if it's QSK; otherwise it misses your over.
	for (CPO_QsoStation_t *st = s_pile; st < s_pile + PILE_MAX; st++) {
		st->call_10ms = 0;
		if (st->on && st != s_st && CWSYNTH_IsSending(VOICE(st))) {
			if (st->qsk) {
				CWSYNTH_Cancel(VOICE(st));
			} else {
				s_missed |= BIT(st);
			}
		}
	}
	if (s_speaking) {
		if (!s_st->qsk) {
			// it can't hear you over its own sending
			Notice(NOTICE_DOUBLED);
			return;
		}
		// A QSK station hears you between its own elements: it stops and listens
		CWSYNTH_Cancel(VOICE(s_st));
		s_speaking = false;
		s_state = s_after;
		if (s_state == QSO_DONE) {
			Done(true, NOTICE_LOGGED);
		}
	}
	if (ch == CW_CHAR_UNKNOWN) {
		return;
	}
	// More keying after the pause that ended the over holds the station's reply, and
	// it hears the whole over again
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
	const bool idle = CW_KeyerIsIdle();
	bool busy = false;   // a station sending, or about to call

	if (s_notice_10ms > 0 && --s_notice_10ms == 0) {
		gUpdateDisplay = true;
	}

	// Each station's level comes down as the pile grows, so their calls sum clear of
	// the DAC's limits. A call due goes out once you've stopped sending.
	const uint32_t top = CPO_StationGain() * 2u / (PileCount() + 1u);
	for (CPO_QsoStation_t *st = s_pile; st < s_pile + PILE_MAX; st++) {
		if (!st->on) {
			continue;
		}
		UpdateVoice(st, top);
		if (st->call_10ms && idle && --st->call_10ms == 0) {
			CWSYNTH_Send(VOICE(st), st->calling, st->wpm, st->eff);
		}
		busy |= st->call_10ms || CWSYNTH_IsSending(VOICE(st));
	}

	// Your rate fades, the band thins out while you're quiet, and callers waiting
	// through your QSO with another lose patience
	if (++s_rate_10ms >= 100) {
		s_rate_10ms = 0;
		s_rate -= (s_rate + RATE_QSO - 1) / RATE_QSO;
	}
	if (idle && !busy && !s_over_len && !s_reply_due) {
		if (++s_quiet_10ms >= QUIET_10MS) {
			s_quiet_10ms = 0;
			s_activity -= (s_activity > 0);
		}
	} else {
		s_quiet_10ms = 0;
	}
	if (s_st && ++s_drain_10ms >= DRAIN_10MS) {
		s_drain_10ms = 0;
		for (CPO_QsoStation_t *st = s_pile; st < s_pile + PILE_MAX; st++) {
			if (st->on && st != s_st) {
				Wait(st);
			}
		}
	}

	if (s_speaking) {
		if (CWSYNTH_IsSending(VOICE(s_st))) {
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
		CWSYNTH_Send(VOICE(s_st), s_say, s_st->wpm, s_st->eff);
		s_speaking = CWSYNTH_IsSending(VOICE(s_st));
		if (!s_speaking) {
			s_state = s_after;   // no synthesizer: on as if it had sent
		}
		gUpdateDisplay = true;
		return;
	}

	// Silence while they wait on you. After a QSO the pile calls; unanswered, it calls
	// again, as long as its patience holds.
	if (!idle || s_over_len || busy) {
		s_wait_10ms = 0;
		return;
	}
	s_wait_10ms++;
	if ((s_state == QSO_DONE && PileCount() && s_wait_10ms >= s_tail_10ms)
		|| (s_state == QSO_ANSWER && s_wait_10ms >= ANSWER_WAIT_10MS)) {
		const bool after_qso = s_state == QSO_DONE;
		Round(PILE_ALL, (after_qso && s_quick) ? 1 : 0, false);
		s_state = PileCount() ? QSO_ANSWER : QSO_DONE;
		if (!after_qso && s_state == QSO_DONE) {
			Notice(NOTICE_LEFT);
		}
		gUpdateDisplay = true;
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
	// Each notice's text, taking your call or the one logged
	static const char *const TEXT[] = {
		[NOTICE_DOUBLED]    = "Doubled: unheard",
		[NOTICE_OP]         = "Op %s",
		[NOTICE_NO_CALL]    = "DE your call?",
		[NOTICE_LEFT]       = "No callers left",
		[NOTICE_LOGGED]     = "Logged %s",
		[NOTICE_NOT_LOGGED] = "Not logged",
	};
	sprintf_(out, TEXT[s_notice], (s_notice == NOTICE_OP) ? s_my : s_logged);
	return true;
}
