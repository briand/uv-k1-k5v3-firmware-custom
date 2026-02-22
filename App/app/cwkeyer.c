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

 // CW Iambic Keyer implementation
// Lean FSM for iambic A/B with optional reversed mapping

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "app/cwkeyer.h"
#include "app/cwhardware.h"
#include "app/cwmacro.h"
#include "audio.h"
#include "settings.h"
#include "misc.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_tim.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_rcc.h"
#include "py32f071_ll_usart.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/timer.h"
#include "driver/system.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/keyboard.h"
#include "driver/backlight.h"
#include "ui/welcome.h"

#include "external/printf/printf.h"

// Debug logging control - set to 1 to enable UART debug output
#define CW_KEYER_DEBUG 0

// Timer scale: 1 kHz tick → 1 ms per tick
// 32-bit counter rolls over at 4294967295 ms (~49.7 days)
#define DITS_PER_WORD 50
#define TICKS_PER_MS 1
#define TICKS_PER_MINUTE (60 * 1000 * TICKS_PER_MS)  // 60,000

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_ELEMENT,
    CWK_STATE_INTER_ELEMENT_GAP,
    CWK_STATE_INTER_CHAR_GAP,    // Extended gap = end of character
    CWK_STATE_INTER_WORD_GAP,    // Long gap = inter-word space
    CWK_STATE_EMIT_NONE,         // Force a NONE action from the keyer
} CW_KeyerFSMState_t;

static CW_KeyerFSMState_t s_KeyerFSMState = CWK_STATE_IDLE;

// Internal keyer runtime state
static uint16_t       s_dit_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_dah_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_gap_count  = 0;      // inter-element gap in ticks (1 dit)
static uint16_t       s_ext_gap_count = 0;       // tick count when auto-char extension kicks in (16-bit)
static uint16_t       s_char_gap_count = 0;  // inter-char gap in ticks (3 dits)
static uint16_t       s_word_gap_count = 0;  // inter-word gap in ticks (7 dits)
static uint16_t       s_elem_start_count = 0;// element start counter (16-bit millis)
static bool           s_active_is_dit = false;
static bool           s_pending_alternate = false; // alternate element queued
/* last sampled paddles moved to app/cwhardware.c */
static bool           s_last_handkey_ptt = false; // last PTT state for handkey mode

// Macro playback state
static char s_playback_buf[CW_MACRO_MAX_LEN * 2 + 1]; // decoded with spaces inserted
static uint16_t s_playback_buf_len = 0; // strlen of the buffer
static uint16_t s_playback_pos = 0; // index into playback buffer
static uint8_t s_play_char_pattern = 0; // current char morse pattern (LSB-first)
static uint8_t s_play_char_len = 0; // number of elements in current char
static uint8_t s_play_elem_index = 0; // current element index within char

// Playback FSM states
typedef enum {
    PB_STATE_IDLE = 0,
    PB_STATE_ACTIVE_ELEMENT,
    PB_STATE_INTER_ELEMENT_GAP,
    PB_STATE_INTER_CHAR_GAP,
    PB_STATE_INTER_WORD_GAP,
} PB_State_t;
static PB_State_t s_pb_state = PB_STATE_IDLE;
static bool s_play_space_pending = false;  // indicates next char should be shown with a leading space

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = true;

// Keyer enabled flag - this means ALL CW keying, not just paddle FSM.
static volatile bool s_enable_keyer = false;

// Last fully configured key input mode. 0xFF means "not configured".
static uint8_t s_last_key_input_mode = 0xFF;

#ifdef ENABLE_FLASHLIGHT
bool gCW_FlashlightSending = false;
#endif

void CW_KeyerResetRuntime(void)
{
    s_KeyerFSMState = CWK_STATE_EMIT_NONE;

    s_elem_start_count = timer_millis_low16();

    s_active_is_dit = false;
    s_pending_alternate = false;
    s_last_handkey_ptt = false;

    // clear any stale edge memory
    CW_HW_ResetKeySamples();
}

// Called when changing to non-CW mode
static void CW_KeyerDeinit()
{
    CW_KeyerResetRuntime();

    // ADC deinit performs the port ground deinit too
    CW_ConfigureADCforCECPaddles(false);
    CW_ConfigurePortRing(false); // make sure PB15 is an input with no pullup, to avoid affecting the line if shorted to mic (keyer rework)

    gCW_KeyerManagesPtt = false;
    gCW_KeyerUsingSD1 = false;
    s_enable_keyer = false;
    s_last_key_input_mode = 0xFF;
}

