#include "sensor_signal.h"
#include <zephyr/ztest.h>
#include <limits.h>

static struct signal_filter filter;

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    sensor_signal_reset(&filter);
}

ZTEST(signal_tests, test_pump_current_scaling)
{
    int inputs[] = {4000, 12000, 20000};
    int expected[] = {0, 500, 1000};
    for (size_t i = 0; i < ARRAY_SIZE(inputs); i++) {
        sensor_signal_reset(&filter);
        struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP,
            INPUT_4_20MA, inputs[i], -1);
        zassert_equal(v.faults, FAULT_NONE);
        zassert_equal(v.scaled_x10, expected[i]);
        zassert_equal(v.filtered_x10, expected[i]);
    }
}

ZTEST(signal_tests, test_ahu_scaling_in_both_modes)
{
    int currents[] = {4000, 12000, 20000};
    int voltages[] = {0, 5000, 10000};
    int expected[] = {-200, 100, 400};
    for (size_t i = 0; i < ARRAY_SIZE(currents); i++) {
        sensor_signal_reset(&filter);
        struct signal_value v = sensor_signal_process(&filter, PROFILE_AHU,
            INPUT_4_20MA, currents[i], -1);
        zassert_equal(v.filtered_x10, expected[i]);
        sensor_signal_reset(&filter);
        v = sensor_signal_process(&filter, PROFILE_AHU, INPUT_0_10V, -1, voltages[i]);
        zassert_equal(v.faults, FAULT_NONE);
        zassert_equal(v.filtered_x10, expected[i]);
    }
}

ZTEST(signal_tests, test_pump_voltage_scaling)
{
    struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP, INPUT_0_10V, -1, 2500);
    zassert_equal(v.scaled_x10, 250);
    zassert_equal(v.filtered_x10, 250);
}

ZTEST(signal_tests, test_current_tolerance_and_fault_boundaries)
{
    int inputs[] = {3599, 3600, 3999, 20001, 21000, 21001};
    uint32_t faults[] = {FAULT_WIRE_BREAK, 0, 0, 0, 0, FAULT_INPUT_RANGE};
    int expected[] = {0, 0, 0, 1000, 1000, 0};
    for (size_t i = 0; i < ARRAY_SIZE(inputs); i++) {
        sensor_signal_reset(&filter);
        struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP,
            INPUT_4_20MA, inputs[i], -1);
        zassert_equal(v.faults, faults[i]);
        zassert_equal(v.scaled_x10, expected[i]);
    }
}

ZTEST(signal_tests, test_voltage_bounds_and_extreme_values)
{
    int inputs[] = {INT_MIN, -1, 0, 10000, 10001, INT_MAX};
    for (size_t i = 0; i < ARRAY_SIZE(inputs); i++) {
        struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP,
            INPUT_0_10V, -1, inputs[i]);
        zassert_equal(v.faults, (inputs[i] < 0 || inputs[i] > 10000) ? FAULT_INPUT_RANGE : 0);
    }
    struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP,
        INPUT_4_20MA, INT_MAX, -1);
    zassert_equal(v.faults, FAULT_INPUT_RANGE);
    v = sensor_signal_process(&filter, PROFILE_PUMP, INPUT_4_20MA, INT_MIN, -1);
    zassert_equal(v.faults, FAULT_WIRE_BREAK);
}

ZTEST(signal_tests, test_moving_average_and_window_wrap)
{
    int voltages[] = {0, 1000, 2000, 3000, 4000, 5000};
    int expected[] = {0, 50, 100, 150, 250, 350};
    for (size_t i = 0; i < ARRAY_SIZE(voltages); i++) {
        struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP,
            INPUT_0_10V, -1, voltages[i]);
        zassert_equal(v.filtered_x10, expected[i]);
    }
}

ZTEST(signal_tests, test_negative_temperature_average)
{
    sensor_signal_process(&filter, PROFILE_AHU, INPUT_0_10V, -1, 0);
    struct signal_value v = sensor_signal_process(&filter, PROFILE_AHU, INPUT_0_10V, -1, 5000);
    zassert_equal(v.scaled_x10, 100);
    zassert_equal(v.filtered_x10, -50);
}

ZTEST(signal_tests, test_fault_is_not_filtered_and_recovery_starts_fresh)
{
    for (int i = 0; i < SIGNAL_FILTER_SIZE; i++) {
        sensor_signal_process(&filter, PROFILE_PUMP, INPUT_4_20MA, 20000, -1);
    }
    struct signal_value v = sensor_signal_process(&filter, PROFILE_PUMP, INPUT_4_20MA, 2000, -1);
    zassert_equal(v.faults, FAULT_WIRE_BREAK);
    zassert_equal(filter.count, 0);
    v = sensor_signal_process(&filter, PROFILE_PUMP, INPUT_4_20MA, 4000, -1);
    zassert_equal(v.filtered_x10, 0);
    zassert_equal(filter.count, 1);
}

ZTEST_SUITE(signal_tests, NULL, NULL, before, NULL, NULL);
