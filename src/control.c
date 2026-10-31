#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

// Чистая функция ПИД-регулятора. Вынесена отдельно, чтобы её можно было тестировать.
int calculate_pid(struct pid_state *state, int setpoint, int measured, int max_step)
{
    const int Kp = 50; 
    const int Ki = 5;  

    int error = setpoint - measured;
    
    // Anti-windup
    state->integral += error;
    if (state->integral > 100000) state->integral = 100000;
    if (state->integral < -100000) state->integral = -100000;

    int output = ((Kp * error) + (Ki * state->integral)) / 10; 
    int target_freq = 2500 + output; 

    // Ограничиваем частоту 0-50 Гц
    if (target_freq > 5000) target_freq = 5000;
    if (target_freq < 0) target_freq = 0;

    // Логика Soft Start (Ограничение шага)
    int freq_diff = target_freq - state->current_freq;
    if (freq_diff > max_step) {
        state->current_freq += max_step;
    } else if (freq_diff < -max_step) {
        state->current_freq -= max_step;
    } else {
        state->current_freq = target_freq;
    }

    state->prev_error = error;
    return state->current_freq;
}

static void control_thread(void)
{
    struct sensor_data data;
    
    // Инициализируем состояние ПИД-регулятора
    struct pid_state pid = { 
        .integral = 0, 
        .prev_error = 0, 
        .current_freq = 2500 
    };
    const int MAX_FREQ_STEP = 100; 

    while (1) {
        if (k_msgq_get(&sensor_msgq, &data, K_FOREVER) == 0) {
            
            // Сброс ПИДа при смене профиля или режима входа
            if (data.profile_changed) {
                pid.integral = 0;
                pid.prev_error = 0;
                pid.current_freq = 2500; 
            }

            // Обработка аварии
            if (data.faults != FAULT_NONE) {
                int stop_freq = 0;
                k_msgq_put(&modbus_tx_msgq, &stop_freq, K_NO_WAIT); 
                
                pid.integral = 0; 
                pid.prev_error = 0;
                pid.current_freq = 0; // При аварии рубим резко!
                
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

            // Вызываем нашу вынесенную функцию
            int target_freq = calculate_pid(&pid, data.setpoint_x10, measured_value, MAX_FREQ_STEP);

            // Отправляем частоту в Modbus
            k_msgq_put(&modbus_tx_msgq, &target_freq, K_NO_WAIT);
            
            // Обновляем данные для монитора
            data.freq_hz_x100 = target_freq;
            k_msgq_put(&monitor_msgq, &data, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(control_tid, 2048, control_thread, NULL, NULL, NULL, 6, 0, 0);