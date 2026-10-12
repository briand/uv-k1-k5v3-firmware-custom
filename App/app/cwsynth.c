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

// CW tone synthesizer on the MCU DAC

#include <string.h>

#include "app/cwmacro.h"
#include "app/cwsynth.h"
#include "driver/dacaudio.h"

#define RATE_HZ        32000u
#define PHASE_PER_HZ   134218u               // 2^32 / RATE_HZ
#define MINUTE_SAMPLES (RATE_HZ * 60u)
#define DIT_SAMPLES    (RATE_HZ * 6u / 5u)   // 1.2 s, which over the WPM is a dit
#define ENV_STEPS      40                    // steps in a keying edge
#define ENV_CHUNK      4                     // samples per step, so an edge is 160: 5 ms
#define KEY_QUEUE      4                     // key changes waiting to play, a power of two
#define CHIRP_CHUNKS   64                    // a chirp settles over 64 steps of 4 samples, 8 ms
#define RX_FULL        4096                  // receive voices at full level...
#define RX_STEP        26                    // ...faded in or out over 158 samples, 5 ms
#define KEY_DOWN       0x01                  // a key change keys the voice down...
#define KEY_TX         0x02                  // ...and is the operator transmitting

// One cycle, 64 points, +-2047. No interpolation: at 32 kHz it sounded the same.
static const int16_t SINE[64] = {
	    0,   201,   399,   594,   783,   965,  1137,  1299,
	 1447,  1582,  1702,  1805,  1891,  1959,  2008,  2037,
	 2047,  2037,  2008,  1959,  1891,  1805,  1702,  1582,
	 1447,  1299,  1137,   965,   783,   594,   399,   201,
	    0,  -201,  -399,  -594,  -783,  -965, -1137, -1299,
	-1447, -1582, -1702, -1805, -1891, -1959, -2008, -2037,
	-2047, -2037, -2008, -1959, -1891, -1805, -1702, -1582,
	-1447, -1299, -1137,  -965,  -783,  -594,  -399,  -201,
};

// SINE copied to RAM at start, so the RAM loop's table reads skip flash wait states
static int16_t s_sine_ram[64];

// How each fist times dits, dahs, the gaps between elements and the gaps between
// characters and words: a per cent of the ideal length, and how far it wanders either
// way. A keyer's weighting trades element for gap, heavy or light; a paddle sent with
// a keyer's elements can still space its letters by hand. A bug's dits come off the
// vibroplex short and even while its dahs are made by hand and long; a straight key
// wanders everywhere and leaves room between letters; a cootie's contacts alternate,
// one side a little heavier (in Shape).
typedef struct {
	uint8_t base;
	uint8_t wander;
} CWSYNTH_Shape_t;

static const CWSYNTH_Shape_t FIST_SHAPE[CWSYNTH_FISTS][4] = {
	{ { 100,  0 }, { 100,  0 }, { 100,  0 }, { 100,  0 } },   // paddle
	{ { 115,  0 }, { 105,  0 }, {  85,  0 }, { 100,  0 } },   // weighted heavy
	{ {  88,  0 }, {  96,  0 }, { 112,  0 }, { 100,  0 } },   // weighted light
	{ { 100,  0 }, { 100,  0 }, { 100,  0 }, { 115, 35 } },   // ragged letter spacing
	{ {  90,  2 }, { 125, 12 }, { 105,  5 }, { 100, 15 } },   // bug
	{ { 105, 15 }, { 113, 12 }, { 100, 20 }, { 120, 20 } },   // straight key
	{ { 100,  8 }, {  97,  8 }, {  85, 10 }, { 100, 15 } },   // cootie
};

enum { SHAPE_DIT = 0, SHAPE_DAH, SHAPE_GAP, SHAPE_CHAR_GAP };

static uint32_t s_rng = 0x2545F491;   // the fill's own: xorshift32

// Keying edge, a raised cosine out of 4096: (1 - cos(pi * step / ENV_STEPS)) / 2
static const uint16_t EDGE[ENV_STEPS + 1] = {
	   0,    6,   25,   57,  100,  156,  223,  302,  391,
	 491,  600,  718,  844,  978, 1118, 1264, 1415, 1570,
	1728, 1887, 2048, 2209, 2368, 2526, 2681, 2832, 2978,
	3118, 3252, 3378, 3496, 3605, 3705, 3794, 3873, 3940,
	3996, 4039, 4071, 4090, 4096,
};

