#include "controller.h"
#include <zephyr/ztest.h>
#include <errno.h>

static struct controller ctrl;
static struct sensor_sample sample;

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    controller_init(&ctrl);
    sample = (struct sensor_sample) {
        .current_ua = 12000, .setpoint_x10 = 500,
        .motor = { .current_ma = 5750, .frequency_x100 = 2500, .valid = true, .available = true },
    };
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
}

ZTEST(motor_protections, test_defaults_derive_from_motor_ratings)
{
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_WARNING].threshold, 12075);
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_TRIP].threshold, 12650);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_WARNING].threshold, 5250);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_TRIP].threshold, 5500);
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_WARNING].hysteresis, 230);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_TRIP].hysteresis, 100);
    // Прежний технологический порог 90% не является током двигателя.
    zassert_equal(ctrl.protections[PROTECTION_HIGH_WARNING].threshold_x10, 900);
}

ZTEST(motor_protections, test_current_warning_delay_hysteresis_and_no_stop)
{
    sample.motor.current_ma = 12074;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 2000);
    zassert_equal(ctrl.active_warnings, 0);
    sample.motor.current_ma = 12075;
    controller_update(&ctrl, &sample, 2100);
    controller_update(&ctrl, &sample, 2599);
    zassert_equal(ctrl.active_warnings, 0);
    controller_update(&ctrl, &sample, 2600);
    zassert_equal(ctrl.active_warnings, WARNING_MOTOR_CURRENT);
    zassert_equal(ctrl.state, STATE_RUNNING);
    sample.motor.current_ma = 11846;
    controller_update(&ctrl, &sample, 2700);
    zassert_equal(ctrl.active_warnings, WARNING_MOTOR_CURRENT);
    sample.motor.current_ma = 11845;
    controller_update(&ctrl, &sample, 2800);
    zassert_equal(ctrl.active_warnings, 0);
}

ZTEST(motor_protections, test_current_trip_latches_and_requires_separate_start)
{
    sample.motor.current_ma = 12650;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 1099);
    zassert_equal(ctrl.state, STATE_RUNNING);
    controller_update(&ctrl, &sample, 1100);
    zassert_equal(ctrl.state, STATE_FAULT);
    zassert_equal(ctrl.process_faults, FAULT_MOTOR_OVERCURRENT);
    zassert_equal(ctrl.sensor_faults, 0);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(controller_reset(&ctrl, 1101), -EBUSY);
    sample.motor.current_ma = 12420; // 110% минус гистерезис 2%.
    controller_update(&ctrl, &sample, 1200);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.latched_faults, FAULT_MOTOR_OVERCURRENT);
    zassert_ok(controller_reset(&ctrl, 1201));
    zassert_equal(ctrl.state, STATE_READY);
    zassert_equal(ctrl.frequency_x100, 0);
    struct controller_event event;
    bool found = false;
    for (size_t i = 0; controller_event_get(&ctrl, i, &event); i++) {
        if (event.type == EVENT_FAULT) {
            zassert_equal(event.motor.current_ma, 12650);
            zassert_equal(event.motor_nominal_current_ma, 11500);
            found = true;
        }
    }
    zassert_true(found);
}

ZTEST(motor_protections, test_actual_frequency_warning_and_trip)
{
    // Частота обратной связи не совпадает с командой frequency_x100.
    ctrl.frequency_x100 = 100;
    sample.motor.frequency_x100 = 5250;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 600);
    zassert_equal(ctrl.active_warnings, WARNING_MOTOR_FREQUENCY);
    zassert_equal(ctrl.state, STATE_RUNNING);
    sample.motor.frequency_x100 = 5500;
    controller_update(&ctrl, &sample, 700);
    controller_update(&ctrl, &sample, 1700);
    zassert_equal(ctrl.active_faults, FAULT_MOTOR_OVERFREQUENCY);
    zassert_equal(ctrl.frequency_x100, 0);
    sample.motor.frequency_x100 = 5401;
    controller_update(&ctrl, &sample, 1800);
    zassert_equal(ctrl.active_faults, FAULT_MOTOR_OVERFREQUENCY);
    sample.motor.frequency_x100 = 5400;
    controller_update(&ctrl, &sample, 1900);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.latched_faults, FAULT_MOTOR_OVERFREQUENCY);
}

ZTEST(motor_protections, test_short_peak_does_not_accumulate_delay)
{
    sample.motor.current_ma = 12650;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 900);
    sample.motor.current_ma = 11000;
    controller_update(&ctrl, &sample, 950);
    sample.motor.current_ma = 12650;
    controller_update(&ctrl, &sample, 1000);
    controller_update(&ctrl, &sample, 1999);
    zassert_equal(ctrl.state, STATE_RUNNING);
    controller_update(&ctrl, &sample, 2000);
    zassert_equal(ctrl.active_faults, FAULT_MOTOR_OVERCURRENT);
}

