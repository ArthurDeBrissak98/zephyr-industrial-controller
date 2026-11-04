#ifndef CONTROLLER_MODBUS_H
#define CONTROLLER_MODBUS_H

#include <stdint.h>
#define CONTROLLER_MODBUS_REGISTER_COUNT 5

int controller_modbus_init(void);
int controller_modbus_error(void);
void controller_modbus_snapshot(uint16_t regs[CONTROLLER_MODBUS_REGISTER_COUNT]);
int controller_modbus_register_read(uint16_t address, uint16_t *value);
int controller_modbus_register_write(uint16_t address, uint16_t value);

#endif
