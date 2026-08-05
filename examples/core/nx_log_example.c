/**
 * @file    nx_log_example.c
 * @brief   Usage examples and self-tests for the nx_log logging facility.
 *
 * Demonstrates:
 *   1. Level filtering (a message below the runtime level is not enqueued).
 *   2. Plain-text prefix with an injected tick timestamp, drained asynchronously
 *      to a sink via nx_log_process.
 *   3. Whole-line drop with a dropped counter when the buffer is full.
 *   4. Runtime level changes via nx_log_set_level.
 *
 * The logger never allocates: every example provides its own static buffers.
 */
#include "nx_core_examples.h"
#include "src/core/nx_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* A sink that captures drained bytes into a static buffer.           */
/* ------------------------------------------------------------------ */
typedef struct {
    char   buf[512];
    size_t len;
} capture_t;

static void capture_write(void *ctx, const uint8_t *data, size_t len)
{
    capture_t *c = (capture_t *)ctx;
    for (size_t i = 0; i < len && c->len + 1u < sizeof(c->buf); ++i) {
        c->buf[c->len++] = (char)data[i];
    }
    c->buf[c->len] = '\0';
}

/* A monotonic fake tick source (no ctx: a system-wide time source). */
static uint32_t g_tick = 1000u;
static uint32_t mock_tick(void) { return g_tick; }

