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

// VCD waveform logging - set to 1 to output VCD format to UART for GTKWave
#define CW_VCD_LOG 0

#if CW_VCD_LOG
// VCD state tracking - previous values for change detection
static bool s_vcd_header_sent = false;
static bool s_vcd_dit = false;
static bool s_vcd_dah = false;
static bool s_vcd_dit_rise = false;
static bool s_vcd_dah_rise = false;
static bool s_vcd_pending = false;
static bool s_vcd_carrier = false;
static bool s_vcd_is_dit = false;
static uint8_t s_vcd_state = 0;
static uint16_t s_vcd_count = 0;
#endif

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
static bool           s_last_dit = false, s_last_dah = false; // last sampled paddles
static bool           s_last_handkey_ptt = false; // last PTT state for handkey mode

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = true;

// Input struct (normalized paddles + edges)
typedef struct {
    bool dit;
    bool dah;
    bool dit_rise;
    bool dah_rise;
} CW_Input;

// Debug helper: log when a dit would be created while only dah is pressed
static void CW_Debug_DahOnlyLog(uint16_t timestamp, const char *where, const CW_Input *in, bool would_create_dit)
{
#if CW_KEYER_DEBUG_DAH_SEND
    if (in && in->dah && !in->dit && would_create_dit) {
        char buf[120];
        int len = sprintf_(buf, "%u DBUG_DAH: %s state=%d active=%d pending=%d in.dah=%d in.dit=%d\r\n",
                           (unsigned)timestamp, where, (int)s_KeyerFSMState, (int)s_active_is_dit, (int)s_pending_alternate, (int)in->dah, (int)in->dit);
        UART_Send(buf, len);
    }
#else
    (void)timestamp;
    (void)where;
    (void)in;
    (void)would_create_dit;
#endif
}

#if CW_VCD_LOG
// Send VCD header (call once at start)
static void VCD_SendHeader(void)
{
    const char* hdr =
        "$timescale 100us $end\n"
        "$scope module keyer $end\n"
        "$var wire 1 d dit $end\n"
        "$var wire 1 D dah $end\n"
        "$var wire 1 r dit_rise $end\n"
        "$var wire 1 R dah_rise $end\n"
        "$var wire 1 p pending $end\n"
        "$var wire 1 c carrier $end\n"
        "$var wire 1 i is_dit $end\n"
        "$var reg 3 s state $end\n"
        "$var reg 16 t count $end\n"
        "$upscope $end\n"
        "$enddefinitions $end\n"
        "#0\n0d\n0D\n0r\n0R\n0p\n0c\n0i\nb000 s\nb0000000000000000 t\n";
    UART_Send(hdr, strlen(hdr));
    s_vcd_header_sent = true;
}

