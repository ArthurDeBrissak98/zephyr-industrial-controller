#include "controller.h"
#include <zephyr/ztest.h>
#include <errno.h>
#include <limits.h>

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
    zassert_equal(ctrl.integral, 0);
    sample.setpoint_x10 = -5000;
    sample.measured_x10 = 5000;
    for (int i = 0; i < 60; i++) {
        int previous = ctrl.frequency_x100;
        controller_update(&ctrl, &sample, 6000 + i * 100);
        zassert_between_inclusive(ctrl.frequency_x100, previous - 100, previous);
        zassert_between_inclusive(ctrl.frequency_x100, 0, 5000);
    }
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.integral, 0);
}

ZTEST(controller_logic, test_voltage_mode_has_no_current_loop_fault)
{
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_AHU;
    config.mode = INPUT_0_10V;
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
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_AHU;
    config.mode = INPUT_0_10V;
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
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_AHU;
    config.mode = INPUT_0_10V;
    zassert_ok(controller_configure(&ctrl, config, 10));
    zassert_false(ctrl.sample_valid);
    struct sensor_sample sample = healthy;
    sample.voltage_mv = 0;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.sample.measured_x10, -200);
}

ZTEST(controller_logic, test_warning_then_trip_and_manual_reset)
{
    struct protection_config warning = {450, 20, 200, true};
    struct protection_config trip = {400, 20, 400, true};
    zassert_ok(controller_protection_configure(&ctrl, PROTECTION_LOW_WARNING, warning, 0));
    zassert_ok(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, trip, 0));
    struct sensor_sample sample = healthy;
    sample.current_ua = 10400;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, 199);
    zassert_equal(ctrl.active_warnings, 0);
    controller_update(&ctrl, &sample, 200);
    zassert_equal(ctrl.active_warnings, WARNING_LOW);
    zassert_equal(ctrl.state, STATE_RUNNING);
    zassert_true(ctrl.frequency_x100 > 0);
    controller_update(&ctrl, &sample, 399);
    zassert_equal(ctrl.active_faults, 0);
    controller_update(&ctrl, &sample, 400);
    zassert_equal(ctrl.active_faults, FAULT_PROCESS_LOW);
    zassert_equal(ctrl.latched_faults, FAULT_PROCESS_LOW);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(controller_reset(&ctrl, 401), -EBUSY);
    for (int i = 5; i <= 8; i++) controller_update(&ctrl, &healthy, i * 100);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.active_warnings, 0);
    zassert_equal(ctrl.state, STATE_FAULT);
    zassert_ok(controller_reset(&ctrl, 801));
    zassert_equal(ctrl.state, STATE_READY);
    zassert_equal(ctrl.frequency_x100, 0);

    int warnings = 0, cleared = 0, faults = 0;
    struct controller_event event;
    for (size_t i = 0; controller_event_get(&ctrl, i, &event); i++) {
        if (event.type == EVENT_WARNING) {
            warnings++;
            zassert_equal(event.warnings, WARNING_LOW);
            zassert_equal(event.timestamp_ms, 200);
        }
        if (event.type == EVENT_WARNING_CLEARED) cleared++;
        if (event.type == EVENT_FAULT) faults++;
    }
    zassert_equal(warnings, 1);
    zassert_equal(cleared, 1);
    zassert_equal(faults, 1);
}

ZTEST(controller_logic, test_protection_settings_validation)
{
    struct protection_config config = {100, 20, -1, true};
    zassert_equal(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, config, 0), -EINVAL);
    config.delay_ms = 60001;
    zassert_equal(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, config, 0), -EINVAL);
    config = (struct protection_config) {990, 20, 100, true};
    zassert_equal(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, config, 0), -EINVAL);
    config = (struct protection_config) {100, -1, 100, true};
    zassert_equal(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, config, 0), -EINVAL);
    config = (struct protection_config) {100, 20, 100, true};
    controller_update(&ctrl, &healthy, 0);
    zassert_ok(controller_start(&ctrl, 0));
    zassert_equal(controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, config, 0), -EBUSY);
}

ZTEST(controller_logic, test_bad_input_interrupts_pending_protection)
{
    struct protection_config config = {450, 20, 500, true};
    zassert_ok(controller_protection_configure(&ctrl, PROTECTION_LOW_WARNING, config, 0));
    struct sensor_sample sample = healthy;
    sample.current_ua = 10400;
    controller_update(&ctrl, &sample, 0);
    sample.current_ua = 2000;
    controller_update(&ctrl, &sample, 400);
    sample.current_ua = 10400;
    controller_update(&ctrl, &sample, 500);
    controller_update(&ctrl, &sample, 999);
    zassert_equal(ctrl.active_warnings, 0);
    controller_update(&ctrl, &sample, 1000);
    zassert_equal(ctrl.active_warnings, WARNING_LOW);
}

