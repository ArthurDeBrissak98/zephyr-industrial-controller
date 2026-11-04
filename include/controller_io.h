#ifndef CONTROLLER_IO_H
#define CONTROLLER_IO_H

#include "controller.h"

// Для платы эти функции реализуются через ADC и интерфейс привода.
void controller_io_read(const struct controller_config *config, struct sensor_sample *sample);
void controller_io_write(int frequency_x100);
int controller_io_inject_wire_break(bool enabled);
int controller_io_inject_signal(bool enabled, int value);
int controller_io_output(void);

#endif
