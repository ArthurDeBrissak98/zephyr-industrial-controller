#include "controller.h"
#include <errno.h>
#include <string.h>

static void record_event(struct controller *ctrl, enum event_type type,
                         uint32_t faults, int64_t now_ms)
{
    struct controller_event *event = &ctrl->events[ctrl->event_head];

    *event = (struct controller_event) {
        .sequence = ++ctrl->event_sequence,
        .timestamp_ms = now_ms,
        .type = type,
        .state = ctrl->state,
        .faults = faults,
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

void controller_init(struct controller *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->config = (struct controller_config) { PROFILE_PUMP, INPUT_4_20MA };
    ctrl->state = STATE_STOPPED;
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

    if (ctrl->active_faults != FAULT_NONE) {
        ctrl->latched_faults |= ctrl->active_faults;
        ctrl->state = STATE_FAULT;
        ctrl->frequency_x100 = 0;
        ctrl->integral = 0;
        if (ctrl->active_faults != previous_faults) {
            record_event(ctrl, EVENT_FAULT, ctrl->active_faults, now_ms);
        }
        return;
    }

    if (previous_faults != FAULT_NONE) {
        record_event(ctrl, EVENT_FAULT_CLEARED, previous_faults, now_ms);
    }
    if (ctrl->state == STATE_STOPPED) {
        ctrl->state = STATE_READY;
        record_event(ctrl, EVENT_READY, FAULT_NONE, now_ms);
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
    record_event(ctrl, EVENT_START, FAULT_NONE, now_ms);
    return 0;
}

void controller_stop(struct controller *ctrl, int64_t now_ms)
{
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    // Команда останова не снимает зафиксированную аварию.
    if (ctrl->state == STATE_RUNNING) {
        ctrl->state = STATE_READY;
        record_event(ctrl, EVENT_STOP, FAULT_NONE, now_ms);
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
    record_event(ctrl, EVENT_RESET, FAULT_NONE, now_ms);
    return 0;
}

int controller_configure(struct controller *ctrl, struct controller_config config,
                         int64_t now_ms)
{
    if ((config.profile != PROFILE_PUMP && config.profile != PROFILE_AHU) ||
        (config.mode != INPUT_4_20MA && config.mode != INPUT_0_10V)) return -EINVAL;
    if (ctrl->state == STATE_RUNNING || ctrl->state == STATE_FAULT) return -EBUSY;
    ctrl->config = config;
    ctrl->sample_valid = false;
    sensor_signal_reset(&ctrl->signal_filter);
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    ctrl->state = STATE_STOPPED;
    record_event(ctrl, EVENT_INPUT_CHANGED, FAULT_NONE, now_ms);
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
