/**
 * @file    nx_event_flags_example.c
 * @brief   Example demonstrating nx_event_flags usage patterns.
 *
 * Three scenarios:
 *   1. Single-context polling (main loop only, no ISR)
 *   2. ISR sets flags, main loop takes them
 *   3. Broadcast + acknowledgment barrier (shutdown coordination)
 */

#include <stdio.h>
#include <assert.h>
#include "src/core/nx_event_flags.h"

/* ------------------------------------------------------------------ */
/* Scenario 1: Single-context polling (no lock)                      */
/* ------------------------------------------------------------------ */

#define WORK_PENDING   (1u << 0)
#define DATA_READY     (1u << 1)

static void scenario_single_context(void)
{
    printf("=== Scenario 1: Single-context polling ===\n");

    nx_event_flags_t flags;
    nx_event_flags_init(&flags, NULL);   /* no lock: main loop only */

    /* Producer marks work pending */
    nx_event_flags_set(&flags, WORK_PENDING);
    assert(nx_event_flags_test(&flags, WORK_PENDING));
    printf("  work pending flag set\n");

    /* Consumer takes it (test + clear atomically) */
    assert(nx_event_flags_take(&flags, WORK_PENDING));
    assert(!nx_event_flags_test(&flags, WORK_PENDING));
    printf("  work pending flag taken and cleared\n");

    /* Multiple sets coalesce: set twice, take once */
    nx_event_flags_set(&flags, DATA_READY);
    nx_event_flags_set(&flags, DATA_READY);
    assert(nx_event_flags_take(&flags, DATA_READY));   /* takes both */
    assert(!nx_event_flags_take(&flags, DATA_READY));  /* second take finds nothing */
    printf("  two sets coalesced into one take\n");

    printf("  passed\n\n");
}

/* ------------------------------------------------------------------ */
/* Scenario 2: ISR sets, main loop takes (with lock)                 */
/* ------------------------------------------------------------------ */

#define RX_READY       (1u << 0)
#define TX_DONE        (1u << 1)

/* Mock lock: just records enter/exit calls to prove it's being used */
static int g_lock_depth = 0;

static uintptr_t mock_enter(void *ctx)
{
    (void)ctx;
    g_lock_depth++;
    return 0u;   /* saved state (unused in this mock) */
}

static void mock_exit(void *ctx, uintptr_t state)
{
    (void)ctx;
    (void)state;
    g_lock_depth--;
}

static const nx_lock_t g_mock_lock = {
    .enter = mock_enter,
    .exit  = mock_exit,
    .ctx   = NULL,
};

/* Simulated ISR: sets RX_READY */
static void mock_uart_isr(nx_event_flags_t *flags)
{
    nx_event_flags_set(flags, RX_READY);   /* atomic under lock */
}

static void scenario_isr_to_main(void)
{
    printf("=== Scenario 2: ISR sets, main loop takes ===\n");

    nx_event_flags_t flags;
    nx_event_flags_init(&flags, &g_mock_lock);
    assert(g_lock_depth == 0);

    /* ISR fires: sets RX_READY */
    mock_uart_isr(&flags);
    assert(g_lock_depth == 0);   /* enter + exit balanced */
    printf("  ISR set RX_READY\n");

    /* Main loop iteration: check and take */
    if (nx_event_flags_take(&flags, RX_READY)) {
        printf("  main loop took RX_READY, processing...\n");
    }
    assert(!nx_event_flags_test(&flags, RX_READY));
    assert(g_lock_depth == 0);

    /* Another ISR sets TX_DONE */
    nx_event_flags_set(&flags, TX_DONE);
    printf("  ISR set TX_DONE\n");

    /* Main loop uses test (non-consuming) to check without clearing */
    assert(nx_event_flags_test(&flags, TX_DONE));
    printf("  main loop tested TX_DONE (still set)\n");
    nx_event_flags_clear(&flags, TX_DONE);
    assert(!nx_event_flags_test(&flags, TX_DONE));
    printf("  main loop cleared TX_DONE explicitly\n");

    printf("  passed\n\n");
}

/* ------------------------------------------------------------------ */
/* Scenario 3: Broadcast + acknowledgment barrier                    */
/* ------------------------------------------------------------------ */

/* Bit definitions: one broadcast request, three ack bits */
#define SIG_SHUTDOWN    (1u << 0)   /* broadcast: system stopping, prepare to halt */
#define SIG_ACK_VALVE   (1u << 1)   /* valve_ctrl finished its shutdown sequence */
#define SIG_ACK_LOG     (1u << 2)   /* log flushed */
#define SIG_ACK_CAN     (1u << 3)   /* CAN stopped transmitting */
#define SIG_ACK_ALL     (SIG_ACK_VALVE | SIG_ACK_LOG | SIG_ACK_CAN)

/* Mock business modules */
static void valve_ctrl_process(nx_event_flags_t *flags)
{
    if (nx_event_flags_test(flags, SIG_SHUTDOWN)) {   /* broadcast: non-consuming */
        /* close valves, latch state, etc. */
        nx_event_flags_set(flags, SIG_ACK_VALVE);
        printf("  [valve_ctrl] saw SHUTDOWN, set ACK_VALVE\n");
    }
}

static void log_process(nx_event_flags_t *flags)
{
    if (nx_event_flags_test(flags, SIG_SHUTDOWN)) {
        /* flush buffered log lines */
        nx_event_flags_set(flags, SIG_ACK_LOG);
        printf("  [log] saw SHUTDOWN, set ACK_LOG\n");
    }
}

static void can_port_process(nx_event_flags_t *flags)
{
    if (nx_event_flags_test(flags, SIG_SHUTDOWN)) {
        /* drain TX queue */
        nx_event_flags_set(flags, SIG_ACK_CAN);
        printf("  [can_port] saw SHUTDOWN, set ACK_CAN\n");
    }
}

static void scenario_shutdown_barrier(void)
{
    printf("=== Scenario 3: Broadcast + acknowledgment barrier ===\n");

    nx_event_flags_t flags;
    nx_event_flags_init(&flags, NULL);   /* single-context for this example */

    /* UDS diagnostics requests a reset: set the broadcast bit */
    nx_event_flags_set(&flags, SIG_SHUTDOWN);
    printf("  [uds] set SHUTDOWN (broadcast)\n");

    /* Main loop iteration 1: each module sees the broadcast and acks */
    valve_ctrl_process(&flags);
    log_process(&flags);
    can_port_process(&flags);

    /* Check that all acks are in */
    assert(nx_event_flags_test_all(&flags, SIG_ACK_ALL));
    printf("  [uds] all acks received: %08X\n", nx_event_flags_get(&flags));

    /* The shutdown bit is still set (broadcast, nobody cleared it) */
    assert(nx_event_flags_test(&flags, SIG_SHUTDOWN));
    printf("  SHUTDOWN bit still set (as expected for a broadcast)\n");

    /* UDS proceeds with the reset now that everyone is ready */
    printf("  [uds] proceeding to reset...\n");

    printf("  passed\n\n");
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int nx_event_flags_example_run(void)
{
    printf("########## nx_event_flags examples ##########\n");
    scenario_single_context();
    scenario_isr_to_main();
    scenario_shutdown_barrier();
    return 0;
}
