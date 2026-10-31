#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modbus_tcp_server, LOG_LEVEL_INF);

/* Индекс интерфейса (0 = первый Modbus-интерфейс) */
#define MODBUS_IFACE 0

/* Карта регистров */
enum {
    REG_LEVEL_X10 = 0,
    REG_TEMP_SUPPLY_X10,
    REG_TEMP_OUTDOOR_X10,
    REG_FREQ_HZ_X100,
    REG_FAULTS,
    REG_COUNT
};

/* Буфер holding-регистров (big-endian, как в Modbus) */
static uint16_t holding_regs[REG_COUNT];

/* --- Функции чтения/записи для сервера --- */

static int holding_reg_read(uint16_t reg, uint16_t *buf, uint16_t len)
{
    if (reg + len > REG_COUNT) {
        return -EINVAL;
    }
    for (int i = 0; i < len; i++) {
        buf[i] = holding_regs[reg + i];
    }
    return 0;
}

static int holding_reg_write(uint16_t reg, uint16_t *buf, uint16_t len)
{
    if (reg + len > REG_COUNT) {
        return -EINVAL;
    }
    for (int i = 0; i < len; i++) {
        holding_regs[reg + i] = buf[i];
    }
    return 0;
}

/* --- Инициализация Modbus TCP сервера --- */

static void modbus_tcp_server_thread(void)
{
    struct modbus_iface_param param = {
        .mode = MODBUS_MODE_TCP,
        .server = {
            .read_holding_reg = holding_reg_read,
            .write_holding_reg = holding_reg_write,
        },
    };

    int err = modbus_init_server(MODBUS_IFACE, param);
    if (err < 0) {
        LOG_ERR("Modbus TCP server init failed: %d", err);
        return;
    }

    LOG_INF("Modbus TCP server running on port 502");

    while (1) {
        /* Здесь можно обновлять holding_regs из sensor_data */
        k_sleep(K_SECONDS(1));
    }
}

K_THREAD_DEFINE(modbus_tcp_tid, 4096, modbus_tcp_server_thread, NULL, NULL, NULL, 8, 0, 0);