typedef struct {
	uint32_t phase;
	uint32_t inc;                  // phase step per sample
	uint16_t gain;                 // full level, out of 4096
	uint8_t  env;                  // EDGE step, 0 shut to ENV_STEPS open
	bool     key;

	// Key changes from the main loop, with the sample each plays at
	uint32_t key_time[KEY_QUEUE];
	uint8_t  key_flags[KEY_QUEUE];   // KEY_DOWN, KEY_TX
	volatile uint8_t key_head;     // the main loop's
	uint8_t  key_tail;             // the fill's

	// Sending text
	const char *text;              // the next character, or NULL when not sending
	uint32_t left;                 // samples left in the element or gap
	uint32_t unit;                 // a dit, in samples
	uint32_t char_gap;             // samples between characters
	uint32_t word_gap;             // samples between words
	uint8_t  pattern;              // the rest of the character: LSB first, 1 = dah
	uint8_t  elements;             // elements left in it
	volatile bool sending;

	// How it sends
	uint8_t  fist;                 // CWSYNTH_Fist_t
	bool     side;                 // a cootie's contact for the next element
	uint8_t  chirp_left;           // steps of its chirp still settling
	uint32_t chirp_inc;            // the phase step its chirp starts off by
} CWSYNTH_Voice_t;

static CWSYNTH_Voice_t s_voices[CWSYNTH_VOICES];
static int32_t s_acc[DACAUDIO_HALF_LEN];   // the voices' sum for one fill, before scaling
static volatile uint16_t s_bias;           // DC offset, ramping to mid-scale and back
static volatile bool s_stopping = false;

// Break-in: the operator's receiver, every voice but the sidetone, falls silent from
// key-down until the hang time after key-up, as a transceiver's does
static volatile uint32_t s_hang;           // samples
static volatile bool s_tx_keyed;           // the operator's key is down
static volatile uint32_t s_rx_open_at;     // when the receiver comes back after it lets go
static volatile uint32_t s_rendered;       // the time of the last fill
static uint16_t s_rx_level = RX_FULL;

// A voice at a steady level: the loop the CPU budget rests on. It sits in a .data
// section, which startup copies to RAM with the rest of .data, so its fetches skip
// flash wait states; long_call because RAM is out of a BL's reach from flash, and -O2
// because -Os spills the table and gain to the stack every sample. A stretch is
// never empty.
__attribute__((section(".data.cwsynth_held"), noinline, long_call, optimize("O2")))
static void Held(int32_t *acc, uint32_t count, uint32_t *phase, uint32_t inc, int32_t gain)
{
	const int16_t *const table = s_sine_ram;
	uint32_t ph = *phase;

	do {
		*acc++ += table[ph >> 26] * gain;
		ph += inc;
	} while (--count);
	*phase = ph;
}