ZTEST(controller_logic, test_active_trip_survives_sensor_fault_until_hysteresis_clear)
{
    struct protection_config config = {600, 50, 0, true};
    zassert_ok(controller_protection_configure(&ctrl, PROTECTION_HIGH_TRIP, config, 0));
    struct sensor_sample sample = healthy;
    sample.current_ua = 20000;
    controller_update(&ctrl, &sample, 0);
    zassert_equal(ctrl.active_faults, FAULT_PROCESS_HIGH);
    sample.current_ua = 2000;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.active_faults, FAULT_PROCESS_HIGH | FAULT_WIRE_BREAK);
    zassert_equal(controller_reset(&ctrl, 101), -EBUSY);
    sample.current_ua = 16000;
    controller_update(&ctrl, &sample, 200);
    zassert_equal(ctrl.active_faults, FAULT_PROCESS_HIGH);
    zassert_equal(controller_reset(&ctrl, 201), -EBUSY);
    for (int i = 3; i <= 6; i++) controller_update(&ctrl, &healthy, i * 100);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.latched_faults, FAULT_PROCESS_HIGH | FAULT_WIRE_BREAK);
    zassert_ok(controller_reset(&ctrl, 601));
}

ZTEST(controller_logic, test_profile_change_restores_protection_defaults)
{
    struct protection_config custom = {450, 20, 300, true};
    zassert_ok(controller_protection_configure(&ctrl, PROTECTION_LOW_WARNING, custom, 0));
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_PUMP;
    config.mode = INPUT_0_10V;
    zassert_ok(controller_configure(&ctrl, config, 0));
    zassert_equal(ctrl.protections[PROTECTION_LOW_WARNING].threshold_x10, 450);
    config.profile = PROFILE_AHU;
    zassert_ok(controller_configure(&ctrl, config, 0));
    zassert_equal(ctrl.protections[PROTECTION_LOW_WARNING].threshold_x10, 50);
    zassert_equal(ctrl.protections[PROTECTION_LOW_WARNING].delay_ms, 500);
}

ZTEST(controller_logic, test_journal_keeps_input_context_after_configuration_change)
{
    struct sensor_sample broken = healthy;
    broken.current_ua = 2000;
    controller_update(&ctrl, &broken, 0);
    controller_update(&ctrl, &healthy, 100);
    zassert_ok(controller_reset(&ctrl, 101));
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_AHU;
    config.mode = INPUT_0_10V;
    zassert_ok(controller_configure(&ctrl, config, 102));
    struct controller_event event;
    zassert_true(controller_event_get(&ctrl, 0, &event));
    zassert_equal(event.type, EVENT_FAULT);
    zassert_equal(event.profile, PROFILE_PUMP);
    zassert_equal(event.mode, INPUT_4_20MA);
    zassert_false(event.signal_valid);
    zassert_equal(event.current_ua, 2000);
}

ZTEST(controller_logic, test_pi_integrates_elapsed_time)
{
    static struct controller fast;
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = 510;
    controller_init(&fast);
    controller_update(&ctrl, &sample, 0);
    controller_update(&fast, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    zassert_ok(controller_start(&fast, 0));
    ctrl.frequency_x100 = 2500;
    fast.frequency_x100 = 2500;
    for (int i = 1; i <= 10; i++) controller_update(&ctrl, &sample, i * 100);
    for (int i = 1; i <= 40; i++) controller_update(&fast, &sample, i * 25);
    zassert_equal(ctrl.integral, 10000);
    zassert_equal(fast.integral, ctrl.integral);
    zassert_equal(ctrl.frequency_x100, 2600);
    zassert_equal(fast.frequency_x100, ctrl.frequency_x100);
    zassert_equal(ctrl.last_update_ms, 1000);
}

ZTEST(controller_logic, test_pi_ignores_repeated_and_old_timestamps)
{
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = 501;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, 10);
    zassert_equal(ctrl.integral, 10);
    controller_update(&ctrl, &sample, 10);
    controller_update(&ctrl, &sample, 5);
    zassert_equal(ctrl.integral, 10);
    zassert_equal(ctrl.last_update_ms, 5);
    controller_update(&ctrl, &sample, 15);
    zassert_equal(ctrl.integral, 20);
}

