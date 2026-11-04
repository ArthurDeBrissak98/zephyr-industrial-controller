#include "controller_app.h"
#include "controller_io.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(controller_app, LOG_LEVEL_INF);
K_MUTEX_DEFINE(controller_lock);
static struct controller controller;

void controller_app_init(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    controller_init(&controller);
    controller_io_inject_wire_break(false);
    controller_io_write(0);
    k_mutex_unlock(&controller_lock);
}

static void sample_locked(void)
{
    struct sensor_sample sample;

    uint32_t previous_faults = controller.active_faults;
    controller_io_read(&controller.config, &sample);
    controller_update(&controller, &sample, k_uptime_get());
    controller_io_write(controller.frequency_x100);
    if (controller.active_faults != previous_faults) {
        if (controller.active_faults) {
            LOG_ERR("FAULT WIRE_BREAK: current_ua=%d output_x100=%d",
                    sample.current_ua, controller_io_output());
        } else {
            LOG_INF("WIRE_BREAK cleared; manual reset required");
        }
    }
}

void controller_app_tick(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    sample_locked();
    k_mutex_unlock(&controller_lock);
}

int controller_app_start(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    // Перед пуском проверяем вход заново, не ждём следующего такта.
    if (controller.state != STATE_RUNNING) sample_locked();
    int ret = controller_start(&controller, k_uptime_get());
    k_mutex_unlock(&controller_lock);
    return ret;
}

void controller_app_stop(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    controller_stop(&controller, k_uptime_get());
    controller_io_write(controller.frequency_x100);
    k_mutex_unlock(&controller_lock);
}

int controller_app_reset(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    if (controller.state == STATE_FAULT) sample_locked();
    int ret = controller_reset(&controller, k_uptime_get());
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_configure(struct controller_config config)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    int ret = controller_configure(&controller, config, k_uptime_get());
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_inject(bool enabled)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    int ret = enabled && controller.config.mode != INPUT_4_20MA ? -ENOTSUP :
              controller_io_inject_wire_break(enabled);
    k_mutex_unlock(&controller_lock);
    return ret;
}

void controller_app_status(struct controller_status *status)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    *status = (struct controller_status) {
        .state = controller.state,
        .config = controller.config,
        .sample = controller.sample,
        .active_faults = controller.active_faults,
        .latched_faults = controller.latched_faults,
        .frequency_x100 = controller.frequency_x100,
        .output_x100 = controller_io_output(),
    };
    k_mutex_unlock(&controller_lock);
}

size_t controller_app_events(struct controller_event *events, size_t capacity)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    size_t count = MIN(capacity, controller.event_count);
    for (size_t i = 0; i < count; i++) {
        controller_event_get(&controller, i, &events[i]);
    }
    k_mutex_unlock(&controller_lock);
    return count;
}
