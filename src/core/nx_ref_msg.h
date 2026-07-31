/**
 * @file    nx_ref_msg.h
 * @brief   Reference-counted messages with multi-queue publish, in pure C.
 *
 * A zero-copy message dispatch layer: a message is allocated once and delivered to
 * multiple queues; what a queue stores is a pointer to the message, so consumers
 * share the same data. Lifetime is managed by a reference count: each successful
 * enqueue increments it, each consumer calls release() when done, and the block
 * returns to the pool when the count hits zero.
 *
 * Memory layout: the message header and data buffer are a single block; the data
 * is a flexible array member aligned to max_align_t.
 *
 * Reference-count convention:
 *   - alloc() returns a message with refcount 1 (the producer reference).
 *   - Each successful publish increments the refcount.
 *   - After publishing, the producer calls release() once to drop its reference.
 *   - A message delivered to no queue still reaches refcount 0 and is freed.
 *
 * Thread safety: the reference count is a plain counter (not atomic). Concurrent
 * access to the same message must be serialized by the caller.
 */
#ifndef NX_REF_MSG_H
#define NX_REF_MSG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/nx_queue.h"
#include "core/nx_tiered_mem_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return codes for module operations.
 */
typedef enum {
    NX_REF_MSG_OK = 0,         /**< Operation succeeded */
    NX_REF_MSG_ERR_PARAM,      /**< Invalid argument (NULL pointer, etc.) */
    NX_REF_MSG_ERR_NOMEM,      /**< Memory-pool allocation failed */
    NX_REF_MSG_ERR_FULL,       /**< Target queue is full (single publish); or, from publish_multi, no queue accepted the message */
    NX_REF_MSG_ERR_STATE,      /**< release() called on a message whose refcount is already 0 */
    NX_REF_MSG_PARTIAL         /**< publish_multi: delivered to some, but not all, queues (some were full) */
} nx_ref_msg_ret_t;

/**
 * @brief Reference-counted message object.
 *
 * @note  Members are implementation details; do not access them directly, use
 *        nx_ref_msg_data()/_len(). The header and data are one
 *        contiguous allocation; data is a flexible array member.
 */
typedef struct nx_ref_msg {
    nx_tiered_mem_pool_t *pool;      /**< Source pool (the whole block is alloc'd from / returned to it) */
    size_t                len;       /**< Data length in bytes */
    size_t                refcount;  /**< Reference count (plain counter, not atomic) */
    _Alignas(max_align_t) uint8_t data[];  /**< Data buffer, right after the header, max_align_t aligned */
} nx_ref_msg_t;

/**
 * @brief  Convenience initializer for a queue that carries reference messages.
 *
 * A reference-message queue stores nx_ref_msg_t* pointers, so its element size is
 * fixed to sizeof(nx_ref_msg_t*). This wrapper encapsulates that, preventing the
 * caller from using the wrong element_size.
 *
 * The on-full policy is fixed to NX_QUEUE_ON_FULL_REJECT: OVERWRITE must not be
 * used, because on a full queue it would silently drop the oldest message pointer,
 * leaving that message's reference forever un-released - a reference leak and a
 * memory leak. When a queue is full, nx_ref_msg_publish returns ERR_FULL and lets
 * the producer decide.
 *
 * @param  q        Queue handle, must not be NULL.
 * @param  buffer   Caller-provided storage, size must be >= capacity * sizeof(nx_ref_msg_t*).
 * @param  capacity Queue capacity (number of message pointers), must be > 0.
 *
 * @return Same return codes as nx_queue_init().
 */
nx_queue_ret_t nx_ref_msg_queue_init(nx_queue_t *q,
                                     void       *buffer,
                                     size_t      capacity);

/**
 * @brief  Allocate a message (header + @p len bytes of data, single allocation).
 *
 * Allocates "sizeof(nx_ref_msg_t) + len" bytes from @p pool in one shot; the data
 * buffer is the object's flexible array, max_align_t aligned, safe for any type.
 * The new message has refcount 1 (producer reference). Data is uninitialized (like
 * malloc), to be filled by the caller.
 *
 * @param  pool Pool handle, must not be NULL.
 * @param  len  Data length in bytes, must be > 0.
 *
 * @return Message pointer; NULL on invalid argument or out of memory.
 */
nx_ref_msg_t *nx_ref_msg_alloc(nx_tiered_mem_pool_t *pool, size_t len);

/**
 * @brief  Return the (writable) data buffer address. NULL for a NULL message.
 */
static inline void *nx_ref_msg_data(const nx_ref_msg_t *msg)
{
    /* Cast away const to return a writable buffer: data is part of the object's own storage. */
    return (msg != NULL) ? (void *)msg->data : NULL;
}

