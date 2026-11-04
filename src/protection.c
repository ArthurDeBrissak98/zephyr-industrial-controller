#include "protection.h"
#include <string.h>

// REQ-PROT-01
bool protection_update(struct protection_state *state, const struct protection_config *config,
                       bool low_limit, int value_x10, int64_t now_ms)
{
    if (!config->enabled) {
        memset(state, 0, sizeof(*state));
        return false;
    }

    if (state->active) {
        int64_t clear_limit = (int64_t)config->threshold_x10 +
            (low_limit ? config->hysteresis_x10 : -(int64_t)config->hysteresis_x10);
        bool recovered = low_limit ? value_x10 >= clear_limit : value_x10 <= clear_limit;
        if (config->hysteresis_x10 == 0) {
            recovered = low_limit ? value_x10 > clear_limit : value_x10 < clear_limit;
        }
        if (recovered) state->active = false;
        return state->active;
    }

    bool exceeded = low_limit ? value_x10 <= config->threshold_x10 :
                                value_x10 >= config->threshold_x10;
    if (!exceeded) {
        state->pending = false;
        return false;
    }
    if (!state->pending || now_ms < state->pending_since_ms) {
        state->pending = true;
        state->pending_since_ms = now_ms;
    }
    if (now_ms - state->pending_since_ms >= config->delay_ms) {
        state->active = true;
        state->pending = false;
    }
    return state->active;
}

const char *protection_name(enum protection_id id)
{
    switch (id) {
    case PROTECTION_LOW_WARNING: return "low-warning";
    case PROTECTION_HIGH_WARNING: return "high-warning";
    case PROTECTION_LOW_TRIP: return "low-trip";
    case PROTECTION_HIGH_TRIP: return "high-trip";
    default: return "unknown";
    }
}
