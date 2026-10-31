#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// тестим на симуляторе. Карта регистров - ABB.
// #include <zephyr/modbus/modbus.h> 

LOG_MODULE_REGISTER(modbus_client, LOG_LEVEL_INF);

// Очередь сюда кидает ПИД, отсюда забирает Modbus
K_MSGQ_DEFINE(modbus_tx_msgq, sizeof(int), 10, 4);

// TODO: если вдруг случится чудо и я займусь этим вплотную, сделать modbus_init_client()
// static int client_iface; 

extern void update_pump_frequency(int freq); // берем из sensor.c

static void modbus_thread(void)
{
    int freq_from_pid;
    
    LOG_INF("Modbus thread started...");

    while (1) {
        // Ждем пока ПИД че-нить пришлет
        if (k_msgq_get(&modbus_tx_msgq, &freq_from_pid, K_FOREVER) == 0) {
            
            // У ABB 20000 это 100% (т.е. 50 Гц). 
            // У нас freq_from_pid = 2500 для 25 Гц.
            // Значит 2500 * 4 = 10000. Вроде правильно.
            // TODO: спросить у сеньора, всегда ли коэффициент 4, или зависит от настроек 46.01 в приводе?
            uint16_t abb_ref = freq_from_pid * 4; 

            /* 
             * Раскомментировать когда на реал перенесу:
             * 
             * // даем команду старт
             * uint16_t cw = 0x0400; // Start bit
             * modbus_write_holding_reg(client_iface, 1, 0x0000, cw);
             * 
             * // Потом пишем частоту
             * int err = modbus_write_holding_reg(client_iface, 1, 0x0001, abb_ref);
             * if(err != 0) {
             *     LOG_ERR("Блин, не смогли отправить в ABB");
             * }
             */

            // --- ДЛЯ СИМУЛЯТОРА ---
            // Просто суем частоту обратно в физику, чтобы водичка бежала
            update_pump_frequency(freq_from_pid);

            // Чето лог спамит сильно, пока закомменчу
            // LOG_INF("Отправили в ABB: %d (Это %d.%02d Гц)", abb_ref, freq_from_pid/100, freq_from_pid%100);
            
            // КОСТЫЛЬ: зачем-то спим, хотя очередь и так блокирующая
            // k_sleep(K_MSEC(10)); 
        }
    }
}

// Приоритет 8, чтобы не мешать ПИДу и датчику
K_THREAD_DEFINE(modbus_tid, 2048, modbus_thread, NULL, NULL, NULL, 8, 0, 0);