#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <strings.h> // Для strcasecmp (игнорирует регистр)

LOG_MODULE_REGISTER(shell_app, LOG_LEVEL_INF);

/* --- Обработчики команд --- */

static int cmd_set_profile(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: set profile <pump|ahu>"); // Исправил подсказку
        return -EINVAL;
    }

    // Используем strcasecmp, чтобы работало и AHU, и ahu
    if (strcasecmp(argv[1], "pump") == 0) {
        current_profile = PROFILE_PUMP;
        shell_print(sh, "Profile set to PUMP (Насос)");
    } else if (strcasecmp(argv[1], "ahu") == 0) {
        current_profile = PROFILE_AHU;
        shell_print(sh, "Profile set to AHU (Приточка)");
    } else {
        shell_error(sh, "Unknown profile: %s", argv[1]);
        return -EINVAL;
    }
    return 0;
}

static int cmd_set_mode(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: set mode <4-20ma|0-10v>"); // Исправил подсказку
        return -EINVAL;
    }

    if (strcasecmp(argv[1], "4-20ma") == 0) {
        input_mode = INPUT_4_20MA;
        shell_print(sh, "Input mode: 4-20mA");
    } else if (strcasecmp(argv[1], "0-10v") == 0) {
        input_mode = INPUT_0_10V;
        shell_print(sh, "Input mode: 0-10V");
    } else {
        shell_error(sh, "Unknown mode: %s", argv[1]);
        return -EINVAL;
    }
    return 0;
}

static int cmd_get_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "=== SYSTEM STATUS ===");
    shell_print(sh, "Profile: %s", 
                (current_profile == PROFILE_PUMP) ? "PUMP" : "AHU");
    shell_print(sh, "Input mode: %s", 
                (input_mode == INPUT_4_20MA) ? "4-20mA" : "0-10V");
    return 0;
}

/* --- Регистрация команд --- */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_set,
    SHELL_CMD(profile, NULL, "Set device profile (pump/ahu)", cmd_set_profile),
    SHELL_CMD(mode,    NULL, "Set analog input mode (4-20ma/0-10v)", cmd_set_mode),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(set, &sub_set, "Set system parameters", NULL);
SHELL_CMD_REGISTER(status, NULL, "Print current system status", cmd_get_status);