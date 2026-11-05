#include "protection.h"
#include <string.h>

bool protection_update_limit(struct protection_state *state, const struct protection_limit *config,
                             bool low_limit, int value, int64_t now_ms)
{
    if (!config->enabled) {
        memset(state, 0, sizeof(*state));
        return false;
    }

    if (state->active) {
        int64_t clear_limit = (int64_t)config->threshold +
            (low_limit ? config->hysteresis : -(int64_t)config->hysteresis);
        bool recovered = low_limit ? value >= clear_limit : value <= clear_limit;
        if (config->hysteresis == 0) {
            recovered = low_limit ? value > clear_limit : value < clear_limit;
        }
        if (recovered) state->active = false;
        return state->active;
    }

    bool exceeded = low_limit ? value <= config->threshold :
                                value >= config->threshold;
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

// REQ-PROT-01
bool protection_update(struct protection_state *state, const struct protection_config *config,
                       bool low_limit, int value_x10, int64_t now_ms)
{
    const struct protection_limit limit = {
        .threshold = config->threshold_x10, .hysteresis = config->hysteresis_x10,
        .delay_ms = config->delay_ms, .enabled = config->enabled,
    };
    return protection_update_limit(state, &limit, low_limit, value_x10, now_ms);
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
