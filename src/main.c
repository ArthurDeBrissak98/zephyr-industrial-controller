#include "controller_app.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    controller_app_init();
    int ret = controller_app_settings_init();
    if (ret && ret != -ENOENT && ret != -ENOTSUP) {
        LOG_ERR("Settings load failed: %d; using defaults", ret);
    }
    while (1) {
        controller_app_tick();
        k_sleep(K_MSEC(CONTROLLER_PERIOD_MS));
    }
    return 0;
}
