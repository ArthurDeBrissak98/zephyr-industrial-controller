#ifndef CONTROLLER_APP_H
#define CONTROLLER_APP_H

#include "controller.h"

struct controller_status {
    enum controller_state state;
    struct controller_config config;
    struct sensor_sample sample;
    uint32_t active_faults;
    uint32_t latched_faults;
    int frequency_x100;
    int output_x100;
};

void controller_app_init(void);
void controller_app_tick(void);
int controller_app_start(void);
void controller_app_stop(void);
int controller_app_reset(void);
int controller_app_configure(struct controller_config config);
int controller_app_inject(bool enabled);
void controller_app_status(struct controller_status *status);
size_t controller_app_events(struct controller_event *events, size_t capacity);

#endif