static uint32_t Rand(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

// An element or gap the length a voice's fist makes it
static uint32_t Shape(CWSYNTH_Voice_t *v, uint32_t ideal, uint8_t kind)
{
	const CWSYNTH_Shape_t *shape = &FIST_SHAPE[v->fist][kind];
	int32_t pct = shape->base;

	if (shape->wander) {
		pct += (int32_t)(Rand() % (2u * shape->wander + 1u)) - shape->wander;
	}
	if (v->fist == CWSYNTH_FIST_COOTIE && kind <= SHAPE_DAH) {
		pct += v->side ? 10 : -10;
		v->side = !v->side;
	}
	return ideal * (uint32_t)pct / 100u;
}

// A sending voice's element or gap has run out: on to the next
static void NextStep(CWSYNTH_Voice_t *v)
{
	if (v->key) {
		// a dit's gap after an element, or a character's after its last
		v->key = false;
		v->left = v->elements ? Shape(v, v->unit, SHAPE_GAP) : Shape(v, v->char_gap, SHAPE_CHAR_GAP);
		return;
	}
	while (!v->elements) {
		const char c = *v->text;
		if (c == '\0') {
			v->text = NULL;
			v->sending = false;
			return;
		}
		v->text++;
		if (c == ' ') {
			// a character gap has already passed
			v->left = Shape(v, v->word_gap - v->char_gap, SHAPE_CHAR_GAP);
			return;
		}
		if (!CW_GetMorseForChar(c, &v->pattern, &v->elements)) {
			v->elements = 0;
		}
	}
	v->key = true;
	v->left = (v->pattern & 1) ? Shape(v, 3 * v->unit, SHAPE_DAH) : Shape(v, v->unit, SHAPE_DIT);
	v->chirp_left = v->chirp_inc ? CHIRP_CHUNKS : 0;
	v->pattern >>= 1;
	v->elements--;
}

// Renders a voice into the fill; when it can't be heard it keeps its timing but
// adds nothing
static void RenderVoice(CWSYNTH_Voice_t *v, uint32_t time, bool audible)
{
	uint32_t i = 0;

	while (i < DACAUDIO_HALF_LEN) {
		// a stretch runs to the voice's next key change or the end of the fill
		uint32_t count = DACAUDIO_HALF_LEN - i;
		if (v->text) {
			if (v->left < count) {
				count = v->left;
			}
		} else {
			while (v->key_tail != v->key_head) {
				const uint8_t t = v->key_tail;
				const int32_t wait = (int32_t)(v->key_time[t] - (time + i));
				if (wait > 0) {
					if ((uint32_t)wait < count) {
						count = wait;
					}
					break;
				}
				// due now, or late
				const uint8_t flags = v->key_flags[t];
				v->key = flags & KEY_DOWN;
				if (flags & KEY_TX) {
					s_tx_keyed = v->key;
					if (!v->key) {
						s_rx_open_at = time + i + s_hang;
					}
				}
				v->key_tail = (t + 1) & (KEY_QUEUE - 1);
			}
		}

		// a chirp: the pitch starts off and settles, a step every 4 samples
		uint32_t inc = v->inc;
		if (v->chirp_left && v->key) {
			if (count > ENV_CHUNK) {
				count = ENV_CHUNK;
			}
			inc += (v->chirp_inc * v->chirp_left) / CHIRP_CHUNKS;
			v->chirp_left--;
		}

		if (v->key ? v->env < ENV_STEPS : v->env > 0) {
			// an edge: one step of the ramp
			if (count > ENV_CHUNK) {
				count = ENV_CHUNK;
			}
			v->env = v->key ? v->env + 1 : v->env - 1;
			const int32_t gain = ((uint32_t)v->gain * EDGE[v->env]) >> 12;
			if (gain && audible) {
				Held(s_acc + i, count, &v->phase, inc, gain);
			}
		} else if (v->key && audible) {
			Held(s_acc + i, count, &v->phase, inc, v->gain);
		}
		// shut: nothing to add, and the phase can wander

		i += count;
		if (v->text) {
			v->left -= count;
			if (v->left == 0) {
				NextStep(v);
			}
		}
	}
}

static uint16_t Clamp(int32_t s)
{
	return (s < 0) ? 0 : (s > 4095) ? 4095 : (uint16_t)s;
}

static void Fill(uint16_t *buf, uint32_t time)
{
	const bool stopping = s_stopping;
	bool sounding = false;

	// The operator is sending: down, between elements within the hang time, or about
	// to key down in this fill
	CWSYNTH_Voice_t *const side = &s_voices[CWSYNTH_SIDETONE];
	bool tx = s_tx_keyed || (int32_t)(s_rx_open_at - time) > 0;
	for (uint8_t t = side->key_tail; t != side->key_head; t = (t + 1) & (KEY_QUEUE - 1)) {
		if ((side->key_flags[t] & (KEY_DOWN | KEY_TX)) == (KEY_DOWN | KEY_TX)
			&& (int32_t)(side->key_time[t] - (time + DACAUDIO_HALF_LEN)) < 0) {
			tx = true;
		}
	}

	memset(s_acc, 0, sizeof(s_acc));
	for (CWSYNTH_Voice_t *v = s_voices; v < s_voices + CWSYNTH_VOICES; v++) {
		if (stopping) {
			v->key = false;
			v->text = NULL;
			v->sending = false;
			v->key_tail = v->key_head;
		}
		// the receive voices first, the sidetone below
		if (v != side) {
			RenderVoice(v, time, !(tx && s_rx_level == 0));
			sounding |= v->env != 0;
		}
	}

	// The receiver fades out under the operator's sending and back in after the hang
	if (tx || s_rx_level != RX_FULL) {
		uint16_t level = s_rx_level;
		for (uint32_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
			if (tx) {
				level = (level > RX_STEP) ? level - RX_STEP : 0;
			} else {
				level = (level + RX_STEP < RX_FULL) ? level + RX_STEP : RX_FULL;
			}
			s_acc[i] = (s_acc[i] >> 12) * level;
		}
		s_rx_level = level;
	}
	RenderVoice(side, time, true);
	sounding |= side->env != 0;
	s_rendered = time;

	// The output ramps up to mid-scale at start, and down to 0 once a stop has
	// let every voice close
	uint16_t bias = s_bias;
	const bool down = stopping && !sounding;
	if (bias == DACAUDIO_MID && !down) {
		for (uint32_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
			buf[i] = Clamp(DACAUDIO_MID + (s_acc[i] >> 12));
		}
	} else {
		for (uint32_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
			if (down) {
				if (bias > 0) {
					bias--;
				}
			} else if (bias < DACAUDIO_MID) {
				bias++;
			}
			buf[i] = Clamp(bias + (s_acc[i] >> 12));
		}
		s_bias = bias;
	}
}

void CWSYNTH_Start(void)
{
	s_stopping = false;   // turns around a stop still ramping down
	if (DACAUDIO_IsRunning()) {
		return;
	}
	memcpy(s_sine_ram, SINE, sizeof(s_sine_ram));
	memset(s_voices, 0, sizeof(s_voices));
	s_bias = 0;
	s_tx_keyed = false;
	s_rx_open_at = 0;
	s_rendered = 0;
	s_rx_level = RX_FULL;
	DACAUDIO_Start(Fill, RATE_HZ);
}

void CWSYNTH_Stop(void)
{
	if (DACAUDIO_IsRunning()) {
		s_stopping = true;
	}
}

void CWSYNTH_StopNow(void)
{
	DACAUDIO_Stop();
	s_stopping = false;
}

void CWSYNTH_Tick10ms(void)
{
	if (s_stopping && s_bias == 0) {
		CWSYNTH_StopNow();
	}
}

bool CWSYNTH_IsRunning(void)
{
	return DACAUDIO_IsRunning();
}

void CWSYNTH_SetFist(uint8_t voice, CWSYNTH_Fist_t fist, uint8_t chirp_hz)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];

	DACAUDIO_Lock();
	v->fist = fist;
	v->chirp_inc = chirp_hz * PHASE_PER_HZ;
	DACAUDIO_Unlock();
}

