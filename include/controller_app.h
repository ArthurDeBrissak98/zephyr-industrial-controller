#ifndef CONTROLLER_APP_H
#define CONTROLLER_APP_H

#include "controller.h"

struct controller_status {
    enum controller_state state;
    struct controller_config config;
    struct sensor_sample sample;
    struct protection_config protections[PROTECTION_COUNT];
    uint32_t active_warnings;
    uint32_t active_faults;
    uint32_t latched_faults;
    int frequency_x100;
    int output_x100;
    bool signal_valid;
};

void controller_app_init(void);
void controller_app_tick(void);
int controller_app_start(void);
void controller_app_stop(void);
int controller_app_reset(void);
int controller_app_configure(struct controller_config config);
int controller_app_protection_configure(enum protection_id id, struct protection_config config);
int controller_app_inject(bool enabled);
int controller_app_inject_signal(bool enabled, int value);
void controller_app_status(struct controller_status *status);
size_t controller_app_events(struct controller_event *events, size_t capacity);

#endif
