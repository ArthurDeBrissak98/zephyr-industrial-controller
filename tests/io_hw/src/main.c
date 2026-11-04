#include "controller_io.h"
#include "io_scaling.h"
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/ztest.h>
#include <errno.h>

static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(test_adc));
static struct controller ctrl;
static struct controller_config config;

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    controller_init(&ctrl);
    config = ctrl.config;
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 1500));
    zassert_ok(adc_emul_const_raw_value_set(adc, 1, 1250));
}

ZTEST(hw_io, test_current_and_voltage_scaling)
{
    struct sensor_sample sample;
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 500));
    zassert_ok(controller_io_read(&config, &sample));
    zassert_equal(sample.current_ua, 4000);
    zassert_equal(sample.voltage_mv, -1);
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 2500));
    zassert_ok(controller_io_read(&config, &sample));
    zassert_equal(sample.current_ua, 20000);
    config.mode = INPUT_0_10V;
    zassert_ok(adc_emul_const_raw_value_set(adc, 1, 2500));
    zassert_ok(controller_io_read(&config, &sample));
    zassert_equal(sample.voltage_mv, 10000);
    zassert_equal(sample.current_ua, -1);
}

ZTEST(hw_io, test_range_and_wire_break_keep_physical_values)
{
    struct sensor_sample sample;
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 0));
    zassert_equal(controller_io_read(&config, &sample), -EIO);
    zassert_equal(sample.current_ua, 0);
    zassert_ok(sample.read_error);
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.sensor_faults, FAULT_WIRE_BREAK);
    config.mode = INPUT_0_10V;
    zassert_ok(adc_emul_const_raw_value_set(adc, 1, 2625));
    zassert_equal(controller_io_read(&config, &sample), -EIO);
    zassert_equal(sample.voltage_mv, 10500);
    zassert_ok(sample.read_error);
}

static int failed_conversion(const struct device *dev, unsigned int channel,
                              void *data, uint32_t *value)
{
    ARG_UNUSED(dev); ARG_UNUSED(channel); ARG_UNUSED(data); ARG_UNUSED(value);
    return -EIO;
}

ZTEST(hw_io, test_adc_error_stops_and_latches_fault)
{
    struct sensor_sample sample;
    zassert_ok(controller_io_read(&config, &sample));
    controller_update(&ctrl, &sample, 0);
    zassert_ok(controller_start(&ctrl, 0));
    zassert_ok(adc_emul_raw_value_func_set(adc, 0, failed_conversion, NULL));
    zassert_equal(controller_io_read(&config, &sample), -EIO);
    zassert_equal(sample.read_error, -EIO);
    controller_update(&ctrl, &sample, 100);
    zassert_equal(ctrl.state, STATE_FAULT);
    zassert_equal(ctrl.sensor_faults, FAULT_INPUT_IO);
    zassert_equal(ctrl.frequency_x100, 0);
    zassert_equal(ctrl.signal_filter.count, 0);
    zassert_equal(controller_reset(&ctrl, 101), -EBUSY);
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 1500));
    zassert_ok(controller_io_read(&config, &sample));
    controller_update(&ctrl, &sample, 200);
    zassert_equal(ctrl.active_faults, 0);
    zassert_equal(ctrl.latched_faults, FAULT_INPUT_IO);
    zassert_ok(controller_reset(&ctrl, 201));
}

static int delayed_conversion(const struct device *dev, unsigned int channel,
                               void *data, uint32_t *value)
{
    ARG_UNUSED(dev); ARG_UNUSED(channel); ARG_UNUSED(data);
    k_sleep(K_MSEC(100));
    *value = 1500;
    return 0;
}

ZTEST(hw_io, test_timeout_does_not_reuse_pending_buffer)
{
    struct sensor_sample sample;
    zassert_ok(adc_emul_raw_value_func_set(adc, 0, delayed_conversion, NULL));
    int64_t started = k_uptime_get();
    zassert_equal(controller_io_read(&config, &sample), -ETIMEDOUT);
    zassert_true(k_uptime_get() - started <= 40);
    zassert_equal(sample.read_error, -ETIMEDOUT);
    // Смена режима не должна запустить второй запрос поверх незавершённого.
    config.mode = INPUT_0_10V;
    started = k_uptime_get();
    zassert_equal(controller_io_read(&config, &sample), -ETIMEDOUT);
    zassert_true(k_uptime_get() - started <= 10);
    k_sleep(K_MSEC(120));
    zassert_ok(controller_io_read(&config, &sample));
    zassert_equal(sample.voltage_mv, 5000);
}

ZTEST(hw_io, test_clipping_and_unsupported_operations)
{
    struct sensor_sample sample;
    zassert_ok(adc_emul_const_raw_value_set(adc, 0, 4095));
    zassert_equal(controller_io_read(&config, &sample), -EIO);
    zassert_equal(sample.read_error, -EIO);
    config.mode = (enum input_mode)99;
    zassert_equal(controller_io_read(&config, &sample), -ENOTSUP);
    zassert_equal(controller_io_inject_signal(true, 1000), -ENOTSUP);
    zassert_equal(controller_io_inject_wire_break(true), -ENOTSUP);
    controller_io_write(5000);
    zassert_equal(controller_io_output(), 0);
}

ZTEST(hw_io, test_scaling_uses_wide_arithmetic_and_validates_parameters)
{
    struct sensor_sample sample = {0};
    zassert_equal(controller_adc_scale(INPUT_4_20MA, 1000, 0, 4, &sample), -EINVAL);
    zassert_equal(controller_adc_scale(INPUT_0_10V, INT32_MAX, 125, 4, &sample), -EIO);
    zassert_equal(controller_adc_scale(INPUT_4_20MA, 2625, 125, 4, &sample), 0);
    zassert_equal(sample.current_ua, 21000);
    zassert_equal(controller_adc_scale(INPUT_4_20MA, 2626, 125, 4, &sample), -EIO);
}

ZTEST_SUITE(hw_io, NULL, NULL, before, NULL, NULL);
