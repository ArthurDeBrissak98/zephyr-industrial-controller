#include "controller_settings.h"
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <errno.h>

#define SETTINGS_MAGIC 0x5a494331U
#define SETTINGS_VERSION 1U

BUILD_ASSERT(CONTROLLER_SETTINGS_SIZE == (9 + 4 * PROTECTION_COUNT) * 4,
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
    put_word(&cursor, crc32_ieee(data, CONTROLLER_SETTINGS_SIZE - 4));
    return 0;
}

// REQ-NVS-01
int controller_settings_decode(const uint8_t *data, size_t size,
                                struct controller_parameters *parameters)
{
    if (size != CONTROLLER_SETTINGS_SIZE) return -EINVAL;
    if (sys_get_le32(data) != SETTINGS_MAGIC) return -EBADMSG;
    if (sys_get_le32(data + 4) != SETTINGS_VERSION) return -ENOTSUP;
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
    int ret = controller_parameters_validate(&decoded);
    if (!ret) *parameters = decoded;
    return ret;
}
