#include "controller_app.h"
#include "controller_settings.h"
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

ZTEST_SUITE(motor_settings, NULL, NULL, before, NULL, NULL);