ZTEST(controller_logic, test_pi_does_not_integrate_stopped_time)
{
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = 510;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, 100);
    controller_stop(&ctrl, 101);
    zassert_ok(controller_start(&ctrl, 10000));
    controller_update(&ctrl, &sample, 10100);
    zassert_equal(ctrl.integral, 1000);
}

ZTEST(controller_logic, test_pi_saturates_before_multiplication_overflows)
{
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = INT_MAX;
    ctrl.config.pid.kp = 0;
    ctrl.config.pid.ki = 0;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, INT64_MAX);
    zassert_equal(ctrl.integral, CONTROLLER_INTEGRAL_LIMIT);
    zassert_between_inclusive(ctrl.frequency_x100, 0, 5000);
    controller_init(&ctrl);
    sample.setpoint_x10 = INT_MIN;
    ctrl.config.pid.kp = 0;
    ctrl.config.pid.ki = 0;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, INT64_MAX);
    zassert_equal(ctrl.integral, -CONTROLLER_INTEGRAL_LIMIT);
    zassert_between_inclusive(ctrl.frequency_x100, 0, 5000);
}

K_SEM_DEFINE(api_entered, 0, 1);
K_SEM_DEFINE(api_finished, 0, 1);
K_THREAD_STACK_DEFINE(api_stack, 2048);
static struct k_thread api_thread;

static void call_api(void *operation, void *unused1, void *unused2)
{
    int op = (int)(intptr_t)operation;
    k_sem_give(&api_entered);
    struct controller_config config = ctrl.config;
    config.profile = PROFILE_PUMP;
    config.mode = INPUT_4_20MA;
    struct protection_config protection = {100, 20, 100, true};
    struct controller_event event;
    switch (op) {
    case 0: controller_update(&ctrl, &healthy, 100); break;
    case 1: controller_start(&ctrl, 100); break;
    case 2: controller_stop(&ctrl, 100); break;
    case 3: controller_reset(&ctrl, 100); break;
    case 4: controller_configure(&ctrl, config, 100); break;
    case 5: controller_protection_configure(&ctrl, PROTECTION_LOW_TRIP, protection, 100); break;
    case 6: controller_event_get(&ctrl, 0, &event); break;
    }
    k_sem_give(&api_finished);
}

ZTEST(controller_logic, test_public_api_waits_for_controller_mutex)
{
    for (int op = 0; op < 7; op++) {
        controller_init(&ctrl);
        controller_update(&ctrl, &healthy, 0);
        k_sem_reset(&api_entered);
        k_sem_reset(&api_finished);
        k_mutex_lock(&ctrl.mutex, K_FOREVER);
        k_thread_create(&api_thread, api_stack, K_THREAD_STACK_SIZEOF(api_stack),
                        call_api, (void *)(intptr_t)op, NULL, NULL, 5, 0, K_NO_WAIT);
        int entered = k_sem_take(&api_entered, K_SECONDS(1));
        int blocked = k_sem_take(&api_finished, K_MSEC(20));
        k_mutex_unlock(&ctrl.mutex);
        int finished = k_sem_take(&api_finished, K_SECONDS(1));
        int joined = k_thread_join(&api_thread, K_SECONDS(1));
        if (joined) k_thread_abort(&api_thread);
        zassert_ok(entered);
        zassert_equal(blocked, -EAGAIN, "API completed while another thread held the mutex");
        zassert_ok(finished);
        zassert_ok(joined);
        int unlocked = k_mutex_lock(&ctrl.mutex, K_NO_WAIT);
        if (!unlocked) k_mutex_unlock(&ctrl.mutex);
        zassert_ok(unlocked, "API did not release the mutex");
    }
}

ZTEST(controller_logic, test_pid_defaults_and_custom_coefficients)
{
    zassert_equal(ctrl.config.pid.bias_x100, 2500);
    zassert_equal(ctrl.config.pid.kp, 50);
    zassert_equal(ctrl.config.pid.ki, 5);
    zassert_equal(ctrl.config.pid.divisor, 10);
    struct controller_config config = ctrl.config;
    config.pid = (struct pid_config) {1000, 2, 1, 2};
    zassert_ok(controller_configure(&ctrl, config, 0));
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = 510;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    ctrl.frequency_x100 = 1000;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.integral, 1000);
    zassert_equal(ctrl.frequency_x100, 1015);
}

