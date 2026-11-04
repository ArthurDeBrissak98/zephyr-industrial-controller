#include "protection.h"
#include <zephyr/ztest.h>

ZTEST(protection_tests, test_low_delay_boundary)
{
    struct protection_state state = {0};
    struct protection_config config = {100, 20, 500, true};
    zassert_false(protection_update(&state, &config, true, 100, 0));
    zassert_false(protection_update(&state, &config, true, 90, 499));
    zassert_true(protection_update(&state, &config, true, 90, 500));
}

ZTEST(protection_tests, test_short_spike_restarts_delay)
{
    struct protection_state state = {0};
    struct protection_config config = {100, 20, 500, true};
    zassert_false(protection_update(&state, &config, true, 90, 0));
    zassert_false(protection_update(&state, &config, true, 101, 400));
    zassert_false(protection_update(&state, &config, true, 90, 500));
    zassert_false(protection_update(&state, &config, true, 90, 999));
    zassert_true(protection_update(&state, &config, true, 90, 1000));
}

ZTEST(protection_tests, test_low_hysteresis)
{
    struct protection_state state = {0};
    struct protection_config config = {100, 20, 0, true};
    zassert_true(protection_update(&state, &config, true, 100, 0));
    zassert_true(protection_update(&state, &config, true, 101, 100));
    zassert_true(protection_update(&state, &config, true, 119, 200));
    zassert_false(protection_update(&state, &config, true, 120, 300));
    zassert_false(protection_update(&state, &config, true, 101, 400));
}

ZTEST(protection_tests, test_high_delay_and_hysteresis)
{
    struct protection_state state = {0};
    struct protection_config config = {900, 20, 500, true};
    zassert_false(protection_update(&state, &config, false, 900, 0));
    zassert_false(protection_update(&state, &config, false, 901, 499));
    zassert_true(protection_update(&state, &config, false, 901, 500));
    zassert_true(protection_update(&state, &config, false, 881, 600));
    zassert_false(protection_update(&state, &config, false, 880, 700));
}

ZTEST(protection_tests, test_zero_hysteresis_does_not_toggle_at_threshold)
{
    struct protection_state state = {0};
    struct protection_config config = {100, 0, 0, true};
    zassert_true(protection_update(&state, &config, true, 100, 0));
    zassert_true(protection_update(&state, &config, true, 100, 100));
    zassert_false(protection_update(&state, &config, true, 101, 200));
}

ZTEST(protection_tests, test_disabled_limit_clears_state)
{
    struct protection_state state = { .active = true, .pending = true };
    struct protection_config config = {100, 20, 0, false};
    zassert_false(protection_update(&state, &config, true, 0, 100));
    zassert_false(state.pending);
    zassert_false(state.active);
}

ZTEST(protection_tests, test_negative_temperature_threshold)
{
    struct protection_state state = {0};
    struct protection_config config = {-100, 20, 0, true};
    zassert_true(protection_update(&state, &config, true, -150, 0));
    zassert_true(protection_update(&state, &config, true, -81, 100));
    zassert_false(protection_update(&state, &config, true, -80, 200));
}

ZTEST_SUITE(protection_tests, NULL, NULL, NULL, NULL, NULL);
