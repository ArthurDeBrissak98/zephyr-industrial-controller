#include "controller_io.h"
#include <errno.h>

static bool wire_break;
static bool signal_override;
static int signal_value;
static int output_frequency;
static bool motor_override[2];
static int motor_value[2];

int controller_io_read(const struct controller_config *config, struct sensor_sample *sample)
{
    // Небольшое отклонение от уставки зависит от команды приводу.
    int deviation = output_frequency > 0 ? (output_frequency - 2500) / 500 : 0;
    int measured = (config->profile == PROFILE_PUMP ? 500 : 200) + deviation;
    int percent_x10 = config->profile == PROFILE_PUMP ? measured : (measured + 200) * 1000 / 600;

    *sample = (struct sensor_sample) {
        .motor = {
            .current_ma = output_frequency > 0 ? config->motor.nominal_current_ma / 2 : 0,
            .frequency_x100 = output_frequency,
            .valid = true, .available = true,
        },
        .setpoint_x10 = config->profile == PROFILE_PUMP ? 500 : 200,
        .current_ua = config->mode == INPUT_4_20MA ? 4000 + percent_x10 * 16 : -1,
        .voltage_mv = config->mode == INPUT_0_10V ? percent_x10 * 10 : -1,
    };
    if (signal_override) {
        if (config->mode == INPUT_4_20MA) sample->current_ua = signal_value;
        else sample->voltage_mv = signal_value;
    }
    if (wire_break && config->mode == INPUT_4_20MA) {
        sample->current_ua = 2000;
    }
    if (motor_override[MOTOR_FEEDBACK_CURRENT]) sample->motor.current_ma = motor_value[MOTOR_FEEDBACK_CURRENT];
    if (motor_override[MOTOR_FEEDBACK_FREQUENCY]) sample->motor.frequency_x100 = motor_value[MOTOR_FEEDBACK_FREQUENCY];
    return 0;
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

int controller_io_inject_signal(bool enabled, int value)
{
    signal_override = enabled;
    signal_value = value;
    return 0;
}

int controller_io_inject_motor(enum motor_feedback_channel channel, bool enabled, int value)
{
    if ((unsigned int)channel >= 2 || (enabled && value < 0)) return -EINVAL;
    motor_override[channel] = enabled;
    motor_value[channel] = value;
    return 0;
}