// Log VCD signal changes
static void VCD_LogSignals(uint16_t timestamp, const CW_Input *in, bool pending, bool carrier, bool is_dit, uint8_t state)
{
    if (!s_vcd_header_sent) {
        VCD_SendHeader();
    }
    
    char buf[80];
    int len = 0;
    
    // Always output timestamp
    len = sprintf_(buf, "#%u\n", timestamp);
    UART_Send(buf, len);
    
    // Output changed signals
    if (in->dit != s_vcd_dit) {
        s_vcd_dit = in->dit;
        len = sprintf_(buf, "%cd\n", in->dit ? '1' : '0');
        UART_Send(buf, len);
    }
    if (in->dah != s_vcd_dah) {
        s_vcd_dah = in->dah;
        len = sprintf_(buf, "%cD\n", in->dah ? '1' : '0');
        UART_Send(buf, len);
    }
    if (in->dit_rise != s_vcd_dit_rise) {
        s_vcd_dit_rise = in->dit_rise;
        len = sprintf_(buf, "%cr\n", in->dit_rise ? '1' : '0');
        UART_Send(buf, len);
    }
    if (in->dah_rise != s_vcd_dah_rise) {
        s_vcd_dah_rise = in->dah_rise;
        len = sprintf_(buf, "%cR\n", in->dah_rise ? '1' : '0');
        UART_Send(buf, len);
    }
    if (pending != s_vcd_pending) {
        s_vcd_pending = pending;
        len = sprintf_(buf, "%cp\n", pending ? '1' : '0');
        UART_Send(buf, len);
    }
    if (carrier != s_vcd_carrier) {
        s_vcd_carrier = carrier;
        len = sprintf_(buf, "%cc\n", carrier ? '1' : '0');
        UART_Send(buf, len);
    }
    if (is_dit != s_vcd_is_dit) {
        s_vcd_is_dit = is_dit;
        len = sprintf_(buf, "%ci\n", is_dit ? '1' : '0');
        UART_Send(buf, len);
    }
    if (state != s_vcd_state) {
        s_vcd_state = state;
        len = sprintf_(buf, "b%c%c%c s\n", 
            (state & 4) ? '1' : '0',
            (state & 2) ? '1' : '0',
            (state & 1) ? '1' : '0');
        UART_Send(buf, len);
    }
    if (timestamp != s_vcd_count) {
        s_vcd_count = timestamp;
        // 16-bit binary
        len = sprintf_(buf, "b%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c t\n",
            (timestamp >> 15) & 1 ? '1' : '0',
            (timestamp >> 14) & 1 ? '1' : '0',
            (timestamp >> 13) & 1 ? '1' : '0',
            (timestamp >> 12) & 1 ? '1' : '0',
            (timestamp >> 11) & 1 ? '1' : '0',
            (timestamp >> 10) & 1 ? '1' : '0',
            (timestamp >> 9) & 1 ? '1' : '0',
            (timestamp >> 8) & 1 ? '1' : '0',
            (timestamp >> 7) & 1 ? '1' : '0',
            (timestamp >> 6) & 1 ? '1' : '0',
            (timestamp >> 5) & 1 ? '1' : '0',
            (timestamp >> 4) & 1 ? '1' : '0',
            (timestamp >> 3) & 1 ? '1' : '0',
            (timestamp >> 2) & 1 ? '1' : '0',
            (timestamp >> 1) & 1 ? '1' : '0',
            (timestamp >> 0) & 1 ? '1' : '0');
        UART_Send(buf, len);
    }
}
#endif

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

	// Reset VOICE chip pins
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
    uint32_t limit = heavy ? 500 : 100; // more samples for heavy de-noise
    uint32_t goal = heavy ? 300 : 60;  // need this many stable samples
    
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
    in->dah_rise = (!s_last_dah && n_dah);
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
                    CW_Debug_DahOnlyLog(cur_count, "ACTIVE: A dah_rise -> pending", &in, (!s_active_is_dit));
                } else if (!s_active_is_dit && in.dit_rise) {
                    s_pending_alternate = true;
                    CW_Debug_DahOnlyLog(cur_count, "ACTIVE: A dit_rise -> pending", &in, (!s_active_is_dit));
                }
            } else {
                // "Elecraft style" Type B with an edge-trigger during the first third of a dah
                // (so holding the alternate on the way into the element won't trigger it)
                if ((!s_active_is_dit) && (elapsed_elem < s_dit_count)) {
                    if (in.dit_rise) {
                        s_pending_alternate = true;
                        CW_Debug_DahOnlyLog(cur_count, "ACTIVE: B early dit_rise -> pending", &in, (!s_active_is_dit));
                    }
                } else {
                    // Standard Type B logic: state detection
                    if (s_active_is_dit && in.dah) {
                        s_pending_alternate = true;
                        CW_Debug_DahOnlyLog(cur_count, "ACTIVE: B dah level -> pending", &in, (!s_active_is_dit));
                    } else if (!s_active_is_dit && in.dit) {
                        s_pending_alternate = true;
                        CW_Debug_DahOnlyLog(cur_count, "ACTIVE: B dit level -> pending", &in, (!s_active_is_dit));
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
                CW_Debug_DahOnlyLog(cur_count, "GAP: A dah_rise -> pending", &in, (!s_active_is_dit));
            } else if (!s_active_is_dit && in.dit_rise) {
                s_pending_alternate = true;
                CW_Debug_DahOnlyLog(cur_count, "GAP: A dit_rise -> pending", &in, (!s_active_is_dit));
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
                CW_Debug_DahOnlyLog(cur_count, "GAP: next -> active", &in, s_active_is_dit);
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
                    // log if we just created a dit while only dah is pressed
                    CW_Debug_DahOnlyLog(cur_count, "CHAR: early -> active", &in, s_active_is_dit);
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
            // Log if this sets a dit while only dah is present
            CW_Debug_DahOnlyLog(cur_count, "WORD: -> active", &in, s_active_is_dit);
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

#if CW_VCD_LOG
    // Log VCD signals - carrier is on if action is ON or HOLD_ON
    bool carrier = (action == CW_ACTION_CARRIER_ON || action == CW_ACTION_CARRIER_HOLD_ON);
    VCD_LogSignals(cur_count, &in, s_pending_alternate, carrier, s_active_is_dit, (uint8_t)s_KeyerFSMState);
#endif

	return action;
}