void CWSYNTH_SetHang(uint16_t hang_ms)
{
	s_hang = hang_ms * (RATE_HZ / 1000u);
}

bool CWSYNTH_Transmitting(void)
{
	return s_tx_keyed || (int32_t)(s_rx_open_at - s_rendered) > 0;
}

void CWSYNTH_SetVoice(uint8_t voice, uint16_t pitch_hz, uint16_t gain)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];

	v->inc = pitch_hz * PHASE_PER_HZ;
	v->gain = gain;
}

void CWSYNTH_Key(uint8_t voice, bool down, bool transmit)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];
	const uint8_t head = v->key_head;
	const uint8_t next = (head + 1) & (KEY_QUEUE - 1);

	// Full only if keying outran the 5 ms delay several times over; drop it then
	if (!DACAUDIO_IsRunning() || next == v->key_tail) {
		return;
	}
	v->key_time[head] = DACAUDIO_EventTime();
	v->key_flags[head] = (down ? KEY_DOWN : 0) | (transmit ? KEY_TX : 0);
	v->key_head = next;
}

void CWSYNTH_Send(uint8_t voice, const char *text, uint8_t wpm, uint8_t eff_wpm)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];
	const uint32_t unit = DIT_SAMPLES / wpm;

	// Stopped, it would never finish
	if (!DACAUDIO_IsRunning()) {
		return;
	}
	DACAUDIO_Lock();
	v->unit = unit;
	if (eff_wpm != 0 && eff_wpm < wpm) {
		// Farnsworth (ARRL, KE3Z), as the keyer's playback does it: PARIS is 31 dits of
		// elements and element gaps plus 19 of character and word gaps, and those gaps
		// get whatever is left of a minute over the effective speed
		const uint32_t gaps = MINUTE_SAMPLES / eff_wpm - 31u * unit;
		v->char_gap = 3u * gaps / 19u;
		v->word_gap = 7u * gaps / 19u;
	} else {
		v->char_gap = 3u * unit;
		v->word_gap = 7u * unit;
	}
	v->text = text;
	v->elements = 0;
	v->key = false;
	v->left = 1;   // the first element starts with the next fill
	v->key_tail = v->key_head;
	v->sending = true;
	DACAUDIO_Unlock();
}

void CWSYNTH_Cancel(uint8_t voice)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];

	DACAUDIO_Lock();
	v->text = NULL;
	v->key = false;
	v->sending = false;
	DACAUDIO_Unlock();
}

bool CWSYNTH_IsSending(uint8_t voice)
{
	return s_voices[voice].sending;
}
