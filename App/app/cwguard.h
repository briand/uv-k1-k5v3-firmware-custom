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

// CW transmit timeout. Normal sending always pauses between characters, so keying
// that runs CW_GUARD_LIMIT_MS without a break of two dits or more is a stuck key or
// paddle (a shorted dit paddle keys endless dits, which a plain key-down timer misses).

#ifndef APP_CWGUARD_H
#define APP_CWGUARD_H

#include <stdint.h>

#include "app/cwkeyer.h"

#define CW_GUARD_LIMIT_MS    7000   // keying without a character break before TX is dropped
#define CW_GUARD_RELEASE_MS  1000   // key must stay up this long to lift the lockout

typedef enum {
	CW_GUARD_OK = 0,
	CW_GUARD_TRIPPED,   // this action tripped the timeout: drop TX now
	CW_GUARD_LOCKED,    // still locked out, waiting for the key to be released
	CW_GUARD_RELEASED,  // lockout lifted on this action
} CW_GuardEvent_t;

// Feed every keyer/playback action. Returns the action to apply: unchanged normally,
// CW_ACTION_CARRIER_OFF on the tripping call, CW_ACTION_NONE while locked out.
CW_Action_t CW_Guard_Filter(CW_Action_t action, uint32_t now_ms, CW_GuardEvent_t *event);

#endif
