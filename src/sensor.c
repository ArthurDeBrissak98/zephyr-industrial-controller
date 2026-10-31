#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

// Дефолтный профиль. Его меняет cli.c, когда мы нажимаем 1 или 2.
volatile int current_profile = PROFILE_PUMP;

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_data), 10, 4);
K_MSGQ_DEFINE(monitor_msgq, sizeof(struct sensor_data), 10, 4);

static atomic_t current_freq_hz_x100;
void update_pump_frequency(int freq) { atomic_set(&current_freq_hz_x100, freq); }

static void sensor_thread(void)
{
    struct sensor_data data = {0};
    int cycle_count = 0;
    int last_profile = -1; // Чтобы отследить, когда именно нажали кнопку переключения
    atomic_set(&current_freq_hz_x100, 2500);

    int level = 500;       
    int temp_sup = 150;    
    int filter_dp = 50;    

    // Убрал дефайн, теперь стартуем с того, что в current_profile
    LOG_INF("Старт профиля: %s", (current_profile == PROFILE_PUMP) ? "PUMP" : "AHU");

    while (1) {
    cycle_count++;
    int freq = atomic_get(&current_freq_hz_x100);

    if (cycle_count % 50 == 0) {
        LOG_INF("[sensor] iter=%d, profile=%d, freq=%d", 
                cycle_count, current_profile, freq);
    }
    
    data.faults = FAULT_NONE;

        // --- ОБРАБОТКА ПЕРЕКЛЮЧЕНИЯ ПРОФИЛЯ ---
        if (current_profile != last_profile) {
            LOG_WRN(">>> ПРОФИЛЬ СМЕНИЛСЯ НА: %s <<<", 
                    (current_profile == PROFILE_PUMP) ? "PUMP" : "AHU");
            last_profile = current_profile;
            
            // Говорим ПИДу, что надо сбросить интеграл
            data.profile_changed = true;
            
            // Сбрасываем физику, а то бак с водой вдруг стал приточной установкой
            level = 500; 
            filter_dp = 50;
            
            // ХЗ зачем, но без очистки очередей ПИД хватал старые данные 
            // и дико глючил. Сеньор сказал "очищай", я очистил. 
            k_msgq_purge(&sensor_msgq);
            k_msgq_purge(&monitor_msgq);
        } else {
            // Если не переключались, то флаг ставим в false, чтобы ПИД не сбрасывался каждый цикл
            data.profile_changed = false;
        }

        data.freq_hz_x100 = freq;
        data.motor_current_ma = (freq * freq) / 7000;  
        
        // Если частота слишком высокая - эмулируем клин и аварию
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

        if (current_profile == PROFILE_PUMP) {
            data.setpoint_x10 = 500; // Уставка 50.0%
            
            if (freq > 0 && !(data.faults & FAULT_WIRE_BREAK)) {
                // Уровень растет/падает + немного шума
                level += ((freq - 2500) / 100) + ((sys_rand32_get() % 20) - 10);
            } else if (freq == 0) {
                level -= 5; // Если насос стоит - вода убывает
            }
            
            if (level > 1000) level = 1000;
            if (level < 0) level = 0;
            
            if (!(data.faults & FAULT_WIRE_BREAK)) {
                data.level_percent_x10 = level;
                data.current_ua = 4000 + (level * 16); 
            }
            
            if (level < 100) data.faults |= FAULT_DRY_RUN; 
        }

        if (current_profile == PROFILE_AHU) {
            data.setpoint_x10 = 200; // Уставка 20.0 C
            
            data.temp_outdoor_x10 = -150 + (sys_rand32_get() % 100);
            
            if (cycle_count % 500 == 0) filter_dp += 10;
            data.filter_dp_pa = filter_dp;
            if (filter_dp > 250) data.faults |= FAULT_FILTER_DIRTY;

            data.temp_return_water_x10 = 500 + (freq / 10); 
            if (data.temp_return_water_x10 < 30) data.faults |= FAULT_FREEZE_WATER;

            // ФИКС: Поменял формулу, чтобы при 25 Гц (2500) температура была ровно 20.0 C.
            // Иначе ПИД разгонял частоту до >4500 и ловил перегрузку!
            temp_sup = 200 + ((freq - 2500) / 100) + ((sys_rand32_get() % 10) - 5);
            data.temp_supply_x10 = temp_sup;

            if (data.temp_outdoor_x10 < -100 && temp_sup < 50) {
                data.faults |= FAULT_FREEZE_AIR;
            }
        }

        // КОСТЫЛЬ: Раз в 5 секунд "оператор" чинит систему, 
        // чтобы симулятор не вставал на постоянку
        if (data.faults != FAULT_NONE && (cycle_count % 50) == 0) {
            if (current_profile == PROFILE_PUMP) level = 300; 
            if (current_profile == PROFILE_AHU) filter_dp = 50; 
            LOG_WRN("Демо: Авария сброшена");
        }

        k_msgq_put(&sensor_msgq, &data, K_FOREVER);
        k_sleep(K_MSEC(100)); 
    }
}

K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread, NULL, NULL, NULL, 5, 0, 0);