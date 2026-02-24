/* Millis API using TIM2 as 1kHz source. */
#ifndef DRIVER_MILLIS_H
#define DRIVER_MILLIS_H

#include <stdint.h>

// Initialize TIM2 to run at ~1kHz ticks.
void millis_init(void);

// Return current millisecond tick (32-bit).
uint32_t millis(void);

// Return elapsed milliseconds since prev (wrap-safe).
uint32_t millis_since(uint32_t prev);

#endif // DRIVER_MILLIS_H
