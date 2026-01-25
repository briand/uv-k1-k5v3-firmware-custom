/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "frequencies.h"
#include <helper/battery.h>
#include "radio.h"
#include <driver/backlight.h>

enum POWER_OnDisplayMode_t {
#ifdef ENABLE_FEAT_F4HWN
    POWER_ON_DISPLAY_MODE_ALL,
    POWER_ON_DISPLAY_MODE_SOUND,
#else
    POWER_ON_DISPLAY_MODE_FULL_SCREEN = 0,
#endif
    POWER_ON_DISPLAY_MODE_MESSAGE,
    POWER_ON_DISPLAY_MODE_VOLTAGE,
    POWER_ON_DISPLAY_MODE_NONE
};
typedef enum POWER_OnDisplayMode_t POWER_OnDisplayMode_t;

#ifdef ENABLE_CW_MODULATOR
// Bit flags for CW key input configuration
#define CW_KEY_FLAG_REVERSED      0x01  // 0=normal, 1=reversed
#define CW_KEY_FLAG_PORT_RING     0x02  // 0=no port ring, 1=use port ring input
#define CW_KEY_FLAG_BUTTONS       0x04  // 0=no buttons, 1=use button inputs
#define CW_KEY_FLAG_NO_KEYER      0x08  // 0=keyer enabled, 1=handkey only
#define CW_KEY_FLAG_PORT_GROUND   0x10  // 0=no port ground, 1=use port ground

enum CW_KeyInputType_t {
	CW_KEY_INPUT_HANDKEY          = 0x08,  // handkey only (disable keyer)
	CW_KEY_INPUT_HANDKEY_PORT     = 0x18,  // handkey + port ground (no keyer)
	CW_KEY_INPUT_BUTTONS_NORMAL   = 0x04,  // buttons
	CW_KEY_INPUT_BUTTONS_REVERSED = 0x05,  // buttons + reversed
	CW_KEY_INPUT_PORT_NORMAL      = 0x12,  // port ring + port ground
	CW_KEY_INPUT_PORT_REVERSED    = 0x13,  // port ring + port ground + reversed
	CW_KEY_INPUT_BOTH_NORMAL      = 0x16,  // buttons + port ring + port ground
	CW_KEY_INPUT_BOTH_REVERSED    = 0x17   // buttons + port ring + port ground + reversed
};
typedef enum CW_KeyInputType_t CW_KeyInputType_t;

enum CW_IambicMode_t {
	CW_IAMBIC_MODE_A = 0,
	CW_IAMBIC_MODE_B
};
typedef enum CW_IambicMode_t CW_IambicMode_t;
#endif

enum TxLockModes_t {
    F_LOCK_DEF, //all default frequencies + configurable
    F_LOCK_FCC,
#ifdef ENABLE_FEAT_F4HWN_CA
    F_LOCK_CA,
#endif
    F_LOCK_CE,
    F_LOCK_GB,
    F_LOCK_430,
    F_LOCK_438,
#ifdef ENABLE_FEAT_F4HWN_PMR
    F_LOCK_PMR,
#endif
#ifdef ENABLE_FEAT_F4HWN_GMRS_FRS_MURS
    F_LOCK_GMRS_FRS_MURS,
#endif
    F_LOCK_ALL, // disable TX on all frequencies
    F_LOCK_NONE, // enable TX on all frequencies
    F_LOCK_LEN
};

/*
enum {
    SCAN_RESUME_TO = 0,
    SCAN_RESUME_CO,
    SCAN_RESUME_SE
};
*/

enum {
    CROSS_BAND_OFF = 0,
    CROSS_BAND_CHAN_A,
    CROSS_BAND_CHAN_B
};

enum {
    DUAL_WATCH_OFF = 0,
    DUAL_WATCH_CHAN_A,
    DUAL_WATCH_CHAN_B
};

enum {
    TX_OFFSET_FREQUENCY_DIRECTION_OFF = 0,
    TX_OFFSET_FREQUENCY_DIRECTION_ADD,
    TX_OFFSET_FREQUENCY_DIRECTION_SUB
};

enum {
    OUTPUT_POWER_USER = 0,
    OUTPUT_POWER_LOW1,
    OUTPUT_POWER_LOW2,
    OUTPUT_POWER_LOW3,
    OUTPUT_POWER_LOW4,
    OUTPUT_POWER_LOW5,
    OUTPUT_POWER_MID,
    OUTPUT_POWER_HIGH
};

