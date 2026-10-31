#ifndef SENSOR_H
#define SENSOR_H

#include <zephyr/kernel.h>

#define PROFILE_PUMP 0
#define PROFILE_AHU  1

// НОВОЕ: Режимы аналогового входа
#define INPUT_4_20MA 0
#define INPUT_0_10V  1

// Убрал дефайн, теперь профиль можно менять прям в терминале
extern volatile int current_profile; 
// Добавил режим входа, чтобы переключать ток/напряжение
extern volatile int input_mode; 

#define FAULT_NONE            0
#define FAULT_WIRE_BREAK      (1 << 0) 
#define FAULT_OVERLOAD        (1 << 1) 
#define FAULT_DRY_RUN         (1 << 2) 
#define FAULT_FREEZE_AIR      (1 << 3) 
#define FAULT_FILTER_DIRTY    (1 << 4) 
#define FAULT_FAN_FAIL        (1 << 5) 
#define FAULT_FREEZE_WATER    (1 << 6) 

struct sensor_data {
    int freq_hz_x100;        
    int setpoint_x10;        
    int motor_current_ma;    
    uint32_t faults;         

    // Насос
    int current_ua;          // Ток в мкА (если 4-20мА)
    int level_percent_x10;   

    // Приточка (AHU)
    int temp_supply_x10;     
    int temp_outdoor_x10;    
    int temp_return_water_x10; 
    int filter_dp_pa;        

    // НОВОЕ: Напряжение в мВ (если 0-10В)
    int voltage_mv;          

    // Флаг, чтобы ПИД сбрасывался при смене профиля/режима
    bool profile_changed; 
};

extern struct k_msgq sensor_msgq;
extern struct k_msgq monitor_msgq;
extern struct k_msgq modbus_tx_msgq;

void alert_isr(const void *arg);

#endif