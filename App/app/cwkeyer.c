// CW Iambic Keyer implementation
// Lean FSM for iambic A/B with optional reversed mapping

#include <stdint.h>
#include <stdbool.h>

#include "app/cwkeyer.h"
#include "settings.h"
#include "bsp/dp32g030/timer.h"
#include "bsp/dp32g030/gpio.h"
#include "bsp/dp32g030/portcon.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/timer.h"
#include "driver/i2c.h"

// Timer scale: 50 kHz tick → 20 µs per tick
// 16-bit counter rolls over at 1310 ms, enough for largest inter-word space
#define TICKS_PER_MS      50U
#define TICKS_PER_MINUTE  (60000U * TICKS_PER_MS)
#define DITS_PER_WORD     50U

// Sampling threshold (in timer ticks) for key scans; set by init
static uint32_t s_sample_thresh = TICKS_PER_MS; // ~1 ms

// Externs required by header
volatile CW_KeyerFSMState_t gCW_KeyerFSMState = CWK_STATE_IDLE;

// Internal keyer runtime state
static bool           s_iambic_b = false;  // true=B, false=A
static uint16_t       s_dit_cnt  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_dah_cnt  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_gap_cnt  = 0;      // inter-element gap in ticks (1 dit)
static uint16_t       s_last_cnt = 0;      // last TIMERBASE0_LOW_CNT sample (16-bit)
static uint16_t       s_elem_start_cnt = 0;// element start counter (16-bit)
static bool           s_active_is_dit = false;
static bool           s_pending_alternate = false; // alternate element queude
static bool           s_both_held_during_elem = false; // iambic-A detection
static bool           s_reverse_keys = false; // normalized mapping flag
static bool           s_last_dit = false, s_last_dah = false; // last sampled paddles

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = false;

// Compute delta with wrap-around protection for a 16-bit up-counter
static inline uint16_t cw_delta_counts(uint16_t prev, uint16_t cur)
{
    return (cur >= prev) ? (uint16_t)(cur - prev) : (uint16_t)(UINT16_MAX - prev + 1U + cur);
}

// Input struct (normalized paddles + edges)
typedef struct {
    bool dit;
    bool dah;
    bool dit_rise;
    bool dit_fall;
    bool dah_rise;
    bool dah_fall;
} CW_Input;

// Read button inputs (PTT and SIDE1)
static void CW_ReadButtons(bool *dit_out, bool *dah_out)
{
    // Read PTT button (PC5) - active low
    bool ptt = !(GPIOC->DATA & (1U << GPIOC_PIN_PTT));
    
    // Read SIDE1 button (PA3) with de-noise
    // Set keyboard matrix pins high
    GPIOA->DATA |= (1U << GPIOA_PIN_KEYBOARD_4) |
                   (1U << GPIOA_PIN_KEYBOARD_5) |
                   (1U << GPIOA_PIN_KEYBOARD_6) |
                   (1U << GPIOA_PIN_KEYBOARD_7);
    
    // De-noise SIDE1 (PA3) - active low when pressed
    bool side1 = false;
    uint16_t reg = 0, reg2;
    unsigned int i, k;
    
    for (i = 0, k = 0, reg = 0; i < 3 && k < 8; i++, k++) {
        SYSTICK_DelayUs(1);
        reg2 = GPIOA->DATA & (1U << GPIOA_PIN_KEYBOARD_0);
        i *= (reg == reg2);  // Reset i if readings differ
        reg = reg2;
    }
    
    if (i >= 3) {
        // Stable reading achieved
        side1 = !reg;  // Active low
    }
    
    // Create I2C stop condition since we might have toggled I2C pins
	// This leaves GPIOA_PIN_KEYBOARD_4 and GPIOA_PIN_KEYBOARD_5 high
	I2C_Stop();

	// Reset VOICE pins
	GPIO_ClearBit(&GPIOA->DATA, GPIOA_PIN_KEYBOARD_6);
	GPIO_SetBit(  &GPIOA->DATA, GPIOA_PIN_KEYBOARD_7);

    *dit_out = side1;
    *dah_out = ptt;
}

// Read port inputs (PA7 tip and PB15 sleeve)
static void CW_ReadPort(bool *dit_out, bool *dah_out)
{
    // PA7 is configured as output, drive low - other side pulls high when open
    // Read PB15 (sleeve) - active low when paddle closes to ground
    bool sleeve = !(GPIOB->DATA & (1U << 15));
    
    // For tip (PA7), we need to briefly make it an input to read it
    // This is the "tip" line - when paddle closes, it pulls to ground
    uint32_t saved_dir = GPIOA->DIR;
    GPIOA->DIR &= ~GPIO_DIR_7_MASK;  // Temporarily input
    SYSTICK_DelayUs(1);  // Brief settling time
    bool tip = !(GPIOA->DATA & (1U << 7));  // Active low
    GPIOA->DIR = saved_dir;  // Restore output
    
    *dit_out = sleeve;
    *dah_out = tip;
}

