#ifndef NX_CORO_H
#define NX_CORO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NX_CORO_WAITING = 0,
  NX_CORO_YIELDED = 1,
  NX_CORO_EXITED  = 2,
  NX_CORO_ENDED   = 3
} nx_coro_ret_t;

typedef struct {
  uint16_t lc;
  size_t ticks;
  size_t (*get_tick)(void);
} nx_coro_stack_t;

#define NX_CORO_INIT(coro_stack, get_tick_func) do { (coro_stack)->lc = 0; (coro_stack)->get_tick = (get_tick_func); } while(0)

#define NX_CORO_BEGIN(coro_stack) switch((coro_stack)->lc) { case 0:

#define NX_CORO_END(coro_stack) default: ; } (coro_stack)->lc = 0; return NX_CORO_ENDED;

#define NX_CORO_EXIT(coro_stack) do { (coro_stack)->lc = 0; return NX_CORO_EXITED; } while(0)

#define NX_CORO_YIELD(coro_stack) do { (coro_stack)->lc = __LINE__; return NX_CORO_YIELDED; case __LINE__: ; } while(0)

#define NX_CORO_WAIT_UNTIL(coro_stack, condition) do { (coro_stack)->lc = __LINE__; case __LINE__: if(!(condition)) return NX_CORO_WAITING; } while(0)

#define NX_CORO_WAIT_WHILE(coro_stack, condition) NX_CORO_WAIT_UNTIL(coro_stack, !(condition))

#define NX_CORO_SLEEP(coro_stack, ticks) do { if ((coro_stack)->get_tick != NULL) { (coro_stack)->ticks = (coro_stack)->get_tick(); NX_CORO_WAIT_UNTIL(coro_stack, (coro_stack)->get_tick() - (coro_stack)->ticks >= ticks); } } while(0)

#define NX_CORO_TIMEDSET(coro_stack) do { if ((coro_stack)->get_tick != NULL) { (coro_stack)->ticks = (coro_stack)->get_tick(); } } while(0)

#define NX_CORO_TIMEDWAIT(coro_stack, ticks) do { if ((coro_stack)->get_tick != NULL) { NX_CORO_WAIT_UNTIL(coro_stack, (coro_stack)->get_tick() - (coro_stack)->ticks >= ticks); } } while(0)

#define NX_CORO_SCHEDULE(coro_stack_expr)	((coro_stack_expr) < NX_CORO_ENDED)

#define NX_CORO_SPAWN(coro_stack, sub_coro_stack, coro_stack_expr) do { NX_CORO_INIT(sub_coro_stack, (coro_stack)->get_tick); NX_CORO_WAIT_WHILE((coro_stack), NX_CORO_SCHEDULE(coro_stack_expr)); } while(0)

#ifdef __cplusplus
}
#endif

#endif /* NX_CORO_H */
