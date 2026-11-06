#include "controller_settings.h"
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <errno.h>

#define SETTINGS_MAGIC 0x5a494331U
#define SETTINGS_VERSION 3U

BUILD_ASSERT(CONTROLLER_SETTINGS_SIZE == (15 + 4 * PROTECTION_COUNT) * 4,
             "Update the settings format size and version");

static void put_word(uint8_t **cursor, uint32_t value)
{
    sys_put_le32(value, *cursor);
    *cursor += 4;
}

static uint32_t get_word(const uint8_t **cursor)
{
    uint32_t value = sys_get_le32(*cursor);
    *cursor += 4;
    return value;
}

int controller_settings_encode(const struct controller_parameters *parameters,
                                uint8_t data[CONTROLLER_SETTINGS_SIZE])
{
    int ret = controller_parameters_validate(parameters);
    if (ret) return ret;
    uint8_t *cursor = data;
    const struct controller_config *config = &parameters->config;
    put_word(&cursor, SETTINGS_MAGIC);
    put_word(&cursor, SETTINGS_VERSION);
    put_word(&cursor, config->profile);
    put_word(&cursor, config->mode);
    put_word(&cursor, config->pid.bias_x100);
    put_word(&cursor, config->pid.kp);
    put_word(&cursor, config->pid.ki);
    put_word(&cursor, config->pid.divisor);
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        const struct protection_config *p = &parameters->protections[i];
        put_word(&cursor, p->threshold_x10);
        put_word(&cursor, p->hysteresis_x10);
        put_word(&cursor, p->delay_ms);
        put_word(&cursor, p->enabled ? 1 : 0);
    }
    put_word(&cursor, config->motor.nominal_voltage_v);
    put_word(&cursor, config->motor.nominal_current_ma);
    put_word(&cursor, config->motor.nominal_frequency_x100);
    put_word(&cursor, config->motor.nominal_speed_rpm);
    put_word(&cursor, config->motor.pole_pairs);
    put_word(&cursor, config->motor.type);
    put_word(&cursor, crc32_ieee(data, CONTROLLER_SETTINGS_SIZE - 4));
    return 0;
}

// REQ-NVS-01
int controller_settings_decode(const uint8_t *data, size_t size,
                                struct controller_parameters *parameters)
{
    if (size != CONTROLLER_SETTINGS_SIZE && size != CONTROLLER_SETTINGS_V2_SIZE &&
        size != CONTROLLER_SETTINGS_LEGACY_SIZE) return -EINVAL;
    if (sys_get_le32(data) != SETTINGS_MAGIC) return -EBADMSG;
    uint32_t version = sys_get_le32(data + 4);
    if (version != 1 && version != 2 && version != SETTINGS_VERSION) return -ENOTSUP;
    if (size != (version == 1 ? CONTROLLER_SETTINGS_LEGACY_SIZE :
                 version == 2 ? CONTROLLER_SETTINGS_V2_SIZE : CONTROLLER_SETTINGS_SIZE)) return -EINVAL;
    if (crc32_ieee(data, size - 4) != sys_get_le32(data + size - 4)) return -EBADMSG;
    const uint8_t *cursor = data + 8;
    struct controller_parameters decoded = {0};
    decoded.config.profile = get_word(&cursor);
    decoded.config.mode = get_word(&cursor);
    decoded.config.pid.bias_x100 = (int32_t)get_word(&cursor);
    decoded.config.pid.kp = (int32_t)get_word(&cursor);
    decoded.config.pid.ki = (int32_t)get_word(&cursor);
    decoded.config.pid.divisor = (int32_t)get_word(&cursor);
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        struct protection_config *p = &decoded.protections[i];
        p->threshold_x10 = (int32_t)get_word(&cursor);
        p->hysteresis_x10 = (int32_t)get_word(&cursor);
        p->delay_ms = (int32_t)get_word(&cursor);
        uint32_t enabled = get_word(&cursor);
        if (enabled > 1) return -EINVAL;
        p->enabled = enabled != 0;
    }
    if (version == 1) {
        decoded.config.motor = controller_motor_defaults();
    } else {
        decoded.config.motor.nominal_voltage_v = (int32_t)get_word(&cursor);
        decoded.config.motor.nominal_current_ma = (int32_t)get_word(&cursor);
        decoded.config.motor.nominal_frequency_x100 = (int32_t)get_word(&cursor);
        decoded.config.motor.nominal_speed_rpm = (int32_t)get_word(&cursor);
        decoded.config.motor.pole_pairs = (int32_t)get_word(&cursor);
    }
    decoded.config.motor.type = version == 3 ? get_word(&cursor) : MOTOR_INDUCTION;
    int ret = controller_parameters_validate(&decoded);
    if (!ret) *parameters = decoded;
    return ret;
}
