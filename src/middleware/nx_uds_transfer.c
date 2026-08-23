/**
 * @file    nx_uds_transfer.c
 * @brief   Implementation of the memory transfer services.
 */
#include <string.h>

#include "nx_uds_transfer.h"

/** @brief Bytes of a 0x34/0x35 request before the two declared fields. */
#define XFER_REQ_FIXED 3u

/** @brief Bytes of a 0x74/0x75 answer before the announced length. */
#define XFER_RSP_FIXED 2u

/**
 * @brief  Read a big-endian field.
 *
 * @param  p   First byte, the most significant one.
 * @param  len How many bytes, 1 upwards.
 * @param  out Where to store the value.
 * @return true when the value fits what an address holds.
 */
static bool xfer_read_be(const uint8_t *p, uint32_t len, nx_uds_addr_t *out)
{
    nx_uds_addr_t v = 0u;
    uint32_t i;

    /* A field declared wider than an address is acceptable while the bytes above
     * the width are zero: a client that always sends four bytes is talking to a
     * server whose addresses happen to be narrower. */
    for (i = 0u; i + sizeof(nx_uds_addr_t) < len; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    for (; i < len; i++) {
        v = (nx_uds_addr_t)((v << 8) | p[i]);
    }
    *out = v;
    return true;
}

/**
 * @brief  Write a big-endian field.
 *
 * @param  p   Where to write, most significant byte first.
 * @param  len How many bytes.
 * @param  v   The value.
 */
static void xfer_write_be(uint8_t *p, uint32_t len, uint32_t v)
{
    uint32_t i;

    for (i = 0u; i < len; i++) {
        p[len - 1u - i] = (uint8_t)(v >> (8u * i));
    }
}

/* ------------------------------------------------------------------ */
/* Setting up and tearing down                                       */
/* ------------------------------------------------------------------ */
bool nx_uds_xfer_init(nx_uds_xfer_t *xfer, const nx_uds_xfer_cfg_t *cfg)
{
    if (xfer == NULL || cfg == NULL || cfg->srv == NULL) {
        return false;
    }
    /* A configuration serving neither direction serves nothing. */
    if (cfg->write_fn == NULL && cfg->read_fn == NULL) {
        return false;
    }

    memset(xfer, 0, sizeof(*xfer));
    xfer->cfg = *cfg;
    if (xfer->cfg.bsc_error_nrc == 0u) {
        xfer->cfg.bsc_error_nrc =
            (uint8_t)NX_UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER;
    }
    return true;
}

/**
 * @brief  Forget the transfer.
 *
 * The one place the state is cleared, so a counter cannot survive into the next
 * transfer and refuse its first block.
 *
 * @param  xfer Handle.
 */
static void xfer_clear(nx_uds_xfer_t *xfer)
{
    memset(&xfer->run, 0, sizeof(xfer->run));
}

void nx_uds_xfer_abort(nx_uds_xfer_t *xfer)
{
    if (xfer == NULL) {
        return;
    }
    xfer_clear(xfer);
}

nx_uds_xfer_dir_t nx_uds_xfer_progress(const nx_uds_xfer_t *xfer,
                                       nx_uds_addr_t *done, nx_uds_addr_t *size)
{
    if (xfer == NULL) {
        return NX_UDS_XFER_NONE;
    }
    if (done != NULL) {
        *done = xfer->run.done;
    }
    if (size != NULL) {
        *size = xfer->run.size;
    }
    return (nx_uds_xfer_dir_t)xfer->run.dir;
}

/* ------------------------------------------------------------------ */
/* Opening a transfer: 0x34 and 0x35                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief  The block length to announce before the application has its say.
 *
 * Drawn from the capacity the payload will travel in: a download's payload arrives
 * in requests, an upload's leaves in answers. Announcing more than that would be
 * announcing a block the server then refuses.
 *
 * @param  xfer Handle.
 * @param  dir  Which way the transfer runs.
 * @return The length, counting the whole message.
 */
static uint32_t xfer_announce_len(const nx_uds_xfer_t *xfer,
                                  nx_uds_xfer_dir_t dir)
{
    uint32_t req = 0u;
    uint32_t rsp = 0u;
    uint32_t len;

    nx_uds_server_apdu_limits(xfer->cfg.srv, &req, &rsp);
    len = (dir == NX_UDS_XFER_DOWNLOAD) ? req : rsp;

    /* A product whose write window is narrower than the link says so. */
    if (xfer->cfg.max_block_len != 0u && xfer->cfg.max_block_len < len) {
        len = xfer->cfg.max_block_len;
    }
    return len;
}

/**
 * @brief  Bytes needed to carry a length, narrowest first.
 *
 * The width is announced alongside the number, so the narrowest that holds it is
 * as good as the widest and costs less on a link with small frames.
 *
 * @param  v The length.
 * @return 1, 2 or 4.
 */
static uint32_t xfer_width_for(uint32_t v)
{
    if (v <= 0xFFu) {
        return 1u;
    }
    if (v <= 0xFFFFu) {
        return 2u;
    }
    return 4u;
}

/**
 * @brief  Open a transfer in one direction.
 *
 * The two services differ in the direction they record and the capacity the length
 * they announce is drawn from; everything else, the request included, is shared.
 *
 * @param  xfer Handle.
 * @param  ctx  Transaction.
 * @param  dir  Which way it runs.
 * @return What to do with the request.
 */
static nx_uds_disposition_t xfer_open(nx_uds_xfer_t *xfer, nx_uds_ctx_t *ctx,
                                      nx_uds_xfer_dir_t dir)
{
    uint32_t addr_len;
    uint32_t size_len;
    nx_uds_addr_t addr = 0u;
    nx_uds_addr_t size = 0u;
    uint32_t block_len;
    uint32_t width;
    uint8_t  nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;

    if (xfer->run.dir != (uint8_t)NX_UDS_XFER_NONE) {
        /* One at a time. A second opening while one runs would abandon whatever the
         * first had written without saying so. */
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if ((dir == NX_UDS_XFER_DOWNLOAD && xfer->cfg.write_fn == NULL)
        || (dir == NX_UDS_XFER_UPLOAD && xfer->cfg.read_fn == NULL)) {
        ctx->nrc = NX_UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPTED;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* The byte after the format names the width of the two fields that follow it:
     * the address in the low half, the length in the high half. Neither may be
     * absent, and together with the fixed part they account for the whole request. */
    addr_len = (uint32_t)(ctx->req[2] & 0x0Fu);
    size_len = (uint32_t)((ctx->req[2] >> 4) & 0x0Fu);
    if (addr_len == 0u || size_len == 0u
        || ctx->req_len != XFER_REQ_FIXED + addr_len + size_len) {
        ctx->nrc = NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (!xfer_read_be(&ctx->req[XFER_REQ_FIXED], addr_len, &addr)
        || !xfer_read_be(&ctx->req[XFER_REQ_FIXED + addr_len], size_len, &size)) {
        /* Declared wider than this server addresses, with something in the bytes
         * above what it can reach. */
        ctx->nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (size == 0u) {
        ctx->nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    block_len = xfer_announce_len(xfer, dir);
    if (xfer->cfg.open_fn != NULL
        && !xfer->cfg.open_fn(xfer->cfg.user, dir, addr, size, ctx->req[1],
                              &block_len, &nrc)) {
        ctx->nrc = nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    /* The application may lower what is announced but not raise it: a block larger
     * than the link carries would be announced and then refused on arrival. */
    if (block_len > xfer_announce_len(xfer, dir)) {
        block_len = xfer_announce_len(xfer, dir);
    }
    if (nx_uds_xfer_payload_room(block_len) == 0u) {
        /* Nothing would fit in a block, so the transfer could not advance. */
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    width = xfer_width_for(block_len);
    if (ctx->out_cap < XFER_RSP_FIXED + width) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    xfer->run.dir       = (uint8_t)dir;
    xfer->run.addr      = addr;
    xfer->run.size      = size;
    xfer->run.done      = 0u;
    xfer->run.block_len = block_len;
    xfer->run.bsc_next  = NX_UDS_XFER_FIRST_BSC;
    xfer->run.bsc_last  = 0u;
    xfer->run.committed = false;
    xfer->run.last_len  = 0u;

    /* The width of the number is announced in the high half of the byte before it;
     * the low half is not used. */
    ctx->out[1] = (uint8_t)(width << 4);
    xfer_write_be(&ctx->out[XFER_RSP_FIXED], width, block_len);
    ctx->out_len = XFER_RSP_FIXED + width;
    return NX_UDS_DISPOSITION_DONE;
}

nx_uds_disposition_t nx_uds_svc_request_download(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_xfer_t *xfer = (nx_uds_xfer_t *)user;

    if (xfer == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    return xfer_open(xfer, ctx, NX_UDS_XFER_DOWNLOAD);
}

nx_uds_disposition_t nx_uds_svc_request_upload(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_xfer_t *xfer = (nx_uds_xfer_t *)user;

    if (xfer == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    return xfer_open(xfer, ctx, NX_UDS_XFER_UPLOAD);
}

/* ------------------------------------------------------------------ */
/* Carrying a block: 0x36                                            */
/* ------------------------------------------------------------------ */
/**
 * @brief  Answer an upload block by reading it out of memory.
 *
 * Used both for a block in turn and for one arriving twice: the address and the
 * length come from state that a repetition does not advance, so the same request
 * yields the same bytes.
 *
 * @param  xfer Handle.
 * @param  ctx  Transaction.
 * @param  len  Payload to read.
 * @return What to do with the request.
 */
static nx_uds_disposition_t xfer_fill_upload(nx_uds_xfer_t *xfer,
                                             nx_uds_ctx_t *ctx,
                                             nx_uds_addr_t offset, uint32_t len)
{
    uint8_t nrc = NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE;

    if (ctx->out_cap < NX_UDS_XFER_BLOCK_OVERHEAD + len) {
        /* The answer would not fit what the server said it would send, which is a
         * server announcing more than it can carry rather than a bad request. */
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (!xfer->cfg.read_fn(xfer->cfg.user, xfer->run.addr + offset,
                           &ctx->out[NX_UDS_XFER_BLOCK_OVERHEAD], len, &nrc)) {
        ctx->nrc = nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    ctx->out_len = NX_UDS_XFER_BLOCK_OVERHEAD + len;
    return NX_UDS_DISPOSITION_DONE;
}

/**
 * @brief  Answer a block that has already been carried out.
 *
 * A client that did not hear the answer sends the block again. What it needs is the
 * answer, not the work: repeating a write would program a page twice, and
 * advancing the cursor would lose a block's worth of the region.
 *
 * @param  xfer Handle.
 * @param  ctx  Transaction.
 * @return What to do with the request.
 */
static nx_uds_disposition_t xfer_repeat(nx_uds_xfer_t *xfer, nx_uds_ctx_t *ctx)
{
    ctx->out[1] = xfer->run.bsc_last;

    if (xfer->run.dir == (uint8_t)NX_UDS_XFER_UPLOAD) {
        /* The same bytes, which means the block that was sent rather than the one
         * that comes next: the cursor has already moved past it, so the block
         * begins its own length back from where the cursor now stands. */
        return xfer_fill_upload(xfer, ctx,
                                xfer->run.done - xfer->run.last_len,
                                xfer->run.last_len);
    }
    ctx->out_len = NX_UDS_XFER_BLOCK_OVERHEAD;
    return NX_UDS_DISPOSITION_DONE;
}

nx_uds_disposition_t nx_uds_svc_transfer_data(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_xfer_t *xfer = (nx_uds_xfer_t *)user;
    uint32_t room;
    uint32_t len;
    uint8_t  bsc;
    uint8_t  nrc = NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE;

    if (xfer == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    if (xfer->run.dir == (uint8_t)NX_UDS_XFER_NONE) {
        /* Nothing is open, so this service has no business arriving. Distinct from a
         * block out of turn, which is the right service at the wrong point in a
         * transfer that does exist. */
        ctx->nrc = NX_UDS_NRC_REQUEST_SEQUENCE_ERROR;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* The byte after the identifier is a counter, and the whole of it is the
     * counter: no part of it asks for anything. */
    bsc  = ctx->req[1];
    room = nx_uds_xfer_payload_room(xfer->run.block_len);

    if (xfer->run.committed && bsc == xfer->run.bsc_last) {
        return xfer_repeat(xfer, ctx);
    }
    if (bsc != xfer->run.bsc_next) {
        /* Neither the next nor the last. The transfer is left open: the client's
         * next correct block should succeed rather than meet a torn-down transfer. */
        ctx->nrc = xfer->cfg.bsc_error_nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (xfer->run.dir == (uint8_t)NX_UDS_XFER_DOWNLOAD) {
        len = ctx->req_len - NX_UDS_XFER_BLOCK_OVERHEAD;
        if (len == 0u || len > room) {
            /* A block larger than what was announced is well formed and larger than
             * the server said it would take, and a block with nothing in it cannot
             * advance the transfer. */
            ctx->nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
            return NX_UDS_DISPOSITION_NEGATIVE;
        }
    } else {
        /* An upload's request carries the counter alone, and the server decides how
         * much to send: a whole block, or whatever is left of the region. */
        len = room;
    }

    if (xfer->run.done + len > xfer->run.size) {
        nx_uds_addr_t left = xfer->run.size - xfer->run.done;

        if (left == 0u) {
            /* The region declared has been transferred; there is nowhere to put
             * this. */
            ctx->nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
            return NX_UDS_DISPOSITION_NEGATIVE;
        }
        if (xfer->run.dir == (uint8_t)NX_UDS_XFER_DOWNLOAD) {
            /* Writing past the end of what the client itself declared. */
            ctx->nrc = NX_UDS_NRC_REQUEST_OUT_OF_RANGE;
            return NX_UDS_DISPOSITION_NEGATIVE;
        }
        len = (uint32_t)left;   /* the last block of an upload is a short one */
    }

    ctx->out[1] = bsc;
    if (xfer->run.dir == (uint8_t)NX_UDS_XFER_DOWNLOAD) {
        if (!xfer->cfg.write_fn(xfer->cfg.user, xfer->run.addr + xfer->run.done,
                                &ctx->req[NX_UDS_XFER_BLOCK_OVERHEAD], len,
                                &nrc)) {
            /* Nothing is advanced, so the client may send the same block again. */
            ctx->nrc = nrc;
            return NX_UDS_DISPOSITION_NEGATIVE;
        }
        ctx->out_len = NX_UDS_XFER_BLOCK_OVERHEAD;
    } else {
        nx_uds_disposition_t d = xfer_fill_upload(xfer, ctx, xfer->run.done, len);

        if (d != NX_UDS_DISPOSITION_DONE) {
            return d;
        }
    }

    /* Committed. The counter advances by one and wraps of its own accord, so the
     * value after 0xFF is 0x00 and is an ordinary value from then on. */
    xfer->run.done += len;
    xfer->run.bsc_last  = bsc;
    xfer->run.bsc_next  = (uint8_t)(bsc + 1u);
    xfer->run.committed = true;
    xfer->run.last_len  = len;
    return NX_UDS_DISPOSITION_DONE;
}

/* ------------------------------------------------------------------ */
/* Finishing: 0x37                                                   */
/* ------------------------------------------------------------------ */
nx_uds_disposition_t nx_uds_svc_transfer_exit(nx_uds_ctx_t *ctx, void *user)
{
    nx_uds_xfer_t *xfer = (nx_uds_xfer_t *)user;
    const uint8_t *record = NULL;
    uint32_t record_len = 0u;
    uint32_t out_len = 0u;
    uint8_t  nrc = NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE;

    if (xfer == NULL) {
        ctx->nrc = NX_UDS_NRC_CONDITIONS_NOT_CORRECT;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (ctx->phase != NX_UDS_PHASE_REQUEST) {
        return NX_UDS_DISPOSITION_DONE;
    }
    if (xfer->run.dir == (uint8_t)NX_UDS_XFER_NONE) {
        ctx->nrc = NX_UDS_NRC_REQUEST_SEQUENCE_ERROR;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* A transfer stopped short of what it declared is refused where the product
     * wants the whole region, and accepted where a client writing an image smaller
     * than the space reserved for it is ordinary. */
    if (xfer->cfg.require_full_size && xfer->run.done != xfer->run.size) {
        ctx->nrc = NX_UDS_NRC_REQUEST_SEQUENCE_ERROR;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    if (ctx->req_len > 1u) {
        record     = &ctx->req[1];
        record_len = ctx->req_len - 1u;
    }

    if (xfer->cfg.close_fn != NULL
        && !xfer->cfg.close_fn(xfer->cfg.user,
                               (nx_uds_xfer_dir_t)xfer->run.dir,
                               xfer->run.done, xfer->run.size,
                               record, record_len,
                               &ctx->out[1],
                               (ctx->out_cap > 1u) ? (ctx->out_cap - 1u) : 0u,
                               &out_len, &nrc)) {
        /* Refused, and the transfer stays open: whatever made the image unusable may
         * be something the client can put right by sending more. */
        ctx->nrc = nrc;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }
    if (1u + out_len > ctx->out_cap) {
        ctx->nrc = NX_UDS_NRC_RESPONSE_TOO_LONG;
        return NX_UDS_DISPOSITION_NEGATIVE;
    }

    /* The transfer is over. The session, the unlocked level and the quiet timer are
     * not this service's to touch: running several transfers in one session without
     * unlocking again is ordinary. */
    xfer_clear(xfer);
    ctx->out_len = 1u + out_len;
    return NX_UDS_DISPOSITION_DONE;
}

