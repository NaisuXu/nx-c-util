/**
 * @file    nx_uds_transfer.h
 * @brief   Moving a block of memory: 0x34, 0x35, 0x36 and 0x37.
 *
 * Four handlers occupying four ordinary service table rows, plus the state one
 * transfer needs: which direction it runs in, the region it covers, how far it has
 * got, and which block is expected next.
 *
 * The memory is not here. The application reads and writes it through two
 * callbacks, and this module never touches an address itself. What it owns is the
 * bookkeeping: that a block arriving twice is written once, that a block arriving
 * out of turn is refused without losing the transfer, and that the size it
 * announces is one the link underneath can carry.
 *
 * A transfer is opened by 0x34 to write memory or 0x35 to read it, carried by any
 * number of 0x36 exchanges, and closed by 0x37. One runs at a time.
 */
#ifndef NX_UDS_TRANSFER_H
#define NX_UDS_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

#include "nx_uds.h"
#include "nx_uds_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Address and length width.
 *
 * Wide enough for the memory the application describes. A build whose addresses
 * fit in 32 bits pays nothing for the ones that do not.
 */
typedef uint32_t nx_uds_addr_t;

/** @brief Which way a transfer runs. */
typedef enum {
    NX_UDS_XFER_NONE = 0,   /**< None is open */
    NX_UDS_XFER_DOWNLOAD,   /**< Opened by 0x34: the client writes memory */
    NX_UDS_XFER_UPLOAD      /**< Opened by 0x35: the client reads memory */
} nx_uds_xfer_dir_t;

/** @brief Bytes of a block that are not payload: the identifier and the counter. */
#define NX_UDS_XFER_BLOCK_OVERHEAD 2u

/** @brief Counter the first block of a fresh transfer carries. */
#define NX_UDS_XFER_FIRST_BSC 0x01u

/**
 * @brief  Payload a block of the announced length holds.
 *
 * The length announced counts the whole message, the service identifier and the
 * block counter included, so the room for payload is that much less. The one place
 * the subtraction is written.
 *
 * @param  block_len The announced length.
 * @return Bytes of payload, 0 where the announced length holds none.
 */
static inline uint32_t nx_uds_xfer_payload_room(uint32_t block_len)
{
    return (block_len > NX_UDS_XFER_BLOCK_OVERHEAD)
               ? (block_len - NX_UDS_XFER_BLOCK_OVERHEAD)
               : 0u;
}

/**
 * @brief Whether a transfer over this region may be opened, and how large.
 *
 * Called once, for a request the table has already found acceptable, before
 * anything is answered. This is where a product decides that the region is one it
 * will write, that the format asked for is one it understands, and how much of the
 * link it is willing to use per block.
 *
 * Returning false refuses with @c NX_UDS_NRC_REQUEST_OUT_OF_RANGE unless @c nrc
 * names another reason. A region the product will not write is out of range; a
 * product that is momentarily unable to is @c NX_UDS_NRC_CONDITIONS_NOT_CORRECT.
 *
 * @param  user      The @c user field of nx_uds_xfer_cfg_t.
 * @param  dir       Which way the transfer runs.
 * @param  addr      Region start, as the request declared it.
 * @param  size      Region length, as the request declared it.
 * @param  format    The dataFormatIdentifier byte: compression in the high nibble,
 *                   encryption in the low one. 0 asks for neither.
 * @param  block_len Room the server arrived at, which may be lowered but not
 *                   raised.
 * @param  nrc       Optional reason to refuse with, when refusing.
 * @return true to open the transfer.
 */
typedef bool (*nx_uds_xfer_open_fn)(void *user, nx_uds_xfer_dir_t dir,
                                   nx_uds_addr_t addr, nx_uds_addr_t size,
                                   uint8_t format, uint32_t *block_len,
                                   uint8_t *nrc);

/**
 * @brief Write one block of a download.
 *
 * Called once per block that is in turn, never for one arriving twice. Returning
 * false refuses with @c NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE unless @c nrc names
 * another reason, and leaves the transfer where it was so the client may retry.
 *
 * @param  user Untouched @c user field.
 * @param  addr Where the block belongs.
 * @param  data Its bytes.
 * @param  len  How many.
 * @param  nrc  Optional reason to refuse with.
 * @return true when the block was written.
 */
typedef bool (*nx_uds_xfer_write_fn)(void *user, nx_uds_addr_t addr,
                                     const uint8_t *data, uint32_t len,
                                     uint8_t *nrc);

