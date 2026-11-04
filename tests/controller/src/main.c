#include "controller.h"
#include <zephyr/ztest.h>
#include <errno.h>

static struct controller ctrl;
static const struct sensor_sample healthy = {
    .current_ua = 12000, .voltage_mv = -1,
    .measured_x10 = 500, .setpoint_x10 = 500,
};

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    controller_init(&ctrl);
}

ZTEST(controller_logic, test_boot_requires_sample_and_start)
{
    zassert_equal(ctrl.state, STATE_STOPPED);
    zassert_equal(controller_start(&ctrl, 0), -EACCES);
    controller_update(&ctrl, &healthy, 100);
    zassert_equal(ctrl.state, STATE_READY);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_ok(controller_start(&ctrl, 110));
    controller_update(&ctrl, &healthy, 200);
    zassert_equal(ctrl.frequency_x100, 100);
}

ZTEST(controller_logic, test_fault_latches_until_reset_and_separate_start)
{
    struct sensor_sample broken = healthy;
    broken.current_ua = 2000;
    controller_update(&ctrl, &healthy, 0);
    zassert_ok(controller_start(&ctrl, 0));
    for (int i = 1; i <= 30; i++) controller_update(&ctrl, &healthy, i * 100);
    zassert_equal(ctrl.frequency_x100, 2500);
    controller_update(&ctrl, &broken, 3100);
    zassert_equal(ctrl.state, STATE_FAULT);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.latched_faults, FAULT_WIRE_BREAK);
    zassert_equal(controller_reset(&ctrl, 3101), -EBUSY);
    controller_stop(&ctrl, 3102);
    zassert_equal(ctrl.state, STATE_FAULT);
    controller_update(&ctrl, &healthy, 3200);
    zassert_equal(ctrl.active_faults, FAULT_NONE);
    zassert_equal(ctrl.latched_faults, FAULT_WIRE_BREAK);
    zassert_equal(controller_start(&ctrl, 3201), -EACCES);
    zassert_ok(controller_reset(&ctrl, 3202));
    controller_update(&ctrl, &healthy, 3300);
    zassert_equal(ctrl.state, STATE_READY);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_ok(controller_start(&ctrl, 3301));
    controller_update(&ctrl, &healthy, 3400);
    zassert_equal(ctrl.frequency_x100, 100);
}

ZTEST(controller_logic, test_journal_records_reason_signal_time_and_stop)
{
    struct sensor_sample broken = healthy;
    broken.current_ua = 2000;
    controller_update(&ctrl, &healthy, 100);
    zassert_ok(controller_start(&ctrl, 120));
    controller_update(&ctrl, &broken, 200);
    struct controller_event event;
    zassert_true(controller_event_get(&ctrl, 2, &event));
    zassert_equal(event.type, EVENT_FAULT);
    zassert_equal(event.state, STATE_FAULT);
    zassert_equal(event.timestamp_ms, 200);
    zassert_equal(event.faults, FAULT_WIRE_BREAK);
    zassert_equal(event.current_ua, 2000);
    zassert_equal(event.frequency_x100, 0);
    controller_update(&ctrl, &broken, 300);
    zassert_equal(ctrl.event_count, 3, "Одна авария не должна заполнять весь журнал");
}

ZTEST(controller_logic, test_wire_break_threshold)
{
    struct sensor_sample sample = healthy;
    sample.current_ua = 3600;
    controller_update(&ctrl, &sample, 0);
    zassert_equal(ctrl.active_faults, FAULT_NONE);
    sample.current_ua = 3599;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.active_faults, FAULT_WIRE_BREAK);
}

