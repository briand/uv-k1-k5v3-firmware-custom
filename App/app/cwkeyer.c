// CW Iambic Keyer implementation
// Lean FSM for iambic A/B with optional reversed mapping

#include <stdint.h>
#include <stdbool.h>

#include "app/cwkeyer.h"
#include "settings.h"
#include "bsp/dp32g030/timer.h"

// Timer scale: 50 kHz tick → 20 µs per tick
#define TICKS_PER_MS      50U
#define TICKS_PER_MINUTE  (60000U * TICKS_PER_MS)
#define DITS_PER_WORD     50U

// Sampling threshold (in timer ticks) for key scans; set by init
static uint32_t s_sample_thresh = TICKS_PER_MS; // ~1 ms

// Externs required by header
volatile CW_KeyerMode_t     gCW_KeyerMode     = CW_KEYER_MODE_OFF;
volatile CW_KeyerFSMState_t gCW_KeyerFSMState = CWK_STATE_IDLE;

// Internal keyer runtime state
static CW_KeyerMode_t s_lastMode = CW_KEYER_MODE_OFF;
static uint32_t       s_dit_cnt  = 0;      // duration in timer ticks
static uint32_t       s_dah_cnt  = 0;      // duration in timer ticks
static uint32_t       s_gap_cnt  = 0;      // inter-element gap in ticks (1 dit)
static uint32_t       s_last_cnt = 0;      // last TIMERBASE0_LOW_CNT sample
static uint32_t       s_elem_start_cnt = 0;// element start counter
static bool           s_active_is_dit = false;
static bool           s_pending_alternate = false; // alternate element queued
static bool           s_both_held_during_elem = false; // iambic-A detection
static bool           s_reverse_keys = false; // normalized mapping flag
static bool           s_last_dit = false, s_last_dah = false; // last sampled paddles

// Reconfigure requested (apply at idle or after gap)
static volatile bool s_cfg_dirty = false;

// Compute delta with wrap-around protection for a 32-bit up-counter
static inline uint32_t cw_delta_counts(uint32_t prev, uint32_t cur)
{
    return (cur >= prev) ? (cur - prev) : (UINT32_MAX - prev + 1U + cur);
}

// Decide initial element when both keys are pressed simultaneously
static inline bool cw_pick_initial_is_dit(CW_KeyerMode_t mode)
{
    switch (mode) {
        case CW_KEYER_MODE_IAMBIC_A_REVERSED:
        case CW_KEYER_MODE_IAMBIC_B_REVERSED:
            return false; // start with DAH on simultaneous press
        case CW_KEYER_MODE_IAMBIC_A:
        case CW_KEYER_MODE_IAMBIC_B:
        default:
            return true;  // start with DIT on simultaneous press
    }
}

// Input struct (normalized paddles + edges)
typedef struct {
    bool dit;
    bool dah;
    bool dit_rise;
    bool dit_fall;
    bool dah_rise;
    bool dah_fall;
} CW_Input;

// Placeholder GPIO reads (wire these to your actual input pins)
static void CW_ReadKeys(CW_Input *in)
{
    // TODO: Wire up to GPIO; for now, placeholder zeros
    bool hw_dit = false;
    bool hw_dah = false;

    // Normalize mapping (reverse swaps paddles)
    bool n_dit = s_reverse_keys ? hw_dah : hw_dit;
    bool n_dah = s_reverse_keys ? hw_dit : hw_dah;

    in->dit_rise = (!s_last_dit && n_dit);
    in->dit_fall = (s_last_dit && !n_dit);
    in->dah_rise = (!s_last_dah && n_dah);
    in->dah_fall = (s_last_dah && !n_dah);
    in->dit = n_dit;
    in->dah = n_dah;

    s_last_dit = n_dit;
    s_last_dah = n_dah;
}

// Initialize for mode and compute tick durations
static void CW_KeyerInit()
{
    // Configure timer for 50 kHz tick (48 MHz / 960)
    TIM0_INIT(960U - 1U, 0U, false);

    const uint32_t wpm = gEeprom.CW_KEY_WPM ? gEeprom.CW_KEY_WPM : 18U;
    const uint32_t dit_ticks = TICKS_PER_MINUTE / (wpm * DITS_PER_WORD);
    const uint32_t dah_ticks = 3U * dit_ticks;

    s_dit_cnt = dit_ticks;
    s_dah_cnt = dah_ticks;
    s_gap_cnt = dit_ticks; // inter-element gap = 1 dit

    s_last_cnt         = TIMERBASE0_LOW_CNT;
    s_elem_start_cnt   = s_last_cnt;
    s_active_is_dit    = false;
    s_pending_alternate = false;
    s_both_held_during_elem = false;
    s_last_dit = false;
    s_last_dah = false;
    s_reverse_keys = (mode == CW_KEYER_MODE_IAMBIC_A_REVERSED || mode == CW_KEYER_MODE_IAMBIC_B_REVERSED);

    gCW_KeyerFSMState = CWK_STATE_IDLE;
    s_lastMode = mode;
}

