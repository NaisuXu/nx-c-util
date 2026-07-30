/**
 * @file    nx_bytes.h
 * @brief   Byte-level utilities: endianness conversion, bit manipulation, and byte operations.
 *
 * A collection of inline helpers for common byte-level operations. Every function
 * is static inline (zero overhead, fully inlined by the compiler) and performs no
 * parameter validation - the caller is responsible for ensuring valid pointers and
 * values, just like memcpy or memset.
 *
 * Design philosophy:
 *   - Static inline: zero call overhead, fully optimized by modern compilers.
 *   - No parameter checks: assumes valid inputs; treats NULL/invalid as undefined
 *     behavior (same as standard library functions).
 *   - Type-safe: unlike macros, the compiler performs full type checking.
 *   - Portable: standard C11, no platform-specific intrinsics.
 */
#ifndef NX_BYTES_H
#define NX_BYTES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Endianness Detection
 * ============================================================================ */

/**
 * @brief  Determine the host byte order at runtime.
 *
 * Returns true if the host is little-endian (e.g., x86, ARM in LE mode),
 * false if big-endian (e.g., some MIPS, PowerPC, network byte order).
 *
 * This is a compile-time constant on most platforms; the compiler will
 * optimize away the function call and directly return the result.
 *
 * @return true if little-endian, false if big-endian.
 *
 * Example:
 *   if (nx_bytes_is_little_endian()) {
 *       // Host is LE, no swap needed for LE data
 *   }
 */
static inline bool nx_bytes_is_little_endian(void)
{
    const uint16_t test = 0x0001;
    return *(const uint8_t *)&test == 0x01;
}

/**
 * @brief  Determine if the host is big-endian at runtime.
 *
 * Returns true if the host is big-endian (network byte order),
 * false if little-endian.
 *
 * @return true if big-endian, false if little-endian.
 */
static inline bool nx_bytes_is_big_endian(void)
{
    return !nx_bytes_is_little_endian();
}

/* ============================================================================
 * Endianness Conversion - Big Endian (Network Byte Order)
 * ============================================================================ */

/**
 * @brief  Read a big-endian 16-bit value from a byte array.
 *
 * @param  ptr Pointer to 2 bytes; must not be NULL.
 * @return The 16-bit value in host byte order.
 */
static inline uint16_t nx_bytes_get_be16(const uint8_t *ptr)
{
    return (uint16_t)(((uint16_t)ptr[0] << 8) | (uint16_t)ptr[1]);
}

/**
 * @brief  Write a 16-bit value as big-endian into a byte array.
 *
 * @param  ptr Pointer to 2 bytes; must not be NULL.
 * @param  val The 16-bit value to write.
 */
static inline void nx_bytes_set_be16(uint8_t *ptr, uint16_t val)
{
    ptr[0] = (uint8_t)(val >> 8);
    ptr[1] = (uint8_t)val;
}

/**
 * @brief  Read a big-endian 32-bit value from a byte array.
 *
 * @param  ptr Pointer to 4 bytes; must not be NULL.
 * @return The 32-bit value in host byte order.
 */
static inline uint32_t nx_bytes_get_be32(const uint8_t *ptr)
{
    return ((uint32_t)ptr[0] << 24) |
           ((uint32_t)ptr[1] << 16) |
           ((uint32_t)ptr[2] << 8)  |
           ((uint32_t)ptr[3]);
}

/**
 * @brief  Write a 32-bit value as big-endian into a byte array.
 *
 * @param  ptr Pointer to 4 bytes; must not be NULL.
 * @param  val The 32-bit value to write.
 */
static inline void nx_bytes_set_be32(uint8_t *ptr, uint32_t val)
{
    ptr[0] = (uint8_t)(val >> 24);
    ptr[1] = (uint8_t)(val >> 16);
    ptr[2] = (uint8_t)(val >> 8);
    ptr[3] = (uint8_t)val;
}

/**
 * @brief  Read a big-endian 64-bit value from a byte array.
 *
 * @param  ptr Pointer to 8 bytes; must not be NULL.
 * @return The 64-bit value in host byte order.
 */
