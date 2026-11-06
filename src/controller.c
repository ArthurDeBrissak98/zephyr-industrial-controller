#include "controller.h"
#include <errno.h>
#include <string.h>

// REQ-LOG-01
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
        .motor = { .current_ma = ctrl->sample.motor.current_ma,
                   .frequency_x100 = ctrl->sample.motor.frequency_x100,
                   .valid = ctrl->motor_feedback_valid,
                   .available = ctrl->sample.motor.available },
        .motor_nominal_current_ma = ctrl->config.motor.nominal_current_ma,
        .motor_nominal_frequency_x100 = ctrl->config.motor.nominal_frequency_x100,
    };
    ctrl->event_head = (ctrl->event_head + 1) % EVENT_CAPACITY;
    if (ctrl->event_count < EVENT_CAPACITY) {
        ctrl->event_count++;
    }
}

static void process_protection_defaults(struct controller *ctrl)
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

static int motor_percent(int nominal, int percent)
{
    // Округляем вверх: порог не должен оказаться ниже заданного процента.
    return (int)(((int64_t)nominal * percent + 99) / 100);
}

// REQ-MOTOR-03
static void motor_protection_defaults(struct controller *ctrl)
{
    const struct motor_parameters *motor = &ctrl->config.motor;
    int nominal[] = {motor->nominal_current_ma, motor->nominal_frequency_x100};
    for (int channel = 0; channel < 2; channel++) {
        bool frequency = channel == 1;
        ctrl->motor_protections[channel * 2] = (struct protection_limit) {
            frequency ? (int)((int64_t)nominal[channel] * 105 / 100) : motor_percent(nominal[channel], 105),
            motor_percent(nominal[channel], 2), frequency ? 0 : 500, true,
        };
        ctrl->motor_protections[channel * 2 + 1] = (struct protection_limit) {
            frequency ? (int)((int64_t)nominal[channel] * 110 / 100) : motor_percent(nominal[channel], 110),
            motor_percent(nominal[channel], 2), frequency ? 0 : 1000, true,
        };
    }
    memset(ctrl->motor_protection_states, 0, sizeof(ctrl->motor_protection_states));
}

static void protection_defaults(struct controller *ctrl)
{
    process_protection_defaults(ctrl);
    motor_protection_defaults(ctrl);
}

// REQ-MOTOR-04
static void update_motor_protections(struct controller *ctrl, int64_t now_ms)
{
    for (int i = 0; i < MOTOR_PROTECTION_COUNT; i++) {
        struct protection_state *state = &ctrl->motor_protection_states[i];
        bool frequency = i == MOTOR_FREQUENCY_WARNING || i == MOTOR_FREQUENCY_TRIP;
        const struct protection_limit *limit = &ctrl->motor_protections[i];
        // Активную причину проверяем и после останова, иначе её нельзя будет снять.
        if (frequency && !state->active &&
            (ctrl->state != STATE_RUNNING || ctrl->config.motor.nominal_frequency_x100 <= 0 ||
             ctrl->sample.motor.frequency_x100 <= limit->threshold)) {
            state->pending = false;
            continue;
        }
        if (ctrl->motor_feedback_valid) {
            int value = i < 2 ? ctrl->sample.motor.current_ma : ctrl->sample.motor.frequency_x100;
            protection_update_limit(state, &ctrl->motor_protections[i], false, value, now_ms);
        } else {
            // Потеря обратной связи не сбрасывает уже активную моторную защиту.
            state->pending = false;
        }
        if (!state->active) continue;
        switch (i) {
        case MOTOR_CURRENT_WARNING: ctrl->active_warnings |= WARNING_MOTOR_CURRENT; break;
        case MOTOR_CURRENT_TRIP: ctrl->process_faults |= FAULT_MOTOR_OVERCURRENT; break;
        case MOTOR_FREQUENCY_WARNING: ctrl->active_warnings |= WARNING_MOTOR_FREQUENCY; break;
        case MOTOR_FREQUENCY_TRIP: ctrl->process_faults |= FAULT_MOTOR_OVERFREQUENCY; break;
        }
    }
}

