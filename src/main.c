#include "controller_app.h"
#include "controller_modbus.h"
#include "controller_watchdog.h"
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#ifdef CONFIG_APP_WATCHDOG
static const struct device *const watchdog = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int watchdog_channel;
static atomic_t crash_requested;
static atomic_t watchdog_ready;

#define WATCHDOG_TIMEOUT_MS 2000

static int watchdog_init(void)
{
    if (!device_is_ready(watchdog)) return -ENODEV;
    const struct wdt_timeout_cfg timeout = {
        // CMSDK в закреплённой версии Zephyr считает два окна до reset.
        // NMI первого окна не кормит WDT; второе заканчивается аппаратным сбросом.
        .window = { .min = 0, .max = IS_ENABLED(CONFIG_WDOG_CMSDK_APB) ?
                    WATCHDOG_TIMEOUT_MS / 2 : WATCHDOG_TIMEOUT_MS },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };
    int ret = wdt_install_timeout(watchdog, &timeout);
    if (ret < 0) return ret;
    watchdog_channel = ret;
    // Не приостанавливаем watchdog во время сна потока.
    return wdt_setup(watchdog, 0);
}
#endif

int controller_crash_test(void)
{
#ifdef CONFIG_APP_WATCHDOG
    if (!atomic_get(&watchdog_ready)) return -ENODEV;
    atomic_set(&crash_requested, 1);
    return 0;
#else
    return -ENOTSUP;
#endif
}

int main(void)
{
    controller_app_init();
    int ret = controller_app_settings_init();
    if (ret && ret != -ENOENT && ret != -ENOTSUP) {
        LOG_ERR("Settings load failed: %d; using defaults", ret);
    }
#ifdef CONFIG_APP_MODBUS
    ret = controller_modbus_init();
    if (ret) LOG_ERR("Modbus initialization failed: %d", ret);
#endif
#ifdef CONFIG_APP_WATCHDOG
    ret = watchdog_init();
    if (ret) {
        LOG_ERR("Watchdog initialization failed: %d; controller stopped", ret);
        return ret;
    }
    atomic_set(&watchdog_ready, 1);
    printk("Watchdog armed: timeout_ms=2000 reset=SOC\n");
#endif
    while (1) {
#ifdef CONFIG_APP_WATCHDOG
        if (atomic_get(&crash_requested)) {
            LOG_WRN("crash_test: polling thread suspended");
            // Shell продолжает работать, но цикл больше не кормит watchdog.
            k_sleep(K_FOREVER);
        }
#endif
        controller_app_tick();
#ifdef CONFIG_APP_WATCHDOG
        // tick завершает чтение, controller_update и запись выхода.
        ret = wdt_feed(watchdog, watchdog_channel);
        if (ret) {
            LOG_ERR("Watchdog feed failed: %d; waiting for reset", ret);
            k_sleep(K_FOREVER);
        }
#endif
        k_sleep(K_MSEC(CONTROLLER_PERIOD_MS));
    }
    return 0;
}