static inline uint64_t nx_bytes_get_be64(const uint8_t *ptr)
{
    return ((uint64_t)ptr[0] << 56) |
           ((uint64_t)ptr[1] << 48) |
           ((uint64_t)ptr[2] << 40) |
           ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) |
           ((uint64_t)ptr[5] << 16) |
           ((uint64_t)ptr[6] << 8)  |
           ((uint64_t)ptr[7]);
}

/**
 * @brief  Write a 64-bit value as big-endian into a byte array.
 *
 * @param  ptr Pointer to 8 bytes; must not be NULL.
 * @param  val The 64-bit value to write.
 */
static inline void nx_bytes_set_be64(uint8_t *ptr, uint64_t val)
{
    ptr[0] = (uint8_t)(val >> 56);
    ptr[1] = (uint8_t)(val >> 48);
    ptr[2] = (uint8_t)(val >> 40);
    ptr[3] = (uint8_t)(val >> 32);
    ptr[4] = (uint8_t)(val >> 24);
    ptr[5] = (uint8_t)(val >> 16);
    ptr[6] = (uint8_t)(val >> 8);
    ptr[7] = (uint8_t)val;
}

/* ============================================================================
 * Endianness Conversion - Little Endian
 * ============================================================================ */

/**
 * @brief  Read a little-endian 16-bit value from a byte array.
 *
 * @param  ptr Pointer to 2 bytes; must not be NULL.
 * @return The 16-bit value in host byte order.
 */
static inline uint16_t nx_bytes_get_le16(const uint8_t *ptr)
{
    return (uint16_t)(((uint16_t)ptr[1] << 8) | (uint16_t)ptr[0]);
}

/**
 * @brief  Write a 16-bit value as little-endian into a byte array.
 *
 * @param  ptr Pointer to 2 bytes; must not be NULL.
 * @param  val The 16-bit value to write.
 */
static inline void nx_bytes_set_le16(uint8_t *ptr, uint16_t val)
{
    ptr[0] = (uint8_t)val;
    ptr[1] = (uint8_t)(val >> 8);
}

/**
 * @brief  Read a little-endian 32-bit value from a byte array.
 *
 * @param  ptr Pointer to 4 bytes; must not be NULL.
 * @return The 32-bit value in host byte order.
 */
static inline uint32_t nx_bytes_get_le32(const uint8_t *ptr)
{
    return ((uint32_t)ptr[3] << 24) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[1] << 8)  |
           ((uint32_t)ptr[0]);
}

/**
 * @brief  Write a 32-bit value as little-endian into a byte array.
 *
 * @param  ptr Pointer to 4 bytes; must not be NULL.
 * @param  val The 32-bit value to write.
 */
static inline void nx_bytes_set_le32(uint8_t *ptr, uint32_t val)
{
    ptr[0] = (uint8_t)val;
    ptr[1] = (uint8_t)(val >> 8);
    ptr[2] = (uint8_t)(val >> 16);
    ptr[3] = (uint8_t)(val >> 24);
}

/**
 * @brief  Read a little-endian 64-bit value from a byte array.
 *
 * @param  ptr Pointer to 8 bytes; must not be NULL.
 * @return The 64-bit value in host byte order.
 */
static inline uint64_t nx_bytes_get_le64(const uint8_t *ptr)
{
    return ((uint64_t)ptr[7] << 56) |
           ((uint64_t)ptr[6] << 48) |
           ((uint64_t)ptr[5] << 40) |
           ((uint64_t)ptr[4] << 32) |
           ((uint64_t)ptr[3] << 24) |
           ((uint64_t)ptr[2] << 16) |
           ((uint64_t)ptr[1] << 8)  |
           ((uint64_t)ptr[0]);
}

/**
 * @brief  Write a 64-bit value as little-endian into a byte array.
 *
 * @param  ptr Pointer to 8 bytes; must not be NULL.
 * @param  val The 64-bit value to write.
 */
