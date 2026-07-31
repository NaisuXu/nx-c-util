/**
 * @file    nx_timer.c
 * @brief   Software timer manager implementation.
 */
#include "nx_timer.h"

void nx_timer_mgr_init(nx_timer_mgr_t *mgr)
{
    if (mgr == NULL) {
        return;
    }
    nx_list_init(&mgr->timers);
    mgr->last_tick = 0;
}

void nx_timer_mgr_process(nx_timer_mgr_t *mgr, uint32_t now)
{
    nx_list_t *pos, *n;

    if (mgr == NULL) {
        return;
    }

    mgr->last_tick = now;

    nx_list_for_each_safe(pos, n, &mgr->timers) {
        nx_timer_t *t = nx_list_entry(pos, nx_timer_t, link);

        if (!t->active) {
            continue;
        }

        /* Overflow-safe comparison: (now - expire) >= 0 when expired */
        if ((int32_t)(now - t->expire_tick) >= 0) {
            if (t->interval == 0) {
                /* One-shot: stop before invoking callback */
                nx_list_del(&t->link);
                t->active = false;
                if (t->cb != NULL) {
                    t->cb(t, t->arg);
                }
            } else {
                /* Periodic: reload then invoke */
                t->expire_tick += t->interval;
                if (t->cb != NULL) {
                    t->cb(t, t->arg);
                }
            }
        }
    }
}

void nx_timer_init(nx_timer_t *t, void (*cb)(struct nx_timer *t, void *arg), void *arg)
{
    if (t == NULL) {
        return;
    }
    nx_list_init(&t->link);
    t->expire_tick = 0;
    t->interval    = 0;
    t->cb          = cb;
    t->arg         = arg;
    t->active      = false;
}

void nx_timer_start(nx_timer_mgr_t *mgr, nx_timer_t *t, uint32_t delay, uint32_t period)
{
    if (mgr == NULL || t == NULL || t->cb == NULL) {
        return;
    }

    /* If already active, remove from the old position */
    if (t->active) {
        nx_list_del(&t->link);
    }

    /* Compute expiry from last processed tick + delay */
    t->expire_tick = mgr->last_tick + delay;
    t->interval    = period;
    t->active      = true;

    /* Add to the manager's list */
    nx_list_add_tail(&mgr->timers, &t->link);
}

void nx_timer_stop(nx_timer_t *t)
{
    if (t == NULL || !t->active) {
        return;
    }
    nx_list_del(&t->link);
    t->active = false;
}
