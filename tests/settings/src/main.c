#include "controller_settings.h"
#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <errno.h>

static struct controller ctrl;
static struct controller_parameters parameters;
static uint8_t data[CONTROLLER_SETTINGS_SIZE];

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    controller_init(&ctrl);
    controller_parameters_get(&ctrl, &parameters);
    zassert_ok(controller_settings_encode(&parameters, data));
}

static void update_crc(void)
{
    sys_put_le32(crc32_ieee(data, sizeof(data) - 4), data + sizeof(data) - 4);
}

ZTEST(settings_tests, test_roundtrip_preserves_all_parameters)
{
    parameters.config.profile = PROFILE_AHU;
    parameters.config.mode = INPUT_0_10V;
    parameters.config.pid = (struct pid_config) {1200, 20, 2, 4};
    parameters.config.motor = (struct motor_parameters) {230, 15000, 6000, 1750, 2, MOTOR_PMSM};
    parameters.protections[PROTECTION_LOW_WARNING] = (struct protection_config) {-50, 10, 700, true};
    parameters.protections[PROTECTION_HIGH_WARNING] = (struct protection_config) {300, 10, 900, false};
    parameters.protections[PROTECTION_LOW_TRIP] = (struct protection_config) {-100, 10, 1000, true};
    parameters.protections[PROTECTION_HIGH_TRIP] = (struct protection_config) {390, 10, 2000, true};
    zassert_ok(controller_settings_encode(&parameters, data));
    struct controller_parameters decoded;
    zassert_ok(controller_settings_decode(data, sizeof(data), &decoded));
    zassert_equal(decoded.config.profile, PROFILE_AHU);
    zassert_equal(decoded.config.mode, INPUT_0_10V);
    zassert_equal(decoded.config.pid.bias_x100, 1200);
    zassert_equal(decoded.config.pid.kp, 20);
    zassert_equal(decoded.config.pid.ki, 2);
    zassert_equal(decoded.config.pid.divisor, 4);
    zassert_mem_equal(&decoded.config.motor, &parameters.config.motor, sizeof(parameters.config.motor));
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        zassert_equal(decoded.protections[i].threshold_x10, parameters.protections[i].threshold_x10);
        zassert_equal(decoded.protections[i].hysteresis_x10, parameters.protections[i].hysteresis_x10);
        zassert_equal(decoded.protections[i].delay_ms, parameters.protections[i].delay_ms);
        zassert_equal(decoded.protections[i].enabled, parameters.protections[i].enabled);
    }
}

ZTEST(settings_tests, test_corrupted_record_does_not_change_output)
{
    struct controller_parameters decoded = parameters;
    data[20] ^= 1;
    zassert_equal(controller_settings_decode(data, sizeof(data), &decoded), -EBADMSG);
    zassert_equal(decoded.config.pid.kp, parameters.config.pid.kp);
    zassert_equal(decoded.config.profile, parameters.config.profile);
}

ZTEST(settings_tests, test_unknown_version_and_wrong_length)
{
    zassert_equal(controller_settings_decode(data, sizeof(data) - 1, &parameters), -EINVAL);
    sys_put_le32(99, data + 4);
    update_crc();
    zassert_equal(controller_settings_decode(data, sizeof(data), &parameters), -ENOTSUP);
}

ZTEST(settings_tests, test_invalid_pid_even_with_valid_crc)
{
    sys_put_le32(0, data + 28);
    update_crc();
    zassert_equal(controller_settings_decode(data, sizeof(data), &parameters), -EINVAL);
}

ZTEST(settings_tests, test_invalid_enabled_field)
{
    sys_put_le32(2, data + 44);
    update_crc();
    zassert_equal(controller_settings_decode(data, sizeof(data), &parameters), -EINVAL);
}

ZTEST(settings_tests, test_invalid_parameters_are_not_saved_or_applied)
{
    parameters.config.profile = PROFILE_AHU;
    parameters.protections[PROTECTION_LOW_TRIP].threshold_x10 = 500;
    zassert_equal(controller_settings_encode(&parameters, data), -EINVAL);
    zassert_equal(controller_parameters_apply(&ctrl, &parameters, 10), -EINVAL);
    zassert_equal(ctrl.config.profile, PROFILE_PUMP);
    zassert_equal(ctrl.protections[PROTECTION_LOW_TRIP].threshold_x10, 50);
    zassert_equal(ctrl.event_count, 0);
}