/**
 * @brief Read one block of an upload.
 *
 * Called for a block that is in turn, and again for one arriving twice: the same
 * address and the same length, so what is handed back must be the same bytes.
 *
 * @param  user Untouched @c user field.
 * @param  addr Where to read from.
 * @param  out  Where to put the bytes.
 * @param  len  How many to read.
 * @param  nrc  Optional reason to refuse with.
 * @return true when the bytes were read.
 */
typedef bool (*nx_uds_xfer_read_fn)(void *user, nx_uds_addr_t addr, uint8_t *out,
                                    uint32_t len, uint8_t *nrc);

/**
 * @brief Finish a transfer.
 *
 * Called for an accepted 0x37, before its answer is assembled. Whatever a product
 * does to make a written image usable - verifying a checksum, marking a partition
 * valid - happens here.
 *
 * The record is whatever the request carried after the identifier, which is the
 * product's to define. A product expecting one refuses a request without it.
 *
 * Returning false refuses with @c NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE unless
 * @c nrc names another reason, and leaves the transfer open.
 *
 * @param  user       Untouched @c user field.
 * @param  dir        Which way the transfer ran.
 * @param  done       Bytes transferred.
 * @param  size       Bytes the region declared.
 * @param  record     Bytes after the identifier, or NULL.
 * @param  record_len How many, 0 when none.
 * @param  out        Where to put bytes to answer with, after the identifier.
 * @param  out_cap    Room there.
 * @param  out_len    Where to report how many were written; 0 for none.
 * @param  nrc        Optional reason to refuse with.
 * @return true to finish the transfer.
 */
typedef bool (*nx_uds_xfer_close_fn)(void *user, nx_uds_xfer_dir_t dir,
                                     nx_uds_addr_t done, nx_uds_addr_t size,
                                     const uint8_t *record, uint32_t record_len,
                                     uint8_t *out, uint32_t out_cap,
                                     uint32_t *out_len, uint8_t *nrc);

/**
 * @brief Configuration of the transfer services.
 */
typedef struct {
    nx_uds_server_t *srv;         /**< The instance these services belong to.
                                   Required: the block size is derived from what it
                                   carries. */

    nx_uds_xfer_open_fn  open_fn; /**< Consulted before a transfer opens; may be
                                   NULL to accept any region the request declares,
                                   which is rarely what a product wants. */
    nx_uds_xfer_write_fn write_fn;/**< Writes a block. Required to serve 0x34. */
    nx_uds_xfer_read_fn  read_fn; /**< Reads a block. Required to serve 0x35. */
    nx_uds_xfer_close_fn close_fn;/**< Finishes a transfer; may be NULL to finish
                                   whatever was transferred without checking it. */
    void                *user;    /**< Passed to all four untouched. */

    uint32_t max_block_len;       /**< Largest block this server will announce, 0 to
                                   announce whatever the server's own capacity
                                   allows. A product with a flash write window
                                   smaller than the link names it here. */

    uint8_t  bsc_error_nrc;       /**< What answers a block out of turn. 0 =
                                   @c NX_UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER,
                                   which is what the standard names for it. Some
                                   programmes require
                                   @c NX_UDS_NRC_REQUEST_SEQUENCE_ERROR instead. */

    bool     require_full_size;   /**< true: a 0x37 arriving before the whole
                                   declared region has been transferred is refused.
                                   Left false, a transfer may be finished short,
                                   which is what a client writing an image smaller
                                   than the region it reserved does. */
} nx_uds_xfer_cfg_t;

/**
 * @brief A transfer, as far as it has got.
 *
 * Declare one in static storage beside the server and hand it to
 * nx_uds_xfer_init. Its address is what all four rows carry as @c user.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed to
 *        nx_uds_xfer_init.
 */
typedef struct {
    nx_uds_xfer_cfg_t cfg;          /**< Copied configuration */
    struct {
        uint8_t       dir;          /**< An nx_uds_xfer_dir_t; NONE when none is
                                     open */
        nx_uds_addr_t addr;         /**< Region start, as declared. Not advanced:
                                     the next block goes at addr + done, and a
                                     block arriving twice reads back from the same
                                     place. */
        nx_uds_addr_t size;         /**< Region length, as declared */
        nx_uds_addr_t done;         /**< Bytes transferred, advanced only by a block
                                     that was committed */
        uint32_t      block_len;    /**< The length that was announced, which is
                                     what an arriving block is measured against */
        uint8_t       bsc_next;     /**< Counter the next new block carries */
        uint8_t       bsc_last;     /**< Counter of the last committed block */
        bool          committed;    /**< Whether any block has been, which is what
                                     makes bsc_last meaningful */
        uint32_t      last_len;     /**< Payload of the last committed block, so one
                                     arriving twice is answered at the same length */
    } run;                          /**< Internal runtime state */
} nx_uds_xfer_t;