void CW_KeyerReconfigure(bool enable)
{
#if CW_KEYER_DEBUG
    if (enable) {
        UART_Send("reconfig true\r\n", 14);
    } else {
        UART_Send("reconfig false\r\n", 15);
    }
#endif

    if (!enable) { 
        CW_KeyerDeinit();
        s_cfg_dirty = false;
        return;
    }

    // take over PTT and SIDE1 (if was or is in the INPUT set) immediately
    gCW_KeyerManagesPtt = true;
    gCW_KeyerUsingSD1 |= gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_SIDE1;
    CW_KeyerResetRuntime();
    s_cfg_dirty = true; // Defer init until idle or gap
}

void CW_UpdateWPM()
{
    const uint32_t wpm = gEeprom.CW_KEY_WPM;
    const uint32_t dit_ticks = TICKS_PER_MINUTE / (wpm * DITS_PER_WORD);

    s_dit_count = dit_ticks;
    s_dah_count = 3U * dit_ticks;
    s_gap_count = dit_ticks; // inter-element gap = 1 dit
    s_ext_gap_count = 3U * dit_ticks / 2U; // after 1.5 dits, hold char gap if key pressed
    s_char_gap_count = 3U * dit_ticks; // inter-char gap = 3 dits
    s_word_gap_count = 7U * dit_ticks; // inter-word gap = 7 dits

#if CW_KEYER_DEBUG
    char buf[80];
    sprintf_(buf, "CW_SetWPM: WPM=%u dit=%u dah=%u gap=%u\r\n", 
             wpm, s_dit_count, s_dah_count, s_gap_count);
    UART_Send(buf, strlen(buf));
#endif
}

// Initialize keyer from gEeprom settings
static void CW_KeyerInit()
{
    const uint8_t key_input_mode = gEeprom.CW_KEY_INPUT;

#if CW_KEYER_DEBUG
    char buf[80];
    sprintf_(buf, "CW_KeyerInit: enabled:%u WPM=%u last_mode=%u key_input_mode=%u\r\n", 
             s_enable_keyer, gEeprom.CW_KEY_WPM, s_last_key_input_mode, key_input_mode);
    UART_Send(buf, strlen(buf));
#endif

    CW_UpdateWPM();
    CW_KeyerResetRuntime();
    s_cfg_dirty = false;
    // Keyer is already enabled with this mode
    if (s_enable_keyer && s_last_key_input_mode == key_input_mode) {
        return;
    }

    bool uses_port_ground = (key_input_mode & CW_KEY_FLAG_PORT_GROUND) != 0;
    bool uses_port_ring   = (key_input_mode & CW_KEY_FLAG_PORT_RING) != 0;
    bool uses_adc         = (key_input_mode & CW_KEY_FLAG_ADC) != 0;

    CW_ConfigurePortRing(uses_port_ring);
    CW_ConfigureADCforCECPaddles(uses_adc);
    if (!uses_adc && uses_port_ground)
        CW_ConfigurePortGround(true);

    gCW_KeyerUsingSD1 = (key_input_mode & CW_KEY_FLAG_SIDE1) != 0;
    s_last_key_input_mode = key_input_mode;
    s_enable_keyer = true;
    gCW_KeyerManagesPtt = true;
}

// --- Macro playback API implementation ---
void CW_StartMacroPlayback(uint8_t macroIndex, bool repeat)
{	
    // Do nothing if recording or playback already in progress
    if (gCW_Recording || gCW_PlaybackActive) return;

    // Load the macro into a local buffer (decoded with spaces)
    memset(s_playback_buf, 0, sizeof(s_playback_buf));
    CW_LoadMacro(macroIndex, s_playback_buf, sizeof(s_playback_buf));
    s_playback_buf_len = (uint16_t)strlen(s_playback_buf);
    s_playback_pos = 0;
    gCW_PlaybackMacroIndex = macroIndex;
    s_play_elem_index = 0;
    s_play_char_len = 0;
    s_play_char_pattern = 0;

    // Store repeat flag
    gCW_PlaybackRepeat = repeat;
    gCW_MessageRepeatCountdown_500ms = 0;  // Clear any pending countdown

    // Clear TX display and prime the playback FSM to start immediately
    CW_ClearTxDisplay();
    s_play_space_pending = false;
    s_pb_state = PB_STATE_INTER_CHAR_GAP;
    s_elem_start_count = timer_millis_low16();
    gCW_PlaybackActive = (s_playback_buf_len > 0);

#if CW_KEYER_DEBUG
    if (gCW_PlaybackActive) {
        char buf[80];
        sprintf_(buf, "Playback started: idx=%u buf='%s'\r\n", macroIndex, s_playback_buf);
        UART_Send(buf, strlen(buf));
    }
#endif
}