// Read GPIO inputs based on configured mode
static void CW_ReadKeys(CW_Input *in)
{
    bool hw_dit = false;
    bool hw_dah = false;
    
    // Read inputs based on configured mode
    switch (gEeprom.CW_KEY_INPUT) {
        case CW_KEY_INPUT_BUTTONS_NORMAL:
        case CW_KEY_INPUT_BUTTONS_REVERSED:
            CW_ReadButtons(&hw_dit, &hw_dah);
            break;
            
        case CW_KEY_INPUT_PORT_NORMAL:
        case CW_KEY_INPUT_PORT_REVERSED:
            CW_ReadPort(&hw_dit, &hw_dah);
            break;
            
        case CW_KEY_INPUT_BOTH_NORMAL:
        case CW_KEY_INPUT_BOTH_REVERSED: {
            bool btn_dit, btn_dah, port_dit, port_dah;
            CW_ReadButtons(&btn_dit, &btn_dah);
            CW_ReadPort(&port_dit, &port_dah);
            // OR both inputs together
            hw_dit = btn_dit || port_dit;
            hw_dah = btn_dah || port_dah;
            break;
        }
            
        case CW_KEY_INPUT_HANDKEY:
        default:
            // No keyer input
            break;
    }

    // Normalize mapping (reverse swaps paddles)
    bool n_dit = s_reverse_keys ? hw_dah : hw_dit;
    bool n_dah = s_reverse_keys ? hw_dit : hw_dah;

    in->dit_rise = (!s_last_dit && n_dit);
    in->dit_fall = (s_last_dit && !n_dit);
    in->dah_rise = (!s_last_dah && n_dah);
    in->dah_fall = (s_last_dah && !n_dah);
    in->dit = n_dit;
    in->dah = n_dah;

    s_last_dit = n_dit;
    s_last_dah = n_dah;
}

// Configure port pins for tip/sleeve paddle input
static void CW_ConfigurePortPins(bool enable_port)
{
    if (enable_port) {
        // Configure PA7 as GPIO output (tip - dah/dit depending on reversal)
        PORTCON_PORTA_SEL0 &= ~PORTCON_PORTA_SEL0_A7_MASK;
        PORTCON_PORTA_SEL0 |= PORTCON_PORTA_SEL0_A7_BITS_GPIOA7;
        GPIOA->DIR |= GPIO_DIR_7_BITS_OUTPUT;
        GPIOA->DATA &= ~(1U << 7); // Set low
        
        // Configure PB15 as GPIO input (sleeve - dit/dah depending on reversal)
        PORTCON_PORTB_SEL1 &= ~PORTCON_PORTB_SEL1_B15_MASK;
        PORTCON_PORTB_SEL1 |= PORTCON_PORTB_SEL1_B15_BITS_GPIOB15;
        GPIOB->DIR &= ~GPIO_DIR_15_MASK; // Set as input
    } else {
        // Configure PA7 back to UART1 TX
        PORTCON_PORTA_SEL0 &= ~PORTCON_PORTA_SEL0_A7_MASK;
        PORTCON_PORTA_SEL0 |= PORTCON_PORTA_SEL0_A7_BITS_UART1_TX;
        // DIR automatically handled by UART peripheral
        
        // Configure PB15 as GPIO output, set high
        PORTCON_PORTB_SEL1 &= ~PORTCON_PORTB_SEL1_B15_MASK;
        PORTCON_PORTB_SEL1 |= PORTCON_PORTB_SEL1_B15_BITS_GPIOB15;
        GPIOB->DIR |= GPIO_DIR_15_BITS_OUTPUT;
        GPIOB->DATA |= (1U << 15); // Set high
    }
}

// Initialize keyer from gEeprom settings
static void CW_KeyerInit()
{
    // Configure timer for 50 kHz tick (48 MHz / 960)
    // TIMERBASE0_LOW_CNT is 16-bit and rolls over at 0xFFFF (~1310 ms)
    TIM0_INIT(960U - 1U, 0xffff, false);

    const uint32_t wpm = gEeprom.CW_KEY_WPM;
    const uint32_t dit_ticks = TICKS_PER_MINUTE / (wpm * DITS_PER_WORD);
    const uint32_t dah_ticks = 3U * dit_ticks;

    s_dit_cnt = (uint16_t)dit_ticks;
    s_dah_cnt = (uint16_t)dah_ticks;
    s_gap_cnt = (uint16_t)dit_ticks; // inter-element gap = 1 dit

    // Load settings from gEeprom
    s_iambic_b = (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_B);
    
    // Determine if keys are reversed based on key input type
    // Every even-numbered input type is the reversed version
    // (except HANDKEY which is not reversed, but doesn't matter)
    s_reverse_keys = (gEeprom.CW_KEY_INPUT & 1) ? false : true;
    
    // Configure port pins based on whether port input is used
    bool uses_port = (gEeprom.CW_KEY_INPUT == CW_KEY_INPUT_PORT_NORMAL ||
                      gEeprom.CW_KEY_INPUT == CW_KEY_INPUT_PORT_REVERSED ||
                      gEeprom.CW_KEY_INPUT == CW_KEY_INPUT_BOTH_NORMAL ||
                      gEeprom.CW_KEY_INPUT == CW_KEY_INPUT_BOTH_REVERSED);
    CW_ConfigurePortPins(uses_port);

    s_last_cnt         = (uint16_t)TIMERBASE0_LOW_CNT;
    s_elem_start_cnt   = s_last_cnt;
    s_active_is_dit    = false;
    s_pending_alternate = false;
    s_both_held_during_elem = false;
    s_last_dit = false;
    s_last_dah = false;

    gCW_KeyerFSMState = CWK_STATE_IDLE;
    s_cfg_dirty = false;
}

