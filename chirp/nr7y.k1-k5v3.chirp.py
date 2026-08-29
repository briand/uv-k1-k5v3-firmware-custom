# Quansheng UV-K5 driver (c) 2023 Jacek Lipkowski <sq5bpf@lipkowski.org>
# Adapted For UV-K5 EGZUMER custom software By EGZUMER, JOC2
# Re-Adapted For UV-K5 EGZUMER/F4HWN custom software By JOC2
# Re-Adapted For UV-K1 & UV-K5 V3 F4HWN custom software By F4HWN
#
#
# based on template.py Copyright 2012 Dan Smith <dsmith@danplanet.com>
#
#
# This is a preliminary version of a driver for the UV-K5
# It is based on my reverse engineering effort described here:
# https://github.com/sq5bpf/uvk5-reverse-engineering
#
# Warning: this driver is experimental, it may brick your radio,
# eat your lunch and mess up your configuration.
#
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import webbrowser
import os

import struct
import logging
import wx


from chirp import chirp_common, directory, bitwise, memmap, errors, util
from chirp.settings import RadioSetting, RadioSettingGroup, \
    RadioSettingValueBoolean, RadioSettingValueList, \
    RadioSettingValueInteger, RadioSettingValueString, \
    RadioSettings, InvalidValueError

LOG = logging.getLogger(__name__)

# Show the obfuscated version of commands. Not needed normally, but
# might be useful for someone who is debugging a similar radio
DEBUG_SHOW_OBFUSCATED_COMMANDS = False

# Show the memory being written/received. Not needed normally, because
# this is the same information as in the packet hexdumps, but
# might be useful for someone debugging some obscure memory issue
DEBUG_SHOW_MEMORY_ACTIONS = False

# TODO: remove the driver version when it's in mainline chirp 
# The @NR7Y_VERSION@ token is replaced with the firmware release tag by the
# release workflow (.github/workflows/main.yml) so the module version always
# matches the firmware it ships with. In an unreleased working copy it stays
# as the literal placeholder.
DRIVER_VERSION = "Quansheng UV-K1 / UV-K5 V3 (F4HWN + NR7Y CW) driver ver: @NR7Y_VERSION@"
FIRMWARE_VERSION_UPDATE = "https://github.com/armel/uv-k1-k5v3-firmware-custom/releases"
CHIRP_DRIVER_VERSION_UPDATE = "https://github.com/armel/uv-k1-k5v3-firmware-custom/releases"

CHAINE_F4HWN = "https://www.youtube.com/@f4hwn" 
 
VALEUR_COMPILER = "ENABLE"

MEM_FORMAT = """
// --------------------

#seekto 0x000000;
struct {
  ul32 freq;
  ul32 offset;

// 0x08
  u8 rxcode;
  u8 txcode;

// 0x0A
  u8 txcodeflag:4,
  rxcodeflag:4;

// 0x0B
  u8 modulation:4,
  offsetDir:4;

// 0x0C
  u8 __UNUSED01:1,
  txLock:1,
  busyChLockout:1,
  txpower:3,
  bandwidth:1,
  freq_reverse:1;

  // 0x0D
  u8 __UNUSED02:4,
  dtmf_pttid:3,
  dtmf_decode:1;

  // 0x0E
  u8 step;
  u8 __UNUSED03;

} channel[1024];

// --------------------

#seekto 0x004000;
struct {
char name[16];
} channelname[1024];


// --------------------

#seekto 0x008000;
struct {
  u8 __UNUSED04:3,
     compander:2,
     band:3;
  u8 scanlist;
} ch_attr[1031];

// --------------------

#seekto 0x00880E;
struct {
    char name[4];
} listname[24];

// --------------------

#seekto 0x009000;
struct {
  ul32 freq;
  ul32 offset;

// 0x08
  u8 rxcode;
  u8 txcode;

// 0x0A
  u8 txcodeflag:4,
  rxcodeflag:4;

// 0x0B
  u8 modulation:4,
  offsetDir:4;

// 0x0C
  u8 __UNUSED05:1,
  txLock:1,
  busyChLockout:1,
  txpower:3,
  bandwidth:1,
  freq_reverse:1;

  // 0x0D
  u8 __UNUSED06:4,
  dtmf_pttid:3,
  dtmf_decode:1;

  // 0x0E
  u8 step;
  u8 __UNUSED07;

} vfo_channel[14];

// --------------------

#seekto 0x00A000;
u8 set_rxa_am:4,
   set_rxa_fm:4;
u8 squelch;
u8 max_talk_time;
u8 noaa_autoscan;
u8 __UNUSED09:1,
   set_nav:1,
   set_key:4,
   set_menu_lock:1,
   key_lock:1;
u8 vox_switch;
u8 vox_level;
u8 mic_gain;

// --------------------

#seekto 0x00A008;
u8 backlight_min:4,
   backlight_max:4;

u8 channel_display_mode;
u8 crossband;
u8 battery_save;
u8 dual_watch;
u8 backlight_time;
u8 __UNUSED10:6,
   set_nfm:1,
   ste:1;
u8 current_state;

// --------------------

#seekto 0x00A010;
ul16 ScreenChannel_A;
ul16 MrChannel_A;
ul16 FreqChannel_A;
ul16 ScreenChannel_B;
ul16 MrChannel_B;
ul16 FreqChannel_B;
ul16 NoaaChannel_A;
ul16 NoaaChannel_B;

// --------------------

#seekto 0x00A028;
ul16 fmfreq[48];

// --------------------

#seekto 0x00A0A8;
u8 keyM_longpress_action:7,
   button_beep:1;

u8 key1_shortpress_action;
u8 key1_longpress_action;
u8 key2_shortpress_action;
u8 key2_longpress_action;
u8 scan_resume_mode;    
u8 auto_keypad_lock;
u8 power_on_dispmode;
ul32 password;

// --------------------

#seekto 0x00A0B8;
u8 voice;
i8 dbm_corr[7];

// --------------------

#seekto 0x00A0C0;
u8 alarm_mode;
u8 roger_beep;
u8 rp_ste;
u8 TX_VFO;
u8 Battery_type;

// --------------------

#seekto 0x00A0C8;
char logo_line1[16];
char logo_line2[16];

// --------------------

#seekto 0x00A0E8;
struct {
    u8 side_tone;
    char separate_code;
    char group_call_code;
    u8 decode_response;
    u8 auto_reset_time;
    u8 preload_time;
    u8 first_code_persist_time;
    u8 hash_persist_time;
    u8 code_persist_time;
    u8 code_interval_time;
    u8 permit_remote_kill;

    #seekto 0x00A0F8;
    char local_code[3];
    #seek 5;
    char kill_code[5];
    #seek 3;
    char revive_code[5];
    #seek 3;
    char up_code[16];
    char down_code[16];
} dtmf;

// --------------------

#seekto 0x00A130;

struct {
    u8 slPriorEnab:1,
       slDef:7;
        
    ul16 slPriorCh1;
    ul16 slPriorCh2;
    ul16 call_channel;

    u8 __UNUSED11;
} sl;

// --------------------

#seekto 0x00A150;
u8 int_flock;
u8 int_350tx_unsused;
u8 int_KILLED;
u8 int_200tx_unsused;
u8 int_500tx_unsused;
u8 int_350en;
u8 int_scren;

u8  backlight_on_TX_RX:2,
    AM_fix:1,
    mic_bar:1,
    battery_text:2,
    live_DTMF_decoder:1,
    __UNUSED12:1;

// --------------------

#seekto 0x00A158;
struct {
u8 ENABLE_DTMF_CALLING:1,
   ENABLE_PWRON_PASSWORD:1,
   ENABLE_TX1750:1,
   ENABLE_ALARM:1,
   ENABLE_VOX:1,
   ENABLE_VOICE:1,
   ENABLE_NOAA:1,
   ENABLE_FMRADIO:1;
u8 ENABLE_CW_MODULATOR:1,
   ENABLE_FEAT_F4HWN_RESCUE_OPS:1,
   ENABLE_BANDSCOPE:1,
   ENABLE_AM_FIX:1,
   ENABLE_FEAT_F4HWN_GAME:1,
   ENABLE_RAW_DEMODULATORS:1,
   ENABLE_WIDE_RX:1,
   ENABLE_FLASHLIGHT:1;
} BUILD_OPTIONS;

// State[2]: keypad lock scope, full byte (0..SET_LCK_LEN-1).
// Firmware >= v5.6 moved set_lck here and widened it to 4 values
// (KEYS / +ACTIONS / +PTT / +ACTIONS+PTT); it used to be a single bit
// packed into the set_gui byte below. See settings.c (State[2] = set_lck).
u8 __UNUSED14:6,
set_lck:2;
u8 __UNUSED15;

u8 set_off_tmr:7,
set_tmr:1;

u8 set_gui:1,
set_met:1,
__UNUSED17:1,
set_inv:1,
set_contrast:4;

u8 set_tot:4,
set_eot:4;

u8 set_pwr:4,
   __UNUSED16:2,
   set_scn:1,
   set_ptt:1;

#seekto 0x00A160;
struct {
    char version[16];
} version;

// EEPROM layout schema marker. Pre-v1.4 firmware never wrote this byte, so it
// reads back as erased 0xFF. See NR7Y_SCHEMA_* above.
#seekto 0x00A170;
u8 schema;

// --------------------

struct {
    struct {
        #seekto 0x00B000;
        u8 openRssiThr[10];
        #seekto 0x00B010;
        u8 closeRssiThr[10];
        #seekto 0x00B020;
        u8 openNoiseThr[10];
        #seekto 0x00B030;
        u8 closeNoiseThr[10];
        #seekto 0x00B040;
        u8 closeGlitchThr[10];
        #seekto 0x00B050;
        u8 openGlitchThr[10];
    } sqlBand4_7;

    struct {
        #seekto 0x00B060;
        u8 openRssiThr[10];
        #seekto 0x00B070;
        u8 closeRssiThr[10];
        #seekto 0x00B080;
        u8 openNoiseThr[10];
        #seekto 0x00B090;
        u8 closeNoiseThr[10];
        #seekto 0x00B0A0;
        u8 closeGlitchThr[10];
        #seekto 0x00B0B0;
        u8 openGlitchThr[10];
    } sqlBand1_3;

    #seekto 0x00B0C0;
    struct {
        ul16 level1;
        ul16 level2;
        ul16 level4;
        ul16 level6;
    } rssiLevelsBands3_7;

    struct {
        ul16 level1;
        ul16 level2;
        ul16 level4;
        ul16 level6;
    } rssiLevelsBands1_2;

    struct {
        struct {
            u8 lower;
            u8 center;
            u8 upper;
        } low;
        struct {
            u8 lower;
            u8 center;
            u8 upper;
        } mid;
        struct {
            u8 lower;
            u8 center;
            u8 upper;
        } hi;
        #seek 7;
    } txp[7];

    #seekto 0x00B140;
    ul16 batLvl[6];

    #seekto 0x00B150;
    ul16 vox1Thr[10];

    #seekto 0x00B168;
    ul16 vox0Thr[10];

    #seekto 0x00B180;
    u8 micLevel[5];

    #seekto 0x00B188;
    il16 xtalFreqLow;

    #seekto 0x00B18E;
    u8 volumeGain;
    u8 dacGain;
} cal;

"""
# F4HWN parameter
FM_CHANNELS_MAX = 48
MR_CHANNELS_MAX = 1024
MR_CHANNELS_LIST = 25

# flags1
FLAGS1_OFFSET_NONE = 0b00
FLAGS1_OFFSET_MINUS = 0b10
FLAGS1_OFFSET_PLUS = 0b01

POWER_HIGH = 0b111
POWER_MEDIUM = 0b110
POWER_LOW5 = 0b101
POWER_LOW4 = 0b100
POWER_LOW3 = 0b011
POWER_LOW2 = 0b010
POWER_LOW1 = 0b001
POWER_USER = 0b000

# SET_LOW_POWER f4hwn
SET_LOW_LIST = [ "< 20mW", "125mW", "250mW", "500mW", "1W", "2W", "5W"]

# SET_PTT f4hwn
SET_PTT_LIST = ["CLASSIC", "ONEPUSH"]

# SET_SCN f4hwn
SET_SCN_LIST = ["FAST", "NORMAL"]

# SET_TOT and SET_EOT f4hwn
SET_TOT_EOT_LIST = ["OFF", "SOUND", "VISUAL", "ALL"]

# SET_OFF_ON f4hwn
SET_OFF_ON_LIST = ["OFF", "ON"]

# SET_lck f4hwn - keypad lock scope (matches firmware enum SET_LCK_t /
# gSubMenu_SET_LCK: KEYS=0, +ACTIONS=1, +PTT=2, +ACTIONS+PTT=3)
SET_LCK_LIST = ["KEYS", "KEYS + ACTIONS", "KEYS + PTT", "KEYS + ACTIONS + PTT"]

# SET_MET SET_GUI f4hwn
SET_MET_LIST = ["TINY", "CLASSIC"]

# dtmf_flags
PTTID_LIST = ["OFF", "UP CODE", "DOWN CODE", "UP+DOWN CODE", "APOLLO QUINDAR"]

# power          
UVK5_POWER_LEVELS = [chirp_common.PowerLevel("USER = < 20mW to 5W", watts=0.000),
                     chirp_common.PowerLevel("LOW 1 = < 20mW", watts=0.020),
                     chirp_common.PowerLevel("LOW 2 = 125mW", watts=0.125),
                     chirp_common.PowerLevel("LOW 3 = 250mW", watts=0.250),
                     chirp_common.PowerLevel("LOW 4 = 500mW", watts=0.500),
                     chirp_common.PowerLevel("LOW 5 = 1W", watts=1.00),
                     chirp_common.PowerLevel("MID = 2W",  watts=2.00),
                     chirp_common.PowerLevel("HIGH = 5W", watts=5.00),
                     ]

# compander
COMPANDER_LIST = ["OFF", "TX", "RX", "TX/RX"]

# rx mode
RXMODE_LIST = ["MAIN ONLY", "DUAL RX RESPOND", "CROSS BAND", "MAIN TX DUAL RX"]

# channel display mode
CHANNELDISP_LIST = ["Frequency (FREQ)", "CHANNEL NUMBER", "NAME", "Name + Frequency (NAME + FREQ)"]

# TalkTime
TALK_TIME_LIST = ["N/U", "N/U", "N/U", "N/U", "N/U", "30 sec", "35 sec", "40 sec", "45 sec", "50 sec", "55 sec", 
                  "1 min", "1 min : 5 sec", "1 min : 10 sec", "1 min : 15 sec", "1 min : 20 sec", "1 min : 25 sec", "1 min : 30 sec", "1 min : 35 sec", "1 min : 40 sec", "1 min : 45 sec", "1 min : 50 sec", "1 min : 55 sec", 
                  "2 min", "2 min : 5 sec", "2 min : 10 sec", "2 min : 15 sec", "2 min : 20 sec", "2 min : 25 sec", "2 min : 30 sec", "2 min : 35 sec", "2 min : 40 sec", "2 min : 45 sec", "2 min : 50 sec", "2 min : 55 sec", 
                  "3 min", "3 min : 5 sec", "3 min : 10 sec", "3 min : 15 sec", "3 min : 20 sec", "3 min : 25 sec", "3 min : 30 sec", "3 min : 35 sec", "3 min : 40 sec", "3 min : 45 sec", "3 min : 50 sec", "3 min : 55 sec", 
                  "4 min", "4 min : 5 sec", "4 min : 10 sec", "4 min : 15 sec", "4 min : 20 sec", "4 min : 25 sec", "4 min : 30 sec", "4 min : 35 sec", "4 min : 40 sec", "4 min : 45 sec", "4 min : 50 sec", "4 min : 55 sec",
                  "5 min", "5 min : 5 sec", "5 min : 10 sec", "5 min : 15 sec", "5 min : 20 sec", "5 min : 25 sec", "5 min : 30 sec", "5 min : 35 sec", "5 min : 40 sec", "5 min : 45 sec", "5 min : 50 sec", "5 min : 55 sec",
                  "6 min", "6 min : 5 sec", "6 min : 10 sec", "6 min : 15 sec", "6 min : 20 sec", "6 min : 25 sec", "6 min : 30 sec", "6 min : 35 sec", "6 min : 40 sec", "6 min : 45 sec", "6 min : 50 sec", "6 min : 55 sec", 
                  "7 min", "7 min : 5 sec", "7 min : 10 sec", "7 min : 15 sec", "7 min : 20 sec", "7 min : 25 sec", "7 min : 30 sec", "7 min : 35 sec", "7 min : 40 sec", "7 min : 45 sec", "7 min : 50 sec", "7 min : 55 sec", 
                  "8 min", "8 min : 5 sec", "8 min : 10 sec", "8 min : 15 sec", "8 min : 20 sec", "8 min : 25 sec", "8 min : 30 sec", "8 min : 35 sec", "8 min : 40 sec", "8 min : 45 sec", "8 min : 50 sec", "8 min : 55 sec", 
                  "9 min", "9 min : 5 sec", "9 min : 10 sec", "9 min : 15 sec", "9 min : 20 sec", "9 min : 25 sec", "9 min : 30 sec", "9 min : 35 sec", "9 min : 40 sec", "9 min : 45 sec", "9 min : 50 sec", "9 min : 55 sec",
                  "10 min", "10 min : 5 sec", "10 min : 10 sec", "10 min : 15 sec", "10 min : 20 sec", "10 min : 25 sec", "10 min : 30 sec", "10 min : 35 sec", "10 min : 40 sec", "10 min : 45 sec", "10 min : 50 sec", "10 min : 55 sec",
                  "11 min", "11 min : 5 sec", "11 min : 10 sec", "11 min : 15 sec", "11 min : 20 sec", "11 min : 25 sec", "11 min : 30 sec", "11 min : 35 sec", "11 min : 40 sec", "11 min : 45 sec", "11 min : 50 sec", "11 min : 55 sec", 
                  "12 min", "12 min : 5 sec", "12 min : 10 sec", "12 min : 15 sec", "12 min : 20 sec", "12 min : 25 sec", "12 min : 30 sec", "12 min : 35 sec", "12 min : 40 sec", "12 min : 45 sec", "12 min : 50 sec", "12 min : 55 sec", 
                  "13 min", "13 min : 5 sec", "13 min : 10 sec", "13 min : 15 sec", "13 min : 20 sec", "13 min : 25 sec", "13 min : 30 sec", "13 min : 35 sec", "13 min : 40 sec", "13 min : 45 sec", "13 min : 50 sec", "13 min : 55 sec", 
                  "14 min", "14 min : 5 sec", "14 min : 10 sec", "14 min : 15 sec", "14 min : 20 sec", "14 min : 25 sec", "14 min : 30 sec", "14 min : 35 sec", "14 min : 40 sec", "14 min : 45 sec", "14 min : 50 sec", "14 min : 55 sec",
                  "15 min"]

# Set NFM value
SET_NFM_LIST = ["NARROW", "NARROWER"]

# ---- EEPROM schema -------------------------------------------------------
# Schema 1 (pre-v1.4 firmware) borrowed bit 6 of the channel flags byte as a
# NARROWEST selector on CW/USB channels, so TX_LOCK could never be set there.
# Schema 2 gives bit 6 back to TX_LOCK and keeps the filter in bit 1 as a plain
# wide/narrow flag, with the firmware deciding what those mean per modulation
# (FM 25k/12.5k, CW and SSB 6.25k/2.0k).
#
# Schema 1 is read-only: an image is upgraded in memory on load and only ever
# written back as schema 2. Pre-v1.4 firmware never wrote the marker, so it
# reads back as erased 0xFF.
NR7Y_SCHEMA_ADDR = 0x00A170
NR7Y_SCHEMA_LEGACY = 1
NR7Y_SCHEMA_CURRENT = 2

# modulation -> (wide name, narrow name). CW and USB have no narrow mode string
# in chirp_common.MODES, so their bandwidth bit rides in an Extra rather than in
# the mode, and both entries are the same name.
_NR7Y_MODE_NAME = {
    0: ("FM", "NFM"),
    1: ("AM", "NAM"),
    2: ("USB", "USB"),
    3: ("CW", "CW"),
}

# mode name -> (modulation, bandwidth). A bandwidth of None means the mode
# string does not determine the bit - it comes from the Bandwidth extra.
_NR7Y_MODE_BITS = {
    "FM":  (0, 0),
    "NFM": (0, 1),
    "AM":  (1, 0),
    "NAM": (1, 1),
    "USB": (2, None),
    "CW":  (3, None),
}

# Wide/narrow as the user sees it. The firmware translates per modulation, so
# CHIRP deliberately does not talk in kilohertz here.
NR7Y_BANDWIDTH_LIST = ["Wide", "Narrow"]

# Set RxA value
SET_RXA_FM_LIST = ["FLAT", "CLEAN", "MID", "BOOST", "MAX"]
SET_RXA_AM_LIST = ["SHARP", "STOCK", "OPEN"]

# Set KEY value
SET_KEY_LIST = ["MENU", "KEY_UP", "KEY_DOWN", "KEY_EXIT", "KEY_STAR"]

# Set Off timer 
SET_OFF_TMR_LIST = ["OFF"]

# Add values from 00h:01m to 02h:00m
for h in range(2):  # From 0 to 2 hours
    if h == 1:  # Add 01h:00m
        SET_OFF_TMR_LIST.append(f"{h:d}h:00m")
    for m in range(1, 60):  # From 1 to 59 minutes (start at 1)
        SET_OFF_TMR_LIST.append(f"{h:d}h:{m:02d}m")

SET_OFF_TMR_LIST.append(f"2h:00m")

# Add Auto Keypad Lock values
AUTO_KEYPAD_LOCK_LIST = ["OFF"]
for s in range(10):  # From 0 to 10 minutes
    for ms in ["00s", "15s", "30s", "45s"]:
        if s == 0 and ms == "00s":  # Cancel "00m:00s"
            continue
        AUTO_KEYPAD_LOCK_LIST.append(f"{s:02d}m:{ms}")
AUTO_KEYPAD_LOCK_LIST.append("10m:00s")  # Add "10m:00s" a the end

# set nav
SET_NAV_LIST = ["LEFT/RIGHT (UV-K1)", "UP/DOWN (UV-K5 V3)"]

# battery save
BATSAVE_LIST = ["OFF", "1:1", "1:2", "1:3", "1:4", "1:5"]

