#ifndef PROTECTION_H
#define PROTECTION_H

#include <stdbool.h>
#include <stdint.h>

#define WARNING_LOW (1U << 0)
#define WARNING_HIGH (1U << 1)
#define FAULT_PROCESS_LOW (1U << 2)
#define FAULT_PROCESS_HIGH (1U << 3)
#define WARNING_MOTOR_CURRENT (1U << 2)
#define WARNING_MOTOR_FREQUENCY (1U << 3)
#define FAULT_MOTOR_OVERCURRENT (1U << 5)
#define FAULT_MOTOR_OVERFREQUENCY (1U << 6)
#define FAULT_MOTOR_FEEDBACK (1U << 7)
#define FAULT_CONFIG_ERROR (1U << 8)
#define FAULT_MOTOR_STALL (1U << 9)
#define MOTOR_STALL_TIME_MS 500
#define MOTOR_STALL_FREQUENCY_X100 500
#define MOTOR_WARNING_MASK (WARNING_MOTOR_CURRENT | WARNING_MOTOR_FREQUENCY)

enum protection_id { PROTECTION_LOW_WARNING, PROTECTION_HIGH_WARNING,
                     PROTECTION_LOW_TRIP, PROTECTION_HIGH_TRIP, PROTECTION_COUNT };

struct protection_config {
    int threshold_x10;
    int hysteresis_x10;
    int delay_ms;
    bool enabled;
};

enum motor_protection_id { MOTOR_CURRENT_WARNING, MOTOR_CURRENT_TRIP,
                           MOTOR_FREQUENCY_WARNING, MOTOR_FREQUENCY_TRIP,
                           MOTOR_PROTECTION_COUNT };

// Единицы порога и гистерезиса совпадают с единицами контролируемого значения.
struct protection_limit {
    int threshold;
    int hysteresis;
    int delay_ms;
    bool enabled;
};

struct protection_state {
    bool pending;
    bool active;
    int64_t pending_since_ms;
};

bool protection_update_limit(struct protection_state *state, const struct protection_limit *config,
                             bool low_limit, int value, int64_t now_ms);
bool protection_update(struct protection_state *state, const struct protection_config *config,
                       bool low_limit, int value_x10, int64_t now_ms);
const char *protection_name(enum protection_id id);

#endif
