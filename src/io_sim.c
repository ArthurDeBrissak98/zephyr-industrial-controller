#include "controller_io.h"

static bool wire_break;
static int output_frequency;

void controller_io_read(const struct controller_config *config, struct sensor_sample *sample)
{
    // Небольшое отклонение от уставки зависит от команды приводу.
    int deviation = output_frequency > 0 ? (output_frequency - 2500) / 500 : 0;
    int measured = (config->profile == PROFILE_PUMP ? 500 : 200) + deviation;
    int percent_x10 = config->profile == PROFILE_PUMP ? measured : (measured + 200) * 1000 / 600;

    *sample = (struct sensor_sample) {
        .setpoint_x10 = config->profile == PROFILE_PUMP ? 500 : 200,
        .measured_x10 = measured,
        .current_ua = config->mode == INPUT_4_20MA ? 4000 + percent_x10 * 16 : -1,
        .voltage_mv = config->mode == INPUT_0_10V ? percent_x10 * 10 : -1,
    };
    if (wire_break && config->mode == INPUT_4_20MA) {
        sample->current_ua = 2000;
    }
}

void controller_io_write(int frequency_x100)
{
    output_frequency = frequency_x100;
}

int controller_io_inject_wire_break(bool enabled)
{
    wire_break = enabled;
    return 0;
}

int controller_io_output(void)
{
    return output_frequency;
}