# battery type
BATTYPE_LIST = ["1600 mAh K5", "2200 mAh K5", "3500 mAh K5", "1400 mAh K1", "2500 mAh K1"]
# bat txt
BAT_TXT_LIST = ["NONE", "VOLTAGE", "PERCENT"]
# Backlight auto mode
BACKLIGHT_LIST = ["OFF", "5 sec", "10 sec", "15 sec", "20 sec", "25 sec", "30 sec", "35 sec", "40 sec", "45 sec", "50 sec", "55 sec", 
                  "1 min", "1 min : 5 sec", "1 min : 10 sec", "1 min : 15 sec", "1 min : 20 sec", "1 min : 25 sec", "1 min : 30 sec", "1 min : 35 sec", "1 min : 40 sec", "1 min : 45 sec", "1 min : 50 sec", "1 min : 55 sec", 
                  "2 min", "2 min : 5 sec", "2 min : 10 sec", "2 min : 15 sec", "2 min : 20 sec", "2 min : 25 sec", "2 min : 30 sec", "2 min : 35 sec", "2 min : 40 sec", "2 min : 45 sec", "2 min : 50 sec", "2 min : 55 sec", 
                  "3 min", "3 min : 5 sec", "3 min : 10 sec", "3 min : 15 sec", "3 min : 20 sec", "3 min : 25 sec", "3 min : 30 sec", "3 min : 35 sec", "3 min : 40 sec", "3 min : 45 sec", "3 min : 50 sec", "3 min : 55 sec", 
                  "4 min", "4 min : 5 sec", "4 min : 10 sec", "4 min : 15 sec", "4 min : 20 sec", "4 min : 25 sec", "4 min : 30 sec", "4 min : 35 sec", "4 min : 40 sec", "4 min : 45 sec", "4 min : 50 sec", "4 min : 55 sec",
                  "5 min", "Always On (ON)"]

# Backlight LVL
BACKLIGHT_LVL_LIST = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"]

# Backlight _TX_RX_LIST
BACKLIGHT_TX_RX_LIST = ["OFF", "TX", "RX", "TX/RX"]

# steps TODO: change order
STEPS = [2.5, 5, 6.25, 10, 12.5, 25, 8.33, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 1.25,
         9, 15, 20, 30, 50, 100, 125, 200, 250, 500]

# ctcss/dcs codes
TMODES = ["", "Tone", "DTCS", "DTCS"]
TONE_NONE = 0
TONE_CTCSS = 1
TONE_DCS = 2
TONE_RDCS = 3


CTCSS_TONES = [
    67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4,
    88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9,
    114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2,
    151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8,
    177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5,
    203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8,
    250.3, 254.1
]

# lifted from ft4.py
DTCS_CODES = [  # TODO: add negative codes
    23,  25,  26,  31,  32,  36,  43,  47,  51,  53,  54,
    65,  71,  72,  73,  74,  114, 115, 116, 122, 125, 131,
    132, 134, 143, 145, 152, 155, 156, 162, 165, 172, 174,
    205, 212, 223, 225, 226, 243, 244, 245, 246, 251, 252,
    255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325,
    331, 332, 343, 346, 351, 356, 364, 365, 371, 411, 412,
    413, 423, 431, 432, 445, 446, 452, 454, 455, 462, 464,
    465, 466, 503, 506, 516, 523, 526, 532, 546, 565, 606,
    612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723,
    731, 732, 734, 743, 754
]

# flock list extended
FLOCK_LIST = ["DEFAULT+ (137-174, 400-470)",
              "FCC HAM (144-148, 420-450)",
              "CA HAM (144-148, 430-450)",
              "CE HAM (144-146, 430-440)",
              "GB HAM (144-148, 430-440)",
              "137-174, 400-430",
              "137-174, 400-438",
              "PMR 446",
              "GMRS FRS MURS",
              "DISABLE ALL",
              "UNLOCK ALL"]

# Scan Resum List              
SCANRESUME_LIST = ["STOP : Stop scan when a signal is received"]

# Add "CARRIER" values
for s in range(20):  # From 0 to 20s
    for ms in ["250ms", "500ms", "750ms"] if s == 0 else ["000ms", "250ms", "500ms", "750ms"]:
        SCANRESUME_LIST.append(f"CARRIER {s:02d}s:{ms} : Listen for this time until the signal disappears")

SCANRESUME_LIST.append(f"CARRIER 20s:000ms : Listen for this time until the signal disappears")

# Add "TIMEOUT" values
for m in range(5, 125, 5):  # From 5 to 120 secondes (2 minutes)
    minutes = m // 60
    seconds = m % 60
    SCANRESUME_LIST.append(f"TIMEOUT {minutes:02d}m:{seconds:02d}s : Listen for this time and resume")

# Welcome and Voice list     
WELCOME_LIST = ["Message line 1, Voltage, Sound (ALL)", "Make 2 short sounds (SOUND)", "User message line 1 and line 2 (MESSAGE)", "Battery voltage (VOLTAGE)", "Picture (LOGO)", "NONE"]
VOICE_LIST = ["OFF", "Chinese", "English"]

# ACTIVE CHANNEL
TX_VFO_LIST = ["A", "B"]
ALARMMODE_LIST = ["SITE", "TONE"]
ROGER_LIST = ["OFF", "Roger beep (ROGER)", "MDC data burst (MDC)"]
RTE_LIST = ["OFF", "100ms", "200ms", "300ms", "400ms",
            "500ms", "600ms", "700ms", "800ms", "900ms", "1000ms"]
