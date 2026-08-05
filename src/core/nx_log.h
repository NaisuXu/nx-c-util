/**
 * @file    nx_log.h
 * @brief   Static, asynchronous, plain-text logging, in pure C.
 *
 * A logging facility built for embedded targets: it formats a message with
 * vsnprintf into a caller-owned ring buffer, then a single process() call on the
 * main loop drains that buffer to an injected write sink. Formatting (which may
 * run in an interrupt) is decoupled from the slow, possibly blocking sink, so a
 * producer never waits on I/O. The sink is optional: with no sink the log stays
 * in the buffer, to be pulled out with nx_log_read or inspected in a debugger.
 *
 * Design goals: aimed at embedded development - simple, predictable, no hidden
 * overhead.
 *
 * Features:
 *   - Plain text: messages are formatted with vsnprintf, so the output is
 *     directly readable on a serial terminal with no decoding tool.
 *   - Asynchronous delivery: a formatted line is enqueued into a byte ring
 *     buffer; process() later hands contiguous segments to the sink, or
 *     nx_log_read pulls them out when there is no sink. Producers are decoupled
 *     from the consumer's latency.
 *   - Purely static: the ring-buffer storage is provided entirely by the caller;
 *     this module uses no dynamic memory and does not depend on malloc/free.
 *   - Level filtering at two stages: NX_LOG_COMPILE_LEVEL strips higher-verbosity
 *     call sites at compile time (their format strings never reach the image),
 *     and a runtime level filters what remains and can change on the fly.
 *   - Whole-line-or-nothing: a line is never written half-way. When the buffer
 *     is full the on_full policy either drops the new line (and counts it) or
 *     evicts the oldest whole lines to make room for it.
 *   - Optional locking: a single-producer/single-consumer setup needs no lock;
 *     when several contexts log concurrently the caller supplies an nx_lock that
 *     wraps the enqueue step. This module introduces no locks of its own.
 */
#ifndef NX_LOG_H
#define NX_LOG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_ringbuf.h"
#include "nx_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Severity levels, most severe first (OFF disables all output).
 */
typedef enum {
    NX_LOG_LEVEL_OFF = 0,  /**< No output */
    NX_LOG_LEVEL_ERROR,    /**< Errors that need attention */
    NX_LOG_LEVEL_WARN,     /**< Unexpected but recoverable conditions */
    NX_LOG_LEVEL_INFO,     /**< High-level progress messages */
    NX_LOG_LEVEL_DEBUG,    /**< Detailed diagnostics */
    NX_LOG_LEVEL_TRACE     /**< Very fine-grained tracing */
} nx_log_level_t;

/**
 * @brief Behavior when the ring buffer cannot hold a new line.
 */
typedef enum {
    NX_LOG_ON_FULL_OVERWRITE_OLD = 0,  /**< Evict oldest whole lines to make room (default) */
    NX_LOG_ON_FULL_DROP_NEW            /**< Drop the new line, keep the oldest */
} nx_log_on_full_t;

/**
 * @brief Highest level kept at compile time; call sites more verbose than this
 *        expand to nothing, so their format strings never reach the image.
 *        Override by defining it before including this header.
 */
#ifndef NX_LOG_COMPILE_LEVEL
#define NX_LOG_COMPILE_LEVEL NX_LOG_LEVEL_TRACE
#endif

/**
 * @brief Maximum length of one formatted line, including prefix and newline.
 *        Longer lines are truncated. Override before including this header.
 */
#ifndef NX_LOG_LINE_MAX
#define NX_LOG_LINE_MAX 128u
#endif

/**
 * @brief Logger configuration supplied by the caller at init time.
 */
typedef struct {
    void   *buffer;       /**< Ring-buffer storage (caller-provided) */
    size_t  buffer_size;  /**< Its size in bytes, must be > 0 */
    /** Sink that consumes drained bytes. NULL keeps logs in the buffer only,
     *  to be pulled out with nx_log_read or inspected in a debugger. */
    void  (*write)(void *ctx, const uint8_t *data, size_t len);
    void   *io_ctx;       /**< Context passed to @c write */
    uint32_t (*get_tick)(void);  /**< Timestamp source; NULL omits the timestamp */
    nx_log_level_t   level;      /**< Runtime minimum level (also gated by compile level) */
    nx_log_on_full_t on_full;    /**< Policy when the buffer cannot hold a new line (default: overwrite oldest) */
    const nx_lock_t *lock;       /**< Critical section around enqueue; NULL = none */
} nx_log_cfg_t;

/**
 * @brief Logger handle.
 *
 * @note  The struct members are implementation details; do not access them
 *        directly, use the provided API instead.
 */
