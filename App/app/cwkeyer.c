// CW Iambic Keyer implementation
// Lean FSM for iambic A/B with optional reversed mapping

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "app/cwkeyer.h"
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

// Timer scale: 10 kHz tick → 100 µs per tick
// 16-bit counter rolls over at 6553 ms
#define DITS_PER_WORD 50
#define TICKS_PER_MS 10
#define TICKS_PER_MINUTE (60 * 1000 * TICKS_PER_MS)  // 600,000

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_DIT,
    CWK_STATE_ACTIVE_DAH,
    CWK_STATE_INTER_ELEMENT_GAP,
    CWK_STATE_INTER_CHAR_GAP,    // Extended gap = end of character
    CWK_STATE_INTER_WORD_GAP,    // Long gap = inter-word space
} CW_KeyerFSMState_t;

static CW_KeyerFSMState_t s_KeyerFSMState = CWK_STATE_IDLE;

// Internal keyer runtime state
static uint16_t       s_dit_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_dah_count  = 0;      // duration in timer ticks (16-bit)
static uint16_t       s_gap_count  = 0;      // inter-element gap in ticks (1 dit)
static uint16_t       s_char_gap_count = 0;  // inter-char gap in ticks (3 dits)
static uint16_t       s_word_gap_count = 0;  // inter-word gap in ticks (7 dits)
static uint16_t       s_last_count = 0;      // last TIMERBASE0_LOW_COUNT sample (16-bit)
static uint16_t       s_elem_start_count = 0;// element start counter (16-bit)
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

// Generic GPIO deglitch function - reads with de-noise
// Returns true if pin is active (low), false if inactive (high)
static bool CW_ReadGpioDeglitched(volatile uint32_t *gpio_data, uint8_t pin_bit, bool heavy)
{
    bool result = false;
    uint16_t reg = 0, reg2;
    unsigned int i, k;
    uint32_t limit = heavy ? 18 : 8; // more samples for heavy de-noise
    uint32_t goal = heavy ? 16 : 4;  // need this many stable samples
    
    for (i = 0, k = 0, reg = 0; i < goal && k < limit; i++, k++) {
        SYSTICK_DelayUs(1);
        reg2 = (*gpio_data) & (1U << pin_bit);
        i *= (reg == reg2);  // Reset i if readings differ
        reg = reg2;
    }
    
    if (i >= goal) {
        // Stable reading achieved - active low
        result = !reg;
    }
    
    return result;
}

// Read the PTT/tip with de-noise
static void CW_ReadPtt(bool *ptt_out)
{
    *ptt_out = CW_ReadGpioDeglitched(&GPIOC->DATA, GPIOC_PIN_PTT, false);
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
    bool hw_tip = false;
    CW_ReadPtt(&hw_tip);
    bool hw_ring = false;
    
    // Read button ring input if enabled
    if (mode & CW_KEY_FLAG_BUTTONS) {
        CW_ReadSideButton(&hw_ring);
    }
    
    // Read port ring input if enabled and OR with button ring
    if (mode & CW_KEY_FLAG_PORT_RING) {
        bool port_ring = CW_ReadGpioDeglitched(&GPIOB->DATA, GPIOB_PIN_BK1080, true);
        hw_ring = hw_ring || port_ring;  // OR both sources
    }
    
    // Determine if keys are reversed
    bool reverse = (mode & CW_KEY_FLAG_REVERSED);
    
    // Map tip/ring to dit/dah based on reversed flag
    *dit_out = reverse ? hw_tip : hw_ring;
    *dah_out = reverse ? hw_ring : hw_tip;

    return true;
}