enum ACTION_OPT_t {
    ACTION_OPT_NONE = 0,
    ACTION_OPT_FLASHLIGHT,
    ACTION_OPT_POWER,
    ACTION_OPT_MONITOR,
    ACTION_OPT_SCAN,
    ACTION_OPT_VOX,
    ACTION_OPT_ALARM,
    ACTION_OPT_FM,
    ACTION_OPT_1750,
    ACTION_OPT_KEYLOCK,
    ACTION_OPT_A_B,
    ACTION_OPT_VFO_MR,
    ACTION_OPT_SWITCH_DEMODUL,
    ACTION_OPT_BLMIN_TMP_OFF, //BackLight Minimum Temporay OFF
#ifdef ENABLE_FEAT_F4HWN
    ACTION_OPT_RXMODE,
    ACTION_OPT_MAINONLY,
    ACTION_OPT_PTT,
    ACTION_OPT_WN,
    ACTION_OPT_BACKLIGHT,
    ACTION_OPT_MUTE,
    ACTION_OPT_RXA,
    #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
        ACTION_OPT_POWER_HIGH,
        ACTION_OPT_REMOVE_OFFSET,
    #endif
#endif
#ifdef ENABLE_REGA
    ACTION_OPT_REGA_ALARM,
    ACTION_OPT_REGA_TEST,
#endif
    ACTION_OPT_LEN
};

#ifdef ENABLE_VOICE
    enum VOICE_Prompt_t
    {
        VOICE_PROMPT_OFF = 0,
        VOICE_PROMPT_CHINESE,
        VOICE_PROMPT_ENGLISH
    };
    typedef enum VOICE_Prompt_t VOICE_Prompt_t;
#endif

enum ALARM_Mode_t {
    ALARM_MODE_SITE = 0,
    ALARM_MODE_TONE
};
typedef enum ALARM_Mode_t ALARM_Mode_t;

enum ROGER_Mode_t {
    ROGER_MODE_OFF = 0,
    ROGER_MODE_ROGER,
    ROGER_MODE_MDC
};
typedef enum ROGER_Mode_t ROGER_Mode_t;

enum CHANNEL_DisplayMode_t {
    MDF_FREQUENCY = 0,
    MDF_CHANNEL,
    MDF_NAME,
    MDF_NAME_FREQ
};
typedef enum CHANNEL_DisplayMode_t CHANNEL_DisplayMode_t;

