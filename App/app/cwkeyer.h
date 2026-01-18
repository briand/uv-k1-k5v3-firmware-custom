// CW Iambic Keyer interface
// a small FSM to handle CW dit/dah timing and iambic A/B semantics

#ifndef APP_CWKEYER_H
#define APP_CWKEYER_H

#include <stdint.h>
#include <stdbool.h>

// Actions emitted by the keyer FSM; mutually exclusive carrier state transitions
typedef enum {
    CW_ACTION_NONE        = 0,  // no carrier active (idle, inter-element gap)
    CW_ACTION_CARRIER_ON  = 1,  // transition carrier from off to on (start element)
    CW_ACTION_CARRIER_OFF = 2,  // transition carrier from on to off (end element)
    CW_ACTION_CARRIER_HOLD = 3, // carrier is on and should remain on (during element)
} CW_Action_t;

// Periodic handler: drive CW state machine and return actions to apply
CW_Action_t CW_HandleState(void);

// Optional: request reconfigure at next safe boundary (applied at gap or idle)
void CW_KeyerReconfigure(void);

// Check keyer inputs before mode change: returns true if inputs valid, false to abort
// new_mode: The CW_KeyInputType_t mode to validate
bool CW_CheckKeyerInputs(uint8_t new_mode);

#endif // APP_CWKEYER_H
