#include "controller_app.h"
#include <zephyr/ztest.h>
#include <errno.h>

K_SEM_DEFINE(worker_ready, 0, 1);

static void worker(void *a, void *b, void *c)
{
    k_sem_take(&worker_ready, K_FOREVER);
    while (1) {
        controller_app_tick();
        k_sleep(K_MSEC(CONTROLLER_PERIOD_MS));
    }
}
K_THREAD_DEFINE(worker_tid, 2048, worker, NULL, NULL, NULL, 5, 0, 0);

static struct controller_status wait_for_state(enum controller_state state)
{
    struct controller_status status;
    int64_t deadline = k_uptime_get() + 1000;
    do {
        controller_app_status(&status);
        if (status.state == state) return status;
        k_msleep(1);
    } while (k_uptime_get() < deadline);
    zassert_equal(status.state, state, "Контроллер не перешёл в ожидаемое состояние");
    return status;
}

ZTEST(controller_scenario, test_wire_break_stop_journal_reset_restart)
{
    controller_app_init();
    k_sem_give(&worker_ready);
    struct controller_status status = wait_for_state(STATE_READY);
    zassert_equal(status.output_x100, 0);
    zassert_ok(controller_app_start());
    k_msleep(CONTROLLER_PERIOD_MS * 3);
    controller_app_status(&status);
    zassert_true(status.output_x100 > 0);

    int64_t injected_at = k_uptime_get();
    zassert_ok(controller_app_inject(true));
    status = wait_for_state(STATE_FAULT);
    zassert_true(k_uptime_get() - injected_at <= CONTROLLER_PERIOD_MS + 50,
                 "Защита сработала позже допустимых 150 мс");
    zassert_equal(status.active_faults, FAULT_WIRE_BREAK);
    zassert_equal(status.latched_faults, FAULT_WIRE_BREAK);
    zassert_equal(status.output_x100, 0);
    zassert_equal(controller_app_reset(), -EBUSY);
    controller_app_stop();
    zassert_equal(controller_app_start(), -EACCES);

    struct controller_event events[EVENT_CAPACITY];
    size_t count = controller_app_events(events, EVENT_CAPACITY);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (events[i].type == EVENT_FAULT) {
            found = true;
            zassert_true(events[i].timestamp_ms >= injected_at);
            zassert_equal(events[i].faults, FAULT_WIRE_BREAK);
            zassert_equal(events[i].current_ua, 2000);
            zassert_equal(events[i].frequency_x100, 0);
        }
    }
    zassert_true(found, "Причина аварии не попала в журнал");

    zassert_ok(controller_app_inject(false));
    k_msleep(CONTROLLER_PERIOD_MS * 3);
    controller_app_status(&status);
    zassert_equal(status.active_faults, FAULT_NONE);
    zassert_equal(status.latched_faults, FAULT_WIRE_BREAK);
    zassert_equal(status.state, STATE_FAULT);
    zassert_equal(status.output_x100, 0);
    zassert_ok(controller_app_reset());
    k_msleep(CONTROLLER_PERIOD_MS * 2);
    controller_app_status(&status);
    zassert_equal(status.state, STATE_READY);
    zassert_equal(status.output_x100, 0, "Сброс не должен запускать привод");
    zassert_ok(controller_app_start());
    k_msleep(CONTROLLER_PERIOD_MS * 2);
    controller_app_status(&status);
    zassert_equal(status.state, STATE_RUNNING);
    zassert_true(status.output_x100 > 0);

    // Новый обрыв перед самым пуском тоже должен блокировать привод.
    controller_app_stop();
    zassert_ok(controller_app_inject(true));
    zassert_equal(controller_app_start(), -EACCES);
    controller_app_status(&status);
    zassert_equal(status.state, STATE_FAULT);
    zassert_equal(status.output_x100, 0);
    zassert_equal(controller_app_reset(), -EBUSY);
    zassert_ok(controller_app_inject(false));
    zassert_ok(controller_app_reset());
}

ZTEST_SUITE(controller_scenario, NULL, NULL, NULL, NULL, NULL);
