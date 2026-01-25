// CW Iambic Keyer implementation
// Lean FSM for iambic A/B with optional reversed mapping

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "app/cwkeyer.h"
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

// Timer scale: 10 kHz tick → 100 µs per tick
// 16-bit counter rolls over at 6553 ms
#define DITS_PER_WORD 50
#define TICKS_PER_MS 10
#define TICKS_PER_MINUTE (60 * 1000 * TICKS_PER_MS)  // 600,000

// Sampling threshold (in timer ticks) for key scans; set by init
static uint32_t s_sample_thresh = TICKS_PER_MS; // ~1 ms

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_DIT,
    CWK_STATE_ACTIVE_DAH,
    CWK_STATE_INTER_ELEMENT_GAP,
} CW_KeyerFSMState_t;

static CW_KeyerFSMState_t s_KeyerFSMState = CWK_STATE_IDLE;

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
static bool           s_last_handkey_ptt = false; // last PTT state for handkey mode

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = true;

// Input struct (normalized paddles + edges)
typedef struct {
    bool dit;
    bool dah;
    bool dit_rise;
    bool dit_fall;
    bool dah_rise;
    bool dah_fall;
} CW_Input;

// Read button ring input (SIDE1)
static void CW_ReadSideButton(bool *ring_out)
{
    // Read SIDE1 button (PA3) as ring with de-noise
    // Set keyboard matrix pins high
    GPIOA->DATA |= (1U << GPIOA_PIN_KEYBOARD_4) |
                   (1U << GPIOA_PIN_KEYBOARD_5) |
                   (1U << GPIOA_PIN_KEYBOARD_6) |
                   (1U << GPIOA_PIN_KEYBOARD_7);
    
    // De-noise SIDE1 (PA3) - active low when pressed
    bool ring = false;
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
        ring = !reg;  // Active low
    }
    
    // Create I2C stop condition since we might have toggled I2C pins
	// This leaves GPIOA_PIN_KEYBOARD_4 and GPIOA_PIN_KEYBOARD_5 high
	I2C_Stop();

	// Reset VOICE pins
	GPIO_ClearBit(&GPIOA->DATA, GPIOA_PIN_KEYBOARD_6);
	GPIO_SetBit(  &GPIOA->DATA, GPIOA_PIN_KEYBOARD_7);

    *ring_out = ring;
}

// Read raw paddle inputs for a specific mode
// Returns true if mode is valid, false otherwise
static bool CW_ReadKeysForMode(uint8_t mode, bool *dit_out, bool *dah_out)
{
    // Check if keyer is disabled (handkey modes)
    if (mode & CW_KEY_FLAG_NO_KEYER) {
        return false;
    }
    
    // Read PTT (PC5) as tip - shared across button and port configs
    bool hw_tip = !(GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT));
    bool hw_ring = false;
    
    // Read button ring input if enabled
    if (mode & CW_KEY_FLAG_BUTTONS) {
        bool btn_ring = false;
        CW_ReadSideButton(&btn_ring);
        hw_ring = btn_ring;
    }
    
    // Read port ring input if enabled and OR with button ring
    if (mode & CW_KEY_FLAG_PORT_RING) {
        bool port_ring = !(GPIO_CheckBit(&GPIOB->DATA, GPIOB_PIN_BK1080));
        hw_ring = hw_ring || port_ring;  // OR both sources
    }
    
    // Determine if keys are reversed
    bool reverse = (mode & CW_KEY_FLAG_REVERSED);
    
    // Map tip/ring to dit/dah based on reversed flag
    *dit_out = reverse ? hw_tip : hw_ring;
    *dah_out = reverse ? hw_ring : hw_tip;
    
    // Log key states
    // bool pb15_is_output = (GPIOB->DIR & GPIO_DIR_15_MASK) != 0;
    // char buf[80];
    // sprintf_(buf, "mode=%u tip=%d ring=%d PB15_out=%d -> dit=%d dah=%d\r\n", 
    //          mode, hw_tip, hw_ring, pb15_is_output, *dit_out, *dah_out);
    //UART_Send(buf, strlen(buf));
    
    return true;
}

