#ifndef APP_CWHARDWARE_H
#define APP_CWHARDWARE_H

#include <stdint.h>
#include <stdbool.h>

// Normalized paddle input + edge flags
typedef struct {
    bool dit;
    bool dah;
    bool dit_rise;
    bool dah_rise;
} CW_Input;

// Read raw inputs for a specific mode
bool CW_ReadKeysForMode(uint8_t mode, bool *dit_out, bool *dah_out);

// Read normalized paddle inputs (computes edges)
void CW_ReadKeys(CW_Input *in);

// Configure port pins for paddle interface
void CW_ConfigurePortGround(bool enable);
void CW_ConfigurePortRing(bool enable);

// Reset hardware-sampled state (call from keyer init)
void CW_HW_ResetKeySamples(void);

#endif // APP_CWHARDWARE_H
