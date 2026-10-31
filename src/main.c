#include <zephyr/kernel.h>

int main(void)
{
    // Потоки запускаются автоматически через K_THREAD_DEFINE в других файлах.
    // Просто спим, чтобы не грузить процессор.
    k_sleep(K_FOREVER);
    return 0;
}