// Stop playback immediately (user interrupted)
void CW_StopPlayback(void)
{
    gCW_PlaybackActive = false;
    gCW_PlaybackRepeat = false;       // Cancel any pending repeat
    gCW_MessageRepeatCountdown_500ms = 0;
    s_pb_state = PB_STATE_IDLE;
    s_playback_buf_len = 0;
    s_playback_pos = 0;
}

// Playback handler - returns CW_Action_t similar to CW_HandleState
CW_Action_t CW_PlaybackHandleState(void)
{
    if (!gCW_PlaybackActive) return CW_ACTION_NONE;

    const uint16_t cur_count = timer_millis_low16();
    CW_Input in = {0};

    // If user hits a paddle during playback, stop playback entirely
    CW_ReadKeys(&in);
    if (in.dit || in.dah) {
        CW_StopPlayback();
        return CW_ACTION_NONE;
    }

    switch (s_pb_state) {
    case PB_STATE_ACTIVE_ELEMENT: {
        const uint16_t target = s_active_is_dit ? s_dit_count : s_dah_count;
        const uint16_t elapsed = timer_millis_low16_since(s_elem_start_count);
        if (elapsed < target) {
            return CW_ACTION_CARRIER_HOLD_ON;
        } else {
            // End element
            s_elem_start_count = cur_count;
            s_pb_state = PB_STATE_INTER_ELEMENT_GAP;
            return CW_ACTION_CARRIER_OFF;
        }
        break;
    }

    case PB_STATE_INTER_ELEMENT_GAP: {
        const uint16_t elapsed = timer_millis_low16_since(s_elem_start_count);
        if (elapsed >= s_gap_count) {
            // Advance to next element in current char
            s_play_elem_index++;
            if (s_play_elem_index < s_play_char_len) {
                // Start next element
                bool is_dit = (((s_play_char_pattern >> s_play_elem_index) & 1) == 0);
                s_active_is_dit = is_dit;
                s_elem_start_count = cur_count;
                s_pb_state = PB_STATE_ACTIVE_ELEMENT;
                return CW_ACTION_CARRIER_ON;
            } else {
                // Char complete - go to char gap
                s_pb_state = PB_STATE_INTER_CHAR_GAP;
                s_elem_start_count = cur_count;
                return CW_ACTION_NONE;
            }
        }
        return CW_ACTION_NONE;
        break;
    }

    case PB_STATE_INTER_CHAR_GAP: {
        const uint16_t elapsed = timer_millis_low16_since(s_elem_start_count);
        if (elapsed < s_char_gap_count) {
            return CW_ACTION_NONE;
        }
        // Move to next character (if any)
        if (s_playback_pos >= s_playback_buf_len) {
            // Done with current pass
            if (gCW_PlaybackRepeat && gEeprom.CW_MESSAGE_REPEAT_DELAY > 0) {
                // Start repeat countdown (value is in 500ms units)
                gCW_MessageRepeatCountdown_500ms = gEeprom.CW_MESSAGE_REPEAT_DELAY * 2;
                gCW_PlaybackActive = false;  // Pause playback while counting down
                s_pb_state = PB_STATE_IDLE;
            } else {
                // No repeat or delay is 0 - just stop
                CW_StopPlayback();
            }
            return CW_ACTION_NONE;
        }
        char ch = s_playback_buf[s_playback_pos++];
        if (ch == ' ') {
            // set pending space for next visible character and play word gap
            s_play_space_pending = true;
            s_pb_state = PB_STATE_INTER_WORD_GAP;
            s_elem_start_count = cur_count;
            return CW_ACTION_NONE;
        }
        // Update TX centerline display with the next char (respect pending space)
        CW_AddToTxDisplay(ch, s_play_space_pending);
        s_play_space_pending = false;

        // Get morse pattern for char 
        uint8_t patt = 0, len = 0;
        if (!CW_GetMorseForChar(ch, &patt, &len)) {
            // Unknown char - skip
            s_pb_state = PB_STATE_INTER_CHAR_GAP;
            s_elem_start_count = cur_count;
            return CW_ACTION_NONE;
        }
        s_play_char_pattern = patt;
        s_play_char_len = len;
        s_play_elem_index = 0;
        // Start first element
        bool is_dit = (((s_play_char_pattern >> s_play_elem_index) & 1) == 0);
        s_active_is_dit = is_dit;
        s_elem_start_count = cur_count;
        s_pb_state = PB_STATE_ACTIVE_ELEMENT;
        return CW_ACTION_CARRIER_ON;
        break;
    }

    case PB_STATE_INTER_WORD_GAP: {
        const uint16_t elapsed = timer_millis_low16_since(s_elem_start_count);
        if (elapsed >= s_word_gap_count) {
            // advance to next char
            s_pb_state = PB_STATE_INTER_CHAR_GAP;
            s_elem_start_count = cur_count;
        }
        return CW_ACTION_NONE;
        break;
    }

    case PB_STATE_IDLE:
    default:
        return CW_ACTION_NONE;
    }
}


