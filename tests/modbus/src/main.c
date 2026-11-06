#include "controller_modbus.h"
#include "controller_app.h"
#include <zephyr/ztest.h>
#include <errno.h>
#include <zephyr/sys/byteorder.h>

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

ZTEST(modbus_map, test_fc16_rejects_entire_request_before_commit)
{
    const uint8_t requests[][9] = {
        {0, 4, 0, 2, 4, 2, 88, 0, 1},
        {0, 3, 0, 2, 4, 0, 1, 2, 88},
        {255, 255, 0, 2, 4, 0, 1, 2, 88},
    };
    uint8_t response[16];
    size_t length;
    for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
        zassert_equal(controller_modbus_fc16(requests[i], sizeof(requests[i]), response, sizeof(response), &length), 2);
        zassert_equal(length, 0);
        uint16_t value;
        zassert_ok(controller_modbus_register_read(4, &value));
        zassert_equal(value, 500);
    }
    const uint8_t invalid_value[] = {0, 4, 0, 1, 2, 3, 233};
    zassert_equal(controller_modbus_fc16(invalid_value, sizeof(invalid_value), response, sizeof(response), &length), 2);
    const uint8_t valid[] = {0, 4, 0, 1, 2, 2, 138};
    zassert_ok(controller_modbus_fc16(valid, sizeof(valid), response, sizeof(response), &length));
    zassert_equal(length, 4);
    zassert_mem_equal(response, valid, 4);
    uint16_t value;
    zassert_ok(controller_modbus_register_read(4, &value));
    zassert_equal(value, 650);
    // Неверная длина и недостаточный буфер ответа не должны менять уставку.
    zassert_equal(controller_modbus_fc16(valid, sizeof(valid) - 1, response, sizeof(response), &length), 3);
    zassert_equal(controller_modbus_fc16(valid, sizeof(valid), response, 3, &length), 4);
    zassert_ok(controller_modbus_register_read(4, &value));
    zassert_equal(value, 650);
}

ZTEST(modbus_map, test_fc03_encodes_one_snapshot_and_validates_range)
{
    const uint8_t request[] = {0, 0, 0, 5};
    const uint8_t expected[] = {10, 0, 1, 0, 0, 0, 0, 1, 244, 1, 244};
    uint8_t response[16];
    size_t length;
    zassert_ok(controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length));
    zassert_equal(length, sizeof(expected));
    zassert_mem_equal(response, expected, sizeof(expected));
    const uint8_t invalid[] = {0, 4, 0, 2};
    zassert_equal(controller_modbus_fc03(invalid, sizeof(invalid), response, sizeof(response), &length), 2);
    zassert_equal(length, 0);
    zassert_equal(controller_modbus_fc03(request, 3, response, sizeof(response), &length), 3);
    zassert_equal(controller_modbus_fc03(request, sizeof(request), response, 10, &length), 4);
}

K_THREAD_STACK_DEFINE(snapshot_stack, 2048);
static struct k_thread snapshot_thread;
static bool snapshot_worker_ok;

static void snapshot_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (int i = 0; i < 80; i++) {
        if (controller_app_inject(true)) snapshot_worker_ok = false;
        controller_app_tick();
        k_sleep(K_MSEC(1));
        if (controller_app_inject(false)) snapshot_worker_ok = false;
        controller_app_tick();
        if (controller_app_reset() || controller_app_start()) snapshot_worker_ok = false;
        k_sleep(K_MSEC(1));
    }
}

ZTEST(modbus_map, test_fc03_stays_consistent_during_fault_transitions)
{
    zassert_ok(controller_app_start());
    snapshot_worker_ok = true;
    int previous_priority = k_thread_priority_get(k_current_get());
    // Писатель может вытеснить читателя даже между регистрами ответа.
    k_thread_priority_set(k_current_get(), 6);
    k_thread_create(&snapshot_thread, snapshot_stack, K_THREAD_STACK_SIZEOF(snapshot_stack),
                    snapshot_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    bool consistent = true;
    const uint8_t request[] = {0, 0, 0, 5};
    for (int i = 0; i < 200; i++) {
        uint8_t response[11];
        size_t length;
        if (controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length)) {
            consistent = false;
        } else {
            uint16_t state = sys_get_be16(response + 1);
            uint16_t faults = sys_get_be16(response + 3);
            int16_t measured = sys_get_be16(response + 7);
            if (state == STATE_RUNNING && (faults || measured <= 0)) consistent = false;
            if (faults == FAULT_WIRE_BREAK && (state != STATE_FAULT || measured != 0)) consistent = false;
        }
        k_sleep(K_MSEC(1));
    }
    int joined = k_thread_join(&snapshot_thread, K_SECONDS(2));
    if (joined) k_thread_abort(&snapshot_thread);
    k_thread_priority_set(k_current_get(), previous_priority);
    zassert_ok(joined);
    zassert_true(snapshot_worker_ok);
    zassert_true(consistent, "FC03 combined fields from different controller states");
}

