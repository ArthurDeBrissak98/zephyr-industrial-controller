#include "io_scaling.h"
#include <errno.h>
#include <limits.h>

int controller_adc_scale(enum input_mode mode, int millivolts, int shunt_ohms,
                         int voltage_multiplier, struct sensor_sample *sample)
{
    if (millivolts < 0 || shunt_ohms <= 0 || voltage_multiplier <= 0) return -EINVAL;
    int64_t value;
    if (mode == INPUT_4_20MA) {
        value = ((int64_t)millivolts * 1000 + shunt_ohms / 2) / shunt_ohms;
        if (value > INT_MAX) return -EIO;
        sample->current_ua = value;
        return value < 3600 || value > 21000 ? -EIO : 0;
    }
    if (mode == INPUT_0_10V) {
        value = (int64_t)millivolts * voltage_multiplier;
        if (value > INT_MAX) return -EIO;
        sample->voltage_mv = value;
        return value > 10000 ? -EIO : 0;
    }
    return -ENOTSUP;
}
