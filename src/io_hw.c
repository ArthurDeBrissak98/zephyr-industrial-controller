#include "controller_io.h"
#include "io_scaling.h"
#include <zephyr/drivers/adc.h>
#include <errno.h>

#define INPUT_NODE DT_PATH(zephyr_user)
#define ADC_TIMEOUT_MS 20

static const struct adc_dt_spec inputs[] = {
    ADC_DT_SPEC_GET_BY_IDX(INPUT_NODE, 0),
    ADC_DT_SPEC_GET_BY_IDX(INPUT_NODE, 1),
};
BUILD_ASSERT(DT_PROP(INPUT_NODE, current_shunt_ohms) > 0);
BUILD_ASSERT(DT_PROP(INPUT_NODE, voltage_divider_multiplier) > 0);

K_MUTEX_DEFINE(adc_lock);
K_SEM_DEFINE(adc_request, 0, 1);
K_SEM_DEFINE(adc_finished, 0, 1);
static struct controller_config request_config;
static struct sensor_sample request_sample;
static int request_result;
static bool pending;
static bool configured[ARRAY_SIZE(inputs)];

static int acquire_sample(const struct controller_config *config, struct sensor_sample *sample)
{
    int index;
    if (config->mode == INPUT_4_20MA) index = 0;
    else if (config->mode == INPUT_0_10V) index = 1;
    else return -ENOTSUP;
    const struct adc_dt_spec *adc = &inputs[index];
    if (!adc_is_ready_dt(adc)) return -EIO;
    if (!configured[index]) {
        int ret = adc_channel_setup_dt(adc);
        if (ret) return ret;
        configured[index] = true;
    }
    int16_t raw_value;
    struct adc_sequence sequence = {
        .buffer = &raw_value, .buffer_size = sizeof(raw_value),
    };
    int ret = adc_sequence_init_dt(adc, &sequence);
    if (ret) return ret;
    ret = adc_read(adc->dev, &sequence);
    if (ret) return ret;
    if (raw_value < 0 || (uint32_t)raw_value >= BIT(adc->resolution) - 1) return -EIO;
    int32_t millivolts = raw_value;
    ret = adc_raw_to_millivolts_dt(adc, &millivolts);
    if (ret) return ret;
    ret = controller_adc_scale(config->mode, millivolts,
        DT_PROP(INPUT_NODE, current_shunt_ohms),
        DT_PROP(INPUT_NODE, voltage_divider_multiplier), sample);
    // Ошибка диапазона не отменяет сам отсчёт: его причину определят защиты.
    if (sample->current_ua >= 0 || sample->voltage_mv >= 0) sample->read_error = 0;
    return ret;
}

static void adc_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sem_take(&adc_request, K_FOREVER);
        request_result = acquire_sample(&request_config, &request_sample);
        if (request_sample.read_error) {
            request_sample.read_error = request_result == -ENOTSUP ? -ENOTSUP : -EIO;
            request_result = request_sample.read_error;
        }
        k_sem_give(&adc_finished);
    }
}

K_THREAD_DEFINE(adc_worker_thread, 1536, adc_worker, NULL, NULL, NULL, 5, 0, 0);

int controller_io_read(const struct controller_config *config, struct sensor_sample *sample)
{
    *sample = (struct sensor_sample) {
        .current_ua = -1, .voltage_mv = -1,
        .setpoint_x10 = config->profile == PROFILE_PUMP ? 500 : 200,
        .read_error = -EIO,
    };
    if (k_mutex_lock(&adc_lock, K_NO_WAIT)) {
        sample->read_error = -EBUSY;
        return -EBUSY;
    }
    int ret = -ETIMEDOUT;
    if (pending) {
        if (k_sem_take(&adc_finished, K_NO_WAIT)) goto out;
        // Старый запрос завершён: только теперь можно менять его память.
        pending = false;
    }
    request_config = *config;
    request_sample = *sample;
    pending = true;
    k_sem_give(&adc_request);
    if (k_sem_take(&adc_finished, K_MSEC(ADC_TIMEOUT_MS))) goto out;
    pending = false;
    *sample = request_sample;
    ret = request_result;
out:
    if (ret == -ETIMEDOUT) sample->read_error = ret;
    k_mutex_unlock(&adc_lock);
    return ret;
}

void controller_io_write(int frequency_x100)
{
    // Выход на привод подключается после выбора его интерфейса; пока он отключён.
    ARG_UNUSED(frequency_x100);
}

int controller_io_output(void)
{
    return 0;
}

int controller_io_inject_wire_break(bool enabled)
{
    ARG_UNUSED(enabled);
    return -ENOTSUP;
}

int controller_io_inject_signal(bool enabled, int value)
{
    ARG_UNUSED(enabled);
    ARG_UNUSED(value);
    return -ENOTSUP;
}
