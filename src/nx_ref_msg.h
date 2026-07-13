/**
 * @file    nx_ref_msg.h
 * @brief   Reference-counted messages with multi-queue publish, in pure C.
 *
 * This module builds a zero-copy message dispatch layer on top of two existing
 * components:
 *   - Memory comes from nx_tiered_mem_pool (tiered static memory pool).
 *   - Messages are delivered into nx_queue (ring buffer).
 *
 * Core idea: a message is allocated once and may be delivered to several queues;
 * what a queue stores is a "pointer to the message object", not the message body,
 * so multiple consumers share the same data (zero copy). Lifetime is managed by a
 * reference count: each successful enqueue does +1, each consumer does release()
 * (-1) when done, and the whole block is returned to the pool when the count hits 0.
 *
 * To publish to several queues, the caller prepares its own array of nx_queue_t*
 * and uses publish_multi to deliver in one shot - the queue set is organized by
 * the caller; this module keeps no subscription table.
 *
 * Memory layout: the message header and the data buffer are a single block from
 * one alloc; the data is a flexible array member right after the header, aligned
 * to max_align_t so it can safely hold any type.
 *
 * Reference-count convention (important):
 *   - The message returned by nx_ref_msg_alloc() has refcount 1, the "producer
 *     reference".
 *   - Each successful publish (enqueue) does refcount +1.
 *   - After publishing, the producer should release() the message once to give up
 *     its own reference.
 *   - This way, even a message delivered to no queue (0 subscribers) still reaches
 *     0 and is freed, with no leak.
 *
 * Thread safety: this module introduces no locks; the reference count is a plain
 * counter (not atomic). Concurrent access to the same message must be serialized
 * by the caller, consistent with nx_queue / nx_tiered_mem_pool.
 */
#ifndef NX_REF_MSG_H
#define NX_REF_MSG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_queue.h"
#include "nx_tiered_mem_pool.h"

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
    NX_REF_MSG_ERR_FULL,       /**< Target queue is full */
    NX_REF_MSG_ERR_STATE       /**< release() called on a message whose refcount is already 0 */
} nx_ref_msg_ret_t;

/**
 * @brief Reference-counted message object.
 *
 * @note  Members are implementation details; do not access them directly, use
 *        nx_ref_msg_data()/_len()/_refcount(). The header and data are one
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
 * @brief  Return the message's current reference count. 0 for a NULL message.
 */
static inline size_t nx_ref_msg_refcount(const nx_ref_msg_t *msg)
{
    return (msg != NULL) ? msg->refcount : 0u;
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
 * @brief  Publish the message to multiple queues in one call.
 *
 * Calls nx_ref_msg_publish for each queue in @p queues: each successful enqueue
 * does refcount +1; a full queue (or a NULL entry) is skipped and holds no
 * reference. The queue set is organized by the caller; this module keeps no
 * subscription table. @p out_delivered returns the number of successful deliveries.
 *
 * @param  msg           Message handle, must not be NULL.
 * @param  queues        Array of queue pointers, must not be NULL; individual entries may be NULL (skipped).
 * @param  count         Array length.
 * @param  out_delivered May be NULL; if non-NULL, receives the number of successful deliveries.
 *
 * @return NX_REF_MSG_OK on success (including delivering to 0 queues);
 *         NX_REF_MSG_ERR_PARAM if msg / queues is NULL.
 */
nx_ref_msg_ret_t nx_ref_msg_publish_multi(nx_ref_msg_t      *msg,
                                          nx_queue_t *const *queues,
                                          size_t             count,
                                          size_t            *out_delivered);

#ifdef __cplusplus
}
#endif

#endif /* NX_REF_MSG_H */
