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

// DAC bench spike for code practice

#include <string.h>

#include "app/cpodac.h"
#include "app/cwkeyer.h"
#include "app/cwmacro.h"
#include "driver/bk4819.h"
#include "driver/dacaudio.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

// Keyer idle time before receive audio is put back after the sidetone took it
#define CPO_DAC_RX_RESUME_10MS 30
// How often the load line updates, and how long each key hint stays up
#define CPO_DAC_LOAD_10MS 100
#define CPO_DAC_HINT_10MS 200

// 32 kHz, the rate that clears the hum; no interpolation, which sounded the same
#define RATE_HZ      32000u
#define PHASE_PER_HZ 134218u   // 2^32 / RATE_HZ
#define DIT_SAMPLES  38400u    // 1.2 s at RATE_HZ, over the WPM
#define ENV_MAX      4096      // keying edge envelope full scale
#define ENV_STEP     26        // ENV_MAX in 158 samples, about 5 ms, against clicks
#define BIAS_STEP    1         // mid-scale in 2048 samples, 64 ms, so the DC step doesn't pop

// One cycle, 64 points, +-2047
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

// SINE copied to RAM for the RAM mixer, so its table reads skip flash wait states too
static int16_t s_sine_ram[64];

// DAC level, 0 to -36 dB in 3 dB steps, out of 4096
static const uint16_t LEVELS[] = {4096, 2900, 2053, 1453, 1029, 728, 516, 365, 258, 183, 130, 92, 65};

// 1/n out of 4096, sharing the level among n voices so they can't sum past it
static const uint16_t SHARE[CPO_DAC_MAX_VOICES] = {4096, 2048, 1365, 1024, 819};

// Each voice sits off the sidetone pitch like callers spread across the passband,
// the last 20 Hz off the first (a zero-beater), and starts some dits late so they
// don't key in step
static const int16_t OFFSET_HZ[CPO_DAC_MAX_VOICES] = {0, 170, -130, 310, 20};
static const uint8_t START_DITS[CPO_DAC_MAX_VOICES] = {0, 5, 11, 2, 8};

// "CQ" in a loop. . and - are elements, each followed by a dit of gap; a space adds
// two more (a character gap) and / four more after that (a word gap).
static const char PATTERN[] = "-.-. --.- /";

// Mixers 1 cycles through, to measure what the QSO simulator's pile-up can afford
//   naive     every voice, every sample: keying, envelope and oscillator in one loop
//   fast      each voice renders the stretches where its envelope holds still into
//             an accumulator in one tight loop, and skips the silent ones
//   fast-ram  the same, with that loop and the sine table in RAM
typedef enum {
	MIXER_NAIVE = 0,
	MIXER_FAST,
	MIXER_FAST_RAM,
	MIXER_COUNT,
} CPO_DacMixer_t;

static const char *const MIXER_NAMES[MIXER_COUNT] = {"naive", "fast", "fast-ram"};

typedef struct {
	uint32_t phase;
	uint32_t inc;      // phase step per sample
	uint16_t gain;     // out of 4096
	uint16_t env;      // keying edge, 0 to ENV_MAX
	uint16_t left;     // samples left in the element or gap, never 0 between fills
	uint8_t  pos;      // PATTERN index
	bool     key;
} CPO_DacVoice_t;

static CPO_DacMode_t s_mode = CPO_DAC_OFF;
static uint8_t s_level = 3;           // -9 dB
static bool s_rx_stale = false;       // the sidetone has had the AF path since receive audio was set
static uint8_t s_rx_idle_10ms = 0;
static uint8_t s_load_10ms = 0;
static uint16_t s_load_avg = 0;       // tenths of a percent of the CPU
static uint16_t s_load_peak = 0;      // tenths of a percent of one half buffer
static uint8_t s_hint = 0;            // which key hint line is up
static uint8_t s_hint_10ms = 0;

// Set here, read by Fill in the DMA interrupt
static CPO_DacVoice_t s_voices[CPO_DAC_MAX_VOICES];
static volatile uint8_t s_voice_count = 1;
static volatile uint8_t s_mixer = MIXER_NAIVE;
static volatile bool s_tone_on = false;    // false keys silently (RX only)
static volatile bool s_stopping = false;   // ramp to 0, then the tick stops the DAC
static volatile uint16_t s_unit;           // samples per dit

