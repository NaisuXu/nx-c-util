/**
 * @file    nx_modbus_rtu.h
 * @brief   Modbus RTU frame structures and helpers, in pure C.
 *
 * Provides in-memory representations of the common Modbus RTU frames (requests,
 * responses, and exception responses), byte-order helpers, and a self-contained
 * CRC-16/MODBUS. The frame structs and the byte helpers are header-side (the
 * helpers are static inline); the CRC routines live in nx_modbus_rtu.c, which is
 * the only part that must be compiled and linked.
 *
 * Wire layout: every frame struct is made of uint8_t fields only, so it has
 * alignment 1 and no padding, and maps 1:1 onto the RTU byte stream without any
 * packing pragma (portable across compilers). A byte buffer can therefore be
 * cast to the matching frame type to parse it in place.
 *
 * Byte order on the wire:
 *   - 16-bit values (address, quantity, register data) are big-endian
 *     (high byte first) - hence the explicit @c _h / @c _l field pairs. Use
 *     nx_modbus_rtu_get_u16 / nx_modbus_rtu_set_u16 to convert.
 *   - The trailing CRC-16/MODBUS is little-endian (low byte first, @c crc_l
 *     then @c crc_h). Compute it with nx_modbus_rtu_crc16 over every byte from
 *     @c addr up to (but not including) the CRC, or use nx_modbus_rtu_set_crc /
 *     nx_modbus_rtu_check_crc to fill or verify a whole frame's CRC.
 *
 * The CRC is table-driven and self-contained.
 */
#ifndef NX_MODBUS_RTU_H
#define NX_MODBUS_RTU_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Broadcast slave address (write requests only; no response expected). */
#define NX_MODBUS_RTU_ADDR_BROADCAST 0u

/** @brief Lowest valid unicast slave address. */
#define NX_MODBUS_RTU_ADDR_MIN 1u

/** @brief Highest valid unicast slave address. */
#define NX_MODBUS_RTU_ADDR_MAX 247u

/** @brief Largest possible RTU frame (ADU) in bytes: addr + PDU(253) + CRC(2). */
#define NX_MODBUS_RTU_MAX_ADU 256u

/** @brief Bit OR'ed into the function code to mark an exception response. */
#define NX_MODBUS_RTU_EXCEPTION_FLAG 0x80u

/**
 * @brief Standard Modbus function codes.
 *
 * Only the widely used data-access codes are listed. The read/write single and
 * write multiple codes are the ones the frame structs below are shaped for.
 */
typedef enum {
    NX_MODBUS_FC_READ_COILS            = 0x01u, /**< Read coils */
    NX_MODBUS_FC_READ_DISCRETE_INPUTS  = 0x02u, /**< Read discrete inputs */
    NX_MODBUS_FC_READ_HOLDING_REGS     = 0x03u, /**< Read holding registers */
    NX_MODBUS_FC_READ_INPUT_REGS       = 0x04u, /**< Read input registers */
    NX_MODBUS_FC_WRITE_SINGLE_COIL     = 0x05u, /**< Write single coil */
    NX_MODBUS_FC_WRITE_SINGLE_REG      = 0x06u, /**< Write single register */
    NX_MODBUS_FC_WRITE_MULTIPLE_COILS  = 0x0Fu, /**< Write multiple coils */
    NX_MODBUS_FC_WRITE_MULTIPLE_REGS   = 0x10u  /**< Write multiple registers */
} nx_modbus_fc_t;

/**
 * @brief Standard Modbus exception codes (carried in an exception response).
 *
 * Meaningful only in nx_modbus_rtu_rsp_exc_t, whose @c cmd has
 * the NX_MODBUS_RTU_EXCEPTION_FLAG bit set.
 */
typedef enum {
    NX_MODBUS_EXC_ILLEGAL_FUNCTION     = 0x01u, /**< Function code not supported */
    NX_MODBUS_EXC_ILLEGAL_DATA_ADDR    = 0x02u, /**< Data address not allowed */
    NX_MODBUS_EXC_ILLEGAL_DATA_VALUE   = 0x03u, /**< Value not allowed for this address */
    NX_MODBUS_EXC_SLAVE_DEVICE_FAILURE = 0x04u, /**< Unrecoverable error while handling request */
    NX_MODBUS_EXC_ACKNOWLEDGE          = 0x05u, /**< Request accepted, long processing in progress */
    NX_MODBUS_EXC_SLAVE_DEVICE_BUSY    = 0x06u, /**< Busy; retry later */
    NX_MODBUS_EXC_MEMORY_PARITY_ERROR  = 0x08u, /**< Memory parity error */
    NX_MODBUS_EXC_GATEWAY_PATH_UNAVAIL = 0x0Au, /**< Gateway could not route the request */
    NX_MODBUS_EXC_GATEWAY_NO_RESPONSE  = 0x0Bu  /**< Gateway target failed to respond */
} nx_modbus_exc_t;

