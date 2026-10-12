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

// CW tone synthesizer on the MCU DAC, which plays over the BK4819's receive audio.
// Each voice is a keyed tone at its own pitch and level, either keyed from the main
// loop (the sidetone) or sending text with its own timing (a station). Output is
// 32 kHz, the rate that keeps the DAC's sampling images from humming under the tone
// in the radio's audio stage, and keying edges are 5 ms raised-cosine ramps.

#ifndef APP_CWSYNTH_H
#define APP_CWSYNTH_H

#include <stdbool.h>
#include <stdint.h>

#define CWSYNTH_VOICES    5
#define CWSYNTH_SIDETONE  0      // the voice the operator's own keying sounds on
#define CWSYNTH_STATION   1      // the first voice that sends text

// A voice at full level, out of 4096: 3 dB under the DAC's full scale, so the
// radio's audio stage stays clear of the nonlinearity near it
#define CWSYNTH_GAIN_MAX  2900

// How a sending voice times its elements
typedef enum {
	CWSYNTH_FIST_PADDLE = 0,   // a keyer's exact timing
	CWSYNTH_FIST_BUG,          // short even dits, long hand-made dahs
	CWSYNTH_FIST_STRAIGHT,     // everything wanders, letters spaced out
	CWSYNTH_FIST_COOTIE,       // alternating contacts, one side heavier
	CWSYNTH_FISTS,
} CWSYNTH_Fist_t;

// Ramps the output up from 0 and starts the DAC; a no-op while running
void CWSYNTH_Start(void);

// Keys every voice off and ramps the output down; the tick stops the DAC once it is
// silent. Start turns a stop that is still ramping around.
void CWSYNTH_Stop(void);

// Stops the DAC at once, no ramp
void CWSYNTH_StopNow(void);

// 10ms tick: finishes a stop
void CWSYNTH_Tick10ms(void);

bool CWSYNTH_IsRunning(void);

// Pitch and full level of a voice, out of 4096; takes effect at once
void CWSYNTH_SetVoice(uint8_t voice, uint16_t pitch_hz, uint16_t gain);

// How a voice sends text: its fist, and how far its pitch chirps at each element's
// start before settling over 8 ms (0 for none). Voices start with a paddle, no chirp.
void CWSYNTH_SetFist(uint8_t voice, CWSYNTH_Fist_t fist, uint8_t chirp_hz);

// Keys a voice from the main loop. Every key change plays a fixed 5 ms later, to
// the sample, so keying keeps its timing whenever the DAC's fill runs. transmit marks
// the operator's sending on the sidetone voice, which the receiver breaks in around;
// a local cue such as the error dit leaves it false.
void CWSYNTH_Key(uint8_t voice, bool down, bool transmit);

// Break-in: every voice but the sidetone, the operator's receiver, fades out from a
// transmitting key-down until hang_ms after the key-up, as a transceiver's receiver
// does. A short hang hears the band between elements (QSK); a long one, between overs.
void CWSYNTH_SetHang(uint16_t hang_ms);

// True from a transmitting key-down until its hang time has run out
bool CWSYNTH_Transmitting(void);

// Sends text on a voice at wpm, with the gaps between characters and words stretched
// to eff_wpm (Farnsworth; 0, or at or above wpm, for none). Takes the characters the
// keyer plays, prosigns included; anything else is skipped. text must stay unchanged
// until the voice is done. Replaces whatever the voice was sending.
void CWSYNTH_Send(uint8_t voice, const char *text, uint8_t wpm, uint8_t eff_wpm);

// Stops a voice's text, letting its tone ramp down
void CWSYNTH_Cancel(uint8_t voice);

// True from CWSYNTH_Send until the voice's last character, and the character gap
// after it, have played. A send while the synthesizer is stopped is dropped.
bool CWSYNTH_IsSending(uint8_t voice);

#endif