// Fill's own, reset before each start
static volatile uint16_t s_bias;           // DC offset, ramping to mid-scale; read by the tick
static bool s_sounding;                    // some voice's envelope was open at the end of the last fill
static int32_t s_acc[DACAUDIO_HALF_LEN];   // the voices' sum for one fill, before scaling

static void NextElement(CPO_DacVoice_t *v)
{
	const uint16_t unit = s_unit;

	if (v->key) {
		v->key = false;
		v->left = unit;
		return;
	}
	const char c = PATTERN[v->pos];
	v->pos = PATTERN[v->pos + 1] ? v->pos + 1 : 0;
	switch (c) {
	case '.':
		v->key = true;
		v->left = unit;
		break;
	case '-':
		v->key = true;
		v->left = 3 * unit;
		break;
	case ' ':
		v->left = 2 * unit;
		break;
	default:
		v->left = 4 * unit;
		break;
	}
}

// Each voice from the top of the pattern, its start delayed by its dits
static void ResetVoices(void)
{
	for (uint8_t i = 0; i < CPO_DAC_MAX_VOICES; i++) {
		CPO_DacVoice_t *v = &s_voices[i];
		v->phase = 0;
		v->env = 0;
		v->key = false;
		v->pos = 0;
		v->left = START_DITS[i] * s_unit + 1;
	}
}

static void EnvStep(CPO_DacVoice_t *v, bool on)
{
	if (on) {
		v->env = (v->env + ENV_STEP < ENV_MAX) ? v->env + ENV_STEP : ENV_MAX;
	} else {
		v->env = (v->env > ENV_STEP) ? v->env - ENV_STEP : 0;
	}
}

// ---- naive: all voices, one sample at a time ----

static int32_t NaiveSample(CPO_DacVoice_t *end, bool keyable)
{
	int32_t acc = 0;

	for (CPO_DacVoice_t *v = s_voices; v < end; v++) {
		if (--v->left == 0) {
			NextElement(v);
		}
		const bool on = v->key && keyable;
		if (on ? v->env < ENV_MAX : v->env > 0) {
			EnvStep(v, on);
		}
		if (v->env) {
			acc += SINE[v->phase >> 26] * (int32_t)(((uint32_t)v->gain * v->env) >> 12);
		}
		v->phase += v->inc;
	}
	return acc;
}

static void MixNaive(CPO_DacVoice_t *end, bool keyable)
{
	for (uint16_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
		s_acc[i] = NaiveSample(end, keyable);
	}
}

// ---- fast: one voice at a time, in stretches ----

// A voice whose envelope holds still: the loop the CPU budget rests on. A stretch is
// never empty, and a word-sized count keeps the loop test to one subtract.
static inline __attribute__((always_inline)) void HeldLoop(int32_t *acc, uint32_t count,
	uint32_t *phase, uint32_t inc, int32_t gain, const int16_t *table)
{
	uint32_t ph = *phase;

	do {
		*acc++ += table[ph >> 26] * gain;
		ph += inc;
	} while (--count);
	*phase = ph;
}

// -O2 here keeps the table and gain in registers, where -Os spills them every sample
__attribute__((optimize("O2")))
static void HeldFlash(int32_t *acc, uint32_t count, uint32_t *phase, uint32_t inc, int32_t gain)
{
	HeldLoop(acc, count, phase, inc, gain, SINE);
}

// The same loop in a .data section, which startup copies to RAM with the rest of
// .data, so its instruction fetches skip the flash wait states. long_call because
// RAM is out of a BL's reach from flash.
__attribute__((section(".data.cpodac_held"), noinline, long_call, optimize("O2")))
static void HeldRam(int32_t *acc, uint32_t count, uint32_t *phase, uint32_t inc, int32_t gain)
{
	HeldLoop(acc, count, phase, inc, gain, s_sine_ram);
}

