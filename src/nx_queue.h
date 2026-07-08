/**
 * @file    nx_queue.h
 * @brief   A generic ring-buffer (FIFO) queue implemented in pure C.
 *
 * Design goals: aimed at embedded development - simple, predictable, no hidden overhead.
 *
 * Features:
 *   - Generic element type: stores elements of any size, measured in bytes (element_size).
 *   - Fixed capacity: capacity is fixed at init time and never grows at runtime.
 *   - Purely static buffer: storage is provided entirely by the caller; this library uses
 *     no dynamic memory and does not depend on malloc/free - suitable for heap-less targets
 *     or scenarios requiring a deterministic memory layout.
 *   - In a single-producer/single-consumer scenario, where one side only pushes and the
 *     other only pops, it is naturally thread-safe; other concurrent scenarios require the
 *     caller to add its own locking (this library introduces no locks).
 */
#ifndef NX_QUEUE_H
#define NX_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return codes for queue operations.
 */
typedef enum {
    NX_QUEUE_OK = 0,        /**< Operation succeeded */
    NX_QUEUE_ERR_PARAM,     /**< Invalid argument (NULL pointer, zero size, etc.) */
    NX_QUEUE_ERR_EMPTY,     /**< Queue is empty */
    NX_QUEUE_ERR_FULL       /**< Queue is full */
} nx_queue_ret_t;

/**
 * @brief Policy applied when pushing into a full queue.
 */
typedef enum {
    NX_QUEUE_ON_FULL_REJECT = 0,   /**< Reject new data when full: push returns NX_QUEUE_ERR_FULL, queue unchanged */
    NX_QUEUE_ON_FULL_OVERWRITE     /**< Overwrite oldest data when full: drop the front element, write the new one */
} nx_queue_on_full_t;

/**
 * @brief Ring-buffer queue handle.
 *
 * @note  The struct members are implementation details; do not access them directly,
 *        use the provided API instead.
 */
typedef struct {
    uint8_t           *buffer;        /**< Underlying byte buffer (caller-provided), sized capacity * element_size */
    size_t             element_size;  /**< Size of a single element in bytes */
    size_t             capacity;      /**< Maximum number of elements that can be stored */
    size_t             head;          /**< Index of the front element (dequeue position) */
    size_t             count;         /**< Current number of elements */
    nx_queue_on_full_t on_full;       /**< Policy applied when the queue is full */
} nx_queue_t;

/**
 * @brief  Initialize a queue with a caller-provided buffer (uses no dynamic memory).
 *
 * Typical usage: statically allocate storage for the queue, then hand it to this function.
 * @code
 *   static uint8_t buf[16 * sizeof(int)];
 *   nx_queue_t q;
 *   nx_queue_init(&q, buf, sizeof(int), 16, NX_QUEUE_ON_FULL_REJECT);
 * @endcode
 *
 * @param  q            Queue handle, must not be NULL.
 * @param  buffer       Caller-provided storage; its size must be >= capacity * element_size.
 * @param  element_size Size of a single element in bytes, must be > 0.
 * @param  capacity     Queue capacity (number of elements), must be > 0.
 * @param  on_full      Policy applied when the queue is full:
 *                        - NX_QUEUE_ON_FULL_REJECT: reject new data (push returns NX_QUEUE_ERR_FULL);
 *                        - NX_QUEUE_ON_FULL_OVERWRITE: overwrite the oldest data in the queue.
 *
 * @return NX_QUEUE_OK on success; otherwise the corresponding error code.
 */
nx_queue_ret_t nx_queue_init(nx_queue_t        *q,
                             void              *buffer,
                             size_t             element_size,
                             size_t             capacity,
                             nx_queue_on_full_t on_full);

/**
 * @brief  Copy an element into the queue (copies element_size bytes).
 *
 * When the queue is full, behavior depends on the on_full policy set at init time:
 *   - NX_QUEUE_ON_FULL_REJECT: does not write, returns NX_QUEUE_ERR_FULL;
 *   - NX_QUEUE_ON_FULL_OVERWRITE: drops the oldest element then writes the new one, returns NX_QUEUE_OK.
 *
 * @param  q       Queue handle.
 * @param  element Pointer to the element to enqueue, must not be NULL.
 *
 * @return NX_QUEUE_OK on success; NX_QUEUE_ERR_FULL if full and policy is reject; NX_QUEUE_ERR_PARAM on invalid argument.
 */
nx_queue_ret_t nx_queue_push(nx_queue_t *q, const void *element);

/**
 * @brief  Dequeue an element, optionally copying it into out.
 *
 * @param  q   Queue handle.
 * @param  out Buffer to receive the element, size must be >= element_size; may be NULL to just discard the front element.
 *
 * @return NX_QUEUE_OK on success; NX_QUEUE_ERR_EMPTY if empty; NX_QUEUE_ERR_PARAM on invalid argument.
 */
nx_queue_ret_t nx_queue_pop(nx_queue_t *q, void *out);

/**
 * @brief  Peek at the front element without dequeuing it.
 *
 * @param  q   Queue handle.
 * @param  out Buffer to receive the element, size must be >= element_size, must not be NULL.
 *
 * @return NX_QUEUE_OK on success; NX_QUEUE_ERR_EMPTY if empty; NX_QUEUE_ERR_PARAM on invalid argument.
 */
nx_queue_ret_t nx_queue_peek(const nx_queue_t *q, void *out);

/**
 * @brief  Clear the queue (does not affect the caller's buffer), resetting it to empty.
 *
 * @param  q Queue handle.
 */
void nx_queue_clear(nx_queue_t *q);

/**
 * @brief  Return the current number of elements. A NULL pointer is treated as 0.
 */
size_t nx_queue_size(const nx_queue_t *q);

/**
 * @brief  Return the queue capacity (number of elements). A NULL pointer is treated as 0.
 */
size_t nx_queue_capacity(const nx_queue_t *q);

/**
 * @brief  Whether the queue is empty. A NULL pointer is treated as "empty".
 */
bool nx_queue_is_empty(const nx_queue_t *q);

/**
 * @brief  Whether the queue is full. A NULL pointer is treated as "full" (cannot be written to).
 */
bool nx_queue_is_full(const nx_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* NX_QUEUE_H */