VOX_LIST = ["OFF", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"]

MEM_SIZE =      0x00B190    # size of all memory
PROG_SIZE =     0x00A171    # size of the memory that we will write (LAST ADDRESS + 1 !!!)
MEM_BLOCK =     0x80        # largest block of memory that we can reliably write
CAL_START =     0x00B000    # calibration memory start address
F4HWN_START =   0x00A158    # calibration F4HWN memory start address

# fm radio supported frequencies
FMMIN = 76.0
FMMAX = 108.0

# bands supported by the UV-K5
BANDS_STANDARD = {
        0: [50.0, 76.0],
        1: [108.0, 136.9999],
        2: [137.0, 173.9999],
        3: [174.0, 349.9999],
        4: [350.0, 399.9999],
        5: [400.0, 469.9999],
        6: [470.0, 600.0]
        }

BANDS_WIDE = {
        0: [18.0, 108.0],
        1: [108.0, 136.9999],
        2: [137.0, 173.9999],
        3: [174.0, 349.9999],
        4: [350.0, 399.9999],
        5: [400.0, 469.9999],
        6: [470.0, 1300.0]
        }

SCANLIST_LIST = ["OFF"] + [f"List [{i}]" for i in range(1, MR_CHANNELS_LIST)] + ["ALL"]

SCANLIST_SELECT_LIST = (
    [f"LIST [{i}]" for i in range(1, MR_CHANNELS_LIST)]
    + ["LIST [ALL]"]
)

DTMF_CHARS = "0123456789ABCD*# "
DTMF_CHARS_ID = "0123456789ABCDabcd"
DTMF_CHARS_KILL = "0123456789ABCDabcd"
DTMF_CHARS_UPDOWN = "0123456789ABCDabcd#* "
DTMF_CODE_CHARS = "ABCD*# "
DTMF_DECODE_RESPONSE_LIST = ["DO NOTHING", "Local ringing (RING)", "Replay response (REPLY)",
                             "Local ringing + reply response (BOTH)"]

KEYACTIONS_LIST = ["NONE",
                   "FLASHLIGHT",
                   "POWER",
                   "MONITOR",
                   "SCAN",
                   "VOX",
                   "ALARM",
                   "FM RADIO",
                   "1750Hz",
                   "LOCK KEYPAD",
                   "VFO A / VFO B",
                   "VFO / MEM",
                   "MODE",
                   "BL_MIN_TMP_OFF",
                   "RX MODE",
                   "MAIN ONLY", 
                   "PTT",                  
                   "WIDE / NARROW",
                   "BACKLIGHT",
                   "MUTE",
                   "RxA",
                   "POWER HIGH",
                   "REMOVE OFFSET",
                   "BEAM"
                  ]

MIC_GAIN_LIST = ["+1.5dB", "+4.0dB", "+8.0dB", "+12.0dB", "+16.0dB", "+20.0dB", "+24.0dB", "+28.0dB", "+31.5dB"]

# ============================================================
# NR7Y CW extension – module-level constants
# ============================================================

# CW settings base address in fusion firmware (PY25Q16 direct address)
_NR7Y_CW_SETTINGS_ADDR = 0x00A140

# CW macro EEPROM addresses (cwmacro.h: reuses DTMF contacts region 0x1C00-0x1CFF)
_NR7Y_CW_MACRO_ADDRS = [0x1C00, 0x1C30, 0x1C60, 0x1C90]
_NR7Y_CW_MACRO_SIZE = 48       # Block layout: [0]=len|SIG, [1..46]=payload, [47]=checksum
_NR7Y_CW_MACRO_MAX_LEN = 46   # Payload bytes
_NR7Y_CW_MACRO_SIG = 0x80     # Signature stored in high bit of length byte

# CW key input mode display strings (menu index 0-10 stored directly in EEPROM bits 0-4)
_NR7Y_CW_KEY_INPUT_MODES = [
    "PTT HandKey",           # 0: 0x08
    "Port HandKey",          # 1: 0x18
    "Side Btn Iambic",       # 2: 0x04
    "Side Btn Iambic Rev",   # 3: 0x05
    "Port Iambic",           # 4: 0x12
    "Port Iambic Reversed",  # 5: 0x13
    "Port and Btn Iambic",       # 6: 0x16
    "Port and Btn Iambic Rev",   # 7: 0x17
    "USB Port Iambic",           # 8: 0x20
    "USB Port Iambic Rev",       # 9: 0x21
    "USB Port HandKey",          # 10: 0x28
]

# CW keyer mode display strings (stored as a full byte 0-3 at _NR7Y_CW_SETTINGS_ADDR+4)
_NR7Y_CW_KEYER_MODES = [
    "Iambic A",       # 0
    "Iambic B",       # 1
    "Ultimatic",      # 2
    "Semi-Auto Bug",  # 3
]

# Extended key actions for NR7Y firmware with CW modulator, mirroring
# App/settings.h enum ACTION_OPT_t.
#
# The firmware enum is conditionally compiled, so a stored key-action number only
# means something relative to the features that build has.  Slots 0-13 are
# unconditional -- the enum keeps a value for ALARM/FM/1750/FLASHLIGHT even when
# the feature is compiled out, which is why those only need filtering out of the
# displayed choices -- but the RESCUE_OPS block shifts everything after it, so the
# index mapping has to be assembled per build instead of hardcoded.  With
# RESCUE_OPS off the CW actions sit at 21-28 and CODE PRACTICE at 29; with it on
# they are 23-30 and 31.
_NR7Y_ACTIONS_COMMON = [
    "NONE",            # 0:  ACTION_OPT_NONE
    "FLASHLIGHT",      # 1:  ACTION_OPT_FLASHLIGHT
    "POWER",           # 2:  ACTION_OPT_POWER
    "MONITOR",         # 3:  ACTION_OPT_MONITOR
    "SCAN",            # 4:  ACTION_OPT_SCAN
    "VOX",             # 5:  ACTION_OPT_VOX
    "ALARM",           # 6:  ACTION_OPT_ALARM
    "FM RADIO",        # 7:  ACTION_OPT_FM
    "1750Hz",          # 8:  ACTION_OPT_1750
    "LOCK KEYPAD",     # 9:  ACTION_OPT_KEYLOCK
    "VFO A / VFO B",   # 10: ACTION_OPT_A_B
    "VFO / MEM",       # 11: ACTION_OPT_VFO_MR
    "MODE",            # 12: ACTION_OPT_SWITCH_DEMODUL
    "BL_MIN_TMP_OFF",  # 13: ACTION_OPT_BLMIN_TMP_OFF
    # ENABLE_FEAT_F4HWN
    "RX MODE",         # 14: ACTION_OPT_RXMODE
    "MAIN ONLY",       # 15: ACTION_OPT_MAINONLY
    "PTT",             # 16: ACTION_OPT_PTT
    "WIDE / NARROW",   # 17: ACTION_OPT_WN
    "BACKLIGHT",       # 18: ACTION_OPT_BACKLIGHT
    "MUTE",            # 19: ACTION_OPT_MUTE
    "RxA",             # 20: ACTION_OPT_RXA
]

# ENABLE_FEAT_F4HWN_RESCUE_OPS
_NR7Y_ACTIONS_RESCUE_OPS = ["POWER HIGH", "REMOVE OFFSET"]

# ENABLE_CW_MODULATOR
_NR7Y_ACTIONS_CW = [
    "PLAY CW MSG 1",
    "PLAY CW MSG 2",
    "PLAY CW MSG 3",
    "PLAY CW MSG 4",
    "REPEAT CW MSG 1",
    "REPEAT CW MSG 2",
    "REPEAT CW MSG 3",
    "REPEAT CW MSG 4",
    # ACTION_OPT_CPO is guarded on ENABLE_CW_MODULATOR *and*
    # ENABLE_CODE_PRACTICE, but BUILD_OPTIONS has no spare bit to report code
    # practice separately and every CW preset enables both, so it rides along
    # with the CW block.
    "CODE PRACTICE",
]

# ENABLE_FEAT_F4HWN_BEAM, then ENABLE_FEAT_F4HWN_RXTX_LOG, then (since F4HWN
# v5.9.0) ENABLE_FEAT_F4HWN_FOXHUNT close out the enum, in that order.  None of
# them has a BUILD_OPTIONS bit, but they all sit past every conditional block
# above, so guessing here cannot shift anything else.  The NR7Y CW presets build
# FOX HUNT and leave BEAM / RF LOG out; the stock F4HWN editions carry all three.
_NR7Y_ACTIONS_TAIL_CW = ["FOX HUNT"]
_NR7Y_ACTIONS_TAIL = ["BEAM", "RF LOG", "FOX HUNT"]


def _nr7y_keyactions_list(has_rescue_ops, has_cw):
    """Return the index -> action-name map for the running firmware build."""
    lst = list(_NR7Y_ACTIONS_COMMON)
    if has_rescue_ops:
        lst += _NR7Y_ACTIONS_RESCUE_OPS
    if has_cw:
        lst += _NR7Y_ACTIONS_CW
        return lst + _NR7Y_ACTIONS_TAIL_CW
    return lst + _NR7Y_ACTIONS_TAIL


def xorarr(data: bytes):
    """the communication is obfuscated using this fine mechanism"""
    tbl = [22, 108, 20, 230, 46, 145, 13, 64, 33, 53, 213, 64, 19, 3, 233, 128]
    ret = b""
    idx = 0
    for byte in data:
        ret += bytes([byte ^ tbl[idx]])
        idx = (idx+1) % len(tbl)
    return ret


def calculate_crc16_xmodem(data: bytes):
    """
    if this crc was used for communication to AND from the radio, then it
    would be a measure to increase reliability.
    but it's only used towards the radio, so it's for further obfuscation
    """
    poly = 0x1021
    crc = 0x0
    for byte in data:
        crc = crc ^ (byte << 8)
        for _ in range(8):
            crc = crc << 1
            if crc & 0x10000:
                crc = (crc ^ poly) & 0xFFFF
    return crc & 0xFFFF


def _send_command(serport, data: bytes):
    """Send a command to UV-K5 radio"""
    LOG.debug("Sending command (unobfuscated) len=0x%4.4x:\n%s",
              len(data), util.hexprint(data))

    crc = calculate_crc16_xmodem(data)
    data2 = data + struct.pack("<H", crc)

    command = struct.pack(">HBB", 0xabcd, len(data), 0) + \
        xorarr(data2) + \
        struct.pack(">H", 0xdcba)
    if DEBUG_SHOW_OBFUSCATED_COMMANDS:
        LOG.debug("Sending command (obfuscated):\n%s", util.hexprint(command))
    try:
        result = serport.write(command)
    except Exception as e:
        raise errors.RadioError("Error writing data to radio") from e
    return result


def _receive_reply(serport):
    header = serport.read(4)
    if len(header) != 4:
        LOG.warning("Header short read: [%s] len=%i",
                    util.hexprint(header), len(header))
        raise errors.RadioError("Header short read")
    if header[0] != 0xAB or header[1] != 0xCD or header[3] != 0x00:
        LOG.warning("Bad response header: %s len=%i",
                    util.hexprint(header), len(header))
        raise errors.RadioError("Bad response header")

    cmd = serport.read(int(header[2]))
    if len(cmd) != int(header[2]):
        LOG.warning("Body short read: [%s] len=%i",
                    util.hexprint(cmd), len(cmd))
        raise errors.RadioError("Command body short read")

    footer = serport.read(4)

    if len(footer) != 4:
        LOG.warning("Footer short read: [%s] len=%i",
                    util.hexprint(footer), len(footer))
        raise errors.RadioError("Footer short read")

    if footer[2] != 0xDC or footer[3] != 0xBA:
        LOG.debug("Reply before bad response footer (obfuscated)"
                  "len=0x%4.4x:\n%s", len(cmd), util.hexprint(cmd))
        LOG.warning("Bad response footer: %s len=%i",
                    util.hexprint(footer), len(footer))
        raise errors.RadioError("Bad response footer")

    if DEBUG_SHOW_OBFUSCATED_COMMANDS:
        LOG.debug("Received reply (obfuscated) len=0x%4.4x:\n%s",
                  len(cmd), util.hexprint(cmd))

    cmd2 = xorarr(cmd)

    LOG.debug("Received reply (unobfuscated) len=0x%4.4x:\n%s",
              len(cmd2), util.hexprint(cmd2))

    return cmd2


def _getstring(data: bytes, begin, maxlen):
    tmplen = min(maxlen+1, len(data))
    ss = [data[i] for i in range(begin, tmplen)]
    key = 0
    for key, val in enumerate(ss):
        if val < ord(' ') or val > ord('~'):
            return ''.join(chr(x) for x in ss[0:key])
    return ''


def _sayhello(serport):
    hellopacket = b"\x14\x05\x04\x00\x6a\x39\x57\x64"

    tries = 5
    while True:
        LOG.debug("Sending hello packet")
        _send_command(serport, hellopacket)
        rep = _receive_reply(serport)
        if rep:
            break
        tries -= 1
        if tries == 0:
            LOG.warning("Failed to initialise radio")
            raise errors.RadioError("Failed to initialize radio")
    if rep.startswith(b'\x18\x05'):
        raise errors.RadioError("Radio is in programming mode, "
                                "restart radio into normal mode")
    firmware = _getstring(rep, 4, 24)

    LOG.info("Found firmware: %s", firmware)
    return firmware


def _readmem(serport, offset, length):
    LOG.debug("Sending readmem offset=0x%4.4x len=0x%4.4x", offset, length)

    readmem = b"\x1b\x05\x08\x00" + \
        struct.pack("<HBB", offset, length, 0) + \
        b"\x6a\x39\x57\x64"
    _send_command(serport, readmem)
    rep = _receive_reply(serport)
    if DEBUG_SHOW_MEMORY_ACTIONS:
        LOG.debug("readmem Received data len=0x%4.4x:\n%s",
                  len(rep), util.hexprint(rep))
    return rep[8:]


def _writemem(serport, data, offset):
    LOG.debug("Sending writemem offset=0x%4.4x len=0x%4.4x",
              offset, len(data))

    if DEBUG_SHOW_MEMORY_ACTIONS:
        LOG.debug("writemem sent data offset=0x%4.4x len=0x%4.4x:\n%s",
                  offset, len(data), util.hexprint(data))

    dlen = len(data)
    writemem = b"\x1d\x05" + \
        struct.pack("<BBHBB", dlen+8, 0, offset, dlen, 1) + \
        b"\x6a\x39\x57\x64"+data

    _send_command(serport, writemem)
    rep = _receive_reply(serport)

    LOG.debug("writemem Received data: %s len=%i",
              util.hexprint(rep), len(rep))

    if (rep[0] == 0x1e and
       rep[4] == (offset & 0xff) and
       rep[5] == (offset >> 8) & 0xff):
        return True

    LOG.warning("Bad data from writemem")
    raise errors.RadioError("Bad response to writemem")


def _resetradio(serport):
    resetpacket = b"\xdd\x05\x00\x00"
    _send_command(serport, resetpacket)


def _check_upload_target(serport, firmware):
    """Refuse to program a radio this module does not understand.

    Two gates, both hard. Download and upload are independent operations, so
    the module cannot assume the radio in front of it is the one the image came
    from - it has to ask, before the first write.
    """
    # 1. Right firmware family? The handshake string is AUTHOR_STRING_2 plus
    #    VERSION_STRING_2 from the firmware's version.c. Our tree sets
    #    AUTHOR_STRING_2 to NR7Y; armel's Fusion sets it to F4HWN. Locally
    #    built firmware reports a bare "NR7Y " because only the release
    #    workflow injects a tag, so match on the prefix alone.
    if not (firmware or "").startswith("NR7Y"):
        raise errors.RadioError(
            "This module only programs NR7Y CW firmware.\n"
            "Radio reports: %s" % (firmware or "(no version string)"))

    # 2. Right EEPROM schema? Block on anything but an exact match. Reading it
    #    from the radio depends on eeprom_compat.c mapping through 0xA171 -
    #    without that the address is unmapped and reads back 0xFF.
    # Read 8 bytes, not 1. do_download proves hello-then-readmem is safe at the
    # 0x80 block size it uses, but a 1-byte read is an unusual size this
    # protocol is never otherwise asked for, and a 1-byte probe is what the
    # removed detect_from_serial() was doing when downloads stopped working.
    # Stay on a size the firmware handles routinely.
    rep = _readmem(serport, NR7Y_SCHEMA_ADDR, 8)
    if not rep:
        raise errors.RadioError("Could not read the EEPROM schema marker.")

    schema = rep[0]
    if schema == 0xFF:          # never written = pre-v1.4. Must come first:
        schema = NR7Y_SCHEMA_LEGACY   # 0xFF > CURRENT would take the wrong branch.

    if schema < NR7Y_SCHEMA_CURRENT:
        raise errors.RadioError(
            "This radio runs firmware older than v1.4 (EEPROM schema %d).\n"
            "Update the firmware first - uploading now would corrupt CW and "
            "SSB channel settings." % schema)

    if schema > NR7Y_SCHEMA_CURRENT:
        raise errors.RadioError(
            "This radio uses EEPROM schema %d, newer than this module "
            "understands (%d).\nUse the CHIRP module that shipped with the "
            "radio's firmware release." % (schema, NR7Y_SCHEMA_CURRENT))


def do_download(radio):
    """download eeprom from radio"""
    serport = radio.pipe
    serport.timeout = 4.0
    status = chirp_common.Status()
    status.cur = 0
    status.max = MEM_SIZE
    status.msg = "Downloading from radio"
    radio.status_fn(status)

    eeprom = b""
    f = _sayhello(serport)
    if f:
        radio.FIRMWARE_VERSION = f
    else:
        raise errors.RadioError("Failed to initialize radio")


    addr = 0
    while addr < MEM_SIZE:
        data = _readmem(serport, addr, MEM_BLOCK)
        status.cur = addr
        radio.status_fn(status)

        if data and len(data) == MEM_BLOCK:
            eeprom += data
            addr += MEM_BLOCK
        else:
            raise errors.RadioError("Memory download incomplete")

    # Provenance travels with the image, so a saved .img can be checked with no
    # radio attached. Mirrors what mainline uvk5.py stores for firmware. Read
    # the schema from the bytes we just fetched rather than asking the radio
    # again - on pre-v1.4 firmware the address is unmapped and reads 0xFF.
    schema = eeprom[NR7Y_SCHEMA_ADDR] if len(eeprom) > NR7Y_SCHEMA_ADDR else 0xFF
    if schema == 0xFF:
        schema = NR7Y_SCHEMA_LEGACY

    radio.metadata = {'nr7y_firmware': f, 'nr7y_schema': schema}

    return memmap.MemoryMapBytes(eeprom)


def do_upload(radio):
    """upload configuration to radio eeprom"""

    serport = radio.pipe
    serport.timeout = 4.0

    status = chirp_common.Status()
    status.cur = 0
    status.msg = "Uploading to radio"

    # Step 0: always write program/config region
    # Step 1: optionally write calibration region
    step = 0

    radio.status_fn(status)

    f = _sayhello(serport)
    if not f:
        # sync_out() discards this function's return value, so returning False
        # here would report success on a failed handshake.
        raise errors.RadioError("Failed to initialize radio")
    radio.FIRMWARE_VERSION = f

    _check_upload_target(serport, f)

    while True:
        if step == 0:
            # Always write the "normal" area
            start_addr = 0x000000
            stop_addr  = PROG_SIZE
            status.max = stop_addr - start_addr
            status.cur = 0
            status.msg = "Uploading to radio"
            radio.status_fn(status)

        elif step == 1 and radio.upload_calibration:
            # Then write calibration area (separate region)
            start_addr = CAL_START
            stop_addr  = MEM_SIZE
            status.max = stop_addr - start_addr
            status.cur = 0
            status.msg = "Uploading calibration"
            radio.status_fn(status)

        else:
            break  # done

        addr = start_addr
        while addr < stop_addr:
            remaining = stop_addr - addr
            chunk = MEM_BLOCK if remaining >= MEM_BLOCK else remaining

            dat = radio.get_mmap()[addr:addr + chunk]

            # Critical: detect empty slice (often happens if mmap is shorter than expected)
            if not dat or len(dat) != chunk:
                raise errors.RadioError(
                    f"Memory upload incomplete at 0x{addr:06X} "
                    f"(wanted {chunk} bytes, got {0 if dat is None else len(dat)})"
                )

            _writemem(serport, dat, addr)

            status.cur = addr - start_addr
            radio.status_fn(status)

            addr += chunk

        step += 1

    status.msg = "Uploaded OK"
    radio.status_fn(status)

    _resetradio(serport)
    return True


def min_max_def(value, min_val, max_val, default):
    """returns value if in bounds or default otherwise"""
    if min_val is not None and value < min_val:
        return default
    if max_val is not None and value > max_val:
        return default
    return value


def list_def(value, lst, default):
    """return value if is in the list, default otherwise"""
    if isinstance(default, str):
        default = lst.index(default)
    if value < 0 or value >= len(lst):
        return default
    return value    

@directory.register
class UVK5RadioEgzumer(chirp_common.CloneModeRadio):
    """Quansheng UV-K5 (egzumer + f4hwn)"""
    VENDOR = "Quansheng"
    MODEL = "UV-K1 & UV-K5 V3 (F4HWN Fusion)"
    BAUD_RATE = 38400
    NEEDS_COMPAT_SERIAL = False
    FIRMWARE_VERSION = ""

# this change to send power level chan in the calibration but under macos it give error
# bugfix calibration : put in comment next line: upload_calibration = False
    upload_calibration = False

# advanced settings too
    upload_advanced = False

    def _get_bands(self):
        is_wide = self._memobj.BUILD_OPTIONS.ENABLE_WIDE_RX \
            if self._memobj is not None else True
        bands = BANDS_WIDE if is_wide else BANDS_STANDARD
        return bands

    def _find_band(self, hz):
        mhz = hz/1000000.0
        bands = self._get_bands()
        for bnd, rng in bands.items():
            if rng[0] <= mhz <= rng[1]:
                return bnd
        return False

    def _get_vfo_channel_names(self):
        """generates VFO_CHANNEL_NAMES"""
        bands = self._get_bands()
        names = []
        for bnd, rng in bands.items():
            name = f"F{bnd + 1}({round(rng[0])}M-{round(rng[1])}M)"
            names.append(name + "A")
            names.append(name + "B")
        return names

    def _get_specials(self):
        """generates SPECIALS"""
        specials = {}
        for idx, name in enumerate(self._get_vfo_channel_names()):
            specials[name] = MR_CHANNELS_MAX + idx
        return specials

    @classmethod
    def get_prompts(cls):
        rp = chirp_common.RadioPrompts()
        rp.experimental = \
            'This is an experimental driver for the Quansheng UV-K5. ' \
            'It may harm your radio, or worse. Use at your own risk.\n\n' \
            'Before attempting to do any changes please download' \
            'the memory image from the radio with chirp ' \
            'and keep it. This can be later used to recover the ' \
            'original settings. \n\n' \
            'some details are not yet implemented'
        rp.pre_download = \
            "1. Turn radio on.\n" \
            "2. Connect cable to mic/spkr connector.\n" \
            "3. Make sure connector is firmly connected.\n" \
            "4. Click OK to download image from radio.\n\n" \
            "It may not work if you turn on the radio " \
            "with the cable already attached\n"
        rp.pre_upload = \
            "1. Turn radio on.\n" \
            "2. Connect cable to mic/spkr connector.\n" \
            "3. Make sure connector is firmly connected.\n" \
            "4. Click OK to upload the image to radio.\n\n" \
            "It may not work if you turn on the radio " \
            "with the cable already attached"
        return rp

    # Return information about this radio's features, including
    # how many memories it has, what bands it supports, etc
    def get_features(self):
        rf = chirp_common.RadioFeatures()
        rf.has_bank = False
        rf.valid_dtcs_codes = DTCS_CODES
        rf.has_rx_dtcs = True
        rf.has_ctone = True
        rf.has_settings = True
        rf.has_comment = False
        rf.valid_name_length = 10
        rf.valid_power_levels = UVK5_POWER_LEVELS
        rf.valid_special_chans = self._get_vfo_channel_names()
        rf.valid_duplexes = ["", "-", "+"]

        steps = STEPS.copy()
        steps.sort()
        rf.valid_tuning_steps = steps

        rf.valid_tmodes = ["", "Tone", "TSQL", "DTCS", "Cross"]
        rf.valid_cross_modes = ["Tone->Tone", "Tone->DTCS", "DTCS->Tone",
                                "->Tone", "->DTCS", "DTCS->", "DTCS->DTCS"]

        rf.valid_characters = chirp_common.CHARSET_ASCII
        rf.valid_modes = ["FM", "NFM", "AM", "NAM", "USB"]

        rf.valid_skips = [""]

        # This radio supports memories 1-250, 251-264 are the VFO memories
        rf.memory_bounds = (1, MR_CHANNELS_MAX)

        # This is what the BK4819 chip supports
        # Will leave it in a comment, might be useful someday
        # rf.valid_bands = [(18000000,  620000000),
        #                  (840000000, 1300000000)
        #                  ]
        rf.valid_bands = []
        bands = self._get_bands()
        for _, rng in bands.items():
            rf.valid_bands.append(
                    (int(rng[0]*1000000), int(rng[1]*1000000)))
        return rf

    # Do a download of the radio from the serial port
    def sync_in(self):
        self._mmap = do_download(self)
        self.process_mmap()

    # Do an upload of the radio to the serial port
    def sync_out(self):
        do_upload(self)

    # Convert the raw byte array into a memory object structure
    def process_mmap(self):
        self._memobj = bitwise.parse(MEM_FORMAT, self._mmap)

    # Return a raw representation of the memory object, which
    # is very helpful for development
    def get_raw_memory(self, number):
        return repr(self._memobj.channel[number-1])

    def validate_memory(self, mem):
        # Ensure frequency and offset are integers (not strings or bitDE objects)
        # to prevent TypeError in parent class validation
        try:
            if not isinstance(mem.freq, int):
                mem.freq = int(mem.freq)
            if not isinstance(mem.offset, int):
                mem.offset = int(mem.offset)
        except (ValueError, TypeError):
            # If conversion fails, let parent handle it with proper error
            pass
        
        msgs = super().validate_memory(mem)

        if mem.duplex == "" :
            return msgs

        # find tx frequency
        if mem.duplex == '-':
            txfreq = mem.freq - mem.offset
        elif mem.duplex == '+':
            txfreq = mem.freq + mem.offset
        else:
            txfreq = mem.freq

        # find band
        band = self._find_band(txfreq)
        if band is False:
            msg = f"Transmit frequency {txfreq/1000000.0:.4f}MHz " \
                   "is not supported by this radio"
            msgs.append(chirp_common.ValidationWarning(msg))

        band = self._find_band(mem.freq)
        if band is False:
            msg = f"The frequency {mem.freq/1000000.0:%.4f}MHz " \
                   "is not supported by this radio"
            msgs.append(chirp_common.ValidationWarning(msg))

        return msgs

    def _set_tone(self, mem, _mem):
        ((txmode, txtone, txpol),
         (rxmode, rxtone, rxpol)) = chirp_common.split_tone_encode(mem)

        if txmode == "Tone":
            txtoval = CTCSS_TONES.index(txtone)
            txmoval = 0b01
        elif txmode == "DTCS":
            txmoval = txpol == "R" and 0b11 or 0b10
            txtoval = DTCS_CODES.index(txtone)
        else:
            txmoval = 0
            txtoval = 0

        if rxmode == "Tone":
            rxtoval = CTCSS_TONES.index(rxtone)
            rxmoval = 0b01
        elif rxmode == "DTCS":
            rxmoval = rxpol == "R" and 0b11 or 0b10
            rxtoval = DTCS_CODES.index(rxtone)
        else:
            rxmoval = 0
            rxtoval = 0

        _mem.rxcodeflag = rxmoval
        _mem.txcodeflag = txmoval
        _mem.rxcode = rxtoval
        _mem.txcode = txtoval

    def _get_tone(self, mem, _mem):
        # Convert bitDE values to int to handle chirp bitwise parser objects
        rxtype = int(_mem.rxcodeflag)
        txtype = int(_mem.txcodeflag)
        
        # Validate tone type indices before accessing TMODES list
        # TMODES has only 4 elements (indices 0-3), but 4-bit fields can have values 0-15
        # Invalid values (4-15) typically occur when EEPROM memory is uninitialized (0xFF)
        if rxtype >= len(TMODES):
            LOG.warning(f"Memory {mem.number}: Invalid rxcodeflag={rxtype} (expected 0-3), resetting to 0")
            rxtype = 0
            _mem.rxcodeflag = 0
            
        if txtype >= len(TMODES):
            LOG.warning(f"Memory {mem.number}: Invalid txcodeflag={txtype} (expected 0-3), resetting to 0")
            txtype = 0
            _mem.txcodeflag = 0
        
        rx_tmode = TMODES[rxtype]
        tx_tmode = TMODES[txtype]

        rx_tone = tx_tone = None

        if tx_tmode == "Tone":
            if _mem.txcode < len(CTCSS_TONES):
                tx_tone = CTCSS_TONES[_mem.txcode]
            else:
                tx_tone = 0
                tx_tmode = ""
        elif tx_tmode == "DTCS":
            if _mem.txcode < len(DTCS_CODES):
                tx_tone = DTCS_CODES[_mem.txcode]
            else:
                tx_tone = 0
                tx_tmode = ""

        if rx_tmode == "Tone":
            if _mem.rxcode < len(CTCSS_TONES):
                rx_tone = CTCSS_TONES[_mem.rxcode]
            else:
                rx_tone = 0
                rx_tmode = ""
        elif rx_tmode == "DTCS":
            if _mem.rxcode < len(DTCS_CODES):
                rx_tone = DTCS_CODES[_mem.rxcode]
            else:
                rx_tone = 0
                rx_tmode = ""

        tx_pol = txtype == 0x03 and "R" or "N"
        rx_pol = rxtype == 0x03 and "R" or "N"

        chirp_common.split_tone_decode(mem, (tx_tmode, tx_tone, tx_pol),
                                       (rx_tmode, rx_tone, rx_pol))

    # ------------------------------------------------------------- image version

    def _eeprom_version_string(self):
        """Firmware version recorded in the image itself, or "" if absent.

        The firmware stamps VERSION_STRING_2 into the EEPROM at boot (see
        SETTINGS_InitEEPROM in settings.c), so unlike FIRMWARE_VERSION - which
        is only set during a live download - this travels with a saved .img and
        can be read with no radio attached.
        """
        try:
            raw = str(self._memobj.version.version)
        except Exception as exc:
            LOG.warning("Cannot read the EEPROM version block: %s", exc)
            return ""

        # Erased flash reads back as 0xFF and the firmware zero-pads a short
        # string, so stop at the first byte that is not printable ASCII.
        out = []
        for ch in raw:
            if not (' ' <= ch <= '~'):
                break
            out.append(ch)
        return ''.join(out).strip()

    # ---------------------------------------------------------------- mode bits

    def _nr7y_apply_mode(self, _mem_chan, memory):
        """Write modulation and bandwidth for the memory's mode.

        The mode string fixes the bandwidth bit only for FM and AM, where
        FM/NFM and AM/NAM are the ordinary CHIRP convention. USB and CW have no
        narrow mode string to carry it (chirp_common.MODES has NCW but no NUSB,
        and using one without the other would be inconsistent), so their bit
        comes from the Bandwidth extra and is never inferred.
        """
        mod, bw = _NR7Y_MODE_BITS.get(memory.mode, (0, 0))
        _mem_chan.modulation = mod

        if bw is None:
            bw = 0
            for setting in getattr(memory, 'extra', None) or []:
                if setting.get_name() == "nr7y_bandwidth":
                    try:
                        bw = NR7Y_BANDWIDTH_LIST.index(str(setting.value))
                    except ValueError:
                        bw = 0
                    break
        _mem_chan.bandwidth = bw

    def _nr7y_mem_chan(self, number):
        """The raw channel or VFO record for a memory number, or None."""
        if not isinstance(number, int):
            return None
        ch_num = number - 1
        if ch_num < 0:
            return None
        if ch_num < MR_CHANNELS_MAX:
            return self._memobj.channel[ch_num]
        try:
            return self._memobj.vfo_channel[ch_num - MR_CHANNELS_MAX]
        except IndexError:
            return None

    # Extract a high-level memory object from the low-level memory map
    # This is called to populate a memory in the UI
    def get_memory(self, number):

        mem = chirp_common.Memory()

        if isinstance(number, str):
            ch_num = self._get_specials()[number]
            mem.extd_number = number
        else:
            ch_num = number - 1

        mem.number = ch_num + 1

        # Access the correct structure based on channel type
        if ch_num < MR_CHANNELS_MAX:
            # Regular memory channel (0-249)
            _mem = self._memobj.channel[ch_num]
        else:
            # VFO channel (250-263) -> vfo_channel[0-13] at 0x0fa0
            vfo_index = ch_num - MR_CHANNELS_MAX
            _mem = self._memobj.vfo_channel[vfo_index]

        is_empty = False
        # We'll consider any blank (i.e. 0MHz frequency) to be empty
        if (_mem.freq == 0xffffffff) or (_mem.freq == 0):
            is_empty = True

        # We'll also look at the channel attributes if a memory has them
        tmpscn = 0
        tmp_comp = 0
        if ch_num < MR_CHANNELS_MAX:
            _mem3 = self._memobj.ch_attr[ch_num]
            # scanlists - use new 5-bit scanlist field
            tmpscn = _mem3.scanlist
            tmp_comp = list_def(_mem3.compander, COMPANDER_LIST, 0)
        elif ch_num < MR_CHANNELS_MAX + 14:
            att_num = MR_CHANNELS_MAX + int((ch_num - MR_CHANNELS_MAX) / 2)
            _mem3 = self._memobj.ch_attr[att_num]
            tmp_comp = list_def(_mem3.compander, COMPANDER_LIST, 0)

        if is_empty:
            mem.empty = True
            # set some sane defaults:
            mem.power = UVK5_POWER_LEVELS[2]
            mem.extra = RadioSettingGroup("Extra", "extra")

            val = RadioSettingValueList(SET_OFF_ON_LIST)
            rs = RadioSetting("txLock", "TXLock", val)
            mem.extra.append(rs)

            val = RadioSettingValueBoolean(False)
            rs = RadioSetting("busyChLockout", "BusyCL", val)
            mem.extra.append(rs)

            val = RadioSettingValueBoolean(False)
            rs = RadioSetting("frev", "FreqRev", val)
            mem.extra.append(rs)

            val = RadioSettingValueList(PTTID_LIST)
            rs = RadioSetting("pttid", "PTTID", val)
            mem.extra.append(rs)

            val = RadioSettingValueBoolean(False)
            rs = RadioSetting("dtmfdecode", "DTMF decode", val)
#            if self._memobj.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#                mem.extra.append(rs)

            val = RadioSettingValueList(COMPANDER_LIST)
            rs = RadioSetting("compander", "Compander", val)
            mem.extra.append(rs)

            val = RadioSettingValueList(SCANLIST_LIST)
            rs = RadioSetting("scanlists", "Scanlists", val)
            mem.extra.append(rs)

            # actually the step and duplex are overwritten by chirp based on
            # bandplan. they are here to document sane defaults for IARU r1
            # mem.tuning_step = 25.0
            # mem.duplex = "off"

            return mem

        if ch_num > MR_CHANNELS_MAX - 1:
            mem.name = self._get_vfo_channel_names()[ch_num-MR_CHANNELS_MAX]
            mem.immutable = ["name", "scanlists"]
        else:
            _mem2 = self._memobj.channelname[ch_num]
            for char in _mem2.name:
                if str(char) == "\xFF" or str(char) == "\x00":
                    break
                mem.name += str(char)
            mem.name = mem.name.rstrip()

        # Convert your low-level frequency to Hertz
        mem.freq = int(_mem.freq)*10
        mem.offset = int(_mem.offset)*10

        if _mem.offsetDir == FLAGS1_OFFSET_MINUS:
            mem.duplex = '-'
        elif _mem.offsetDir == FLAGS1_OFFSET_PLUS:
                mem.duplex = '+'
        else:
                mem.duplex = ''
                                
        # tone data
        self._get_tone(mem, _mem)

        # ---- NR7Y: mode by table, not by arithmetic ----------------------
        # Upstream computed the mode index as modulation * 2 + bandwidth, which
        # only works while every modulation has a narrow variant in valid_modes.
        # It does not: CHIRP has no NUSB string (chirp_common.MODES has NCW but
        # not NUSB), so the list is ragged and the stride runs off the end at
        # index 5. An explicit table sidesteps it entirely, and lets the
        # bandwidth bit stay meaningful for USB/CW instead of being pinned.
        modulation = int(_mem.modulation)
        bandwidth = int(_mem.bandwidth)

        names = _NR7Y_MODE_NAME.get(modulation)
        if names is None:
            LOG.warning(f"Memory {mem.number}: Invalid modulation={modulation}, "
                        f"bandwidth={bandwidth}, using FM as default")
            mem.mode = "FM"
            _mem.modulation = 0
            _mem.bandwidth = 0
        else:
            mem.mode = names[bandwidth & 1]

        # tuning step
        tstep = int(_mem.step)
        if tstep < len(STEPS):
            mem.tuning_step = STEPS[tstep]
        else:
            LOG.warning(f"Memory {mem.number}: Invalid step={tstep}, using 2.5 as default")
            mem.tuning_step = 2.5

        # power
        txpower = int(_mem.txpower)
        if txpower == POWER_HIGH:
            mem.power = UVK5_POWER_LEVELS[7]
        elif txpower == POWER_MEDIUM:
            mem.power = UVK5_POWER_LEVELS[6]
        elif txpower == POWER_LOW5:
            mem.power = UVK5_POWER_LEVELS[5]
        elif txpower == POWER_LOW4:
            mem.power = UVK5_POWER_LEVELS[4]
        elif txpower == POWER_LOW3:
            mem.power = UVK5_POWER_LEVELS[3]
        elif txpower == POWER_LOW2:
            mem.power = UVK5_POWER_LEVELS[2]
        elif txpower == POWER_LOW1:
            mem.power = UVK5_POWER_LEVELS[1]
        else:
            # Invalid power value, use default
            LOG.warning(f"Memory {mem.number}: Invalid txpower={txpower}, using USER as default")
            mem.power = UVK5_POWER_LEVELS[0]

        # We'll consider any blank (i.e. 0MHz frequency) to be empty
        if (_mem.freq == 0xffffffff) or (_mem.freq == 0):
            mem.empty = True
        else:
            mem.empty = False

        mem.extra = RadioSettingGroup("Extra", "extra")

        # TXLock
        enc = list_def(_mem.txLock, SET_OFF_ON_LIST, 0)
        val = RadioSettingValueList(SET_OFF_ON_LIST, None, enc)
        rs = RadioSetting("txLock", "TX Lock  (TXLock)", val)
        rs.set_doc('TXLock: If the TX is permit on this channel, allow TX')
        mem.extra.append(rs)

        # BusyCL
        val = RadioSettingValueBoolean(_mem.busyChLockout)
        rs = RadioSetting("busyChLockout", "Busy Ch Lockout (BusyCL)", val)
        rs.set_doc('BusyCL: If the channel is Busy, do not allow TX')
        mem.extra.append(rs)

        # Frequency reverse
        val = RadioSettingValueBoolean(_mem.freq_reverse)
        rs = RadioSetting("frev", "Reverse Frequencies (R)", val)
        rs.set_doc('R: Is this needs to be reversed?')
        mem.extra.append(rs)

        # PTTID
        pttid = list_def(_mem.dtmf_pttid, PTTID_LIST, 0)
        val = RadioSettingValueList(PTTID_LIST, None, pttid)
        rs = RadioSetting("pttid", "PTT ID (PTT ID)", val)
        rs.set_doc('PTT ID:  How do you want the ID to be sent\n' + \
                   '* NONE : Nothing sent\n' + \
                   '* UP CODE : Send UPCODE when TX.\n' + \
                   '* DOWW CODE : Send DWCODE when back to RX\n' + \
                   '* UP+DOWN Code : Send UPCODE and DWCODE\n' + \
                   '* APOLLO QUINDAR : Send beep at start and end of TX')

        mem.extra.append(rs)

        # DTMF DECODE
        val = RadioSettingValueBoolean(_mem.dtmf_decode)
        rs = RadioSetting("dtmfdecode", "DTMF decode (D Decd)", val)
#        if self._memobj.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#            mem.extra.append(rs)

        # Compander
        val = RadioSettingValueList(COMPANDER_LIST, None, tmp_comp)
        rs = RadioSetting("compander", "Compander (Compnd)", val)
        rs.set_doc('Compnd: Do you want to compand on this frequency?')
        mem.extra.append(rs)

        val = RadioSettingValueList(SCANLIST_LIST, None, tmpscn)
        rs = RadioSetting("scanlists", "Scanlists (SList)", val)
        rs.set_doc('SList: Is this frequency is part of a scan list?')
        mem.extra.append(rs)

        return mem

    def set_settings(self, settings):
        _mem = self._memobj
        for element in settings:
            if not isinstance(element, RadioSetting):
                self.set_settings(element)
                continue

            elname = element.get_name()

            # basic settings

            # VFO_A e80 ScreenChannel_A
            if elname == "VFO_A_chn":
                _mem.ScreenChannel_A = int(element.value)
                if _mem.ScreenChannel_A < MR_CHANNELS_MAX:
                    _mem.MrChannel_A = _mem.ScreenChannel_A
                elif _mem.ScreenChannel_A < MR_CHANNELS_MAX + 7:
                    _mem.FreqChannel_A = _mem.ScreenChannel_A
                else:
                    _mem.NoaaChannel_A = _mem.ScreenChannel_A

            # VFO_B e83
            elif elname == "VFO_B_chn":
                _mem.ScreenChannel_B = int(element.value)
                if _mem.ScreenChannel_B < MR_CHANNELS_MAX:
                    _mem.MrChannel_B = _mem.ScreenChannel_B
                elif _mem.ScreenChannel_B < MR_CHANNELS_MAX + 7:
                    _mem.FreqChannel_B = _mem.ScreenChannel_B
                else:
                    _mem.NoaaChannel_B = _mem.ScreenChannel_B

            # TX_VFO  channel selected A,B
            elif elname == "TX_VFO":
                _mem.TX_VFO = int(element.value)

            # call channel
            elif elname == "call_channel":
                _mem.sl.call_channel = int(element.value)

            # squelch
            elif elname == "squelch":
                _mem.squelch = int(element.value)

            # TOT
            elif elname == "tot":
                _mem.max_talk_time = int(element.value)

            # NOAA autoscan
            elif elname == "noaa_autoscan":
                _mem.noaa_autoscan = int(element.value)

            # VOX
            elif elname == "vox":
                voxvalue = int(element.value)
                _mem.vox_switch = voxvalue > 0
                _mem.vox_level = (voxvalue - 1) if _mem.vox_switch else 0

            # mic gain
            elif elname == "mic_gain":
                _mem.mic_gain = int(element.value)

            # Channel display mode
            elif elname == "channel_display_mode":
                _mem.channel_display_mode = int(element.value)

            # RX Mode
            elif elname == "rx_mode":
                tmptxmode = int(element.value)
                tmpmainvfo = _mem.TX_VFO + 1
                _mem.crossband = tmpmainvfo * bool(tmptxmode & 0b10)
                _mem.dual_watch = tmpmainvfo * bool(tmptxmode & 0b01)

            # Battery Save
            elif elname == "battery_save":
                _mem.battery_save = int(element.value)

            # Backlight auto mode
            elif elname == "backlight_time":
                _mem.backlight_time = int(element.value)

            # Backlight min
            elif elname == "backlight_min":
                _mem.backlight_min = int(element.value)

            # Backlight max
            elif elname == "backlight_max":
                _mem.backlight_max = int(element.value)

            # Backlight TX_RX
            elif elname == "backlight_on_TX_RX":
                _mem.backlight_on_TX_RX = int(element.value)
            # AM_fix
            elif elname == "AM_fix":
                _mem.AM_fix = int(element.value)

            # mic_bar
            elif elname == "mic_bar":
                _mem.mic_bar = int(element.value)

            # Batterie txt
            elif elname == "battery_text":
                _mem.battery_text = int(element.value)

            # Tail tone elimination
            elif elname == "ste":
                _mem.ste = int(element.value)

            # VFO Open
            #elif elname == "freq_mode_allowed":
            #    _mem.freq_mode_allowed = int(element.value)

            # Current State
            elif elname == "current_state":
                _mem.current_state = int(element.value)


            # Beep control
            elif elname == "button_beep":
                _mem.button_beep = int(element.value)

            # Scan resume mode
            elif elname == "scan_resume_mode":
                _mem.scan_resume_mode = int(element.value)

            # Keypad lock
            elif elname == "key_lock":
                _mem.key_lock = int(element.value)

            # Set nav
            elif elname == "set_nav":
                if self.upload_advanced:
                    _mem.set_nav = int(element.value)

            # Auto keypad lock
            elif elname == "auto_keypad_lock":
                _mem.auto_keypad_lock = int(element.value)

            # Power on display mode
            elif elname == "welcome_mode":
                _mem.power_on_dispmode = int(element.value)

            # Keypad Tone
            elif elname == "voice":
                _mem.voice = int(element.value)

            elif elname.startswith("dbm_corr_"):
                if self.upload_advanced:
                    i = int(elname.split("_")[-1])
                    _mem.dbm_corr[i] = int(element.value)

#            elif elname == "password":
#                if element.value.get_value() is None or element.value == "":
#                    _mem.password = 0xFFFFFFFF
#                else:
#                    _mem.password = int(element.value)

            # Alarm mode
            elif elname == "alarm_mode":
                _mem.alarm_mode = int(element.value)

            # Reminding of end of talk
            elif elname == "roger_beep":
                _mem.roger_beep = int(element.value)

            # Repeater tail tone elimination
            elif elname == "rp_ste":
                _mem.rp_ste = int(element.value)

            # Logo string 1
            elif elname == "logo1":
                bts = str(element.value).rstrip("\x20\xff\x00")+"\x00"*12
                _mem.logo_line1 = bts[0:12]+"\x00\xff\xff\xff"

            # Logo string 2
            elif elname == "logo2":
                bts = str(element.value).rstrip("\x20\xff\x00")+"\x00"*12
                _mem.logo_line2 = bts[0:12]+"\x00\xff\xff\xff"

            # unlock settings

            # FLOCK
            elif elname == "int_flock":
                _mem.int_flock = int(element.value)

#            # 350TX
#            elif elname == "int_350tx":
#                _mem.int_350tx = int(element.value)

            # KILLED
            elif elname == "int_KILLED":
                _mem.int_KILLED = int(element.value)

#            # 200TX
#            elif elname == "int_200tx":
#                _mem.int_200tx = int(element.value)

#            # 500TX
#            elif elname == "int_500tx":
#                _mem.int_500tx = int(element.value)

            # 350EN
            elif elname == "int_350en":
                _mem.int_350en = int(element.value)

            # SCREN
            elif elname == "int_scren":
                _mem.int_scren = int(element.value)

            # battery type
            elif elname == "Battery_type":
                if self.upload_advanced:
                    _mem.Battery_type = int(element.value)

            # set low_power f4hwn
            elif elname == "set_pwr":
                _mem.set_pwr = int(element.value)

            # set ptt f4hwn
            elif elname == "set_ptt":
                _mem.set_ptt = int(element.value)

            # set tot f4hwn
            elif elname == "set_tot":
                _mem.set_tot = int(element.value)

            # set eot f4hwn
            elif elname == "set_eot":
                _mem.set_eot = int(element.value)

            # set_contrast f4hwn
            elif elname == "set_contrast":
                _mem.set_contrast = int(element.value)

            # set inv f4hwn
            elif elname == "set_inv":
                _mem.set_inv = int(element.value)

            # set lck f4hwn
            elif elname == "set_lck":
                _mem.set_lck = int(element.value)

            # set met f4hwn
            elif elname == "set_met":
                _mem.set_met = int(element.value)

            # set gui f4hwn
            elif elname == "set_gui":
                _mem.set_gui = int(element.value)
                               
            # set tmr f4hwn
            elif elname == "set_tmr":
                _mem.set_tmr = int(element.value)

            # set off f4hwn
            elif elname == "set_off_tmr":
                _mem.set_off_tmr = int(element.value)

            # set nfm f4hwn
            elif elname == "set_nfm":
                _mem.set_nfm = int(element.value)

            # set rxa f4hwn
            elif elname == "set_rxa_fm":
                _mem.set_rxa_fm = int(element.value)

            elif elname == "set_rxa_am":
                _mem.set_rxa_am = int(element.value)

            # set key f4hwn
            elif elname == "set_key":
                _mem.set_key = int(element.value)

            # set ptt f4hwn
            elif elname == "set_scn":
                _mem.set_scn = int(element.value)

             # set menu lock f4hwn
            elif elname == "set_menu_lock":
                _mem.set_menu_lock = int(element.value)

            # fm radio
            for i in range(1, FM_CHANNELS_MAX + 1):
                freqname = "FM_" + str(i)
                if elname == freqname:
                    val = str(element.value).strip()
                    try:
                        val2 = int(float(val)*10)
                    except Exception:
                        val2 = 0xffff

                    if val2 < FMMIN*10 or val2 > FMMAX*10:
                        val2 = 0xffff
#                        raise errors.InvalidValueError(
#                                "FM radio frequency should be a value "
#                                "in the range %.1f - %.1f" % (FMMIN , FMMAX))
                    _mem.fmfreq[i-1] = val2

            # dtmf settings
            if elname == "dtmf_side_tone":
                _mem.dtmf.side_tone = int(element.value)

            elif elname == "dtmf_separate_code":
                _mem.dtmf.separate_code = str(element.value)

            elif elname == "dtmf_group_call_code":
                _mem.dtmf.group_call_code = element.value

            elif elname == "dtmf_decode_response":
                _mem.dtmf.decode_response = int(element.value)

            elif elname == "dtmf_auto_reset_time":
                _mem.dtmf.auto_reset_time = int(element.value)

            elif elname == "dtmf_preload_time":
                _mem.dtmf.preload_time = int(int(element.value)/10)

            elif elname == "dtmf_first_code_persist_time":
                _mem.dtmf.first_code_persist_time = int(int(element.value)/10)

            elif elname == "dtmf_hash_persist_time":
                _mem.dtmf.hash_persist_time = int(int(element.value)/10)

            elif elname == "dtmf_code_persist_time":
                _mem.dtmf.code_persist_time = \
                        int(int(element.value)/10)

            elif elname == "dtmf_code_interval_time":
                _mem.dtmf.code_interval_time = \
                        int(int(element.value)/10)

            elif elname == "dtmf_permit_remote_kill":
                _mem.dtmf.permit_remote_kill = \
                        int(element.value)

            elif elname == "dtmf_dtmf_local_code":
                k = str(element.value).rstrip("\x20\xff\x00") + "\x00"*3
                _mem.dtmf.local_code = k[0:3]

            elif elname == "dtmf_dtmf_up_code":
                k = str(element.value).strip("\x20\xff\x00") + "\x00"*16
                _mem.dtmf.up_code = k[0:16]

            elif elname == "dtmf_dtmf_down_code":
                k = str(element.value).rstrip("\x20\xff\x00") + "\x00"*16
                _mem.dtmf.down_code = k[0:16]

            elif elname == "dtmf_kill_code":
                k = str(element.value).strip("\x20\xff\x00") + "\x00"*5
                _mem.dtmf.kill_code = k[0:5]

            elif elname == "dtmf_revive_code":
                k = str(element.value).strip("\x20\xff\x00") + "\x00"*5
                _mem.dtmf.revive_code = k[0:5]

            elif elname == "live_DTMF_decoder":
                _mem.live_DTMF_decoder = int(element.value)

            # scanlist stuff
            if elname == "slDef":
                _mem.sl.slDef = int(element.value) + 1

            elif elname == "slPriorEnab":
                _mem.sl.slPriorEnab = int(element.value)

            elif elname == "slPriorCh1":
                _mem.sl.slPriorCh1 = int(element.value)

            elif elname == "slPriorCh2":
                _mem.sl.slPriorCh2 = int(element.value)

            elif elname.startswith("listname"):
                idx = int(elname.replace("listname", ""))
                if 0 <= idx < (MR_CHANNELS_LIST - 1):
                    val_str = str(element.value)  # Plus de strip()
                    
                    if val_str:  # Si non vide
                        val_bytes = val_str.encode('ascii', 'ignore')[:3]
                        val_bytes = val_bytes + b'\x20' * (4 - len(val_bytes))
                    else:  # Si vide, remplir avec 0xFF
                        val_bytes = b'\xFF' * 4
                        
                    _mem.listname[idx].name = val_bytes

            # Shortcuts

            if elname == "key1_shortpress_action":
                _mem.key1_shortpress_action = KEYACTIONS_LIST.index(element.value)

            elif elname == "key1_longpress_action":
                _mem.key1_longpress_action = KEYACTIONS_LIST.index(element.value)

            elif elname == "key2_shortpress_action":
                _mem.key2_shortpress_action = KEYACTIONS_LIST.index(element.value)

            elif elname == "key2_longpress_action":
                _mem.key2_longpress_action = KEYACTIONS_LIST.index(element.value)

            elif elname == "keyM_longpress_action":
                _mem.keyM_longpress_action = KEYACTIONS_LIST.index(element.value)

# this change to send power level chan in the calibration but under macos it give error
# bugfix calibration : remove the comment on next 2 line:
#            elif elname == "upload_calibration":
#                self._upload_calibration = bool(element.value)

            elif element.changed() and elname.startswith("_mem.cal."):
                exec(elname + " = element.value.get_value()")

    def get_settings(self):
        _mem = self._memobj

# add menu firmware with version and option display if version 3.0 and up
        ValFirm = "Firmware : " + self.FIRMWARE_VERSION 
        Compair1 = "F4HWN v5.0"  

        if self.FIRMWARE_VERSION == "":
            # No live radio, but the image carries the version the firmware
            # stamped into EEPROM, so report that rather than nothing.
            _img_version = self._eeprom_version_string()
            if _img_version:
                ValFirm = "Firmware : " + _img_version + " (from image)"
            else:
                ValFirm = "Firmware : Only when read from the radio "
      
        else:
            FIRMWARE_VERSION_RADIO = self.FIRMWARE_VERSION
            ValFirm = ValFirm + " Fusion Edition"
  
        radio_firmware = RadioSettingGroup("radio_firmwarebasic", ValFirm)
# add link for mise a jour information

        val = RadioSettingValueBoolean(False)
        def validate_Go_Web_Firmware(value):
            if value :
                msg = "To see information for the update of the Firmware F4HWN \n"
                ret = wx.MessageBox(msg, "Warning", wx.OK | wx.CANCEL |
                                    wx.CANCEL_DEFAULT | wx.ICON_WARNING)
                if ret == wx.OK :
                    webbrowser.open(FIRMWARE_VERSION_UPDATE)
                value = False
                    
            return value

        val.set_validate_callback(validate_Go_Web_Firmware)
        rs = RadioSetting("Update_Firmware_mise_a_jour","To see information for the update of the Firmware F4HWN , select this box ->", val)
        rs.set_doc('To see information for the update of the Firmware F4HWN !')
        radio_firmware.append(rs)

# end add link for mise a jour information

# end add menu firmware with version and option display
        
        basic = RadioSettingGroup("basic", "Basic Settings")
        advanced = RadioSettingGroup("advanced", "Advanced Settings")
        keya = RadioSettingGroup("keya", "Programmable Keys")
        dtmf = RadioSettingGroup("dtmf", "DTMF Settings")
#        dtmfc = RadioSettingGroup("dtmfc", "DTMF Contacts")
        scanl = RadioSettingGroup("scn", "Scan Lists")
        unlock = RadioSettingGroup("unlock", "Unlock Settings")
        fmradio = RadioSettingGroup("fmradio", "FM Broadcast Receiver")
        calibration = RadioSettingGroup("calibration", "Calibration")
        help_user = RadioSettingGroup("help_user", "Help For User")
        
        roinfo = RadioSettingGroup("roinfo", "Driver Information + Link WEB")
        top = RadioSettings()
        top.append(radio_firmware)
        top.append(basic)
        top.append(advanced)
        top.append(keya)
        top.append(dtmf)
#        if _mem.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#            top.append(dtmfc)
        top.append(scanl)
        top.append(unlock)
        if _mem.BUILD_OPTIONS.ENABLE_FMRADIO:
            top.append(fmradio)
        top.append(roinfo)
        top.append(calibration)
        top.append(help_user)
        
        # helper function
        def append_label(radio_setting, label, descr=""):
            if not hasattr(append_label, 'idx'):
                append_label.idx = 0

            val = RadioSettingValueString(len(descr), len(descr), descr)
            val.set_mutable(False)
            rs = RadioSetting("label" + str(append_label.idx), label, val)
            append_label.idx += 1
            radio_setting.append(rs)

        # Programmable keys
        def get_action(action_num):
            """"get actual key action"""
            has_alarm = self._memobj.BUILD_OPTIONS.ENABLE_ALARM
            has_1750 = self._memobj.BUILD_OPTIONS.ENABLE_TX1750
            has_flashlight = self._memobj.BUILD_OPTIONS.ENABLE_FLASHLIGHT
            has_fm_radio = self._memobj.BUILD_OPTIONS.ENABLE_FMRADIO
            has_rescue_ops = self._memobj.BUILD_OPTIONS.ENABLE_FEAT_F4HWN_RESCUE_OPS
            has_game = self._memobj.BUILD_OPTIONS.ENABLE_FEAT_F4HWN_GAME
            has_vox = self._memobj.BUILD_OPTIONS.ENABLE_VOX
            
            lst = KEYACTIONS_LIST.copy()
            lst.remove("BACKLIGHT") # Only for key press on TX
            lst.remove("BL_MIN_TMP_OFF")

            if not has_alarm:
                lst.remove("ALARM")
            if not has_1750:
                lst.remove("1750Hz")
            if not has_flashlight:
                lst.remove("FLASHLIGHT")
            if not has_fm_radio:
                lst.remove("FM RADIO")
            if not has_rescue_ops:
                lst.remove("POWER HIGH")
                lst.remove("REMOVE OFFSET")
            if not has_vox:
                lst.remove("MUTE")

            action_num = int(action_num)
            if action_num >= len(KEYACTIONS_LIST) or \
               KEYACTIONS_LIST[action_num] not in lst:
                action_num = 0
            return lst, KEYACTIONS_LIST[action_num]

        val1s = RadioSettingValueList(*get_action(_mem.key1_shortpress_action))
        rs = RadioSetting("key1_shortpress_action",
                          "Side Key 1 Short Press (F1Shrt)", val1s)
        rs.set_doc('F1Shrt: Select the action to do when pressing F1 key for a ' + \
                   'SHORT time, F1 key is located on the left side, first key under PTT')
        keya.append(rs)

        val1l = RadioSettingValueList(*get_action(_mem.key1_longpress_action))
        rs = RadioSetting("key1_longpress_action",
                          "Side Key 1 Long Press (F1Long)", val1l)
        rs.set_doc('F1Long: Select the action to do when pressing F1 key for a ' + \
                   'LONG time, F1 key is located on the left side, first key under PTT')
        keya.append(rs)

        val2s = RadioSettingValueList(*get_action(_mem.key2_shortpress_action))
        rs = RadioSetting("key2_shortpress_action",
                          "Side Key 2 Short Press (F2Shrt)", val2s)
        rs.set_doc('F2Shrt: Select the action to do when pressing F2 key for a ' + \
                   'SHORT time, F2 key is located on the left side, second key under PTT')
        keya.append(rs)

        val2l = RadioSettingValueList(*get_action(_mem.key2_longpress_action))
        rs = RadioSetting("key2_longpress_action",
                          "Side Key 2 Long Press (F2Long)", val2l)
        rs.set_doc('F2Long: Select the action to do when pressing F2 key for a ' + \
                   'LONG time, F2 key is located on the left side, second key under PTT')
        keya.append(rs)

        valm = RadioSettingValueList(*get_action(_mem.keyM_longpress_action))
        rs = RadioSetting("keyM_longpress_action",
                          "Menu Key Long Press (M Long)", valm)
        rs.set_doc('M Long: Select the action to do when pressing M key for a ' + \
                   'LONG time, M key is located below the screen on the left')
        keya.append(rs)

        # ----------------- DTMF settings

        tmpval = str(_mem.dtmf.separate_code)
        if tmpval not in DTMF_CODE_CHARS:
            tmpval = '*'
        val = RadioSettingValueString(1, 1, tmpval)
        val.set_charset(DTMF_CODE_CHARS)
        sep_code_setting = RadioSetting("dtmf_separate_code",
                                        "Separate Code", val)
        sep_code_setting.set_doc('Separate Code: ')

        tmpval = str(_mem.dtmf.group_call_code)
        if tmpval not in DTMF_CODE_CHARS:
            tmpval = '#'
        val = RadioSettingValueString(1, 1, tmpval)
        val.set_charset(DTMF_CODE_CHARS)
        group_code_setting = RadioSetting("dtmf_group_call_code",
                                          "Group Call Code", val)
        group_code_setting.set_doc('Group Call Code: ')

        tmpval = min_max_def(_mem.dtmf.first_code_persist_time * 10,
                             30, 1000, 300)
        val = RadioSettingValueInteger(30, 1000, tmpval, 10)
        first_code_per_setting = \
            RadioSetting("dtmf_first_code_persist_time",
                         "First Code Persist Time (ms)", val)
        first_code_per_setting.set_doc('First code persist time: How long to you want the first DTMF ' + \
                                       'will be sent (in milliseconds)')

        tmpval = min_max_def(_mem.dtmf.hash_persist_time * 10, 30, 1000, 300)
        val = RadioSettingValueInteger(30, 1000, tmpval, 10)
        spec_per_setting = RadioSetting("dtmf_hash_persist_time",
                                        "Persist Time (ms)", val)
        spec_per_setting.set_doc('#/* persist time: How long this code # or / or * will be sent (in milliseconds)')
        
        tmpval = min_max_def(_mem.dtmf.code_persist_time * 10, 30, 1000, 300)
        val = RadioSettingValueInteger(30, 1000, tmpval, 10)
        code_per_setting = RadioSetting("dtmf_code_persist_time",
                                        "Code Persist Time (ms)", val)
        code_per_setting.set_doc('Code persist time: How long the code will be sent (in milliseconds)')

        tmpval = min_max_def(_mem.dtmf.code_interval_time * 10, 30, 1000, 300)
        val = RadioSettingValueInteger(30, 1000, tmpval, 10)
        code_int_setting = RadioSetting("dtmf_code_interval_time",
                                        "Code Interval Time (ms)", val)
        code_int_setting.set_doc('Code interval time: How long to wait between each code sent (in milliseconds)')

        tmpval = str(_mem.dtmf.local_code).upper().strip(
                "\x00\xff\x20")
        for i in tmpval:
            if i in DTMF_CHARS_ID:
                continue
            tmpval = "103"
            break
        val = RadioSettingValueString(3, 3, tmpval)
        val.set_charset(DTMF_CHARS_ID)
        ani_id_setting = \
            RadioSetting("dtmf_dtmf_local_code",
                         "Local Code (3 chars 0-9 ABCD) (ANI ID)", val)
        ani_id_setting.set_doc('ANI ID: DTMF communication radio ID')

        tmpval = str(_mem.dtmf.up_code).upper().strip(
                "\x00\xff\x20")
        for i in tmpval:
            if i in DTMF_CHARS_UPDOWN or i == "":
                continue
            else:
                tmpval = "123"
                break
        val = RadioSettingValueString(1, 16, tmpval)
        val.set_charset(DTMF_CHARS_UPDOWN)
        up_code_setting = \
            RadioSetting("dtmf_dtmf_up_code",
                         "Up Code (1-16 chars 0-9 ABCD*#) (UPCode)", val)
        up_code_setting.set_doc('UPCode: DTMF code sent at the beginning of transmission')

        tmpval = str(_mem.dtmf.down_code).upper().strip(
                "\x00\xff\x20")
        for i in tmpval:
            if i in DTMF_CHARS_UPDOWN:
                continue
            else:
                tmpval = "456"
                break
        val = RadioSettingValueString(1, 16, tmpval)
        val.set_charset(DTMF_CHARS_UPDOWN)
        dw_code_setting = \
            RadioSetting("dtmf_dtmf_down_code",
                         "Down Code (1-16 chars 0-9 ABCD*#) (DWCode)", val)
        dw_code_setting.set_doc('DWCode: DTMF code sent at the end of a transmission')

        val = RadioSettingValueBoolean(_mem.dtmf.side_tone)
        dtmf_side_tone_setting = \
            RadioSetting("dtmf_side_tone",
                         "DTMF Sidetone On Speaker When Sent (D ST)", val)
        dtmf_side_tone_setting.set_doc('D ST: DTMF side tone switch, lets you hear transmitted ' + \
                                       'tones in the radio speaker')

        tmpval = list_def(_mem.dtmf.decode_response,
                          DTMF_DECODE_RESPONSE_LIST, 0)
        val = RadioSettingValueList(DTMF_DECODE_RESPONSE_LIST, None, tmpval)
        dtmf_resp_setting = RadioSetting("dtmf_decode_response",
                                         "Decode Response (D Resp)", val)
        dtmf_resp_setting.set_doc('D Resp: DTMF decoding response')

        tmpval = min_max_def(_mem.dtmf.auto_reset_time, 5, 60, 10)
        val = RadioSettingValueInteger(5, 60, tmpval)
        d_hold_setting = RadioSetting("dtmf_auto_reset_time",
                                      "Auto Reset Time (s) (D Hold)", val)
        d_hold_setting.set_doc('D Hold: DTMF auto reset time')

        # D Prel
        tmpval = min_max_def(_mem.dtmf.preload_time * 10, 30, 990, 300)
        val = RadioSettingValueInteger(30, 990, tmpval, 10)
        d_prel_setting = RadioSetting("dtmf_preload_time",
                                      "Pre-Load Time (ms) (D Prel)", val)
        d_prel_setting.set_doc('D Prel: DTMF pre-load time')
        
        # D LIVE
        val = RadioSettingValueBoolean(_mem.live_DTMF_decoder)
        d_live_setting = \
            RadioSetting("live_DTMF_decoder", "Displays DTMF codes"
                         " received in the middle of the screen (D Live)", val)
        d_live_setting.set_doc('D Live: Displays DTMF codes received by radio in the middle of the screen')
        
        val = RadioSettingValueBoolean(_mem.dtmf.permit_remote_kill)
        perm_kill_setting = RadioSetting("dtmf_permit_remote_kill",
                                         "Permit Remote Kill", val)

        tmpval = str(_mem.dtmf.kill_code).upper().strip(
                "\x00\xff\x20")
        for i in tmpval:
            if i in DTMF_CHARS_KILL:
                continue
            else:
                tmpval = "77777"
                break
        if not len(tmpval) == 5:
            tmpval = "77777"
        val = RadioSettingValueString(5, 5, tmpval)
        val.set_charset(DTMF_CHARS_KILL)
        kill_code_setting = RadioSetting("dtmf_kill_code",
                                         "Kill Code (5 chars 0-9 ABCD)", val)

        tmpval = str(_mem.dtmf.revive_code).upper().strip(
                "\x00\xff\x20")
        for i in tmpval:
            if i in DTMF_CHARS_KILL:
                continue
            else:
                tmpval = "88888"
                break
        if not len(tmpval) == 5:
            tmpval = "88888"
        val = RadioSettingValueString(5, 5, tmpval)
        val.set_charset(DTMF_CHARS_KILL)
        rev_code_setting = RadioSetting("dtmf_revive_code",
                                        "Revive Code (5 chars 0-9 ABCD)", val)

        val = RadioSettingValueBoolean(_mem.int_KILLED)
        killed_setting = RadioSetting("int_KILLED", "DTMF Kill Lock", val)

        # ----------------- Scan Lists

        tmpscanl = list_def(_mem.sl.slDef - 1, SCANLIST_SELECT_LIST, 1)
        val = RadioSettingValueList(SCANLIST_SELECT_LIST, None, tmpscanl)
        rs = RadioSetting("slDef", "Default Scan Lists (SList)", val)
        rs.set_doc('SList: Selects which lists are used by the memory scan\n' + \
                    '* LIST [1] to LIST [24]\n' + \
                    '* ALL : All channels (except OFF)\n')
        scanl.append(rs)

        val = RadioSettingValueBoolean(_mem.sl.slPriorEnab)
        rs = RadioSetting("slPriorEnab", "List 1 Priority Channel Scan", val)
        rs.set_doc('List 1 priority: Is this list has priority')
        scanl.append(rs)

        ch_list = []
        for ch in range(1, MR_CHANNELS_MAX + 1):
            ch_list.append("Channel M" + str(ch))
        ch_list.append("None")

        tmpch = list_def(_mem.sl.slPriorCh1, ch_list, 0)
        val = RadioSettingValueList(ch_list, None, tmpch)
        rs = RadioSetting("slPriorCh1", "List 1 Priority Channel 1", val)
        rs.set_doc('List 1 priority channel 1: Select the channel you want for priority')
        scanl.append(rs)

        tmpch = list_def(_mem.sl.slPriorCh2, ch_list, 0)
        val = RadioSettingValueList(ch_list, None, tmpch)
        rs = RadioSetting("slPriorCh2", "List 1 Priority Channel 2", val)
        rs.set_doc('List 1 priority channel 2: Select the channel you want for priority')
        scanl.append(rs)

        # List names (24 entries)
        for i in range(MR_CHANNELS_LIST - 1):
            # Get the character array object from memory
            name_obj = _mem.listname[i].name
            
            valid_bytes = []
            # Iterate through each element in the character array
            for char_element in name_obj:
                # Explicitly cast the charDataElement to an integer to avoid TypeError
                val = int(char_element)
                
                # Filter for printable ASCII characters only (32-126)
                if 32 <= val <= 126:
                    valid_bytes.append(val)
            
            # Decode the valid bytes to a string and strip whitespace
            listname = bytes(valid_bytes).decode('ascii', errors='ignore').strip()

            # Create the CHIRP setting object
            val = RadioSettingValueString(0, 3, listname)
            listname_setting = RadioSetting(
                f"listname{i}", 
                f"Scan List Name {i+1}", 
                val
            )
            listname_setting.set_doc(
                f'Name for scan list {i+1}\n'
                f'Maximum 3 characters'
            )
            scanl.append(listname_setting)

    # basic.append(listname_setting)

        # ----------------- Basic settings

        ch_list = []
        for ch in range(1, MR_CHANNELS_MAX + 1):
            ch_list.append("Channel M" + str(ch))
        for bnd in range(1, 8):
            ch_list.append("Band F" + str(bnd))
        if _mem.BUILD_OPTIONS.ENABLE_NOAA:
            for bnd in range(1, 11):
                ch_list.append("NOAA N" + str(bnd))

        tmpfreq0 = list_def(_mem.ScreenChannel_A, ch_list, 0)
        val = RadioSettingValueList(ch_list, None, tmpfreq0)
        freq0_setting = RadioSetting("VFO_A_chn",
                                     "VFO A Current Channel/Band", val)
        freq0_setting.set_doc('VFO A current channel/band: To select what is displayed on the VFO A\n' + \
                              '* CHANNEL number M1-M1024\n' + \
                              '* BAND F1-F7\n' + \
                              'look at the correspondence between memory and frequency in the memory tab')

        tmpfreq1 = list_def(_mem.ScreenChannel_B, ch_list, 0)
        val = RadioSettingValueList(ch_list, None, tmpfreq1)
        freq1_setting = RadioSetting("VFO_B_chn",
                                      "VFO B Current Channel/Band", val)
        freq1_setting.set_doc('VFO B current channel/band: To select what is displayed in the VFO B\n' + \
                              '* CHANNEL number M1-M1024\n' + \
                              '* BAND F1-F7\n' + \
                              'look at the correspondence between memory and frequency in the memory tab')

        tmptxvfo = list_def(_mem.TX_VFO, TX_VFO_LIST, 0)
        val = RadioSettingValueList(TX_VFO_LIST, None, tmptxvfo)
        tx_vfo_setting = RadioSetting("TX_VFO", "Main VFO", val)
        tx_vfo_setting.set_doc('Main VFO: To select which VFO is active, ' + \
                               '( A at the top, B at the bottom ) ')

                                     
        # Set_Low_Power f4hwn
        tmpsetpwr = list_def(_mem.set_pwr, SET_LOW_LIST, 0)
        val = RadioSettingValueList(SET_LOW_LIST, SET_LOW_LIST[tmpsetpwr])
        SetPwrSetting = RadioSetting("set_pwr", "Define Power Value when User selection is selected in POWER (SetPwr)", val)
        SetPwrSetting.set_doc('SetPwr: This is the level TX power when (Power) is set to User,.\n' + \
                              '(see the "Power" column in the memories tab to set the TX level for each channel), ' + \
                              'if the User level is select, it will use this TX power level ')
        
        # Set_Ptt f4hwn
        tmpsetptt = list_def(_mem.set_ptt, SET_PTT_LIST, 0)
        val = RadioSettingValueList(SET_PTT_LIST, SET_PTT_LIST[tmpsetptt])
        SetPttSetting = RadioSetting("set_ptt", "Ptt Mode: Set PTT Key Operating Mode (SetPtt)", val)
        SetPttSetting.set_doc('SetPtt :\n' + \
                              '* CLASSIC : press and hold to transmit, release to stop the transmission.\n' + \
                              '* ONEPUSH : press once to transmit, no need to hold.\n' + \
                              '       Simply press once to start transmission, and press a second time to stop.\n' + \
                              ' No more finger cramps :) ')

        # Set_tot f4hwn
        tmpsettot = list_def(_mem.set_tot, SET_TOT_EOT_LIST, 0)
        val = RadioSettingValueList(SET_TOT_EOT_LIST, SET_TOT_EOT_LIST[tmpsettot])
        SetTotSetting = RadioSetting("set_tot", "Set TX Timeout Indicator (SetTot)", val)
        SetTotSetting.set_doc('SetTot: Indication of the TX timeout\n' + \
                                '* NONE : no information\n' + \
                                '* SOUND : audio information\n' + \
                                '* VISUAL : screen blinking\n' + \
                                '* ALL : audio and screen blinking')

        # Set_eot f4hwn
        tmpseteot = list_def(_mem.set_eot, SET_TOT_EOT_LIST, 0)
        val = RadioSettingValueList(SET_TOT_EOT_LIST, SET_TOT_EOT_LIST[tmpseteot])
        SetEotSetting = RadioSetting("set_eot", "Set End Of Transmission Indicator (SetEot)", val)
        SetEotSetting.set_doc('SetEot: End Of Transmission indication\n' + \
                                '* NONE : no information\n' + \
                                '* SOUND : audio information\n' + \
                                '* VISUAL : screen blinking\n' + \
                                '* ALL : audio and screen blinking')

        # Set_contrast f4hwn
        tmpcontrast = min_max_def(_mem.set_contrast, 0, 15, 11)
        val = RadioSettingValueInteger(0, 15, tmpcontrast)
        contrastSetting = RadioSetting("set_contrast", "Set Contrast Level (SetCtr)", val)
        contrastSetting.set_doc('SetCtr: Set the display contrast level from 0 to 15, default is 10')
       
        # Set_inv f4hwn
        tmpsetinv = list_def(_mem.set_inv, SET_OFF_ON_LIST, 0)
        val = RadioSettingValueList(SET_OFF_ON_LIST, SET_OFF_ON_LIST[tmpsetinv])
        SetInvSetting = RadioSetting("set_inv", "Invert Display (SetInv)", val)
        SetInvSetting.set_doc('SetInv: black text on light background or light text on black background\n' + \
                              '* OFF : black text (default)\n' + \
                              '* ON : light text')
        # Set_lck, uses
        tmpsetlck = list_def(_mem.set_lck, SET_LCK_LIST, 0)
        val = RadioSettingValueList(SET_LCK_LIST, SET_LCK_LIST[tmpsetlck])
        SetLckSetting = RadioSetting("set_lck", "Keypad Lock Scope (SetLck)", val)
        SetLckSetting.set_doc('Select which controls are disabled when the keypad is locked:\n' + \
                            '* KEYS\n' + \
                            '* KEYS + ACTIONS\n' + \
                            '* KEYS + PTT\n' + \
                            '* KEYS + ACTIONS + PTT')
        
        # Set_met f4hwn
        tmpsetmet = list_def(_mem.set_met, SET_MET_LIST, 0)
        val = RadioSettingValueList(SET_MET_LIST, SET_MET_LIST[tmpsetmet])
        SetMetSetting = RadioSetting("set_met", "S-Meter Display Style (SetMet)", val)
        SetMetSetting.set_doc('SetMet: Change the style of the S-meter display\n' + \
                              '* CLASSIC : classic display\n' + \
                              '* TINY : smaller display')
        # Set_gui f4hwn
        tmpsetgui = list_def(_mem.set_gui, SET_MET_LIST, 0)
        val = RadioSettingValueList(SET_MET_LIST, SET_MET_LIST[tmpsetgui])
        SetGuiSetting = RadioSetting("set_gui", "Display Text Style (SetGui)", val)
        SetGuiSetting.set_doc('SetGui: Change the display text style\n' + \
                              '* CLASSIC : normal font\n' + \
                              '* TINY : smaller font')

        # Set_tmr f4hwn
        tmpsettmr = list_def(_mem.set_tmr, SET_OFF_ON_LIST, 0)
        val = RadioSettingValueList(SET_OFF_ON_LIST, SET_OFF_ON_LIST[tmpsettmr])
        SetTmrSetting = RadioSetting("set_tmr", "Set Timer (SetTmr)", val)
        SetTmrSetting.set_doc('SetTmr: To enable or disable the timer on transmit and receive (on the left of the status bar...)\n' + \
                              '* OFF : Disable \n' + \
                              '* ON : Enable ')

        # Set_off f4hwn
        tmpsetoff = list_def(_mem.set_off_tmr, SET_OFF_TMR_LIST, 0)
        val = RadioSettingValueList(SET_OFF_TMR_LIST, SET_OFF_TMR_LIST[tmpsetoff])
        SetOffSetting = RadioSetting("set_off_tmr", "Set Off (SetOff)", val)
        SetOffSetting.set_doc('SetOff: Put the radio in battery save, when no RX or keypress \n' + \
                              'To inform you that the radio is in this mode, the Led will flashing in RED \n' + \
                              'the Display will be OFF and hardware go in sleepmode')

        # Set_NFM f4hwn
        tmpsetnfm = list_def(_mem.set_nfm, SET_NFM_LIST, 0)
        val = RadioSettingValueList(SET_NFM_LIST, SET_NFM_LIST[tmpsetnfm])
        SetNFMSetting = RadioSetting("set_nfm", "Set NFM (SetNFM)", val)
        SetNFMSetting.set_doc('SetNFM: Set Narrow FM bandwidth \n' + \
                              '* 12.5 kHz \n' + \
                              '* 6.25 kHz')

        # Set_RXA f4hwn
        tmpsetrxafm = list_def(_mem.set_rxa_fm, SET_RXA_FM_LIST, 0)
        val = RadioSettingValueList(SET_RXA_FM_LIST, SET_RXA_FM_LIST[tmpsetrxafm])
        SetRxAFMSetting = RadioSetting("set_rxa_fm", "Set RxA (SetRxA) FM", val)
        SetRxAFMSetting.set_doc('SetRxA: Set Rx FM Audio profile \n' + \
                              '* FLAT \n' + \
                              '* CLEAN \n' + \
                              '* MID \n' + \
                              '* BOOST \n' + \
                              '* MAX')

        tmpsetrxaam = list_def(_mem.set_rxa_am, SET_RXA_AM_LIST, 0)
        val = RadioSettingValueList(SET_RXA_AM_LIST, SET_RXA_AM_LIST[tmpsetrxaam])
        SetRxAAMSetting = RadioSetting("set_rxa_am", "Set RxA (SetRxA) AM", val)
        SetRxAAMSetting.set_doc('SetRxA: Set Rx FM Audio profile \n' + \
                              '* TIGHT \n' + \
                              '* CLEAN \n' + \
                              '* OPEN')

        # Set_KEY f4hwn
        tmpsetkey = list_def(_mem.set_key, SET_KEY_LIST, 0)
        val = RadioSettingValueList(SET_KEY_LIST, SET_KEY_LIST[tmpsetkey])
        SetKEYSetting = RadioSetting("set_key", "Set KEY (SetKEY)", val)
        SetKEYSetting.set_doc('SetKEY: Set KEY to enable RescueOps mode')


        # Set_Scn f4hwn
        tmpsetscn = list_def(_mem.set_scn, SET_SCN_LIST, 0)
        val = RadioSettingValueList(SET_SCN_LIST, SET_SCN_LIST[tmpsetscn])
        SetScnSetting = RadioSetting("set_scn", "Set Scn (SetScn)", val)
        SetScnSetting.set_doc('SetScn : Set Scan mode\n' + \
                              '* NORMAL : classic scan mode, checks each channel or frequency with the usual full tune.\n' + \
                              '* FAST : faster scan mode using a quick signal precheck before the full tune.')

        # Set_Menu_Lock f4hwn
        tmpsetmenulock = list_def(_mem.set_menu_lock, SET_OFF_ON_LIST, 0)
        val = RadioSettingValueList(SET_OFF_ON_LIST, SET_OFF_ON_LIST[tmpsetmenulock])
        SetMenuLockSetting = RadioSetting("set_menu_lock", "Set RescueOps", val)
        SetMenuLockSetting.set_doc('Enable or disable RescueOps mode')

        tmpval = list_def(_mem.set_nav, SET_NAV_LIST, 1)
        val = RadioSettingValueList(SET_NAV_LIST, None, tmpval)
        SetMenuNavSetting = RadioSetting("set_nav", "Set Navigation", val)
        SetMenuNavSetting.set_doc('Set Nav: set navigation\n' + \
                                    'LEFT/RIGHT (UV-K1)\n ' \
                                    'or UP/DOWN (UV-K5 V3)')

        tmpsq = min_max_def(_mem.squelch, 0, 9, 1)
        val = RadioSettingValueInteger(0, 9, tmpsq)
        squelch_setting = RadioSetting("squelch", "Squelch (Sql)", val)
        squelch_setting.set_doc('Sql: Squelch sensitivity level from 0 to 9, 0 to disable squelch')
        
        ch_list = []
        for ch in range(1, MR_CHANNELS_MAX + 1):
            ch_list.append("Channel M" + str(ch))

        tmpc = list_def(_mem.sl.call_channel, ch_list, 0)
        val = RadioSettingValueList(ch_list, None, tmpc)
        call_channel_setting = RadioSetting("call_channel", "One-Key Call Channel (1 Call)", val)
        call_channel_setting.set_doc('1 Call: One-key call channel, lets you quickly switch to the ' + \
                                     'designed channel with the "9 Call" key ')

        val = RadioSettingValueBoolean(_mem.key_lock)
        keypad_lock_setting = RadioSetting("key_lock", "Keypad Locked", val)
        keypad_lock_setting.set_doc('Keypad locked: Lock the keypad now')
        
        tmpval = list_def(_mem.auto_keypad_lock, AUTO_KEYPAD_LOCK_LIST, 1)
        val = RadioSettingValueList(AUTO_KEYPAD_LOCK_LIST, None, tmpval)
        auto_keypad_lock_setting = RadioSetting("auto_keypad_lock", "Auto Lock Keypad After Delay (KeyLck)", val)
        auto_keypad_lock_setting.set_doc('KeyLck: Keypad lock\n' + \
                                         'OFF (no keypad lock)\n' \
                                         'or Delay (from 15s to 10m) of inactivity')
                                         
        tmptot = list_def(_mem.max_talk_time,  TALK_TIME_LIST, 1)
        val = RadioSettingValueList(TALK_TIME_LIST, None, tmptot)
        tx_t_out_setting = RadioSetting("tot", "Max TX Timeout (TxTOut)", val)
        tx_t_out_setting.set_doc('TxTOut: Select the TX time limit\n' + \
                                 'See option (SetTot) of F4HWN')

        tmpbatsave = list_def(_mem.battery_save, BATSAVE_LIST, 5)
        val = RadioSettingValueList(BATSAVE_LIST, None, tmpbatsave)
        bat_save_setting = RadioSetting("battery_save", "Battery Saver (BatSav)", val)
        bat_save_setting.set_doc('BatSav: Battery saver option, ratio between active time and sleep time')
               
        val = RadioSettingValueBoolean(_mem.noaa_autoscan)
        noaa_auto_scan_setting = RadioSetting("noaa_autoscan", "NOAA Autoscan (NOAA-S)", val)
        noaa_auto_scan_setting.set_doc('NOAA-S: ')
        
        tmpmicgain = list_def(_mem.mic_gain, MIC_GAIN_LIST, 2)
        val = RadioSettingValueList(MIC_GAIN_LIST, None, tmpmicgain)
        mic_gain_setting = RadioSetting("mic_gain", "Mic Gain (Mic)", val)
        mic_gain_setting.set_doc('Mic: Set the microphone sensitivity level (Gain)')
        
        val = RadioSettingValueBoolean(_mem.mic_bar)
        mic_bar_setting = RadioSetting("mic_bar", "Microphone Level Bar Display (MicBar)", val)
        mic_bar_setting.set_doc('MicBar: Display the microphone level bar while transmitting')
        tmpchdispmode = list_def(_mem.channel_display_mode,
                                 CHANNELDISP_LIST, 0)
        val = RadioSettingValueList(CHANNELDISP_LIST, None, tmpchdispmode)
        ch_disp_setting = RadioSetting("channel_display_mode",
                                       "Channel Display Mode (ChDisp)", val)
        ch_disp_setting.set_doc('ChDisp: What to display on screen:\n' + \
                                '* NAME\n' + \
                                '* CHANNEL NUMBER\n' + \
                                '* FREQ\n' + \
                                '* NAME + FREQ')
        tmpdispmode = list_def(_mem.power_on_dispmode, WELCOME_LIST, 0)
        val = RadioSettingValueList(WELCOME_LIST, None, tmpdispmode)
        p_on_msg_setting = RadioSetting("welcome_mode", "Power On Display Message (POnMsg)", val)
        p_on_msg_setting.set_doc('POnMsg: When powering up the radio, what to display:\n' + \
                                 '* ALL : message line 1 + voltage + sound\n' + \
                                 '* SOUND : beep beep 2 only\n' + \
                                 '* MESSAGE : message lines 1 and 2 only\n' + \
                                 '* VOLTAGE : battery voltage only\n' + \
                                 '* NONE : nothing')



        logo1 = str(_mem.logo_line1).strip("\x20\x00\xff") + "\x00"
        logo1 = _getstring(logo1.encode('ascii', errors='ignore'), 0, 12)
        val = RadioSettingValueString(0, 12, logo1)
        logo1_setting = RadioSetting("logo1", "Message Line 1 (12 characters max)", val)
        logo1_setting.set_doc('Message line 1: The first message line,\n' + \
                              'with a maximum of 12 characters\n' + \
                              'See option (POnMsg) to display it')
                               
        logo2 = str(_mem.logo_line2).strip("\x20\x00\xff") + "\x00"
        logo2 = _getstring(logo2.encode('ascii', errors='ignore'), 0, 12)
        val = RadioSettingValueString(0, 12, logo2)
        logo2_setting = RadioSetting("logo2", "Message Line 2 (12 characters max)", val)
        logo2_setting.set_doc('Message line 2: the second message line,\n' + \
                              'with a maximum of 12 characters\n' + \
                              'See option (POnMsg) to display it')

        tmpbattxt = list_def(_mem.battery_text, BAT_TXT_LIST, 2)
        val = RadioSettingValueList(BAT_TXT_LIST, None, tmpbattxt)
        bat_txt_setting = RadioSetting("battery_text", "Battery Level Display (BatTXT)", val)
        bat_txt_setting.set_doc('BatTXT: Display additional battery info on the status bar\n' + \
                                '* PERCENT : Percentage of remaining power\n' + \
                                '* VOLTAGE : Voltage\n' + \
                                '* NONE : Nothing')
        tmpback = list_def(_mem.backlight_time, BACKLIGHT_LIST, 0)
        val = RadioSettingValueList(BACKLIGHT_LIST, None, tmpback)
        back_lt_setting = RadioSetting("backlight_time", "Backlight Time (BLTime)", val)
        back_lt_setting.set_doc('BLTime: Backlight duration, how long the backlight will stay on\n' + \
                                'after the end of an action')


        tmpback = list_def(_mem.backlight_min, BACKLIGHT_LVL_LIST, 0)
        val = RadioSettingValueList(BACKLIGHT_LVL_LIST, None, tmpback)
        bl_min_setting = RadioSetting("backlight_min", "Minimum Backlight Level (BLMin)", val)
        bl_min_setting.set_doc('BLMin: Minimum backlight brightness, when the screen backlight turns off\n' + \
                               'it will dim to this value')


        tmpback = list_def(_mem.backlight_max, BACKLIGHT_LVL_LIST, 10)
        val = RadioSettingValueList(BACKLIGHT_LVL_LIST, None, tmpback)
        bl_max_setting = RadioSetting("backlight_max", "Maximum Backlight Level (BLMax)", val)
        bl_max_setting.set_doc('BLMax: Maximum backlight brightness, when the screen backlight turns on\n' + \
                               'it will light up to this value')

        
        tmpback = list_def(_mem.backlight_on_TX_RX, BACKLIGHT_TX_RX_LIST, 0)
        val = RadioSettingValueList(BACKLIGHT_TX_RX_LIST, None, tmpback)
        blt_trx_setting = RadioSetting("backlight_on_TX_RX", "Backlight on TX/RX (BLTxRx)", val)
        blt_trx_setting.set_doc('BLTxRx : Backlight activation on TX or RX or both TX and RX or no backlight at all\n' + \
                                '* OFF : OFF in all cases\n' + \
                                '* TX : turn ON when TX only\n' + \
                                '* RX : ON when RX only\n' + \
                                '* TX/RX : ON when TX and RX')

        
        val = RadioSettingValueBoolean(_mem.button_beep)
        beep_setting = RadioSetting("button_beep", "Keypad Beep (Beep)", val)
        beep_setting.set_doc('Beep: Beep sound when a key is pressed')


        tmpalarmmode = list_def(_mem.roger_beep, ROGER_LIST, 0)
        val = RadioSettingValueList(ROGER_LIST, None, tmpalarmmode)
        roger_setting = RadioSetting("roger_beep", "End Of Transmission Beep (Roger)", val)
        roger_setting.set_doc('Roger: Squelch tail eliminator, eliminates noise at the end of a transmission')


        val = RadioSettingValueBoolean(_mem.ste)
        ste_setting = RadioSetting("ste", "Squelch Tail Elimination (STE)", val)
        ste_setting.set_doc('STE: Squelch tail eliminator, eliminates noise at the end of a transmission')

        tmprte = list_def(_mem.rp_ste, RTE_LIST, 0)
        val = RadioSettingValueList(RTE_LIST, None, tmprte)
        rp_ste_setting = RadioSetting("rp_ste", "Repeater Squelch Tail Elimination (RP STE)", val)
        rp_ste_setting.set_doc('RP STE: Repeater squelch tail eliminator')

        val = RadioSettingValueBoolean(_mem.AM_fix)
        am_fix_setting = RadioSetting("AM_fix", "AM Reception Fix (AM Fix) ** Has no effect with firmware 3.0 or higher **", val)
        am_fix_setting.set_doc('AM Fix: Activates autogain in AM reception ** Has no effect with firmware 3.0 or higher **')

        tmpvox = min_max_def((_mem.vox_level + 1) * _mem.vox_switch, 0, 10, 0)
        val = RadioSettingValueList(VOX_LIST, None, tmpvox)
        vox_setting = RadioSetting("vox", "Voice-Operated Switch (VOX)", val)
        vox_setting.set_doc('VOX: Voice TX activation sensitivity level')
        
        tmprxmode = list_def((bool(_mem.crossband) << 1)
                             + bool(_mem.dual_watch),
                             RXMODE_LIST, 0)
        val = RadioSettingValueList(RXMODE_LIST, None, tmprxmode)
        rx_mode_setting = RadioSetting("rx_mode", "RX Mode (RX MODE)", val)
        rx_mode_setting.set_doc('RX MODE:\n' + \
                                '* MAIN ONLY : Transmits and listens on the main frequency\n' + \
                                '* DUAL RX RESPOND : Listens both frequencies, if signal received on the secondary frequency, it locks to \n' + \
                                'it for a couple of seconds so you can respond to the call (DWR)\n' + \
                                '* CROSS BAND : Always transmits on the primary and listens on the secondary frequency (XB)\n' + \
                                '* MAIN TX DUAL RX : Always transmits on the primary, listens to both (DW)')

        #val = RadioSettingValueBoolean(_mem.freq_mode_allowed)
        #freq_mode_allowed_setting = RadioSetting("freq_mode_allowed", "Frequency Mode Allowed", val)
        #freq_mode_allowed_setting.set_doc('Frequency mode allowed')


        tmpscanres = list_def(_mem.scan_resume_mode, SCANRESUME_LIST, 0)
        val = RadioSettingValueList(SCANRESUME_LIST, None, tmpscanres)
        scn_rev_setting = RadioSetting("scan_resume_mode", "Scan Resume Mode (ScnRev)", val)
        scn_rev_setting.set_doc('ScnRev: Scan Resume Mode\n' + \
                                '* CARRIER : Listen X seconds after signal disappears and resume\n' + \
                                '* STOP : After receiving a signal, stop the scan\n' + \
                                '* TIMEOUT : Resume scan after X seconds pause')
        
        tmpvoice = list_def(_mem.voice, VOICE_LIST, 0)
        val = RadioSettingValueList(VOICE_LIST, None, tmpvoice)
        voice_setting = RadioSetting("voice", "Voice", val)
        voice_setting.set_doc('Voice: Voice announcement\n' + \
                                '* NONE : No voice\n' + \
                                '* CHINA : Chinese voice\n' + \
                                '* ENGLISH : English voice')

        tmpalarmmode = list_def(_mem.alarm_mode, ALARMMODE_LIST, 0)
        val = RadioSettingValueList(ALARMMODE_LIST, None, tmpalarmmode)
        alarm_setting = RadioSetting("alarm_mode", "Alarm Mode", val)

        # ----------------- Extra settings

       # Upload Advanced Settings checkbox
        val = RadioSettingValueBoolean(False)

        def validate_upload_advanced(value):
            if value and not self.upload_advanced:
                msg = "This will overwrite advanced settings on the radio."
                ret = wx.MessageBox(msg, "Warning", wx.OK | wx.CANCEL |
                                    wx.CANCEL_DEFAULT | wx.ICON_WARNING)
                value = ret == wx.OK
            self.upload_advanced = value
            return value

        val.set_validate_callback(validate_upload_advanced)
        rs = RadioSetting("upload_advanced", "Upload Advanced Settings", val)
        rs.set_doc('Check this box to upload the advanced settings to the radio.\n'
                   'Covers: Battery Type, Navigation, S-meter corrections.\n'
                   'Leave unchecked to preserve existing values on the radio.')
        advanced.append(rs)

        # Battery Type
        tmpbtype = list_def(_mem.Battery_type, BATTYPE_LIST, 0)
        val = RadioSettingValueList(BATTYPE_LIST, BATTYPE_LIST[tmpbtype])
        bat_type_setting = RadioSetting("Battery_type", "Battery Type (BatTyp)", val)
        bat_type_setting.set_doc('BatTyp: What type of battery the radio is using, this affect\n' + \
                                 'the level value of the battery on the display')

        advanced.append(bat_type_setting)
        advanced.append(SetMenuNavSetting)

        # S-meter dBm correction per band
        dBmCorrDefault = [-15, -16, -10, -4, -7, -6, -1]
        band_names = ["Band 1: < 108 MHz", "Band 2: 108 - 137 MHz", "Band 3: 137 - 174 MHz",
                      "Band 4: 174 - 350 MHz", "Band 5: 350 - 400 MHz", "Band 6: 400 - 470 MHz", "Band 7: > 470 MHz"]

        for i, (name, default) in enumerate(zip(band_names, dBmCorrDefault)):
            tmp = int(_mem.dbm_corr[i])
            if tmp not in range(-64, 64):
                tmp = default
            val = RadioSettingValueInteger(-64, 64, tmp)
            rs = RadioSetting(f"dbm_corr_{i}", f"S-meter correction {name} (dBm)", val)
            rs.set_doc(f'S-meter dBm correction for {name}.\n'
                       f'Default: {default} dBm.\n'
                       f'Inject -93 dBm and adjust until display reads -93 dBm.')
            advanced.append(rs)


        # Power on password
#        def validate_password(value):
#            value = value.strip(" ")
#            if value.isdigit():
#                return value.zfill(6)
#            if value != "":
#                raise InvalidValueError("Power on password "
#                                        "can only have digits")
#            return ""

#        pswd_str = str(int(_mem.password)).zfill(6) \
#            if _mem.password < 1000000 else ""
#        val = RadioSettingValueString(0, 6, pswd_str)
#        val.set_validate_callback(validate_password)
#        pswd_setting = RadioSetting("password", "Power on password", val)

        # ----------------- FM radio

        append_label(fmradio, "Channel Memory Radio (MR)", "Frequency (MHz)")

        for i in range(1, FM_CHANNELS_MAX + 1):
            fmfreq = _mem.fmfreq[i-1]/10.0
            freq_name = str(fmfreq)
            if fmfreq < FMMIN or fmfreq > FMMAX:
                freq_name = ""
            rs = RadioSetting("FM_" + str(i), "Ch " + str(i),
                              RadioSettingValueString(0, 5, freq_name))
            rs.set_doc('FM Broadcast frequency: Enter the frequency in MHz, example: 96.9\n' + \
                       'To listen the FM Broadcast band, Long press the 5 key, then if you want to scan for\n' + \
                       'stations around, press *. Scan result will erase the existing FM broadcast list.')
            fmradio.append(rs)

        # ----------------- Unlock settings

        # F-LOCK
        def validate_int_flock(value):
            mem_val = self._memobj.int_flock
            if mem_val != 10 and value == FLOCK_LIST[10]:
                msg = "\"" + value + "\" can only be enabled from radio menu"
                raise InvalidValueError(msg)
            return value

        tmpflock = list_def(_mem.int_flock, FLOCK_LIST, 0)
        val = RadioSettingValueList(FLOCK_LIST, None, tmpflock)
        val.set_validate_callback(validate_int_flock)
        f_lock_setting = RadioSetting("int_flock", "TX Frequency Lock (F Lock)", val)
        f_lock_setting.set_doc('F Lock: Sets the TX frequency band plan')
        
        val = RadioSettingValueBoolean(_mem.int_350en)
        en350_setting = RadioSetting("int_350en", "Unlock 350-400 MHz RX (350 En)", val)
        en350_setting.set_doc('Enables reception on 350 MHz')

        # ----------------- Driver Info

        if self.FIRMWARE_VERSION == "":
            _img_version = self._eeprom_version_string()
            if _img_version:
                firmware = "%s  (recorded in this image, not read live)" % _img_version
            else:
                firmware = "To get the firmware version please download " \
                           "the image from the radio first"
        else:
            firmware = self.FIRMWARE_VERSION

        append_label(roinfo,
                     "=" * 6 + " Firmware F4HWN " + "=" * 300, "=" * 300)

        append_label(roinfo, "Firmware Version", firmware)

#       message box to the web page link (firmware)

        val = RadioSettingValueBoolean(False)
        def validate_Go_Web_Page0(value):
            if value :
                msg = "Go to the web Page of the Firmware F4HWN \n" \
                      + FIRMWARE_VERSION_UPDATE
                ret = wx.MessageBox(msg, "Warning", wx.OK | wx.CANCEL |
                                    wx.CANCEL_DEFAULT | wx.ICON_WARNING)
                if ret == wx.OK :
                    webbrowser.open(FIRMWARE_VERSION_UPDATE)
                value = False
                    
            return value

        val.set_validate_callback(validate_Go_Web_Page0)
        rs = RadioSetting("Update_Firmware_0","Go to the web page of Latest Firmware F4HWN select this Box ->", val)
        rs.set_doc('Be sure you have the latest firmware available!')
        roinfo.append(rs)
        
        val = RadioSettingValueString(0,75,FIRMWARE_VERSION_UPDATE)
        rs = RadioSetting("Update_Firmware_1","Or copy this link (CTRL-C), paste (CTRL-V) to your browser -> ", val)
        rs.set_doc('Be sure you have the latest firmware available!')
        roinfo.append(rs)
        append_label(roinfo, "The Firmware is done by F4HWN","")
        append_label(roinfo,"","")
                
        append_label(roinfo,
                     "=" * 6 + " Chirp Driver F4HWN " + "=" * 300, "=" * 300)
                     
        append_label(roinfo, "Driver Chirp Version         ", DRIVER_VERSION)
        
#       message box to the web page link (chirp)

        val = RadioSettingValueBoolean(False)
        def validate_Go_Web_Page(value):
            if value :
                msg = "Go to the web Page of the Chirp Driver F4HWN \n" \
                      + CHIRP_DRIVER_VERSION_UPDATE
                ret = wx.MessageBox(msg, "Warning", wx.OK | wx.CANCEL |
                                    wx.CANCEL_DEFAULT | wx.ICON_WARNING)
                if ret == wx.OK :
                    webbrowser.open(CHIRP_DRIVER_VERSION_UPDATE)
                value = False
                    
            return value

        val.set_validate_callback(validate_Go_Web_Page)
        rs = RadioSetting("Update_Driver_Chirp_0","Go to the web page of Latest chirp Driver F4HWN select this Box ->", val)
        rs.set_doc('Be sure you have the latest CHIRP driver available!')
        roinfo.append(rs)

        val = RadioSettingValueString(0,75,CHIRP_DRIVER_VERSION_UPDATE)
        rs = RadioSetting("Update_Driver_Chirp_1","Or copy this link (CTRL-C), paste (CTRL-V) to your browser -> ", val)
        rs.set_doc('Be sure you have the latest CHIRP driver available!')
        roinfo.append(rs)
        
        append_label(roinfo, "The driver module is done by VE2ZJM & F4HWN")
        append_label(roinfo,"","" )
        
        # ----------------- Calibration

        val = RadioSettingValueBoolean(False)

        def validate_upload_calibration(value):
            if value and not self.upload_calibration:
                msg = "This option may break your radio!!!\n" \
                    "You are doing this at your own risk.\n" \
                    "Make sure you have a working calibration backup.\n" \
                    "Don't use it unless you know what you're doing."
                ret = wx.MessageBox(msg, "Warning", wx.OK | wx.CANCEL |
                                    wx.CANCEL_DEFAULT | wx.ICON_WARNING)
                value = ret == wx.OK
            self.upload_calibration = value
            return value

        val.set_validate_callback(validate_upload_calibration)
        radio_setting = RadioSetting("upload_calibration",
                                     "Upload Calibration", val)
        radio_setting.set_doc('To Upload only the setting in the calibration section to the radio, ' + \
                              'you need to check this box, then upload to radio.')
        calibration.append(radio_setting)

        radio_setting_group = RadioSettingGroup("squelch_calibration",
                                                "Squelch")
        calibration.append(radio_setting_group)

        bands = {"sqlBand1_3": "Frequency Band 1-3",
                 "sqlBand4_7": "Frequency Band 4-7"}
        for bnd, bndn in bands.items():
            append_label(radio_setting_group,
                         "=" * 6 + " " + bndn + " " + "=" * 300, "=" * 300)
            for sql in range(0, 10):
                prefix = "_mem.cal." + bnd + "."
                postfix = "[" + str(sql) + "]"
                append_label(radio_setting_group, "Squelch " + str(sql))

                name = prefix + "openRssiThr" + postfix
                tempval = min_max_def(eval(name), 0, 255, 0)
                val = RadioSettingValueInteger(0, 255, tempval)
                radio_setting = RadioSetting(name, "Open RSSI Threshold", val)
                radio_setting_group.append(radio_setting)

                name = prefix + "closeRssiThr" + postfix
                tempval = min_max_def(eval(name), 0, 255, 0)
                val = RadioSettingValueInteger(0, 255, tempval)
                radio_setting = RadioSetting(name, "Close RSSI Threshold", val)
                radio_setting_group.append(radio_setting)

                name = prefix + "openNoiseThr" + postfix
                tempval = min_max_def(eval(name), 0, 127, 0)
                val = RadioSettingValueInteger(0, 127, tempval)
                radio_setting = RadioSetting(name, "Open Noise Threshold", val)
                radio_setting_group.append(radio_setting)

                name = prefix + "closeNoiseThr" + postfix
                tempval = min_max_def(eval(name), 0, 127, 0)
                val = RadioSettingValueInteger(0, 127, tempval)
                radio_setting = RadioSetting(name, "Close Noise Threshold", val)
                radio_setting_group.append(radio_setting)

                name = prefix + "openGlitchThr" + postfix
                tempval = min_max_def(eval(name), 0, 255, 0)
                val = RadioSettingValueInteger(0, 255, tempval)
                radio_setting = RadioSetting(name, "Open Glitch Threshold",
                                             val)
                radio_setting_group.append(radio_setting)

                name = prefix + "closeGlitchThr" + postfix
                tempval = min_max_def(eval(name), 0, 255, 0)
                val = RadioSettingValueInteger(0, 255, tempval)
                radio_setting = RadioSetting(name, "Close Glitch Threshold",
                                             val)
                radio_setting_group.append(radio_setting)

        radio_setting_group = RadioSettingGroup("rssi_level_calibration",
                                                "RSSI Levels")
        calibration.append(radio_setting_group)

        bands = {"rssiLevelsBands1_2": "1-2 ", "rssiLevelsBands3_7": "3-7 "}
        for bnd, bndn in bands.items():
            append_label(radio_setting_group,
                         "=" * 6 +
                         " RSSI levels for QS original small bar graph, bands "
                         + bndn + "=" * 300, "=" * 300)
            for lvl in [1, 2, 4, 6]:
                name = "_mem.cal." + bnd + ".level" + str(lvl)
                tempval = min_max_def(eval(name), 0, 65535, 0)
                val = RadioSettingValueInteger(0, 65535, tempval)
                radio_setting = RadioSetting(name, "Level " + str(lvl), val)
                radio_setting_group.append(radio_setting)

#

        radio_setting_group = RadioSettingGroup("tx_power_calibration",
                                                "TX power")
        calibration.append(radio_setting_group)

        for bnd in range(0, 7):
            append_label(radio_setting_group, "=" * 6 + " TX power band "
                         + str(bnd+1) + " " + "=" * 300, "=" * 300)
            powers = {"low": "Low", "mid": "Medium", "hi": "High"}
            for pwr, pwrn in powers.items():
                append_label(radio_setting_group, pwrn)
                bounds = ["lower", "center", "upper"]
                for bound in bounds:
                    name = f"_mem.cal.txp[{bnd}].{pwr}.{bound}"
                    tempval = min_max_def(eval(name), 0, 255, 0)
                    val = RadioSettingValueInteger(0, 255, tempval)
                    radio_setting = RadioSetting(name, bound.capitalize(), val)
                    radio_setting_group.append(radio_setting)

#

        radio_setting_group = RadioSettingGroup("battery_calibration",
                                                "Battery")
        calibration.append(radio_setting_group)

        for lvl in range(0, 6):
            name = "_mem.cal.batLvl[" + str(lvl) + "]"
            temp_val = min_max_def(eval(name), 0, 4999, 4999)
            val = RadioSettingValueInteger(0, 4999, temp_val)
            radio_setting = \
                RadioSetting(name, "Level " + str(lvl) +
                             (" (voltage calibration)" if lvl == 3 else ""),
                             val)
            radio_setting_group.append(radio_setting)

        radio_setting_group = RadioSettingGroup("vox_calibration", "Vox Calibration")
        calibration.append(radio_setting_group)

        for lvl in range(0, 10):
            append_label(radio_setting_group, "Level " + str(lvl + 1))

            name = "_mem.cal.vox1Thr[" + str(lvl) + "]"
            val = RadioSettingValueInteger(0, 65535, eval(name))
            radio_setting = RadioSetting(name, "On", val)
            radio_setting_group.append(radio_setting)

            name = "_mem.cal.vox0Thr[" + str(lvl) + "]"
            val = RadioSettingValueInteger(0, 65535, eval(name))
            radio_setting = RadioSetting(name, "Off", val)
            radio_setting_group.append(radio_setting)

        radio_setting_group = RadioSettingGroup("mic_calibration",
                                                "Microphone Sensitivity")
        calibration.append(radio_setting_group)

        for lvl in range(0, 5):
            name = "_mem.cal.micLevel[" + str(lvl) + "]"
            tempval = min_max_def(eval(name), 0, 31, 31)
            val = RadioSettingValueInteger(0, 31, tempval)
            radio_setting = RadioSetting(name, "Level " + str(lvl), val)
            radio_setting_group.append(radio_setting)

        radio_setting_group = RadioSettingGroup("other_calibration", "Other")
        calibration.append(radio_setting_group)

        name = "_mem.cal.xtalFreqLow"
        temp_val = min_max_def(eval(name), -1000, 1000, 0)
        val = RadioSettingValueInteger(-1000, 1000, temp_val)
        radio_setting = RadioSetting(name, "Xtal Frequency Low", val)
        radio_setting_group.append(radio_setting)

        name = "_mem.cal.volumeGain"
        temp_val = min_max_def(eval(name), 0, 63, 58)
        val = RadioSettingValueInteger(0, 63, temp_val)
        radio_setting = RadioSetting(name, "Volume Gain", val)
        radio_setting_group.append(radio_setting)

        name = "_mem.cal.dacGain"
        temp_val = min_max_def(eval(name), 0, 15, 8)
        val = RadioSettingValueInteger(0, 15, temp_val)
        radio_setting = RadioSetting(name, "DAC Gain", val)
        radio_setting_group.append(radio_setting)



        # ----------------- Help User

        help_group = RadioSettingGroup("Explain_Display_HELP",
                                                "Documentation Display")
        help_user.append(help_group)

        append_label(help_group, "=" * 6 + " Documentation Display " + "=" * 50, "=" * 6  + " Location on display " + "=" * 50)
                         
        append_label(help_group,"Status Line", "First line on the display, on the top of the display" )
        append_label(help_group,"(PS) = Power Save", "Status line" )
        append_label(help_group,"(S) = Scan active", "Status line" )
        append_label(help_group,"(0) = Scan memories that are not on any scan list", "On the right side of the VFO" )
        append_label(help_group,"(1) = Scan memories on list 1", "On the right side of the VFO" )
        append_label(help_group,"(2) = Scan memories on list 2", "On the right side of the VFO" )
        append_label(help_group,"(3) = Scan memories on list 3", "On the right side of the VFO" )
        append_label(help_group,"(1 2 3) = Scan ALL memories", "On the right side of the VFO"  )
     
        append_label(help_group,"(MO) = Main only, display only one VFO", "Status line" )
        append_label(help_group,"(DWR) = Dual RX respond", "Status line" )
        append_label(help_group,"(DW) = Main TX dual RX", "Status line" )
        append_label(help_group,"(XD) = Cross Band", "Status line" )
        append_label(help_group,"(VX) = VOX active", "Status line" )
        append_label(help_group,"(CL) = PTT classic mode" , "Status line" )
        append_label(help_group,"(OP) = PTT one push mode" , "Status line" )
        append_label(help_group,"(F) = Activates the second function of a key" , "Status line" )

        append_label(help_group,"" , "" )

        append_label(help_group,"(Dark Arrow) = Active VFO" , "On the left side of the VFO" )
        append_label(help_group,"(Mxxx) = Memory number M1-M200", "On the left side of the VFO" )
        append_label(help_group,"(Fx) =  Band number F1-F7 " , "On the left side of the VFO" )
        append_label(help_group,"(TX) = VFO used when TX" , "On the left side of the VFO" )
        append_label(help_group,"(RX) = Radio receiving" , "On the left side of the VFO" )

        append_label(help_group,"" , "" )
        append_label(help_group, "=" * 6 + " if (SetGui) is set on CLASSIC " + "=" * 50, "=" * 6  + "=" * 100)
        append_label(help_group,"(FM,USB,AM) = Reception mode" , "Under each VFO" )
        append_label(help_group,"(DC) = RX DCS active" , "Under each VFO" )
        append_label(help_group,"(CT) = RX CTCSS active" , "Under each VFO" )
        append_label(help_group,"(H) = High TX power" , "Under each VFO" )
        append_label(help_group,"(M) = Medium TX power" , "Under each VFO" )
        append_label(help_group,"(Lx) = Low TX power, with x = level 1 to 5" , "Under each VFO" )
        append_label(help_group,"(W) = Wide BF filter: 12.5kHz (WIDE)" , "Under each VFO" )
        append_label(help_group,"(N) = Narrow BF filter: 6.25kHz (NARROW)" , "Under each VFO" )

        append_label(help_group,"" , "" )

        append_label(help_group, "=" * 6 + " If (SetGui) is set on TINY " + "=" * 50, "=" * 6  + "=" * 100)

        append_label(help_group,"(FM,USB,AM) = Reception Mode" , "Under each VFO" )
        append_label(help_group,"(HIGH) = High TX power" , "Under each VFO" )
        append_label(help_group,"(MID) = Medium TX power" , "Under each VFO" )
        append_label(help_group,"(LOWx) = Low TX power, with x = level 1 to 5" , "Under each VFO" )
        append_label(help_group,"(CT x) = RX CTCSS active, with x = value" , "Under each VFO" )
        append_label(help_group,"(DC x) = RX CTCSS active, with x = value" , "Under each VFO" )
        append_label(help_group,"(x.xxK) = no incrementation of the frequency" , "Under each VFO" )
        append_label(help_group,"(WIDE) = Wide BF filter: 12.5kHz" , "Under each VFO" )
        append_label(help_group,"(NAR) = Narrow BF filter: 6.25kHz" , "Under each VFO" )
               
        append_label(help_group,"" , "" )

        append_label(help_group, "=" * 6 + "=" * 100, "=" * 6  + "=" * 100)
       
        append_label(help_group,"(SQLx) = Squelch level, with x = value from 1 to 9" , "Displayed below each VFO" )
        append_label(help_group,"(MONI) = RX monitoring (squelch disabled)" , "Displayed below each VFO" )
       
        help_group = RadioSettingGroup("Explain_Keyboard_HELP", "Keypad documentation" )
        help_user.append(help_group)
        append_label(help_group, "=" * 6 + " Keypad and buttons' function " + "=" * 50, "=" * 6  + " When pressing (F #) before the key " + "=" * 50)
                                                   
        append_label(help_group,"(1 BAND) = long press: change the active frequency band (F1 to F7)  " , "Same as long press  " )
        append_label(help_group,"(1 BAND) = short press: digit 1  "," " )
        append_label(help_group,"","" )

        append_label(help_group,"(2 A/B) = long press: change the active VFO, A(top) / B(bottom) " , "Same as long press " )
        append_label(help_group,"(2 A/B) = short press: digit 2  "," " )

        append_label(help_group,"","" )
        
        append_label(help_group,"(3 VFO MEM) = long press: toggle between VFO and memory " , "Same as long press  " )
        append_label(help_group,"(3 VFO MEM) = short press: digit 3  "," " )

        append_label(help_group,"","" )
        
        append_label(help_group,"(* SCAN) = long press: if in VFO mode, start a scan ", "Make a scan list from the active channel" )
        append_label(help_group,"(* SCAN) = long press, if in memory mode: = switch scan list (1,2, ALL) ", "Make a scan list from the active channel" )
        append_label(help_group,"(* SCAN) = short press:  DTMF input mode (S) ", "Start CTCSS listening on the current frequency" )

        append_label(help_group,"","" )
        append_label(help_group,"(4 FC) = long press: start the CTCSS & frequency scan " , "Same as long press " )
        append_label(help_group,"(4 FC) = short press: digit 4  "," " )


        append_label(help_group,"","" ) 
        
        append_label(help_group,"(5 NOAA) = long press: if in VFO mode: display ScnRnG " , "Display fagci spectrum analyze " )
        append_label(help_group,"(5 NOAA) = long press: if in memory mode,  change the scan list " , " ")
        append_label(help_group,"(5 NOAA) = short press: digit 5  "," " )

        append_label(help_group,"","" )
                
        append_label(help_group,"(6 H/M/L) = long press: change the TX power level (High/Medium/Low) of the active VFO " , "Same as long press " )
        append_label(help_group,"(6 H/M/L) = short press: digit 6  "," " )

        append_label(help_group,"","" )
                
        append_label(help_group,"(0 FM) = long press: FM Broadcast listening ", "Same as long press " )
        append_label(help_group,"(0 FM) = short press: digit 0  "," " )

        append_label(help_group,"","")
        
        append_label(help_group,"(7 VOX) = long press: change voice Activation (VX) " , "Same as long press " )
        append_label(help_group,"(7 VOX) = short press: digit 7  "," " )

        append_label(help_group,"","" )
                
        append_label(help_group,"(8 R) = long press: reverse (if using a shift) (R) " , "Same as long press ")
        append_label(help_group,"(8 R) = short press: digit 8  ","Force backlight on/off " )
        append_label(help_group,"","Disable backlight timeout (BLTime)")

        append_label(help_group,"","" )
        
        append_label(help_group,"(9 CALL) = long press: switch current channel to 1-Call channel" , "Same as long press")
        append_label(help_group,"(9 CALL) = short press: digit 9","Enable backlight timeout (BLTime)" )

        append_label(help_group,"","" )
        
        append_label(help_group,"(F #) = long, press: keypad lock/unlock", )
        append_label(help_group,"(F #) = short press: activates the secondary button function", )

        append_label(help_group,"","" )
        
        append_label(help_group,"(M A) = long press: user programmable button","" )
        append_label(help_group,"(M A) = short press: Menu access","" )

        append_label(help_group,"","" )
        
        append_label(help_group,"(Arrow UP B) = long press: scroll VFO/MEM upwards","" )
        append_label(help_group,"(Arrow UP B) = short press: increase VFO/MEM value ","increase squelch value" )

        append_label(help_group,"","" )
        append_label(help_group,"(Arrow DOWN C) = long press: scroll VFO/MEM downwards","" )
        append_label(help_group,"(Arrow DOWN C) = short press: decrease VFO/MEM value","decrease squelch value" )

        append_label(help_group,"","" )
        
        append_label(help_group, "=" * 6 + " side button" + "=" * 50, "=" * 6  + "" + "=" * 50)

        append_label(help_group,"","" )
        
        append_label(help_group,"(PTT)  = long press: transmit on active TX","" )
        append_label(help_group,"(PTT)  = short press: transmit on active TX","" )

        append_label(help_group,"","" )
        
        append_label(help_group,"(.)  = long press: user programmable button","" )
        append_label(help_group,"(.)  = short press, user programmable button","" )

        append_label(help_group,"","" )
        append_label(help_group,"(..)  = long press, user programmable button","" )
        append_label(help_group,"(..)  = short press, user programmable button","" )

        append_label(help_group,"","" )
        
        append_label(help_group, "=" * 6 + " special function " + "=" * 50, "=" * 6  + "" + "=" * 50)

        append_label(help_group,"","" )
        
        append_label(help_group,"To put the radio in programming MODE firmware","Hold PTT key then switch on the radio, the flashlight will light up" )

        append_label(help_group,"","" )
        
        append_label(help_group,"To activate the hidden menu","Hold the PTT key and the button just below it then turn on the radio" )
        append_label(help_group,"", "The hidden items are at the end of the regular menu" )

        
        help_group = RadioSettingGroup("Chirp_Language_HELP",
                                       "How to change CHIRP language")
        help_user.append(help_group)
        
        append_label(help_group,"It's now possible to change the language directly in CHIRP ", )
        append_label(help_group,"Go on the View tab, then Language... You will need to restart chirp to get the new language ", )

        # -------- LAYOUT
        append_label(basic,
                     "=" * 6 + " Start of F4HWN Settings." + "=" * 300, "=" * 300)
        basic.append(SetPwrSetting)
        basic.append(SetPttSetting)
        basic.append(SetTotSetting)
        basic.append(SetEotSetting)
        basic.append(contrastSetting)
        basic.append(SetInvSetting)
        basic.append(SetLckSetting)
        basic.append(SetMetSetting)
        basic.append(SetGuiSetting)
        basic.append(SetTmrSetting)
        basic.append(SetOffSetting)
        basic.append(SetNFMSetting)
        basic.append(SetRxAFMSetting)
        basic.append(SetRxAAMSetting)
        if _mem.BUILD_OPTIONS.ENABLE_FEAT_F4HWN_RESCUE_OPS:
            basic.append(SetKEYSetting)
        basic.append(SetScnSetting)
        if _mem.BUILD_OPTIONS.ENABLE_FEAT_F4HWN_RESCUE_OPS:
            basic.append(SetMenuLockSetting)

        append_label(basic,
                     "=" * 6 + " End of F4HWN settings " + "=" * 300, "=" * 300)

        append_label(basic,
                     "=" * 6 + " General settings " + "=" * 300, "=" * 300)

        basic.append(squelch_setting)
        basic.append(rx_mode_setting)
        basic.append(call_channel_setting)
        basic.append(auto_keypad_lock_setting)
        basic.append(tx_t_out_setting)
        basic.append(bat_save_setting)
        basic.append(scn_rev_setting)
        
        if _mem.BUILD_OPTIONS.ENABLE_NOAA:
            basic.append(noaa_auto_scan_setting)
        if _mem.BUILD_OPTIONS.ENABLE_AM_FIX:
            basic.append(am_fix_setting)

        append_label(basic,
                     "=" * 6 + " Display settings " + "=" * 300, "=" * 300)

        basic.append(bat_txt_setting)
        basic.append(mic_bar_setting)
        basic.append(ch_disp_setting)
        basic.append(p_on_msg_setting)
        basic.append(logo1_setting)
        basic.append(logo2_setting)

        append_label(basic, "=" * 6 + " Backlight settings "
                     + "=" * 300, "=" * 300)

        basic.append(back_lt_setting)
        basic.append(bl_min_setting)
        basic.append(bl_max_setting)
        basic.append(blt_trx_setting)

        append_label(basic, "=" * 6 + " Audio related settings "
                     + "=" * 300, "=" * 300)

        if _mem.BUILD_OPTIONS.ENABLE_VOX:
            basic.append(vox_setting)
        basic.append(mic_gain_setting)
        basic.append(beep_setting)
        basic.append(roger_setting)
        basic.append(ste_setting)
        basic.append(rp_ste_setting)
        if _mem.BUILD_OPTIONS.ENABLE_VOICE:
            basic.append(voice_setting)
        if _mem.BUILD_OPTIONS.ENABLE_ALARM:
            basic.append(alarm_setting)

        append_label(basic, "=" * 6 + " Radio state " + "=" * 300, "=" * 300)

        basic.append(freq0_setting)
        basic.append(freq1_setting)
        basic.append(tx_vfo_setting)
        basic.append(keypad_lock_setting)

#        advanced.append(freq_mode_allowed_setting)
#        if _mem.BUILD_OPTIONS.ENABLE_PWRON_PASSWORD:
#            advanced.append(pswd_setting)

#        if _mem.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#            dtmf.append(sep_code_setting)
#            dtmf.append(group_code_setting)
        dtmf.append(first_code_per_setting)
        dtmf.append(spec_per_setting)
        dtmf.append(code_per_setting)
        dtmf.append(code_int_setting)
#        if _mem.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#            dtmf.append(ani_id_setting)
        dtmf.append(up_code_setting)
        dtmf.append(dw_code_setting)
        dtmf.append(d_prel_setting)
        dtmf.append(dtmf_side_tone_setting)
        dtmf.append(d_live_setting)
        
#        if _mem.BUILD_OPTIONS.ENABLE_DTMF_CALLING:
#            dtmf.append(dtmf_resp_setting)
#            dtmf.append(d_hold_setting)
#            dtmf.append(perm_kill_setting)
#            dtmf.append(kill_code_setting)
#            dtmf.append(rev_code_setting)
#            dtmf.append(killed_setting)

        unlock.append(f_lock_setting)
        unlock.append(en350_setting)      

        return top

    def set_memory(self, memory):
        """
        Store details about a high-level memory to the memory map
        This is called when a user edits a memory in the UI
        """
        number = memory.number-1
        att_num = number if number < MR_CHANNELS_MAX else MR_CHANNELS_MAX + int((number - MR_CHANNELS_MAX) / 2)

        # Get a low-level memory object mapped to the image
        # Access the correct structure based on channel type
        if number < MR_CHANNELS_MAX:
            # Regular memory channel (0-249)
            _mem_chan = self._memobj.channel[number]
        else:
            # VFO channel (250-263) -> vfo_channel[0-13] at 0x0fa0
            vfo_index = number - MR_CHANNELS_MAX
            _mem_chan = self._memobj.vfo_channel[vfo_index]
        
        _mem_attr = self._memobj.ch_attr[att_num]

        # Initialize scanlist fields (new 5-bit field and old 1-bit flags)
        _mem_attr.scanlist = 0
        _mem_attr.compander = 0

        # empty memory
        if memory.empty:
            _mem_chan.set_raw(b"\xFF" * 16)

            if number < MR_CHANNELS_MAX:
                _mem_chname = self._memobj.channelname[number]
                _mem_chname.set_raw(b"\x20" * 16)

                # deleted marker: 0xFFFF
                _mem_attr = self._memobj.ch_attr[number]   # ou att_num, ici c'est pareil pour MR
                _mem_attr.set_raw(b"\xFF\xFF")
            
            return memory

        # find band
        band = self._find_band(memory.freq)

        # ---- NR7Y: mode by table, and never pin the bandwidth bit --------
        # Upstream forced bandwidth = 1 for USB, which made the bit unusable as
        # a filter selector. Under EEPROM schema 2 that bit is the user's choice
        # (wide = 6.25k, narrow = 2.0k on CW/SSB), so USB and CW take it from
        # the Bandwidth extra instead - see _nr7y_apply_mode().
        self._nr7y_apply_mode(_mem_chan, memory)

        # frequency/offset
        _mem_chan.freq = memory.freq/10
        _mem_chan.offset = memory.offset/10

        if memory.duplex == "":
            _mem_chan.offsetDir = FLAGS1_OFFSET_NONE
        elif memory.duplex == '-':
            _mem_chan.offsetDir = FLAGS1_OFFSET_MINUS
        elif memory.duplex == '+':
            _mem_chan.offsetDir = FLAGS1_OFFSET_PLUS

        # set band

#        _mem_attr.is_free = 0
        _mem_attr.band = band

        # channels >200 are the 14 VFO chanells and don't have names
        if number < MR_CHANNELS_MAX:
            _mem_chname = self._memobj.channelname[number]
            tag = memory.name.ljust(10) + "\x00"*6
            _mem_chname.name = tag  # Store the alpha tag

        # tone data
        self._set_tone(memory, _mem_chan)

        # step
        _mem_chan.step = STEPS.index(memory.tuning_step)

        # tx power
        if str(memory.power) == str(UVK5_POWER_LEVELS[7]):
            _mem_chan.txpower = POWER_HIGH
        elif str(memory.power) == str(UVK5_POWER_LEVELS[6]):
            _mem_chan.txpower = POWER_MEDIUM
        elif str(memory.power) == str(UVK5_POWER_LEVELS[5]):
            _mem_chan.txpower = POWER_LOW5
        elif str(memory.power) == str(UVK5_POWER_LEVELS[4]):
            _mem_chan.txpower = POWER_LOW4
        elif str(memory.power) == str(UVK5_POWER_LEVELS[3]):
            _mem_chan.txpower = POWER_LOW3
        elif str(memory.power) == str(UVK5_POWER_LEVELS[2]):
            _mem_chan.txpower = POWER_LOW2
        elif str(memory.power) == str(UVK5_POWER_LEVELS[1]):
            _mem_chan.txpower = POWER_LOW1
        else:
            _mem_chan.txpower = POWER_USER

        # -------- EXTRA SETTINGS

        def get_setting(name, def_val):
            if name in memory.extra:
                return int(memory.extra[name].value)
            return def_val

        _mem_chan.txLock = get_setting("txLock", 0)
        _mem_chan.busyChLockout = get_setting("busyChLockout", False)
        _mem_chan.dtmf_pttid = get_setting("pttid", 0)
        _mem_chan.freq_reverse = get_setting("frev", False)
        _mem_chan.dtmf_decode = get_setting("dtmfdecode", False)
        _mem_attr.compander = get_setting("compander", 0)
        if number < MR_CHANNELS_MAX:
            tmp_val = get_setting("scanlists", 0)
            _mem_attr.scanlist = tmp_val

        return memory


# ================================================================================
# NR7Y Fusion variant – adds CW modulator support to the F4HWN Fusion driver
# ================================================================================

@directory.register
class UVK5_NR7Y_Fusion(UVK5RadioEgzumer):
    """Quansheng UV-K1/K5v3 with F4HWN Fusion + NR7Y CW firmware"""
    VENDOR = "Quansheng"
    MODEL = "UV-K1 & UV-K5 V3 (F4HWN Fusion)"
    VARIANT = "NR7Y"

    # ------------------------------------------------------------------ CW detection

    def _is_nr7y_cw_firmware(self):
        """Return True if firmware was compiled with ENABLE_CW_MODULATOR."""
        try:
            return bool(self._memobj.BUILD_OPTIONS.ENABLE_CW_MODULATOR)
        except Exception as exc:
            LOG.warning("Cannot read ENABLE_CW_MODULATOR: %s", exc)
            return False

    # ------------------------------------------------------------------ features

    def get_features(self):
        rf = super().get_features()
        rf.valid_modes = ["FM", "NFM", "AM", "NAM", "USB", "CW"]
        return rf

    # ------------------------------------------------------------------ memory

    def get_memory(self, number):
        mem = super().get_memory(number)

        _mem_chan = self._nr7y_mem_chan(number)
        if _mem_chan is None or getattr(mem, 'empty', False):
            return mem

        # On CW and SSB the bandwidth bit is not carried by the mode string, so
        # surface it as its own setting. Plain Wide/Narrow: the firmware decides
        # that means 6.25k or 2.0k on these modes, and naming the widths here
        # would only invite confusion with the FM ones.
        if int(_mem_chan.modulation) in (2, 3) and self._is_nr7y_cw_firmware():
            enc = list_def(int(_mem_chan.bandwidth), NR7Y_BANDWIDTH_LIST, 0)
            rs = RadioSetting(
                "nr7y_bandwidth", "Bandwidth",
                RadioSettingValueList(NR7Y_BANDWIDTH_LIST, None, enc))
            rs.set_doc(
                'Receive filter for this CW/SSB channel.\n'
                '* Wide: 6.25 kHz\n'
                '* Narrow: 2.0 kHz')
            if mem.extra is None:
                mem.extra = RadioSettingGroup("Extra", "extra")
            mem.extra.append(rs)

        return mem

    # ------------------------------------------------------------------ schema

    @classmethod
    def nr7y_approve_schema(cls, schema):
        """True if this module understands that EEPROM layout.

        Schema 1 is accepted because it is upgraded in memory on load; anything
        past 2 was written by a newer firmware and belongs to a newer module.
        """
        return schema <= NR7Y_SCHEMA_CURRENT

    # NOTE: no detect_from_serial(), and no detected_by() sub-model either.
    # These two go together: CloneModeRadio.detect_from_serial() asserts that a
    # class with detected models implements its own, so registering one without
    # the other makes every clone fail with "Internal driver error" before the
    # port is even touched (chirp_common.py, detect_from_serial).
    #
    #
    # An earlier version probed the schema byte over the wire before the
    # download so a newer-schema radio could be routed to NR7YRestrictedRadio.
    # It broke downloads outright: clone.py runs detection on the open port
    # before do_download, and the extra hello/readmem exchange left the radio
    # not answering at all - every download died with "Header short read".
    # The firmware drops any command whose timestamp does not match the last
    # hello (uart.c CMD_051B) and answers nothing, which is exactly that
    # symptom.
    #
    # It bought nothing today: no schema past 2 exists, so detection could only
    # ever return this class anyway. The download path is now byte-for-byte the
    # same conversation the working v1.3 module has. When a schema 3 does exist,
    # wire demotion up then - and test it on hardware first.

    def _nr7y_read_schema(self):
        """Schema of the loaded image, normalised. 0xFF means pre-v1.4."""
        try:
            schema = int(self._memobj.schema)
        except Exception as exc:
            LOG.warning("Cannot read EEPROM schema marker: %s", exc)
            return NR7Y_SCHEMA_LEGACY
        # Normalise erased flash first: 0xFF is 255, so comparing it before
        # this would take the "newer schema" branch.
        if schema == 0xFF:
            return NR7Y_SCHEMA_LEGACY
        return schema

    def _nr7y_upgrade_schema(self):
        """Bring a schema 1 image up to schema 2, in memory.

        Same mapping the firmware applies on first boot: on CW and USB records
        the old NARROWEST flag in bit 6 becomes the filter bit, and bit 6 goes
        back to being TX_LOCK, set on. Schema 1 is read-only - this module
        never writes it back.
        """
        migrated = 0

        for record in list(self._memobj.channel) + list(self._memobj.vfo_channel):
            try:
                mod = int(record.modulation)
            except Exception:
                continue
            # An erased record decodes to modulation 0xF and matches neither.
            if mod not in (2, 3):
                continue

            was_narrowest = int(record.txLock) & 1
            record.bandwidth = was_narrowest
            record.txLock = 1
            migrated += 1

        self._memobj.schema = NR7Y_SCHEMA_CURRENT
        self.metadata = {'nr7y_schema': NR7Y_SCHEMA_CURRENT}

        LOG.warning(
            'Image used EEPROM schema %d and has been upgraded to schema %d '
            '(%d CW/SSB channels). Saving this file will store it in the new '
            'layout, which pre-v1.4 firmware cannot read.',
            NR7Y_SCHEMA_LEGACY, NR7Y_SCHEMA_CURRENT, migrated)

    def _check_schema_at_load(self):
        """Gate an image the way mainline uvk5.py gates firmware at load.

        Missing provenance is a warning, not a failure - an image written by an
        older or modified module simply has nothing to check. A schema that is
        present and unsupported raises.
        """
        if 'nr7y_schema' not in self.metadata:
            # The EEPROM version block is still there even when the module's
            # own metadata is not, which is exactly the case this warns about.
            img_version = self._eeprom_version_string()
            LOG.warning('This image has no EEPROM schema information%s. It may '
                        'have been generated with an old or modified version '
                        'of this module. Download a fresh image from your '
                        'radio for the best safety and compatibility.',
                        ' (firmware reports %s)' % img_version
                        if img_version else '')

        schema = self._nr7y_read_schema()
        if not self.nr7y_approve_schema(schema):
            raise errors.RadioError(
                'Image uses EEPROM schema %d, which is not supported by this '
                'module' % schema)

    def process_mmap(self):
        super().process_mmap()
        self._check_schema_at_load()
        if self._nr7y_read_schema() < NR7Y_SCHEMA_CURRENT:
            self._nr7y_upgrade_schema()

    # ------------------------------------------------------------------ key actions helper

    def _nr7y_action_map(self):
        """Return the index -> action-name map for the firmware on the radio.

        Mirrors the conditionally-compiled ACTION_OPT_t enum so the numbers we
        read and write line up with the actions that build actually has.
        """
        try:
            has_rescue_ops = bool(
                self._memobj.BUILD_OPTIONS.ENABLE_FEAT_F4HWN_RESCUE_OPS)
        except Exception as exc:
            LOG.warning("Cannot read ENABLE_FEAT_F4HWN_RESCUE_OPS: %s", exc)
            has_rescue_ops = False
        return _nr7y_keyactions_list(has_rescue_ops,
                                     self._is_nr7y_cw_firmware())

    def _get_nr7y_action(self, action_num):
        """Return (choices_list, current_str) for a programmable key.

        The map from _nr7y_action_map only contains actions the build compiled,
        so the filtering here is purely about which of those to offer.
        """
        base_list = self._nr7y_action_map()

        has_alarm       = self._memobj.BUILD_OPTIONS.ENABLE_ALARM
        has_1750        = self._memobj.BUILD_OPTIONS.ENABLE_TX1750
        has_flashlight  = self._memobj.BUILD_OPTIONS.ENABLE_FLASHLIGHT
        has_fm_radio    = self._memobj.BUILD_OPTIONS.ENABLE_FMRADIO
        has_vox         = self._memobj.BUILD_OPTIONS.ENABLE_VOX

        lst = base_list.copy()
        # Remove TX-only actions that must not appear in key-press menus
        for a in ("BACKLIGHT", "BL_MIN_TMP_OFF"):
            if a in lst:
                lst.remove(a)

        # These keep an enum slot even when compiled out, so they are always in
        # the map and only ever need hiding from the choices.
        if not has_alarm:
            if "ALARM" in lst:
                lst.remove("ALARM")
        if not has_1750:
            if "1750Hz" in lst:
                lst.remove("1750Hz")
        if not has_flashlight:
            if "FLASHLIGHT" in lst:
                lst.remove("FLASHLIGHT")
        if not has_fm_radio:
            if "FM RADIO" in lst:
                lst.remove("FM RADIO")
        if not has_vox:
            if "MUTE" in lst:
                lst.remove("MUTE")

        action_num = int(action_num)
        if action_num >= len(base_list) or base_list[action_num] not in lst:
            action_num = 0
        return lst, base_list[action_num]

    def _build_nr7y_keya_group(self):
        """Rebuild the Programmable Keys group using the NR7Y extended actions list."""
        _mem = self._memobj
        keya = RadioSettingGroup("keya", "Programmable Keys")
        for name, label in (
            ("key1_shortpress_action", "Side Key 1 Short Press (F1Shrt)"),
            ("key1_longpress_action",  "Side Key 1 Long Press (F1Long)"),
            ("key2_shortpress_action", "Side Key 2 Short Press (F2Shrt)"),
            ("key2_longpress_action",  "Side Key 2 Long Press (F2Long)"),
            ("keyM_longpress_action",  "Menu Key Long Press (M Long)"),
        ):
            choices, current = self._get_nr7y_action(int(getattr(_mem, name)))
            keya.append(RadioSetting(name, label,
                                     RadioSettingValueList(choices, current)))
        return keya

    # ------------------------------------------------------------------ CW settings group builder

    def _build_cw_settings_group(self):
        """Return a RadioSettingGroup containing all CW settings."""
        cw = RadioSettingGroup("cw", "CW Settings")

        # Sidetone Frequency (450-950 Hz in 50 Hz steps)
        freq_opts = ["%d Hz" % (450 + i * 50) for i in range(11)]
        try:
            fi = self._get_cw_frequency_idx()
            cw.append(RadioSetting("cw_frequency", "Sidetone Frequency",
                                   RadioSettingValueList(freq_opts, freq_opts[fi])))
        except Exception as e:
            LOG.error("CW freq setting: %s", e)

        # Sidetone Volume (0=OFF, 1-6)
        vol_opts = ["OFF"] + [str(i) for i in range(1, 7)]
        try:
            vi = self._get_cw_sidetone_level()
            cw.append(RadioSetting("cw_sidetone_level", "Sidetone Volume",
                                   RadioSettingValueList(vol_opts, vol_opts[vi])))
        except Exception as e:
            LOG.error("CW vol setting: %s", e)

        # Keyer Mode
        try:
            mi = self._get_cw_keyer_mode()
            cw.append(RadioSetting("cw_keyer_mode", "Keyer Mode",
                                   RadioSettingValueList(_NR7Y_CW_KEYER_MODES,
                                                        _NR7Y_CW_KEYER_MODES[mi])))
        except Exception as e:
            LOG.error("CW keyer mode: %s", e)

        # Keyer Speed (10-45 WPM)
        try:
            wpm = self._get_cw_wpm()
            cw.append(RadioSetting("cw_wpm", "Keyer Speed (WPM)",
                                   RadioSettingValueInteger(10, 45, wpm)))
        except Exception as e:
            LOG.error("CW WPM: %s", e)

        # Key Input Mode
        try:
            ki = self._get_cw_key_input_idx()
            cw.append(RadioSetting("cw_key_input", "Key Input Mode",
                                   RadioSettingValueList(_NR7Y_CW_KEY_INPUT_MODES,
                                                        _NR7Y_CW_KEY_INPUT_MODES[ki])))
        except Exception as e:
            LOG.error("CW key input: %s", e)

        # Break-in
        try:
            breakin_opts = ["OFF", "ON"]
            bi = 1 if self._get_cw_breakin() else 0
            rs_bi = RadioSetting("cw_break_in", "Break-in (CWbkin)",
                                 RadioSettingValueList(breakin_opts, breakin_opts[bi]))
            rs_bi.set_doc("ON: TX during keying. OFF: sidetone only, no RF TX.")
            cw.append(rs_bi)
        except Exception as e:
            LOG.error("CW break-in: %s", e)

        # Message Repeat Delay
        try:
            rd = self._get_cw_repeat_delay()
            rs_rd = RadioSetting("cw_repeat_delay", "Message Repeat Delay (sec)",
                                 RadioSettingValueInteger(0, 127, rd))
            rs_rd.set_doc("Delay between repeated CW message plays (0-127 seconds)")
            cw.append(rs_rd)
        except Exception as e:
            LOG.error("CW repeat delay: %s", e)

        # CW Macros (4 messages)
        macros = RadioSettingGroup("cw_macros", "CW Macros")
        for i in range(1, 5):
            try:
                txt = self._get_cw_msg(i)
                rs_m = RadioSetting("cw_msg%d" % i, "CW Message %d" % i,
                                    RadioSettingValueString(0, 46, txt))
                rs_m.set_doc("CW macro %d (A-Z, 0-9, /, ?, ., ,, -, = only; max 46 chars)" % i)
                macros.append(rs_m)
            except Exception as e:
                LOG.error("CW macro %d: %s", i, e)
        cw.append(macros)
        return cw

    # ------------------------------------------------------------------ get_settings

    def get_settings(self):
        top = super().get_settings()

        # Build a replacement settings tree: swap out the keya group and
        # (when CW firmware is detected) append the CW settings group.
        has_cw = self._is_nr7y_cw_firmware()

        new_top = RadioSettings()
        for group in top:
            try:
                if group.get_name() == 'keya':
                    new_top.append(self._build_nr7y_keya_group())
                    continue
            except AttributeError:
                pass
            new_top.append(group)

        if has_cw:
            new_top.append(self._build_cw_settings_group())
        return new_top

    # ------------------------------------------------------------------ set_settings

    def set_settings(self, settings):
        _mem = self._memobj
        has_cw = self._is_nr7y_cw_firmware()
        # Same map the settings were built from, so a name round-trips to the
        # number this firmware build means by it.
        action_list = self._nr7y_action_map()

        for element in settings:
            if not isinstance(element, RadioSetting):
                # Recurse into groups
                self.set_settings(element)
                continue

            name = element.get_name()

            # Programmable key actions – use the correct actions list
            if name in ("key1_shortpress_action", "key1_longpress_action",
                        "key2_shortpress_action", "key2_longpress_action",
                        "keyM_longpress_action"):
                try:
                    idx = action_list.index(str(element.value))
                except ValueError:
                    idx = 0
                setattr(_mem, name, idx)
                continue

            # CW settings (only present when CW firmware is active)
            if has_cw and name.startswith("cw_"):
                self._apply_cw_setting(name, element)
                continue

            # All other settings: delegate to the base class handler
            super().set_settings([element])

    # ------------------------------------------------------------------ CW setting applier

    def _apply_cw_setting(self, name, element):
        """Dispatch a single cw_* setting to its encode/write helper."""
        try:
            if name == "cw_frequency":
                freq_opts = ["%d Hz" % (450 + i * 50) for i in range(11)]
                self._set_cw_frequency_idx(freq_opts.index(str(element.value)))
            elif name == "cw_sidetone_level":
                vol_opts = ["OFF"] + [str(i) for i in range(1, 7)]
                self._set_cw_sidetone_level(vol_opts.index(str(element.value)))
            elif name == "cw_keyer_mode":
                self._set_cw_keyer_mode(_NR7Y_CW_KEYER_MODES.index(str(element.value)))
            elif name == "cw_wpm":
                self._set_cw_wpm(int(element.value))
            elif name == "cw_key_input":
                self._set_cw_key_input_idx(
                    _NR7Y_CW_KEY_INPUT_MODES.index(str(element.value)))
            elif name == "cw_break_in":
                self._set_cw_breakin(["OFF", "ON"].index(str(element.value)))
            elif name == "cw_repeat_delay":
                self._set_cw_repeat_delay(int(element.value))
            elif name.startswith("cw_msg"):
                # "cw_msg1" → idx 1 … "cw_msg4" → idx 4
                self._set_cw_msg(int(name[6:]), str(element.value))
        except Exception as e:
            LOG.error("Error applying CW setting %s: %s", name, e)

    # ------------------------------------------------------------------ CW byte-level helpers
    # All addresses are in the _mmap (image) coordinate space which is 1:1 with
    # PY25Q16 addresses for the fusion firmware.

    def _mmap_byte(self, addr):
        """Read one byte from the image at the given address."""
        val = self._mmap[addr]
        if isinstance(val, (bytes, bytearray)):
            val = val[0]
        return int(val) & 0xFF

    def _mmap_set(self, addr, byte_val):
        """Write one byte to the image at the given address."""
        self._mmap[addr] = int(byte_val) & 0xFF

    # -- Sidetone frequency (bits 0-3 of CW byte 0) --

    def _get_cw_frequency_idx(self):
        """Return sidetone frequency index 0-10 (450-950 Hz, 50 Hz steps)."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR)
        if b == 0xFF:
            return 3   # Default 600 Hz
        # Firmware stores (Hz/10 − 45) / 5 in bits 0-3
        freq_val = 45 + (b & 0x0F) * 5   # Hz/10
        return max(0, min(10, (freq_val * 10 - 450) // 50))

    def _set_cw_frequency_idx(self, idx):
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR)
        freq_hz = 450 + int(idx) * 50
        encoded = (freq_hz // 10 - 45) // 5
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR, (b & 0xF0) | (encoded & 0x0F))

    # -- Sidetone level (bits 4-6 of CW byte 0) --

    def _get_cw_sidetone_level(self):
        """Return sidetone volume index 0-6 (0=OFF)."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR)
        if b == 0xFF:
            return 4   # Default level 4
        return (b >> 4) & 0x07

    def _set_cw_sidetone_level(self, level):
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR)
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR, (b & 0x0F) | ((int(level) & 0x07) << 4))

    # -- Keyer mode (full byte at CW byte 4) --

    def _get_cw_keyer_mode(self):
        """Return 0=Iambic A, 1=Iambic B, 2=Ultimatic, 3=Semi-Auto Bug."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 4)
        if b >= len(_NR7Y_CW_KEYER_MODES):
            return 1   # Default Mode B (blank/invalid EEPROM)
        return b

    def _set_cw_keyer_mode(self, mode):
        mode = max(0, min(len(_NR7Y_CW_KEYER_MODES) - 1, int(mode)))
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR + 4, mode)

    # -- Keyer speed (bits 0-6 of CW byte 1; mode no longer shares this byte) --

    def _get_cw_wpm(self):
        """Return WPM (10-45)."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 1)
        if b == 0xFF:
            return 18  # Default 18 WPM
        wpm = b & 0x7F
        return wpm if 10 <= wpm <= 45 else 18

    def _set_cw_wpm(self, wpm):
        wpm = max(10, min(45, int(wpm)))
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR + 1, wpm & 0x7F)

    # -- Key input mode (bits 0-4 of CW byte 2) --

    def _get_cw_key_input_idx(self):
        """Return key input menu index 0-10."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 2)
        if b == 0xFF or b >= 0x80:
            return 0   # Default HandKey
        idx = b & 0x1F
        return idx if idx < len(_NR7Y_CW_KEY_INPUT_MODES) else 0

    def _set_cw_key_input_idx(self, idx):
        if idx < 0 or idx >= len(_NR7Y_CW_KEY_INPUT_MODES):
            idx = 0
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 2)
        break_in = (b >> 6) & 0x01
        reserved = b & 0x20
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR + 2,
                       (int(idx) & 0x1F) | reserved | (break_in << 6))

    # -- Break-in enable (bit 6 of CW byte 2) --

    def _get_cw_breakin(self):
        """Return 1=ON (break-in enabled), 0=OFF."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 2)
        if b == 0xFF or b >= 0x80:
            return 1   # Default ON
        return (b >> 6) & 0x01

    def _set_cw_breakin(self, enable):
        enable = 1 if enable else 0
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 2)
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR + 2,
                       (b & 0x1F) | (b & 0x20) | (enable << 6))

    # -- Message repeat delay (bits 0-6 of CW byte 3) --

    def _get_cw_repeat_delay(self):
        """Return repeat delay in seconds (0-127)."""
        b = self._mmap_byte(_NR7Y_CW_SETTINGS_ADDR + 3)
        if b >= 0x80:
            return 4   # Default 4 seconds
        return b & 0x7F

    def _set_cw_repeat_delay(self, delay):
        self._mmap_set(_NR7Y_CW_SETTINGS_ADDR + 3, max(0, min(127, int(delay))) & 0x7F)

    # ------------------------------------------------------------------ CW macro read/write

    def _get_cw_msg(self, idx):
        """Read and decode CW macro. idx is 1-4.  Returns plain-text string."""
        if idx < 1 or idx > 4:
            return ""
        addr = _NR7Y_CW_MACRO_ADDRS[idx - 1]
        try:
            raw = bytes(self._mmap[addr:addr + _NR7Y_CW_MACRO_SIZE])
        except Exception as e:
            LOG.error("Error reading CW macro %d from 0x%04x: %s", idx, addr, e)
            return ""

        length_byte = raw[0]
        if length_byte == 0xFF:
            return ""   # Empty macro
        if (length_byte & _NR7Y_CW_MACRO_SIG) == 0:
            return ""   # Missing signature – not a valid macro block
        length = length_byte & ~_NR7Y_CW_MACRO_SIG
        if length == 0 or length > _NR7Y_CW_MACRO_MAX_LEN:
            return ""

        # Verify checksum (last byte of the block)
        checksum = sum(raw[1:length + 1]) & 0xFF
        if raw[_NR7Y_CW_MACRO_SIZE - 1] != checksum:
            LOG.warning("CW macro %d checksum mismatch (expected 0x%02x, got 0x%02x)",
                        idx, checksum, raw[_NR7Y_CW_MACRO_SIZE - 1])
            return ""

        # Decode: each payload byte has bit 7 = inter-word space before this char
        result = []
        for i in range(1, length + 1):
            byte = raw[i]
            has_space = (byte & 0x80) != 0
            char = chr(byte & 0x7F)
            if has_space and result:   # No leading space
                result.append(' ')
            result.append(char)
        text = ''.join(result)
        LOG.info("CW macro %d read: '%s' (%d chars)", idx, text, length)
        return text

    def _set_cw_msg(self, idx, text):
        """Encode and write CW macro. idx is 1-4.  text is plain ASCII."""
        if idx < 1 or idx > 4:
            return
        addr = _NR7Y_CW_MACRO_ADDRS[idx - 1]
        raw = bytearray([0xFF] * _NR7Y_CW_MACRO_SIZE)

        if not text or not text.strip():
            for i in range(_NR7Y_CW_MACRO_SIZE):
                self._mmap[addr + i] = raw[i]
            LOG.info("CW macro %d cleared", idx)
            return

        # Encode words, inserting inter-word space flag on first char of each word
        encoded = []
        for word_idx, word in enumerate(text.upper().split()):
            for char_idx, char in enumerate(word):
                if len(encoded) >= _NR7Y_CW_MACRO_MAX_LEN:
                    break
                if not ((char >= 'A' and char <= 'Z') or
                        (char >= '0' and char <= '9') or
                        char in ('/', '?', ',', '.', '-', '=')):
                    LOG.debug("CW macro %d: skipping invalid char '%s'", idx, char)
                    continue
                has_space = (word_idx > 0 and char_idx == 0)
                encoded.append(ord(char) | (0x80 if has_space else 0x00))

        if not encoded:
            for i in range(_NR7Y_CW_MACRO_SIZE):
                self._mmap[addr + i] = raw[i]
            LOG.warning("CW macro %d: empty after filtering '%s'", idx, text)
            return

        raw[0] = len(encoded) | _NR7Y_CW_MACRO_SIG
        for i, b in enumerate(encoded):
            raw[i + 1] = b
        checksum = sum(encoded) & 0xFF
        raw[_NR7Y_CW_MACRO_SIZE - 1] = checksum

        for i in range(_NR7Y_CW_MACRO_SIZE):
            self._mmap[addr + i] = raw[i]
        LOG.info("CW macro %d saved: '%s' (%d chars, chk=0x%02x)",
                 idx, text, len(encoded), checksum)