// Read GPIO inputs based on configured mode
static void CW_ReadKeys(CW_Input *in)
{
    static int times_called = 0;
    if(++times_called % 1000 == 0) {
        //UART_Send("Reading keys\r\n", 14);
    }
    
    bool n_dit = false;
    bool n_dah = false;
    
    // Read inputs using helper function
    if (!CW_ReadKeysForMode(gEeprom.CW_KEY_INPUT, &n_dit, &n_dah)) {
        // Handkey mode or invalid - no keyer input
        n_dit = false;
        n_dah = false;
    }

    // Compute edges
    in->dit_rise = (!s_last_dit && n_dit);
    in->dit_fall = (s_last_dit && !n_dit);
    in->dah_rise = (!s_last_dah && n_dah);
    in->dah_fall = (s_last_dah && !n_dah);
    in->dit = n_dit;
    in->dah = n_dah;

    s_last_dit = n_dit;
    s_last_dah = n_dah;
}

// Configure port ground pin (PA8) for tip/ring paddle input
// When enabled: PA8 becomes GPIO output low (acts as ground for paddle port)
// When disabled: PA8 returns to UART1 RX function with DMA
static void CW_ConfigurePortGround(bool enable)
{
    if (enable) {
        // Disable UART RX and RX DMA to prevent unwanted DMA transfers while PA8 is GPIO
        UART1->CTRL &= ~(UART_CTRL_RXEN_MASK | UART_CTRL_RXDMAEN_MASK);
        
        // Disable DMA Channel 0
        DMA_CH0->CTR &= ~DMA_CH_CTR_CH_EN_MASK;
        
        // Clear any pending UART RX flags
        UART1->IF = UART_IF_RXTO_BITS_SET | UART_IF_RXFIFO_FULL_BITS_SET;
        
        // Configure PA8 (UART RX) as a GPIO low output (sleeve - acts as ground for the paddle port input)
        PORTCON_PORTA_SEL1 &= ~PORTCON_PORTA_SEL1_A8_MASK;
        //PORTCON_PORTA_SEL1 |= PORTCON_PORTA_SEL1_A8_BITS_GPIOA8;
        GPIOA->DIR |= GPIO_DIR_8_BITS_OUTPUT;
        GPIO_ClearBit(&GPIOA->DATA, GPIOA_PIN_UART_RX); // Set low
    } else {
        // Configure PA8 back to UART1 RX
        PORTCON_PORTA_SEL1 &= ~PORTCON_PORTA_SEL1_A8_MASK;
        PORTCON_PORTA_SEL1 |= PORTCON_PORTA_SEL1_A8_BITS_UART1_RX;
        // DIR automatically handled by UART peripheral
        
        // Re-enable UART RX and RX DMA
        UART1->CTRL |= UART_CTRL_RXEN_BITS_ENABLE | UART_CTRL_RXDMAEN_BITS_ENABLE;
        
        // Re-enable DMA Channel 0
        DMA_CH0->CTR |= DMA_CH_CTR_CH_EN_BITS_ENABLE;
    }
}

// Configure port ring pin (PB15) for paddle input
// When enabled: PB15 becomes GPIO input for paddle ring
// When disabled: PB15 becomes GPIO output high (BK1080 control pin)
static void CW_ConfigurePortRing(bool enable)
{
    if (enable) {
        // Configure PB15 as GPIO input (ring) with pull-up enabled
        //PORTCON_PORTB_PU |= PORTCON_PORTB_PU_B15_MASK; // Enable pull-up (reads high when floating)
        GPIOB->DIR &= ~(0 | GPIO_DIR_15_MASK); // PB15 as INPUT
        PORTCON_PORTB_IE |= PORTCON_PORTB_IE_B15_BITS_ENABLE; // Enable input buffer
    } else {
        // Configure PB15 as GPIO output, set high
        //PORTCON_PORTB_PU &= ~PORTCON_PORTB_PU_B15_MASK; // Disable pull-up
        PORTCON_PORTB_IE &= ~PORTCON_PORTB_IE_B15_MASK; // Disable input buffer
        PORTCON_PORTB_SEL1 &= ~PORTCON_PORTB_SEL1_B15_MASK;
        PORTCON_PORTB_SEL1 |= PORTCON_PORTB_SEL1_B15_BITS_GPIOB15;
        GPIO_SetBit(&GPIOB->DATA, GPIOB_PIN_BK1080); // Set PB15 high
        GPIOB->DIR |= GPIO_DIR_15_BITS_OUTPUT; // Then switch to output
    }
}

