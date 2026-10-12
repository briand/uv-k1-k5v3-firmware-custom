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

// MCU DAC audio driver

#include "driver/dacaudio.h"
#include "driver/millis.h"
#include "driver/systick.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dac.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_tim.h"

#ifdef ENABLE_VOICE
#error "dacaudio and voice prompts both drive the DAC, TIM6 and DMA1 channel 3"
#endif

#define TIMx        TIM6
#define DAC_CHANNEL LL_DAC_CHANNEL_1
#define DMA_CHANNEL LL_DMA_CHANNEL_3

static uint16_t s_buf[DACAUDIO_HALF_LEN * 2];
static DACAUDIO_Fill_t s_fill;
static bool s_running = false;
static bool s_inited = false;
static uint32_t s_rate_hz = 32000;
static volatile uint32_t s_next_time = 0;     // sample time of the next fill's first sample
static volatile uint32_t s_busy_cycles = 0;   // in fills since the last TakeLoad
static volatile uint32_t s_peak_cycles = 0;   // longest single fill since then
static uint32_t s_load_since_ms = 0;

static void Init(void)
{
	// voice.c, the other user of this path, never clocks TIM6, so nothing here
	// assumes it has run
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_DAC1);
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
	LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
	LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

	LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_ANALOG);

	// 48 MHz / (1 + 3) = 12 MHz counts; each update event triggers a conversion.
	// The reload is preloaded so a rate change lands on a sample boundary.
	LL_TIM_SetPrescaler(TIMx, 3);
	LL_TIM_EnableARRPreload(TIMx);
	LL_TIM_SetTriggerOutput(TIMx, LL_TIM_TRGO_UPDATE);

	LL_DAC_SetTriggerSource(DAC1, DAC_CHANNEL, LL_DAC_TRIG_EXT_TIM6_TRGO);
	LL_DAC_SetOutputBuffer(DAC1, DAC_CHANNEL, LL_DAC_OUTPUT_BUFFER_ENABLE);
	LL_DAC_EnableDMAReq(DAC1, DAC_CHANNEL);

	LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
	LL_SYSCFG_SetDMARemap(DMA1, DMA_CHANNEL, LL_SYSCFG_DMA_MAP_DAC1);
	LL_DMA_ConfigTransfer(DMA1, DMA_CHANNEL,
		LL_DMA_DIRECTION_MEMORY_TO_PERIPH
		| LL_DMA_MODE_CIRCULAR
		| LL_DMA_PERIPH_NOINCREMENT
		| LL_DMA_MEMORY_INCREMENT
		| LL_DMA_PDATAALIGN_HALFWORD
		| LL_DMA_MDATAALIGN_HALFWORD
		| LL_DMA_PRIORITY_HIGH);
	LL_DMA_SetMemoryAddress(DMA1, DMA_CHANNEL, (uint32_t)s_buf);
	LL_DMA_SetPeriphAddress(DMA1, DMA_CHANNEL,
		LL_DAC_DMA_GetRegAddr(DAC1, DAC_CHANNEL, LL_DAC_DMA_REG_DATA_12BITS_RIGHT_ALIGNED));
	LL_DMA_EnableIT_HT(DMA1, DMA_CHANNEL);
	LL_DMA_EnableIT_TC(DMA1, DMA_CHANNEL);

	// A half buffer lasts 2.5 ms even at 32 kHz, so the lowest priority has time to spare
	NVIC_SetPriority(DMA1_Channel2_3_IRQn, 3);
	NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
}

