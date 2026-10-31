#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>  // Для abs()

LOG_MODULE_REGISTER(monitor, LOG_LEVEL_INF);

void alert_isr(const void *arg) {
    ARG_UNUSED(arg);
    LOG_ERR(">>> [ISR] АВАРИЯ! <<<");
}

static void monitor_thread(void)
{
    struct sensor_data data;

    while (1) {
        int ret = k_msgq_get(&monitor_msgq, &data, K_MSEC(2500));

        if (ret == 0) {
            // Выгребаем очередь, чтобы получить самые свежие данные. 
            // Пока монитор спит, датчик успевает накидать кучу сообщений.
            struct sensor_data latest;
            while (k_msgq_get(&monitor_msgq, &latest, K_NO_WAIT) == 0) {
                data = latest;
            }

            // Если профиль переключили, выводим сообщение, чтобы было понятно, что мы перестроились
            if (data.profile_changed) {
                LOG_INF("--- МОНИТОР: ПРОФИЛЬ УСПЕШНО ПЕРЕКЛЮЧЕН ---");
            }

            if (data.faults != FAULT_NONE) {
                LOG_ERR("--- Профиль: %s ---", (current_profile == PROFILE_PUMP) ? "НАСОС" : "ПРИТОЧКА");
                if (data.faults & FAULT_WIRE_BREAK)   LOG_ERR("АВАРИЯ: Обрыв 4-20мА");
                if (data.faults & FAULT_OVERLOAD)     LOG_ERR("АВАРИЯ: Перегрузка");
                if (data.faults & FAULT_DRY_RUN)      LOG_ERR("АВАРИЯ: Сухой ход!");
                if (data.faults & FAULT_FREEZE_AIR)   LOG_ERR("АВАРИЯ: Замораживание по воздуху!");
                if (data.faults & FAULT_FILTER_DIRTY) LOG_ERR("АВАРИЯ: Забит фильтр!");
                if (data.faults & FAULT_FREEZE_WATER) LOG_ERR("АВАРИЯ: Замораживание теплоносителя!");
                if (data.faults & FAULT_FAN_FAIL)     LOG_ERR("АВАРИЯ: Вентилятор не крутится!");
                LOG_WRN("E-Stop активен");
            } else {
                LOG_INF("--- Профиль: %s ---", (current_profile == PROFILE_PUMP) ? "НАСОС" : "ПРИТОЧКА");
                if (current_profile == PROFILE_PUMP) {
                    LOG_INF("Уровень: %d.%d %% | Частота: %d.%02d Гц | Ток: %d.%02d А", 
                        data.level_percent_x10 / 10, data.level_percent_x10 % 10,
                        data.freq_hz_x100 / 100, data.freq_hz_x100 % 100,
                        data.motor_current_ma / 1000, (data.motor_current_ma % 1000) / 10);
                } else if (current_profile == PROFILE_AHU) {
                    // Остаток от деления тоже отрицательный, поэтому выводилось что-то типа -11.-6. Взял модуль для дробной части.
                    int sup_w = data.temp_supply_x10 / 10;
                    int sup_f = abs(data.temp_supply_x10 % 10);
                    int out_w = data.temp_outdoor_x10 / 10;
                    int out_f = abs(data.temp_outdoor_x10 % 10);
                    int ret_w = data.temp_return_water_x10 / 10;
                    int ret_f = abs(data.temp_return_water_x10 % 10);
                    
                    LOG_INF("Приток: %d.%d C | Улица: %d.%d C | Теплоноситель: %d.%d C",
                            sup_w, sup_f, out_w, out_f, ret_w, ret_f);
                    LOG_INF("Фильтр: %d Па | Частота: %d.%02d Гц", 
                            data.filter_dp_pa,
                            data.freq_hz_x100 / 100, data.freq_hz_x100 % 100);
                }
            }
        }
        
        k_sleep(K_SECONDS(2)); 
    }
}

K_THREAD_DEFINE(monitor_tid, 2048, monitor_thread, NULL, NULL, NULL, 7, 0, 0);