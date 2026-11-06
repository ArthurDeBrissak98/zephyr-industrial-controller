#ifndef CONTROLLER_APP_H
#define CONTROLLER_APP_H

#include "controller.h"

struct controller_status {
    enum controller_state state;
    struct controller_config config;
    struct motor_data motor;
    struct sensor_sample sample;
    struct protection_config protections[PROTECTION_COUNT];
    struct protection_limit motor_protections[MOTOR_PROTECTION_COUNT];
    bool motor_feedback_valid;
    bool motor_current_injected;
    bool motor_frequency_injected;
    uint32_t active_warnings;
    uint32_t sensor_faults;
    uint32_t process_faults;
    uint32_t active_faults;
    uint32_t latched_faults;
    int frequency_x100;
    int output_x100;
    bool signal_valid;
    unsigned int filter_count;
    bool signal_injected;
    bool wire_break_injected;
    int injected_value;
    bool settings_ready;
    bool settings_present;
    int settings_error;
};

int controller_app_settings_init(void);
int controller_app_settings_save(void);
int controller_app_settings_load(void);
int controller_app_settings_erase(void);
void controller_app_init(void);
void controller_app_tick(void);
int controller_app_start(void);
int controller_app_setpoint_set(int setpoint_x10);
int controller_app_motor_save(unsigned int field, int value);
int controller_app_motor_configure(unsigned int first, const uint16_t *values, size_t count);
void controller_app_stop(void);
int controller_app_reset(void);
int controller_app_configure(struct controller_config config);
int controller_app_protection_configure(enum protection_id id, struct protection_config config);
int controller_app_inject_motor(enum motor_feedback_channel channel, bool enabled, int value);
int controller_app_inject(bool enabled);
int controller_app_inject_signal(bool enabled, int value);
void controller_app_status(struct controller_status *status);
void controller_app_events_clear(void);
size_t controller_app_events(struct controller_event *events, size_t capacity);

#endif
