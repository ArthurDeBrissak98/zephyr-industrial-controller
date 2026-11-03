#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include "sensor.h"

// --- ЗАГЛУШКИ ДЛЯ ТЕСТОВ ---
// 1. Очереди, которые использует control.c
K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_data), 10, 4);
K_MSGQ_DEFINE(monitor_msgq, sizeof(struct sensor_data), 10, 4);
K_MSGQ_DEFINE(modbus_tx_msgq, sizeof(int), 10, 4);

// 2. Глобальные переменные профиля и режима
volatile int current_profile = PROFILE_PUMP;
volatile int input_mode = INPUT_4_20MA;

// Объявляем функцию из control.c (структура pid_state уже известна из sensor.h)
int calculate_pid(struct pid_state *state, int setpoint, int measured, int max_step);

ZTEST(pid_tests, test_pid_basic_calculation)
{
    struct pid_state state = { .integral = 0, .prev_error = 0, .current_freq = 2500 };
    
    int setpoint = 600;
    int measured = 500;
    int max_step = 100;

    int result = calculate_pid(&state, setpoint, measured, max_step);

    // Ожидаем 2500 + 100 (шаг) = 2600
    zassert_equal(result, 2600, "PID calculation failed, got %d", result);
    zassert_equal(state.current_freq, 2600, "State freq not updated");
}

ZTEST(pid_tests, test_pid_limits)
{
    struct pid_state state = { .integral = 0, .prev_error = 0, .current_freq = 2500 };
    
    int result = calculate_pid(&state, 5000, 0, 100);

    zassert_equal(result, 2600, "PID should be limited by max_step");

    for (int i = 0; i < 30; i++) {
        result = calculate_pid(&state, 5000, 0, 100);
    }

    zassert_equal(result, 5000, "PID exceeded max frequency limit");
}

static int control_sample(uint32_t faults, bool changed)
{
    struct sensor_data sample = {
        .setpoint_x10 = 500,
        .level_percent_x10 = 500,
        .faults = faults,
        .profile_changed = changed,
    };
    int command;

    zassert_ok(k_msgq_put(&sensor_msgq, &sample, K_SECONDS(1)));
    zassert_ok(k_msgq_get(&modbus_tx_msgq, &command, K_SECONDS(1)));
    return command;
}

ZTEST(pid_tests, test_mode_change_after_fault_preserves_soft_start)
{
    current_profile = PROFILE_PUMP;
    zassert_equal(control_sample(FAULT_WIRE_BREAK, false), 0);
    zassert_equal(control_sample(FAULT_NONE, true), 100,
                  "Changing mode must not jump from stop to 25 Hz");
}

ZTEST(pid_tests, test_profile_change_while_running_preserves_frequency)
{
    current_profile = PROFILE_PUMP;
    zassert_equal(control_sample(FAULT_OVERLOAD, false), 0);
    for (int step = 1; step <= 10; step++) {
        zassert_equal(control_sample(FAULT_NONE, false), step * 100);
    }
    zassert_equal(control_sample(FAULT_NONE, true), 1100,
                  "Resetting PID history must preserve the current frequency");
}

ZTEST_SUITE(pid_tests, NULL, NULL, NULL, NULL, NULL);
