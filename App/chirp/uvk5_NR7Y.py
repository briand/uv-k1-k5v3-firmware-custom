# uvk5_NR7Y.py
# CHIRP driver for Quansheng UV-K5 with NR7Y CW firmware
# Supports CW modulator settings and 4 macro memories

import re
import logging
import struct

from chirp import directory, bitwise, memmap, errors, chirp_common
from chirp.settings import (
    RadioSetting, RadioSettingGroup, RadioSettingValueBoolean,
    RadioSettingValueInteger, RadioSettingValueString, RadioSettingValueList,
    RadioSettings
)

# Import the official UV-K5 driver from CHIRP
from chirp.drivers import uvk5

LOG = logging.getLogger(__name__)

# CW Macro constants
CW_MACRO_ADDRS = [0x1C00, 0x1C30, 0x1C60, 0x1C90]
CW_MACRO_SIZE = 48
CW_MACRO_MAX_LEN = 46
CW_MACRO_SIG = 0x80

# CW settings EEPROM addresses
CW_SETTINGS_ADDR = 0x0F20  # 3 bytes: freq/vol, mode/wpm, key_input

# Key input mode display strings (menu index stored directly, bitmap not stored)
CW_KEY_INPUT_MODES = [
    "PTT HandKey",                    # 0: 0x08
    "PTT+Tip HandKey",               # 1: 0x18
    "PTT dah, Side1 dit",             # 2: 0x04
    "PTT dit, Side1 dah",             # 3: 0x05
    "PTT+Tip dah, Ring dit",          # 4: 0x12
    "PTT+Tip dit, Ring dah",          # 5: 0x13
    "Both dah, Both dit",             # 6: 0x16
    "Both dit, Both dah",             # 7: 0x17
    "CEC (PTT dah, Tip dit)",         # 8: 0x20 - NEW beta3
    "CEC (PTT dit, Tip dah)"          # 9: 0x21 - NEW beta3
]
# Complete programmable key actions list from firmware (settings.h:105-127)
# Beta3 extends with repeat actions - synced with firmware beta3
KEYACTIONS_LIST = [
    "None",                      # 0: ACTION_OPT_NONE
    "Flashlight on/off",         # 1: ACTION_OPT_FLASHLIGHT
    "Power select",              # 2: ACTION_OPT_POWER
    "Monitor",                   # 3: ACTION_OPT_MONITOR
    "Scan on/off",               # 4: ACTION_OPT_SCAN
    "VOX on/off",                # 5: ACTION_OPT_VOX
    "Alarm on/off",              # 6: ACTION_OPT_ALARM
    "FM radio on/off",           # 7: ACTION_OPT_FM
    "Transmit 1750 Hz",          # 8: ACTION_OPT_1750
    "Keylock",                   # 9: ACTION_OPT_KEYLOCK
    "A/B",                       # 10: ACTION_OPT_A_B
    "VFO/MR",                    # 11: ACTION_OPT_VFO_MR
    "Switch Demodulation",       # 12: ACTION_OPT_SWITCH_DEMODUL
    "Backlight Min Temp Off",    # 13: ACTION_OPT_BLMIN_TMP_OFF
    "Play CW MSG 1",             # 14: ACTION_OPT_PLAY_CWMSG1 (CW firmware)
    "Play CW MSG 2",             # 15: ACTION_OPT_PLAY_CWMSG2 (CW firmware)
    "Play CW MSG 3",             # 16: ACTION_OPT_PLAY_CWMSG3 (CW firmware)
    "Play CW MSG 4",             # 17: ACTION_OPT_PLAY_CWMSG4 (CW firmware)
    "Repeat CW MSG 1",           # 18: ACTION_OPT_REPEAT_CWMSG1 (beta3)
    "Repeat CW MSG 2",           # 19: ACTION_OPT_REPEAT_CWMSG2 (beta3)
    "Repeat CW MSG 3",           # 20: ACTION_OPT_REPEAT_CWMSG3 (beta3)
    "Repeat CW MSG 4",           # 21: ACTION_OPT_REPEAT_CWMSG4 (beta3)
    # SPECTRUM disabled due to lack of space
]


