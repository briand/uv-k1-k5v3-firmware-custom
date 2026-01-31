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
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/timer.h"
#include "bsp/dp32g030/gpio.h"
#include "bsp/dp32g030/portcon.h"
#include "bsp/dp32g030/uart.h"
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

// Debug: log when a dit is created while only dah is pressed
#ifndef CW_KEYER_DEBUG_DAH_SEND
#define CW_KEYER_DEBUG_DAH_SEND 1
#endif

// Timer scale: 10 kHz tick → 100 µs per tick
// 16-bit counter rolls over at 6553 ms
#define DITS_PER_WORD 50
#define TICKS_PER_MS 10
#define TICKS_PER_MINUTE (60 * 1000 * TICKS_PER_MS)  // 600,000

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_ELEMENT,
    CWK_STATE_INTER_ELEMENT_GAP,
    CWK_STATE_INTER_CHAR_GAP,    // Extended gap = end of character
    CWK_STATE_INTER_WORD_GAP,    // Long gap = inter-word space
} CW_KeyerFSMState_t;

static CW_KeyerFSMState_t s_KeyerFSMState = CWK_STATE_IDLE;

// Internal keyer runtime state
static uint16_t       s_dit_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_dah_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_gap_count  = 0;      // inter-element gap in ticks (1 dit)
static uint16_t       s_ext_gap_count = 0;       // tick count when auto-char extension kicks in (16-bit)
static uint16_t       s_char_gap_count = 0;  // inter-char gap in ticks (3 dits)
static uint16_t       s_word_gap_count = 0;  // inter-word gap in ticks (7 dits)
static uint16_t       s_last_count = 0;      // last TIMERBASE0_LOW_COUNT sample (16-bit)
static uint16_t       s_elem_start_count = 0;// element start counter (16-bit)
static bool           s_active_is_dit = false;
static bool           s_pending_alternate = false; // alternate element queued
/* last sampled paddles moved to app/cwhardware.c */
static bool           s_last_handkey_ptt = false; // last PTT state for handkey mode

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = true;


void CW_UpdateWPM()
{
    // TIMERBASE0_LOW_COUNT is 16-bit 10 kHz tick and rolls over at 0xFFFF (~6553 ms)
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
    CW_UpdateWPM();

    // Load settings from gEeprom

    // Configure port pins based on bit flags
    bool uses_port_ground = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_GROUND);
    bool uses_port_ring = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_RING);
    
#if CW_KEYER_DEBUG
    bool is_handkey = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER);
    bool uses_buttons = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_BUTTONS);
    char buf[120];
    sprintf_(buf, "CW_Init: mode=0x%02X handkey=%d btns=%d pG=%d pR=%d rev=%d\r\n",
             gEeprom.CW_KEY_INPUT, is_handkey, uses_buttons, uses_port_ground, uses_port_ring, (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_REVERSED));
    UART_Send(buf, strlen(buf));
#endif
    
    CW_ConfigurePortGround(uses_port_ground);
    CW_ConfigurePortRing(uses_port_ring);

    s_last_count         = (uint16_t)TIMERBASE0_LOW_CNT;
    s_active_is_dit    = false;
    CW_HW_ResetKeySamples();

    s_KeyerFSMState = CWK_STATE_IDLE;
    s_cfg_dirty = false;
#if CW_KEYER_DEBUG
    UART_Send("keyer init done\r\n", 17);
#endif
}

void CW_KeyerReconfigure(void)
{
    s_cfg_dirty = true;
#if CW_KEYER_DEBUG
    UART_Send("keyer marked for reconfig\r\n", 27);
#endif
}

