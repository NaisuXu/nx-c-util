/**
 * @file    nx_ringbuf.h
 * @brief   A byte-oriented ring buffer (FIFO) implemented in pure C.
 *
 * Where nx_queue stores fixed-size elements with all-or-nothing push/pop, this
 * module stores a raw byte stream: writes and reads move a variable number of
 * bytes and may transfer only part of the request when the buffer is nearly full
 * or nearly empty. That matches serial I/O (UART RX/TX) and other byte streams.
 *
 * Design goals: aimed at embedded development - simple, predictable, no hidden
 * overhead.
 *
 * Features:
 *   - Byte stream: read / write / peek / discard operate on byte counts and
 *     return how many bytes were actually moved (a partial transfer is normal).
 *   - Fixed capacity: capacity is fixed at init time and never grows at runtime.
 *   - Purely static buffer: storage is provided entirely by the caller; this
 *     library uses no dynamic memory and does not depend on malloc/free.
 *   - DMA-friendly: the "linear" helpers expose the largest physically
 *     contiguous readable / writable region, so a DMA engine can work against
 *     the ring buffer directly without a bounce buffer.
 *   - In a single-producer/single-consumer scenario (one side only writes, the
 *     other only reads) it is naturally thread-safe on a single core; other
 *     concurrent scenarios require the caller to add its own locking (see
 *     nx_lock.h). This module introduces no locks.
 */
#ifndef NX_RINGBUF_H
#define NX_RINGBUF_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Byte ring-buffer handle.
 *
 * @note  The struct members are implementation details; do not access them
 *        directly, use the provided API instead.
 */
typedef struct {
    uint8_t *buffer;    /**< Underlying byte buffer (caller-provided), sized @c capacity */
    size_t   capacity;  /**< Buffer size in bytes */
    size_t   read_pos;  /**< Index of the oldest byte (read position) */
    size_t   count;     /**< Current number of bytes stored */
} nx_ringbuf_t;

/**
 * @brief  Initialize a ring buffer with a caller-provided buffer (no dynamic memory).
 *
 * @param  rb       Ring-buffer handle, must not be NULL.
 * @param  buffer   Caller-provided storage; its size must be >= @p capacity.
 * @param  capacity Buffer size in bytes, must be > 0.
 *
 * @return true on success; false on invalid argument.
 */
bool nx_ringbuf_init(nx_ringbuf_t *rb, void *buffer, size_t capacity);

/**
 * @brief  Write up to @p len bytes into the ring buffer.
 *
 * Copies as many bytes as fit; if free space is smaller than @p len, only the
 * bytes that fit are written (a partial write). No overwrite of unread data.
 *
 * @param  rb   Ring-buffer handle.
 * @param  src  Source bytes, must not be NULL when @p len > 0.
 * @param  len  Number of bytes to write.
 *
 * @return The number of bytes actually written (0 .. len; 0 on invalid argument).
 */
size_t nx_ringbuf_write(nx_ringbuf_t *rb, const void *src, size_t len);

/**
 * @brief  Read up to @p len bytes out of the ring buffer, removing them.
 *
 * Copies as many bytes as are available; if fewer than @p len are stored, only
 * the available bytes are read (a partial read).
 *
 * @param  rb   Ring-buffer handle.
 * @param  dst  Destination buffer (size must be >= the returned count); may be
 *              NULL to discard the bytes (equivalent to nx_ringbuf_discard).
 * @param  len  Maximum number of bytes to read.
 *
 * @return The number of bytes actually read/removed (0 .. len).
 */
size_t nx_ringbuf_read(nx_ringbuf_t *rb, void *dst, size_t len);

/**
 * @brief  Copy up to @p len bytes without removing them (look-ahead).
 *
 * @param  rb   Ring-buffer handle.
 * @param  dst  Destination buffer, must not be NULL when @p len > 0.
 * @param  len  Maximum number of bytes to copy.
 *
 * @return The number of bytes actually copied (0 .. len).
 */
