#ifndef PROTECTION_H
#define PROTECTION_H

#include <stdbool.h>
#include <stdint.h>

#define WARNING_LOW (1U << 0)
#define WARNING_HIGH (1U << 1)
#define FAULT_PROCESS_LOW (1U << 2)
#define FAULT_PROCESS_HIGH (1U << 3)

enum protection_id { PROTECTION_LOW_WARNING, PROTECTION_HIGH_WARNING,
                     PROTECTION_LOW_TRIP, PROTECTION_HIGH_TRIP, PROTECTION_COUNT };

struct protection_config {
    int threshold_x10;
    int hysteresis_x10;
    int delay_ms;
    bool enabled;
};

struct protection_state {
    bool pending;
    bool active;
    int64_t pending_since_ms;
};

bool protection_update(struct protection_state *state, const struct protection_config *config,
                       bool low_limit, int value_x10, int64_t now_ms);
const char *protection_name(enum protection_id id);

#endif
