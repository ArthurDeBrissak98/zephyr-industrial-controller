#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modbus_tcp, LOG_LEVEL_INF);

// Глобальный массив, куда sensor.c будет писать данные
uint16_t holding_regs[5] = {0};

// Заглушка потока, так как нативного TCP в Zephyr 4.5.0 нет
static void modbus_tcp_server_thread(void)
{
    LOG_WRN("Modbus TCP пока не реализован (нет API в Zephyr 4.5.0)");
    while (1) {
        k_sleep(K_SECONDS(1));
    }
}

K_THREAD_DEFINE(modbus_tcp_tid, 2048, modbus_tcp_server_thread, NULL, NULL, NULL, 8, 0, 0);