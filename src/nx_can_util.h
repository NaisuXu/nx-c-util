/**
 * @file    nx_can_util.h
 * @brief   Common CAN / CAN FD data structures and helpers, in pure C.
 *
 * Provides a generic in-memory representation of a CAN frame (classic CAN and
 * CAN FD) plus small, dependency-free helpers such as DLC <-> byte-length
 * conversion. Header-only: all helpers are static inline, so nothing needs to be
 * compiled or linked.
 *
 * The frame payload is a flexible array member, so the caller sizes the storage
 * to the actual data length (up to 64 bytes for CAN FD).
 */
#ifndef NX_CAN_UTIL_H
#define NX_CAN_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Largest valid DLC (Data Length Code) field value.
 *
 * A DLC is 4 bits, so it ranges 0..15. For classic CAN only 0..8 are used; CAN
 * FD reuses 9..15 to encode payloads larger than 8 bytes.
 */
#define NX_CAN_MAX_DLC 15u

/** @brief Maximum payload of a classic CAN frame, in bytes. */
#define NX_CAN_MAX_CLASSIC_LEN 8u

/** @brief Maximum payload of a CAN FD frame, in bytes. */
#define NX_CAN_MAX_FD_LEN 64u

/**
 * @brief Frame direction, as seen between a host and a CAN tool/adapter.
 *
 * The tool sits between the host and the bus, so direction distinguishes not
 * only receive vs. transmit but also a transmit *request* from its *completion
 * report*. Encoded in 2 bits.
 */
typedef enum {
    NX_CAN_DIR_TX  = 0u,  /**< Host -> tool: a frame the host asks the tool to transmit */
    NX_CAN_DIR_RX  = 1u,  /**< Tool -> host: a frame received from the bus */
    NX_CAN_DIR_TXR = 2u   /**< Tool -> host: transmit-completion report for a prior TX */
    /* value 3 reserved */
} nx_can_dir_t;

/**
 * @brief Error / result code, shared by RX error frames and TXR send results.
 *
 * Carried in the @c err_code bitfield and meaningful only when @c is_err is set.
 * The same encoding serves both directions: for an RX frame it names the error
 * frame's cause; for a TXR report it names why the transmit failed. Low-level
 * CAN error kinds (bit/stuff/form/ack/crc) apply to both; arbitration-lost is
 * mainly a TXR outcome, while overrun is an RX one.
 */
typedef enum {
    NX_CAN_ERR_NONE     = 0u,  /**< No error (RX: normal frame; TXR: transmit succeeded) */
    NX_CAN_ERR_BIT      = 1u,  /**< Bit error */
    NX_CAN_ERR_STUFF    = 2u,  /**< Bit-stuffing error */
    NX_CAN_ERR_FORM     = 3u,  /**< Form (format) error */
    NX_CAN_ERR_ACK      = 4u,  /**< Acknowledgement error */
    NX_CAN_ERR_CRC      = 5u,  /**< CRC error */
    NX_CAN_ERR_ARB_LOST = 6u,  /**< Arbitration lost (mainly TXR) */
    NX_CAN_ERR_BUS_OFF  = 7u,  /**< Controller went bus-off */
    NX_CAN_ERR_TIMEOUT  = 8u,  /**< Operation timed out (TXR: transmit timed out) */
    NX_CAN_ERR_OVERRUN  = 9u   /**< RX overrun: a frame was lost */
    /* values 10..31 reserved */
} nx_can_err_t;

/**
 * @brief A CAN / CAN FD frame.
 *
 * @note  @c data is a flexible array member: the caller allocates
 *        sizeof(nx_can_msg_t) + payload_len bytes and fills @c data.
 */
typedef struct {
    uint32_t id;             /**< Frame identifier (11-bit standard or 29-bit extended) */
    union {
        uint32_t raw;        /**< All flag bits as one word (for fast copy/compare) */
        struct {
            uint32_t dir      : 2;  /**< Direction; one of nx_can_dir_t (TX / RX / TXR) */
            uint32_t dlc      : 4;  /**< Data Length Code (0..15); see nx_can_dlc_to_len */
            uint32_t is_ext   : 1;  /**< Extended (29-bit) identifier */
            uint32_t is_remote: 1;  /**< Remote transmission request (classic CAN only) */
            uint32_t is_fd    : 1;  /**< CAN FD frame */
            uint32_t brs      : 1;  /**< Bit-rate switch (CAN FD) */
            uint32_t esi      : 1;  /**< Error state indicator (CAN FD) */
            uint32_t is_err   : 1;  /**< Error/failure frame (RX: error frame; TXR: transmit failed) */
            uint32_t err_code : 4;  /**< Cause when is_err is set; one of nx_can_err_t */
            uint32_t reserved : 16; /**< Reserved, keep 0 */
        } bits;
    } flags;                 /**< Frame flags */
    uint64_t timestamp;      /**< Timestamp (unit is caller-defined, e.g. microseconds) */
    uint8_t  data[];         /**< Payload, length given by nx_can_dlc_to_len(flags.bits.dlc) */
} nx_can_msg_t;

/**
 * @brief  Convert a DLC to the actual payload length in bytes.
 *
 * Handles both classic CAN (DLC 0..8 map to 0..8 bytes) and CAN FD (DLC 9..15
 * map to 12/16/20/24/32/48/64 bytes).
 *
 * @param  dlc Data Length Code, 0..15.
 * @return Payload length in bytes; 0 if @p dlc is out of range.
 */
static inline uint32_t nx_can_dlc_to_len(uint8_t dlc)
{
    static const uint8_t dlc_table[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8,   /* 0..8: classic CAN */
        12, 16, 20, 24, 32, 48, 64   /* 9..15: CAN FD */
    };
    return (dlc <= NX_CAN_MAX_DLC) ? dlc_table[dlc] : 0u;
}

/**
 * @brief  Convert a payload length in bytes to the DLC that carries it.
 *
 * For lengths that are not an exact CAN FD size, rounds up to the next valid
 * size (e.g. 10 bytes -> DLC 10, i.e. 16 bytes). Lengths above 64 are clamped
 * to DLC 15 (64 bytes).
 *
 * @param  len Payload length in bytes.
 * @return The DLC value (0..15) that carries at least @p len bytes.
 */
static inline uint8_t nx_can_len_to_dlc(uint32_t len)
{
    if (len <= 8u)  { return (uint8_t)len; }
    if (len <= 12u) { return 9u;  }
    if (len <= 16u) { return 10u; }
    if (len <= 20u) { return 11u; }
    if (len <= 24u) { return 12u; }
    if (len <= 32u) { return 13u; }
    if (len <= 48u) { return 14u; }
    return 15u;   /* up to 64 bytes */
}

#ifdef __cplusplus
}
#endif

#endif /* NX_CAN_UTIL_H */
