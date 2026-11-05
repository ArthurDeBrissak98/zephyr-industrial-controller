#ifndef CONTROLLER_IO_H
#define CONTROLLER_IO_H

#include "controller.h"

// Чтение — только из потока. 0 — исправный вход; -EIO — сбой или диапазон,
// -ETIMEDOUT — ADC не завершил преобразование, -ENOTSUP — неподдерживаемый режим.
// При сбое получения отсчёта sample->read_error != 0. При ошибке диапазона
// физическое значение сохраняется, чтобы защиты различали обрыв и перегрузку.
int controller_io_read(const struct controller_config *config, struct sensor_sample *sample);
void controller_io_write(int frequency_x100);
int controller_io_inject_wire_break(bool enabled);
int controller_io_inject_signal(bool enabled, int value);
int controller_io_output(void);
int controller_io_inject_motor(enum motor_feedback_channel channel, bool enabled, int value);

#endif
