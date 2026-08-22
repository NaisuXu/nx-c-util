/**
 * @file    nx_event_flags.h
 * @brief   Polled event flags: 32 independent bits with optional atomicity.
 *
 * @author  Claude
 * @date    2025-01-15
 *
 * A lightweight, non-blocking event-group mechanism for signaling between modules
 * in a cooperative main loop, and from ISRs into the main loop. Each bit is an
 * independent flag; the caller assigns meaning to each one.
 *
 * No blocking wait, no scheduler dependency: poll with test/take, set from anywhere.
 * With a NULL lock, single-context only (main loop polling itself, no ISR). With a
 * lock, set/clear/take are atomic, ISR-safe, and preemption-safe.
 */

#ifndef NX_EVENT_FLAGS_H
#define NX_EVENT_FLAGS_H

#include <stdint.h>
#include <stdbool.h>
#include "nx_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event flags: 32 independent bits, each with a caller-assigned meaning.
 *
 * The structure is small enough (8 bytes, aligned) to pass by value on many
 * platforms, but pass by pointer for consistency with other nx modules.
 */
typedef struct {
    volatile uint32_t bits;   /**< The flag word; written under lock, read anytime */
    const nx_lock_t  *lock;   /**< NULL = single-context (no ISR, no preemption) */
} nx_event_flags_t;

/**
 * @brief  Initialize an event flags instance.
 *
 * All bits start clear. The lock is optional: pass NULL for single-context use
 * (main loop only, no ISR); pass a valid lock for ISR-safe / preemption-safe
 * set/clear/take operations.
 *
 * @param  ef   Instance to initialize, must not be NULL.
 * @param  lock Optional lock for atomicity; NULL disables locking.
 */
static inline void nx_event_flags_init(nx_event_flags_t *ef, const nx_lock_t *lock)
{
    ef->bits = 0u;
    ef->lock = lock;
}

/**
 * @brief  Set (raise) one or more flags.
 *
 * Each bit in @p mask that is 1 becomes set; bits that are 0 in @p mask are
 * left unchanged. Multiple set() calls before a take() coalesce: the flag stays
 * raised until explicitly cleared or taken.
 *
 * @param  ef   Instance, must not be NULL.
 * @param  mask Bits to set.
 */
static inline void nx_event_flags_set(nx_event_flags_t *ef, uint32_t mask)
{
    if (ef->lock != NULL) {
        uintptr_t state = ef->lock->enter(ef->lock->ctx);
        ef->bits |= mask;
        ef->lock->exit(ef->lock->ctx, state);
    } else {
        ef->bits |= mask;
    }
}

/**
 * @brief  Clear (lower) one or more flags.
 *
 * Each bit in @p mask that is 1 becomes cleared; bits that are 0 in @p mask are
 * left unchanged.
 *
 * @param  ef   Instance, must not be NULL.
 * @param  mask Bits to clear.
 */
static inline void nx_event_flags_clear(nx_event_flags_t *ef, uint32_t mask)
{
    if (ef->lock != NULL) {
        uintptr_t state = ef->lock->enter(ef->lock->ctx);
        ef->bits &= ~mask;
        ef->lock->exit(ef->lock->ctx, state);
    } else {
        ef->bits &= ~mask;
    }
}

/**
 * @brief  Test whether any of the given flags are set.
 *
 * Non-consuming: the flags stay raised. Use this for broadcast events that
 * multiple modules need to see. Returns true if (bits & mask) != 0, i.e., at
 * least one bit in @p mask is currently set.
 *
 * @param  ef   Instance, must not be NULL.
 * @param  mask Bits to test.
 * @return true if any bit in @p mask is set; false if all are clear.
 */
static inline bool nx_event_flags_test(const nx_event_flags_t *ef, uint32_t mask)
{
    return (ef->bits & mask) != 0u;
}

/**
 * @brief  Test whether all of the given flags are set.
 *
 * Non-consuming: the flags stay raised. Use this for barrier / synchronization
 * scenarios where you wait for multiple modules to each set their own ack bit.
 * Returns true if (bits & mask) == mask, i.e., every bit in @p mask is set.
 *
 * @param  ef   Instance, must not be NULL.
 * @param  mask Bits to test.
 * @return true if all bits in @p mask are set; false otherwise.
 */
static inline bool nx_event_flags_test_all(const nx_event_flags_t *ef, uint32_t mask)
{
    return (ef->bits & mask) == mask;
}

/**
 * @brief  Test and atomically clear the given flags.
 *
 * Consuming: if any bit in @p mask is set, clears all of them and returns true.
 * If none are set, returns false and leaves the flags unchanged. Use this for
 * one-shot work items where only one consumer should handle each occurrence.
 *
 * The test + clear is atomic when a lock is configured, so an ISR can set a
 * flag while the main loop is taking it, and neither update is lost.
 *
 * @param  ef   Instance, must not be NULL.
 * @param  mask Bits to test and clear.
 * @return true if any bit in @p mask was set (and has now been cleared);
 *         false if all were already clear.
 */
static inline bool nx_event_flags_take(nx_event_flags_t *ef, uint32_t mask)
{
    bool raised;
    if (ef->lock != NULL) {
        uintptr_t state = ef->lock->enter(ef->lock->ctx);
        raised = (ef->bits & mask) != 0u;
        if (raised) {
            ef->bits &= ~mask;
        }
        ef->lock->exit(ef->lock->ctx, state);
    } else {
        raised = (ef->bits & mask) != 0u;
        if (raised) {
            ef->bits &= ~mask;
        }
    }
    return raised;
}

/**
 * @brief  Read the entire flag word.
 *
 * For debugging and logging: capture the current state as a 32-bit snapshot.
 * The value may be stale by the time the caller examines it; this is expected.
 *
 * @param  ef Instance, must not be NULL.
 * @return The current value of all 32 flags.
 */
static inline uint32_t nx_event_flags_get(const nx_event_flags_t *ef)
{
    return ef->bits;
}

#ifdef __cplusplus
}
#endif

#endif /* NX_EVENT_FLAGS_H */
