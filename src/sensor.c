#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_data), 10, 4);
K_MSGQ_DEFINE(monitor_msgq, sizeof(struct sensor_data), 10, 4);

static atomic_t current_freq_hz_x100;
void update_pump_frequency(int freq) { atomic_set(&current_freq_hz_x100, freq); }

static void sensor_thread(void)
{
    struct sensor_data data = {0};
    int cycle_count = 0;
    atomic_set(&current_freq_hz_x100, 2500);

    int level = 500;       
    int temp_sup = 150;    
    int filter_dp = 50;    

    LOG_INF("Старт профиля: %s", (ACTIVE_PROFILE == PROFILE_PUMP) ? "PUMP" : "AHU");

    while (1) {
        cycle_count++;
        int freq = atomic_get(&current_freq_hz_x100);
        
        // ОБЯЗАТЕЛЬНО сбрасываем флаги, иначе любая мимолетная авария 
        // залипнет навсегда и насос не запустится
        data.faults = FAULT_NONE;

        data.freq_hz_x100 = freq;
        data.motor_current_ma = 5000 + (freq / 10); 
        
        if (freq > 4500) {
            data.motor_current_ma = 15000; // Эмуляция клина
            data.faults |= FAULT_OVERLOAD;
        }

        // Раз в 10 сек роняем связь на 2 сек (для проверки защиты)
        if ((cycle_count % 100) >= 80) {
            data.faults |= FAULT_WIRE_BREAK;
            data.current_ua = 2000;
            data.level_percent_x10 = -1;
        }

        if (ACTIVE_PROFILE == PROFILE_PUMP) {
            data.setpoint_x10 = 500; 
            
            if (freq > 0 && !(data.faults & FAULT_WIRE_BREAK)) {
                // Уровень растет/падает + немного шума
                level += ((freq - 2500) / 100) + ((sys_rand32_get() % 20) - 10);
            } else if (freq == 0) {
                level -= 5; 
            }
            
            if (level > 1000) level = 1000;
            if (level < 0) level = 0;
            
            if (!(data.faults & FAULT_WIRE_BREAK)) {
                data.level_percent_x10 = level;
                data.current_ua = 4000 + (level * 16); 
            }
            
            if (level < 100) data.faults |= FAULT_DRY_RUN; 
        }

        if (ACTIVE_PROFILE == PROFILE_AHU) {
            data.setpoint_x10 = 200; 
            data.temp_outdoor_x10 = -150 + (sys_rand32_get() % 100);
            
            if (cycle_count % 500 == 0) filter_dp += 10;
            data.filter_dp_pa = filter_dp;
            if (filter_dp > 250) data.faults |= FAULT_FILTER_DIRTY;

            data.temp_return_water_x10 = 500 + (freq / 10); 
            if (data.temp_return_water_x10 < 30) data.faults |= FAULT_FREEZE_WATER;

            temp_sup = 100 + (freq / 50) + ((sys_rand32_get() % 10) - 5);
            data.temp_supply_x10 = temp_sup;

            if (data.temp_outdoor_x10 < -100 && temp_sup < 50) {
                data.faults |= FAULT_FREEZE_AIR;
            }
        }

        // КОСТЫЛЬ: Раз в 5 секунд "оператор" чинит систему, 
        // чтобы симулятор не вставал на постоянку
        if (data.faults != FAULT_NONE && (cycle_count % 50) == 0) {
            if (ACTIVE_PROFILE == PROFILE_PUMP) level = 300; 
            if (ACTIVE_PROFILE == PROFILE_AHU) filter_dp = 50; 
            LOG_WRN("Демо: Авария сброшена");
        }

        k_msgq_put(&sensor_msgq, &data, K_FOREVER);
        k_sleep(K_MSEC(100)); 
    }
}

K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread, NULL, NULL, NULL, 5, 0, 0);