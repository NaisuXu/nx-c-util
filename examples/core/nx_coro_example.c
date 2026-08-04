/**
 * @file    nx_coro_example.c
 * @brief   Usage examples for the nx_coro stackless coroutines.
 *
 * Demonstrates:
 *   1. Two endless coroutines interleaved by NX_CORO_YIELD - each one an
 *      infinite while(1) loop, driven forward one step per round by the caller.
 *   2. NX_CORO_WAIT_UNTIL: suspending on a condition the caller drives, plus
 *      what "code before NX_CORO_BEGIN runs on every call" looks like.
 *   3. Nesting: a parent coroutine running a child to completion with
 *      NX_CORO_WAIT_WHILE + NX_CORO_SCHEDULE.
 *   4. Time: NX_CORO_SLEEP for a relative delay, and NX_CORO_TIMEDSET /
 *      NX_CORO_TIMEDWAIT to pace an operation against a reference tick.
 *   5. A request with a deadline: it ends normally when the reply arrives in
 *      time, and bails out with NX_CORO_EXIT when it does not.
 *
 * Nothing here allocates and there is no scheduler: every coroutine's state is
 * a plain local struct, and the while loop in each example is the scheduler -
 * it calls the coroutine again and again until it reports that it finished.
 */
#include "nx_core_examples.h"
#include "src/core/nx_coro.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Example 1: interleaving two endless coroutines with NX_CORO_YIELD  */
/* ------------------------------------------------------------------ */

/* State struct: nx_coro_stack_t plus everything that must survive a yield.
 * `step` cannot be a local - a local's value is lost at every suspend point. */
typedef struct {
    nx_coro_stack_t base;
    const char     *name;
    int             step;
} ticker_t;

/* An endless coroutine: the while(1) never exits, so this never reaches
 * NX_CORO_END and never reports finished. It yields once per iteration, which
 * is what keeps an infinite loop cooperative - each call runs exactly one
 * iteration and hands control back. */
static nx_coro_ret_t coro_ticker(ticker_t *st)
{
    NX_CORO_BEGIN(&st->base);

    while (1) {
        printf("    %s: step %d\n", st->name, ++st->step);
        NX_CORO_YIELD(&st->base);   /* returns now, resumes on the next line */
    }

    NX_CORO_END(&st->base);
}

