#include "controller_modbus.h"
#include "controller_app.h"
#include <zephyr/sys/atomic.h>
#include <errno.h>
#ifdef CONFIG_APP_MODBUS
#include <zephyr/modbus/modbus.h>
#endif

static atomic_t interface_error = ATOMIC_INIT(-ENOTSUP);

// REQ-MBUS-01
void controller_modbus_snapshot(uint16_t regs[CONTROLLER_MODBUS_REGISTER_COUNT])
{
    // status берёт замок приложения и мьютекс контроллера при копировании.
    struct controller_status status;
    controller_app_status(&status);
    regs[0] = status.state;
    regs[1] = status.active_faults;
    regs[2] = status.active_warnings;
    regs[3] = (uint16_t)status.sample.measured_x10;
    regs[4] = (uint16_t)status.sample.setpoint_x10;
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
#endif
    atomic_set(&interface_error, ret);
    return ret;
}