// REQ-PROT-02
static void update_protections(struct controller *ctrl, int64_t now_ms)
{
    uint32_t previous_warnings = ctrl->active_warnings;
    ctrl->active_warnings = 0;
    ctrl->process_faults = 0;
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
        case PROTECTION_LOW_TRIP: ctrl->process_faults |= FAULT_PROCESS_LOW; break;
        case PROTECTION_HIGH_TRIP: ctrl->process_faults |= FAULT_PROCESS_HIGH; break;
        }
    }
    update_motor_protections(ctrl, now_ms);
    uint32_t added = ctrl->active_warnings & ~previous_warnings;
    uint32_t cleared = previous_warnings & ~ctrl->active_warnings;
    if (added) record_event(ctrl, EVENT_WARNING, 0, added, now_ms);
    if (cleared) record_event(ctrl, EVENT_WARNING_CLEARED, 0, cleared, now_ms);
}

uint32_t calculate_pole_pairs(uint32_t nominal_freq_x100, uint32_t nominal_speed_rpm)
{
    if (nominal_speed_rpm < 50 || nominal_speed_rpm > 30000 ||
        nominal_freq_x100 < 100 || nominal_freq_x100 > 50000) return 0;

    // Сохраняем сотые доли герца; промежуточные произведения могут не влезть в uint32_t.
    uint64_t numerator = (uint64_t)60 * nominal_freq_x100;
    uint64_t denominator = (uint64_t)100 * nominal_speed_rpm;
    uint32_t pole_pairs_calc = (uint32_t)((numerator + denominator / 2) / denominator);

    return pole_pairs_calc >= 1 && pole_pairs_calc <= 100 ? pole_pairs_calc : 0;
}

static int motor_sync_speed_and_slip(const struct motor_parameters *parameters,
                                    struct motor_data *data)
{
    if (data->pole_pairs_calc == 0 || data->pole_pairs_calc > 100 ||
        parameters->nominal_frequency_x100 <= 0 || parameters->nominal_speed_rpm <= 0)
        return -EINVAL;

    // Частота уже в сотых герца: 50 Гц и две пары дают 1500 об/мин.
    uint32_t sync_speed_rpm = (uint64_t)60 * parameters->nominal_frequency_x100 /
                             ((uint64_t)data->pole_pairs_calc * 100);
    if (sync_speed_rpm == 0 || (uint32_t)parameters->nominal_speed_rpm > sync_speed_rpm)
        return -EINVAL;
    int64_t slip_x1000 = ((int64_t)sync_speed_rpm - parameters->nominal_speed_rpm) * 1000 /
                        sync_speed_rpm;
    if (slip_x1000 < 0 || slip_x1000 > 200) return -EINVAL;

    data->sync_speed_rpm = sync_speed_rpm;
    data->slip_x1000 = (int32_t)slip_x1000;
    data->max_freq_hz_x100 = (uint64_t)parameters->nominal_frequency_x100 * 120 / 100;
    return 0;
}

int calculate_sync_speed_and_slip(struct controller *ctrl)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = motor_sync_speed_and_slip(&ctrl->config.motor, &ctrl->motor);
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