static void MixVoice(CPO_DacVoice_t *v, bool keyable, bool ram)
{
	uint16_t i = 0;

	while (i < DACAUDIO_HALF_LEN) {
		// a stretch runs to the voice's next keying change or the end of the fill
		uint16_t count = DACAUDIO_HALF_LEN - i;
		if (v->left < count) {
			count = v->left;
		}

		const bool on = v->key && keyable;
		if (on ? v->env < ENV_MAX : v->env > 0) {
			// an edge: the envelope moves every sample, until it is fully open or shut
			uint16_t k = 0;
			while (k < count && (on ? v->env < ENV_MAX : v->env > 0)) {
				EnvStep(v, on);
				s_acc[i + k] += SINE[v->phase >> 26] * (int32_t)(((uint32_t)v->gain * v->env) >> 12);
				v->phase += v->inc;
				k++;
			}
			count = k;
		} else if (on) {
			if (ram) {
				HeldRam(s_acc + i, count, &v->phase, v->inc, v->gain);
			} else {
				HeldFlash(s_acc + i, count, &v->phase, v->inc, v->gain);
			}
		}
		// shut and silent: nothing to add, and the phase can wander

		i += count;
		v->left -= count;
		if (v->left == 0) {
			NextElement(v);
		}
	}
}

static void MixFast(CPO_DacVoice_t *end, bool keyable, bool ram)
{
	memset(s_acc, 0, sizeof(s_acc));
	for (CPO_DacVoice_t *v = s_voices; v < end; v++) {
		MixVoice(v, keyable, ram);
	}
}

static void Fill(uint16_t *buf, uint16_t n)
{
	const bool stopping = s_stopping;
	uint16_t bias = s_bias;

	(void)n;   // always DACAUDIO_HALF_LEN

	if (bias < DACAUDIO_MID && !stopping) {
		// the voices wait for the ramp up
		for (uint16_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
			if (bias < DACAUDIO_MID) {
				bias += BIAS_STEP;
			}
			buf[i] = bias;
		}
		s_bias = bias;
		return;
	}

	CPO_DacVoice_t *const end = s_voices + s_voice_count;
	const bool keyable = s_tone_on && !stopping;
	switch (s_mixer) {
	case MIXER_FAST:
		MixFast(end, keyable, false);
		break;
	case MIXER_FAST_RAM:
		MixFast(end, keyable, true);
		break;
	default:
		MixNaive(end, keyable);
		break;
	}

	// A stop keys every voice off; once they have all closed, the output ramps to 0
	const bool ramp_down = stopping && !s_sounding;
	for (uint16_t i = 0; i < DACAUDIO_HALF_LEN; i++) {
		int32_t s = s_acc[i] >> 12;
		if (s > 2047) {
			s = 2047;
		} else if (s < -2047) {
			s = -2047;
		}
		if (ramp_down && bias > 0) {
			bias -= BIAS_STEP;
		}
		buf[i] = (uint16_t)(bias + s);
	}
	s_bias = bias;

	bool sounding = false;
	for (CPO_DacVoice_t *v = s_voices; v < end; v++) {
		if (v->env) {
			sounding = true;
		}
	}
	s_sounding = sounding;
}

// Pitch, speed and level follow the settings, which practice can change
static void FollowSettings(void)
{
	const uint16_t gain = ((uint32_t)LEVELS[s_level] * SHARE[s_voice_count - 1]) >> 12;
	const int32_t tone_hz = gEeprom.CW_TONE_FREQUENCY * 10;

	for (uint8_t i = 0; i < CPO_DAC_MAX_VOICES; i++) {
		s_voices[i].inc = (uint32_t)(tone_hz + OFFSET_HZ[i]) * PHASE_PER_HZ;
		s_voices[i].gain = gain;
	}
	s_unit = DIT_SAMPLES / gEeprom.CW_KEY_WPM;
}

static void StartDac(void)
{
	s_stopping = false;   // a stop still ramping down turns around
	if (!DACAUDIO_IsRunning()) {
		memcpy(s_sine_ram, SINE, sizeof(s_sine_ram));
		FollowSettings();
		ResetVoices();
		s_bias = 0;
		s_sounding = false;
		DACAUDIO_Start(Fill, RATE_HZ);
	}
	FollowSettings();
}

// What the sidetone release does outside practice: the CW demodulator back on the
// AF output at RX's own DAC gain. The squelch stays open: practice doesn't service
// the BK4819's interrupts.
static void ApplyRx(void)
{
	RADIO_SetModulation(MODULATION_CW);
	s_rx_stale = false;
}

// Practice's idle state from CPO_Enter: the tone generator owns AF, silent
static void ApplyQuiet(void)
{
	BK4819_SetAF(BK4819_AF_ALAM);
	BK4819_SetScrambleFrequencyControlWord(0);
}

static bool HasRx(void)
{
	return s_mode == CPO_DAC_MIX || s_mode == CPO_DAC_RX;
}

