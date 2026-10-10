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

// CW transmit timeout for stuck keys and paddles

#include <stdbool.h>

#include "app/cwguard.h"
#include "settings.h"

static bool     s_keyed;          // the last action left the carrier on
static bool     s_locked;         // tripped, waiting for the key to be released
static uint32_t s_run_start_ms;   // first element after the last character break
static uint32_t s_off_since_ms;   // when the carrier last went off

CW_Action_t CW_Guard_Filter(CW_Action_t action, uint32_t now_ms, CW_GuardEvent_t *event)
{
	const bool keyed = (action == CW_ACTION_CARRIER_ON || action == CW_ACTION_CARRIER_HOLD_ON);

	// two dits of silence: longer than the gap inside a character, shorter than the
	// gap between characters, and never reached by a stuck paddle's steady 1-dit gaps
	const uint32_t break_ms = 2400u / gEeprom.CW_KEY_WPM;

	if (keyed && !s_keyed && now_ms - s_off_since_ms >= break_ms)
		s_run_start_ms = now_ms;
	if (!keyed && s_keyed)
		s_off_since_ms = now_ms;
	s_keyed = keyed;

	*event = CW_GUARD_OK;

	if (s_locked) {
		if (!keyed && now_ms - s_off_since_ms >= CW_GUARD_RELEASE_MS) {
			s_locked = false;
			*event = CW_GUARD_RELEASED;
			return action;
		}
		*event = CW_GUARD_LOCKED;
		return CW_ACTION_NONE;
	}

	if (keyed && now_ms - s_run_start_ms >= CW_GUARD_LIMIT_MS) {
		s_locked = true;
		*event = CW_GUARD_TRIPPED;
		return CW_ACTION_CARRIER_OFF;
	}

	return action;
}