// REQ-MOTOR-02
int controller_motor_validate(const struct motor_parameters *motor)
{
    if (motor->nominal_voltage_v <= 0 || motor->nominal_voltage_v > UINT16_MAX ||
        motor->nominal_current_ma <= 0 || motor->nominal_current_ma > MOTOR_MAX_CURRENT_MA ||
        motor->nominal_frequency_x100 <= 0 || motor->nominal_frequency_x100 > UINT16_MAX ||
        motor->nominal_speed_rpm <= 0 || motor->nominal_speed_rpm > UINT16_MAX ||
        motor->pole_pairs <= 0 || motor->pole_pairs > UINT16_MAX ||
        (unsigned int)motor->type > MOTOR_SYNRM) return -EINVAL;
    if (calculate_pole_pairs(motor->nominal_frequency_x100, motor->nominal_speed_rpm) == 0)
        return -EINVAL;
    // Номинальные обороты не могут превышать синхронную скорость поля.
    int64_t speed = (int64_t)motor->nominal_speed_rpm * motor->pole_pairs * 100;
    if (speed > (int64_t)motor->nominal_frequency_x100 * 60) return -EINVAL;
    struct motor_data data = {
        .pole_pairs_calc = calculate_pole_pairs(motor->nominal_frequency_x100, motor->nominal_speed_rpm),
    };
    return motor_sync_speed_and_slip(motor, &data);
}

static int validate_config(struct controller_config config)
{
    if ((config.profile != PROFILE_PUMP && config.profile != PROFILE_AHU) ||
        (config.mode != INPUT_4_20MA && config.mode != INPUT_0_10V)) return -EINVAL;
    if (config.pid.bias_x100 < 0 || config.pid.bias_x100 > 5000 ||
        config.pid.kp < 0 || config.pid.ki < 0 || config.pid.divisor <= 0) return -EINVAL;
    if (controller_motor_validate(&config.motor)) return -EINVAL;
    return 0;
}

static int validate_protection(enum controller_profile profile, enum protection_id id,
                               struct protection_config config)
{
    int minimum = profile == PROFILE_PUMP ? 0 : -200;
    int maximum = profile == PROFILE_PUMP ? 1000 : 400;
    bool low = id == PROTECTION_LOW_WARNING || id == PROTECTION_LOW_TRIP;
    int64_t clear_limit = (int64_t)config.threshold_x10 +
        (low ? config.hysteresis_x10 : -(int64_t)config.hysteresis_x10);
    if (config.threshold_x10 < minimum || config.threshold_x10 > maximum ||
        config.hysteresis_x10 < 0 || clear_limit < minimum || clear_limit > maximum ||
        config.delay_ms < 0 || config.delay_ms > 60000) return -EINVAL;
    return 0;
}

int controller_parameters_validate(const struct controller_parameters *parameters)
{
    if (validate_config(parameters->config)) return -EINVAL;
    const struct protection_config *p = parameters->protections;
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        if (validate_protection(parameters->config.profile, i, p[i])) return -EINVAL;
    }
    if (p[PROTECTION_LOW_TRIP].threshold_x10 >= p[PROTECTION_HIGH_TRIP].threshold_x10 ||
        p[PROTECTION_LOW_WARNING].threshold_x10 >= p[PROTECTION_HIGH_WARNING].threshold_x10) {
        return -EINVAL;
    }
    return 0;
}

// REQ-MOTOR-01
struct motor_parameters controller_motor_defaults(void)
{
    return (struct motor_parameters) {
        .nominal_voltage_v = 400,
        .nominal_current_ma = 11500,
        .nominal_frequency_x100 = 5000,
        .nominal_speed_rpm = 1450,
        .pole_pairs = 2,
        .type = MOTOR_INDUCTION,
    };
}

void controller_init(struct controller *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    k_mutex_init(&ctrl->mutex);
    ctrl->last_update_ms = 0;
    ctrl->config = (struct controller_config) {
        .profile = PROFILE_PUMP,
        .mode = INPUT_4_20MA,
        .pid = { .bias_x100 = 2500, .kp = 50, .ki = 5, .divisor = 10 },
        .motor = controller_motor_defaults(),
    };
    ctrl->state = STATE_STOPPED;
    protection_defaults(ctrl);
    ctrl->motor.pole_pairs_calc = calculate_pole_pairs(ctrl->config.motor.nominal_frequency_x100,
                                                     ctrl->config.motor.nominal_speed_rpm);
    if (calculate_sync_speed_and_slip(ctrl)) {
        ctrl->motor.slip_x1000 = 0;
        ctrl->process_faults |= FAULT_CONFIG_ERROR;
        ctrl->active_faults = ctrl->sensor_faults | ctrl->process_faults;
        ctrl->latched_faults |= FAULT_CONFIG_ERROR;
        ctrl->state = STATE_FAULT;
        record_event(ctrl, EVENT_FAULT, FAULT_CONFIG_ERROR, 0, 0);
    }
}

