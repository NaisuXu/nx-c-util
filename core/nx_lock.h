/**
 * @file    nx_lock.h
 * @brief   Pluggable critical-section abstraction, in pure C.
 *
 * The other modules keep no locks and leave synchronization to the caller. This
 * header provides it portably: a pair of "enter / exit critical section"
 * function pointers the caller fills in with the primitive best suited to the
 * target, then wraps around the short operations that need protecting (a queue
 * push/pop, a pool alloc/free, a refcount change).
 *
 * enter returns an implementation-defined "saved state" that the matching exit
 * must be given back, so nested critical sections restore correctly. On a
 * bare-metal MCU, enter typically saves the interrupt-enable state and disables
 * interrupts, and exit restores exactly that state:
 *
 *     uintptr_t s = nx_lock_enter(&g_lock);
 *     nx_queue_push(&q, &item);
 *     nx_lock_exit(&g_lock, s);
 *
 * A NULL lock (or a lock with a NULL enter/exit) is a no-op that returns 0, so
 * the same call sites compile to nothing on a single-threaded build.
 */
#ifndef NX_LOCK_H
#define NX_LOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Critical-section primitives supplied by the caller.
 *
 * Fill this in once, at startup, with the enter/exit pair for your platform,
 * then pass its address to nx_lock_enter / nx_lock_exit around the operations
 * you want to protect.
 *
 * @note enter and exit must be a matching pair. exit must undo exactly what
 *       enter did, using the saved state enter returned, so that nested critical
 *       sections behave correctly.
 */
typedef struct nx_lock {
    /**
     * @brief Enter a critical section.
     * @param ctx  The @c ctx field below, passed through unchanged.
     * @return An implementation-defined saved state to hand back to @c exit
     *         (e.g. the previous interrupt-enable state). Return 0 if unused.
     */
    uintptr_t (*enter)(void *ctx);

    /**
     * @brief Leave a critical section, restoring the state @c enter returned.
     * @param ctx    The @c ctx field below, passed through unchanged.
     * @param saved  The value previously returned by @c enter.
     */
    void (*exit)(void *ctx, uintptr_t saved);

    /**
     * @brief Optional user context passed to enter/exit (e.g. a mutex object
     *        pointer). May be NULL when the primitives need no context (as for
     *        a bare-metal interrupt-disable).
     */
    void *ctx;
} nx_lock_t;

/**
 * @brief  Enter the critical section described by @p lock.
 *
 * @param  lock Lock handle; may be NULL (or have a NULL @c enter), in which case
 *              this is a no-op returning 0 - convenient for single-threaded builds.
 * @return The saved state to pass to nx_lock_exit (0 when @p lock is a no-op).
 */
static inline uintptr_t nx_lock_enter(const nx_lock_t *lock)
{
    if (lock == NULL || lock->enter == NULL) {
        return 0u;
    }
    return lock->enter(lock->ctx);
}

/**
 * @brief  Leave the critical section entered with nx_lock_enter.
 *
 * @param  lock  Lock handle; may be NULL (or have a NULL @c exit), in which case
 *               this is a no-op. Must be the same lock passed to nx_lock_enter.
 * @param  saved The value returned by the matching nx_lock_enter call.
 */
static inline void nx_lock_exit(const nx_lock_t *lock, uintptr_t saved)
{
    if (lock == NULL || lock->exit == NULL) {
        return;
    }
    lock->exit(lock->ctx, saved);
}

#ifdef __cplusplus
}
#endif

#endif /* NX_LOCK_H */
