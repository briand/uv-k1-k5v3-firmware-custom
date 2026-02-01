NR7Y Beta2 Firmware README

Thanks for beta testing! Please post feedback in the Slack. I'll push this code to GitHub soon and collect issues there as well.

**MOST IMPORTANT**
This firmware only works on "V1" firmware. How do you know? Well, you kinda don't. If you bought your radio early last year, it's V1. If you bought it late last year, it could be V2. If you bought it in 2026, it could be V3!

If the flashing process doesn't work, you know you don't have a V1. If the attempt messes up your radio, there are recovery steps listed online. Sorry about that. I will work on a V2/V3 compatible version of the firmware soon. Note: Bootloader version is not the same as radio hardware version; it's ok if you see "bootloader version 2.xx" during flashing.

Upload using your favorite Quansheng flash tool. I use k5tool (https://github.com/qrp73/K5TOOL). This link also has recovery information on the landing README page, if needed. The web-based https://egzumer.github.io/uvtools/ works well too, but Chrome-only.

After flashing, I *strongly advise* resetting the eeprom to ensure there are no lingering settings from other firmware versions. 
 1. Hold down PTT and Side Button 1 while turning on
 2. Release buttons, menu will be automatically presented.
 3. Go up to Reset and pick ALL. 


## Changelist from beta1 -> beta2
- Recording and playback of macro messages
- On-screen decoder while sending via keyer
- Increased deglitch to resolve RFI-induced extra elements (extra dit when you send a dah, and vice versa)
- Fixed inconsistent sidetone volume setting
- Don't allow setting a port-based keyer mode if no rework or if keys appear stuck
- Fix keyer menu naming "sleeve" -> "ring" for port
- Main screen: show RSSI signal bar in RX mode instead of AGC debug data

# CW (Morse Code) Firmware Guide

This guide describes the CW (Continuous Wave / Morse Code) menu options added to the UV-K5 firmware when `ENABLE_CW_MODULATOR` is enabled.

## Accessing CW Mode

1. Set your VFO modulation to **CW** mode (in the modulation menu), or long-press 0 (FM) to change modulations
2. Configure CW parameters in the menu system (Menu > CWfreq, CWvol, etc.)
3. Press PTT is a CW straight key by default

### Filtering

Menu option #9 (Filter) includes an extra setting for 1.7k. Might be the best we can do on this transciever.

## CW Menu Options

#### CWfreq - CW Sidetone Frequency

Sets the audio frequency for the CW sidetone you hear locally when transmitting. This is also used as the BFO (Beat Frequency Oscillator) offset when receiving CW signals.

#### CWvol - CW Sidetone Volume Level

Controls the volume level of the CW sidetone you hear when transmitting. The lowest value is "OFF"

#### CWkmod - CW Keyer Mode

Selects the keyer mode when using paddle inputs (dual-lever key). These modes are modeled after Elecraft mode A/B behavior. 

**Note:** Keyer mode only applies when using paddle inputs (buttons or port paddles). When using handkey modes, the keyer is disabled and this setting has no effect.

#### CWwpm - CW Speed (Words Per Minute)

Sets the sending speed for the automatic iambic keyer in Words Per Minute.

**Note:** Speed only applies when using paddle inputs with the iambic keyer enabled. Handkey modes follow your manual keying speed.

#### CWkin - CW Key Input Configuration

Configures how the CW input signals are connected and interpreted. This is the most complex setting as it determines which physical inputs are used and how they're mapped.

**Note:** The port modes (PTT+port, tip/ring settings) require an iambic key rework. If you have already done the original straight key mod and wish to keep it, this will continue to work with the "PTT HandKey" mode or PTT/Side1 modes.

#### CWmsg1 / CWmsg2 - CW Message Recording and Playback

Messages 1 and 2 may store up to 40 characters for playback (not including spaces).

Messages start empty - enter the menu and use arrows to change macro option:
- record new? - Select with 'menu' button to begin recording
- play - Select with 'menu' button to begin playback

### CW Message Recording
- Recordings are made using the currently programmed keyer settings and wpm. Attempting to record while not in CW modulation or without a keyer (while in handkey mode) will not work.
- RF is not transmitted while recording. 
- While recording is in progress, the display will show the most recently recorded characters, and a macro character count. 
- To save the macro when complete, Push the 'menu' button. 
- To exit the recording without saving, push the 'exit' button.

### CW Message Playback
There are two ways to playback the CW messages:
- Enter the menu selection for the given message, and change up/down to 'Play', then select with 'menu' button. Playback will begin.
- Assign playback to an action button (side buttons or Menu button):
  - In the menu system choose one of menu items 23 through 27 to assign playback to side button 1 or 2 short press, 1 or 2 long press, or Menu button long press. Keep in mind that Side Button 1 is unavailable for macro sending when set as a keyer button. The assigned action is ignored.
  - After choosing the button menu item, from the action list pick "PLAY CW MSG1" or "PLAY CW MSG2"
- During message playback, the display will show the characters being sent, and a flashing arrow on the left side to indicate a message is being played.
- Message playback can be interrupted by tapping a keyer key.

#### Input Options Explained:

**1. PTT HandKey** (Default)
- Use PTT button as a straight key

**2. PTT+port HandKey**
- PTT button as CW key (straight key mode)
- Also accepts a straight key in the 3.5mm port (with iambic rework)

**3. PTT dah, SD1 dit** (Buttons Normal)
- PTT = DAH (long element)
- Side button 1 (SD1) = DIT (short element)
- Automatic iambic keyer enabled

**4. PTT dit, SD1 dah** (Buttons Reversed)
- PTT = DIT (short element)
- Side button 1 (SD1) = DAH (long element)
- Automatic iambic keyer enabled
- **Reversed paddle orientation** (for left-handed operators or personal preference)

**5. PTT+tip dah, ring dit** (Port Normal)
- 3.5mm port TIP or PTT button = DAH
- 3.5mm port RING = DIT
- Automatic iambic keyer enabled

**6. PTT+tip dit, ring dah** (Port Reversed)
- 3.5mm port TIP or PTT button = DIT
- 3.5mm port RING = DAH
- Automatic iambic keyer enabled
- **Reversed paddle orientation**

**7. PTT+tip dah, SD1+ring dit** (Both Normal)
- PTT OR Tip = DAH
- Side button 1 (SD1) OR Ring = DIT
- Automatic iambic keyer enabled
- Both radio buttons AND external port paddles work simultaneously

**8. PTT+tip dit, SD1+ring dah** (Both Reversed)
- PTT OR Tip = DIT
- Side button 1 (SD1) OR Ring = DAH
- Automatic iambic keyer enabled
- Both radio buttons AND external port paddles work simultaneously
- **Reversed paddle orientation**

**Last Updated**: January 31, 2026