static int64_t pi_target(const struct pid_config *config, int64_t error, int64_t integral)
{
    return config->bias_x100 +
        ((int64_t)config->kp * error + (int64_t)config->ki * integral / CONTROLLER_PERIOD_MS) /
        config->divisor;
}

// REQ-PI-01
static void update_pi(struct controller *ctrl, int64_t dt_ms)
{
    const struct pid_config *config = &ctrl->config.pid;
    int64_t error = (int64_t)ctrl->sample.setpoint_x10 - ctrl->sample.measured_x10;
    int64_t candidate = ctrl->integral;

    // Проверяем запас до насыщения раньше, чем умножаем ошибку на время.
    if (dt_ms > 0 && error > 0) {
        int64_t room = CONTROLLER_INTEGRAL_LIMIT - candidate;
        candidate = dt_ms > room / error ? CONTROLLER_INTEGRAL_LIMIT : candidate + error * dt_ms;
    } else if (dt_ms > 0 && error < 0) {
        int64_t room = candidate + CONTROLLER_INTEGRAL_LIMIT;
        candidate = dt_ms > room / -error ? -CONTROLLER_INTEGRAL_LIMIT : candidate + error * dt_ms;
    }

    int64_t target = pi_target(config, error, candidate);
    int64_t slip_comp_x100 = 0;
    // Slip compensation: добавляет частоту пропорционально нагрузке (I/I_nom * slip_nom).
    if (ctrl->motor_feedback_valid && ctrl->config.motor.nominal_current_ma > 0 &&
        ctrl->config.motor.nominal_frequency_x100 > 0 && ctrl->motor.slip_x1000 > 0) {
        int64_t load_x1000 = (int64_t)ctrl->sample.motor.current_ma * 1000 /
                            ctrl->config.motor.nominal_current_ma;
        int64_t factor = (int64_t)ctrl->motor.slip_x1000 * ctrl->config.motor.nominal_frequency_x100;
        // Ограничиваем до тройного произведения: даже завышенный ток не переполнит int64_t.
        if (load_x1000 > 500000000LL / factor) {
            slip_comp_x100 = 500;
        } else {
            slip_comp_x100 = factor * load_x1000 / 1000000LL;
        }
        if (slip_comp_x100 > 500) slip_comp_x100 = 500;
        if (slip_comp_x100 < 0) slip_comp_x100 = 0;
    }
    target += slip_comp_x100;
    int64_t max_frequency = MIN(5000U, ctrl->motor.max_freq_hz_x100);
    bool pushes_saturation = (target >= max_frequency && error > 0) || (target <= 0 && error < 0);
    if (!pushes_saturation) ctrl->integral = candidate;
    // При обратном знаке ошибки интегратор может выйти из насыщения.
    if (target > max_frequency) target = max_frequency;
    if (target < 0) target = 0;
    int difference = (int)target - ctrl->frequency_x100;
    if (difference > 100) difference = 100;
    if (difference < -100) difference = -100;
    ctrl->frequency_x100 += difference;
}

