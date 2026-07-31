/**
 * @file    nx_timer_example.c
 * @brief   Example usage of the nx_timer software timer manager.
 */
#include "src/core/nx_timer.h"

#include <stdio.h>
#include <stdint.h>

/* Example: LED blinker using a periodic timer */
static void led_blink_cb(nx_timer_t *t, void *arg)
{
    (void)t;
    int *state = (int *)arg;
    *state = !(*state);
    printf("  LED %s\n", *state ? "ON" : "OFF");
}

/* Example: One-shot alarm */
static void alarm_cb(nx_timer_t *t, void *arg)
{
    (void)t;
    const char *msg = (const char *)arg;
    printf("  ALARM: %s\n", msg);
}

/* Example: Periodic status report */
static void status_cb(nx_timer_t *t, void *arg)
{
    (void)t;
    int *counter = (int *)arg;
    (*counter)++;
    printf("  Status report #%d\n", *counter);
}

int nx_timer_example_run(void)
{
    printf("########## nx_timer examples ##########\n");

    nx_timer_mgr_t mgr;
    nx_timer_t led_timer, alarm_timer, status_timer;
    int led_state = 0;
    int status_count = 0;

    /* Initialize the manager */
    nx_timer_mgr_init(&mgr);

    /* Initialize timers */
    nx_timer_init(&led_timer, led_blink_cb, &led_state);
    nx_timer_init(&alarm_timer, alarm_cb, (void *)"System startup complete");
    nx_timer_init(&status_timer, status_cb, &status_count);

    /* Start timers:
     * - LED blinks every 10 ticks, starting at tick 0
     * - Alarm fires once at tick 5
     * - Status reports every 15 ticks, starting at tick 15
     */
    nx_timer_start(&mgr, &led_timer, 0, 10);
    nx_timer_start(&mgr, &alarm_timer, 5, 0);
    nx_timer_start(&mgr, &status_timer, 15, 15);

    printf("\nSimulating 50 ticks (1 tick = 1 arbitrary time unit):\n");
    for (uint32_t tick = 0; tick < 50; tick++) {
        printf("Tick %u:\n", tick);
        nx_timer_mgr_process(&mgr, tick);
    }

    /* Stop the LED timer */
    printf("\nStopping LED timer at tick 50\n");
    nx_timer_stop(&led_timer);

    printf("\nContinuing for 20 more ticks:\n");
    for (uint32_t tick = 50; tick < 70; tick++) {
        printf("Tick %u:\n", tick);
        nx_timer_mgr_process(&mgr, tick);
    }

    /* Restart LED with different period */
    printf("\nRestarting LED timer with period 5 at tick 70\n");
    nx_timer_start(&mgr, &led_timer, 0, 5);

    printf("\nFinal 15 ticks:\n");
    for (uint32_t tick = 70; tick < 85; tick++) {
        printf("Tick %u:\n", tick);
        nx_timer_mgr_process(&mgr, tick);
    }

    printf("\n");
    return 0;
}
