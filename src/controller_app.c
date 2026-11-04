#include "controller_app.h"
#include "controller_io.h"
#include "controller_settings.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(controller_app, LOG_LEVEL_INF);
K_MUTEX_DEFINE(controller_lock);
static struct controller controller;
static bool signal_injected;
static bool wire_break_injected;
static int injected_value;
static bool settings_ready;
static bool settings_present;
static int settings_error;
static bool setpoint_override;
static int setpoint_x10;
static enum controller_profile setpoint_profile;

void controller_app_init(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    controller_init(&controller);
    signal_injected = false;
    wire_break_injected = false;
    injected_value = 0;
    settings_ready = false;
    settings_present = false;
    settings_error = 0;
    setpoint_override = false;
    controller_io_inject_wire_break(false);
    controller_io_inject_signal(false, 0);
    controller_io_write(0);
    k_mutex_unlock(&controller_lock);
}

static void sample_locked(void)
{
    struct sensor_sample sample;

    uint32_t previous_faults = controller.active_faults;
    uint32_t previous_warnings = controller.active_warnings;
    controller_io_read(&controller.config, &sample);
    if (setpoint_profile != controller.config.profile) setpoint_override = false;
    if (setpoint_override) sample.setpoint_x10 = setpoint_x10;
    controller_update(&controller, &sample, k_uptime_get());
    controller_io_write(controller.frequency_x100);
    if (controller.active_warnings != previous_warnings) {
        LOG_WRN("Warnings changed: active=0x%x measured_x10=%d",
                controller.active_warnings, controller.sample.measured_x10);
    }
    if (controller.active_faults != previous_faults) {
        if (controller.active_faults) {
            LOG_ERR("FAULT active=0x%x measured_x10=%d current_ua=%d voltage_mv=%d output_x100=%d",
                    controller.active_faults, controller.sample.measured_x10,
                    sample.current_ua, sample.voltage_mv, controller_io_output());
        } else {
            LOG_INF("Input faults cleared; manual reset required");
        }
    }
}

void controller_app_tick(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    sample_locked();
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
}

int controller_app_setpoint_set(int value)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    bool pump = controller.config.profile == PROFILE_PUMP;
    int ret = 0;
    if ((unsigned int)controller.state > STATE_FAULT ||
        value < (pump ? 0 : -200) || value > (pump ? 1000 : 400)) {
        ret = -EINVAL;
    } else {
        setpoint_x10 = value;
        setpoint_profile = controller.config.profile;
        setpoint_override = true;
        controller.sample.setpoint_x10 = value;
    }
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_start(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    // Перед пуском проверяем вход заново, не ждём следующего такта.
    if (controller.state != STATE_RUNNING) sample_locked();
    int ret = controller_start(&controller, k_uptime_get());
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

void controller_app_stop(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    controller_stop(&controller, k_uptime_get());
    controller_io_write(controller.frequency_x100);
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
}

int controller_app_reset(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    if (controller.state == STATE_FAULT) sample_locked();
    int ret = controller_reset(&controller, k_uptime_get());
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_configure(struct controller_config config)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    bool profile_changed = controller.config.profile != config.profile;
    int ret = controller_configure(&controller, config, k_uptime_get());
    if (!ret && profile_changed) setpoint_override = false;
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_inject(bool enabled)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    int ret = enabled && controller.config.mode != INPUT_4_20MA ? -ENOTSUP :
              controller_io_inject_wire_break(enabled);
    if (!ret) wire_break_injected = enabled;
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

void controller_app_status(struct controller_status *status)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    *status = (struct controller_status) {
        .state = controller.state,
        .config = controller.config,
        .sample = controller.sample,
        .active_warnings = controller.active_warnings,
        .sensor_faults = controller.sensor_faults,
        .process_faults = controller.process_faults,
        .active_faults = controller.active_faults,
        .latched_faults = controller.latched_faults,
        .frequency_x100 = controller.frequency_x100,
        .output_x100 = controller_io_output(),
        .signal_valid = controller.sample_valid,
        .filter_count = controller.signal_filter.count,
        .signal_injected = signal_injected,
        .wire_break_injected = wire_break_injected,
        .injected_value = injected_value,
        .settings_ready = settings_ready,
        .settings_present = settings_present,
        .settings_error = settings_error,
    };
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        status->protections[i] = controller.protections[i];
    }
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
}

int controller_app_inject_signal(bool enabled, int value)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    int ret = controller_io_inject_signal(enabled, value);
    if (!ret) {
        signal_injected = enabled;
        injected_value = enabled ? value : 0;
    }
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

size_t controller_app_events(struct controller_event *events, size_t capacity)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    size_t count = MIN(capacity, controller.event_count);
    for (size_t i = 0; i < count; i++) {
        controller_event_get(&controller, i, &events[i]);
    }
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return count;
}

int controller_app_protection_configure(enum protection_id id, struct protection_config config)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
    int ret = controller_protection_configure(&controller, id, config, k_uptime_get());
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

#ifdef CONFIG_APP_SETTINGS
static int load_settings_locked(void)
{
    if (controller.state == STATE_RUNNING || controller.state == STATE_FAULT) return -EBUSY;
    struct controller_parameters parameters;
    int ret = controller_settings_read(&parameters);
    if (!ret) ret = controller_parameters_apply(&controller, &parameters, k_uptime_get());
    if (!ret) {
        settings_present = true;
        setpoint_override = false;
    }
    if (ret == -ENOENT) settings_present = false;
    return ret;
}
#endif

int controller_app_settings_init(void)
{
    int ret = -ENOTSUP;
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
#ifdef CONFIG_APP_SETTINGS
    ret = controller_settings_init();
    settings_ready = ret == 0;
    if (!ret) ret = load_settings_locked();
    settings_error = ret == -ENOENT ? 0 : ret;
#endif
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_settings_save(void)
{
    int ret = -ENOTSUP;
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
#ifdef CONFIG_APP_SETTINGS
    if (!settings_ready) ret = -ENODEV;
    else if (controller.state == STATE_RUNNING || controller.state == STATE_FAULT) ret = -EBUSY;
    else {
        struct controller_parameters parameters;
        controller_parameters_get(&controller, &parameters);
        ret = controller_settings_write(&parameters);
        if (!ret) settings_present = true;
    }
    settings_error = ret;
#endif
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_settings_load(void)
{
    int ret = -ENOTSUP;
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
#ifdef CONFIG_APP_SETTINGS
    ret = settings_ready ? load_settings_locked() : -ENODEV;
    settings_error = ret;
#endif
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

int controller_app_settings_erase(void)
{
    int ret = -ENOTSUP;
    k_mutex_lock(&controller_lock, K_FOREVER);
    k_mutex_lock(&controller.mutex, K_FOREVER);
#ifdef CONFIG_APP_SETTINGS
    if (!settings_ready) ret = -ENODEV;
    else if (controller.state == STATE_RUNNING || controller.state == STATE_FAULT) ret = -EBUSY;
    else ret = controller_settings_erase();
    if (!ret) settings_present = false;
    settings_error = ret;
#endif
    k_mutex_unlock(&controller.mutex);
    k_mutex_unlock(&controller_lock);
    return ret;
}

void controller_app_events_clear(void)
{
    k_mutex_lock(&controller_lock, K_FOREVER);
    controller_events_clear(&controller);
    k_mutex_unlock(&controller_lock);
}
