#include "controller_app.h"
#include "controller_settings.h"
#include "controller_modbus.h"
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <string.h>

static struct controller_parameters stored;
static bool present;
static int write_error;
static unsigned int writes;

int controller_settings_init(void) { return 0; }
int controller_settings_read(struct controller_parameters *parameters)
{
    if (!present) return -ENOENT;
    *parameters = stored;
    return 0;
}
int controller_settings_write(const struct controller_parameters *parameters)
{
    writes++;
    if (write_error) return write_error;
    stored = *parameters;
    present = true;
    return 0;
}
int controller_settings_erase(void) { present = false; return 0; }

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    present = false;
    write_error = 0;
    writes = 0;
    controller_app_init();
    zassert_equal(controller_app_settings_init(), -ENOENT);
    controller_app_tick();
}

ZTEST(motor_settings, test_save_recalculates_and_restores_motor)
{
    zassert_ok(controller_app_motor_save(2, 6000));
    zassert_ok(controller_app_motor_save(3, 1750));
    zassert_ok(controller_app_motor_save(5, MOTOR_SYNRM));
    zassert_equal(writes, 3);
    zassert_equal(stored.config.motor.type, MOTOR_SYNRM);
    controller_app_init();
    zassert_ok(controller_app_settings_init());
    struct controller_status status;
    controller_app_status(&status);
    zassert_equal(status.config.motor.type, MOTOR_SYNRM);
    zassert_equal(status.motor.pole_pairs_calc, 2);
    zassert_equal(status.motor.sync_speed_rpm, 1800);
    zassert_equal(status.motor.slip_x1000, 27);
    zassert_equal(status.motor.max_freq_hz_x100, 7200);
}

ZTEST(motor_settings, test_write_failure_preserves_ram_history_and_stored_config)
{
    zassert_ok(controller_app_motor_save(1, 15000));
    struct controller_status before, after;
    controller_app_status(&before);
    struct controller_event events[EVENT_CAPACITY];
    size_t count = controller_app_events(events, ARRAY_SIZE(events));
    write_error = -EIO;
    zassert_equal(controller_app_motor_save(1, 16000), -EIO);
    zassert_equal(controller_app_motor_save(5, MOTOR_PMSM), -EIO);
    controller_app_status(&after);
    zassert_mem_equal(&before.config, &after.config, sizeof(before.config));
    zassert_mem_equal(&before.motor, &after.motor, sizeof(before.motor));
    zassert_equal(controller_app_events(events, ARRAY_SIZE(events)), count);
    zassert_equal(stored.config.motor.nominal_current_ma, 15000);
    zassert_equal(stored.config.motor.type, MOTOR_INDUCTION);
    write_error = 0;
    zassert_ok(controller_app_motor_save(1, 16000));
}

ZTEST(motor_settings, test_invalid_candidate_does_not_write_flash)
{
    zassert_equal(controller_app_motor_save(3, 1), -EINVAL);
    zassert_equal(controller_app_motor_save(5, 99), -EINVAL);
    zassert_equal(controller_app_motor_save(1, 0), -EINVAL);
    zassert_equal(writes, 0);
    zassert_ok(controller_app_motor_save(1, 15000));
}

ZTEST(motor_settings, test_running_and_faulted_controller_cannot_save_motor)
{
    zassert_ok(controller_app_start());
    zassert_equal(controller_app_motor_save(1, 15000), -EBUSY);
    zassert_ok(controller_app_inject(true));
    controller_app_tick();
    zassert_equal(controller_app_motor_save(5, MOTOR_PMSM), -EBUSY);
    zassert_equal(writes, 0);
}

static uint8_t write_block(uint16_t address, const uint16_t *values, size_t count)
{
    uint8_t request[15], response[4];
    sys_put_be16(address, request);
    sys_put_be16(count, request + 2);
    request[4] = count * 2;
    for (size_t i = 0; i < count; i++) sys_put_be16(values[i], request + 5 + i * 2);
    size_t length;
    return controller_modbus_fc16(request, 5 + count * 2, response, sizeof(response), &length);
}