ZTEST(controller_logic, test_pid_configuration_validation)
{
    for (int invalid = 0; invalid < 4; invalid++) {
        struct controller_config config = ctrl.config;
        if (invalid == 0) config.pid.divisor = 0;
        if (invalid == 1) config.pid.kp = -1;
        if (invalid == 2) config.pid.ki = -1;
        if (invalid == 3) config.pid.bias_x100 = 5001;
        zassert_equal(controller_configure(&ctrl, config, 0), -EINVAL);
        zassert_equal(ctrl.config.pid.divisor, 10);
        zassert_equal(ctrl.event_count, 0);
    }
}

ZTEST(controller_logic, test_conditional_integration_at_both_limits)
{
    struct sensor_sample sample = healthy;
    ctrl.config.pid.kp = 0;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    ctrl.integral = 1000000;
    sample.setpoint_x10 = 510;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.integral, 1000000);
    sample.setpoint_x10 = 490;
    controller_update(&ctrl, &sample, 200);
    zassert_equal(ctrl.integral, 999000);
    ctrl.integral = -1000000;
    controller_update(&ctrl, &sample, 300);
    zassert_equal(ctrl.integral, -1000000);
    sample.setpoint_x10 = 510;
    controller_update(&ctrl, &sample, 400);
    zassert_equal(ctrl.integral, -999000);
}

ZTEST(controller_logic, test_conditional_integration_at_exact_output_boundary)
{
    struct sensor_sample sample = healthy;
    ctrl.config.pid.ki = 0;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    sample.setpoint_x10 = 1000;
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.integral, 0);
    sample.setpoint_x10 = 0;
    controller_update(&ctrl, &sample, 200);
    zassert_equal(ctrl.integral, 0);
}

ZTEST(controller_logic, test_large_pid_coefficients_do_not_overflow)
{
    struct controller_config config = ctrl.config;
    config.pid.kp = INT_MAX;
    config.pid.ki = INT_MAX;
    config.pid.divisor = 1;
    zassert_ok(controller_configure(&ctrl, config, 0));
    struct sensor_sample sample = healthy;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    sample.setpoint_x10 = INT_MIN;
    controller_update(&ctrl, &sample, INT64_MAX);
    zassert_equal(ctrl.integral, 0);
    zassert_equal(ctrl.frequency_x100, 0);
}

ZTEST(controller_logic, test_crossed_or_equal_thresholds_are_rejected)
{
    enum protection_id ids[] = { PROTECTION_LOW_TRIP, PROTECTION_HIGH_TRIP,
                                PROTECTION_LOW_WARNING, PROTECTION_HIGH_WARNING };
    int equal[] = {950, 50, 900, 100};
    for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
        int old = ctrl.protections[ids[i]].threshold_x10;
        for (int crossing = 0; crossing <= 1; crossing++) {
            bool low = ids[i] == PROTECTION_LOW_TRIP || ids[i] == PROTECTION_LOW_WARNING;
            struct protection_config config = {equal[i] + (low ? crossing : -crossing), 0, 100, true};
            zassert_equal(controller_protection_configure(&ctrl, ids[i], config, 0), -EINVAL);
            zassert_equal(ctrl.protections[ids[i]].threshold_x10, old);
            zassert_equal(ctrl.event_count, 0);
        }
    }
}

ZTEST(controller_logic, test_last_update_is_saved_on_early_return)
{
    controller_update(&ctrl, &healthy, 100);
    zassert_equal(ctrl.last_update_ms, 100);
    struct sensor_sample broken = healthy;
    broken.current_ua = 2000;
    controller_update(&ctrl, &broken, 200);
    zassert_equal(ctrl.last_update_ms, 200);
    controller_update(&ctrl, &broken, 150);
    zassert_equal(ctrl.last_update_ms, 150);
    zassert_equal(ctrl.integral, 0);
}

ZTEST(controller_logic, test_dt_rebases_after_negative_time_and_handles_extreme_gap)
{
    struct sensor_sample sample = healthy;
    sample.setpoint_x10 = 501;
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    controller_update(&ctrl, &sample, -10);
    zassert_equal(ctrl.integral, 0);
    zassert_equal(ctrl.last_update_ms, -10);
    controller_update(&ctrl, &sample, 0);
    zassert_equal(ctrl.integral, 10);
    ctrl.config.pid.kp = 0;
    ctrl.config.pid.ki = 0;
    controller_update(&ctrl, &sample, INT64_MIN);
    controller_update(&ctrl, &sample, INT64_MAX);
    zassert_equal(ctrl.integral, CONTROLLER_INTEGRAL_LIMIT);
    zassert_equal(ctrl.last_update_ms, INT64_MAX);
}

ZTEST_SUITE(controller_logic, NULL, NULL, before, NULL, NULL);