/**
 * @brief Common frame header (address + function code).
 *
 * @note  The header fields are inlined into each frame struct below rather than
 *        embedded, to keep the byte layout identical to the RTU stream without
 *        relying on any packing pragma (portable across compilers). This struct
 *        is provided for reference / generic header inspection.
 */
typedef struct {
    uint8_t addr;  /**< Slave address, 1..247 (0 = broadcast) */
    uint8_t cmd;   /**< Function code; one of nx_modbus_fc_t */
} nx_modbus_rtu_header_t;

/**
 * @brief Fixed-length request, for function codes 01/02/03/04/05/06.
 *
 * For 01..04 the two 16-bit fields are starting address + quantity; for 05/06
 * they are data address + the value to write.
 */
typedef struct {
    uint8_t addr;     /**< Slave address */
    uint8_t cmd;      /**< Function code */
    uint8_t addr_h;   /**< Starting/data address, high byte */
    uint8_t addr_l;   /**< Starting/data address, low byte */
    uint8_t qty_h;    /**< Quantity / write value, high byte */
    uint8_t qty_l;    /**< Quantity / write value, low byte */
    uint8_t crc_l;    /**< CRC low byte (transmitted first) */
    uint8_t crc_h;    /**< CRC high byte */
} nx_modbus_rtu_req_fix_t;

/**
 * @brief Variable-length request, for function codes 0F/10.
 *
 * @c byte_count is the number of payload data bytes; the CRC trails the payload.
 *
 * @note  @c payload is a flexible array member laid out as: @c byte_count output
 *        bytes, then @c crc_l and @c crc_h. The CRC is therefore not a named
 *        struct field here; read it at @c payload[byte_count] / @c [byte_count+1].
 */
typedef struct {
    uint8_t addr;        /**< Slave address */
    uint8_t cmd;         /**< Function code */
    uint8_t addr_h;      /**< Starting address, high byte */
    uint8_t addr_l;      /**< Starting address, low byte */
    uint8_t qty_h;       /**< Quantity of coils/registers, high byte */
    uint8_t qty_l;       /**< Quantity of coils/registers, low byte */
    uint8_t byte_count;  /**< Number of data bytes that follow */
    uint8_t payload[];   /**< Output values (byte_count bytes) + crc_l + crc_h */
} nx_modbus_rtu_req_var_t;

/**
 * @brief Fixed-length response, for function codes 05/06/0F/10.
 *
 * 05/06 echo the data address + written value; 0F/10 echo the starting address
 * + quantity.
 */
typedef struct {
    uint8_t addr;     /**< Slave address */
    uint8_t cmd;      /**< Function code */
    uint8_t addr_h;   /**< 05/06: data address; 0F/10: starting address, high byte */
    uint8_t addr_l;   /**< 05/06: data address; 0F/10: starting address, low byte */
    uint8_t data_h;   /**< 05/06: write value; 0F/10: quantity, high byte */
    uint8_t data_l;   /**< 05/06: write value; 0F/10: quantity, low byte */
    uint8_t crc_l;    /**< CRC low byte (transmitted first) */
    uint8_t crc_h;    /**< CRC high byte */
} nx_modbus_rtu_rsp_fix_t;

/**
 * @brief Variable-length response, for function codes 01/02/03/04.
 *
 * @c byte_count is the number of data bytes; the CRC trails the payload.
 *
 * @note  @c payload is a flexible array member laid out as: @c byte_count read
 *        data bytes, then @c crc_l and @c crc_h.
 */
typedef struct {
    uint8_t addr;        /**< Slave address */
    uint8_t cmd;         /**< Function code */
    uint8_t byte_count;  /**< Number of data bytes that follow */
    uint8_t payload[];   /**< Read data (byte_count bytes) + crc_l + crc_h */
} nx_modbus_rtu_rsp_var_t;

/**
 * @brief Exception response, valid for any function code.
 *
 * The returned function code is the original code OR'ed with
 * NX_MODBUS_RTU_EXCEPTION_FLAG (0x80).
 */
typedef struct {
    uint8_t addr;            /**< Slave address */
    uint8_t cmd;             /**< Original function code | 0x80 */
    uint8_t exception_code;  /**< Exception code; one of nx_modbus_exc_t */
    uint8_t crc_l;           /**< CRC low byte (transmitted first) */
    uint8_t crc_h;           /**< CRC high byte */
} nx_modbus_rtu_rsp_exc_t;

