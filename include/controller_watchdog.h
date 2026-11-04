#ifndef CONTROLLER_WATCHDOG_H
#define CONTROLLER_WATCHDOG_H

/* Вызывается из Shell; зависание выполняет сам поток опроса. */
int controller_crash_test(void);

#endif
