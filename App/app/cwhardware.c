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

 // Hardware input helpers for CW keyer (port config, debounced reads, etc.)
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "misc.h"
#include "app/cwhardware.h"
#include "settings.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "bsp/dp32g030/portcon.h"
#include "bsp/dp32g030/uart.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/adc.h"
#include "bsp/dp32g030/saradc.h"
#include "driver/timer.h"
#include "external/printf/printf.h"

#define ENABLE_CEC_KEYER_DEBUG 0

// Local state for last sampled paddles (edge detection)
static bool s_last_dit = false;
static bool s_last_dah = false;

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

// static uint16_t ReadCH3()
// {
//     // OLD SINGLE SAMPLE CODE (keep for reference):
//     // Trigger ADC conversion
//     (*(volatile uint32_t *)0x400BA004U) = 0x1U;  // SARADC_START
    
//     // Wait for CH3 end of conversion (0x400BA028 = CH0 + 3*sizeof(ADC_Channel_t))
//     while (!(*(volatile uint32_t *)0x400BA028U & 0x1)) {}
    
//     // Clear interrupt flag for CH3
//     (*(volatile uint32_t *)0x400BA00CU) = (1U << 3);
    
//     // 12-bit data (0x400BA02C = CH3 DATA register)
//     return (uint16_t)((*(volatile uint32_t *)0x400BA02CU) & 0xFFFU);
// }
uint16_t CW_ReadCH3()
{    
    ADC_Start();
    while (!ADC_CheckEndOfConversion(ADC_CH3)) {}
    return ADC_GetValue(ADC_CH3);
}


static void CW_ReadADCkeys(bool *tip_out, bool *ring_out)
{
    // Take baseline ADC sample with timing
    // uint16_t start_tick = timer_jiffies();
    
    // being absolutely paranoid about performance, we enable only CH3 for this loop, then set back
    uint32_t regval = SARADC_CFG;
    SARADC_CFG = (regval & ~SARADC_CFG_CH_SEL_MASK) | (ADC_CH3 << SARADC_CFG_CH_SEL_SHIFT);

    uint16_t baseline = CW_ReadCH3();

    // Validate with up to 4 more samples - stop if any differs by >40 from baseline
    uint16_t val = baseline;
    int samples_taken = 1;
    
    for (int i = 0; i < 12; i++) {
        uint16_t sample = CW_ReadCH3();
        samples_taken++;
        
        int16_t diff = (int16_t)sample - (int16_t)baseline;
        if (diff < 0) diff = -diff;
        
        if (diff > CW_ADC_GLITCH_GUARDBAND) {
            val = 0;  // Inconsistent reading detected
            break;
            SYSTICK_DelayUs(5);  // Short delay before next sample
        }
    }
    SARADC_CFG = regval;  // Restore original channel config so battery monitoring etc. still works
    
    // uint16_t elapsed = timer_jiffies_since(start_tick);
    // Log timing and validation
    // char log_buf[64];
    // sprintf(log_buf, "ADC: %u jiffies, %d samples, baseline=%u->%u\r\n", elapsed, samples_taken, baseline, val);
    // UART_LogSend(log_buf, strlen(log_buf));

    if (val < gEeprom.CW_ADC_CABLE_20K - CW_ADC_RANGE_LIMIT || val > CW_ADC_MAX) return;  // no paddle pressed or fault
    else if (val < gEeprom.CW_ADC_CABLE_20K + CW_ADC_RANGE_LIMIT) *ring_out = true;  // 20k ohm
    else if (val < gEeprom.CW_ADC_CABLE_10K + CW_ADC_RANGE_LIMIT) *tip_out  = true;  // 10k ohm
    else *tip_out = *ring_out = true;
}

// Read raw paddle inputs for a specific mode
// Returns true if mode is valid, false otherwise
bool CW_ReadKeysForMode(uint8_t mode, bool *dit_out, bool *dah_out)
{
    // Check if keyer is disabled (handkey modes)
    if (mode & CW_KEY_FLAG_NO_KEYER) {
        return false;
    }

    if (mode & CW_KEY_FLAG_ADC) {
        // ADC (CEC cable) input
        bool adc_tip = false;
        bool adc_ring = false;
        CW_ReadADCkeys(&adc_tip, &adc_ring);

        // Determine if keys are reversed
        bool reverse = (mode & CW_KEY_FLAG_REVERSED);

        // Map tip/ring to dit/dah based on reversed flag
        *dit_out = reverse ? adc_tip : adc_ring;
        *dah_out = reverse ? adc_ring : adc_tip;

        return true;
    } 

    // Read PTT (PC5) as tip - shared across button and port configs
    bool hw_tip = false;
    CW_ReadPtt(&hw_tip);
    bool hw_ring = false;

    // Read button ring input if enabled
    if (mode & CW_KEY_FLAG_SIDE1) {
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
void CW_ReadKeys(CW_Input *in)
{
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
}

// Configure port ground pin (PA8) for tip/ring paddle input
// When enabled: PA8 becomes GPIO output low (acts as ground for paddle port)
// When disabled: PA8 returns to UART1 RX function with DMA
void CW_ConfigurePortGround(bool enable)
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
#if ENABLE_KEYER_DEBUG
    char buf[50];
    sprintf_(buf, "Port Ground %s\r\n", enable ? "Enabled" : "Disabled");
    UART_Send(buf, strlen(buf));
#endif
}

// Configure port ring pin (PB15) for paddle input
// When enabled: PB15 becomes GPIO input for paddle ring
// When disabled: PB15 becomes GPIO output high (BK1080 control pin)
void CW_ConfigurePortRing(bool enable)
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
#if ENABLE_KEYER_DEBUG
    char buf[50];
    sprintf_(buf, "Port Ring %s\r\n", enable ? "Enabled" : "Disabled");
    UART_Send(buf, strlen(buf));
#endif
}

void CW_ConfigureADCforCECPaddles(bool enable)
{
    //UART_Send("adc init...", strlen("adc init..."));
    if (enable) {
        gCW_KeyerUsesPTT = true;

        // Enable ADC on PA8 (SARADC CH3) and configure input buffer/pulldown
        PORTCON_PORTA_SEL1 = (PORTCON_PORTA_SEL1 & ~PORTCON_PORTA_SEL1_A8_MASK) | PORTCON_PORTA_SEL1_A8_BITS_SARADC_CH3;

    	// Configure PTT (GPIOC pin 5) as output and drive low
        GPIOC->DIR = (GPIOC->DIR & ~GPIO_DIR_5_MASK) | GPIO_DIR_5_BITS_OUTPUT;
        GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_PTT);
    } else {

        gCW_KeyerUsesPTT = false;

        // return PA8 to UART
        CW_ConfigurePortGround(false);

        // return PTT to GPIO input
        GPIOC->DIR &= ~GPIO_DIR_5_MASK; // INPUT
        PORTCON_PORTC_IE |= PORTCON_PORTC_IE_C5_BITS_ENABLE; // Enable input buffer
    }
#if ENABLE_CEC_KEYER_DEBUG
    char buf[50];
    sprintf_(buf, "ADC for CEC %s\r\n", enable ? "Enabled" : "Disabled");
    UART_Send(buf, strlen(buf));
#endif
}

// Reset sampled key states (used from keyer init)
void CW_HW_ResetKeySamples(void)
{
    s_last_dit = false;
    s_last_dah = false;
}