ZTEST(motor_protections, test_motor_configuration_recomputes_limits)
{
    struct motor_parameters motor = ctrl.config.motor;
    motor.nominal_current_ma = 15000;
    motor.nominal_frequency_x100 = 6000;
    zassert_equal(controller_motor_configure(&ctrl, motor, 1), -EBUSY);
    controller_stop(&ctrl, 2);
    zassert_ok(controller_motor_configure(&ctrl, motor, 3));
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_WARNING].threshold, 15750);
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_TRIP].threshold, 16500);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_WARNING].threshold, 6300);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_TRIP].threshold, 6600);
    zassert_equal(ctrl.protections[PROTECTION_HIGH_WARNING].threshold_x10, 900);
    zassert_equal(ctrl.state, STATE_READY);
}

ZTEST(motor_protections, test_threshold_arithmetic_exceeds_uint16_without_overflow)
{
    controller_stop(&ctrl, 0);
    struct motor_parameters motor = ctrl.config.motor;
    motor.nominal_current_ma = 65535;
    motor.nominal_frequency_x100 = 50000;
    zassert_ok(controller_motor_configure(&ctrl, motor, 1));
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_TRIP].threshold, 72089);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_TRIP].threshold, 55000);
}

ZTEST(motor_protections, test_motor_and_process_faults_remain_independent)
{
    ctrl.protections[PROTECTION_HIGH_TRIP] = (struct protection_config) {600, 50, 0, true};
    sample.current_ua = 20000;
    sample.motor.current_ma = 12650;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 1100);
    zassert_equal(ctrl.active_faults, FAULT_PROCESS_HIGH | FAULT_MOTOR_OVERCURRENT);
    sample.current_ua = 2000;
    controller_update(&ctrl, &sample, 1200);
    zassert_equal(ctrl.sensor_faults, FAULT_WIRE_BREAK);
    zassert_equal(ctrl.process_faults, FAULT_PROCESS_HIGH | FAULT_MOTOR_OVERCURRENT);
}

ZTEST(motor_protections, test_lost_feedback_stops_instead_of_disabling_protection)
{
    sample.motor.current_ma = 12650;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 1100);
    sample.motor.valid = false;
    controller_update(&ctrl, &sample, 1200);
    zassert_equal(ctrl.sensor_faults, FAULT_MOTOR_FEEDBACK);
    zassert_equal(ctrl.process_faults, FAULT_MOTOR_OVERCURRENT);
    zassert_equal(controller_reset(&ctrl, 1201), -EBUSY);
    sample.motor = (struct motor_feedback) {.current_ma = 0, .frequency_x100 = 0, .valid = true, .available = true};
    controller_update(&ctrl, &sample, 1300);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.latched_faults, FAULT_MOTOR_OVERCURRENT | FAULT_MOTOR_FEEDBACK);
    zassert_ok(controller_reset(&ctrl, 1301));
}

ZTEST(motor_protections, test_both_motor_faults_and_warnings_are_reported)
{
    sample.motor.current_ma = 12650;
    sample.motor.frequency_x100 = 5500;
    controller_update(&ctrl, &sample, 100);
    controller_update(&ctrl, &sample, 1100);
    zassert_equal(ctrl.active_faults, FAULT_MOTOR_OVERCURRENT | FAULT_MOTOR_OVERFREQUENCY);
    zassert_equal(ctrl.active_warnings, WARNING_MOTOR_CURRENT | WARNING_MOTOR_FREQUENCY);
    zassert_equal(ctrl.frequency_x100, 0);
}

ZTEST(motor_protections, test_negative_feedback_is_invalid)
{
    sample.motor.frequency_x100 = -1;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.sensor_faults, FAULT_MOTOR_FEEDBACK);
    zassert_false(ctrl.motor_feedback_valid);
    zassert_equal(ctrl.state, STATE_FAULT);
}

ZTEST(motor_protections, test_first_invalid_sample_of_available_feedback_is_fault)
{
    controller_init(&ctrl);
    sample.motor.valid = false;
    sample.motor.available = true;
    controller_update(&ctrl, &sample, 0);
    zassert_equal(ctrl.sensor_faults, FAULT_MOTOR_FEEDBACK);
    zassert_equal(controller_start(&ctrl, 1), -EACCES);
}

ZTEST_SUITE(motor_protections, NULL, NULL, before, NULL, NULL);
