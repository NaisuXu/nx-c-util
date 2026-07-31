/**
 * @file    nx_timer.h
 * @brief   Software timer manager driven by a single tick source, in pure C.
 *
 * A set of software timers tracked against one monotonically increasing tick
 * counter. The caller drives it: an interrupt or main-loop counter advances the
 * tick, and nx_timer_process(mgr, now) is called periodically to fire whichever
 * timers have expired. This module touches no hardware, so it works the same on
 * bare metal, under an RTOS, or on a PC.
 *
 * Tick unit: a tick is a caller-defined unit, not milliseconds. Whatever your
 * source counts in (a 1 ms SysTick, a 10 ms RTOS tick, microseconds on a PC) is
 * the unit of every delay/period argument. nx_timer_start(t, 100, 0) means "fire
 * 100 ticks from now".
 *
 * Callback context: the callbacks run inside nx_timer_process. Where you call
 * process decides their context - call it from the main loop for relaxed
 * callbacks, or from the tick interrupt for tighter latency (then keep callbacks
 * very short). This module adds no locks; serialize access yourself if timers
 * are started/stopped from a different context than process.
 *
 * Overflow: ticks are uint32_t and wrap around. Expiry is compared with a signed
 * difference, so wrap is handled correctly as long as no single timer's delay or
 * period exceeds INT32_MAX ticks (~24.8 days at a 1 ms tick).
 *
 * Timers are tracked in an intrusive list, so the module allocates nothing: each
 * timer lives in caller-owned storage.
 */
#ifndef NX_TIMER_H
#define NX_TIMER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/nx_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Software timer control block.
 *
 * Embed this structure in your timer data, or allocate it as a standalone timer.
 * Initialize with nx_timer_init and start with nx_timer_start.
 */
typedef struct nx_timer {
    nx_list_t   link;          /**< list hook; managed by the module */
    uint32_t    expire_tick;   /**< absolute tick when this timer fires */
    uint32_t    interval;      /**< reload value; 0 = one-shot, !=0 = periodic */
    void      (*cb)(struct nx_timer *t, void *arg);  /**< callback; never NULL after init */
    void       *arg;           /**< user context for the callback */
    bool        active;        /**< true when started, false when stopped */
} nx_timer_t;

/**
 * @brief Timer manager; one per application or subsystem.
 *
 * Tracks all started timers on an intrusive list.
 */
typedef struct {
    nx_list_t   timers;        /**< active timer list */
    uint32_t    last_tick;     /**< last tick value passed to process */
} nx_timer_mgr_t;

/**
 * @brief Initialize a timer manager.
 *
 * Call once, before starting any timers.
 *
 * @param mgr  The manager to initialize; must not be NULL.
 */
void nx_timer_mgr_init(nx_timer_mgr_t *mgr);

/**
 * @brief Process expired timers against the current tick.
 *
 * Call this periodically (from your tick interrupt or main loop). Each expired
 * timer's callback is invoked immediately. Periodic timers are reloaded, one-shot
 * timers are stopped.
 *
 * @param mgr  The manager; must not be NULL.
 * @param now  The current tick count from your tick source.
 */
void nx_timer_mgr_process(nx_timer_mgr_t *mgr, uint32_t now);

/**
 * @brief Initialize a timer control block.
 *
 * Sets the callback and user context. The timer is stopped after this call;
 * use nx_timer_start to arm it.
 *
 * @param t    The timer to initialize; must not be NULL.
 * @param cb   Callback invoked when the timer expires; must not be NULL.
 * @param arg  User context passed to the callback; may be NULL.
 */
void nx_timer_init(nx_timer_t *t, void (*cb)(struct nx_timer *t, void *arg), void *arg);

/**
 * @brief Start a timer.
 *
 * The timer will fire after @p delay ticks, then every @p period ticks if
 * periodic. If already active, it is restarted with the new delay/period.
 *
 * @param mgr     The manager tracking this timer; must not be NULL.
 * @param t       The timer to start; must not be NULL and must have been initialized.
 * @param delay   Ticks from now until first expiry.
 * @param period  Reload interval in ticks; 0 = one-shot, non-zero = periodic.
 */
void nx_timer_start(nx_timer_mgr_t *mgr, nx_timer_t *t, uint32_t delay, uint32_t period);

/**
 * @brief Stop a timer.
 *
 * After this call the timer is inactive and will not fire. Stopping an already
 * stopped timer is safe and does nothing.
 *
 * @param t  The timer to stop; must not be NULL.
 */
void nx_timer_stop(nx_timer_t *t);

#ifdef __cplusplus
}
#endif

#endif /* NX_TIMER_H */