@directory.register
@directory.detected_by(uvk5.UVK5Radio)
class UVK5_NR7Y(uvk5.UVK5RadioBase):
    """Quansheng UV-K5 with NR7Y CW firmware"""

    VENDOR = "Quansheng"
    MODEL = "UV-K5"
    VARIANT = "NR7Y"
    
    # Make firmware writable (not restricted)
    NEEDS_COMPAT_SERIAL = False
    
    @classmethod
    def k5_approve_firmware(cls, firmware):
        """Approve NR7Y firmware versions"""
        # Accept any firmware with "NR7Y" in the name
        result = firmware and 'NR7Y' in firmware.upper()
        if result:
            LOG.info(f"NR7Y firmware approved: {firmware}")
        return result
    
    @classmethod
    def match_model(cls, filedata, filename):
        """Override to match NR7Y firmware in image files"""
        # Let base class do basic validation
        try:
            if not uvk5.UVK5Radio.match_model(filedata, filename):
                return False
        except:
            pass
        
        # Check for NR7Y firmware string in the image
        try:
            # Firmware version usually stored around 0x2000-0x3000
            data_str = bytes(filedata[0x2000:0x3000]).decode('ascii', errors='ignore')
            if 'NR7Y' in data_str:
                LOG.info("NR7Y firmware detected in image file")
                return True
        except Exception as e:
            LOG.debug(f"Error checking image file: {e}")
        
        return False

    def _is_nr7y_cw_firmware(self) -> bool:
        """Check if firmware has CW modulator enabled"""
        try:
            # Read build options from 0x1FF0-0x1FF1
            build_opts = bytes(self._mmap[0x1FF0:0x1FF2])
            # Bit 6 of byte 1 indicates ENABLE_CW_MODULATOR
            has_cw = (build_opts[1] & 0x40) != 0
            LOG.info(f"CW modulator flag: {has_cw} (0x1FF1=0x{build_opts[1]:02x})")
            return has_cw
        except Exception as e:
            LOG.error(f"Error checking CW firmware flag: {e}")
            return False

    def _update_key_actions(self, rs: RadioSettings) -> None:
        """Replace programmable key settings with extended action list"""
        _mem = self._memobj
        
        # Find the key actions group and its parent index
        keya = None
        keya_index = None
        for i, group in enumerate(rs):
            if hasattr(group, 'get_name') and group.get_name() == 'keya':
                keya = group
                keya_index = i
                break
        
        if not keya:
            LOG.warning("Could not find key actions group")
            return
        
        # Create new RadioSettingGroup with extended actions
        new_keya = RadioSettingGroup("keya", "Programmable keys")
        
        # Copy over non-key-action settings from original group
        for setting in keya:
            if hasattr(setting, 'get_name'):
                name = setting.get_name()
                if 'key' not in name or 'action' not in name:
                    # Keep non-key-action settings
                    new_keya.append(setting)
        
        # Add key action settings with extended action list
        tmpval = int(_mem.key1_shortpress_action)
        if tmpval >= len(KEYACTIONS_LIST):
            tmpval = 0
        rs_new = RadioSetting("key1_shortpress_action", "Side key 1 short press",
                          RadioSettingValueList(
                              KEYACTIONS_LIST, current_index=tmpval))
        new_keya.append(rs_new)

        tmpval = int(_mem.key1_longpress_action)
        if tmpval >= len(KEYACTIONS_LIST):
            tmpval = 0
        rs_new = RadioSetting("key1_longpress_action", "Side key 1 long press",
                          RadioSettingValueList(
                              KEYACTIONS_LIST, current_index=tmpval))
        new_keya.append(rs_new)

        tmpval = int(_mem.key2_shortpress_action)
        if tmpval >= len(KEYACTIONS_LIST):
            tmpval = 0
        rs_new = RadioSetting("key2_shortpress_action", "Side key 2 short press",
                          RadioSettingValueList(
                              KEYACTIONS_LIST, current_index=tmpval))
        new_keya.append(rs_new)

        tmpval = int(_mem.key2_longpress_action)
        if tmpval >= len(KEYACTIONS_LIST):
            tmpval = 0
        rs_new = RadioSetting("key2_longpress_action", "Side key 2 long press",
                          RadioSettingValueList(
                              KEYACTIONS_LIST, current_index=tmpval))
        new_keya.append(rs_new)
        
        # Replace old group with new one
        rs[keya_index] = new_keya
        
        LOG.info(f"Updated programmable keys with {len(KEYACTIONS_LIST)} actions")

    def get_settings(self):
        """Get radio settings including CW if detected"""
        LOG.info("UVK5_NR7Y.get_settings() called")
        
        try:
            rs = super().get_settings()
        except Exception as e:
            LOG.error(f"Error getting base settings: {e}")
            rs = RadioSettings()
        
        # Update programmable key actions with extended list (includes CW messages)
        self._update_key_actions(rs)

        # Check if CW firmware
        if not self._is_nr7y_cw_firmware():
            LOG.warning("CW modulator not enabled in firmware - skipping CW settings")
            return rs

        LOG.info("CW modulator detected - adding CW settings")

        # Remove DTMF contacts if present (conflicts with CW macros)
        self._remove_dtmf_contacts(rs)

        # Add CW settings group
        cw = RadioSettingGroup("cw", "CW Settings")

        # Sidetone Frequency (450-950 Hz in 50 Hz steps)
        freq_opts = ["%d Hz" % (450 + i * 50) for i in range(11)]
        try:
            freq_idx = self._get_cw_frequency_idx()
            cw_freq = RadioSetting(
                "cw.frequency",
                "Sidetone Frequency",
                RadioSettingValueList(freq_opts, freq_opts[freq_idx])
            )
            cw.append(cw_freq)
        except Exception as e:
            LOG.error(f"Error adding frequency setting: {e}")

        # Sidetone Volume (0=OFF, 1-6)
        vol_opts = ["OFF"] + [str(i) for i in range(1, 7)]
        try:
            vol_idx = self._get_cw_sidetone_level()
            cw_vol = RadioSetting(
                "cw.sidetone_level",
                "Sidetone Volume",
                RadioSettingValueList(vol_opts, vol_opts[vol_idx])
            )
            cw.append(cw_vol)
        except Exception as e:
            LOG.error(f"Error adding volume setting: {e}")

        # Keyer Mode (Iambic A/B)
        mode_opts = ["Iambic A", "Iambic B"]
        try:
            mode_idx = self._get_cw_keyer_mode()
            cw_mode = RadioSetting(
                "cw.keyer_mode",
                "Keyer Mode",
                RadioSettingValueList(mode_opts, mode_opts[mode_idx])
            )
            cw.append(cw_mode)
        except Exception as e:
            LOG.error(f"Error adding keyer mode: {e}")

        # Keyer Speed (10-30 WPM)
        try:
            wpm = self._get_cw_wpm()
            cw_wpm = RadioSetting(
                "cw.wpm",
                "Keyer Speed (WPM)",
                RadioSettingValueInteger(10, 30, wpm)
            )
            cw.append(cw_wpm)
        except Exception as e:
            LOG.error(f"Error adding WPM setting: {e}")

        # Key Input Configuration
        try:
            key_idx = self._get_cw_key_input_idx()
            cw_key_input = RadioSetting(
                "cw.key_input",
                "Key Input Mode",
                RadioSettingValueList(CW_KEY_INPUT_MODES,
                                     CW_KEY_INPUT_MODES[key_idx])
            )
            cw.append(cw_key_input)
        except Exception as e:
            LOG.error(f"Error adding key input setting: {e}")

        # CW Break-in (CWbkin) - OFF/ON
        try:
            breakin_opts = ["OFF", "ON"]
            breakin_idx = 1 if self._get_cw_breakin() else 0
            cw_breakin = RadioSetting(
                "cw.break_in",
                "Break-in",
                RadioSettingValueList(breakin_opts, breakin_opts[breakin_idx])
            )
            cw_breakin.set_doc("ON: RF TX during keying. OFF: sidetone only, no TX.")
            cw.append(cw_breakin)
        except Exception as e:
            LOG.error(f"Error adding break-in setting: {e}")

        # Message Repeat Delay (beta3)
        try:
            repeat_delay = self._get_cw_repeat_delay()
            cw_repeat = RadioSetting(
                "cw.repeat_delay",
                "Message Repeat Delay (sec)",
                RadioSettingValueInteger(0, 127, repeat_delay)
            )
            cw_repeat.set_doc("Delay between repeated CW message transmissions (0-127 seconds)")
            cw.append(cw_repeat)
        except Exception as e:
            LOG.error(f"Error adding repeat delay setting: {e}")

        # CW Macros (4 messages)
        macros = RadioSettingGroup("cw_macros", "CW Macros")
        for i in range(1, 5):
            try:
                msg_text = self._get_cw_msg(i)
                val = RadioSettingValueString(0, 46, msg_text)
                msg = RadioSetting(
                    f"cw.msg{i}",
                    f"CW Message {i}",
                    val
                )
                msg.set_doc(f"CW macro {i} (A-Z, 0-9, /, ? only, max 46 chars)")
                macros.append(msg)
            except Exception as e:
                LOG.error(f"Error adding macro {i}: {e}")
        
        cw.append(macros)
        rs.append(cw)
        
        LOG.info(f"Added CW settings group with {len(list(cw))} settings")
        return rs

    def set_settings(self, settings):
        """Apply settings to radio - follows base class pattern with CW support"""
        _mem = self._memobj
        for element in settings:
            if not isinstance(element, RadioSetting):
                # It's a group, recurse into it
                self.set_settings(element)
                continue
            
            # It's an individual setting
            setting = element
            name = setting.get_name()
            
            # Handle programmable key actions with extended list
            if name == "key1_shortpress_action":
                _mem.key1_shortpress_action = KEYACTIONS_LIST.index(str(setting.value))
                LOG.debug(f"Set key1_shortpress_action to {setting.value}")
                continue
            elif name == "key1_longpress_action":
                _mem.key1_longpress_action = KEYACTIONS_LIST.index(str(setting.value))
                LOG.debug(f"Set key1_longpress_action to {setting.value}")
                continue
            elif name == "key2_shortpress_action":
                _mem.key2_shortpress_action = KEYACTIONS_LIST.index(str(setting.value))
                LOG.debug(f"Set key2_shortpress_action to {setting.value}")
                continue
            elif name == "key2_longpress_action":
                _mem.key2_longpress_action = KEYACTIONS_LIST.index(str(setting.value))
                LOG.debug(f"Set key2_longpress_action to {setting.value}")
                continue
            
            # Handle CW settings
            if name.startswith("cw."):
                try:
                    if name == "cw.frequency":
                        freq_opts = ["%d Hz" % (450 + i * 50) for i in range(11)]
                        idx = freq_opts.index(str(setting.value))
                        self._set_cw_frequency_idx(idx)
                        LOG.debug(f"Set CW frequency to {freq_opts[idx]}")
                    elif name == "cw.sidetone_level":
                        vol_opts = ["OFF"] + [str(i) for i in range(1, 7)]
                        idx = vol_opts.index(str(setting.value))
                        self._set_cw_sidetone_level(idx)
                        LOG.debug(f"Set CW volume to {vol_opts[idx]}")
                    elif name == "cw.keyer_mode":
                        idx = ["Iambic A", "Iambic B"].index(str(setting.value))
                        self._set_cw_keyer_mode(idx)
                        LOG.debug(f"Set keyer mode to {setting.value}")
                    elif name == "cw.wpm":
                        self._set_cw_wpm(int(setting.value))
                        LOG.debug(f"Set WPM to {setting.value}")
                    elif name == "cw.key_input":
                        idx = CW_KEY_INPUT_MODES.index(str(setting.value))
                        self._set_cw_key_input_idx(idx)
                        LOG.debug(f"Set key input to {setting.value}")
                    elif name in ("cw.break_in", "cw.oper_mode"):
                        # Match firmware menu behavior: OFF=0, ON=1
                        idx = ["OFF", "ON"].index(str(setting.value)) if name == "cw.break_in" else ["OFF (Normal CW)", "ON (CPO Mode)"].index(str(setting.value))
                        self._set_cw_breakin(1 if idx > 0 else 0)
                        LOG.debug(f"Set break-in to {setting.value}")
                    elif name == "cw.repeat_delay":
                        self._set_cw_repeat_delay(int(setting.value))
                        LOG.debug(f"Set CW repeat delay to {setting.value}")
                    elif name.startswith("cw.msg"):
                        # Extract macro number from "cw.msg1" → "1"
                        # "cw.msg" is 6 chars, so number is at index 6
                        macro_num = name[6:]  # Skip "cw.msg" to get "1", "2", "3", "4"
                        idx = int(macro_num)
                        self._set_cw_msg(idx, str(setting.value))
                        LOG.info(f"Saved macro {idx}: '{str(setting.value)[:20]}...'")
                except Exception as e:
                    LOG.error(f"Error applying CW setting {name}: {e}")
                    import traceback
                    traceback.print_exc()
                continue
            
            # Non-CW settings - let base class handle them
            # Call parent's set_settings logic directly for this one setting
            if name == "call_channel":
                _mem.call_channel = int(setting.value)-1
            elif name == "squelch":
                _mem.squelch = int(setting.value)
            elif name == "tot":
                _mem.max_talk_time = int(setting.value)
            elif name == "noaa_autoscan":
                _mem.noaa_autoscan = setting.value and 1 or 0
            elif name == "vox_switch":
                _mem.vox_switch = setting.value and 1 or 0
            elif name == "vox_level":
                _mem.vox_level = int(setting.value)-1
            elif name == "mic_gain":
                _mem.mic_gain = int(setting.value)
            # ... base class handles all other settings through its implementation
            # We'll just let anything else pass through by calling parent on groups

    def _remove_dtmf_contacts(self, rs: RadioSettings) -> None:
        """Remove DTMF contacts group to prevent conflicts"""
        removed = []
        for group in list(rs):
            if hasattr(group, 'get_name'):
                name = group.get_name()
                # Remove any DTMF contact groups
                if 'contact' in name.lower() and 'dtmf' in name.lower():
                    rs.remove(group)
                    removed.append(name)
        
        if removed:
            LOG.info(f"Removed DTMF contact groups: {removed}")

    # ======== CW Settings Encode/Decode ========
    
    def _get_cw_frequency_idx(self) -> int:
        """Get sidetone frequency index (0-10 for 450-950 Hz)"""
        byte0 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR:CW_SETTINGS_ADDR+1]))[0]
        if byte0 == 0xFF:
            return 3  # Default 600 Hz (index 3)
        # Formula from settings.c:234 - stored as (Hz/10 - 45) / 5
        freq_value = 45 + (byte0 & 0x0F) * 5  # This gives Hz/10
        # Convert to Hz and then to index
        freq_hz = freq_value * 10
        idx = (freq_hz - 450) // 50
        return max(0, min(10, idx))
    
    def _set_cw_frequency_idx(self, idx: int) -> None:
        """Set sidetone frequency from index"""
        byte0 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR:CW_SETTINGS_ADDR+1]))[0]
        freq_hz = 450 + idx * 50
        freq_value = freq_hz // 10  # Convert to deciHz
        encoded = (freq_value - 45) // 5
        # Preserve bits 4-6 (sidetone level), update bits 0-3
        byte0 = (byte0 & 0xF0) | (encoded & 0x0F)
        self._mmap[CW_SETTINGS_ADDR] = byte0

    def _get_cw_sidetone_level(self) -> int:
        """Get sidetone volume level (0-6)"""
        byte0 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR:CW_SETTINGS_ADDR+1]))[0]
        if byte0 == 0xFF:
            return 4  # Default level 4
        # Formula from settings.c:235 - bits 4-6
        return (byte0 >> 4) & 0x07
    
    def _set_cw_sidetone_level(self, level: int) -> None:
        """Set sidetone volume level (0-6)"""
        byte0 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR:CW_SETTINGS_ADDR+1]))[0]
        # Preserve bits 0-3 (frequency), update bits 4-6
        byte0 = (byte0 & 0x0F) | ((level & 0x07) << 4)
        self._mmap[CW_SETTINGS_ADDR] = byte0

    def _get_cw_keyer_mode(self) -> int:
        """Get keyer mode (0=A, 1=B)"""
        byte1 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+1:CW_SETTINGS_ADDR+2]))[0]
        if byte1 == 0xFF:
            return 0  # Default Mode A
        # Formula from settings.c:236 - bit 7
        return 1 if (byte1 & 0x80) else 0
    
    def _set_cw_keyer_mode(self, mode: int) -> None:
        """Set keyer mode (0=A, 1=B)"""
        byte1 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+1:CW_SETTINGS_ADDR+2]))[0]
        if mode == 1:
            byte1 |= 0x80  # Set bit 7
        else:
            byte1 &= 0x7F  # Clear bit 7
        self._mmap[CW_SETTINGS_ADDR + 1] = byte1

    def _get_cw_wpm(self) -> int:
        """Get keyer speed in WPM"""
        byte1 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+1:CW_SETTINGS_ADDR+2]))[0]
        if byte1 == 0xFF:
            return 18  # Default 18 WPM
        # Formula from settings.c:237 - bits 0-5
        wpm = byte1 & 0x3F
        if wpm < 10 or wpm > 30:
            return 18
        return wpm
    
    def _set_cw_wpm(self, wpm: int) -> None:
        """Set keyer speed in WPM (10-30)"""
        wpm = max(10, min(30, wpm))
        byte1 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+1:CW_SETTINGS_ADDR+2]))[0]
        # Preserve bit 7 (keyer mode), update bits 0-5
        byte1 = (byte1 & 0x80) | (wpm & 0x3F)
        self._mmap[CW_SETTINGS_ADDR + 1] = byte1

    def _get_cw_key_input_idx(self) -> int:
        """Get key input mode index (0-9) - stored directly in bits 0-4"""
        byte2 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+2:CW_SETTINGS_ADDR+3]))[0]
        if byte2 == 0xFF or byte2 >= 0x80:
            return 0  # Default HandKey
        # bits 0-4 = menu index (0-9)
        menu_idx = byte2 & 0x1F
        if menu_idx >= len(CW_KEY_INPUT_MODES):
            LOG.debug(f"Invalid key input menu index {menu_idx}, defaulting to HandKey")
            return 0
        return menu_idx
    
    def _set_cw_key_input_idx(self, idx: int) -> None:
        """Set key input mode from index - stored directly in bits 0-5"""
        if idx < 0 or idx >= len(CW_KEY_INPUT_MODES):
            idx = 0
        byte2 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+2:CW_SETTINGS_ADDR+3]))[0]
        # Preserve bit 6 (break-in) and bit 5 (reserved), set bits 0-5 (menu index)
        break_in = (byte2 >> 6) & 0x01
        reserved = byte2 & 0x20
        self._mmap[CW_SETTINGS_ADDR + 2] = (idx & 0x1F) | reserved | (break_in << 6)

    def _get_cw_breakin(self) -> int:
        """Get CW break-in enable (0=OFF, 1=ON)"""
        byte2 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+2:CW_SETTINGS_ADDR+3]))[0]
        if byte2 == 0xFF or byte2 >= 0x80:
            return 1  # Default Break-in ON
        return (byte2 >> 6) & 0x01
    
    def _set_cw_breakin(self, mode: int) -> None:
        """Set CW break-in enable (0=OFF, 1=ON)"""
        mode = 1 if mode else 0
        byte2 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+2:CW_SETTINGS_ADDR+3]))[0]
        # Preserve bits 0-4 (key input) and bit 5 (reserved), set bit 6 (break-in)
        key_input = byte2 & 0x1F
        reserved = byte2 & 0x20
        self._mmap[CW_SETTINGS_ADDR + 2] = key_input | reserved | (mode << 6)

    def _get_cw_repeat_delay(self) -> int:
        """Get CW message repeat delay in seconds (0-127) - beta3"""
        byte3 = struct.unpack('B', bytes(self._mmap[CW_SETTINGS_ADDR+3:CW_SETTINGS_ADDR+4]))[0]
        if byte3 >= 0x80:  # High bit set = invalid
            return 4  # Default 4 seconds
        return byte3 & 0x7F
    
    def _set_cw_repeat_delay(self, delay: int) -> None:
        """Set CW message repeat delay (0-127 seconds) - beta3"""
        delay = max(0, min(127, delay))
        self._mmap[CW_SETTINGS_ADDR + 3] = delay & 0x7F

    # ======== CW Macro Encode/Decode ========
    
    def _get_cw_msg(self, idx: int) -> str:
        """Read CW macro from EEPROM with validation"""
        if idx < 1 or idx > 4:
            return ""
        
        addr = CW_MACRO_ADDRS[idx - 1]
        try:
            raw = bytes(self._mmap[addr:addr + CW_MACRO_SIZE])
        except Exception as e:
            LOG.error(f"Error reading macro {idx} from 0x{addr:04x}: {e}")
            return ""
        
        # Parse macro format (from cwmacro.c:113-148)
        length_byte = raw[0]
        
        if length_byte == 0xFF:
            LOG.debug(f"Macro {idx}: empty")
            return ""  # Empty macro
        
        if (length_byte & CW_MACRO_SIG) == 0:
            LOG.debug(f"Macro {idx}: no signature (byte0=0x{length_byte:02x})")
            return ""  # Invalid signature
        
        length = length_byte & ~CW_MACRO_SIG
        if length == 0 or length > CW_MACRO_MAX_LEN:
            LOG.debug(f"Macro {idx}: invalid length {length}")
            return ""
        
        # Verify checksum (last byte)
        checksum = sum(raw[1:length + 1]) & 0xFF
        if raw[CW_MACRO_SIZE - 1] != checksum:
            LOG.warning(f"Macro {idx} checksum fail (expected 0x{checksum:02x}, got 0x{raw[CW_MACRO_SIZE - 1]:02x})")
            return ""
        
        # Decode characters
        result = []
        for i in range(1, length + 1):
            byte = raw[i]
            has_space = (byte & 0x80) != 0
            char = chr(byte & 0x7F)
            
            if has_space and result:  # Don't add space at start
                result.append(' ')
            result.append(char)
        
        text = ''.join(result)
        LOG.info(f"Macro {idx}: '{text}' ({length} chars, checksum OK)")
        return text

    def _set_cw_msg(self, idx: int, text: str) -> None:
        """Write CW macro to EEPROM with checksum"""
        if idx < 1 or idx > 4:
            return
        
        addr = CW_MACRO_ADDRS[idx - 1]
        
        # Clear block
        raw = bytearray([0xFF] * CW_MACRO_SIZE)
        
        if not text or text.strip() == "":
            # Empty macro - write byte by byte
            for i in range(CW_MACRO_SIZE):
                self._mmap[addr + i] = raw[i]
            LOG.info(f"Cleared macro {idx}")
            return
        
        # Validate and encode characters
        encoded = []
        words = text.upper().split()
        char_count = 0
        
        for word_idx, word in enumerate(words):
            for char_idx, char in enumerate(word):
                if char_count >= CW_MACRO_MAX_LEN:
                    break
                
                # Validate character (A-Z, 0-9, /, ?)
                if not ((char >= 'A' and char <= 'Z') or 
                        (char >= '0' and char <= '9') or 
                        char in ['/', '?']):
                    LOG.warning(f"Skipping invalid char '{char}' in macro {idx}")
                    continue
                
                # Add space marker before word (except first char overall)
                has_space = (word_idx > 0 and char_idx == 0)
                byte = ord(char) | (0x80 if has_space else 0x00)
                encoded.append(byte)
                char_count += 1
        
        if char_count == 0:
            # Empty after filtering - write byte by byte
            for i in range(CW_MACRO_SIZE):
                self._mmap[addr + i] = raw[i]
            LOG.warning(f"Macro {idx} empty after filtering invalid chars from '{text}'")
            return
        
        # Set length with signature
        raw[0] = char_count | CW_MACRO_SIG
        
        # Set encoded characters
        for i, byte in enumerate(encoded):
            raw[i + 1] = byte
        
        # Calculate and set checksum
        checksum = sum(encoded) & 0xFF
        raw[CW_MACRO_SIZE - 1] = checksum
        
        # Write to memory byte by byte
        for i in range(CW_MACRO_SIZE):
            self._mmap[addr + i] = raw[i]
        
        LOG.info(f"Saved macro {idx}: '{text}' ({char_count} chars, checksum=0x{checksum:02x})")


