#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "sensor_signal.h"
#include "protection.h"
#define CONTROLLER_PERIOD_MS 100
#define EVENT_CAPACITY 32
#define CONTROLLER_INTEGRAL_LIMIT (100000LL * CONTROLLER_PERIOD_MS)

enum controller_state { STATE_STOPPED, STATE_READY, STATE_RUNNING, STATE_FAULT };
enum event_type { EVENT_READY, EVENT_START, EVENT_STOP, EVENT_FAULT,
                  EVENT_FAULT_CLEARED, EVENT_RESET, EVENT_INPUT_CHANGED,
                  EVENT_WARNING, EVENT_WARNING_CLEARED, EVENT_PROTECTION_CHANGED, EVENT_SETTINGS_LOADED };

struct pid_config {
    int bias_x100;
    int kp;
    int ki;
    int divisor;
};

struct motor_parameters {
    int nominal_voltage_v;
    int nominal_current_ma;
    int nominal_frequency_x100;
    int nominal_speed_rpm;
    int pole_pairs;
};

struct controller_config {
    enum controller_profile profile;
    enum input_mode mode;
    struct pid_config pid;
    struct motor_parameters motor;
};

struct controller_parameters {
    struct controller_config config;
    struct protection_config protections[PROTECTION_COUNT];
};

struct sensor_sample {
    int read_error;
    int current_ua;
    int voltage_mv;
    int scaled_x10;
    int measured_x10;
    int setpoint_x10;
};

struct controller_event {
    uint32_t sequence;
    enum controller_profile profile;
    enum input_mode mode;
    bool signal_valid;
    int64_t timestamp_ms;
    enum event_type type;
    enum controller_state state;
    uint32_t faults;
    uint32_t warnings;
    int current_ua;
    int frequency_x100;
    int voltage_mv;
    int measured_x10;
};

struct controller {
    struct k_mutex mutex;
    struct controller_config config;
    enum controller_state state;
    struct sensor_sample sample;
    bool sample_valid;
    struct signal_filter signal_filter;
    struct protection_config protections[PROTECTION_COUNT];
    struct protection_state protection_states[PROTECTION_COUNT];
    uint32_t active_warnings;
    uint32_t sensor_faults;
    uint32_t process_faults;
    uint32_t active_faults;
    uint32_t latched_faults;
    int frequency_x100;
    int64_t integral;
    int64_t last_update_ms;
    struct controller_event events[EVENT_CAPACITY];
    size_t event_head;
    size_t event_count;
    uint32_t event_sequence;
};

// Инициализация до запуска потоков. Остальные операции с контроллером — не из ISR.
int controller_motor_validate(const struct motor_parameters *motor);
struct motor_parameters controller_motor_defaults(void);
void controller_init(struct controller *ctrl);
void controller_update(struct controller *ctrl, const struct sensor_sample *sample,
                       int64_t now_ms);
int controller_start(struct controller *ctrl, int64_t now_ms);
void controller_stop(struct controller *ctrl, int64_t now_ms);
int controller_reset(struct controller *ctrl, int64_t now_ms);
int controller_configure(struct controller *ctrl, struct controller_config config,
                         int64_t now_ms);
int controller_protection_configure(struct controller *ctrl, enum protection_id id,
                                    struct protection_config config, int64_t now_ms);
int controller_parameters_validate(const struct controller_parameters *parameters);
void controller_parameters_get(struct controller *ctrl, struct controller_parameters *parameters);
int controller_parameters_apply(struct controller *ctrl,
                                const struct controller_parameters *parameters, int64_t now_ms);
bool controller_event_get(struct controller *ctrl, size_t index,
                          struct controller_event *event);
void controller_events_clear(struct controller *ctrl);
const char *controller_state_name(enum controller_state state);
const char *controller_event_name(enum event_type type);

#endif
