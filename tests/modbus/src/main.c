#include "controller_modbus.h"
#include "controller_app.h"
#include <zephyr/ztest.h>
#include <errno.h>

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    controller_app_init();
    controller_app_tick();
}

ZTEST(modbus_map, test_register_map_and_invalid_addresses)
{
    uint16_t regs[5];
    controller_modbus_snapshot(regs);
    zassert_equal(regs[0], STATE_READY);
    zassert_equal(regs[1], 0);
    zassert_equal(regs[2], 0);
    zassert_equal(regs[3], 500);
    zassert_equal(regs[4], 500);
    for (int i = 0; i < 5; i++) {
        uint16_t value;
        zassert_ok(controller_modbus_register_read(i, &value));
        zassert_equal(value, regs[i]);
    }
    uint16_t value = 123;
    zassert_equal(controller_modbus_register_read(5, &value), -ENOTSUP);
    zassert_equal(value, 123);
    for (int i = 0; i < 5; i++) {
        if (i != 4) zassert_equal(controller_modbus_register_write(i, 1), -ENOTSUP);
    }
}

ZTEST(modbus_map, test_setpoint_survives_polling_and_rejected_writes)
{
    zassert_ok(controller_app_start());
    zassert_ok(controller_modbus_register_write(4, 700));
    for (int i = 0; i < 10; i++) controller_app_tick();
    zassert_equal(controller_modbus_register_write(4, 1001), -EINVAL);
    zassert_equal(controller_modbus_register_write(4, UINT16_MAX), -EINVAL);
    uint16_t value;
    zassert_ok(controller_modbus_register_read(4, &value));
    zassert_equal(value, 700);
    struct controller_status status;
    controller_app_status(&status);
    zassert_equal(status.sample.setpoint_x10, 700);
    zassert_equal(status.state, STATE_RUNNING);
}

ZTEST(modbus_map, test_negative_ahu_setpoint_and_profile_defaults)
{
    struct controller_status status;
    controller_app_status(&status);
    status.config.profile = PROFILE_AHU;
    zassert_ok(controller_app_configure(status.config));
    zassert_ok(controller_app_inject_signal(true, 4000));
    zassert_ok(controller_modbus_register_write(4, (uint16_t)-50));
    controller_app_tick();
    uint16_t value;
    zassert_ok(controller_modbus_register_read(4, &value));
    zassert_equal((int16_t)value, -50);
    zassert_ok(controller_modbus_register_read(3, &value));
    zassert_equal((int16_t)value, -200);
    zassert_equal(controller_modbus_register_write(4, (uint16_t)-201), -EINVAL);
    zassert_equal(controller_modbus_register_write(4, 401), -EINVAL);
    status.config.profile = PROFILE_PUMP;
    zassert_ok(controller_app_configure(status.config));
    controller_app_tick();
    zassert_ok(controller_modbus_register_read(4, &value));
    zassert_equal(value, 500);
}

ZTEST(modbus_map, test_fault_state_is_visible_and_writes_do_not_reset_it)
{
    zassert_ok(controller_app_start());
    zassert_ok(controller_app_inject(true));
    controller_app_tick();
    uint16_t regs[5];
    controller_modbus_snapshot(regs);
    zassert_equal(regs[0], STATE_FAULT);
    zassert_equal(regs[1], FAULT_WIRE_BREAK);
    zassert_ok(controller_modbus_register_write(4, 600));
    controller_app_tick();
    controller_modbus_snapshot(regs);
    zassert_equal(regs[0], STATE_FAULT);
    zassert_equal(regs[1], FAULT_WIRE_BREAK);
    zassert_equal(regs[4], 600);
    struct controller_status status;
    controller_app_status(&status);
    zassert_equal(status.output_x100, 0);
}

ZTEST(modbus_map, test_warning_register)
{
    struct protection_config warning = {600, 10, 0, true};
    zassert_ok(controller_app_protection_configure(PROTECTION_LOW_WARNING, warning));
    controller_app_tick();
    uint16_t regs[5];
    controller_modbus_snapshot(regs);
    zassert_equal(regs[0], STATE_READY);
    zassert_equal(regs[1], 0);
    zassert_equal(regs[2], WARNING_LOW);
}

ZTEST_SUITE(modbus_map, NULL, NULL, before, NULL, NULL);
