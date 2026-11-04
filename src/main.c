#include "controller_app.h"
#include <zephyr/kernel.h>

int main(void)
{
    controller_app_init();
    while (1) {
        controller_app_tick();
        k_sleep(K_MSEC(CONTROLLER_PERIOD_MS));
    }
    return 0;
}