ZTEST(controller_logic, test_frequency_limits_and_ramp)
{
    struct sensor_sample sample = healthy;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    sample.setpoint_x10 = 5000;
    sample.measured_x10 = 0;
    for (int i = 0; i < 60; i++) {
        int previous = ctrl.frequency_x100;
        controller_update(&ctrl, &sample, i * 100);
        zassert_between_inclusive(ctrl.frequency_x100, previous, previous + 100);
        zassert_between_inclusive(ctrl.frequency_x100, 0, 5000);
    }
    zassert_equal(ctrl.frequency_x100, 5000);
    zassert_equal(ctrl.integral, 100000);
    sample.setpoint_x10 = -5000;
    sample.measured_x10 = 5000;
    for (int i = 0; i < 60; i++) {
        int previous = ctrl.frequency_x100;
        controller_update(&ctrl, &sample, 6000 + i * 100);
        zassert_between_inclusive(ctrl.frequency_x100, previous - 100, previous);
        zassert_between_inclusive(ctrl.frequency_x100, 0, 5000);
    }
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.integral, -100000);
}

ZTEST(controller_logic, test_voltage_mode_has_no_current_loop_fault)
{
    struct controller_config config = { PROFILE_AHU, INPUT_0_10V };
    zassert_ok(controller_configure(&ctrl, config, 0));
    struct sensor_sample sample = healthy;
    sample.current_ua = -1;
    sample.voltage_mv = 5000;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.state, STATE_READY);
    zassert_equal(ctrl.active_faults, FAULT_NONE);
}

ZTEST(controller_logic, test_configuration_cannot_bypass_fault_or_running_state)
{
    struct controller_config config = { PROFILE_AHU, INPUT_0_10V };
    controller_update(&ctrl, &healthy, 0);
    zassert_ok(controller_start(&ctrl, 0));
    zassert_equal(controller_configure(&ctrl, config, 1), -EBUSY);
    struct sensor_sample broken = healthy;
    broken.current_ua = 2000;
    controller_update(&ctrl, &broken, 100);
    zassert_equal(controller_configure(&ctrl, config, 101), -EBUSY);
}

ZTEST(controller_logic, test_stop_and_new_start_keep_soft_start)
{
    controller_update(&ctrl, &healthy, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &healthy, 100);
    controller_stop(&ctrl, 101);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.state, STATE_READY);
    zassert_ok(controller_start(&ctrl, 102));
    controller_update(&ctrl, &healthy, 200);
    zassert_equal(ctrl.frequency_x100, 100);
}

ZTEST(controller_logic, test_journal_wrap_preserves_order)
{
    controller_update(&ctrl, &healthy, 0);
    for (int i = 0; i < EVENT_CAPACITY; i++) {
        zassert_ok(controller_start(&ctrl, i * 2));
        controller_stop(&ctrl, i * 2 + 1);
    }
    zassert_equal(ctrl.event_count, EVENT_CAPACITY);
    struct controller_event previous, next;
    zassert_true(controller_event_get(&ctrl, 0, &previous));
    zassert_equal(previous.sequence, ctrl.event_sequence - EVENT_CAPACITY + 1);
    for (int i = 1; i < EVENT_CAPACITY; i++) {
        zassert_true(controller_event_get(&ctrl, i, &next));
        zassert_equal(next.sequence, previous.sequence + 1);
        previous = next;
    }
    zassert_false(controller_event_get(&ctrl, EVENT_CAPACITY, &next));
}

ZTEST(controller_logic, test_regulator_uses_scaled_and_filtered_signal)
{
    struct sensor_sample sample = healthy;
    sample.measured_x10 = -9999;
    controller_update(&ctrl, &sample, 0);
    zassert_equal(ctrl.sample.measured_x10, 500);
    sample.current_ua = 20000;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.sample.scaled_x10, 1000);
    zassert_equal(ctrl.sample.measured_x10, 750);
}

ZTEST(controller_logic, test_configuration_resets_filter)
{
    controller_update(&ctrl, &healthy, 0);
    struct controller_config config = { PROFILE_AHU, INPUT_0_10V };
    zassert_ok(controller_configure(&ctrl, config, 10));
    zassert_false(ctrl.sample_valid);
    struct sensor_sample sample = healthy;
    sample.voltage_mv = 0;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.sample.measured_x10, -200);
}

ZTEST_SUITE(controller_logic, NULL, NULL, before, NULL, NULL);
