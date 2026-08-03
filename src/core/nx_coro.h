/**
 * @file    nx_coro.h
 * @brief   Stackless coroutines for embedded systems, in pure C macros.
 *
 * A coroutine is an ordinary function that can suspend in the middle and resume
 * there on the next call, built on Duff's device and __LINE__. Nothing is saved
 * across a suspend point except one line number: no stack, no context switch,
 * no allocation. Coroutines never block - they return, and the caller's main
 * loop calls them again.
 *
 * Restrictions, all from the switch-based implementation:
 * - Locals do not survive a suspend point; persistent state goes in the struct.
 * - No switch statement of your own between BEGIN and END.
 * - At most one suspend point per source line (the labels are __LINE__).
 * - Suspend points must be lexically between BEGIN and END, same function.
 * - Code before NX_CORO_BEGIN runs on every call, not just the first.
 *
 * A resume point expands to `lc = __LINE__; case __LINE__:`, which GCC/Clang
 * read as a case falling through. It cannot happen - the switch only jumps to
 * those labels - so build with -Wno-implicit-fallthrough under -Wextra.
 */
#ifndef NX_CORO_H
#define NX_CORO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return status of one call into a coroutine; running states sort below finished ones. */
typedef enum {
    NX_CORO_WAITING = 0,  /**< Suspended on a condition, sleep, or timeout */
    NX_CORO_YIELDED = 1,  /**< Suspended by an explicit NX_CORO_YIELD */
    NX_CORO_EXITED  = 2,  /**< Finished early via NX_CORO_EXIT */
    NX_CORO_ENDED   = 3   /**< Finished by falling off the end at NX_CORO_END */
} nx_coro_ret_t;

/** Coroutine state for yield and condition waits; caller-owned storage. */
typedef struct {
    uint16_t lc;               /**< Resume point: 0 = not started, else a __LINE__ */
} nx_coro_stack_t;

/** Coroutine state with a tick source; required by the time-based macros. */
typedef struct {
    uint16_t lc;               /**< Resume point: 0 = not started, else a __LINE__ */
    size_t (*get_tick)(void);  /**< Tick source; NULL disables the time macros */
    size_t   ticks;            /**< Reference timestamp of the pending sleep/timeout */
} nx_coro_stack_plus_t;

/** Reset an nx_coro_stack_t so the next call starts from the top. */
#define NX_CORO_INIT(coro_stack) \
    (coro_stack)->lc = 0;

/** Initialize an nx_coro_stack_plus_t and bind its tick source (NULL to disable). */
#define NX_CORO_INIT_PLUS(coro_stack, get_tick_func) \
    do { (coro_stack)->lc = 0; (coro_stack)->get_tick = (get_tick_func); } while(0)

/** Open the coroutine body; must come before any suspend point. */
#define NX_CORO_BEGIN(coro_stack) \
    switch((coro_stack)->lc) { case 0:

/** Close the coroutine body; returns NX_CORO_ENDED and resets the state. */
#define NX_CORO_END(coro_stack) \
    default: ; } (coro_stack)->lc = 0; return NX_CORO_ENDED;

/** Finish now from anywhere in the body; returns NX_CORO_EXITED and resets the state. */
#define NX_CORO_EXIT(coro_stack) \
    do { (coro_stack)->lc = 0; return NX_CORO_EXITED; } while(0)

/** Suspend unconditionally; resumes on the next statement on the next call. */
#define NX_CORO_YIELD(coro_stack) \
    do { (coro_stack)->lc = __LINE__; return NX_CORO_YIELDED; case __LINE__: ; } while(0)

/** Suspend until a condition holds; tested on arrival, so an already-true one does not suspend. */
#define NX_CORO_WAIT_UNTIL(coro_stack, condition) \
    do { (coro_stack)->lc = __LINE__; case __LINE__: if(!(condition)) return NX_CORO_WAITING; } while(0)

/** Suspend while a condition holds; the inverse of NX_CORO_WAIT_UNTIL. */
#define NX_CORO_WAIT_WHILE(coro_stack, condition) \
    NX_CORO_WAIT_UNTIL(coro_stack, !(condition))

/** Call a coroutine and report whether it is still running. Wrap the call, not a saved status. */
#define NX_CORO_SCHEDULE(coro_expr) \
    ((coro_expr) < NX_CORO_EXITED)

/** Suspend for at least `delay` ticks; the wait can only end on a call. */
#define NX_CORO_SLEEP(coro_stack, delay) \
    do { \
        if ((coro_stack)->get_tick != NULL) { \
            (coro_stack)->ticks = (coro_stack)->get_tick(); \
            NX_CORO_WAIT_UNTIL(coro_stack, ((coro_stack)->get_tick() - (coro_stack)->ticks) >= (delay)); \
        } \
    } while(0)

/** Stamp the current tick as the reference for a later NX_CORO_TIMEDWAIT. */
#define NX_CORO_TIMEDSET(coro_stack) \
    do { if ((coro_stack)->get_tick != NULL) { (coro_stack)->ticks = (coro_stack)->get_tick(); } } while(0)

/** Suspend until `delay` ticks have elapsed since NX_CORO_TIMEDSET; does not re-stamp. */
#define NX_CORO_TIMEDWAIT(coro_stack, delay) \
    do { \
        if ((coro_stack)->get_tick != NULL) { \
            NX_CORO_WAIT_UNTIL(coro_stack, ((coro_stack)->get_tick() - (coro_stack)->ticks) >= (delay)); \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* NX_CORO_H */
