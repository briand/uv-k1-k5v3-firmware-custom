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

// One-time migration of CW storage from the pre-v2.0 locations:
//   macros       0x1C00 (over MR channel records 448-459) -> 0xD010 (flash 0x00B010)
//   key actions  build-dependent numbers -> fixed ACTION_OPT_t IDs (F4HWN v6.2)
//   CW settings  0xA140 (second half of the AES key)      -> 0xA208
//   EEPROM schema 0xA170                                   -> 0xA204
//
// TEMPORARY: meant to ship for one or two releases after v2.0. After a boot of
// v2.0 every radio has both headers ('CWX' at 0xA200, 'CWM' at 0xD000) and
// schema 2, so removing this leaves a plain "write a header if missing". To
// remove it:
//   - delete this file and cwmigrate.c, drop cwmigrate.c from CMakeLists.txt
//   - SETTINGS_InitCwBlock(): with no header, write version 1 and schema
//     EEPROM_SCHEMA_CURRENT; with one, do nothing (drop the
//     SETTINGS_UpgradeSchema call)
//   - delete SETTINGS_UpgradeSchema, SETTINGS_MigrateFilterSchema and
//     MigrateFlagsRegion (settings.c), and EEPROM_SCHEMA_LEGACY_ADDR,
//     CW_SETTINGS_LEGACY_ADDR and CW_MACRO_LEGACY_ADDR
//   - CHIRP: _nr7y_convert_legacy_layout (which also renumbers key actions)
//     and _nr7y_upgrade_schema can go once pre-v2.0 images no longer need to
//     load
// A radio that skips every release carrying this loses its macros and CW
// settings, its key binds point at the wrong actions, and a pre-v1.4 radio
// also misses the schema 2 filter migration.

#ifndef APP_CWMIGRATE_H
#define APP_CWMIGRATE_H

#include <stdbool.h>
#include <stdint.h>

// All three are called while the CW block header is missing, in this order,
// and the caller writes the header right after the last.

// Copies macros and CW settings to their new homes without overwriting
// anything already there, then deletes the channels that held valid legacy
// macros and clears the old macro and CW settings bytes. Returns false,
// having cleared nothing, if the copied macros did not read back correctly.
bool CW_MigrateLegacyStorage(void);

// Renumbers the stored key actions to the fixed ACTION_OPT_t IDs, unless the
// data came from armel firmware. Not idempotent: runs once, before the header.
void CW_MigrateLegacyActions(void);

// The schema byte left at 0xA170 by v1.4-v1.x, as is (0xFF before v1.4).
// Read, never written.
uint8_t CW_MigrateLegacySchema(void);

#endif
