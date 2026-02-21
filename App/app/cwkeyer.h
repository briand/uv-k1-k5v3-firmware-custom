// CW Iambic Keyer interface
// a small FSM to handle CW dit/dah timing and iambic A/B semantics

#ifndef APP_CWKEYER_H
#define APP_CWKEYER_H

#include <stdint.h>
#include <stdbool.h>

// Keyer modes
typedef enum {
    CW_KEYER_MODE_OFF = 0,
    CW_KEYER_MODE_IAMBIC_A,
    CW_KEYER_MODE_IAMBIC_B,
    CW_KEYER_MODE_IAMBIC_A_REVERSED,
    CW_KEYER_MODE_IAMBIC_B_REVERSED,
} CW_KeyerMode_t;

// Keyer FSM states
typedef enum {
    CWK_STATE_IDLE = 0,
    CWK_STATE_ACTIVE_DIT,
    CWK_STATE_ACTIVE_DAH,
    CWK_STATE_INTER_ELEMENT_GAP,
} CW_KeyerFSMState_t;

// Current keyer mode (menu-controlled elsewhere)
extern volatile CW_KeyerMode_t     gCW_KeyerMode;
// Current FSM state (for visibility/debugging)
extern volatile CW_KeyerFSMState_t gCW_KeyerFSMState;

// Actions emitted by the keyer FSM; treated as a bitmask
typedef enum {
    CW_ACTION_NONE     = 0,
    CW_ACTION_KEY_DOWN = 1u << 0,  // begin RF keying
    CW_ACTION_KEY_UP   = 1u << 1,  // end RF keying
} CW_Action_t;

// Periodic handler: drive CW state machine and return actions to apply
CW_Action_t CW_HandleState(void);

// Optional: request reconfigure at next safe boundary (applied at gap or idle)
void CW_KeyerReconfigure(void);

#endif // APP_CWKEYER_H
