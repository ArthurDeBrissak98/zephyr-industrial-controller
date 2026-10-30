#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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

            if (data.faults != FAULT_NONE) {
                if (data.faults & FAULT_WIRE_BREAK)   LOG_ERR("АВАРИЯ: Обрыв 4-20мА");
                if (data.faults & FAULT_OVERLOAD)     LOG_ERR("АВАРИЯ: Перегрузка");
                if (data.faults & FAULT_DRY_RUN)      LOG_ERR("АВАРИЯ: Сухой ход!");
                if (data.faults & FAULT_FREEZE_AIR)   LOG_ERR("АВАРИЯ: Замораживание по воздуху!");
                if (data.faults & FAULT_FILTER_DIRTY) LOG_ERR("АВАРИЯ: Забит фильтр!");
                if (data.faults & FAULT_FREEZE_WATER) LOG_ERR("АВАРИЯ: Замораживание теплоносителя!");
                if (data.faults & FAULT_FAN_FAIL)     LOG_ERR("АВАРИЯ: Вентилятор не крутится!");
                LOG_WRN("E-Stop активен");
            } else {
                if (ACTIVE_PROFILE == PROFILE_PUMP) {
                    LOG_INF("Уровень: %d.%d %% | Частота: %d.%02d Гц | Ток: %d мА", 
                            data.level_percent_x10 / 10, data.level_percent_x10 % 10,
                            data.freq_hz_x100 / 100, data.freq_hz_x100 % 100,
                            data.motor_current_ma);
                } else if (ACTIVE_PROFILE == PROFILE_AHU) {
                    LOG_INF("Приток: %d.%d C | Улица: %d.%d C | Теплоноситель: %d.%d C",
                            data.temp_supply_x10 / 10, data.temp_supply_x10 % 10,
                            data.temp_outdoor_x10 / 10, data.temp_outdoor_x10 % 10,
                            data.temp_return_water_x10 / 10, data.temp_return_water_x10 % 10);
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