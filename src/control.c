#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

static void control_thread(void)
{
    struct sensor_data data;
    const int Kp = 50; 
    const int Ki = 5;  
    int integral = 0;
    int prev_error = 0;

    // НОВОЕ: Переменная текущей частоты для Soft Start
    int current_freq = 2500; 
    const int MAX_FREQ_STEP = 100; // Не более 1 Гц за цикл (100 мс)

    while (1) {
        if (k_msgq_get(&sensor_msgq, &data, K_FOREVER) == 0) {
            
            // Сброс ПИДа при смене профиля или режима входа
            if (data.profile_changed) {
                integral = 0;
                prev_error = 0;
                current_freq = 2500; 
            }

            if (data.faults != FAULT_NONE) {
                int stop_freq = 0;
                k_msgq_put(&modbus_tx_msgq, &stop_freq, K_NO_WAIT); 
                
                integral = 0; 
                prev_error = 0;
                current_freq = 0; // При аварии рубим резко!
                data.freq_hz_x100 = 0;
                k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
                continue; 
            }

            int measured_value = 0;
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
            int target_freq = 2500 + output; 

            if (target_freq > 5000) target_freq = 5000;
            if (target_freq < 0) target_freq = 0;

            // ЛОГИКА SOFT START
            int freq_diff = target_freq - current_freq;
            if (freq_diff > MAX_FREQ_STEP) {
                current_freq += MAX_FREQ_STEP;
            } else if (freq_diff < -MAX_FREQ_STEP) {
                current_freq -= MAX_FREQ_STEP;
            } else {
                current_freq = target_freq;
            }

            k_msgq_put(&modbus_tx_msgq, &current_freq, K_NO_WAIT);
            
            data.freq_hz_x100 = current_freq;
            k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(control_tid, 2048, control_thread, NULL, NULL, NULL, 6, 0, 0);