/**
 * @brief  Return the message data length in bytes. 0 for a NULL message.
 */
static inline size_t nx_ref_msg_len(const nx_ref_msg_t *msg)
{
    return (msg != NULL) ? msg->len : 0u;
}

/**
 * @brief  Shrink a message's data length; can only get smaller, never larger.
 *
 * Only @c len changes (no memory is freed); growing is rejected, not clamped.
 * The block is fixed at alloc time, so this only trims the reported length.
 *
 * @param  msg     Message handle, must not be NULL.
 * @param  new_len New data length; must be <= the current length.
 *
 * @return NX_REF_MSG_OK on success; NX_REF_MSG_ERR_PARAM if @p msg is NULL or
 *         @p new_len exceeds the current length.
 */
static inline nx_ref_msg_ret_t nx_ref_msg_shrink(nx_ref_msg_t *msg, size_t new_len)
{
    if (msg == NULL || new_len > msg->len) {
        return NX_REF_MSG_ERR_PARAM;   /* NULL, or an attempt to grow */
    }

    msg->len = new_len;
    return NX_REF_MSG_OK;
}

/**
 * @brief  Publish the message to a single queue (enqueues one nx_ref_msg_t* pointer).
 *
 * The reference count is incremented only on a successful enqueue; if the queue is
 * full it returns NX_REF_MSG_ERR_FULL and leaves the count unchanged.
 *
 * @param  msg Message handle.
 * @param  q   Target queue (should be initialized by nx_ref_msg_queue_init).
 *
 * @return NX_REF_MSG_OK on success; NX_REF_MSG_ERR_FULL if full; NX_REF_MSG_ERR_PARAM on invalid argument.
 */
nx_ref_msg_ret_t nx_ref_msg_publish(nx_ref_msg_t *msg, nx_queue_t *q);

/**
 * @brief  Release one reference: refcount -1; when it reaches 0 the whole block is
 *         returned to the memory pool.
 *
 * @param  msg Message handle.
 *
 * @return NX_REF_MSG_OK on success (may already be freed);
 *         NX_REF_MSG_ERR_PARAM if msg is NULL;
 *         NX_REF_MSG_ERR_STATE if the refcount is already 0 (double release).
 */
nx_ref_msg_ret_t nx_ref_msg_release(nx_ref_msg_t *msg);

/**
 * @brief  Publish the message to multiple queues in one call (best-effort).
 *
 * @p queues is a NULL-terminated array: nx_ref_msg_publish is called for each
 * entry up to (but not including) the first NULL. Delivery is best-effort: each
 * successful enqueue does refcount +1; a full queue is skipped and holds no
 * reference, and the remaining queues are still attempted. The queue set is
 * organized by the caller; this module keeps no subscription table.
 *
 * The return code reflects the aggregate outcome, so a caller that inspects only
 * the return value still learns whether anything was dropped:
 *   - all queues accepted the message (or the list is empty) -> NX_REF_MSG_OK;
 *   - some, but not all, accepted it                          -> NX_REF_MSG_PARTIAL;
 *   - a non-empty list where no queue accepted it             -> NX_REF_MSG_ERR_FULL.
 *
 * @p out_delivered (optional) receives the number of successful deliveries.
 * @p out_first_failed (optional) receives the index of the FIRST queue that was
 * full; when nothing failed it is set to the number of queues (i.e. one past the
 * last valid index). For finer detail (every failing queue), call
 * nx_ref_msg_publish per queue and inspect each return value directly.
 *
 * @warning The array MUST end with a NULL entry. Because there is no count, a
 *          missing terminator makes this function read past the end of the array
 *          (undefined behavior). Consequently NULL cannot be used as a mid-array
 *          "skip" placeholder - the first NULL ends the list.
 *
 * @param  msg              Message handle, must not be NULL.
 * @param  queues           NULL-terminated array of queue pointers, must not be NULL.
 * @param  out_delivered    May be NULL; if non-NULL, receives the number of successful deliveries.
 * @param  out_first_failed May be NULL; if non-NULL, receives the index of the first
 *                          full queue, or the queue count if none failed.
 *
 * @return NX_REF_MSG_OK if every queue accepted the message (including an empty list);
 *         NX_REF_MSG_PARTIAL if some but not all accepted it;
 *         NX_REF_MSG_ERR_FULL if a non-empty list had no successful delivery;
 *         NX_REF_MSG_ERR_PARAM if msg / queues is NULL.
 */
nx_ref_msg_ret_t nx_ref_msg_publish_multi(nx_ref_msg_t      *msg,
                                          nx_queue_t *const *queues,
                                          size_t            *out_delivered,
                                          size_t            *out_first_failed);

#ifdef __cplusplus
}
#endif

#endif /* NX_REF_MSG_H */
