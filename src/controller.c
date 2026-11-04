#include "controller.h"
#include <errno.h>
#include <string.h>

static void record_event(struct controller *ctrl, enum event_type type,
                         uint32_t faults, uint32_t warnings, int64_t now_ms)
{
    struct controller_event *event = &ctrl->events[ctrl->event_head];

    *event = (struct controller_event) {
        .sequence = ++ctrl->event_sequence,
        .timestamp_ms = now_ms,
        .type = type,
        .state = ctrl->state,
        .faults = faults,
        .warnings = warnings,
        .current_ua = ctrl->sample.current_ua,
        .voltage_mv = ctrl->sample.voltage_mv,
        .measured_x10 = ctrl->sample.measured_x10,
        .frequency_x100 = ctrl->frequency_x100,
    };
    ctrl->event_head = (ctrl->event_head + 1) % EVENT_CAPACITY;
    if (ctrl->event_count < EVENT_CAPACITY) {
        ctrl->event_count++;
    }
}

static void protection_defaults(struct controller *ctrl)
{
    bool pump = ctrl->config.profile == PROFILE_PUMP;
    ctrl->protections[PROTECTION_LOW_WARNING] =
        (struct protection_config) { pump ? 100 : 50, pump ? 20 : 10, 500, true };
    ctrl->protections[PROTECTION_HIGH_WARNING] =
        (struct protection_config) { pump ? 900 : 350, pump ? 20 : 10, 500, true };
    ctrl->protections[PROTECTION_LOW_TRIP] =
        (struct protection_config) { pump ? 50 : 0, pump ? 20 : 10, 1000, true };
    ctrl->protections[PROTECTION_HIGH_TRIP] =
        (struct protection_config) { pump ? 950 : 380, pump ? 20 : 10, 1000, true };
}

static void update_protections(struct controller *ctrl, int64_t now_ms)
{
    uint32_t previous_warnings = ctrl->active_warnings;
    ctrl->active_warnings = 0;
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        struct protection_state *state = &ctrl->protection_states[i];
        if (ctrl->sample_valid) {
            bool low = i == PROTECTION_LOW_WARNING || i == PROTECTION_LOW_TRIP;
            protection_update(state, &ctrl->protections[i], low,
                              ctrl->sample.measured_x10, now_ms);
        } else {
            // Время с неисправным входом не засчитываем в задержку.
            state->pending = false;
        }
        if (!state->active) continue;
        switch (i) {
        case PROTECTION_LOW_WARNING: ctrl->active_warnings |= WARNING_LOW; break;
        case PROTECTION_HIGH_WARNING: ctrl->active_warnings |= WARNING_HIGH; break;
        case PROTECTION_LOW_TRIP: ctrl->active_faults |= FAULT_PROCESS_LOW; break;
        case PROTECTION_HIGH_TRIP: ctrl->active_faults |= FAULT_PROCESS_HIGH; break;
        }
    }
    uint32_t added = ctrl->active_warnings & ~previous_warnings;
    uint32_t cleared = previous_warnings & ~ctrl->active_warnings;
    if (added) record_event(ctrl, EVENT_WARNING, 0, added, now_ms);
    if (cleared) record_event(ctrl, EVENT_WARNING_CLEARED, 0, cleared, now_ms);
}

void controller_init(struct controller *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->config = (struct controller_config) { PROFILE_PUMP, INPUT_4_20MA };
    ctrl->state = STATE_STOPPED;
    protection_defaults(ctrl);
}

void controller_update(struct controller *ctrl, const struct sensor_sample *sample,
                       int64_t now_ms)
{
    uint32_t previous_faults = ctrl->active_faults;

    struct signal_value signal = sensor_signal_process(&ctrl->signal_filter,
        ctrl->config.profile, ctrl->config.mode, sample->current_ua, sample->voltage_mv);
    ctrl->sample = *sample;
    ctrl->sample.scaled_x10 = signal.scaled_x10;
    ctrl->sample.measured_x10 = signal.filtered_x10;
    ctrl->sample_valid = signal.faults == FAULT_NONE;
    ctrl->active_faults = signal.faults;
    update_protections(ctrl, now_ms);

    if (ctrl->active_faults != FAULT_NONE) {
        ctrl->latched_faults |= ctrl->active_faults;
        ctrl->state = STATE_FAULT;
        ctrl->frequency_x100 = 0;
        ctrl->integral = 0;
        if (ctrl->active_faults != previous_faults) {
            record_event(ctrl, EVENT_FAULT, ctrl->active_faults, ctrl->active_warnings, now_ms);
        }
        return;
    }

    if (previous_faults != FAULT_NONE) {
        record_event(ctrl, EVENT_FAULT_CLEARED, previous_faults, ctrl->active_warnings, now_ms);
    }
    if (ctrl->state == STATE_STOPPED) {
        ctrl->state = STATE_READY;
        record_event(ctrl, EVENT_READY, FAULT_NONE, ctrl->active_warnings, now_ms);
    }
    if (ctrl->state != STATE_RUNNING) {
        return;
    }

    int64_t error = (int64_t)ctrl->sample.setpoint_x10 - ctrl->sample.measured_x10;
    int64_t integral = ctrl->integral + error;
    if (integral > 100000) integral = 100000;
    if (integral < -100000) integral = -100000;
    ctrl->integral = (int)integral;

    int64_t target = 2500 + (50 * error + 5 * integral) / 10;
    if (target > 5000) target = 5000;
    if (target < 0) target = 0;
    int difference = (int)target - ctrl->frequency_x100;
    if (difference > 100) difference = 100;
    if (difference < -100) difference = -100;
    ctrl->frequency_x100 += difference;
}