void CW_KeyerReconfigure(void)
{
    s_cfg_dirty = true;
}

CW_Action_t CW_HandleState(void)
{
    CW_Action_t actions = CW_ACTION_NONE;

    // Mode change resets immediately
    if (gCW_KeyerMode != s_lastMode) {
        CW_KeyerInitForMode(gCW_KeyerMode);
    }

    if (gCW_KeyerMode == CW_KEYER_MODE_OFF) {
        gCW_KeyerFSMState = CWK_STATE_IDLE;
        return actions;
    }

    const uint32_t cur_cnt = TIMERBASE0_LOW_CNT;
    const uint32_t delta_since_last = cw_delta_counts(s_last_cnt, cur_cnt);
    if (delta_since_last < s_sample_thresh) {
        return actions;
    }
    s_last_cnt = cur_cnt;

    CW_Input in;
    CW_ReadKeys(&in);

    switch (gCW_KeyerFSMState) {
    case CWK_STATE_IDLE:
        if (in.dit || in.dah) {
            if (in.dit && in.dah) {
                s_active_is_dit = cw_pick_initial_is_dit(gCW_KeyerMode);
                s_both_held_during_elem = true;
            } else {
                s_active_is_dit = in.dit;
                s_both_held_during_elem = false;
            }

            s_pending_alternate = false;
            s_elem_start_cnt = cur_cnt;
            gCW_KeyerFSMState = s_active_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
            actions |= CW_ACTION_KEY_DOWN;
        }
        break;

    case CWK_STATE_ACTIVE_DIT:
    case CWK_STATE_ACTIVE_DAH: {
        const uint32_t target = (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DIT) ? s_dit_cnt : s_dah_cnt;
        const uint32_t elapsed_elem = cw_delta_counts(s_elem_start_cnt, cur_cnt);

        // Iambic alternation detection
        if (in.dit && in.dah) {
            s_both_held_during_elem = true;
        }
        if (gCW_KeyerMode == CW_KEYER_MODE_IAMBIC_B || gCW_KeyerMode == CW_KEYER_MODE_IAMBIC_B_REVERSED) {
            if (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DIT && in.dah) {
                s_pending_alternate = true;
            } else if (gCW_KeyerFSMState == CWK_STATE_ACTIVE_DAH && in.dit) {
                s_pending_alternate = true;
            }
        }

        if (elapsed_elem >= target) {
            actions |= CW_ACTION_KEY_UP;
            s_elem_start_cnt = cur_cnt;
            gCW_KeyerFSMState = CWK_STATE_INTER_ELEMENT_GAP;
        }
        break; }

    case CWK_STATE_INTER_ELEMENT_GAP: {
        const uint32_t elapsed_gap = cw_delta_counts(s_elem_start_cnt, cur_cnt);
        if (elapsed_gap >= s_gap_cnt) {
            bool next_is_dit = false;
            bool have_next = false;

            if (gCW_KeyerMode == CW_KEYER_MODE_IAMBIC_A || gCW_KeyerMode == CW_KEYER_MODE_IAMBIC_A_REVERSED) {
                if (s_both_held_during_elem && (in.dit || in.dah)) {
                    next_is_dit = !s_active_is_dit; // alternate
                    have_next = true;
                }
            } else {
                if (s_pending_alternate) {
                    next_is_dit = !s_active_is_dit;
                    have_next = true;
                } else if (in.dit || in.dah) {
                    if (in.dit && in.dah) {
                        next_is_dit = cw_pick_initial_is_dit(gCW_KeyerMode);
                    } else {
                        next_is_dit = in.dit;
                    }
                    have_next = true;
                }
            }

            s_pending_alternate = false;
            s_both_held_during_elem = false;
            s_elem_start_cnt = cur_cnt;

            if (have_next) {
                gCW_KeyerFSMState = next_is_dit ? CWK_STATE_ACTIVE_DIT : CWK_STATE_ACTIVE_DAH;
                s_active_is_dit   = next_is_dit;
                actions |= CW_ACTION_KEY_DOWN;
            } else {
                gCW_KeyerFSMState = CWK_STATE_IDLE;
                if (s_cfg_dirty) { CW_KeyerInitForMode(gCW_KeyerMode); s_cfg_dirty = false; }
            }
        }
        break; }

    default:
        gCW_KeyerFSMState = CWK_STATE_IDLE;
        break;
    }

    return actions;
}