void CPO_Dac_NextMode(void)
{
	s_mode = (CPO_DacMode_t)((s_mode + 1) % CPO_DAC_MODE_COUNT);
	s_tone_on = (s_mode == CPO_DAC_MIX || s_mode == CPO_DAC_ONLY);
	if (s_mode == CPO_DAC_OFF) {
		s_stopping = true;
	} else {
		StartDac();
	}
	if (HasRx()) {
		ApplyRx();
	} else {
		ApplyQuiet();
	}
	gUpdateDisplay = true;
}

void CPO_Dac_StepLevel(bool louder)
{
	if (s_mode == CPO_DAC_OFF) {
		return;
	}
	if (louder) {
		if (s_level > 0) {
			s_level--;
		}
	} else if (s_level < ARRAY_SIZE(LEVELS) - 1) {
		s_level++;
	}
	FollowSettings();
	gUpdateDisplay = true;
}

void CPO_Dac_NextMixer(void)
{
	if (s_mode == CPO_DAC_OFF) {
		return;
	}
	s_mixer = (s_mixer + 1) % MIXER_COUNT;
	gUpdateDisplay = true;
}

void CPO_Dac_NextVoices(void)
{
	if (s_mode == CPO_DAC_OFF) {
		return;
	}
	s_voice_count = (s_voice_count % CPO_DAC_MAX_VOICES) + 1;
	FollowSettings();
	gUpdateDisplay = true;
}

void CPO_Dac_Stop(void)
{
	DACAUDIO_Stop();
	s_mode = CPO_DAC_OFF;
	s_tone_on = false;
	s_stopping = false;
}

void CPO_Dac_Tick10ms(void)
{
	if (s_mode != CPO_DAC_OFF && ++s_hint_10ms >= CPO_DAC_HINT_10MS) {
		s_hint_10ms = 0;
		s_hint++;
		gUpdateDisplay = true;
	}
	if (!DACAUDIO_IsRunning()) {
		return;
	}
	if (s_stopping) {
		if (s_bias == 0) {
			DACAUDIO_Stop();
		}
		return;
	}
	FollowSettings();

	if (++s_load_10ms >= CPO_DAC_LOAD_10MS) {
		s_load_10ms = 0;
		DACAUDIO_TakeLoad(&s_load_avg, &s_load_peak);
		gUpdateDisplay = true;
	}

	// Paddle keying or the copy drill's playback puts the sidetone on AF, and
	// practice never hands it back, so put receive audio back once all is quiet
	if (HasRx()) {
		if (!CW_KeyerIsIdle() || gCW_PlaybackActive) {
			s_rx_stale = true;
			s_rx_idle_10ms = 0;
		} else if (s_rx_stale && ++s_rx_idle_10ms >= CPO_DAC_RX_RESUME_10MS) {
			ApplyRx();
		}
	}
}

bool CPO_Dac_IsActive(void)
{
	return s_mode != CPO_DAC_OFF;
}

bool CPO_Dac_GetStatus(char out[CPO_DAC_STATUS_SIZE])
{
	static const char *const names[] = {"", "DAC+RX", "RX only", "DAC only"};

	if (s_mode == CPO_DAC_OFF) {
		return false;
	}
	if (s_mode == CPO_DAC_RX) {
		strcpy(out, names[s_mode]);
	} else {
		sprintf_(out, "%s %ddB", names[s_mode], -3 * (int)s_level);
	}
	return true;
}

void CPO_Dac_GetSynth(char out[CPO_DAC_STATUS_SIZE])
{
	sprintf_(out, "32k %uv %s", s_voice_count, MIXER_NAMES[s_mixer]);
}

void CPO_Dac_GetLoad(char out[CPO_DAC_STATUS_SIZE])
{
	const uint16_t avg = (s_load_avg > 999) ? 999 : s_load_avg;
	const uint16_t peak = (s_load_peak > 9990) ? 999 : (s_load_peak + 5) / 10;
	sprintf_(out, "cpu %u.%u%% pk %u%%", avg / 10, avg % 10, peak);
}

const char *CPO_Dac_GetHint(void)
{
	static const char *const hints[] = {"9 source 3/6 level", "1 mixer  7 voices"};

	return (s_mode == CPO_DAC_OFF) ? "9 DAC test" : hints[s_hint % ARRAY_SIZE(hints)];
}
