#ifndef SENSOR_SIGNAL_H
#define SENSOR_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#define FAULT_NONE 0U
#define FAULT_WIRE_BREAK (1U << 0)
#define FAULT_INPUT_RANGE (1U << 1)
#define FAULT_INPUT_IO (1U << 4)
#define SIGNAL_FILTER_SIZE 4

enum controller_profile { PROFILE_PUMP, PROFILE_AHU };
enum input_mode { INPUT_4_20MA, INPUT_0_10V };

struct signal_filter {
    int samples[SIGNAL_FILTER_SIZE];
    int64_t sum;
    unsigned int count;
    unsigned int next;
};

struct signal_value {
    int scaled_x10;
    int filtered_x10;
    uint32_t faults;
};

void sensor_signal_reset(struct signal_filter *filter);
struct signal_value sensor_signal_process(struct signal_filter *filter,
                                         enum controller_profile profile,
                                         enum input_mode mode,
                                         int current_ua, int voltage_mv);

#endif