static uint8_t write_motor(uint16_t address, const uint16_t *values, uint16_t count)
{
    uint8_t request[15], response[4];
    sys_put_be16(address, request);
    sys_put_be16(count, request + 2);
    request[4] = count * 2;
    for (unsigned int i = 0; i < count; i++) sys_put_be16(values[i], request + 5 + i * 2);
    size_t length;
    return controller_modbus_fc16(request, 5 + count * 2, response, sizeof(response), &length);
}

ZTEST(modbus_map, test_motor_data_registers_are_big_endian_and_read_only)
{
    const uint8_t request[] = {0, 40, 0, 8};
    uint8_t response[17];
    size_t length;
    zassert_ok(controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length));
    const uint8_t expected[] = {16, 0, 0, 0x13, 0x88, 0x05, 0xaa, 0, 115,
                               0, 2, 0x05, 0xdc, 0, 33, 0x17, 0x70};
    zassert_equal(length, sizeof(expected));
    zassert_mem_equal(response, expected, sizeof(expected));
    zassert_equal(controller_modbus_register_write(40, 1), -ENOTSUP);
    zassert_equal(controller_modbus_fc03(request, sizeof(request), response, 16, &length), 4);
    const uint8_t crossing[] = {0, 47, 0, 2};
    zassert_equal(controller_modbus_fc03(crossing, sizeof(crossing), response, sizeof(response), &length), 2);
}

ZTEST(modbus_map, test_motor_registers_and_sparse_map)
{
    const uint8_t request[] = {0, 10, 0, 5};
    uint8_t response[11];
    size_t length;
    zassert_ok(controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length));
    const uint16_t expected[] = {400, 11500, 5000, 1450, 2};
    zassert_equal(length, 11);
    for (int i = 0; i < 5; i++) zassert_equal(sys_get_be16(response + 1 + i * 2), expected[i]);
    for (uint16_t address = 5; address < 10; address++) {
        uint16_t value = 123;
        zassert_equal(controller_modbus_register_read(address, &value), -ENOTSUP);
        zassert_equal(value, 123);
    }
    const uint8_t gap[] = {0, 4, 0, 7};
    zassert_equal(controller_modbus_fc03(gap, sizeof(gap), response, sizeof(response), &length), 2);
    const uint8_t beyond[] = {0, 14, 0, 2};
    zassert_equal(controller_modbus_fc03(beyond, sizeof(beyond), response, sizeof(response), &length), 2);
    zassert_equal(controller_modbus_register_write(10, 230), -ENOTSUP);
}

ZTEST(modbus_map, test_motor_fc16_validates_final_candidate_before_commit)
{
    const uint16_t invalid[] = {230, 0, 6000, 1750, 2};
    zassert_equal(write_motor(10, invalid, 5), 3);
    uint16_t value;
    zassert_ok(controller_modbus_register_read(10, &value));
    zassert_equal(value, 400);
    const uint16_t lower_frequency[] = {4000};
    zassert_equal(write_motor(12, lower_frequency, 1), 3);
    // Частота 40 Гц и 1150 об/мин согласованы только как единая запись.
    const uint16_t pair[] = {4000, 1150};
    zassert_ok(write_motor(12, pair, 2));
    struct controller_status status;
    controller_app_status(&status);
    zassert_equal(status.config.motor.nominal_frequency_x100, 4000);
    zassert_equal(status.config.motor.nominal_speed_rpm, 1150);
    const uint16_t pole_zero[] = {0};
    zassert_equal(write_motor(14, pole_zero, 1), 3);
    const uint16_t crossing[] = {3, 230};
    zassert_equal(write_motor(14, crossing, 2), 2);
    controller_app_status(&status);
    zassert_equal(status.config.motor.pole_pairs, 2);
    zassert_equal(status.config.motor.nominal_voltage_v, 400);
}

ZTEST(modbus_map, test_motor_fc16_rejects_invalid_pole_pair_calculation)
{
    const uint16_t invalid[][5] = {
        {230, 15000, 5000, 1, 2},
        {230, 15000, 99, 50, 1},
        {230, 15000, 50001, 1500, 2},
        {230, 15000, 50000, 30001, 1},
        {230, 15000, 50000, 50, 2},
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        zassert_equal(write_motor(10, invalid[i], 5), 3);
        struct controller_status status;
        controller_app_status(&status);
        struct motor_parameters defaults = controller_motor_defaults();
        zassert_mem_equal(&status.config.motor, &defaults, sizeof(defaults));
    }
    const uint16_t valid[] = {230, 15000, 6000, 1750, 2};
    zassert_ok(write_motor(10, valid, 5));
}

