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
//   4. key actions renumbered to fixed IDs, unless the data came from armel
//   5. legacy schema read from 0xA170, filter migration if it is behind
//   6. CW block header written: the commit
// Steps 1-3 are safe to repeat: 1 and 2 skip anything already in place, so a
// rerun never replaces migrated data with the invalidated legacy copy, and 3
// only touches legacy blocks that still validate. Steps 4 and 5 are not: a cut
// before 6 reruns them, the same window v1.4 had for the filter migration.
//
// Legacy bytes are invalidated with 0x00, not 0xFF: clearing bits never erases
// a sector, so a power cut cannot take 0xA170 (same sector as 0xA140) or the
// channels sharing 0x1000 with the macros down with it.

#include <string.h>

#include "app/cwmacro.h"
#include "app/cwmigrate.h"
#include "driver/eeprom.h"
#include "driver/py25q16.h"
#include "misc.h"
#include "settings.h"

#define LEGACY_MACRO_ADDR(n)  (CW_MACRO_LEGACY_ADDR + (n) * CW_MACRO_BLOCK_SIZE)
#define MACRO_ADDR(n)         (CW_MACRO1_EEPROM_ADDR + (n) * CW_MACRO_BLOCK_SIZE)
#define CHANNELS_PER_BLOCK    (CW_MACRO_BLOCK_SIZE / 16)

// Upstream armel v6.1.0+ (not this tree) stores a scan-list mask at 0xA170 with
// this signature behind it. Guards radios flashed to v2.0 from that firmware.
static const uint8_t ARMEL_MIX_SIG[4] = {'M', 'I', 'X', 0x01};

// 0xA170 as left by the previous firmware: v1.4-v1.x schema byte, or armel's
// scan-list mask with ARMEL_MIX_SIG behind it
static bool CameFromArmel(uint8_t legacy[8])
{
    PY25Q16_ReadBuffer(EEPROM_SCHEMA_LEGACY_ADDR, legacy, 8);
    return memcmp(legacy + 3, ARMEL_MIX_SIG, sizeof(ARMEL_MIX_SIG)) == 0;
}

uint8_t CW_MigrateLegacySchema(void)
{
    uint8_t legacy[8];

    // Came from armel: 0xA170 is not a schema, and armel's channel layout
    // already matches schema 2.
    if (CameFromArmel(legacy))
        return EEPROM_SCHEMA_CURRENT;

    return legacy[0];
}

// Key actions are stored as the fixed IDs of ACTION_OPT_t since the F4HWN v6.2
// merge: upstream's at 0-24, the CW block at 32-45. v1.4 and older numbered the
// enum by build options; every NR7Y release built the CW preset with the same
// ones (F4HWN, CW and code practice on; RESCUE_OPS, BEAM, RF LOG, FOX HUNT off),
// so one table covers them all. ALARM, BLMIN TMP OFF and BACKLIGHT are gone
// upstream and become NONE. Values past the table (0xFF, the erased 7-bit M long
// press) are not actions either way and stay as they are. The CHIRP module
// carries the same table (_NR7Y_LEGACY_ACTION_IDS).
//
// NOT idempotent: new FM (6) would read as old ALARM. The CW block header is
// what guarantees it runs once.
static void MigrateActionIds(void)
{
    static const uint8_t NEW_ID[] = {
        [0]  = ACTION_OPT_NONE,           [1]  = ACTION_OPT_FLASHLIGHT,
        [2]  = ACTION_OPT_POWER,          [3]  = ACTION_OPT_MONITOR,
        [4]  = ACTION_OPT_SCAN,           [5]  = ACTION_OPT_VOX,
        [6]  = ACTION_OPT_NONE,           // ALARM
        [7]  = ACTION_OPT_FM,             [8]  = ACTION_OPT_1750,
        [9]  = ACTION_OPT_KEYLOCK,        [10] = ACTION_OPT_A_B,
        [11] = ACTION_OPT_VFO_MR,         [12] = ACTION_OPT_SWITCH_DEMODUL,
        [13] = ACTION_OPT_NONE,           // BLMIN TMP OFF
        [14] = ACTION_OPT_RXMODE,         [15] = ACTION_OPT_MAINONLY,
        [16] = ACTION_OPT_PTT,            [17] = ACTION_OPT_WN,
        [18] = ACTION_OPT_NONE,           // BACKLIGHT
        [19] = ACTION_OPT_MUTE,           [20] = ACTION_OPT_RXA,
        [21] = ACTION_OPT_PLAY_CWMSG1,    [22] = ACTION_OPT_PLAY_CWMSG2,
        [23] = ACTION_OPT_PLAY_CWMSG3,    [24] = ACTION_OPT_PLAY_CWMSG4,
        [25] = ACTION_OPT_REPEAT_CWMSG1,  [26] = ACTION_OPT_REPEAT_CWMSG2,
        [27] = ACTION_OPT_REPEAT_CWMSG3,  [28] = ACTION_OPT_REPEAT_CWMSG4,
        [29] = ACTION_OPT_CPO,            [30] = ACTION_OPT_CW_KEYER_MODE,
        [31] = ACTION_OPT_CW_PROPER_ROGER,[32] = ACTION_OPT_CW_SPEED,
        [33] = ACTION_OPT_CW_KEY_INPUT,   [34] = ACTION_OPT_CW_BREAK_IN,
    };

    // 0xA0A8: bit 0 beep, bits 1-7 M long press; then SIDE1/SIDE2 short/long
    uint8_t Data[8];
    PY25Q16_ReadBuffer(0x00A0A8, Data, sizeof(Data));

    const uint8_t mLong = Data[0] >> 1;
    if (mLong < ARRAY_SIZE(NEW_ID))
        Data[0] = (uint8_t)((NEW_ID[mLong] << 1) | (Data[0] & 0x01));
    for (uint8_t i = 1; i <= 4; i++)
        if (Data[i] < ARRAY_SIZE(NEW_ID))
            Data[i] = NEW_ID[Data[i]];

    PY25Q16_WriteBuffer(0x00A0A8, Data, sizeof(Data), false);
}

void CW_MigrateLegacyActions(void)
{
    uint8_t legacy[8];

    // armel v6.2 already stores the fixed IDs. (v6.1 numbered by build
    // options too, differently per edition; there is no telling which.)
    if (!CameFromArmel(legacy))
        MigrateActionIds();
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
            EEPROM_WriteBuffer(MACRO_ADDR(n) + i, block + i, 8);

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