size_t nx_ringbuf_peek(const nx_ringbuf_t *rb, void *dst, size_t len);

/**
 * @brief  Discard up to @p len bytes from the front without copying them.
 *
 * @param  rb   Ring-buffer handle.
 * @param  len  Maximum number of bytes to drop.
 *
 * @return The number of bytes actually discarded (0 .. len).
 */
size_t nx_ringbuf_discard(nx_ringbuf_t *rb, size_t len);

/**
 * @brief  Clear the buffer (does not touch the caller's storage), resetting it to empty.
 *
 * @param  rb Ring-buffer handle.
 */
void nx_ringbuf_clear(nx_ringbuf_t *rb);

/**
 * @brief  Expose the largest physically contiguous readable region (DMA-friendly).
 *
 * Because the data may wrap around the end of the buffer, the readable bytes are
 * not always contiguous. This returns a pointer to the first readable byte and,
 * via @p out_len, how many bytes are contiguous from there (i.e. up to the wrap
 * point). Consume them with nx_ringbuf_discard after use. Call again to reach
 * any bytes past the wrap.
 *
 * @param  rb      Ring-buffer handle.
 * @param  out_len Receives the contiguous readable length; must not be NULL.
 *
 * @return Pointer to the first readable byte, or NULL if empty / invalid argument.
 */
const uint8_t *nx_ringbuf_peek_linear(const nx_ringbuf_t *rb, size_t *out_len);

/**
 * @brief  Expose the largest physically contiguous writable region (DMA-friendly).
 *
 * Returns a pointer to the first free byte and, via @p out_len, how many bytes
 * are contiguous from there (up to the wrap point or the buffer end). After a
 * DMA/manual fill of up to that many bytes, call nx_ringbuf_commit to make them
 * available to readers.
 *
 * @param  rb      Ring-buffer handle.
 * @param  out_len Receives the contiguous writable length; must not be NULL.
 *
 * @return Pointer to the first writable byte, or NULL if full / invalid argument.
 */
uint8_t *nx_ringbuf_poke_linear(nx_ringbuf_t *rb, size_t *out_len);

/**
 * @brief  Commit @p len bytes written directly via nx_ringbuf_poke_linear.
 *
 * @param  rb  Ring-buffer handle.
 * @param  len Number of bytes filled; must not exceed the free space (it is
 *             clamped to the free space defensively).
 *
 * @return The number of bytes actually committed.
 */
size_t nx_ringbuf_commit(nx_ringbuf_t *rb, size_t len);

/**
 * @brief  Return the number of readable bytes. A NULL pointer is treated as 0.
 */
static inline size_t nx_ringbuf_size(const nx_ringbuf_t *rb)
{
    return (rb != NULL) ? rb->count : 0U;
}

/**
 * @brief  Return the buffer capacity in bytes. A NULL pointer is treated as 0.
 */
static inline size_t nx_ringbuf_capacity(const nx_ringbuf_t *rb)
{
    return (rb != NULL) ? rb->capacity : 0U;
}

/**
 * @brief  Return the free space in bytes. A NULL pointer is treated as 0.
 */
static inline size_t nx_ringbuf_free(const nx_ringbuf_t *rb)
{
    return (rb != NULL) ? (rb->capacity - rb->count) : 0U;
}

/**
 * @brief  Whether the buffer is empty. A NULL pointer is treated as "empty".
 */
static inline bool nx_ringbuf_is_empty(const nx_ringbuf_t *rb)
{
    return (rb == NULL) || (rb->count == 0U);
}

/**
 * @brief  Whether the buffer is full. A NULL pointer is treated as "full".
 */
static inline bool nx_ringbuf_is_full(const nx_ringbuf_t *rb)
{
    return (rb == NULL) || (rb->buffer == NULL) || (rb->count == rb->capacity);
}

#ifdef __cplusplus
}
#endif

#endif /* NX_RINGBUF_H */
