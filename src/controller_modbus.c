#include "controller_modbus.h"
#include "controller_app.h"
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <zephyr/sys/byteorder.h>
#ifdef CONFIG_APP_MODBUS
#include <zephyr/modbus/modbus.h>
#endif

static atomic_t interface_error = ATOMIC_INIT(-ENOTSUP);

static void take_snapshot(struct modbus_snapshot *snapshot)
{
    // Весь status копируется за один захват мьютекса контроллера.
    struct controller_status status;
    controller_app_status(&status);
    *snapshot = (struct modbus_snapshot) {
        .state = status.state, .faults = status.active_faults,
        .warnings = status.active_warnings,
        .measured_x10 = status.sample.measured_x10,
        .setpoint_x10 = status.sample.setpoint_x10,
    };
}

static uint16_t snapshot_register(const struct modbus_snapshot *snapshot, unsigned int address)
{
    switch (address) {
    case 0: return snapshot->state;
    case 1: return snapshot->faults;
    case 2: return snapshot->warnings;
    case 3: return (uint16_t)snapshot->measured_x10;
    default: return (uint16_t)snapshot->setpoint_x10;
    }
}

void controller_modbus_snapshot(uint16_t regs[CONTROLLER_MODBUS_REGISTER_COUNT])
{
    struct modbus_snapshot snapshot;
    take_snapshot(&snapshot);
    for (unsigned int i = 0; i < CONTROLLER_MODBUS_REGISTER_COUNT; i++) {
        regs[i] = snapshot_register(&snapshot, i);
    }
}

uint8_t controller_modbus_fc03(const uint8_t *request, size_t size,
                               uint8_t *response, size_t capacity, size_t *length)
{
    *length = 0;
    if (size != 4) return 3;
    uint16_t address = sys_get_be16(request);
    uint16_t count = sys_get_be16(request + 2);
    if (!count || count > 125) return 3;
    if (address >= CONTROLLER_MODBUS_REGISTER_COUNT ||
        count > CONTROLLER_MODBUS_REGISTER_COUNT - address) return 2;
    if (capacity < 1U + count * 2U) return 4;
    struct modbus_snapshot snapshot;
    take_snapshot(&snapshot);
    response[0] = count * 2;
    for (unsigned int i = 0; i < count; i++) {
        sys_put_be16(snapshot_register(&snapshot, address + i), response + 1 + i * 2);
    }
    *length = 1U + count * 2U;
    return 0;
}

uint8_t controller_modbus_fc16(const uint8_t *request, size_t size,
                               uint8_t *response, size_t capacity, size_t *length)
{
    *length = 0;
    if (size < 5) return 3;
    uint16_t address = sys_get_be16(request);
    uint16_t count = sys_get_be16(request + 2);
    if (!count || count > 123 || request[4] != count * 2U || size != 5U + count * 2U) return 3;
    if (count > CONTROLLER_MODBUS_REGISTER_COUNT) return 2;
    if (capacity < 4) return 4;
    // Фаза 1: распаковать и проверить весь пакет, не меняя состояние.
    uint16_t values[CONTROLLER_MODBUS_REGISTER_COUNT];
    for (unsigned int i = 0; i < count; i++) values[i] = sys_get_be16(request + 5 + i * 2);
    for (unsigned int i = 0; i < count; i++) {
        if ((uint32_t)address + i != 4) return 2;
    }
    // Фаза 2: setter под мьютексом проверяет текущий профиль и состояние,
    // затем применяет уставку. Это исключает смену профиля между проверкой и записью.
    if (controller_app_setpoint_set((int16_t)values[0])) return 2;
    sys_put_be16(address, response);
    sys_put_be16(count, response + 2);
    *length = 4;
    return 0;
}

int controller_modbus_register_read(uint16_t address, uint16_t *value)
{
    if (address >= CONTROLLER_MODBUS_REGISTER_COUNT) return -ENOTSUP;
    uint16_t regs[CONTROLLER_MODBUS_REGISTER_COUNT];
    controller_modbus_snapshot(regs);
    *value = regs[address];
    return 0;
}

int controller_modbus_register_write(uint16_t address, uint16_t value)
{
    if (address != 4) return -ENOTSUP;
    return controller_app_setpoint_set((int16_t)value);
}

int controller_modbus_error(void)
{
    return atomic_get(&interface_error);
}

#ifdef CONFIG_APP_MODBUS
static bool frame_callback(const int iface, const struct modbus_adu *const request,
                            struct modbus_adu *const response, uint8_t *const exception,
                            void *const user_data)
{
    ARG_UNUSED(iface);
    ARG_UNUSED(user_data);
    size_t length = 0;
    *exception = request->fc == 3 ?
        controller_modbus_fc03(request->data, request->length, response->data, sizeof(response->data), &length) :
        controller_modbus_fc16(request->data, request->length, response->data, sizeof(response->data), &length);
    response->length = length;
    return true;
}
MODBUS_CUSTOM_FC_DEFINE(snapshot_read, frame_callback, 3, NULL);
MODBUS_CUSTOM_FC_DEFINE(atomic_write, frame_callback, 16, NULL);
#endif

int controller_modbus_init(void)
{
    int ret = -ENOTSUP;
#ifdef CONFIG_APP_MODBUS
    static struct modbus_user_callbacks callbacks = {
        .holding_reg_rd = controller_modbus_register_read,
        .holding_reg_wr = controller_modbus_register_write,
    };
    const struct modbus_iface_param parameters = {
        .mode = MODBUS_MODE_RTU,
        .server = { .unit_id = 1, .user_cb = &callbacks },
        .serial = { .baud = 19200, .parity = UART_CFG_PARITY_NONE,
                    .stop_bits = UART_CFG_STOP_BITS_2 },
    };
    int iface = modbus_iface_get_by_name(DEVICE_DT_NAME(DT_NODELABEL(controller_modbus)));
    ret = iface < 0 ? iface : modbus_init_server(iface, parameters);
    if (!ret) ret = modbus_register_user_fc(iface, &modbus_cfg_snapshot_read);
    if (!ret) ret = modbus_register_user_fc(iface, &modbus_cfg_atomic_write);
    if (ret && iface >= 0) modbus_disable(iface);
#endif
    atomic_set(&interface_error, ret);
    return ret;
}
