#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

// Внешний массив из modbus_tcp.c, куда мы будем складывать данные для SCADA
extern uint16_t holding_regs[]; 

// Дефолтный профиль и режим. Их меняет shell.c через команды set profile/set mode
volatile int current_profile = PROFILE_PUMP;
volatile int input_mode = INPUT_4_20MA; 

K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_data), 10, 4);
K_MSGQ_DEFINE(monitor_msgq, sizeof(struct sensor_data), 10, 4);

static atomic_t current_freq_hz_x100;
void update_pump_frequency(int freq) { atomic_set(&current_freq_hz_x100, freq); }

static void sensor_thread(void)
{
    struct sensor_data data = {0};
    int cycle_count = 0;
    int last_profile = -1; 
    int last_input_mode = -1;
    atomic_set(&current_freq_hz_x100, 2500);

    int level = 500;       
    int temp_sup = 200;    
    int filter_dp = 50;    

    LOG_INF("Старт профиля: %s, режим входа: %s", 
            (current_profile == PROFILE_PUMP) ? "PUMP" : "AHU",
            (input_mode == INPUT_4_20MA) ? "4-20mA" : "0-10V");

    while (1) {
        cycle_count++;
        int freq = atomic_get(&current_freq_hz_x100);
        
        // ОБЯЗАТЕЛЬНО сбрасываем флаги каждый цикл
        data.faults = FAULT_NONE;
        data.profile_changed = false;

        // --- ОБРАБОТКА ПЕРЕКЛЮЧЕНИЯ ПРОФИЛЯ ---
        if (current_profile != last_profile) {
            LOG_WRN(">>> ПРОФИЛЬ СМЕНИЛСЯ НА: %s <<<", 
                    (current_profile == PROFILE_PUMP) ? "PUMP" : "AHU");
            last_profile = current_profile;
            data.profile_changed = true;
            level = 500; 
            filter_dp = 50;
            k_msgq_purge(&sensor_msgq);
            k_msgq_purge(&monitor_msgq);
            k_msgq_put(&monitor_msgq, &data, K_NO_WAIT); // Будим монитор
        }

        // --- ОБРАБОТКА ПЕРЕКЛЮЧЕНИЯ РЕЖИМА ВХОДА (ток/напряжение) ---
        if (input_mode != last_input_mode) {
            LOG_WRN(">>> РЕЖИМ ВХОДА СМЕНИЛСЯ НА: %s <<<", 
                    (input_mode == INPUT_4_20MA) ? "4-20mA" : "0-10V");
            last_input_mode = input_mode;
            data.profile_changed = true; // Используем тот же флаг для сброса ПИДа
            level = 500;
            filter_dp = 50;
            k_msgq_purge(&sensor_msgq);
            k_msgq_purge(&monitor_msgq);
            k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
        }

        data.freq_hz_x100 = freq;
        
        // Ток двигателя: квадратичная зависимость, чтобы не было 5А на холостом ходу
        data.motor_current_ma = (freq * freq) / 7000; 
        
        if (freq > 4500) {
            data.motor_current_ma = 8000; // Эмуляция клина
            data.faults |= FAULT_OVERLOAD;
        }

        // Раз в 10 сек роняем связь на 2 сек (для проверки защиты)
        if ((cycle_count % 100) >= 80) {
            data.faults |= FAULT_WIRE_BREAK;
            data.current_ua = 2000;
            data.level_percent_x10 = -1;
        }

        // --- ЛОГИКА НАСОСА ---
        if (current_profile == PROFILE_PUMP) {
            data.setpoint_x10 = 500; 
            
            if (freq > 0 && !(data.faults & FAULT_WIRE_BREAK)) {
                // Уменьшил шум и коэффициент, чтобы ПИД успевал реагировать
                level += ((freq - 2500) / 200) + ((sys_rand32_get() % 11) - 5);
            } else if (freq == 0) {
                level -= 2; // Уровень падает медленнее, когда насос стоит
            }
            
            if (level > 1000) level = 1000;
            if (level < 0) level = 0;
            
            if (!(data.faults & FAULT_WIRE_BREAK)) {
                data.level_percent_x10 = level;
                // Выбираем, что выдавать: ток или напряжение
                if (input_mode == INPUT_4_20MA) {
                    data.current_ua = 4000 + (level * 16);
                    data.voltage_mv = -1; 
                } else {
                    data.voltage_mv = level * 10; 
                    data.current_ua = -1;
                }
            }
            
            // Сухой ход теперь при уровне ниже 5% (было 10% и система залипала)
            if (level < 50) data.faults |= FAULT_DRY_RUN; 
        }

        // --- ЛОГИКА ПРИТОЧКИ (AHU) ---
        if (current_profile == PROFILE_AHU) {
            data.setpoint_x10 = 200; 
            data.temp_outdoor_x10 = -150 + (sys_rand32_get() % 100);
            
            if (cycle_count % 500 == 0) filter_dp += 10;
            data.filter_dp_pa = filter_dp;
            if (filter_dp > 250) data.faults |= FAULT_FILTER_DIRTY;

            data.temp_return_water_x10 = 500 + (freq / 10); 
            if (data.temp_return_water_x10 < 30) data.faults |= FAULT_FREEZE_WATER;

            // Формула температуры приточки
            temp_sup = 200 + ((freq - 2500) / 100) + ((sys_rand32_get() % 10) - 5);
            data.temp_supply_x10 = temp_sup;

            // Пересчитываем в проценты для аналогового сигнала
            int percent = (temp_sup + 200) * 100 / 600; 
            if (input_mode == INPUT_4_20MA) {
                data.current_ua = 4000 + (percent * 160);
                data.voltage_mv = -1;
            } else {
                data.voltage_mv = percent * 100;
                data.current_ua = -1;
            }

            if (data.temp_outdoor_x10 < -100 && temp_sup < 50) {
                data.faults |= FAULT_FREEZE_AIR;
            }
        }

        // КОСТЫЛЬ: автосброс аварии раз в 5 секунд
        if (data.faults != FAULT_NONE && (cycle_count % 50) == 0) {
            if (current_profile == PROFILE_PUMP) level = 300; 
            if (current_profile == PROFILE_AHU) filter_dp = 50; 
            LOG_WRN("Демо: Авария сброшена");
        }

        // --- ОБНОВЛЯЕМ РЕГИСТРЫ ДЛЯ MODBUS TCP ---
        // SCADA будет читать эти значения (заглушка, так как TCP нет в Zephyr 4.5.0)
        holding_regs[0] = (uint16_t)data.level_percent_x10;
        holding_regs[1] = (uint16_t)data.temp_supply_x10;
        holding_regs[2] = (uint16_t)data.temp_outdoor_x10;
        holding_regs[3] = (uint16_t)data.freq_hz_x100;
        holding_regs[4] = (uint16_t)(data.faults & 0xFFFF);

        k_msgq_put(&sensor_msgq, &data, K_FOREVER);
        k_sleep(K_MSEC(100)); 
    }
}

K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread, NULL, NULL, NULL, 5, 0, 0);