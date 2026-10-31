#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modbus_client, LOG_LEVEL_INF);

K_MSGQ_DEFINE(modbus_tx_msgq, sizeof(int), 10, 4);

extern void update_pump_frequency(int freq); 

static void modbus_thread(void)
{
    int freq_from_pid;
    
    LOG_INF("Modbus thread started...");

    while (1) {
        if (k_msgq_get(&modbus_tx_msgq, &freq_from_pid, K_FOREVER) == 0) {
            
            uint16_t abb_ref = freq_from_pid * 4; 
            (void)abb_ref; // Заглушка, чтобы компилятор не ругался

            /* 
             * Раскомментировать когда на реал перенесу:
             * uint16_t cw = (freq_from_pid > 0) ? 0x0400 : 0x0401;
             * modbus_write_holding_reg(client_iface, 1, 0x0000, cw);
             * int err = modbus_write_holding_reg(client_iface, 1, 0x0001, abb_ref);
             */

            update_pump_frequency(freq_from_pid);
        }
    }
}

K_THREAD_DEFINE(modbus_tid, 2048, modbus_thread, NULL, NULL, NULL, 8, 0, 0);