// Initialize keyer from gEeprom settings
static void CW_KeyerInit()
{
    // TIMERBASE0_LOW_CNT is 16-bit 10 kHz tick and rolls over at 0xFFFF (~6553 ms)

    const uint32_t wpm = gEeprom.CW_KEY_WPM;
    const uint32_t dit_ticks = TICKS_PER_MINUTE / (wpm * DITS_PER_WORD);
    const uint32_t dah_ticks = 3U * dit_ticks;

    s_dit_cnt = (uint16_t)dit_ticks;
    s_dah_cnt = (uint16_t)dah_ticks;
    s_gap_cnt = (uint16_t)dit_ticks; // inter-element gap = 1 dit

    // Load settings from gEeprom
    s_iambic_b = (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_B);
    
    // Check reversed flag
    s_reverse_keys = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_REVERSED);
    
    // Configure port pins based on bit flags
    bool uses_port_ground = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_GROUND);
    bool uses_port_ring = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_RING);
    bool is_handkey = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER);
    bool uses_buttons = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_BUTTONS);
    
    char buf[120];
    sprintf_(buf, "CW_Init: mode=0x%02X handkey=%d btns=%d pG=%d pR=%d rev=%d\r\n",
             gEeprom.CW_KEY_INPUT, is_handkey, uses_buttons, uses_port_ground, uses_port_ring, s_reverse_keys);
    UART_Send(buf, strlen(buf));
    
    CW_ConfigurePortGround(uses_port_ground);
    CW_ConfigurePortRing(uses_port_ring);

    s_last_cnt         = (uint16_t)TIMERBASE0_LOW_CNT;
    s_active_is_dit    = false;
    s_last_dit = false;
    s_last_dah = false;

    s_KeyerFSMState = CWK_STATE_IDLE;
    s_cfg_dirty = false;
    UART_Send("keyer init done\r\n", 17);
}

void CW_KeyerReconfigure(void)
{
    s_cfg_dirty = true;
    UART_Send("keyer marked for reconfig\r\n", 27);
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
    
    UART_Send("Checking CW keyer inputs\r\n", 26);
    
    // Temporarily configure port pins if needed
    if (uses_port_ground || uses_port_ring) {
        UART_Send("Configuring port pins for CW keyer check\r\n", 42);
        CW_ConfigurePortGround(uses_port_ground);
        CW_ConfigurePortRing(uses_port_ring);
        
        // Allow pins to stabilize after configuration
        SYSTEM_DelayMs(50);
    }
    UART_Send("done with config, starting validation\r\n", 40);

    // Check inputs with 10ms intervals - consider stuck if key stays down for over 10 consecutive checks
    int stuck_count = 0;
    bool any_stuck = false;
    int total_checks = 0;
    
    for (int i = 0; i < 20; i++) {  // Check up to 20 times = 200ms max
        bool dit = false, dah = false;
        CW_ReadKeysForMode(new_mode, &dit, &dah);
        total_checks++;
        
        // Debug output every 5 checks
        if (i % 5 == 0) {
            char dbg[50];
            sprintf_(dbg, "check %d: dit=%d dah=%d stuck=%d\r\n", i, dit, dah, stuck_count);
            UART_Send(dbg, strlen(dbg));
        }
        
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
    
    char buf[60];
    sprintf_(buf, "total_checks=%d stuck_count=%d stuck=%d\r\n", 
             total_checks, stuck_count, any_stuck);
    UART_Send(buf, strlen(buf));

    // If no stuck keys detected, mode is valid
    if (!any_stuck) {
        UART_Send("CW keyer inputs valid\r\n", 24);
        return true;
    }
    UART_Send("CW keyer inputs stuck\r\n", 24);

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
        UART_Send("handkey PTT on\r\n", 17);
    } else if (!ptt && s_last_handkey_ptt) {
        // PTT released
        action = CW_ACTION_CARRIER_OFF;
        UART_Send("handkey PTT off\r\n", 18);
    } else if (ptt && s_last_handkey_ptt) {
        // PTT being held - keep carrier active
        action = CW_ACTION_CARRIER_HOLD;
    }

    s_last_handkey_ptt = ptt;
    return action;
}