// REQ-SENS-02
// REQ-MOTOR-05
static void controller_update_locked(struct controller *ctrl, const struct sensor_sample *sample,
                       int64_t now_ms)
{
    int64_t dt_ms = 0;
    if (now_ms > ctrl->last_update_ms) {
        // Разность крайних int64_t тоже может не поместиться в int64_t.
        if (ctrl->last_update_ms < 0 && now_ms > INT64_MAX + ctrl->last_update_ms) {
            dt_ms = INT64_MAX;
        } else {
            dt_ms = now_ms - ctrl->last_update_ms;
        }
    }
    uint32_t previous_faults = ctrl->active_faults;

    struct signal_value signal;
    if (sample->read_error) {
        sensor_signal_reset(&ctrl->signal_filter);
        signal = (struct signal_value) { .faults = FAULT_INPUT_IO };
    } else {
        signal = sensor_signal_process(&ctrl->signal_filter,
            ctrl->config.profile, ctrl->config.mode, sample->current_ua, sample->voltage_mv);
    }
    ctrl->sample = *sample;
    ctrl->sample.scaled_x10 = signal.scaled_x10;
    ctrl->sample.measured_x10 = signal.filtered_x10;
    ctrl->sample_valid = signal.faults == FAULT_NONE;
    ctrl->sensor_faults = signal.faults;
    ctrl->motor_feedback_seen |= sample->motor.available || sample->motor.valid;
    ctrl->motor_feedback_valid = sample->motor.available && sample->motor.valid && sample->motor.current_ma >= 0 &&
                                 sample->motor.frequency_x100 >= 0;
    if (ctrl->motor_feedback_seen && !ctrl->motor_feedback_valid) ctrl->sensor_faults |= FAULT_MOTOR_FEEDBACK;
    update_protections(ctrl, now_ms);
    // Проверяем и рабочую копию: повреждённые параметры не должны попасть в расчёт управления.
    if (ctrl->motor.pole_pairs_calc == 0 || controller_motor_validate(&ctrl->config.motor))
        ctrl->process_faults |= FAULT_CONFIG_ERROR;
    ctrl->active_faults = ctrl->sensor_faults | ctrl->process_faults;

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
    ctrl->last_update_ms = now_ms;
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

// REQ-CTRL-01
static int controller_reset_locked(struct controller *ctrl, int64_t now_ms)
{
    if (ctrl->state != STATE_FAULT) return -EALREADY;
    if (!ctrl->sample_valid || ctrl->active_faults) return -EBUSY;
    ctrl->latched_faults = FAULT_NONE;
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    ctrl->state = STATE_READY;
    ctrl->last_update_ms = now_ms;
    record_event(ctrl, EVENT_RESET, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

static int controller_configure_locked(struct controller *ctrl, struct controller_config config,
                         int64_t now_ms)
{
    if (validate_config(config)) return -EINVAL;
    if (ctrl->state == STATE_RUNNING || ctrl->state == STATE_FAULT) return -EBUSY;
    struct controller_config previous_config = ctrl->config;
    struct motor_data previous_motor = ctrl->motor;
    bool profile_changed = ctrl->config.profile != config.profile;
    ctrl->config = config;
    ctrl->motor.pole_pairs_calc = calculate_pole_pairs(config.motor.nominal_frequency_x100,
                                                     config.motor.nominal_speed_rpm);
    if (calculate_sync_speed_and_slip(ctrl)) {
        ctrl->config = previous_config;
        ctrl->motor = previous_motor;
        return -EINVAL;
    }
    // До успешного расчёта не меняем защиты, журнал и состояние управления.
    if (ctrl->active_warnings) {
        record_event(ctrl, EVENT_WARNING_CLEARED, 0, ctrl->active_warnings, now_ms);
    }
    if (profile_changed) process_protection_defaults(ctrl);
    motor_protection_defaults(ctrl);
    memset(ctrl->protection_states, 0, sizeof(ctrl->protection_states));
    ctrl->active_warnings = 0;
    ctrl->sample_valid = false;
    sensor_signal_reset(&ctrl->signal_filter);
    ctrl->frequency_x100 = 0;
    ctrl->integral = 0;
    ctrl->state = STATE_STOPPED;
    ctrl->last_update_ms = now_ms;
    record_event(ctrl, EVENT_INPUT_CHANGED, FAULT_NONE, ctrl->active_warnings, now_ms);
    return 0;
}

static int controller_protection_configure_locked(struct controller *ctrl, enum protection_id id,
                                    struct protection_config config, int64_t now_ms)
{
    if ((unsigned int)id >= PROTECTION_COUNT) return -EINVAL;
    if (ctrl->state == STATE_RUNNING || ctrl->state == STATE_FAULT) return -EBUSY;
    if (validate_protection(ctrl->config.profile, id, config)) return -EINVAL;
    bool low = id == PROTECTION_LOW_WARNING || id == PROTECTION_LOW_TRIP;
    enum protection_id opposite;
    switch (id) {
    case PROTECTION_LOW_TRIP: opposite = PROTECTION_HIGH_TRIP; break;
    case PROTECTION_HIGH_TRIP: opposite = PROTECTION_LOW_TRIP; break;
    case PROTECTION_LOW_WARNING: opposite = PROTECTION_HIGH_WARNING; break;
    default: opposite = PROTECTION_LOW_WARNING; break;
    }
    int other_threshold = ctrl->protections[opposite].threshold_x10;
    if ((low && config.threshold_x10 >= other_threshold) ||
        (!low && config.threshold_x10 <= other_threshold)) return -EINVAL;
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
    case EVENT_SETTINGS_LOADED: return "SETTINGS_LOADED";
    case EVENT_MOTOR_CHANGED: return "MOTOR_CHANGED";
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

// REQ-CONC-01
void controller_update(struct controller *ctrl, const struct sensor_sample *sample, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    controller_update_locked(ctrl, sample, now_ms);
    ctrl->last_update_ms = now_ms;
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

void controller_parameters_get(struct controller *ctrl, struct controller_parameters *parameters)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    parameters->config = ctrl->config;
    memcpy(parameters->protections, ctrl->protections, sizeof(ctrl->protections));
    k_mutex_unlock(&ctrl->mutex);
}

int controller_parameters_apply(struct controller *ctrl,
                                const struct controller_parameters *parameters, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_parameters_validate(parameters);
    if (!ret) ret = controller_configure_locked(ctrl, parameters->config, now_ms);
    if (!ret) {
        memcpy(ctrl->protections, parameters->protections, sizeof(ctrl->protections));
        record_event(ctrl, EVENT_SETTINGS_LOADED, 0, ctrl->active_warnings, now_ms);
    }
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}

// REQ-LOG-02
void controller_events_clear(struct controller *ctrl)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    memset(ctrl->events, 0, sizeof(ctrl->events));
    ctrl->event_head = 0;
    ctrl->event_count = 0;
    // Нумерацию продолжаем, чтобы новые записи не путались со старой выгрузкой.
    k_mutex_unlock(&ctrl->mutex);
}

int controller_motor_configure(struct controller *ctrl, struct motor_parameters motor, int64_t now_ms)
{
    k_mutex_lock(&ctrl->mutex, K_FOREVER);
    int ret = controller_motor_validate(&motor);
    if (!ret && ctrl->state != STATE_STOPPED && ctrl->state != STATE_READY) ret = -EBUSY;
    if (!ret) {
        struct motor_parameters previous_parameters = ctrl->config.motor;
        struct motor_data previous_motor = ctrl->motor;
        ctrl->config.motor = motor;
        ctrl->motor.pole_pairs_calc = calculate_pole_pairs(motor.nominal_frequency_x100,
                                                         motor.nominal_speed_rpm);
        ret = calculate_sync_speed_and_slip(ctrl);
        if (ret) {
            ctrl->config.motor = previous_parameters;
            ctrl->motor = previous_motor;
            goto out;
        }
        uint32_t cleared = ctrl->active_warnings & MOTOR_WARNING_MASK;
        motor_protection_defaults(ctrl);
        ctrl->active_warnings &= ~MOTOR_WARNING_MASK;
        if (cleared) record_event(ctrl, EVENT_WARNING_CLEARED, 0, cleared, now_ms);
        record_event(ctrl, EVENT_MOTOR_CHANGED, 0, ctrl->active_warnings, now_ms);
    }
out:
    k_mutex_unlock(&ctrl->mutex);
    return ret;
}