static inline void nx_bytes_set_le64(uint8_t *ptr, uint64_t val)
{
    ptr[0] = (uint8_t)val;
    ptr[1] = (uint8_t)(val >> 8);
    ptr[2] = (uint8_t)(val >> 16);
    ptr[3] = (uint8_t)(val >> 24);
    ptr[4] = (uint8_t)(val >> 32);
    ptr[5] = (uint8_t)(val >> 40);
    ptr[6] = (uint8_t)(val >> 48);
    ptr[7] = (uint8_t)(val >> 56);
}

/* ============================================================================
 * Byte Swap (in-place endianness reversal)
 * ============================================================================ */

/**
 * @brief  Swap the byte order of a 16-bit value (0x1234 -> 0x3412).
 *
 * @param  val The 16-bit value to swap.
 * @return The byte-swapped value.
 */
static inline uint16_t nx_bytes_swap16(uint16_t val)
{
    return (uint16_t)(((val >> 8) & 0x00FFu) | ((val << 8) & 0xFF00u));
}

/**
 * @brief  Swap the byte order of a 32-bit value (0x12345678 -> 0x78563412).
 *
 * @param  val The 32-bit value to swap.
 * @return The byte-swapped value.
 */
static inline uint32_t nx_bytes_swap32(uint32_t val)
{
    return ((val >> 24) & 0x000000FFu) |
           ((val >>  8) & 0x0000FF00u) |
           ((val <<  8) & 0x00FF0000u) |
           ((val << 24) & 0xFF000000u);
}

/**
 * @brief  Swap the byte order of a 64-bit value.
 *
 * @param  val The 64-bit value to swap.
 * @return The byte-swapped value.
 */
static inline uint64_t nx_bytes_swap64(uint64_t val)
{
    return ((val >> 56) & 0x00000000000000FFull) |
           ((val >> 40) & 0x000000000000FF00ull) |
           ((val >> 24) & 0x0000000000FF0000ull) |
           ((val >>  8) & 0x00000000FF000000ull) |
           ((val <<  8) & 0x000000FF00000000ull) |
           ((val << 24) & 0x0000FF0000000000ull) |
           ((val << 40) & 0x00FF000000000000ull) |
           ((val << 56) & 0xFF00000000000000ull);
}

/* ============================================================================
 * Bit Manipulation - Generic macros for any integer type
 * ============================================================================ */

/**
 * @brief  Set a bit in an integer variable (bit 0 is LSB).
 *
 * Works with any integer type (uint8_t, uint16_t, uint32_t, etc.).
 *
 * @param  var  Variable reference (e.g., my_byte, my_u32).
 * @param  bit  Bit position (0 = LSB).
 *
 * Example: NX_BIT_SET(flags, 3);  // Set bit 3
 */
#define NX_BIT_SET(var, bit)    ((var) |= (1u << (bit)))

/**
 * @brief  Clear a bit in an integer variable (bit 0 is LSB).
 *
 * Works with any integer type.
 *
 * @param  var  Variable reference.
 * @param  bit  Bit position (0 = LSB).
 *
 * Example: NX_BIT_CLEAR(flags, 3);  // Clear bit 3
 */
#define NX_BIT_CLEAR(var, bit)  ((var) &= ~(1u << (bit)))

/**
 * @brief  Toggle a bit in an integer variable (bit 0 is LSB).
 *
 * Works with any integer type.
 *
 * @param  var  Variable reference.
 * @param  bit  Bit position (0 = LSB).
 *
 * Example: NX_BIT_TOGGLE(flags, 3);  // Flip bit 3
 */
#define NX_BIT_TOGGLE(var, bit) ((var) ^= (1u << (bit)))

/**
 * @brief  Test whether a bit is set in an integer value (bit 0 is LSB).
 *
 * Works with any integer type. Returns non-zero if set, zero if clear.
 *
 * @param  val  Value to test (can be a variable or expression).
 * @param  bit  Bit position (0 = LSB).
 * @return Non-zero if bit is set, zero if clear.
 *
 * Example: if (NX_BIT_TEST(flags, 3)) { ... }
 */
#define NX_BIT_TEST(val, bit)   (((val) & (1u << (bit))) != 0u)

#ifdef __cplusplus
}
#endif

#endif /* NX_BYTES_H */