typedef struct {
    uint16_t               ScreenChannel[2]; // current channels set in the radio (memory or frequency channels)
    uint16_t               FreqChannel[2]; // last frequency channels used
    uint16_t               MrChannel[2]; // last memory channels used
#ifdef ENABLE_NOAA
    uint16_t           NoaaChannel[2];
#endif

    // The actual VFO index (0-upper/1-lower) that is now used for RX, 
    // It is being alternated by dual watch, and flipped by crossband
    uint8_t               RX_VFO;

    // The main VFO index (0-upper/1-lower) selected by the user
    // 
    uint8_t               TX_VFO;

    uint8_t               field7_0xa;
    uint8_t               field8_0xb;

#ifdef ENABLE_FMRADIO
    uint16_t          FM_SelectedFrequency;
    uint8_t           FM_SelectedChannel;
    bool              FM_IsMrMode;
    uint16_t          FM_FrequencyPlaying;
    uint8_t           FM_Band  : 2;
    //uint8_t         FM_Space : 2;
#endif

    uint8_t               SQUELCH_LEVEL;
    uint8_t               TX_TIMEOUT_TIMER;
    bool                  KEY_LOCK;
#ifdef ENABLE_FEAT_F4HWN
    bool                  KEY_LOCK_PTT;
    bool                  SET_NAV;
#endif
#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    bool                  MENU_LOCK;
    uint8_t               SET_KEY;
#endif
    bool                  VOX_SWITCH;
    uint8_t               VOX_LEVEL;
#ifdef ENABLE_VOICE
    VOICE_Prompt_t    VOICE_PROMPT;
#endif
    bool                  BEEP_CONTROL;
    uint8_t               CHANNEL_DISPLAY_MODE;
    bool                  TAIL_TONE_ELIMINATION;
    bool                  VFO_OPEN;
    uint8_t               DUAL_WATCH;
    uint8_t               CROSS_BAND_RX_TX;
    uint8_t               BATTERY_SAVE;
    uint8_t               BACKLIGHT_TIME;
    uint8_t               SCAN_RESUME_MODE;
    uint8_t               SCAN_LIST_DEFAULT;
    bool                  SCAN_LIST_ENABLED;
    uint16_t              SCANLIST_PRIORITY_CH[6];
//#ifdef ENABLE_FEAT_F4HWN_RESUME_STATE // Fix me !!! What the hell is this?
    uint8_t               CURRENT_STATE;
    uint8_t               CURRENT_LIST;
//#endif                                // Fix me !!! What the hell is this?

    uint8_t               field29_0x26;
    uint8_t               field30_0x27;
    
    uint8_t               field37_0x32;
    uint8_t               field38_0x33;

    uint8_t               AUTO_KEYPAD_LOCK;
#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
    ALARM_Mode_t      ALARM_MODE;
#endif
    POWER_OnDisplayMode_t POWER_ON_DISPLAY_MODE;
    ROGER_Mode_t          ROGER;
    uint8_t               REPEATER_TAIL_TONE_ELIMINATION;
    uint8_t               KEY_1_SHORT_PRESS_ACTION;
    uint8_t               KEY_1_LONG_PRESS_ACTION;
    uint8_t               KEY_2_SHORT_PRESS_ACTION;
    uint8_t               KEY_2_LONG_PRESS_ACTION;
    uint8_t               MIC_SENSITIVITY;
    uint8_t               MIC_SENSITIVITY_TUNING;
    uint16_t              CHAN_1_CALL;
#ifdef ENABLE_DTMF_CALLING
    char                  ANI_DTMF_ID[8];
    char                  KILL_CODE[8];
    char                  REVIVE_CODE[8];
#endif
    char                  DTMF_UP_CODE[16];

    uint8_t               field57_0x6c;
    uint8_t               field58_0x6d;

    char                  DTMF_DOWN_CODE[16];

    uint8_t               field60_0x7e;
    uint8_t               field61_0x7f;

#ifdef ENABLE_DTMF_CALLING
    char                  DTMF_SEPARATE_CODE;
    char                  DTMF_GROUP_CALL_CODE;
    uint8_t               DTMF_DECODE_RESPONSE;
    uint8_t               DTMF_auto_reset_time;
#endif  
    uint16_t              DTMF_PRELOAD_TIME;
    uint16_t              DTMF_FIRST_CODE_PERSIST_TIME;
    uint16_t              DTMF_HASH_CODE_PERSIST_TIME;
    uint16_t              DTMF_CODE_PERSIST_TIME;
    uint16_t              DTMF_CODE_INTERVAL_TIME;
    bool                  DTMF_SIDE_TONE;
#ifdef ENABLE_DTMF_CALLING
    bool                  PERMIT_REMOTE_KILL;
#endif
    int16_t               BK4819_XTAL_FREQ_LOW;
#ifdef ENABLE_NOAA
    bool              NOAA_AUTO_SCAN;
#endif
    uint8_t               VOLUME_GAIN;
    #ifdef ENABLE_FEAT_F4HWN
        uint8_t           VOLUME_GAIN_BACKUP;
    #endif
    uint8_t               DAC_GAIN;

    VFO_Info_t            VfoInfo[2];
    uint32_t              POWER_ON_PASSWORD;
    uint16_t              VOX1_THRESHOLD;
    uint16_t              VOX0_THRESHOLD;

    uint8_t               field77_0x95;
    uint8_t               field78_0x96;
    uint8_t               field79_0x97;

    uint8_t               KEY_M_LONG_PRESS_ACTION;
    uint8_t               BACKLIGHT_MIN;
#ifdef ENABLE_BLMIN_TMP_OFF
    BLMIN_STAT_t          BACKLIGHT_MIN_STAT;
#endif
    uint8_t               BACKLIGHT_MAX;
    BATTERY_Type_t        BATTERY_TYPE;
#ifdef ENABLE_RSSI_BAR
    uint8_t               S0_LEVEL;
    uint8_t               S9_LEVEL;
#endif
#ifdef ENABLE_CW_MODULATOR
	uint8_t			  	  CW_TONE_FREQUENCY; 	// in 50 Hz steps, 0 = 450Hz, 1 = 500Hz, 2 = 550Hz, etc
	uint8_t               CW_SIDETONE_LEVEL;	// CW sidetone level: 0=off, 1-6 scaled volume levels
	CW_IambicMode_t       CW_KEYER_MODE;		// Iambic A or B (keyer disabled when CW_KEY_INPUT == HANDKEY)
	uint8_t               CW_KEY_WPM;			// actual WPM
	CW_KeyInputType_t     CW_KEY_INPUT;			// Combined button/port input selection
#endif

} EEPROM_Config_t;

extern EEPROM_Config_t gEeprom;

void     SETTINGS_InitEEPROM(void);
void     SETTINGS_LoadCalibration(void);
uint32_t SETTINGS_FetchChannelFrequency(const uint16_t channel);
void     SETTINGS_FetchChannelName(char *s, const uint16_t channel);
void     SETTINGS_FactoryReset(bool bIsAll);
#ifdef ENABLE_FMRADIO
    void SETTINGS_SaveFM(void);
#endif
void SETTINGS_SaveVfoIndices(void);
void SETTINGS_SaveVfoIndicesFlush(void);
void SETTINGS_SaveSettings(void);
void SETTINGS_SaveChannelName(uint16_t channel, const char * name);
void SETTINGS_SaveChannel(uint16_t Channel, uint8_t VFO, const VFO_Info_t *pVFO, uint8_t Mode);
void SETTINGS_SaveBatteryCalibration(const uint16_t * batteryCalibration);
void SETTINGS_UpdateChannel(uint16_t channel, const VFO_Info_t *pVFO, bool keep, bool check, bool save);
void SETTINGS_WriteBuildOptions(void);
#ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
    void SETTINGS_WriteCurrentState(void);
#endif
#ifdef ENABLE_FEAT_F4HWN_VOL
    void SETTINGS_WriteCurrentVol(void);
#endif
#ifdef ENABLE_FEAT_F4HWN
    void SETTINGS_ResetTxLock(void);
#endif
#endif
