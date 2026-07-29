/**
 * @file    nx_ringbuf.c
 * @brief   Implementation of the nx_ringbuf byte ring buffer (purely static buffer, no dynamic memory).
 */
#include "nx_ringbuf.h"

#include <string.h>

/* PLACEHOLDER_BODY */

bool nx_ringbuf_init(nx_ringbuf_t *rb, void *buffer, size_t capacity)
{
    if (rb == NULL || buffer == NULL || capacity == 0U) {
        return false;
    }

    rb->buffer   = (uint8_t *)buffer;
    rb->capacity = capacity;
    rb->read_pos = 0U;
    rb->count    = 0U;

    return true;
}

size_t nx_ringbuf_write(nx_ringbuf_t *rb, const void *src, size_t len)
{
    if (rb == NULL || rb->buffer == NULL || (src == NULL && len > 0U)) {
        return 0U;
    }

    size_t free_space = rb->capacity - rb->count;
    if (len > free_space) {
        len = free_space;   /* partial write: only what fits */
    }
    if (len == 0U) {
        return 0U;
    }

    /* Write position = (read_pos + count) modulo capacity. */
    size_t wpos = rb->read_pos + rb->count;
    if (wpos >= rb->capacity) {
        wpos -= rb->capacity;
    }

    /* Up to the buffer end in one memcpy, the wrapped remainder in a second. */
    size_t first = rb->capacity - wpos;
    if (first > len) {
        first = len;
    }

    const uint8_t *in = (const uint8_t *)src;
    memcpy(rb->buffer + wpos, in, first);
    if (len > first) {
        memcpy(rb->buffer, in + first, len - first);
    }

    rb->count += len;
    return len;
}

/* Copy up to len bytes starting at read_pos into dst (no state change). */
static size_t nx_ringbuf_copy_out(const nx_ringbuf_t *rb, uint8_t *dst, size_t len)
{
    if (len > rb->count) {
        len = rb->count;   /* partial: only what is available */
    }
    if (len == 0U) {
        return 0U;
    }

    size_t first = rb->capacity - rb->read_pos;
    if (first > len) {
        first = len;
    }

    memcpy(dst, rb->buffer + rb->read_pos, first);
    if (len > first) {
        memcpy(dst + first, rb->buffer, len - first);
    }
    return len;
}

size_t nx_ringbuf_peek(const nx_ringbuf_t *rb, void *dst, size_t len)
{
    if (rb == NULL || rb->buffer == NULL || (dst == NULL && len > 0U)) {
        return 0U;
    }
    return nx_ringbuf_copy_out(rb, (uint8_t *)dst, len);
}

size_t nx_ringbuf_discard(nx_ringbuf_t *rb, size_t len)
{
    if (rb == NULL || rb->buffer == NULL) {
        return 0U;
    }
    if (len > rb->count) {
        len = rb->count;
    }

    rb->read_pos += len;
    if (rb->read_pos >= rb->capacity) {
        rb->read_pos -= rb->capacity;
    }
    rb->count -= len;
    return len;
}

size_t nx_ringbuf_read(nx_ringbuf_t *rb, void *dst, size_t len)
{
    if (rb == NULL || rb->buffer == NULL) {
        return 0U;
    }

    /* dst == NULL is allowed: read then just discards the bytes. */
    size_t n = (dst != NULL) ? nx_ringbuf_copy_out(rb, (uint8_t *)dst, len) : len;
    return nx_ringbuf_discard(rb, n);
}

void nx_ringbuf_clear(nx_ringbuf_t *rb)
{
    if (rb == NULL) {
        return;
    }
    rb->read_pos = 0U;
    rb->count    = 0U;
}

const uint8_t *nx_ringbuf_peek_linear(const nx_ringbuf_t *rb, size_t *out_len)
{
    if (rb == NULL || rb->buffer == NULL || out_len == NULL || rb->count == 0U) {
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return NULL;
    }

    /* Contiguous readable run = from read_pos up to the wrap point (or count). */
    size_t to_end = rb->capacity - rb->read_pos;
    *out_len = (rb->count < to_end) ? rb->count : to_end;
    return rb->buffer + rb->read_pos;
}

uint8_t *nx_ringbuf_poke_linear(nx_ringbuf_t *rb, size_t *out_len)
{
    if (rb == NULL || rb->buffer == NULL || out_len == NULL ||
        rb->count == rb->capacity) {
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return NULL;
    }

    size_t wpos = rb->read_pos + rb->count;
    if (wpos >= rb->capacity) {
        wpos -= rb->capacity;
    }

    /* Contiguous free run = from wpos up to the buffer end or the read_pos. */
    size_t free_space = rb->capacity - rb->count;
    size_t to_end     = rb->capacity - wpos;
    *out_len = (free_space < to_end) ? free_space : to_end;
    return rb->buffer + wpos;
}

size_t nx_ringbuf_commit(nx_ringbuf_t *rb, size_t len)
{
    if (rb == NULL || rb->buffer == NULL) {
        return 0U;
    }

    size_t free_space = rb->capacity - rb->count;
    if (len > free_space) {
        len = free_space;   /* defensive clamp */
    }
    rb->count += len;
    return len;
}