void DACAUDIO_Start(DACAUDIO_Fill_t fill, uint32_t rate_hz)
{
	if (s_running) {
		return;
	}
	if (!s_inited) {
		Init();
		s_inited = true;
	}
	LL_TIM_SetAutoReload(TIMx, SystemCoreClock / 4 / rate_hz - 1);
	s_rate_hz = rate_hz;

	s_fill = fill;
	fill(s_buf, 0);
	fill(s_buf + DACAUDIO_HALF_LEN, DACAUDIO_HALF_LEN);
	s_next_time = 2 * DACAUDIO_HALF_LEN;
	s_busy_cycles = 0;
	s_peak_cycles = 0;
	s_load_since_ms = millis();

	// The output turns on at the first sample rather than wherever it last stopped
	LL_DAC_ConvertData12RightAligned(DAC1, DAC_CHANNEL, s_buf[0]);
	LL_DAC_Enable(DAC1, DAC_CHANNEL);
	LL_DAC_EnableTrigger(DAC1, DAC_CHANNEL);
	SYSTICK_DelayUs(15);   // DAC wake-up

	LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
	LL_DMA_SetDataLength(DMA1, DMA_CHANNEL, DACAUDIO_HALF_LEN * 2);
	LL_DMA_ClearFlag_GI3(DMA1);
	LL_DMA_EnableChannel(DMA1, DMA_CHANNEL);

	LL_TIM_SetCounter(TIMx, 0);
	LL_TIM_GenerateEvent_UPDATE(TIMx);   // load the prescaler and reload now
	LL_TIM_EnableCounter(TIMx);
	s_running = true;
}

void DACAUDIO_Stop(void)
{
	if (!s_running) {
		return;
	}
	LL_TIM_DisableCounter(TIMx);
	LL_DMA_DisableChannel(DMA1, DMA_CHANNEL);
	LL_DAC_Disable(DAC1, DAC_CHANNEL);
	s_running = false;
}

bool DACAUDIO_IsRunning(void)
{
	return s_running;
}

// The fill renders the half that has just played, to play after the other half, so
// something placed at the same offset into the next fill's block as now is into
// the half playing plays two half buffers from now
uint32_t DACAUDIO_EventTime(void)
{
	NVIC_DisableIRQ(DMA1_Channel2_3_IRQn);
	const uint32_t left = LL_DMA_GetDataLength(DMA1, DMA_CHANNEL);
	uint32_t next = s_next_time;
	if (LL_DMA_IsActiveFlag_HT3(DMA1) || LL_DMA_IsActiveFlag_TC3(DMA1)) {
		next += DACAUDIO_HALF_LEN;   // a fill is due, and its block comes before ours
	}
	NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);

	return next + (2 * DACAUDIO_HALF_LEN - left) % DACAUDIO_HALF_LEN;
}

void DACAUDIO_Lock(void)
{
	NVIC_DisableIRQ(DMA1_Channel2_3_IRQn);
}

void DACAUDIO_Unlock(void)
{
	NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
}

void DACAUDIO_TakeLoad(uint16_t *avg_permille, uint16_t *peak_permille)
{
	const uint32_t now = millis();
	const uint32_t ms = now - s_load_since_ms;

	NVIC_DisableIRQ(DMA1_Channel2_3_IRQn);
	const uint32_t busy = s_busy_cycles;
	const uint32_t peak = s_peak_cycles;
	s_busy_cycles = 0;
	s_peak_cycles = 0;
	NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
	s_load_since_ms = now;

	// a thousandth of the cycles in the window, and of a half buffer's play time
	const uint32_t window = ms * (SystemCoreClock / 1000000);
	const uint32_t half = SystemCoreClock / s_rate_hz * DACAUDIO_HALF_LEN;
	*avg_permille = window ? busy / window : 0;
	*peak_permille = peak * 1000u / half;
}

// SysTick counts core clocks down from LOAD over its 10 ms period, so the cycles a
// fill takes are the drop in VAL, or the drop past a reload
static void TimedFill(uint16_t *half)
{
	const uint32_t t0 = SysTick->VAL;
	s_fill(half, s_next_time);
	s_next_time += DACAUDIO_HALF_LEN;
	const uint32_t t1 = SysTick->VAL;
	const uint32_t cycles = (t0 >= t1) ? t0 - t1 : t0 + SysTick->LOAD + 1 - t1;

	s_busy_cycles += cycles;
	if (cycles > s_peak_cycles) {
		s_peak_cycles = cycles;
	}
}

// Half transfer: the first half has gone out and can be refilled; transfer
// complete: the second half has, while DMA wraps to the first
void DMA1_Channel2_3_IRQHandler(void)
{
	if (LL_DMA_IsActiveFlag_HT3(DMA1)) {
		LL_DMA_ClearFlag_HT3(DMA1);
		TimedFill(s_buf);
	}
	if (LL_DMA_IsActiveFlag_TC3(DMA1)) {
		LL_DMA_ClearFlag_TC3(DMA1);
		TimedFill(s_buf + DACAUDIO_HALF_LEN);
	}
}
