#ifndef CONTROLLER_MODBUS_H
#define CONTROLLER_MODBUS_H

#include "controller.h"
#include <stdint.h>
#include <stddef.h>
// Размер прежнего блока состояния 0–4; моторный блок имеет отдельные адреса.
#define CONTROLLER_MODBUS_REGISTER_COUNT 5
#define CONTROLLER_MODBUS_MOTOR_FIRST 10
#define CONTROLLER_MODBUS_MOTOR_COUNT 5

struct modbus_snapshot {
    uint16_t state;
    uint16_t faults;
    uint16_t warnings;
    int16_t measured_x10;
    int16_t setpoint_x10;
    struct motor_parameters motor;
};

uint8_t controller_modbus_fc03(const uint8_t *request, size_t size,
                               uint8_t *response, size_t capacity, size_t *length);
uint8_t controller_modbus_fc16(const uint8_t *request, size_t size,
                               uint8_t *response, size_t capacity, size_t *length);
int controller_modbus_init(void);
int controller_modbus_error(void);
void controller_modbus_snapshot(uint16_t regs[CONTROLLER_MODBUS_REGISTER_COUNT]);
int controller_modbus_register_read(uint16_t address, uint16_t *value);
int controller_modbus_register_write(uint16_t address, uint16_t value);

#endif
