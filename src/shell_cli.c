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

static bool parse_integer(const char *text, int *value)
{
    char *end;
    errno = 0;
    long result = strtol(text, &end, 10);
    if (errno || end == text || *end || result < INT_MIN || result > INT_MAX) return false;
    *value = (int)result;
    return true;
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
    shell_print(sh, "warnings=0x%x", status.active_warnings);
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
        int value;
        if (!parse_integer(argv[2], &value)) return command_result(sh, -EINVAL);
        return command_result(sh, controller_app_inject_signal(true, value));
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
        shell_print(sh, "#%u t=%lldms event=%s state=%s faults=0x%x current_ua=%d frequency_x100=%d voltage_mv=%d measured_x10=%d warnings=0x%x",
                    e->sequence, (long long)e->timestamp_ms, controller_event_name(e->type),
                    controller_state_name(e->state), e->faults, e->current_ua, e->frequency_x100,
                    e->voltage_mv, e->measured_x10, e->warnings);
    }
    return 0;
}

static int protection_id_parse(const char *name)
{
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        if (!strcmp(name, protection_name(i))) return i;
    }
    return -1;
}


static int cmd_protection_show(const struct shell *sh, size_t argc, char **argv)
{
    struct controller_status status;
    controller_app_status(&status);
    for (int i = 0; i < PROTECTION_COUNT; i++) {
        const struct protection_config *p = &status.protections[i];
        shell_print(sh, "%s enabled=%s threshold_x10=%d hysteresis_x10=%d delay_ms=%d",
                    protection_name(i), p->enabled ? "yes" : "no",
                    p->threshold_x10, p->hysteresis_x10, p->delay_ms);
    }
    return 0;
}

static int cmd_protection_set(const struct shell *sh, size_t argc, char **argv)
{
    int id = protection_id_parse(argv[1]);
    struct protection_config config = { .enabled = true };
    if (id < 0 || !parse_integer(argv[2], &config.threshold_x10) ||
        !parse_integer(argv[3], &config.hysteresis_x10) ||
        !parse_integer(argv[4], &config.delay_ms)) return command_result(sh, -EINVAL);
    return command_result(sh, controller_app_protection_configure(id, config));
}

static int cmd_protection_disable(const struct shell *sh, size_t argc, char **argv)
{
    int id = protection_id_parse(argv[1]);
    if (id < 0) return command_result(sh, -EINVAL);
    struct controller_status status;
    controller_app_status(&status);
    status.protections[id].enabled = false;
    return command_result(sh, controller_app_protection_configure(id, status.protections[id]));
}

SHELL_STATIC_SUBCMD_SET_CREATE(protection_commands,
    SHELL_CMD_ARG(show, NULL, "Настройки защит", cmd_protection_show, 1, 0),
    SHELL_CMD_ARG(set, NULL, "Имя, порог, гистерезис (x10), задержка (мс)", cmd_protection_set, 5, 0),
    SHELL_CMD_ARG(disable, NULL, "Отключить выбранное ограничение", cmd_protection_disable, 2, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(protect, &protection_commands, "Технологические защиты", NULL);

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
