#ifndef CONTROLLER_SETTINGS_H
#define CONTROLLER_SETTINGS_H

#include "controller.h"

#define CONTROLLER_SETTINGS_LEGACY_SIZE 100
#define CONTROLLER_SETTINGS_SIZE 120

int controller_settings_encode(const struct controller_parameters *parameters,
                                uint8_t data[CONTROLLER_SETTINGS_SIZE]);
int controller_settings_decode(const uint8_t *data, size_t size,
                                struct controller_parameters *parameters);
int controller_settings_init(void);
int controller_settings_read(struct controller_parameters *parameters);
int controller_settings_write(const struct controller_parameters *parameters);
int controller_settings_erase(void);

#endif
