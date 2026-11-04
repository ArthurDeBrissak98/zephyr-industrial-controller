#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_signal.h"
#define CONTROLLER_PERIOD_MS 100
#define EVENT_CAPACITY 32

enum controller_state { STATE_STOPPED, STATE_READY, STATE_RUNNING, STATE_FAULT };
enum event_type { EVENT_READY, EVENT_START, EVENT_STOP, EVENT_FAULT,
                  EVENT_FAULT_CLEARED, EVENT_RESET, EVENT_INPUT_CHANGED };

struct controller_config {
    enum controller_profile profile;
    enum input_mode mode;
};

struct sensor_sample {
    int current_ua;
    int voltage_mv;
    int scaled_x10;
    int measured_x10;
    int setpoint_x10;
};

struct controller_event {
    uint32_t sequence;
    int64_t timestamp_ms;
    enum event_type type;
    enum controller_state state;
    uint32_t faults;
    int current_ua;
    int frequency_x100;
    int voltage_mv;
    int measured_x10;
};

struct controller {
    struct controller_config config;
    enum controller_state state;
    struct sensor_sample sample;
    bool sample_valid;
    struct signal_filter signal_filter;
    uint32_t active_faults;
    uint32_t latched_faults;
    int frequency_x100;
    int integral;
    struct controller_event events[EVENT_CAPACITY];
    size_t event_head;
    size_t event_count;
    uint32_t event_sequence;
};

void controller_init(struct controller *ctrl);
void controller_update(struct controller *ctrl, const struct sensor_sample *sample,
                       int64_t now_ms);
int controller_start(struct controller *ctrl, int64_t now_ms);
void controller_stop(struct controller *ctrl, int64_t now_ms);
int controller_reset(struct controller *ctrl, int64_t now_ms);
int controller_configure(struct controller *ctrl, struct controller_config config,
                         int64_t now_ms);
bool controller_event_get(const struct controller *ctrl, size_t index,
                          struct controller_event *event);
const char *controller_state_name(enum controller_state state);
const char *controller_event_name(enum event_type type);

#endif