void CW_KeyerReconfigure(void)
{
    s_cfg_dirty = true;
}

CW_Action_t CW_HandleState(void)
{
    CW_Action_t actions = CW_ACTION_NONE;

    // Check if keyer is enabled (any mode other than HANDKEY)
    if (gEeprom.CW_KEY_INPUT == CW_KEY_INPUT_HANDKEY) {
        gCW_KeyerFSMState = CWK_STATE_IDLE;
        return actions;
    }

    // Check dirty flag at idle - reconfigure if needed
    if (s_cfg_dirty && gCW_KeyerFSMState == CWK_STATE_IDLE) {
        CW_KeyerInit();
        return actions;
    }

    const uint16_t cur_cnt = (uint16_t)TIMERBASE0_LOW_CNT;
    const uint16_t delta_since_last = cw_delta_counts(s_last_cnt, cur_cnt);
    if (delta_since_last < s_sample_thresh) {
        return actions;
    }
    s_last_cnt = cur_cnt;

    CW_Input in;
    CW_ReadKeys(&in);

    switch (gCW_KeyerFSMState) {
    case CWK_STATE_IDLE:
        if (in.dit || in.dah) {
            if (in.dit && in.dah) {
                // Both pressed: start with dit (normal) or dah (reversed)
                s_active_is_dit = !s_reverse_keys;
                s_both_held_during_elem = true;
            } else {
                s_active_is_dit = in.dit;
                s_both_held_during_elem = false;
            }

            s_pending_alternate = false;
            s_elem_start_cnt = cur_cnt;
            gCW_KeyerFSMState = s_active_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
            actions = CW_ACTION_CARRIER_ON;
        }
        break;

    case CWK_STATE_ACTIVE_DIT:
    case CWK_STATE_ACTIVE_DAH: {
        const uint16_t target = (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DIT) ? s_dit_cnt : s_dah_cnt;
        const uint16_t elapsed_elem = cw_delta_counts(s_elem_start_cnt, cur_cnt);

        // Iambic alternation detection
        if (in.dit && in.dah) {
            s_both_held_during_elem = true;
        }
        
        // Iambic B: detect opposite paddle press during element
        if (s_iambic_b) {
            if (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DIT && in.dah) {
                s_pending_alternate = true;
            } else if (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DAH && in.dit) {
                s_pending_alternate = true;
            }
        }

        if (elapsed_elem >= target) {
            actions = CW_ACTION_CARRIER_OFF;
            s_elem_start_cnt = cur_cnt;
            gCW_KeyerFSMState = CWK_STATE_INTER_ELEMENT_GAP;
        } else {
            // Continue holding PTT during active element
            // lets try without - use CARRIER_ON as a transitional state only
            //actions = CW_ACTION_CARRIER_ON;
        }
        break; }

    case CWK_STATE_INTER_ELEMENT_GAP: {
        const uint16_t elapsed_gap = cw_delta_counts(s_elem_start_cnt, cur_cnt);
        
        // Check dirty flag during inter-element gap
        if (s_cfg_dirty) {
            CW_KeyerInit();
            return actions;
        }
        
        if (elapsed_gap >= s_gap_cnt) {
            bool next_is_dit = false;
            bool have_next = false;

            // Iambic A: only alternate if both were held during element and still held
            if (!s_iambic_b) {
                if (s_both_held_during_elem && (in.dit || in.dah)) {
                    next_is_dit = !s_active_is_dit; // alternate
                    have_next = true;
                }
            } else {
                // Iambic B: use pending flag or check current state
                if (s_pending_alternate) {
                    next_is_dit = !s_active_is_dit;
                    have_next = true;
                } else if (in.dit || in.dah) {
                    if (in.dit && in.dah) {
                        next_is_dit = !s_reverse_keys;
                    } else {
                        next_is_dit = in.dit;
                    }
                    have_next = true;
                }
            }

            s_pending_alternate = false;
            s_both_held_during_elem = false;
            s_elem_start_cnt = cur_cnt;

            if (have_next) {
                gCW_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
                s_active_is_dit   = next_is_dit;
                actions = CW_ACTION_CARRIER_ON;
            } else {
                gCW_KeyerFSMState = CWK_STATE_IDLE;
                // Already in NONE state, carrier was turned off at end of last element
            }
        }
        break; }

    default:
        gCW_KeyerFSMState = CWK_STATE_IDLE;
        break;
    }

    return actions;
}
