// CW Iambic Keyer interface
// a small FSM to handle CW dit/dah timing and iambic A/B semantics

#ifndef APP_CWKEYER_H
#define APP_CWKEYER_H

#include <stdint.h>
#include <stdbool.h>

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_DIT,
    CWK_STATE_ACTIVE_DAH,
    CWK_STATE_INTER_ELEMENT_GAP,
} CW_KeyerFSMState_t;

// Current FSM state (for visibility/debugging)
extern volatile CW_KeyerFSMState_t gCW_KeyerFSMState;

// Actions emitted by the keyer FSM; mutually exclusive carrier state transitions
typedef enum {
    CW_ACTION_NONE        = 0,  // no change needed, carrier already off
    CW_ACTION_CARRIER_ON  = 1,  // carrier should be/stay on (press/hold PTT)
    CW_ACTION_CARRIER_OFF = 2,  // carrier should turn off (release PTT)
} CW_Action_t;

// Periodic handler: drive CW state machine and return actions to apply
CW_Action_t CW_HandleState(void);

// Optional: request reconfigure at next safe boundary (applied at gap or idle)
void CW_KeyerReconfigure(void);

#endif // APP_CWKEYER_H