/* ------------------------------------------------------------------ */
/* Example 1: level filtering + async drain with timestamp prefix     */
/* ------------------------------------------------------------------ */
static void example_filter_and_drain(void)
{
    printf("Example 1: level filtering + async drain\n");

    static uint8_t storage[256];
    static capture_t cap;
    cap.len = 0;

    nx_log_t log;
    nx_log_cfg_t cfg = {
        .buffer      = storage,
        .buffer_size = sizeof(storage),
        .write       = capture_write,
        .io_ctx      = &cap,
        .get_tick    = mock_tick,
        .level       = NX_LOG_LEVEL_INFO,   /* DEBUG/TRACE filtered at runtime */
        .lock        = NULL,
    };
    bool ok = nx_log_init(&log, &cfg);
    assert(ok);

    NX_LOGI(&log, "boot %s", "ready");   /* enqueued */
    NX_LOGD(&log, "verbose %d", 42);     /* below INFO -> not enqueued */

    /* Nothing reaches the sink until we drain. */
    assert(cap.len == 0);
    size_t drained = nx_log_process(&log);
    assert(drained > 0);

    printf("  sink got: %s", cap.buf);
    assert(strstr(cap.buf, "boot ready") != NULL);   /* INFO present */
    assert(strstr(cap.buf, "[1000]") != NULL);       /* timestamp present */
    assert(strstr(cap.buf, "verbose") == NULL);      /* DEBUG absent */
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 2: whole-line drop when the buffer is full                 */
/* ------------------------------------------------------------------ */
static void example_drop_on_full(void)
{
    printf("Example 2: whole-line drop on full buffer\n");

    static uint8_t storage[300];  /* small: fills after a couple of lines */
    static capture_t cap;
    cap.len = 0;

    nx_log_t log;
    nx_log_cfg_t cfg = {
        .buffer      = storage,
        .buffer_size = sizeof(storage),
        .write       = capture_write,
        .io_ctx      = &cap,
        .get_tick    = NULL,                /* no timestamp this time */
        .level       = NX_LOG_LEVEL_TRACE,
        .on_full     = NX_LOG_ON_FULL_DROP_NEW,  /* keep oldest, drop the rest */
        .lock        = NULL,
    };
    assert(nx_log_init(&log, &cfg));

    /* Enqueue until the buffer is full; excess lines are dropped whole. */
    for (int i = 0; i < 20; ++i) {
        NX_LOGE(&log, "message number %d padding padding", i);
    }
    assert(nx_log_dropped(&log) > 0);
    printf("  dropped %zu lines (buffer only %zu bytes)\n",
           nx_log_dropped(&log), sizeof(storage));

    /* Whatever made it in drains cleanly and ends on a newline. */
    size_t drained = nx_log_process(&log);
    assert(drained > 0);
    assert(cap.buf[cap.len - 1u] == '\n');   /* never a half line */
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Example 3: runtime level change                                    */
/* ------------------------------------------------------------------ */
static void example_runtime_level(void)
{
    printf("Example 3: runtime level change\n");

    static uint8_t storage[256];
    static capture_t cap;
    cap.len = 0;

    nx_log_t log;
    nx_log_cfg_t cfg = {
        .buffer      = storage,
        .buffer_size = sizeof(storage),
        .write       = capture_write,
        .io_ctx      = &cap,
        .get_tick    = NULL,
        .level       = NX_LOG_LEVEL_ERROR,   /* start strict */
        .lock        = NULL,
    };
    assert(nx_log_init(&log, &cfg));

    NX_LOGD(&log, "hidden");              /* filtered */
    nx_log_set_level(&log, NX_LOG_LEVEL_DEBUG);
    NX_LOGD(&log, "now visible");         /* passes after the change */

    (void)nx_log_process(&log);
    assert(strstr(cap.buf, "hidden") == NULL);
    assert(strstr(cap.buf, "now visible") != NULL);
    printf("  level raised at runtime, DEBUG now flows\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 4: keep-newest by overwriting the oldest whole lines        */
/* ------------------------------------------------------------------ */
static void example_overwrite_old(void)
{
    printf("Example 4: overwrite oldest lines (keep newest)\n");

    static uint8_t storage[128];
    static capture_t cap;
    cap.len = 0;

    nx_log_t log;
    nx_log_cfg_t cfg = {
        .buffer      = storage,
        .buffer_size = sizeof(storage),
        .write       = capture_write,
        .io_ctx      = &cap,
        .get_tick    = NULL,
        .level       = NX_LOG_LEVEL_TRACE,
        .on_full     = NX_LOG_ON_FULL_OVERWRITE_OLD,
        .lock        = NULL,
    };
    assert(nx_log_init(&log, &cfg));

    /* Log more than fits; the newest lines must survive, the oldest evicted. */
    for (int i = 0; i < 30; ++i) {
        NX_LOGI(&log, "seq %d", i);
    }

    (void)nx_log_process(&log);
    /* The last line is the freshest and must be present; early ones evicted. */
    assert(strstr(cap.buf, "seq 29") != NULL);
    assert(strstr(cap.buf, "seq 0\n") == NULL);
    /* Drained content still ends on a clean line boundary. */
    assert(cap.len > 0 && cap.buf[cap.len - 1u] == '\n');
    printf("  newest survived (\"seq 29\" kept), oldest evicted\n\n");
}

/* ------------------------------------------------------------------ */
/* Example 5: no sink — accumulate in memory, pull out with read       */
/* ------------------------------------------------------------------ */
static void example_no_sink_read(void)
{
    printf("Example 5: no sink, pull with nx_log_read\n");

    static uint8_t storage[256];

    nx_log_t log;
    nx_log_cfg_t cfg = {
        .buffer      = storage,
        .buffer_size = sizeof(storage),
        .write       = NULL,            /* no output interface at all */
        .io_ctx      = NULL,
        .get_tick    = NULL,
        .level       = NX_LOG_LEVEL_INFO,
        .on_full     = NX_LOG_ON_FULL_OVERWRITE_OLD,
        .lock        = NULL,
    };
    assert(nx_log_init(&log, &cfg));   /* NULL sink is accepted */

    NX_LOGI(&log, "stored %d", 1);
    NX_LOGW(&log, "stored %d", 2);

    /* process() is a no-op without a sink. */
    assert(nx_log_process(&log) == 0);

    /* Pull the accumulated log out on demand (e.g. from a debug shell). */
    char out[256] = {0};
    size_t n = nx_log_read(&log, out, sizeof(out) - 1u);
    assert(n > 0);
    assert(strstr(out, "stored 1") != NULL);
    assert(strstr(out, "stored 2") != NULL);
    /* Reading removed them: a second read yields nothing. */
    assert(nx_log_read(&log, out, sizeof(out) - 1u) == 0);
    printf("  read %zu bytes back from memory-only log\n\n", n);
}

int nx_log_example_run(void)
{
    printf("=== nx_log examples ===\n");
    example_filter_and_drain();
    example_drop_on_full();
    example_runtime_level();
    example_overwrite_old();
    example_no_sink_read();
    printf("nx_log: all checks passed\n\n");
    return 0;
}
