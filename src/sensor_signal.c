#include "sensor_signal.h"
#include <string.h>

void sensor_signal_reset(struct signal_filter *filter)
{
    memset(filter, 0, sizeof(*filter));
}

struct signal_value sensor_signal_process(struct signal_filter *filter,
                                         enum controller_profile profile,
                                         enum input_mode mode,
                                         int current_ua, int voltage_mv)
{
    struct signal_value value = {0};
    int input;
    int input_min;
    int input_max;

    if (profile != PROFILE_PUMP && profile != PROFILE_AHU) {
        value.faults = FAULT_INPUT_RANGE;
    } else if (mode == INPUT_4_20MA) {
        input = current_ua;
        input_min = 4000;
        input_max = 20000;
        if (input < 3600) value.faults = FAULT_WIRE_BREAK;
        else if (input > 21000) value.faults = FAULT_INPUT_RANGE;
    } else if (mode == INPUT_0_10V) {
        input = voltage_mv;
        input_min = 0;
        input_max = 10000;
        if (input < input_min || input > input_max) value.faults = FAULT_INPUT_RANGE;
    } else {
        value.faults = FAULT_INPUT_RANGE;
    }

    // Аварийный отсчёт не попадает в фильтр. После восстановления начинаем заново.
    if (value.faults) {
        sensor_signal_reset(filter);
        return value;
    }

    // Для токовой петли допускаем небольшой выход за номинальные 4–20 мА.
    if (input < input_min) input = input_min;
    if (input > input_max) input = input_max;
    int output_min = profile == PROFILE_PUMP ? 0 : -200;
    int output_max = profile == PROFILE_PUMP ? 1000 : 400;
    value.scaled_x10 = output_min +
        (int)((int64_t)(input - input_min) * (output_max - output_min) /
              (input_max - input_min));

    if (filter->count == SIGNAL_FILTER_SIZE) {
        filter->sum -= filter->samples[filter->next];
    } else {
        filter->count++;
    }
    filter->samples[filter->next] = value.scaled_x10;
    filter->sum += value.scaled_x10;
    filter->next = (filter->next + 1) % SIGNAL_FILTER_SIZE;
    value.filtered_x10 = (int)(filter->sum / filter->count);
    return value;
}