/**
 * @brief  Read a big-endian 16-bit value from two wire bytes (high, low).
 *
 * Modbus carries addresses, quantities and register values as big-endian
 * 16-bit words. Use this to combine an @c _h / @c _l field pair into a value.
 *
 * @param  high High (first-transmitted) byte.
 * @param  low  Low (second-transmitted) byte.
 * @return The 16-bit value.
 */
static inline uint16_t nx_modbus_rtu_get_u16(uint8_t high, uint8_t low)
{
    return (uint16_t)(((uint16_t)high << 8) | (uint16_t)low);
}

/**
 * @brief  Split a 16-bit value into big-endian wire bytes (high, low).
 *
 * @param  value    The 16-bit value.
 * @param  out_high Receives the high (first-transmitted) byte; must not be NULL.
 * @param  out_low  Receives the low (second-transmitted) byte; must not be NULL.
 */
static inline void nx_modbus_rtu_set_u16(uint16_t value,
                                         uint8_t *out_high, uint8_t *out_low)
{
    if (out_high != NULL) { *out_high = (uint8_t)(value >> 8); }
    if (out_low  != NULL) { *out_low  = (uint8_t)(value & 0xFFu); }
}

/**
 * @brief  Locate the CRC low byte within a variable-length request's payload.
 *
 * In nx_modbus_rtu_req_var_t the CRC is not a named field: it sits
 * right after the @c byte_count data bytes, at @c payload[byte_count] (low) and
 * @c payload[byte_count + 1] (high). This returns a pointer to those two bytes.
 *
 * @param  frame Request frame, must not be NULL.
 * @return Pointer to crc_l (crc_h follows at +1), or NULL if @p frame is NULL.
 */
static inline uint8_t *nx_modbus_rtu_req_var_crc(nx_modbus_rtu_req_var_t *frame)
{
    return (frame != NULL) ? &frame->payload[frame->byte_count] : NULL;
}

/**
 * @brief  Locate the CRC low byte within a variable-length response's payload.
 *
 * As above but for nx_modbus_rtu_rsp_var_t: the CRC follows the
 * @c byte_count read-data bytes, at @c payload[byte_count] / @c [byte_count + 1].
 *
 * @param  frame Response frame, must not be NULL.
 * @return Pointer to crc_l (crc_h follows at +1), or NULL if @p frame is NULL.
 */
static inline uint8_t *nx_modbus_rtu_rsp_var_crc(nx_modbus_rtu_rsp_var_t *frame)
{
    return (frame != NULL) ? &frame->payload[frame->byte_count] : NULL;
}

/**
 * @brief  Compute the Modbus RTU frame check (CRC-16/MODBUS) over a byte range.
 *
 * Table-driven implementation (256-entry lookup); self-contained, so this module
 * does not depend on nx_crc. Parameters are the registered CRC-16/MODBUS values
 * (poly 0x8005 reflected, init 0xFFFF, refin/refout true, xorout 0).
 *
 * Compute it over every byte of the frame from @c addr up to (but not including)
 * the two CRC bytes. The result is placed on the wire low byte first.
 *
 * @param  data Pointer to the bytes to check; may be NULL only if @p len is 0.
 * @param  len  Number of bytes to process.
 * @return The 16-bit CRC (0xFFFF for a NULL @p data with non-zero @p len).
 */
uint16_t nx_modbus_rtu_crc16(const uint8_t *data, size_t len);

/**
 * @brief  Compute a frame's CRC and write it to the two trailing bytes.
 *
 * Runs nx_modbus_rtu_crc16 over @c frame[0 .. len-3] (everything before the CRC)
 * and stores the result little-endian into @c frame[len-2] (low) and
 * @c frame[len-1] (high), matching the RTU on-wire order.
 *
 * @param  frame Frame buffer, must not be NULL.
 * @param  len   Total frame length including the 2 CRC bytes; must be >= 5
 *               (the shortest valid ADU is a 5-byte exception response).
 * @return true on success; false if @p frame is NULL or @p len < 5.
 */
bool nx_modbus_rtu_set_crc(uint8_t *frame, size_t len);

/**
 * @brief  Verify a received frame's trailing CRC.
 *
 * Recomputes the CRC over @c frame[0 .. len-3] and compares it against the
 * little-endian CRC stored in the last two bytes.
 *
 * @param  frame Frame buffer, must not be NULL.
 * @param  len   Total frame length including the 2 CRC bytes; must be >= 5
 *               (the shortest valid ADU is a 5-byte exception response).
 * @return true if the stored CRC matches; false on mismatch or bad arguments.
 */
bool nx_modbus_rtu_check_crc(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NX_MODBUS_RTU_H */
