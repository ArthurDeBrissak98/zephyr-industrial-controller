#include "controller_app.h"
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

static int command_result(const struct shell *sh, int ret)
{
    if (ret) shell_error(sh, "Команда отклонена: %d", ret);
    else shell_print(sh, "OK");
    return ret;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
    return command_result(sh, controller_app_start());
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
    controller_app_stop();
    return command_result(sh, 0);
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
    return command_result(sh, controller_app_reset());
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    struct controller_status status;
    controller_app_status(&status);
    shell_print(sh, "state=%s profile=%s mode=%s",
                controller_state_name(status.state),
                status.config.profile == PROFILE_PUMP ? "pump" : "ahu",
                status.config.mode == INPUT_4_20MA ? "4-20ma" : "0-10v");
    shell_print(sh, "active=0x%x latched=0x%x current_ua=%d voltage_mv=%d",
                status.active_faults, status.latched_faults,
                status.sample.current_ua, status.sample.voltage_mv);
    shell_print(sh, "frequency_x100=%d output_x100=%d",
                status.frequency_x100, status.output_x100);
    shell_print(sh, "scaled_x10=%d measured_x10=%d signal_valid=%s",
                status.sample.scaled_x10, status.sample.measured_x10,
                status.signal_valid ? "yes" : "no");
    return 0;
}

static int cmd_inject(const struct shell *sh, size_t argc, char **argv)
{
    if (!strcmp(argv[1], "signal")) {
        if (!strcmp(argv[2], "off")) {
            return command_result(sh, controller_app_inject_signal(false, 0));
        }
        char *end;
        errno = 0;
        long value = strtol(argv[2], &end, 10);
        if (errno || end == argv[2] || *end || value < INT_MIN || value > INT_MAX) {
            return command_result(sh, -EINVAL);
        }
        return command_result(sh, controller_app_inject_signal(true, (int)value));
    }
    if (strcmp(argv[1], "wire-break")) return command_result(sh, -EINVAL);
    bool enabled;
    if (!strcmp(argv[2], "on")) enabled = true;
    else if (!strcmp(argv[2], "off")) enabled = false;
    else return command_result(sh, -EINVAL);
    return command_result(sh, controller_app_inject(enabled));
}

static int cmd_profile(const struct shell *sh, size_t argc, char **argv)
{
    struct controller_status status;
    controller_app_status(&status);
    if (!strcmp(argv[1], "pump")) status.config.profile = PROFILE_PUMP;
    else if (!strcmp(argv[1], "ahu")) status.config.profile = PROFILE_AHU;
    else return command_result(sh, -EINVAL);
    return command_result(sh, controller_app_configure(status.config));
}

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
    struct controller_status status;
    controller_app_status(&status);
    if (!strcmp(argv[1], "4-20ma")) status.config.mode = INPUT_4_20MA;
    else if (!strcmp(argv[1], "0-10v")) status.config.mode = INPUT_0_10V;
    else return command_result(sh, -EINVAL);
    return command_result(sh, controller_app_configure(status.config));
}

static int cmd_events(const struct shell *sh, size_t argc, char **argv)
{
    struct controller_event events[EVENT_CAPACITY];
    size_t count = controller_app_events(events, EVENT_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        const struct controller_event *e = &events[i];
        shell_print(sh, "#%u t=%lldms event=%s state=%s faults=0x%x current_ua=%d frequency_x100=%d voltage_mv=%d measured_x10=%d",
                    e->sequence, (long long)e->timestamp_ms, controller_event_name(e->type),
                    controller_state_name(e->state), e->faults, e->current_ua, e->frequency_x100,
                    e->voltage_mv, e->measured_x10);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(settings,
    SHELL_CMD_ARG(profile, NULL, "Профиль: pump или ahu", cmd_profile, 2, 0),
    SHELL_CMD_ARG(mode, NULL, "Вход: 4-20ma или 0-10v", cmd_mode, 2, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_ARG_REGISTER(start, NULL, "Пуск", cmd_start, 1, 0);
SHELL_CMD_ARG_REGISTER(stop, NULL, "Останов", cmd_stop, 1, 0);
SHELL_CMD_ARG_REGISTER(reset, NULL, "Сброс после устранения причины", cmd_reset, 1, 0);
SHELL_CMD_ARG_REGISTER(status, NULL, "Состояние контроллера и выхода", cmd_status, 1, 0);
SHELL_CMD_ARG_REGISTER(inject, NULL, "inject wire-break on|off; inject signal <value|off>", cmd_inject, 3, 0);
SHELL_CMD_ARG_REGISTER(events, NULL, "Журнал событий", cmd_events, 1, 0);
SHELL_CMD_REGISTER(set, &settings, "Настройки остановленного контроллера", NULL);