static void expect_block(const uint16_t expected[8])
{
    const uint8_t request[] = {0, 40, 0, 8};
    uint8_t response[17];
    size_t length;
    zassert_ok(controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length));
    zassert_equal(length, sizeof(response));
    zassert_equal(response[0], 16);
    for (size_t i = 0; i < 8; i++) zassert_equal(sys_get_be16(response + 1 + i * 2), expected[i]);
}

ZTEST(motor_settings, test_fc16_motor_data_commits_and_persists_one_candidate)
{
    const uint16_t values[] = {MOTOR_SYNRM, 6000, 1750, 6400};
    zassert_ok(write_block(40, values, ARRAY_SIZE(values)));
    zassert_equal(writes, 1);
    zassert_equal(stored.config.motor.nominal_current_ma, 640000);
    const uint16_t expected[] = {2, 6000, 1750, 6400, 2, 1800, 27, 7200};
    expect_block(expected);
    uint16_t legacy_current;
    zassert_ok(controller_modbus_register_read(11, &legacy_current));
    zassert_equal(legacy_current, UINT16_MAX);
    controller_app_init();
    zassert_ok(controller_app_settings_init());
    expect_block(expected);
}

ZTEST(motor_settings, test_fc16_motor_data_rejects_invalid_candidate_with_exception_four)
{
    const uint16_t invalid[][4] = {
        {3, 5000, 1450, 115}, {0, 0, 1450, 115}, {0, 5000, 0, 115},
        {0, 5000, 1450, 6401}, {0, 5000, 2100, 115},
    };
    const uint16_t defaults[] = {0, 5000, 1450, 115, 2, 1500, 33, 6000};
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        zassert_equal(write_block(40, invalid[i], 4), 4);
        expect_block(defaults);
    }
    zassert_equal(writes, 0);
}

ZTEST(motor_settings, test_fc16_motor_data_write_failure_leaves_ram_and_journal_unchanged)
{
    const uint16_t values[] = {1, 6000, 1750, 150};
    const uint16_t defaults[] = {0, 5000, 1450, 115, 2, 1500, 33, 6000};
    struct controller_event events[EVENT_CAPACITY];
    size_t count = controller_app_events(events, ARRAY_SIZE(events));
    write_error = -EIO;
    zassert_equal(write_block(40, values, 4), 4);
    expect_block(defaults);
    zassert_false(present);
    zassert_equal(controller_app_events(events, ARRAY_SIZE(events)), count);
    write_error = 0;
    zassert_ok(write_block(40, values, 4));
}

ZTEST(motor_settings, test_fc16_motor_data_read_only_crossing_is_atomic)
{
    const uint16_t values[] = {200, 1, 1, 1, 1};
    zassert_equal(write_block(43, values, 2), 2);
    zassert_equal(write_block(40, values, 5), 2);
    for (int address = 44; address <= 49; address++) zassert_equal(write_block(address, values, 1), 2);
    const uint16_t defaults[] = {0, 5000, 1450, 115, 2, 1500, 33, 6000};
    expect_block(defaults);
    zassert_equal(writes, 0);
    uint16_t value;
    zassert_equal(controller_modbus_register_read(48, &value), -ENOTSUP);
    zassert_equal(controller_modbus_register_read(49, &value), -ENOTSUP);
}

ZTEST(motor_settings, test_fc16_motor_data_is_blocked_in_running_and_fault)
{
    const uint16_t values[] = {0, 6000, 1750, 150};
    zassert_ok(controller_app_start());
    zassert_equal(write_block(40, values, 4), 4);
    zassert_ok(controller_app_inject(true));
    controller_app_tick();
    zassert_equal(write_block(40, values, 4), 4);
    zassert_equal(writes, 0);
}

ZTEST_SUITE(motor_settings, NULL, NULL, before, NULL, NULL);