void CW_PlaybackIndicatorDeadline(void)
{
    /* Called from APP_TimeSlice10ms() every 10ms. Use a local 10ms-tick counter
     * (kept private to the function) and toggle the indicator every 25 ticks
     * (~250 ms). Keep the indicator off when playback is inactive.
     */
    static uint8_t s_tick_count_10ms = 0;
    const uint8_t TARGET_TICKS = 25; /* ~250 ms */

    if (gCW_PlaybackActive || gCW_MessageRepeatCountdown_500ms > 0) {
        if (++s_tick_count_10ms >= TARGET_TICKS) {
            s_tick_count_10ms = 0;
            gCW_PlayIndicatorOn = !gCW_PlayIndicatorOn;
            gUpdateDisplay = true;
        }
    } else {
        /* Reset counter and clear indicator when not playing */
        s_tick_count_10ms = 0;
        if (gCW_PlayIndicatorOn) {
            gCW_PlayIndicatorOn = false;
            gUpdateDisplay = true;
        }
    }
}

// Check keyer inputs before mode change
// Returns true if inputs are valid (released), false to abort mode change
bool CW_CheckKeyerInputs(uint8_t new_mode)
{    
    // hard deconfig, get all pins in a known state
    CW_KeyerDeinit();
    
    // Handkey mode without port ground doesn't need further validation
    if (new_mode & CW_KEY_FLAG_NO_KEYER && !(new_mode & CW_KEY_FLAG_PORT_GROUND)) {
        return true;
    }
    
    // Determine if we need to configure port pins for this mode (use bit flags)
    bool uses_port_ground = (new_mode & CW_KEY_FLAG_PORT_GROUND);
    bool uses_port_ring = (new_mode & CW_KEY_FLAG_PORT_RING);
    bool uses_adc = (new_mode & CW_KEY_FLAG_ADC);

    // Button-only modes don't need validation (no port pins to check)
    if (!uses_port_ground && !uses_port_ring && !uses_adc) {
        return true;
    }
    
#if CW_KEYER_DEBUG
    UART_Send("Checking CW keyer inputs\r\n", 26);
#endif
    
    // Temporarily configure port pins if needed
    if (uses_port_ground)
    {        
#if CW_KEYER_DEBUG
        UART_Send("Configuring port ground for CW keyer check\r\n", 44);
#endif
        CW_ConfigurePortGround(uses_port_ground);
    }
    if( uses_port_ring) {
#if CW_KEYER_DEBUG
        UART_Send("Configuring port ring for CW keyer check\r\n", 42);
#endif
        CW_ConfigurePortRing(uses_port_ring);
        
    } else if(uses_adc) {
#if CW_KEYER_DEBUG
        UART_Send("Configuring ADC for CW keyer check\r\n", 38);
#endif
        CW_ConfigureADCforCECPaddles(true);
    }

    // Allow pins to stabilize after configuration
    SYSTEM_DelayMs(50);

#if CW_KEYER_DEBUG
    UART_Send("done with config, starting validation\r\n", 40);
#endif

    // Check inputs with 1ms intervals - consider stuck if key stays down for 3 checks out of 20
    int stuck_count = 0;
    bool any_stuck = false;
#if CW_KEYER_DEBUG
    int total_checks = 0;
#endif
    
    for (int i = 0; i < 20; i++) {  // Check up to 20 times = 200ms max
        bool dit = false, dah = false;
        CW_ReadKeysForMode(new_mode, &dit, &dah);
#if CW_KEYER_DEBUG
        total_checks++;
        
        // Debug output every 5 checks
        if (i % 5 == 0) {
            char dbg[50];
            sprintf_(dbg, "check %d: dit=%d dah=%d stuck=%d\r\n", i, dit, dah, stuck_count);
            UART_Send(dbg, strlen(dbg));
        }
#endif
        
        if (dit || dah) {
            stuck_count++;
            if (stuck_count > 2) {  // 3 strikes
                any_stuck = true;
                break;
            }
        }
        
        SYSTEM_DelayMs(10);
    }
    
#if CW_KEYER_DEBUG
    char buf[60];
    sprintf_(buf, "total_checks=%d stuck_count=%d stuck=%d\r\n", 
             total_checks, stuck_count, any_stuck);
    UART_Send(buf, strlen(buf));
#endif

    CW_KeyerDeinit();  // put ports back, disable keyer

    // If no stuck keys detected, mode is valid
    if (!any_stuck) {
#if CW_KEYER_DEBUG
        UART_Send("CW keyer inputs valid\r\n", 24);
#endif
        return true;
    }
#if CW_KEYER_DEBUG
    UART_Send("CW keyer inputs stuck\r\n", 24);
#endif


    return false;
}

