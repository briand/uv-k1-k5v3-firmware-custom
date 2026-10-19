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

// Legacy CW storage migration, see cwmigrate.h. TEMPORARY.
//
// Order (SETTINGS_InitCwBlock drives it):
//   1. macros copied, macro header written last
//   2. CW settings copied
//   3. legacy macros and settings invalidated
//   4. legacy schema read from 0xA170, filter migration if it is behind
//   5. CW block header written: the commit
// Steps 1-3 are safe to repeat: 1 and 2 skip anything already in place, so a
// rerun never replaces migrated data with the invalidated legacy copy, and 3
// only touches legacy blocks that still validate. A cut between 4 and 5 reruns
// the filter migration, the same window v1.4 had.
//
// Legacy bytes are invalidated with 0x00, not 0xFF: clearing bits never erases
// a sector, so a power cut cannot take 0xA170 (same sector as 0xA140) or the
// channels sharing 0x1000 with the macros down with it.

#include <string.h>

#include "app/cwmacro.h"
#include "app/cwmigrate.h"
#include "driver/eeprom.h"
#include "driver/py25q16.h"
#include "settings.h"

#define LEGACY_MACRO_ADDR(n)  (CW_MACRO_LEGACY_ADDR + (n) * CW_MACRO_BLOCK_SIZE)
#define MACRO_ADDR(n)         (CW_MACRO1_EEPROM_ADDR + (n) * CW_MACRO_BLOCK_SIZE)
#define CHANNELS_PER_BLOCK    (CW_MACRO_BLOCK_SIZE / 16)

// Upstream armel v6.1.0+ (not this tree) stores a scan-list mask at 0xA170 with
// this signature behind it. Guards radios flashed to v2.0 from that firmware.
static const uint8_t ARMEL_MIX_SIG[4] = {'M', 'I', 'X', 0x01};

uint8_t CW_MigrateLegacySchema(void)
{
    uint8_t legacy[8];
    PY25Q16_ReadBuffer(EEPROM_SCHEMA_LEGACY_ADDR, legacy, sizeof(legacy));

    // Came from armel: 0xA170 is not a schema, and armel's channel layout
    // already matches schema 2.
    if (memcmp(legacy + 3, ARMEL_MIX_SIG, sizeof(ARMEL_MIX_SIG)) == 0)
        return EEPROM_SCHEMA_CURRENT;

    return legacy[0];
}

static bool MigrateMacros(void)
{
    // Already migrated (header loss later, or a rerun after power loss)
    if (CW_MacroHeaderValid())
        return true;

    uint8_t block[CW_MACRO_BLOCK_SIZE];
    uint8_t check[CW_MACRO_BLOCK_SIZE];

    for (uint8_t n = 0; n < CW_MACRO_COUNT; n++)
    {
        EEPROM_ReadBuffer(LEGACY_MACRO_ADDR(n), block, sizeof(block));
        if (CW_MacroBlockLength(block) == 0)
            memset(block, 0xFF, sizeof(block));

        for (uint8_t i = 0; i < CW_MACRO_BLOCK_SIZE; i += 8)
            EEPROM_WriteBuffer(MACRO_ADDR(n) + i, block + i);

        EEPROM_ReadBuffer(MACRO_ADDR(n), check, sizeof(check));
        if (memcmp(block, check, sizeof(block)) != 0)
            return false;
    }

    // Last, so a half-done copy is never mistaken for a finished one
    CW_WriteMacroHeader();
    return CW_MacroHeaderValid();
}

static void MigrateSettings(void)
{
    uint8_t data[8];
    PY25Q16_ReadBuffer(CW_SETTINGS_ADDR, data, sizeof(data));

    for (uint8_t i = 0; i < sizeof(data); i++)
        if (data[i] != 0xFF)
            return;  // already holds settings

    PY25Q16_ReadBuffer(CW_SETTINGS_LEGACY_ADDR, data, sizeof(data));

    // All zero: invalidated by an earlier run, nothing to carry over
    uint8_t any = 0;
    for (uint8_t i = 0; i < sizeof(data); i++)
        any |= data[i];
    if (any == 0)
        return;

    // Copied as-is: the load-time validation copes with 0xFF or garbage
    PY25Q16_WriteBuffer(CW_SETTINGS_ADDR, data, sizeof(data), false);
}

static void InvalidateLegacy(void)
{
    uint8_t block[CW_MACRO_BLOCK_SIZE];

    for (uint8_t n = 0; n < CW_MACRO_COUNT; n++)
    {
        // Re-read rather than trust the copy step: it may have run on an
        // earlier boot. A block that fails validation is a real channel.
        EEPROM_ReadBuffer(LEGACY_MACRO_ADDR(n), block, sizeof(block));
        if (CW_MacroBlockLength(block) == 0)
            continue;

        // The menu's delete: empty attributes and a blank name
        const uint16_t first = LEGACY_MACRO_ADDR(n) / 16;
        for (uint16_t ch = first; ch < first + CHANNELS_PER_BLOCK; ch++)
            SETTINGS_UpdateChannel(ch, NULL, false);

        // That leaves the records alone, so clear the macro bytes too.
        // Frequency 0 also reads as an empty channel in CHIRP.
        memset(block, 0x00, sizeof(block));
        PY25Q16_WriteBuffer(LEGACY_MACRO_ADDR(n), block, sizeof(block), false);
    }

    uint8_t blank[8];
    memset(blank, 0x00, sizeof(blank));
    PY25Q16_WriteBuffer(CW_SETTINGS_LEGACY_ADDR, blank, sizeof(blank), false);
}

bool CW_MigrateLegacyStorage(void)
{
    if (!MigrateMacros())
        return false;
    MigrateSettings();
    InvalidateLegacy();
    return true;
}