typedef struct {
    nx_log_cfg_t cfg;      /**< Copied configuration */
    nx_ringbuf_t rb;       /**< Live ring buffer over cfg.buffer */
    size_t       dropped;  /**< Lines dropped because the buffer was full */
} nx_log_t;

/**
 * @brief  Initialize a logger from @p cfg (no dynamic memory).
 *
 * @param  log  Logger handle, must not be NULL.
 * @param  cfg  Configuration; @c buffer and @c buffer_size (> 0) are required.
 *              @c write may be NULL to keep logs in the buffer only.
 *
 * @return true on success; false on invalid argument.
 */
bool nx_log_init(nx_log_t *log, const nx_log_cfg_t *cfg);

/**
 * @brief  Change the runtime minimum level. A no-op if @p log is NULL.
 */
void nx_log_set_level(nx_log_t *log, nx_log_level_t level);

/**
 * @brief  Format one line and enqueue it (usually called through the NX_LOGx macros).
 *
 * Returns immediately if @p level is below the runtime level. The line is
 * formatted on the caller's stack, then the enqueue is wrapped in the configured
 * lock. If the ring buffer cannot hold the whole line, the configured on_full
 * policy applies: either the new line is dropped (and the dropped counter is
 * incremented), or the oldest whole lines are evicted to make room.
 *
 * @param  log   Logger handle.
 * @param  level Severity of this message.
 * @param  file  Source file (typically __FILE__).
 * @param  line  Source line (typically __LINE__).
 * @param  fmt   printf-style format string.
 *
 * @return true if the whole line was enqueued; false if filtered, dropped, or on
 *         invalid argument. With NX_LOG_ON_FULL_OVERWRITE_OLD a line that fits in
 *         the empty buffer always enqueues (older lines are evicted).
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 5, 6)))
#endif
bool nx_log_write(nx_log_t *log, nx_log_level_t level,
                  const char *file, int line, const char *fmt, ...);

/**
 * @brief  Drain queued bytes to the sink. Call from the main loop.
 *
 * A no-op returning 0 when no sink is configured (cfg.write == NULL); use
 * nx_log_read to pull the bytes out instead.
 *
 * Each chunk is copied out of the ring under the configured lock, then handed to
 * the sink outside it: the consume stays atomic against a producer (including one
 * in an ISR) while the slow, possibly blocking sink never runs inside the lock,
 * so a producer never waits on I/O.
 *
 * @param  log Logger handle.
 *
 * @return The number of bytes handed to the sink this call.
 */
size_t nx_log_process(nx_log_t *log);

/**
 * @brief  Copy queued bytes out of the buffer, removing them.
 *
 * The pull-model counterpart to nx_log_process: useful when there is no sink
 * (cfg.write == NULL) and the caller reads the accumulated log on demand (a
 * debug shell, a diagnostic command). The enqueue-side lock is held while
 * reading so a concurrent producer stays safe.
 *
 * @param  log Logger handle.
 * @param  dst Destination buffer; may be NULL to discard the bytes.
 * @param  max Maximum number of bytes to copy.
 *
 * @return The number of bytes actually read/removed (0 .. max).
 */
size_t nx_log_read(nx_log_t *log, void *dst, size_t max);

/**
 * @brief  Return the number of lines dropped so far because the buffer was full.
 *         A NULL handle is treated as 0.
 */
size_t nx_log_dropped(const nx_log_t *log);

/** @cond internal */
#define NX_LOG__EMIT(log, lvl, ...)                                  \
    do {                                                             \
        if ((lvl) <= NX_LOG_COMPILE_LEVEL) {                         \
            nx_log_write((log), (lvl), __FILE__, __LINE__,           \
                         __VA_ARGS__);                               \
        }                                                            \
    } while (0)
/** @endcond */

/** Log at ERROR level. */
#define NX_LOGE(log, ...) NX_LOG__EMIT((log), NX_LOG_LEVEL_ERROR, __VA_ARGS__)
/** Log at WARN level. */
#define NX_LOGW(log, ...) NX_LOG__EMIT((log), NX_LOG_LEVEL_WARN,  __VA_ARGS__)
/** Log at INFO level. */
#define NX_LOGI(log, ...) NX_LOG__EMIT((log), NX_LOG_LEVEL_INFO,  __VA_ARGS__)
/** Log at DEBUG level. */
#define NX_LOGD(log, ...) NX_LOG__EMIT((log), NX_LOG_LEVEL_DEBUG, __VA_ARGS__)
/** Log at TRACE level. */
#define NX_LOGT(log, ...) NX_LOG__EMIT((log), NX_LOG_LEVEL_TRACE, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* NX_LOG_H */