ZTEST(modbus_map, test_motor_cannot_be_changed_running_or_faulted)
{
    const uint16_t motor[] = {230, 15000, 6000, 1750, 2};
    zassert_ok(controller_app_start());
    zassert_equal(write_motor(10, motor, 5), 2);
    zassert_ok(controller_app_inject(true));
    controller_app_tick();
    zassert_equal(write_motor(10, motor, 5), 2);
    struct controller_status status;
    controller_app_status(&status);
    zassert_equal(status.state, STATE_FAULT);
    zassert_equal(status.config.motor.nominal_voltage_v, 400);
}

ZTEST(modbus_map, test_motor_validation_rejects_zero_overflow_and_excess_speed)
{
    struct motor_parameters motor = controller_motor_defaults();
    motor.nominal_speed_rpm = 1501;
    zassert_equal(controller_motor_validate(&motor), -EINVAL);
    motor = controller_motor_defaults();
    motor.nominal_voltage_v = 65536;
    zassert_equal(controller_motor_validate(&motor), -EINVAL);
    motor.nominal_voltage_v = 400;
    motor.nominal_current_ma = -1;
    zassert_equal(controller_motor_validate(&motor), -EINVAL);
    motor.nominal_current_ma = 11500;
    motor.pole_pairs = 0;
    zassert_equal(controller_motor_validate(&motor), -EINVAL);
}

static void motor_snapshot_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    const uint16_t motors[2][5] = {{400, 11500, 5000, 1450, 2}, {230, 15000, 6000, 1750, 2}};
    for (int i = 0; i < 80; i++) {
        if (write_motor(10, motors[i % 2], 5)) snapshot_worker_ok = false;
        k_sleep(K_MSEC(1));
    }
}

ZTEST(modbus_map, test_motor_fc03_snapshot_is_consistent_during_batch_writes)
{
    const uint16_t motors[2][5] = {{400, 11500, 5000, 1450, 2}, {230, 15000, 6000, 1750, 2}};
    snapshot_worker_ok = true;
    int previous_priority = k_thread_priority_get(k_current_get());
    k_thread_priority_set(k_current_get(), 6);
    k_thread_create(&snapshot_thread, snapshot_stack, K_THREAD_STACK_SIZEOF(snapshot_stack),
                    motor_snapshot_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    bool consistent = true;
    const uint8_t request[] = {0, 10, 0, 5};
    for (int i = 0; i < 200; i++) {
        uint8_t response[11];
        size_t length;
        if (controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length)) {
            consistent = false;
        } else {
            bool first = true, second = true;
            for (int j = 0; j < 5; j++) {
                uint16_t value = sys_get_be16(response + 1 + j * 2);
                first = first && value == motors[0][j];
                second = second && value == motors[1][j];
            }
            if (!first && !second) consistent = false;
        }
        k_sleep(K_MSEC(1));
    }
    int joined = k_thread_join(&snapshot_thread, K_SECONDS(2));
    if (joined) k_thread_abort(&snapshot_thread);
    k_thread_priority_set(k_current_get(), previous_priority);
    zassert_ok(joined);
    zassert_true(snapshot_worker_ok);
    zassert_true(consistent, "FC03 mixed parameters from different motors");
}

ZTEST(modbus_map, test_motor_data_snapshot_includes_consistent_derived_fields)
{
    const uint16_t motors[2][8] = {{0, 5000, 1450, 115, 2, 1500, 33, 6000},
                                    {0, 6000, 1750, 150, 2, 1800, 27, 7200}};
    snapshot_worker_ok = true;
    int previous_priority = k_thread_priority_get(k_current_get());
    k_thread_priority_set(k_current_get(), 6);
    k_thread_create(&snapshot_thread, snapshot_stack, K_THREAD_STACK_SIZEOF(snapshot_stack),
                    motor_snapshot_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    bool consistent = true;
    const uint8_t request[] = {0, 40, 0, 8};
    for (int i = 0; i < 200; i++) {
        uint8_t response[17];
        size_t length;
        if (controller_modbus_fc03(request, sizeof(request), response, sizeof(response), &length)) {
            consistent = false;
        } else {
            bool first = true, second = true;
            for (int j = 0; j < 8; j++) {
                uint16_t value = sys_get_be16(response + 1 + j * 2);
                first = first && value == motors[0][j];
                second = second && value == motors[1][j];
            }
            if (!first && !second) consistent = false;
        }
        k_sleep(K_MSEC(1));
    }
    int joined = k_thread_join(&snapshot_thread, K_SECONDS(2));
    if (joined) k_thread_abort(&snapshot_thread);
    k_thread_priority_set(k_current_get(), previous_priority);
    zassert_ok(joined);
    zassert_true(snapshot_worker_ok);
    zassert_true(consistent, "FC03 mixed parameters from different motors");
}

ZTEST_SUITE(modbus_map, NULL, NULL, before, NULL, NULL);