int controller_start(struct controller *ctrl, int64_t now_ms)
{
    if (ctrl->state != STATE_READY || !ctrl->sample_valid || ctrl->latched_faults) {
        return -EACCES;
    }
    ctrl->state = STATE_RUNNING;
    ctrl->integral = 0;
    record_event(ctrl, EVENT_START, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

void controller_stop(struct controller *ctrl, int64_t now_ms)
{
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    // Команда останова не снимает зафиксированную аварию.
    if (ctrl->state == STATE_RUNNING) {
        ctrl->state = STATE_READY;
        record_event(ctrl, EVENT_STOP, FAULT_NONE, ctrl->active_warnings, now_ms);
    }
}

int controller_reset(struct controller *ctrl, int64_t now_ms)
{
    if (ctrl->state != STATE_FAULT) return -EALREADY;
    if (!ctrl->sample_valid || ctrl->active_faults) return -EBUSY;
    ctrl->latched_faults = FAULT_NONE;
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    ctrl->state = STATE_READY;
    record_event(ctrl, EVENT_RESET, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

int controller_configure(struct controller *ctrl, struct controller_config config,
                         int64_t now_ms)
{
    if ((config.profile != PROFILE_PUMP && config.profile != PROFILE_AHU) ||
        (config.mode != INPUT_4_20MA && config.mode != INPUT_0_10V)) return -EINVAL;
    if (ctrl->state == STATE_RUNNING || ctrl->state == STATE_FAULT) return -EBUSY;
    if (ctrl->active_warnings) {
        record_event(ctrl, EVENT_WARNING_CLEARED, 0, ctrl->active_warnings, now_ms);
    }
    bool profile_changed = ctrl->config.profile != config.profile;
    ctrl->config = config;
    if (profile_changed) protection_defaults(ctrl);
    memset(ctrl->protection_states, 0, sizeof(ctrl->protection_states));
    ctrl->active_warnings = 0;
    ctrl->sample_valid = false;
    sensor_signal_reset(&ctrl->signal_filter);
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    ctrl->state = STATE_STOPPED;
    record_event(ctrl, EVENT_INPUT_CHANGED, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

int controller_protection_configure(struct controller *ctrl, enum protection_id id,
                                    struct protection_config config, int64_t now_ms)
{
    if (id < 0 || id >= PROTECTION_COUNT) return -EINVAL;
    if (ctrl->state == STATE_RUNNING || ctrl->state == STATE_FAULT) return -EBUSY;
    int minimum = ctrl->config.profile == PROFILE_PUMP ? 0 : -200;
    int maximum = ctrl->config.profile == PROFILE_PUMP ? 1000 : 400;
    bool low = id == PROTECTION_LOW_WARNING || id == PROTECTION_LOW_TRIP;
    int64_t clear_limit = (int64_t)config.threshold_x10 +
        (low ? config.hysteresis_x10 : -(int64_t)config.hysteresis_x10);
    if (config.threshold_x10 < minimum || config.threshold_x10 > maximum ||
        config.hysteresis_x10 < 0 || clear_limit < minimum || clear_limit > maximum ||
        config.delay_ms < 0 || config.delay_ms > 60000) return -EINVAL;
    ctrl->protections[id] = config;
    memset(&ctrl->protection_states[id], 0, sizeof(ctrl->protection_states[id]));
    record_event(ctrl, EVENT_PROTECTION_CHANGED, 0, ctrl->active_warnings, now_ms);
    return 0;
}

bool controller_event_get(const struct controller *ctrl, size_t index,
                          struct controller_event *event)
{
    if (index >= ctrl->event_count) return false;
    size_t oldest = (ctrl->event_head + EVENT_CAPACITY - ctrl->event_count) % EVENT_CAPACITY;
    *event = ctrl->events[(oldest + index) % EVENT_CAPACITY];
    return true;
}

const char *controller_state_name(enum controller_state state)
{
    switch (state) {
    case STATE_STOPPED: return "STOPPED";
    case STATE_READY: return "READY";
    case STATE_RUNNING: return "RUNNING";
    case STATE_FAULT: return "FAULT";
    default: return "UNKNOWN";
    }
}

const char *controller_event_name(enum event_type type)
{
    switch (type) {
    case EVENT_WARNING: return "WARNING";
    case EVENT_WARNING_CLEARED: return "WARNING_CLEARED";
    case EVENT_PROTECTION_CHANGED: return "PROTECTION_CHANGED";
    case EVENT_READY: return "READY";
    case EVENT_START: return "START";
    case EVENT_STOP: return "STOP";
    case EVENT_FAULT: return "FAULT";
    case EVENT_FAULT_CLEARED: return "FAULT_CLEARED";
    case EVENT_RESET: return "RESET";
    case EVENT_INPUT_CHANGED: return "INPUT_CHANGED";
    default: return "UNKNOWN";
    }
}
