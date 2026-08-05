/**
 * @file    nx_log.c
 * @brief   Implementation of the nx_log logging facility.
 */
#include "nx_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Single-character tag for each level, indexed by nx_log_level_t. */
static const char NX_LOG_TAGS[] = { '-', 'E', 'W', 'I', 'D', 'T' };

/**
 * @brief Discard the oldest line (up to and including its trailing '\n').
 *
 * The buffer only ever holds whole '\n'-terminated lines, so scanning for the
 * first newline finds the end of the oldest line. A line may wrap the buffer, so
 * the scan walks across contiguous segments. If no newline is found (should not
 * happen), the whole buffer is cleared as a defensive fallback.
 */
static void log_evict_oldest_line(nx_ringbuf_t *rb)
{
    for (;;) {
        size_t         seg_len = 0u;
        const uint8_t *seg     = nx_ringbuf_peek_linear(rb, &seg_len);
        if (seg == NULL || seg_len == 0u) {
            break;   /* no newline found: fall through to clear */
        }
        for (size_t i = 0u; i < seg_len; ++i) {
            if (seg[i] == (uint8_t)'\n') {
                (void)nx_ringbuf_discard(rb, i + 1u);   /* through the newline */
                return;
            }
        }
        (void)nx_ringbuf_discard(rb, seg_len);   /* advance past this segment */
    }
    nx_ringbuf_clear(rb);
}

bool nx_log_init(nx_log_t *log, const nx_log_cfg_t *cfg)
{
    if (log == NULL || cfg == NULL || cfg->buffer == NULL ||
        cfg->buffer_size == 0u) {
        return false;
    }
    if (!nx_ringbuf_init(&log->rb, cfg->buffer, cfg->buffer_size)) {
        return false;
    }
    log->cfg     = *cfg;
    log->dropped = 0u;
    return true;
}

void nx_log_set_level(nx_log_t *log, nx_log_level_t level)
{
    if (log != NULL) {
        log->cfg.level = level;
    }
}

bool nx_log_write(nx_log_t *log, nx_log_level_t level,
                  const char *file, int line, const char *fmt, ...)
{
    char   line_buf[NX_LOG_LINE_MAX];
    char   tag;
    int    n;
    size_t len;

    if (log == NULL || fmt == NULL) {
        return false;
    }
    if (level == NX_LOG_LEVEL_OFF || level > log->cfg.level) {
        return false;   /* filtered out at runtime */
    }

    tag = (level <= NX_LOG_LEVEL_TRACE) ? NX_LOG_TAGS[level] : '?';

    /* Prefix: "[T] " optionally "[tick] " then "file:line: ". */
    if (log->cfg.get_tick != NULL) {
        n = snprintf(line_buf, sizeof(line_buf), "[%c] [%lu] %s:%d: ",
                     tag, (unsigned long)log->cfg.get_tick(), file, line);
    } else {
        n = snprintf(line_buf, sizeof(line_buf), "[%c] %s:%d: ",
                     tag, file, line);
    }
    if (n < 0) {
        return false;
    }
    len = (size_t)n;
    if (len >= sizeof(line_buf)) {
        len = sizeof(line_buf) - 1u;   /* prefix alone filled the buffer */
    } else {
        va_list ap;
        va_start(ap, fmt);
        n = vsnprintf(line_buf + len, sizeof(line_buf) - len, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return false;
        }
        len += (size_t)n;
        if (len >= sizeof(line_buf)) {
            len = sizeof(line_buf) - 1u;   /* message truncated */
        }
    }

    /* Guarantee a trailing newline (overwriting the last byte if truncated). */
    if (len + 1u < sizeof(line_buf)) {
        line_buf[len]      = '\n';
        line_buf[len + 1u] = '\0';
        len += 1u;
    } else {
        line_buf[sizeof(line_buf) - 1u] = '\n';
        len = sizeof(line_buf);
    }

    {
        bool      ok    = false;
        uintptr_t saved = nx_lock_enter(log->cfg.lock);

        if (log->cfg.on_full == NX_LOG_ON_FULL_OVERWRITE_OLD) {
            /* Evict oldest whole lines until the new one fits (or nothing is
             * left to evict, which only happens if len exceeds capacity). */
            while (nx_ringbuf_free(&log->rb) < len &&
                   nx_ringbuf_size(&log->rb) > 0u) {
                log_evict_oldest_line(&log->rb);
                log->dropped++;
            }
        }

        if (nx_ringbuf_free(&log->rb) >= len) {
            (void)nx_ringbuf_write(&log->rb, line_buf, len);
            ok = true;
        } else {
            log->dropped++;   /* line larger than the whole buffer */
        }
        nx_lock_exit(log->cfg.lock, saved);
        return ok;
    }
}

size_t nx_log_process(nx_log_t *log)
{
    size_t total = 0u;

    if (log == NULL || log->cfg.write == NULL) {
        return 0u;   /* no sink: pull with nx_log_read instead */
    }

    /* Copy each chunk out of the ring under the lock, then hand it to the
     * (possibly slow, possibly blocking) sink OUTSIDE the lock. Copying under
     * the lock makes the consume atomic against a producer - even one in an ISR,
     * even one evicting the oldest line under NX_LOG_ON_FULL_OVERWRITE_OLD (which
     * moves read_pos on the producer side) - so the sink never streams bytes that
     * are being overwritten, and a producer never waits on I/O. */
    for (;;) {
        uint8_t   chunk[NX_LOG_LINE_MAX];
        size_t    n;
        uintptr_t saved = nx_lock_enter(log->cfg.lock);

        n = nx_ringbuf_read(&log->rb, chunk, sizeof(chunk));
        nx_lock_exit(log->cfg.lock, saved);

        if (n == 0u) {
            break;
        }
        log->cfg.write(log->cfg.io_ctx, chunk, n);
        total += n;
    }
    return total;
}

size_t nx_log_read(nx_log_t *log, void *dst, size_t max)
{
    size_t    n;
    uintptr_t saved;

    if (log == NULL) {
        return 0u;
    }
    saved = nx_lock_enter(log->cfg.lock);
    n = nx_ringbuf_read(&log->rb, dst, max);
    nx_lock_exit(log->cfg.lock, saved);
    return n;
}

size_t nx_log_dropped(const nx_log_t *log)
{
    return (log != NULL) ? log->dropped : 0u;
}
