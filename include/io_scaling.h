#ifndef IO_SCALING_H
#define IO_SCALING_H

#include "controller.h"

int controller_adc_scale(enum input_mode mode, int millivolts, int shunt_ohms,
                         int voltage_multiplier, struct sensor_sample *sample);

#endif