/**
 * @brief  Set up the transfer services.
 *
 * @param  xfer Handle to initialise, must not be NULL.
 * @param  cfg  Configuration, copied.
 * @return true on success; false where @c srv is missing, or where neither
 *         @c write_fn nor @c read_fn is given and so no direction can be served.
 */
bool nx_uds_xfer_init(nx_uds_xfer_t *xfer, const nx_uds_xfer_cfg_t *cfg);

/**
 * @brief  Abandon whatever transfer is open.
 *
 * What an application calls when something outside these services makes an open
 * transfer meaningless: leaving the session it was being run in, a reset, a
 * security level dropping. A transfer left open across such an event would carry a
 * counter into the next attempt and refuse its first block.
 *
 * @param  xfer Handle; NULL does nothing.
 */
void nx_uds_xfer_abort(nx_uds_xfer_t *xfer);

/**
 * @brief  How far the open transfer has got.
 *
 * @param  xfer Handle, must not be NULL.
 * @param  done Where to store the bytes transferred; may be NULL.
 * @param  size Where to store the bytes declared; may be NULL.
 * @return Which way it runs, NX_UDS_XFER_NONE when none is open.
 */
nx_uds_xfer_dir_t nx_uds_xfer_progress(const nx_uds_xfer_t *xfer,
                                       nx_uds_addr_t *done, nx_uds_addr_t *size);

/**
 * @brief  0x34 RequestDownload — open a transfer that writes memory.
 *
 * The row's @c user is the nx_uds_xfer_t. Answers with the length of one block,
 * which the client sizes every block to and which counts the whole message rather
 * than its payload alone.
 *
 * @note  The row must declare @c min_len 5 and @c max_len 33, which is the widest
 *        the declared widths allow; the exact length follows from the request's own
 *        format byte and is checked here. No sub-function.
 */
nx_uds_disposition_t nx_uds_svc_request_download(nx_uds_ctx_t *ctx, void *user);

/**
 * @brief  0x35 RequestUpload — open a transfer that reads memory.
 *
 * The same request shape as 0x34 and the same answer. What differs is the direction
 * and the capacity the announced length is drawn from: the payload of an upload
 * travels in the answers, so it is bounded by what the server can send.
 *
 * @note  The row must declare @c min_len 5 and @c max_len 33. No sub-function.
 */
nx_uds_disposition_t nx_uds_svc_request_upload(nx_uds_ctx_t *ctx, void *user);

/**
 * @brief  0x36 TransferData — carry one block.
 *
 * On a download the block arrives in the request and is answered with the counter
 * alone; on an upload the request is the counter alone and the block is the answer.
 *
 * A block whose counter is the one just committed is answered again without being
 * carried out a second time, which is what makes a lost answer recoverable rather
 * than a doubled write. A block whose counter is neither the next nor the last is
 * refused, and the transfer is left open for the client to try again.
 *
 * @note  The row must declare @c min_len 2 and @c max_len 0. The real ceiling is
 *        the length that was announced, and a block over it is refused here as out
 *        of range rather than as the wrong length: the message is well formed and
 *        larger than what the server said it would take.
 *
 *        The row must NOT declare @c NX_UDS_SVC_HAS_SUB_FUNCTION. The byte after
 *        the identifier is a counter, not a sub-function, and reading its top bit as
 *        a request for silence would answer half of every transfer with nothing.
 */
nx_uds_disposition_t nx_uds_svc_transfer_data(nx_uds_ctx_t *ctx, void *user);

/**
 * @brief  0x37 RequestTransferExit — finish a transfer.
 *
 * Closes what is open, after the application has had its say on whether what was
 * transferred is usable. Leaves the session, the unlocked level and the quiet timer
 * alone: a client commonly runs several transfers in one session.
 *
 * @note  The row must declare @c min_len 1 and @c max_len 0. No sub-function.
 */
nx_uds_disposition_t nx_uds_svc_transfer_exit(nx_uds_ctx_t *ctx, void *user);
#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_TRANSFER_H */
