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
	bool     key_down[KEY_QUEUE];
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
} CWSYNTH_Voice_t;

static CWSYNTH_Voice_t s_voices[CWSYNTH_VOICES];
static int32_t s_acc[DACAUDIO_HALF_LEN];   // the voices' sum for one fill, before scaling
static volatile uint16_t s_bias;           // DC offset, ramping to mid-scale and back
static volatile bool s_stopping = false;

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

// A sending voice's element or gap has run out: on to the next
static void NextStep(CWSYNTH_Voice_t *v)
{
	if (v->key) {
		// a dit's gap after an element, or a character's after its last
		v->key = false;
		v->left = v->elements ? v->unit : v->char_gap;
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
			v->left = v->word_gap - v->char_gap;
			return;
		}
		if (!CW_GetMorseForChar(c, &v->pattern, &v->elements)) {
			v->elements = 0;
		}
	}
	v->key = true;
	v->left = (v->pattern & 1) ? 3 * v->unit : v->unit;
	v->pattern >>= 1;
	v->elements--;
}

static void RenderVoice(CWSYNTH_Voice_t *v, uint32_t time)
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
				v->key = v->key_down[t];   // due now, or late
				v->key_tail = (t + 1) & (KEY_QUEUE - 1);
			}
		}

		if (v->key ? v->env < ENV_STEPS : v->env > 0) {
			// an edge: one step of the ramp
			if (count > ENV_CHUNK) {
				count = ENV_CHUNK;
			}
			v->env = v->key ? v->env + 1 : v->env - 1;
			const int32_t gain = ((uint32_t)v->gain * EDGE[v->env]) >> 12;
			if (gain) {
				Held(s_acc + i, count, &v->phase, v->inc, gain);
			}
		} else if (v->key) {
			Held(s_acc + i, count, &v->phase, v->inc, v->gain);
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

	memset(s_acc, 0, sizeof(s_acc));
	for (CWSYNTH_Voice_t *v = s_voices; v < s_voices + CWSYNTH_VOICES; v++) {
		if (stopping) {
			v->key = false;
			v->text = NULL;
			v->sending = false;
			v->key_tail = v->key_head;
		}
		RenderVoice(v, time);
		if (v->env) {
			sounding = true;
		}
	}

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

void CWSYNTH_SetVoice(uint8_t voice, uint16_t pitch_hz, uint16_t gain)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];

	v->inc = pitch_hz * PHASE_PER_HZ;
	v->gain = gain;
}

void CWSYNTH_Key(uint8_t voice, bool down)
{
	CWSYNTH_Voice_t *v = &s_voices[voice];
	const uint8_t head = v->key_head;
	const uint8_t next = (head + 1) & (KEY_QUEUE - 1);

	// Full only if keying outran the 5 ms delay several times over; drop it then
	if (!DACAUDIO_IsRunning() || next == v->key_tail) {
		return;
	}
	v->key_time[head] = DACAUDIO_EventTime();
	v->key_down[head] = down;
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
