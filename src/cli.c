#include "sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <sys/select.h>
#include <unistd.h>

LOG_MODULE_REGISTER(cli, LOG_LEVEL_INF);

static void cli_thread(void)
{
    LOG_INF("=====================================");
    LOG_INF("Нажми '1' + Enter для профиля НАСОС");
    LOG_INF("Нажми '2' + Enter для профиля ПРИТОЧКА (AHU)");
    LOG_INF("=====================================");

    while (1) {
        fd_set fds;
        // Таймаут 100 мс. Если ввода нет - select() просто вернет 0, 
        // и Zephyr спокойно продолжит работу (сенсор, монитор и т.д.)
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char buf[16] = {0};
            int n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
            
            if (n > 0) {
                if (buf[0] == '1') {
                    current_profile = PROFILE_PUMP;
                    LOG_WRN(">>> Переключено на НАСОС <<<");
                } else if (buf[0] == '2') {
                    current_profile = PROFILE_AHU;
                    LOG_WRN(">>> Переключено на ПРИТОЧКА (AHU) <<<");
                }
            }
        }
        
        // Даем другим потокам поработать
        k_sleep(K_MSEC(10));
    }
}

// Приоритет 9 (самый низкий), чтобы CLI точно не мешал управлению
K_THREAD_DEFINE(cli_tid, 2048, cli_thread, NULL, NULL, NULL, 9, 0, 0);