// Check keyer inputs before mode change
// Returns true if inputs are valid (released), false to abort mode change
bool CW_CheckKeyerInputs(uint8_t new_mode)
{
    // Handkey mode doesn't need validation (no keyer flag set)
    if (new_mode & CW_KEY_FLAG_NO_KEYER) {
        return true;
    }
    
    // Determine if we need to configure port pins for this mode (use bit flags)
    bool uses_port_ground = (new_mode & CW_KEY_FLAG_PORT_GROUND);
    bool uses_port_ring = (new_mode & CW_KEY_FLAG_PORT_RING);
    
    // Button-only modes don't need validation (no port pins to check)
    if (!uses_port_ground && !uses_port_ring) {
        return true;
    }
    
#if CW_KEYER_DEBUG
    UART_Send("Checking CW keyer inputs\r\n", 26);
#endif
    
    // Temporarily configure port pins if needed
    if (uses_port_ground || uses_port_ring) {
#if CW_KEYER_DEBUG
        UART_Send("Configuring port pins for CW keyer check\r\n", 42);
#endif
        CW_ConfigurePortGround(uses_port_ground);
        CW_ConfigurePortRing(uses_port_ring);
        
        // Allow pins to stabilize after configuration
        SYSTEM_DelayMs(50);
    }
#if CW_KEYER_DEBUG
    UART_Send("done with config, starting validation\r\n", 40);
#endif

    // Check inputs with 10ms intervals - consider stuck if key stays down for over 10 consecutive checks
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
            if (stuck_count > 10) {
                any_stuck = true;
                break;
            }
        } else {
            stuck_count = 0;  // Reset if keys released
        }
        
        SYSTEM_DelayMs(10);
    }
    
#if CW_KEYER_DEBUG
    char buf[60];
    sprintf_(buf, "total_checks=%d stuck_count=%d stuck=%d\r\n", 
             total_checks, stuck_count, any_stuck);
    UART_Send(buf, strlen(buf));
#endif

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

    CW_ConfigurePortGround(false);
    CW_ConfigurePortRing(false);

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

CW_Action_t CW_HandleState(void)
{
    // Default action: carrier is off and stays off (gap or idle)
    CW_Action_t action = CW_ACTION_NONE;

#if CW_KEYER_DEBUG
    static CW_KeyerFSMState_t last_logged_state = CWK_STATE_IDLE;
#endif

    // check dirty flag at idle - reconfigure only when safe
    if (s_cfg_dirty && s_KeyerFSMState == CWK_STATE_IDLE) {
        CW_KeyerInit();
    }

    const uint16_t cur_count = (uint16_t)TIMERBASE0_LOW_CNT;
    const uint16_t delta_since_last = timer_jiffies_since(s_last_count);
    if (delta_since_last < TICKS_PER_MS) {
        // Not enough time has passed - return appropriate action for current state
        // ACTIVE states: hold carrier on, GAP/IDLE states: no action (carrier off)
        if (s_KeyerFSMState == CWK_STATE_ACTIVE_ELEMENT) {
            return CW_ACTION_CARRIER_HOLD_ON;
        }
        return CW_ACTION_NONE;
    }
    s_last_count = cur_count;

#if CW_KEYER_DEBUG
    // Log state changes
    if (s_KeyerFSMState != last_logged_state) {
        const char* state_names[] = {"IDLE", "ACTIVE_ELEMENT", "INTER_ELEM", "INTER_CHAR", "INTER_WORD"};
        char buf[60];
        sprintf_(buf, "STATE: %s -> %s (%s)\r\n", state_names[last_logged_state], state_names[s_KeyerFSMState], s_active_is_dit ? "dit" : "dah");
        UART_Send(buf, strlen(buf));
        last_logged_state = s_KeyerFSMState;
    }
#endif

    // Check if keyer is disabled (handkey modes have NO_KEYER flag set)
    if (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER) {
#if CW_KEYER_DEBUG
        static uint32_t handkey_log_count = 0;
        if (++handkey_log_count % 1000 == 0) {
            UART_Send("CW in handkey mode\r\n", 20);
        }
#endif
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
#if CW_KEYER_DEBUG
            UART_Send("entered if block\r\n", 18);
#endif
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
        const uint16_t target = s_active_is_dit ? s_dit_count : s_dah_count;
        const uint16_t elapsed_elem = timer_jiffies_since(s_elem_start_count);

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
        const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);
        
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
		const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);

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
		const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);
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