static void example_yield(void)
{
    printf("Example 1: two endless coroutines interleaved with NX_CORO_YIELD\n");

    ticker_t a = { .name = "coro_a" };
    ticker_t b = { .name = "coro_b" };
    NX_CORO_INIT(&a.base);
    NX_CORO_INIT(&b.base);

    /* The scheduler decides when to stop, not the coroutines: one pass of this
     * loop advances each of them by one step, and after three rounds we simply
     * stop calling them. They stay suspended at their yield, ready to resume. */
    for (int round = 1; round <= 3; round++) {
        printf("  round %d:\n", round);
        coro_ticker(&a);
        coro_ticker(&b);
    }
    printf("  stopped after 3 rounds (both still suspended at their yield)\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: suspending on a condition with NX_CORO_WAIT_UNTIL       */
/* ------------------------------------------------------------------ */

/* No state beyond the resume point is needed here: `level` is a parameter, so
 * the caller re-supplies it on every call and the wait condition always sees
 * the current value. */
static nx_coro_ret_t coro_wait_level(nx_coro_stack_t *cs, int level)
{
    /* Anything before NX_CORO_BEGIN runs on *every* call, because the dispatch
     * switch has not happened yet - handy for per-poll work, surprising if
     * unintended. */
    printf("    coro_wait_level: polled with level=%d\n", level);

    NX_CORO_BEGIN(cs);

    printf("    coro_wait_level: started, waiting for level >= 3\n");
    NX_CORO_WAIT_UNTIL(cs, level >= 3);   /* NX_CORO_WAITING until true */
    printf("    coro_wait_level: condition met\n");

    NX_CORO_END(cs);
}

static void example_wait(void)
{
    printf("Example 2: NX_CORO_WAIT_UNTIL on a caller-driven condition\n");

    nx_coro_stack_t cs;
    NX_CORO_INIT(&cs);

    /* The caller owns the condition and raises it between polls. */
    int level = 0;
    while (NX_CORO_SCHEDULE(coro_wait_level(&cs, level))) {
        printf("  not finished yet, raising level to %d\n", ++level);
    }
    printf("  coro_wait_level finished\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: a parent coroutine driving a child to completion        */
/* ------------------------------------------------------------------ */

/* The child's state lives in the parent's state struct, so it survives the
 * parent's own suspend points. */
typedef struct {
    nx_coro_stack_t base;
    nx_coro_stack_t child;
} parent_t;

static nx_coro_ret_t coro_parent(parent_t *st, int level)
{
    NX_CORO_BEGIN(&st->base);

    printf("    coro_parent: starting child\n");
    NX_CORO_INIT(&st->child);

    /* Wait while the child is still running. Each call into the parent drives
     * the child exactly one step, and the parent reports NX_CORO_WAITING for
     * as long as the child has not finished. */
    NX_CORO_WAIT_WHILE(&st->base, NX_CORO_SCHEDULE(coro_wait_level(&st->child, level)));

    printf("    coro_parent: child finished, carrying on\n");

    NX_CORO_END(&st->base);
}

static void example_nesting(void)
{
    printf("Example 3: nesting with NX_CORO_WAIT_WHILE + NX_CORO_SCHEDULE\n");

    parent_t st;
    NX_CORO_INIT(&st.base);

    int level = 0;
    while (NX_CORO_SCHEDULE(coro_parent(&st, level))) {
        printf("  parent not finished yet, raising level to %d\n", ++level);
    }
    printf("  coro_parent finished\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 4: NX_CORO_SLEEP and NX_CORO_TIMEDSET / NX_CORO_TIMEDWAIT  */
/* ------------------------------------------------------------------ */

/* A simulated tick source. On real hardware this would read a SysTick or RTOS
 * counter; here the example loop advances it, one tick per poll. A 32-bit tick
 * matches nx_coro_stack_plus_t::get_tick and gives a platform-independent range. */
static uint32_t g_tick;

static uint32_t sim_get_tick(void)
{
    return g_tick;
}

/* Time-based macros need nx_coro_stack_plus_t, which carries the tick source
 * and the reference timestamp. */
static nx_coro_ret_t coro_paced(nx_coro_stack_plus_t *cs)
{
    NX_CORO_BEGIN(cs);

    /* Relative delay: stamps "now" on arrival, then waits 3 ticks. */
    printf("    coro_paced: sleeping 3 ticks (tick %u)\n", (unsigned)sim_get_tick());
    NX_CORO_SLEEP(cs, 3);
    printf("    coro_paced: woke up at tick %u\n", (unsigned)sim_get_tick());

    /* Reference-based delay: arm the reference, do work, then wait out the
     * remainder of the interval. Time spent working counts toward the 5 ticks,
     * which is what makes this a period rather than another sleep. */
    NX_CORO_TIMEDSET(cs);
    printf("    coro_paced: armed at tick %u, working\n", (unsigned)sim_get_tick());
    NX_CORO_YIELD(cs);                  /* stand-in for one step of real work */
    NX_CORO_TIMEDWAIT(cs, 5);
    printf("    coro_paced: 5 ticks since arming (tick %u)\n", (unsigned)sim_get_tick());

    NX_CORO_END(cs);
}

static void example_time(void)
{
    printf("Example 4: NX_CORO_SLEEP and NX_CORO_TIMEDSET/NX_CORO_TIMEDWAIT\n");

    nx_coro_stack_plus_t cs;
    NX_CORO_INIT_PLUS(&cs, sim_get_tick);

    g_tick = 0;
    while (NX_CORO_SCHEDULE(coro_paced(&cs))) {
        g_tick++;                       /* stands in for the tick interrupt */
        printf("  tick %u\n", (unsigned)g_tick);
    }
    printf("  coro_paced finished at tick %u\n\n", (unsigned)g_tick);
}

/* ------------------------------------------------------------------ */
/* Example 5: a deadline, ending either normally or with NX_CORO_EXIT */
/* ------------------------------------------------------------------ */

typedef struct {
    nx_coro_stack_plus_t base;
    const int           *reply;   /* "reply arrived" flag, owned by the caller */
} request_t;

#define REQUEST_TIMEOUT 4U        /* ticks */

static nx_coro_ret_t coro_request(request_t *st)
{
    NX_CORO_BEGIN(&st->base);

    printf("    coro_request: sent, waiting up to %u ticks\n", REQUEST_TIMEOUT);
    NX_CORO_TIMEDSET(&st->base);

    /* NX_CORO_TIMEDWAIT would always wait the full interval, so a "first of
     * the two" deadline is spelled out as one condition: reply, or expiry. */
    NX_CORO_WAIT_UNTIL(&st->base,
                       *st->reply ||
                       (st->base.get_tick() - st->base.ticks) >= REQUEST_TIMEOUT);

    if (!*st->reply) {
        printf("    coro_request: timed out, giving up\n");
        NX_CORO_EXIT(&st->base);        /* finishes as NX_CORO_EXITED */
    }
    printf("    coro_request: reply received at tick %u\n", (unsigned)sim_get_tick());

    NX_CORO_END(&st->base);             /* finishes as NX_CORO_ENDED */
}

/* Run one request; the reply arrives at reply_tick, or never if it is 0. */
static void run_request(const char *title, uint32_t reply_tick)
{
    printf("  %s:\n", title);

    int       reply = 0;
    request_t st    = { .reply = &reply };
    NX_CORO_INIT_PLUS(&st.base, sim_get_tick);

    g_tick = 0;
    nx_coro_ret_t ret = NX_CORO_ENDED;
    while (NX_CORO_SCHEDULE((ret = coro_request(&st)))) {
        g_tick++;
        if (reply_tick != 0 && g_tick == reply_tick) {
            reply = 1;
            printf("    tick %u: reply arrives\n", (unsigned)g_tick);
        } else {
            printf("    tick %u\n", (unsigned)g_tick);
        }
    }

    /* EXITED vs ENDED is how the caller tells a bail-out from a clean finish. */
    printf("    result: %s\n", (ret == NX_CORO_ENDED) ? "ENDED" : "EXITED");
}

static void example_timeout(void)
{
    printf("Example 5: deadline with NX_CORO_TIMEDSET and NX_CORO_EXIT\n");
    run_request("reply arrives at tick 2", 2);
    run_request("no reply at all", 0);
    printf("\n");
}

int nx_coro_example_run(void)
{
    printf("########## nx_coro examples ##########\n");
    example_yield();
    example_wait();
    example_nesting();
    example_time();
    example_timeout();
    return 0;
}