CW_Action_t ptt_action(void)
{
    CW_Action_t action = CW_ACTION_NONE;

    // Read PTT button (PC5) - active low
    bool ptt = !(GPIOC->DATA & (1U << GPIOC_PIN_PTT));

    if (ptt && !s_last_handkey_ptt) {
        // PTT pressed
        action = CW_ACTION_CARRIER_ON;
#if CW_KEYER_DEBUG
        UART_Send("handkey PTT on\r\n", 17);
#endif
    } else if (!ptt && s_last_handkey_ptt) {
        // PTT released
        action = CW_ACTION_CARRIER_OFF;
#if CW_KEYER_DEBUG
        UART_Send("handkey PTT off\r\n", 18);
#endif
    } else if (ptt && s_last_handkey_ptt) {
        // PTT being held - keep carrier active
        action = CW_ACTION_CARRIER_HOLD_ON;
    }

    s_last_handkey_ptt = ptt;
    return action;
}

// Manage CW sending - called via main->CW_AppUpdate() every 1 ms.
// Uses timer_millis() to manage deadlines for element duration and spacing.
CW_Action_t CW_HandleState(void)
{
    // Default action: carrier is off and stays off (gap or idle)
    CW_Action_t action = CW_ACTION_NONE;

    // Apply pending init if needed.
    if (s_cfg_dirty && s_KeyerFSMState == CWK_STATE_IDLE) {
        CW_KeyerInit();
        return CW_ACTION_NONE;
    } else if(s_KeyerFSMState == CWK_STATE_EMIT_NONE || !s_enable_keyer) {
        s_KeyerFSMState = CWK_STATE_IDLE;
        return CW_ACTION_NONE;
    }

    const uint16_t cur_count = timer_millis_low16();

    // Check if keyer is disabled (handkey modes have NO_KEYER flag set)
    if (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER) {
        return ptt_action();
    }

    // Input struct - will be sampled at appropriate times in each state
    CW_Input in = {0};

    switch (s_KeyerFSMState) {
    ///
    ///  IDLE
    ///
    case CWK_STATE_IDLE:
        // IDLE: Sample continuously for any key press
        CW_ReadKeys(&in);
#if CW_KEYER_DEBUG
        if (in.dit || in.dah) {
            char buf[60];
            sprintf_(buf, "%u IDLE sees dit=%d dah=%d\r\n", (unsigned)cur_count, in.dit, in.dah);
            UART_Send(buf, strlen(buf));
        }
        static uint32_t idle_count = 0;
        if (idle_count++ % 3000 == 0)
            UART_Send("keyer is idle\r\n", 15);
#endif
        if (in.dit || in.dah) {
            // Explicit handling when both paddles are pressed:
            // If both pressed and previous element was a dit, choose dah; otherwise choose dit.
            if (in.dit && !in.dah) {
                s_active_is_dit = true;
            } else if (in.dah && !in.dit) {
                s_active_is_dit = false;
            } else {
                // both pressed -> toggle previous element type (dit->dah, dah->dit)
                s_active_is_dit = !s_active_is_dit;
            }

            s_pending_alternate = false;
            s_elem_start_count = cur_count;
            s_KeyerFSMState = CWK_STATE_ACTIVE_ELEMENT;
#if CW_KEYER_DEBUG
            UART_Send("keyer going active\r\n", 20);
#endif
            action = CW_ACTION_CARRIER_ON;
        }
        break;

    ///
    ///  ACTIVE
    ///
    case CWK_STATE_ACTIVE_ELEMENT:
        const uint32_t target = s_active_is_dit ? s_dit_count : s_dah_count;
        const uint32_t elapsed_elem = timer_millis_low16_since(s_elem_start_count);

#if CW_KEYER_DEBUG
        {
            static uint32_t debug_count = 0;
            if (++debug_count % 10 == 0) {  // Print every 10th iteration
                char buf[80];
                sprintf_(buf, "elem: elapsed=%u target=%u start=%u cur=%u\r\n", 
                         elapsed_elem, target, s_elem_start_count, (unsigned)cur_count);
                UART_Send(buf, strlen(buf));
            }
        }
#endif
        // Sample paddles for memory logic - skip if we already know alternate is pending
        if(!s_pending_alternate)
        {
            CW_ReadKeys(&in);
        
            // Memory logic: depends on keyer mode and element type
            if (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_A) {
                // Type A: Purely edge detection for opposite paddle throughout element
                if (s_active_is_dit && in.dah_rise) {
                    s_pending_alternate = true;
                } else if (!s_active_is_dit && in.dit_rise) {
                    s_pending_alternate = true;
                }
            } else {
                // "Elecraft style" Type B with an edge-trigger during the first third of a dah
                // (so holding the alternate on the way into the element won't trigger it)
                if ((!s_active_is_dit) && (elapsed_elem < s_dit_count)) {
                    if (in.dit_rise) {
                        s_pending_alternate = true;
                    }
                } else {
                    // Standard Type B logic: state detection
                    if (s_active_is_dit && in.dah) {
                        s_pending_alternate = true;
                    } else if (!s_active_is_dit && in.dit) {
                        s_pending_alternate = true;
                    }
                }
            }
        }

        if (elapsed_elem >= target) {
            action = CW_ACTION_CARRIER_OFF;
            s_elem_start_count = cur_count;
            // Emit element to encoder on state exit
            CW_EncoderProcessElement(s_active_is_dit ? CW_ELEMENT_DIT : CW_ELEMENT_DAH);
            s_KeyerFSMState = CWK_STATE_INTER_ELEMENT_GAP;
        } else {
            // Carrier is on and should remain on during element
            action = CW_ACTION_CARRIER_HOLD_ON;
        }
        break;

    ///
    ///   ELEMENT GAP
    ///
    case CWK_STATE_INTER_ELEMENT_GAP: {
        const uint16_t elapsed_gap = timer_millis_low16_since(s_elem_start_count);
        
        // Read only if needed
        if(!s_pending_alternate) // briand - lets try doing this regardless of mode // && (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_A))
        {
            // keep doing sampling during gap for memory logic
            CW_ReadKeys(&in);
        
            // Mode A style Edge detection for opposite key throughout element AND gap, but for both modes
            if (s_active_is_dit && in.dah_rise) {
                s_pending_alternate = true;
            } else if (!s_active_is_dit && in.dit_rise) {
                s_pending_alternate = true;
            }
            // I think we don't want B reading during gap? this was probably the double-dit problem.
            // else {
            //     // Standard Type B logic: state detection
            //     if (s_active_is_dit && in.dah) {
            //         s_pending_alternate = true;
            //     } else if (!s_active_is_dit && in.dit) {
            //         s_pending_alternate = true;
            //     }
            // }
        }
        if (elapsed_gap >= s_gap_count) {

            bool next_is_dit = false;
            bool have_next = false;

            // alternating is already decided
            if(s_pending_alternate)
            {
                if(!s_active_is_dit)
                    next_is_dit = true;
                have_next = true;
            }
            else  // no pending alternate
            {            
                // Gap complete with no alternate pending - take a sample
                CW_ReadKeys(&in);

                if (in.dit || in.dah) {
                    // Explicit handling: only create a dit if dit was actually pressed.
                    // If both paddles are pressed, choose the opposite of the prior element:
                    // prior dit -> choose dah, otherwise choose dit.
                    if (in.dit && !in.dah) {
                        next_is_dit = true;
                    } else if (in.dah && !in.dit) {
                        next_is_dit = false;
                    } else { /* both pressed -> choose opposite of previous */
                        next_is_dit = !s_active_is_dit;
                    }
                    have_next = true;
                }
            } 

            s_pending_alternate = false;

            if (have_next) {
                s_elem_start_count = cur_count;  // start new count for the new element
                s_KeyerFSMState = CWK_STATE_ACTIVE_ELEMENT;
                s_active_is_dit = next_is_dit;
                // Sample now and log if this creates a dit while only dah is pressed
                CW_ReadKeys(&in);
                action = CW_ACTION_CARRIER_ON;				
            } else {
                // No key input - transition to inter-char gap (carry over timing)
                s_KeyerFSMState = CWK_STATE_INTER_CHAR_GAP;
            }
        }
		break;
	}

    ///
    ///    CHAR GAP
    ///
	case CWK_STATE_INTER_CHAR_GAP: {
		const uint16_t elapsed_gap = timer_millis_low16_since(s_elem_start_count);

		if (elapsed_gap < s_char_gap_count) {  // until char gap complete

            CW_ReadKeys(&in);
            bool have_key = (in.dit || in.dah);

            if (elapsed_gap < s_ext_gap_count) {
			    // Early period: immediate send, same character continues
                if (have_key) {
                    // If both pressed: choose the opposite of the prior element (if prior was dit, send dah; otherwise send dit)
                    if (in.dit && !in.dah) {
                        s_active_is_dit = true;
                    } else if (in.dah && !in.dit) {
                        s_active_is_dit = false;
                    } else { // both pressed -> toggle previous
                        s_active_is_dit = !s_active_is_dit;
                    }

                    s_elem_start_count = cur_count;
                    s_KeyerFSMState = CWK_STATE_ACTIVE_ELEMENT;
                    action = CW_ACTION_CARRIER_ON;
                }
		    } else {
			    // Hold period (ext_gap <= elapsed < char_gap): queue key but wait for char_gap deadline
			    if (have_key && !s_pending_alternate) {
				    s_pending_alternate = true;
				    s_active_is_dit = in.dit;
			    }
            }

		} else {
			// Char gap complete - character boundary reached
			CW_EncoderProcessElement(CW_ELEMENT_INTER_CHAR_SPACE);
			if (s_pending_alternate) {
				// Send queued key now because the gap is over
				s_pending_alternate = false;
				s_elem_start_count = cur_count;
				s_KeyerFSMState = CWK_STATE_ACTIVE_ELEMENT;
				action = CW_ACTION_CARRIER_ON;
			} else {
				s_KeyerFSMState = CWK_STATE_INTER_WORD_GAP;
			}
		}
		break;
	}

    ///
    ///   WORD GAP
    ///
	case CWK_STATE_INTER_WORD_GAP: {
		// Post char-gap: monitor and send immediately, or goto idle at word_gap
		const uint16_t elapsed_gap = timer_millis_low16_since(s_elem_start_count);
		CW_ReadKeys(&in);
		
		if (in.dit || in.dah) {
            s_active_is_dit = in.dit;
			s_elem_start_count = cur_count;
			s_KeyerFSMState = CWK_STATE_ACTIVE_ELEMENT;
			action = CW_ACTION_CARRIER_ON;
		} else if (elapsed_gap >= s_word_gap_count) {
			CW_EncoderProcessElement(CW_ELEMENT_INTER_WORD_SPACE);
			s_KeyerFSMState = CWK_STATE_IDLE;
		}
		break;
	}

	default:
		s_KeyerFSMState = CWK_STATE_IDLE;
		break;
	}

	return action;
}