CW_Action_t CW_HandleState(void)
{
    CW_Action_t action = CW_ACTION_NONE;
    static uint16_t last_debug_cnt = 0;
    static uint32_t state_count = 0;

    // Check if keyer is disabled (handkey modes have NO_KEYER flag set)
    if (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER) {
        static uint32_t handkey_log_count = 0;
        if (++handkey_log_count % 5000 == 0) {
            UART_Send("CW in handkey mode\r\n", 20);
        }
        return ptt_action();
    } else {
        static uint32_t keyer_log_count = 0;
        if (++keyer_log_count % 5000 == 0) {
            char buf[60];
            sprintf_(buf, "CW in keyer mode (0x%02X)\r\n", gEeprom.CW_KEY_INPUT);
            UART_Send(buf, strlen(buf));
        }
    }
    
    if(state_count++ % 5000 == 0) {
    char buf[40];
    sprintf_(buf, "keyer not SK cnt=%u\r\n", (uint16_t)TIMERBASE0_LOW_CNT);
    //UART_Send(buf, strlen(buf));
    }

    // Check dirty flag at idle - reconfigure if needed
    if (s_cfg_dirty && s_KeyerFSMState == CWK_STATE_IDLE) {
        CW_KeyerInit();
    }

    const uint16_t cur_cnt = (uint16_t)TIMERBASE0_LOW_CNT;
    const uint16_t delta_since_last = timer_jiffies_since(s_last_cnt);
    if (delta_since_last < s_sample_thresh) {
        return action;
    }
    s_last_cnt = cur_cnt;

    CW_Input in;
    CW_ReadKeys(&in);
    if(timer_millis_since(last_debug_cnt) >= 100) {
        last_debug_cnt = cur_cnt;
        bool pb15_is_output = (GPIOB->DIR & (1U << 15)) != 0;
        char buf[100];
        sprintf_(buf, "dit=%d dah=%d dr=%d df=%d hr=%d hf=%d PB15_out=%d\r\n",
                in.dit, in.dah, in.dit_rise, in.dit_fall, in.dah_rise, in.dah_fall, pb15_is_output);
        //UART_Send(buf, strlen(buf));
    }

    switch (s_KeyerFSMState) {
    case CWK_STATE_IDLE:
        // if (idle_count++ % 3000 == 0)
        //     UART_Send("keyer is idle\r\n", 15);
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
            s_KeyerFSMState = s_active_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
            UART_LogSend("keyer going active\r\n", 20);
            action = CW_ACTION_CARRIER_ON;
        }
        break;

    case CWK_STATE_ACTIVE_DIT:
        UART_LogSend("dit\r\n", 5);
        [[fallthrough]];
    case CWK_STATE_ACTIVE_DAH: 
    {
        UART_LogSend("dah - keyer is active\r\n", 18);
        const uint16_t target = (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT) ? s_dit_cnt : s_dah_cnt;
        const uint16_t elapsed_elem = timer_jiffies_since(s_elem_start_cnt);

        // Iambic alternation detection
        if (in.dit && in.dah) {
            s_both_held_during_elem = true;
        }
        
        // Iambic B: detect opposite paddle press during element
        if (s_iambic_b) {
            if (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT && in.dah) {
                s_pending_alternate = true;
            } else if (s_KeyerFSMState == CWK_STATE_ACTIVE_DAH && in.dit) {
                s_pending_alternate = true;
            }
        }

        if (elapsed_elem >= target) {
            action = CW_ACTION_CARRIER_OFF;
            s_elem_start_cnt = cur_cnt;
            s_KeyerFSMState = CWK_STATE_INTER_ELEMENT_GAP;
        } else {
            // Carrier is on and should remain on during element
            action = CW_ACTION_CARRIER_HOLD;
        }
        break; 
    }

    case CWK_STATE_INTER_ELEMENT_GAP: {
        const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_cnt);
        
        // Check dirty flag during inter-element gap
        if (s_cfg_dirty) {
            CW_KeyerInit();
            s_KeyerFSMState = CWK_STATE_INTER_ELEMENT_GAP; // stay in gap state post-init
            return action;
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
                s_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
                s_active_is_dit   = next_is_dit;
                action = CW_ACTION_CARRIER_ON;
            } else {
                s_KeyerFSMState = CWK_STATE_IDLE;
                // Already in NONE state, carrier was turned off at end of last element
            }
        }
        break; }

    default:
        s_KeyerFSMState = CWK_STATE_IDLE;
        break;
    }

    return action;
}
