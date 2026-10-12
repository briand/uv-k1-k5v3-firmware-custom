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
#define SAY_SIZE    160  // what the station sends next
#define TOKENS_MAX  24
#define CALL_SIZE   11   // room for a portable call such as VE3/W1ABC
#define NAME_SIZE   9
#define QTH_SIZE    16

// In 10 ms ticks
#define REPLY_MIN_10MS    60     // the station's reaction time, from 0.6 s...
#define REPLY_SPAN_10MS   90     // ...to 1.5 s
#define ANSWER_WAIT_10MS  600    // silence after its call before it calls again
#define CONFIRM_WAIT_10MS 1000   // silence after its report before it asks
#define DITDIT_WAIT_10MS  400    // how long it listens for your dit dit
#define NOTICE_10MS       150
#define CALLS_MAX         3      // calls with no answer before it leaves

typedef enum {
	QSO_IDLE = 0,   // waiting for a CQ
	QSO_ANSWER,     // it has called you, and waits for you to come back to it
	QSO_CONFIRM,    // it has sent its report, and waits for yours back and 73
	QSO_DITDIT,     // it has signed, and listens for your dit dit
	QSO_DONE,       // the score is up
} CPO_QsoState_t;

typedef enum {
	NOTICE_DOUBLED = 0,
	NOTICE_OP,
	NOTICE_NO_CALL,
	NOTICE_LEFT,
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
	bool tu;
	bool ditdit;             // it ends in EE or E E
	bool name_ok, qth_ok;    // its name or a word of its QTH came back
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
static uint8_t s_notice_10ms = 0;

typedef struct {
	uint8_t tries;      // overs it took you to get its call right
	uint8_t asked;      // repeats you asked for
	bool worked;        // you got its call right
	bool brief;         // RST and state only, no name or QTH
	bool rst;           // you sent a report
	bool name, qth;     // you sent them back
	bool sign;          // you sent 73 or SK
	bool ditdit;
	bool finished;      // it signed
	uint32_t start_ms;
	uint32_t end_ms;
} CPO_QsoScore_t;

static CPO_QsoScore_t s_score;
static CPO_QsoScore_t s_score_before;   // as it was before the over being heard
static bool s_rehear = false;           // the over grew after it was heard

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
	s_rehear = false;
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

static bool IsCall(const char *t)
{
	bool digit = false, letter = false;
	const size_t n = strlen(t);

	if (n < 3 || n >= CALL_SIZE || IsRst(t)) {
		return false;
	}
	for (; *t; t++) {
		if (*t >= '0' && *t <= '9') {
			digit = true;
		} else if (*t >= 'A' && *t <= 'Z') {
			letter = true;
		} else if (*t != '/') {
			return false;
		}
	}
	return digit && letter;
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

static bool InQth(const char *t)
{
	char words[QTH_SIZE];
	const uint8_t n = CountItems(s_qth, ' ');

	for (uint8_t i = 0; i < n; i++) {
		PickItem(s_qth, ' ', i, words, sizeof(words));
		if (Is(t, words)) {
			return true;
		}
	}
	return false;
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
	memset(o, 0, sizeof(*o));
	o->best = 0xFF;

	for (uint8_t i = 0; i < s_tok_count; i++) {
		const char *t = s_tok[i];
		const char *prev = i ? s_tok[i - 1] : "";
		const char *next = (i + 1 < s_tok_count) ? s_tok[i + 1] : "";
		const size_t n = strlen(t);

		if (Is(t, "CQ") || (Is(t, "QRZ") && (s_state == QSO_IDLE || s_state == QSO_DONE))) {
			o->cq = true;
		} else if (Is(t, "POTA") || Is(t, "SOTA") || Is(t, "WWFF") || Is(t, "TEST")
			|| Is(t, "CONTEST") || Is(t, "FD")) {
			o->special = true;
		} else if (Is(t, "QTH")) {
			o->qth = true;
		} else if (Is(t, "TU")) {
			o->tu = true;
		} else if (Is(t, "DE")) {
			if (IsCall(next)) {
				o->de = next;
			}
		} else if (IsRst(t)) {
			o->rst = true;
			o->rst_599 |= Is(t, "599") || Is(t, "5NN");
			o->cut |= strchr(t, 'N') != NULL;
		} else if (IsCall(t) && !Is(t, s_my)) {
			const uint8_t d = Distance(t, s_call);
			if (d < o->best) {
				o->best = d;
			}
			if (!o->other) {
				o->other = t;
			}
		} else if (Is(t, "NAME") || Is(t, "OP") || Is(t, "NM")) {
			if (Is(next, "IS") || Is(next, "HR")) {
				next = (i + 2 < s_tok_count) ? s_tok[i + 2] : "";
			}
			if (IsName(next)) {
				o->op = next;
			}
		} else if (Is(t, "GM") || Is(t, "GA") || Is(t, "GE") || Is(t, "GD")) {
			o->greet = t;
		} else if (Is(t, "AGN") || (Is(t, "RPT") && (Is(prev, "PSE") || Is(prev, "PLS")))
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
		// 73, also run on to a word without a word gap, and SK
		if ((strstr(t, "73") && !IsRst(t) && !IsCall(t)) || Is(t, "SK")) {
			o->sign = true;
		}
		if (s_name[0] && Is(t, s_name)) {
			o->name_ok = true;
		}
		if (s_qth[0] && InQth(t)) {
			o->qth_ok = true;
		}
	}

	if (s_tok_count) {
		const char *last = s_tok[s_tok_count - 1];
		if (Is(last, "K") || Is(last, "KN") || Is(last, "BK")) {
			o->end = last;
		}
		o->ditdit = Is(last, "EE")
			|| (Is(last, "E") && s_tok_count > 1 && Is(s_tok[s_tok_count - 2], "E"));
	}
}

// ---- the QSO ----

static void Done(bool finished)
{
	CWSYNTH_Cancel(CWSYNTH_STATION);
	s_speaking = false;
	s_reply_due = false;
	s_state = QSO_DONE;
	s_score.finished = finished;
	s_score.end_ms = millis();
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
}

static void HearCq(const CPO_QsoOver_t *o)
{
	const char *mine = o->de ? o->de : o->other;

	CWSYNTH_Cancel(CWSYNTH_STATION);
	s_speaking = false;
	if (mine && !Is(mine, s_my)) {
		strcpy(s_my, mine);
		Notice(NOTICE_OP);
	}
	if (!s_my[0]) {
		Notice(NOTICE_NO_CALL);
		s_state = QSO_IDLE;
		return;
	}

	NewStation();
	memset(&s_score, 0, sizeof(s_score));
	s_score.start_ms = millis();
	s_op[0] = '\0';
	s_greet[0] = '\0';
	s_end[0] = '\0';
	s_calls = 1;
	s_prompted = false;
	s_brief = false;
	s_quick = o->special;

	// It answers your CQ with its call: once on a POTA, SOTA or contest frequency,
	// once or twice otherwise
	SayStart();
	if (!s_quick && Rnd(3) == 0) {
		Say("DE");
	}
	SayCall(s_quick ? 1 : 1 + Rnd(2));
	Reply(QSO_ANSWER);
	s_state = QSO_ANSWER;
}

static void HearAnswer(const CPO_QsoOver_t *o)
{
	s_calls = 0;
	SayStart();
	if (o->best == 0) {
		// you have its call right: its report
		s_score.tries++;
		s_score.worked = true;
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
		s_score.brief = s_brief;
		s_score.rst = o->rst;
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
		SayOneOf("HW?|HW CPY?|=");
		SayOver();
		Reply(QSO_CONFIRM);
		return;
	}
	if (!o->ask && o->best != 0xFF) {
		s_score.tries++;
	}
	// Asked for, nothing it could take for its call, or a call one or two off:
	// its call twice. Something further off: once, as if it heard someone else.
	SayCall((o->best <= 2 || o->best == 0xFF || o->ask) ? 2 : 1);
	Reply(QSO_ANSWER);
}

static void HearConfirm(const CPO_QsoOver_t *o)
{
	s_score.name |= o->name_ok;
	s_score.qth |= o->qth_ok;
	SayStart();

	if (o->ask) {
		s_score.asked++;
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

	s_score.ditdit = o->ditdit;
	if (s_brief) {
		// A short sign: TU or 73 and dit dit, or just yours back
		s_score.sign = o->sign || o->tu;
		SayOneOf(o->ditdit ? "EE|E E|73 EE|TU EE" : "73 EE|TU 73 EE|TU EE|73 TU EE|E E");
		Reply(o->ditdit ? QSO_DONE : QSO_DITDIT);
		return;
	}

	// It signs, rogering your 73, and ends with dit dit
	s_score.sign = o->sign;
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
	if (o.cq) {
		HearCq(&o);
	} else if (s_state == QSO_ANSWER) {
		HearAnswer(&o);
	} else if (s_state == QSO_CONFIRM) {
		HearConfirm(&o);
	} else if (s_state == QSO_DITDIT) {
		s_score.ditdit = o.ditdit;
		Done(true);
	}
	Unsplit();
}

// How long a pause ends your over: short after K, KN, BK, SK, AR or a question,
// longer otherwise, and never under a few dits at your speed
static uint32_t OverGapMs(void)
{
	const uint32_t dit_ms = 1200u / gEeprom.CW_KEY_WPM;
	const char *gap = strrchr(s_over, ' ');
	const char *last = gap ? gap + 1 : s_over;
	const size_t n = strlen(last);
	const bool end = Is(last, "K") || Is(last, "KN") || Is(last, "BK") || Is(last, "AR")
		|| Is(last, "SK") || (n && last[n - 1] == '?') || s_state == QSO_DITDIT;
	const uint32_t dits = end ? 10 * dit_ms : 30 * dit_ms;
	const uint32_t floor = end ? 700 : 2000;

	return dits > floor ? dits : floor;
}

void CPO_Qso_Enter(void)
{
	s_state = QSO_IDLE;
	s_reply_due = false;
	s_speaking = false;
	s_notice_10ms = 0;
	ClearOver();
	CW_ClearTxDisplay();
	gUpdateDisplay = true;
}

void CPO_Qso_Exit(void)
{
	CWSYNTH_Cancel(CWSYNTH_STATION);
	CPO_Qso_Enter();
}

void CPO_Qso_Restart(void)
{
	CPO_Qso_Exit();
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
		// it can't hear you over its own sending
		Notice(NOTICE_DOUBLED);
		return;
	}
	if (ch == CW_CHAR_UNKNOWN) {
		return;
	}
	// More after the pause that ended the over: hold its reply and hear it all again
	if (s_over_heard) {
		s_rehear = true;
	}
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

	if (s_speaking) {
		if (CWSYNTH_IsSending(CWSYNTH_STATION)) {
			return;
		}
		s_speaking = false;
		s_state = s_after;
		s_wait_10ms = 0;
		if (s_state == QSO_DONE) {
			Done(true);
		}
		gUpdateDisplay = true;
		return;
	}

	const bool idle = CW_KeyerIsIdle();

	if (s_over_len && !s_over_heard && idle && millis_since(s_last_char_ms) >= OverGapMs()) {
		// Heard again, the whole over counts once
		if (s_rehear) {
			s_score = s_score_before;
		} else {
			s_score_before = s_score;
		}
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
		CWSYNTH_Send(CWSYNTH_STATION, s_say, gEeprom.CW_KEY_WPM, 0);
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
	if (s_state == QSO_ANSWER && s_wait_10ms >= ANSWER_WAIT_10MS) {
		if (++s_calls > CALLS_MAX) {
			Notice(NOTICE_LEFT);
			s_state = QSO_IDLE;
			gUpdateDisplay = true;
		} else {
			SayStart();
			SayCall(1 + Rnd(2));
			Reply(QSO_ANSWER);
		}
		s_wait_10ms = 0;
	} else if (s_state == QSO_CONFIRM && s_wait_10ms >= CONFIRM_WAIT_10MS) {
		if (s_prompted) {
			Done(false);
		} else {
			s_prompted = true;
			SayStart();
			Say("?");
			Reply(QSO_CONFIRM);
		}
		s_wait_10ms = 0;
	} else if (s_state == QSO_DITDIT && s_wait_10ms >= DITDIT_WAIT_10MS) {
		Done(true);
	}
}

void CPO_Qso_GetStatus(char hint[CPO_QSO_LINE_SIZE], char info[CPO_QSO_LINE_SIZE])
{
	info[0] = '\0';
	if (s_speaking || s_reply_due) {
		strcpy(hint, "Copy");
	} else if (s_state == QSO_ANSWER) {
		strcpy(hint, "Answer the call");
	} else if (s_state == QSO_CONFIRM) {
		strcpy(hint, s_brief ? "TU 73" : "Confirm and 73");
	} else if (s_state == QSO_DITDIT) {
		strcpy(hint, "dit dit");
	} else {
		strcpy(hint, s_my[0] ? "Call CQ" : "CQ DE your call K");
	}
	if (s_score.worked && s_state != QSO_IDLE) {
		strcpy(info, s_call);
	} else if (s_state == QSO_IDLE && s_my[0]) {
		sprintf_(info, "Op %s", s_my);
	}
}

bool CPO_Qso_GetScore(char lines[5][CPO_QSO_LINE_SIZE])
{
	if (s_state != QSO_DONE) {
		return false;
	}
	const uint32_t secs = (s_score.end_ms - s_score.start_ms) / 1000u;

	// A brief QSO never had its name
	if (s_score.brief) {
		strcpy(lines[0], s_call);
	} else {
		sprintf_(lines[0], "%s %s", s_call, s_name);
	}
	strcpy(lines[1], s_qth);
	if (!s_score.worked) {
		strcpy(lines[2], "Call not worked");
	} else if (s_score.tries == 1) {
		strcpy(lines[2], "Call 1st try");
	} else {
		sprintf_(lines[2], "Call %u tries", s_score.tries);
	}
	if (s_score.brief) {
		sprintf_(lines[3], "Brief RST %c 73 %c", s_score.rst ? 'Y' : '-', s_score.sign ? 'Y' : '-');
	} else {
		sprintf_(lines[3], "Name %c QTH %c 73 %c", s_score.name ? 'Y' : '-',
			s_score.qth ? 'Y' : '-', s_score.sign ? 'Y' : '-');
	}
	if (s_score.finished) {
		sprintf_(lines[4], "Rpt %u EE %c %u:%02u", s_score.asked, s_score.ditdit ? 'Y' : '-',
			secs / 60u, secs % 60u);
	} else {
		sprintf_(lines[4], "Unfinished %u:%02u", secs / 60u, secs % 60u);
	}
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
	default:
		strcpy(out, "Station left");
		break;
	}
	return true;
}
