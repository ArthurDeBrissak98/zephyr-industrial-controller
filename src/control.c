#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

static void control_thread(void)
{
    struct sensor_data data;
    // Коэффы подобраны "на глаз" для симулятора. 
    // TODO: потом надо будет нормально посчитать, а не просто тыкать пальцем в небо
    const int Kp = 50; 
    const int Ki = 5;  
    int integral = 0;
    int prev_error = 0;

    while (1) {
        if (k_msgq_get(&sensor_msgq, &data, K_FOREVER) == 0) {

            static int dbg_count = 0;
        if (++dbg_count % 50 == 0) {
            LOG_INF("[control] got data, setpoint=%d, faults=%d", 
                    data.setpoint_x10, data.faults);
        }
            
            // Если профиль сменили через терминал, ПИД надо сбросывать
            // и насос (или приточка) тупят при переключении.
            if (data.profile_changed) {
                integral = 0;
                prev_error = 0;
            }

            // Если сработала любая защита - рубим частоту в ноль
            if (data.faults != FAULT_NONE) {
                int stop_freq = 0;
                k_msgq_put(&modbus_tx_msgq, &stop_freq, K_NO_WAIT); // Отправляем 0 Гц в Modbus-поток
                
                integral = 0; // Anti-windup: сбрасываем интеграл, чтобы не было выброса при восстановлении
                prev_error = 0;
                data.freq_hz_x100 = 0;
                k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
                continue; 
            }

            int measured_value = 0;
            // Проверяем активный профиль
            if (current_profile == PROFILE_PUMP) {
                measured_value = data.level_percent_x10;
            } else if (current_profile == PROFILE_AHU) {
                measured_value = data.temp_supply_x10;
            }

            int error = data.setpoint_x10 - measured_value;
            integral += error;
            if (integral > 100000) integral = 100000;
            if (integral < -100000) integral = -100000;

            int output = ((Kp * error) + (Ki * integral)) / 10; 
            
            // 2500 = 25 Гц (середина диапазона)
            int target_freq = 2500 + output; 

            if (target_freq > 5000) target_freq = 5000;
            if (target_freq < 0) target_freq = 0;

            // Отправляем результат в отдельный поток Modbus
            // K_NO_WAIT, чтобы ПИД не залипал, если очередь вдруг полная
            k_msgq_put(&modbus_tx_msgq, &target_freq, K_NO_WAIT);
            
            data.freq_hz_x100 = target_freq;
            k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(control_tid, 2048, control_thread, NULL, NULL, NULL, 6, 0, 0);