ZTEST(settings_tests, test_settings_apply_is_atomic_and_keeps_drive_stopped)
{
    parameters.protections[PROTECTION_LOW_WARNING].threshold_x10 = 960;
    parameters.protections[PROTECTION_HIGH_WARNING].threshold_x10 = 980;
    parameters.protections[PROTECTION_LOW_WARNING].hysteresis_x10 = 5;
    parameters.protections[PROTECTION_HIGH_WARNING].hysteresis_x10 = 5;
    zassert_ok(controller_parameters_apply(&ctrl, &parameters, 100));
    zassert_equal(ctrl.protections[PROTECTION_LOW_WARNING].threshold_x10, 960);
    zassert_equal(ctrl.protections[PROTECTION_HIGH_WARNING].threshold_x10, 980);
    zassert_equal(ctrl.state, STATE_STOPPED);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.last_update_ms, 100);
}

ZTEST(settings_tests, test_load_is_rejected_while_running)
{
    const struct sensor_sample sample = { .current_ua = 12000, .setpoint_x10 = 500 };
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    parameters.config.pid.kp = 25;
    zassert_equal(controller_parameters_apply(&ctrl, &parameters, 1), -EBUSY);
    zassert_equal(ctrl.config.pid.kp, 50);
    zassert_equal(ctrl.state, STATE_RUNNING);
}

ZTEST(settings_tests, test_legacy_settings_add_default_motor_parameters)
{
    uint8_t legacy[CONTROLLER_SETTINGS_LEGACY_SIZE];
    memcpy(legacy, data, sizeof(legacy) - 4);
    sys_put_le32(1, legacy + 4);
    sys_put_le32(crc32_ieee(legacy, sizeof(legacy) - 4), legacy + sizeof(legacy) - 4);
    struct controller_parameters decoded;
    zassert_ok(controller_settings_decode(legacy, sizeof(legacy), &decoded));
    zassert_equal(decoded.config.motor.nominal_voltage_v, 400);
    zassert_equal(decoded.config.motor.nominal_current_ma, 11500);
    zassert_equal(decoded.config.motor.nominal_frequency_x100, 5000);
    zassert_equal(decoded.config.motor.nominal_speed_rpm, 1450);
    zassert_equal(decoded.config.motor.pole_pairs, 2);
    zassert_equal(decoded.config.pid.kp, parameters.config.pid.kp);
    zassert_ok(controller_settings_encode(&decoded, data));
    zassert_equal(sys_get_le32(data + 4), 3);
}

ZTEST(settings_tests, test_v2_settings_load_as_induction)
{
    uint8_t legacy[CONTROLLER_SETTINGS_V2_SIZE];
    memcpy(legacy, data, sizeof(legacy) - 4);
    sys_put_le32(2, legacy + 4);
    sys_put_le32(crc32_ieee(legacy, sizeof(legacy) - 4), legacy + sizeof(legacy) - 4);
    struct controller_parameters decoded;
    zassert_ok(controller_settings_decode(legacy, sizeof(legacy), &decoded));
    zassert_equal(decoded.config.motor.type, MOTOR_INDUCTION);
    zassert_equal(decoded.config.motor.nominal_frequency_x100, 5000);
}

ZTEST(settings_tests, test_invalid_motor_type_with_valid_crc_is_rejected)
{
    sys_put_le32(99, data + 116);
    update_crc();
    zassert_equal(controller_settings_decode(data, sizeof(data), &parameters), -EINVAL);
}

ZTEST(settings_tests, test_invalid_motor_with_valid_crc_is_rejected)
{
    sys_put_le32(0, data + 100); // Номинальный ток в записи v2.
    update_crc();
    zassert_equal(controller_settings_decode(data, sizeof(data), &parameters), -EINVAL);
    struct controller_parameters invalid = parameters;
    invalid.config.motor.nominal_speed_rpm = 1600;
    zassert_equal(controller_parameters_apply(&ctrl, &invalid, 100), -EINVAL);
    zassert_equal(ctrl.config.motor.nominal_speed_rpm, 1450);
}

ZTEST(settings_tests, test_settings_apply_recomputes_motor_limits)
{
    parameters.config.motor.nominal_current_ma = 15000;
    parameters.config.motor.nominal_frequency_x100 = 6000;
    parameters.protections[PROTECTION_HIGH_WARNING].threshold_x10 = 920;
    zassert_ok(controller_parameters_apply(&ctrl, &parameters, 100));
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_WARNING].threshold, 15750);
    zassert_equal(ctrl.motor_protections[MOTOR_CURRENT_TRIP].threshold, 16500);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_WARNING].threshold, 6300);
    zassert_equal(ctrl.motor_protections[MOTOR_FREQUENCY_TRIP].threshold, 6600);
    zassert_equal(ctrl.protections[PROTECTION_HIGH_WARNING].threshold_x10, 920);
}

ZTEST_SUITE(settings_tests, NULL, NULL, before, NULL, NULL);
