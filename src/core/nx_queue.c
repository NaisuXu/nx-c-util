/**
 * @file    nx_queue.c
 * @brief   Implementation of the nx_queue ring-buffer queue (purely static buffer, no dynamic memory).
 */
#include "nx_queue.h"

#include <string.h>

/**
 * @brief Compute the byte offset of the index-th logical element within the underlying buffer.
 */
static inline uint8_t *nx_queue_slot(const nx_queue_t *q, size_t index)
{
    return q->buffer + (index * q->element_size);
}

nx_queue_ret_t nx_queue_init(nx_queue_t        *q,
                             void              *buffer,
                             size_t             element_size,
                             size_t             capacity,
                             nx_queue_on_full_t on_full)
{
    if (q == NULL || buffer == NULL || element_size == 0U || capacity == 0U) {
        return NX_QUEUE_ERR_PARAM;
    }

    q->buffer       = (uint8_t *)buffer;
    q->element_size = element_size;
    q->capacity     = capacity;
    q->head         = 0U;
    q->count        = 0U;
    q->on_full      = on_full;

    return NX_QUEUE_OK;
}

nx_queue_ret_t nx_queue_push(nx_queue_t *q, const void *element)
{
    if (q == NULL || q->buffer == NULL || element == NULL) {
        return NX_QUEUE_ERR_PARAM;
    }
    if (q->count == q->capacity) {
        if (q->on_full == NX_QUEUE_ON_FULL_REJECT) {
            return NX_QUEUE_ERR_FULL;
        }
        /* NX_QUEUE_ON_FULL_OVERWRITE: drop the oldest element to free a slot. */
        q->head++;
        if (q->head >= q->capacity) {
            q->head -= q->capacity;
        }
        q->count--;
    }

    /* Tail position = (head + count) modulo capacity. */
    size_t tail = q->head + q->count;
    if (tail >= q->capacity) {
        tail -= q->capacity;
    }

    memcpy(nx_queue_slot(q, tail), element, q->element_size);
    q->count++;

    return NX_QUEUE_OK;
}

nx_queue_ret_t nx_queue_pop(nx_queue_t *q, void *out)
{
    if (q == NULL || q->buffer == NULL) {
        return NX_QUEUE_ERR_PARAM;
    }
    if (q->count == 0U) {
        return NX_QUEUE_ERR_EMPTY;
    }

    if (out != NULL) {
        memcpy(out, nx_queue_slot(q, q->head), q->element_size);
    }

    q->head++;
    if (q->head >= q->capacity) {
        q->head -= q->capacity;
    }
    q->count--;

    return NX_QUEUE_OK;
}

nx_queue_ret_t nx_queue_peek(const nx_queue_t *q, void *out)
{
    if (q == NULL || q->buffer == NULL || out == NULL) {
        return NX_QUEUE_ERR_PARAM;
    }
    if (q->count == 0U) {
        return NX_QUEUE_ERR_EMPTY;
    }

    memcpy(out, nx_queue_slot(q, q->head), q->element_size);

    return NX_QUEUE_OK;
}

void nx_queue_clear(nx_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    q->head  = 0U;
    q->count = 0U;
}
