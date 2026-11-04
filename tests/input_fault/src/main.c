#include "controller_app.h"
#include <zephyr/ztest.h>
#include <errno.h>

static void check_single_bad_sample(enum input_mode mode, int input, uint32_t fault)
{
    controller_app_init();
    struct controller_config config = { PROFILE_PUMP, mode };
    zassert_ok(controller_app_configure(config));

    struct controller_status status;
    controller_app_status(&status);
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        struct protection_config protection = status.protections[i];
        protection.delay_ms = 60000;
        zassert_ok(controller_app_protection_configure(i, protection));
    }
    int normal = mode == INPUT_4_20MA ? 12000 : 5000;
    zassert_ok(controller_app_inject_signal(true, normal));
    controller_app_tick();
    zassert_ok(controller_app_start());

    // Заполняем фильтр нормальными отсчётами и успеваем разогнать привод.
    for (int i = 0; i < 10; i++) controller_app_tick();
    controller_app_status(&status);
    zassert_equal(status.state, STATE_RUNNING);
    zassert_equal(status.sample.measured_x10, 500);
    zassert_true(status.output_x100 >= 500);

    zassert_ok(controller_app_inject_signal(true, input));
    controller_app_tick();
    controller_app_status(&status);
    zassert_equal(status.state, STATE_FAULT);
    zassert_equal(status.active_faults, fault);
    zassert_equal(status.latched_faults, fault);
    zassert_equal(status.frequency_x100, 0);
    zassert_equal(status.output_x100, 0);
    zassert_false(status.signal_valid);

    struct controller_event events[EVENT_CAPACITY];
    size_t count = controller_app_events(events, EVENT_CAPACITY);
    zassert_true(count > 0);
    const struct controller_event *event = &events[count - 1];
    zassert_equal(event->type, EVENT_FAULT);
    zassert_equal(event->faults, fault);
    zassert_equal(event->frequency_x100, 0);
    if (mode == INPUT_4_20MA) zassert_equal(event->current_ua, input);
    else zassert_equal(event->voltage_mv, input);

    // Даже один плохой отсчёт требует сброса после восстановления входа.
    zassert_equal(controller_app_reset(), -EBUSY);
    zassert_ok(controller_app_inject_signal(true, normal));
    controller_app_tick();
    controller_app_status(&status);
    zassert_equal(status.active_faults, FAULT_NONE);
    zassert_equal(status.latched_faults, fault);
    zassert_equal(status.state, STATE_FAULT);
    zassert_equal(status.output_x100, 0);
    zassert_equal(status.sample.measured_x10, 500);
    zassert_equal(controller_app_start(), -EACCES);
    zassert_ok(controller_app_reset());
    controller_app_status(&status);
    zassert_equal(status.state, STATE_READY);
    zassert_equal(status.output_x100, 0);
}

ZTEST(input_fault_tests, test_current_above_range)
{
    check_single_bad_sample(INPUT_4_20MA, 21001, FAULT_INPUT_RANGE);
}

ZTEST(input_fault_tests, test_current_below_wire_break_limit)
{
    check_single_bad_sample(INPUT_4_20MA, 3599, FAULT_WIRE_BREAK);
}

ZTEST(input_fault_tests, test_voltage_above_range)
{
    check_single_bad_sample(INPUT_0_10V, 10001, FAULT_INPUT_RANGE);
}

ZTEST(input_fault_tests, test_voltage_below_range)
{
    check_single_bad_sample(INPUT_0_10V, -1, FAULT_INPUT_RANGE);
}

ZTEST_SUITE(input_fault_tests, NULL, NULL, NULL, NULL, NULL);