// Read GPIO inputs based on configured mode
static void CW_ReadKeys(CW_Input *in)
{
#if CW_KEYER_DEBUG
    static int times_called = 0;
    if(++times_called % 1000 == 0) {
        //UART_Send("Reading keys\r\n", 14);
    }
#endif
    
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


#if CW_KEYER_DEBUG
    if(in->dit_rise || in->dah_rise || in->dit_fall || in->dah_fall) {
        char buf[50];
        sprintf_(buf, "%u edge: dit=%d dah=%d\r\n", (unsigned)TIMERBASE0_LOW_CNT,
                 n_dit, n_dah);
        UART_Send(buf, strlen(buf));
    }
#endif
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
        // Configure PB15 as GPIO input (ring)
        GPIOB->DIR &= ~(0 | GPIO_DIR_15_MASK); // PB15 as INPUT
        PORTCON_PORTB_IE |= PORTCON_PORTB_IE_B15_BITS_ENABLE; // Enable input buffer
        PORTCON_PORTB_PU |= PORTCON_PORTB_PU_B15_BITS_ENABLE; // activate the PB15 pullup
    } else {
        // Configure PB15 as GPIO output, set high
        PORTCON_PORTB_IE &= ~PORTCON_PORTB_IE_B15_MASK; // Disable input buffer
        PORTCON_PORTB_PU &= ~PORTCON_PORTB_PU_B15_MASK; // disable the PB15 pullup
        PORTCON_PORTB_SEL1 &= ~PORTCON_PORTB_SEL1_B15_MASK;
        PORTCON_PORTB_SEL1 |= PORTCON_PORTB_SEL1_B15_BITS_GPIOB15;
        GPIO_SetBit(&GPIOB->DATA, GPIOB_PIN_BK1080); // Set PB15 high
        GPIOB->DIR |= GPIO_DIR_15_BITS_OUTPUT; // Then switch to output
    }
}

void CW_UpdateWPM()
{
    // TIMERBASE0_LOW_COUNT is 16-bit 10 kHz tick and rolls over at 0xFFFF (~6553 ms)
    const uint16_t wpm = gEeprom.CW_KEY_WPM;
    const uint16_t dit_ticks = TICKS_PER_MINUTE / (wpm * DITS_PER_WORD);

    s_dit_count = dit_ticks;
    s_dah_count = 3U * dit_ticks;
    s_gap_count = dit_ticks; // inter-element gap = 1 dit
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
    // Check reversed flag
    s_reverse_keys = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_REVERSED);
    
    // Configure port pins based on bit flags
    bool uses_port_ground = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_GROUND);
    bool uses_port_ring = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_PORT_RING);
    
#if CW_KEYER_DEBUG
    bool is_handkey = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_NO_KEYER);
    bool uses_buttons = (gEeprom.CW_KEY_INPUT & CW_KEY_FLAG_BUTTONS);
    char buf[120];
    sprintf_(buf, "CW_Init: mode=0x%02X handkey=%d btns=%d pG=%d pR=%d rev=%d\r\n",
             gEeprom.CW_KEY_INPUT, is_handkey, uses_buttons, uses_port_ground, uses_port_ring, s_reverse_keys);
    UART_Send(buf, strlen(buf));
#endif
    
    CW_ConfigurePortGround(uses_port_ground);
    CW_ConfigurePortRing(uses_port_ring);

    s_last_count         = (uint16_t)TIMERBASE0_LOW_CNT;
    s_active_is_dit    = false;
    s_last_dit = false;
    s_last_dah = false;

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
        action = CW_ACTION_CARRIER_HOLD;
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
        if (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT || s_KeyerFSMState == CWK_STATE_ACTIVE_DAH) {
            return CW_ACTION_CARRIER_HOLD;
        }
        return CW_ACTION_NONE;
    }
    s_last_count = cur_count;

