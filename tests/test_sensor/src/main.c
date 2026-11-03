#include <zephyr/ztest.h>
#include "sensor.h"

uint16_t holding_regs[5];

static void next_sample(struct sensor_data *sample)
{
    int ret;

    // При смене профиля очередь очищается, и чтение возвращает ENOMSG.
    do {
        ret = k_msgq_get(&sensor_msgq, sample, K_SECONDS(1));
    } while (ret == -ENOMSG);
    zassert_ok(ret);
}

static void check_wire_break(int profile, int mode)
{
    struct sensor_data sample;
    bool saw_wire_break = false;

    current_profile = profile;
    input_mode = mode;
    k_msgq_purge(&sensor_msgq);

    // Пропускаем отсчёты, которые могли остаться от прошлого режима.
    for (int i = 0; i < 2; i++) {
        next_sample(&sample);
    }

    // За 100 циклов обязательно попадём на имитацию обрыва.
    for (int i = 0; i < 110; i++) {
        next_sample(&sample);
        if (sample.faults & FAULT_WIRE_BREAK) {
            saw_wire_break = true;
            zassert_equal(mode, INPUT_4_20MA,
                          "Voltage input must not report a current-loop break");
            zassert_equal(sample.current_ua, 2000,
                          "Analog conversion overwrote the broken-loop signal");
            zassert_equal(sample.voltage_mv, -1);
        }
    }

    zassert_equal(saw_wire_break, mode == INPUT_4_20MA);
}

ZTEST(sensor_tests, test_pump_current_loop_break)
{
    check_wire_break(PROFILE_PUMP, INPUT_4_20MA);
}

ZTEST(sensor_tests, test_pump_voltage_has_no_current_loop_break)
{
    check_wire_break(PROFILE_PUMP, INPUT_0_10V);
}

ZTEST(sensor_tests, test_ahu_current_loop_break)
{
    check_wire_break(PROFILE_AHU, INPUT_4_20MA);
}

ZTEST(sensor_tests, test_ahu_voltage_has_no_current_loop_break)
{
    check_wire_break(PROFILE_AHU, INPUT_0_10V);
}

ZTEST_SUITE(sensor_tests, NULL, NULL, NULL, NULL, NULL);
