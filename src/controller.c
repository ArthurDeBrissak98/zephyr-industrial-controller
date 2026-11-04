#include "controller.h"
#include <errno.h>
#include <string.h>

static void record_event(struct controller *ctrl, enum event_type type,
                         uint32_t faults, uint32_t warnings, int64_t now_ms)
{
    struct controller_event *event = &ctrl->events[ctrl->event_head];

    *event = (struct controller_event) {
        .sequence = ++ctrl->event_sequence,
        .profile = ctrl->config.profile,
        .mode = ctrl->config.mode,
        .signal_valid = ctrl->sample_valid,
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
    k_mutex_init(&ctrl->mutex);
    ctrl->last_time_ms = -1;
    ctrl->config = (struct controller_config) { PROFILE_PUMP, INPUT_4_20MA };
    ctrl->state = STATE_STOPPED;
    protection_defaults(ctrl);
}

static void update_pi(struct controller *ctrl, int64_t dt_ms)
{
    int64_t error = (int64_t)ctrl->sample.setpoint_x10 - ctrl->sample.measured_x10;
    // Проверяем запас до насыщения раньше, чем умножаем ошибку на время.
    if (error > 0) {
        int64_t room = CONTROLLER_INTEGRAL_LIMIT - ctrl->integral;
        if (dt_ms > room / error) ctrl->integral = CONTROLLER_INTEGRAL_LIMIT;
        else ctrl->integral += error * dt_ms;
    } else if (error < 0) {
        int64_t room = ctrl->integral + CONTROLLER_INTEGRAL_LIMIT;
        if (dt_ms > room / -error) ctrl->integral = -CONTROLLER_INTEGRAL_LIMIT;
        else ctrl->integral += error * dt_ms;
    }

    // Коэффициент Ki сохраняет прежнее действие при периоде 100 мс.
    int64_t target = 2500 + (50 * error + 5 * ctrl->integral / CONTROLLER_PERIOD_MS) / 10;
    if (target > 5000) target = 5000;
    if (target < 0) target = 0;
    int difference = (int)target - ctrl->frequency_x100;
    if (difference > 100) difference = 100;
    if (difference < -100) difference = -100;
    ctrl->frequency_x100 += difference;
}

static void controller_update_locked(struct controller *ctrl, const struct sensor_sample *sample,
                       int64_t now_ms)
{
    int64_t dt_ms = 0;
    if (now_ms >= 0 && now_ms >= ctrl->last_time_ms) {
        if (ctrl->last_time_ms >= 0) dt_ms = now_ms - ctrl->last_time_ms;
        ctrl->last_time_ms = now_ms;
    }
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

    update_pi(ctrl, dt_ms);
}

static int controller_start_locked(struct controller *ctrl, int64_t now_ms)
{
    if (ctrl->state != STATE_READY || !ctrl->sample_valid || ctrl->latched_faults) {
        return -EACCES;
    }
    ctrl->state = STATE_RUNNING;
    ctrl->integral = 0;
    if (now_ms >= 0 && now_ms > ctrl->last_time_ms) ctrl->last_time_ms = now_ms;
    record_event(ctrl, EVENT_START, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

static void controller_stop_locked(struct controller *ctrl, int64_t now_ms)
{
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    // Команда останова не снимает зафиксированную аварию.
    if (ctrl->state == STATE_RUNNING) {
        ctrl->state = STATE_READY;
        record_event(ctrl, EVENT_STOP, FAULT_NONE, ctrl->active_warnings, now_ms);
    }
}

static int controller_reset_locked(struct controller *ctrl, int64_t now_ms)
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

static int controller_configure_locked(struct controller *ctrl, struct controller_config config,
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

static int controller_protection_configure_locked(struct controller *ctrl, enum protection_id id,
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

static bool controller_event_get_locked(const struct controller *ctrl, size_t index,
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

void controller_update(struct controller *ctrl, const struct sensor_sample *sample, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    controller_update_locked(ctrl, sample, now_ms);
    k_mutex_unlock(&ctrl->mutex);
}

int controller_start(struct controller *ctrl, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_start_locked(ctrl, now_ms);
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

void controller_stop(struct controller *ctrl, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    controller_stop_locked(ctrl, now_ms);
    k_mutex_unlock(&ctrl->mutex);
}

int controller_reset(struct controller *ctrl, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_reset_locked(ctrl, now_ms);
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

int controller_configure(struct controller *ctrl, struct controller_config config, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_configure_locked(ctrl, config, now_ms);
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

int controller_protection_configure(struct controller *ctrl, enum protection_id id, struct protection_config config, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_protection_configure_locked(ctrl, id, config, now_ms);
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

bool controller_event_get(struct controller *ctrl, size_t index, struct controller_event *event)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    bool found = controller_event_get_locked(ctrl, index, event);
    k_mutex_unlock(&ctrl->mutex);
    return found;
}