#if CW_KEYER_DEBUG
    // Log state changes
    if (s_KeyerFSMState != last_logged_state) {
        const char* state_names[] = {"IDLE", "ACTIVE_DIT", "ACTIVE_DAH", "INTER_ELEM", "INTER_CHAR", "INTER_WORD"};
        char buf[60];
        sprintf_(buf, "STATE: %s -> %s\r\n", state_names[last_logged_state], state_names[s_KeyerFSMState]);
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
    CW_Input in;

    switch (s_KeyerFSMState) {
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
            if (in.dit && in.dah) {
                // Both pressed: start with dit (normal) or dah (reversed)
                s_active_is_dit = !s_reverse_keys;
                s_both_held_during_elem = true;
            } else {
                s_active_is_dit = in.dit;
                s_both_held_during_elem = false;
            }

            s_pending_alternate = false;
            s_elem_start_count = cur_count;
            s_KeyerFSMState = s_active_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
#if CW_KEYER_DEBUG
            UART_Send("keyer going active\r\n", 20);
#endif
            action = CW_ACTION_CARRIER_ON;
        }
        break;

    case CWK_STATE_ACTIVE_DIT:
    case CWK_STATE_ACTIVE_DAH: 
    {
        // ACTIVE: Sample paddles during element for memory logic
        const uint16_t target = (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT) ? s_dit_count : s_dah_count;
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

        // Sample paddles for memory logic
        CW_ReadKeys(&in);
        
        // Track if both held during element (for Type A)
        if (in.dit && in.dah) {
            s_both_held_during_elem = true;
        }
        
        // Memory logic: depends on keyer mode and element type
        if (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_A) {
            // Type A: Edge detection for opposite paddle
            if (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT && in.dah_rise) {
                s_pending_alternate = true;
            } else if (s_KeyerFSMState == CWK_STATE_ACTIVE_DAH && in.dit_rise) {
                s_pending_alternate = true;
            }
        } else {
            // Type B / Elecraft: State detection, with hybrid for Elecraft during dash
            bool use_edge_detection = false;
            
            // Elecraft Mode B: Use Type A logic (edge) during first 1/3 of dash
            if ((s_KeyerFSMState == CWK_STATE_ACTIVE_DAH) && (elapsed_elem < s_dit_count)) {
                use_edge_detection = true;
            }
            
            if (use_edge_detection) {
                // Type A logic: edge detection
                if (in.dit_rise) {
                    s_pending_alternate = true;
                }
            } else {
                // Type B logic: state detection
                if (s_KeyerFSMState == CWK_STATE_ACTIVE_DIT && in.dah) {
                    s_pending_alternate = true;
                } else if (s_KeyerFSMState == CWK_STATE_ACTIVE_DAH && in.dit) {
                    s_pending_alternate = true;
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
            action = CW_ACTION_CARRIER_HOLD;
        }
        break; 
    }

    case CWK_STATE_INTER_ELEMENT_GAP: {
        // INTER_ELEMENT_GAP: Do NOT sample until gap completes
        const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);
        
        if (elapsed_gap >= s_gap_count) {
            // Gap completed - NOW sample to determine next state
            CW_ReadKeys(&in);
            
            bool next_is_dit = false;
            bool have_next = false;

            // Determine next element based on mode and memory
            if (gEeprom.CW_KEYER_MODE == CW_IAMBIC_MODE_A) {
                // Type A: only alternate if both were held during element and either still pressed
                if (s_both_held_during_elem && (in.dit || in.dah)) {
                    next_is_dit = !s_active_is_dit; // alternate
                    have_next = true;
                }
            } else {
                // Type B / Elecraft: use pending flag (memory) or check current state
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
            s_elem_start_count = TIMERBASE0_LOW_CNT;

            if (have_next) {
                s_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
                s_active_is_dit   = next_is_dit;
                action = CW_ACTION_CARRIER_ON;				
            } else {
                // No key input - transition to inter-char gap (carry over timing)
                s_KeyerFSMState = CWK_STATE_INTER_CHAR_GAP;
            }
        }
		break;
	}

	case CWK_STATE_INTER_CHAR_GAP: {
		// INTER_CHAR_GAP: Sample continuously for new input
		const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);
		
		CW_ReadKeys(&in);
		
		// Check for new key input
		if (in.dit || in.dah) {
			bool next_is_dit = (in.dit && in.dah) ? !s_reverse_keys : in.dit;
			s_elem_start_count = TIMERBASE0_LOW_CNT;
			s_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
			s_active_is_dit = next_is_dit;

			action = CW_ACTION_CARRIER_ON;
            // Emit element to encoder on state exit
            CW_EncoderProcessElement(CW_ELEMENT_INTER_CHAR_SPACE);
		} else if (elapsed_gap >= s_char_gap_count) {
            // Emit element to encoder on state exit
            CW_EncoderProcessElement(CW_ELEMENT_INTER_CHAR_SPACE);
			// Transition to inter-word gap - carry over timing
			s_KeyerFSMState = CWK_STATE_INTER_WORD_GAP;
		}
		break;
	}

	case CWK_STATE_INTER_WORD_GAP: {
		// INTER_WORD_GAP: Sample continuously for new input
		const uint16_t elapsed_gap = timer_jiffies_since(s_elem_start_count);
		
		CW_ReadKeys(&in);
		
		// Check for new key input
		if (in.dit || in.dah) {
			
			bool next_is_dit = (in.dit && in.dah) ? !s_reverse_keys : in.dit;
			s_elem_start_count = TIMERBASE0_LOW_CNT;
			s_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
            s_active_is_dit = next_is_dit;
            action = CW_ACTION_CARRIER_ON;
            
		} else if (elapsed_gap >= s_word_gap_count) {
			// Long silence - go back to idle
			s_KeyerFSMState = CWK_STATE_IDLE;
            CW_EncoderProcessElement(CW_ELEMENT_INTER_WORD_SPACE);
		}
		break;
	}

	default:
		s_KeyerFSMState = CWK_STATE_IDLE;
		break;
	}

	return action;
}
