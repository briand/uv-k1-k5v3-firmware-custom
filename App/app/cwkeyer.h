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
    CW_ACTION_CARRIER_HOLD_ON = 3, // carrier is on and should remain on (during element)
} CW_Action_t;

// Periodic handler: drive CW state machine and return actions to apply
CW_Action_t CW_HandleState(void);

// Set CW keyer speed from EEPROM; updates internal timing parameters
void CW_UpdateWPM();

// Optional: request reconfigure at next safe boundary (applied at gap or idle)
void CW_KeyerReconfigure(void);

// Check keyer inputs before mode change: returns true if inputs valid, false to abort
// new_mode: The CW_KeyInputType_t mode to validate
bool CW_CheckKeyerInputs(uint8_t new_mode);

// Macro playback API
// Start playback of macro (0 or 1). Sets playback active flag and loads macro.
void CW_StartMacroPlayback(uint8_t macroIndex);

// Query whether playback is active
bool CW_IsMacroPlaybackActive(void);

// Periodic handler for playback FSM - returns CW_Action_t (carrier on/off/hold)
CW_Action_t CW_PlaybackHandleState(void);

// Periodic deadline handler to refresh CW playback UI indicator (blinker).
// Call from a periodic context (e.g., APP_TimeSlice10ms). Uses timer_jiffies/millis.
void CW_PlaybackIndicatorDeadline(void);

#endif // APP_CWKEYER_H
