/**
 * @file    nx_uds_client.h
 * @brief   ISO 14229 diagnostic client (the test tool side), in pure C.
 *
 * Takes a request A_PDU and reports the outcome: the response A_PDU that came
 * back, or why none did. What carries the request is not described here and not
 * known to this module: a request leaves through nx_uds_client_send_fn, which
 * the application wires to whatever it is speaking over, and a response enters
 * through nx_uds_client_indicate() as plain bytes plus how it was addressed.
 *
 * The client runs one transaction at a time, and a transaction is a question and
 * the wait for its answer. It sends a request, waits P2 for a response, and if
 * the server says the answer is still coming (a negative response carrying 0x78
 * responsePending) waits P2* and keeps going, up to a bounded number of
 * extensions. The wait windows are the server's to set: a 0x10 positive response
 * publishes P2 and P2*, and the client adopts them for the conversation unless it
 * is configured with @c fixed_timing, in which case it always uses its own
 * values.
 *
 * Nothing here allocates. The request and response buffers are caller-provided,
 * and the request is kept in the caller's buffer for as long as the transaction
 * runs. The module is non-blocking: every operation starts or advances some part
 * of a state machine, and the caller pumps it with nx_uds_client_process() once
 * per main-loop iteration.
 *
 * One request in flight. A second is refused until the first resolves, so the
 * client is a test tool that asks and waits rather than a peer that streams.
 */
#ifndef NX_UDS_CLIENT_H
#define NX_UDS_CLIENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_uds.h"
#include "nx_tp_sdu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief P2_client default: how long to wait for a response, in us. */
#define NX_UDS_CLIENT_DEFAULT_P2_US            50000u

/** @brief P2*_client default: how long one extension may take, in us. */
#define NX_UDS_CLIENT_DEFAULT_P2_STAR_US       5000000u

/** @brief Default limit on how long the send path may refuse a request, in us. */
#define NX_UDS_CLIENT_DEFAULT_SEND_TIMEOUT_US  5000000u

/** @brief Default limit on how many 0x78 extensions one transaction tolerates. */
#define NX_UDS_CLIENT_DEFAULT_MAX_PENDING      32u

/**
 * @brief Return codes for module operations.
 */
typedef enum {
    NX_UDS_CLIENT_OK = 0,       /**< Operation succeeded */
    NX_UDS_CLIENT_ERR_PARAM,    /**< Invalid argument (NULL pointer, bad config, bad length) */
    NX_UDS_CLIENT_ERR_BUSY,     /**< A transaction is already running */
    NX_UDS_CLIENT_ERR_STATE     /**< Wrong state to accept the operation */
} nx_uds_client_ret_t;

/**
 * @brief How a transaction ended.
 *
 * The application is told this once, at the moment the transaction resolves,
 * through the configuration's @c result_fn. The response that arrived - if one
 * did - is in the response buffer, and its length is available from
 * nx_uds_client_resp_len().
 */
typedef enum {
    NX_UDS_CLIENT_RESULT_OK = 0,       /**< A positive response to the request arrived */
    NX_UDS_CLIENT_RESULT_NEGATIVE,     /**< A negative response to the request arrived;
                                        the request was understood and refused */
    NX_UDS_CLIENT_RESULT_NO_RESPONSE,  /**< The first wait window ran out in silence
                                        where the request had asked for no positive
                                        response: the server was owed none, so the
                                        silence is the expected outcome rather than
                                        a failure */
    NX_UDS_CLIENT_RESULT_TIMEOUT,      /**< A response was owed and none came: the
                                        send path never accepted the request, it
                                        reported the request failed to transmit, a
                                        request that did not ask for silence was met
                                        with silence, a server that had said the
                                        answer was coming went quiet for its whole
                                        extension, or more 0x78 extensions arrived
                                        than allowed */
    NX_UDS_CLIENT_RESULT_PROTOCOL_ERROR, /**< A frame arrived that the state does not
                                        allow: a response for another service, or
                                        one this client cannot hold */
    NX_UDS_CLIENT_RESULT_CANCELED      /**< The transaction was canceled by the caller */
} nx_uds_client_result_t;

/**
 * @brief  Hand a finished request to whatever is carrying this conversation.
 *
 * Called from nx_uds_client_process() with a complete request A_PDU. The bytes
 * belong to the client and are valid only for the duration of the call, so an
 * implementation that queues them copies them.
 *
 * @param  user    The configuration's @c send_user, untouched by the client.
 * @param  link    The connection to send on.
 * @param  req     The request, service identifier included.
 * @param  len     Its length in bytes.
 * @param  ta_type How the request is addressed; an nx_tp_ta_type_t value.
 *
 * @return true if the request was taken. false means the carrier could not take
 *         it now, and the client offers the same request again on later
 *         process() calls until @c send_timeout_us runs out.
 */
typedef bool (*nx_uds_client_send_fn)(void *user, uint8_t link, const uint8_t *req,
                                      uint32_t len, uint8_t ta_type);

/** @brief A diagnostic client instance. */
typedef struct nx_uds_client nx_uds_client_t;

/**
 * @brief  Called when a transaction resolves, with its outcome.
 *
 * Reports how the transaction ended, after it has resolved: the response, when
 * there is one, is in the client's response buffer and its length is available
 * from nx_uds_client_resp_len(). Optional.
 *
 * @param  user   The configuration's @c result_user, untouched by the client.
 * @param  clt    The client, so the application can read the response buffer and
 *                query the session or timing at the moment of the result.
 * @param  result How the transaction ended; see nx_uds_client_result_t.
 */
typedef void (*nx_uds_client_result_fn)(void *user, nx_uds_client_t *clt,
                                        nx_uds_client_result_t result);

/**
 * @brief  Configuration (copied into the instance by init).
 *
 * The request and response buffers are caller-owned and must outlive the
 * instance. Every timing field treats 0 as "use the documented default".
 */
typedef struct {
    /* ---- what to do with the outcome ---- */
    nx_uds_client_result_fn result_fn; /**< Reports how a transaction ended; may be
                                        NULL. */
    void                   *result_user; /**< Passed to the result callback untouched */

    /* ---- where a request goes ---- */
    nx_uds_client_send_fn  send_fn;    /**< Hands a finished request to the carrier.
                                        May be NULL where the carrier installs
                                        itself afterwards with
                                        nx_uds_client_set_send, which is what a
                                        binding does; until one is installed the
                                        client's send path never accepts a request
                                        and the transaction times out. */
    void                   *send_user; /**< Passed to the send callback untouched */

    /* ---- the buffers ---- */
    uint8_t   *req_buf;                /**< Where the request in flight is kept.
                                        Caller-owned. Required, at least 1 byte; a
                                        request built by nx_uds_client_request
                                        needs at least 2. */
    size_t     req_buf_size;           /**< Its size in bytes. Required. A request
                                        longer than this is refused by request(). */
    uint8_t   *rsp_buf;                /**< Where the response that arrived is kept,
                                        so the application can read it from the
                                        result callback. Caller-owned. Required,
                                        and at least NX_UDS_NEG_RSP_LEN so a
                                        negative response always fits. */
    size_t     rsp_buf_size;           /**< Its size in bytes. Required. A response
                                        longer than this is reported as a protocol
                                        error, so size it to the largest response
                                        the client expects. */

    /* ---- identity ---- */
    uint8_t    link;                   /**< The connection this instance speaks on. A
                                        response or outcome from another is
                                        refused, so one instance stays one
                                        conversation. */

    /* ---- timing ---- */
    uint32_t (*get_us)(void);          /**< Free-running microsecond counter
                                        (wrap-safe). Required. */
    uint32_t  p2_us;                   /**< Default window to wait for a response, in
                                        us. 0 = 50 ms. Ignored while a 0x10 positive
                                        response has published its own value and
                                        @c fixed_timing is false. */
    uint32_t  p2_star_us;              /**< Default window one 0x78 extension grants,
                                        in us. 0 = 5 s. */
    uint32_t  send_timeout_us;         /**< Longest the send path may refuse a request
                                        before the transaction is given up on, in us.
                                        0 = 5 s. */
    uint8_t   max_pending;             /**< How many 0x78 extensions one transaction
                                        tolerates before it is given up on. 0 = 32.
                                        A bound on time alone does not bound the
                                        wait: a server that never finishes would
                                        keep extending the window forever. */
    bool      fixed_timing;            /**< true: always use @c p2_us and @c
                                        p2_star_us, ignoring whatever a 0x10
                                        positive response publishes. false (the
                                        default): adopt the values the server
                                        publishes, so the wait is what the server
                                        says it is. */
} nx_uds_client_cfg_t;

/**
 * @brief State of the single transaction in flight.
 *
 * @note  Internal state; do not access directly.
 */
typedef struct {
    uint8_t  state;        /**< Transaction state machine position */
    uint8_t  sid;          /**< Service identifier of the request in flight */
    bool     suppress_pos; /**< The request asked for no positive response. Cleared
                            once a 0x78 extension arrives, because having said the
                            answer is coming the server owes one. */
    uint8_t  ta_type;      /**< How the request is addressed; an nx_tp_ta_type_t
                            value */
    uint8_t  link;         /**< The connection, mirroring the configuration */
    uint8_t  pend_count;   /**< 0x78 extensions received so far */
    uint32_t req_len;      /**< Bytes of the request kept in the request buffer */
    uint32_t started;      /**< get_us() when the transaction was armed */
    uint32_t send_deadline;/**< get_us() by which the send path must have accepted
                            the request */
    uint32_t deadline;     /**< get_us() by which the current wait must be over */
} nx_uds_client_txn_t;

/**
 * @brief Instance of a diagnostic client (one conversation per instance).
 *
 * Declare one in static storage and hand it to nx_uds_client_init.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed
 *        to nx_uds_client_init.
 */
typedef struct nx_uds_client {
    nx_uds_client_cfg_t cfg;      /**< Copied configuration */
    struct {
        nx_uds_client_txn_t txn;  /**< The transaction in flight */
        uint32_t resp_len;        /**< Bytes of the response buffer that are valid.
                                   Cleared when a transaction is armed, set when a
                                   response arrives. */
        uint32_t p2_us;           /**< Effective ordinary window, in us: the
                                   configured value or one a 0x10 positive response
                                   published. */
        uint32_t p2_star_us;      /**< Effective extended window, in us. */
        uint8_t  session;         /**< Active session, as learned from a 0x10
                                   positive response; an nx_uds_session_t value. */
    } run;                        /**< Internal runtime state */
} nx_uds_client_t;

/**
 * @brief  Initialize an instance from a configuration.
 *
 * The client starts idle in the default session, with its wait windows set to the
 * configured values. The send path may be supplied later with
 * nx_uds_client_set_send, which is what a binding that needs the client's address
 * to attach itself does.
 *
 * @param  clt Instance to initialize, must not be NULL.
 * @param  cfg Configuration, must not be NULL; @c req_buf, @c rsp_buf and
 *             @c get_us required, the buffers at least 1 byte and the response
 *             buffer at least NX_UDS_NEG_RSP_LEN.
 *
 * @return NX_UDS_CLIENT_OK on success; ERR_PARAM where anything required is
 *         missing or mis-sized.
 */
nx_uds_client_ret_t nx_uds_client_init(nx_uds_client_t *clt,
                                       const nx_uds_client_cfg_t *cfg);

/**
 * @brief  Send a request for a service that carries a sub-function.
 *
 * Builds the request A_PDU from its parts: the service identifier, the
 * sub-function byte - including the suppression bit, which the caller sets to ask
 * for no positive response - and the data that follows. The bit is read both into
 * the frame and into the transaction, so a request that asks for silence is
 * interpreted as one that asked for silence.
 *
 * Nothing is sent from inside this call: the request is armed and offered to the
 * send path from nx_uds_client_process().
 *
 * @param  clt     Instance, must not be NULL.
 * @param  sid     The service identifier.
 * @param  subfunc The sub-function byte, suppression bit as desired.
 * @param  data    The bytes after the sub-function; may be NULL when @p len is 0.
 * @param  len     How many such bytes.
 * @param  ta_type How the request is addressed; an nx_tp_ta_type_t value.
 *
 * @return NX_UDS_CLIENT_OK if the transaction was armed; ERR_PARAM on bad
 *         arguments or a request that does not fit the request buffer; ERR_BUSY
 *         when a transaction is already running.
 */
nx_uds_client_ret_t nx_uds_client_request(nx_uds_client_t *clt, uint8_t sid,
                                          uint8_t subfunc, const uint8_t *data,
                                          size_t len, nx_tp_ta_type_t ta_type);

/**
 * @brief  Send an already assembled request A_PDU.
 *
 * For a frame the client is not asked to understand: the bytes are kept and sent
 * as they are, and the send path is told how the request is addressed. Because
 * the client did not build a sub-function byte, it does not read a suppression
 * request off the frame; a request that asks for silence should be sent with
 * nx_uds_client_request, which makes the bit known to the transaction.
 *
 * Nothing is sent from inside this call.
 *
 * @param  clt     Instance, must not be NULL.
 * @param  req     The request, service identifier included, must not be NULL.
 * @param  len     Its length in bytes, must be > 0 and fit the request buffer.
 * @param  ta_type How the request is addressed; an nx_tp_ta_type_t value.
 *
 * @return NX_UDS_CLIENT_OK if the transaction was armed; ERR_PARAM on bad
 *         arguments; ERR_BUSY when a transaction is already running.
 */
nx_uds_client_ret_t nx_uds_client_request_raw(nx_uds_client_t *clt,
                                              const uint8_t *req, size_t len,
                                              nx_tp_ta_type_t ta_type);

/**
 * @brief  Drive the client once; call periodically from the main loop.
 *
 * Each call offers the request in flight to the send path, or advances the wait
 * for a response, or reports a cancellation.
 *
 * A request the send path would not take is offered again on the next call, so a
 * transmit path that fills up briefly costs the transaction the time it takes to
 * drain - but only until @c send_timeout_us, after which the transaction is given
 * up on. A wait that runs out resolves as NO_RESPONSE where the request asked for
 * silence, and as TIMEOUT where a response was owed and none came.
 *
 * @param  clt Instance, must not be NULL.
 * @return NX_UDS_CLIENT_OK; ERR_PARAM on a NULL instance.
 */
nx_uds_client_ret_t nx_uds_client_process(nx_uds_client_t *clt);

/**
 * @brief  Give the client a response that arrived.
 *
 * The response is copied into the response buffer and interpreted against the
 * transaction in flight: a positive response for the requested service resolves
 * the transaction, a negative response either pends it (0x78) or resolves it as
 * refused, and anything else is a protocol error.
 *
 * @param  clt     Instance, must not be NULL.
 * @param  rsp     The response, must not be NULL.
 * @param  len     Its length in bytes, must be > 0.
 * @param  ta_type How it was addressed; an nx_tp_ta_type_t value. Carried for
 *                 completeness; the client does not arbitrate a response on it.
 * @param  link    Which connection it arrived on; must be the configured one.
 *
 * @return NX_UDS_CLIENT_OK if the response was accepted; ERR_PARAM on bad
 *         arguments or a link that is not this instance's; ERR_STATE when no
 *         transaction is waiting for a response, which is what a response that
 *         arrives out of the blue will see.
 */
nx_uds_client_ret_t nx_uds_client_indicate(nx_uds_client_t *clt, const uint8_t *rsp,
                                           uint32_t len, uint8_t ta_type,
                                           uint8_t link);

/**
 * @brief  Report what became of the request the client handed to the carrier.
 *
 * The transaction is not over when the request is handed down: it is over when
 * the request has actually gone out, and a failure here is heard immediately
 * rather than after the response window runs out. A successful outcome does
 * nothing - the client is already waiting for a response - and a report for a
 * transaction that is not waiting is ignored.
 *
 * @param  clt    Instance, must not be NULL.
 * @param  link   Which connection the report concerns; must be the configured one.
 * @param  result The carrier's outcome; an nx_tp_result_t value. Anything other
 *                than NX_TP_N_OK ends the transaction as TIMEOUT.
 */
void nx_uds_client_confirm(nx_uds_client_t *clt, uint8_t link, uint8_t result);

/**
 * @brief  Drop the transaction in flight.
 *
 * The cancellation is reported to the application, through the result callback,
 * at the next nx_uds_client_process() call rather than here: an application can
 * cancel from anywhere, and the reporting happens at a clean point in the loop.
 *
 * @param  clt Instance, must not be NULL.
 * @return NX_UDS_CLIENT_OK if a running transaction was canceled; ERR_PARAM on a
 *         NULL instance; ERR_STATE when nothing is running, or it is already
 *         being canceled.
 */
nx_uds_client_ret_t nx_uds_client_cancel(nx_uds_client_t *clt);

/**
 * @brief  Whether a transaction is in flight.
 *
 * @param  clt Instance, must not be NULL.
 * @return true while a request is being sent or awaited.
 */
bool nx_uds_client_is_busy(const nx_uds_client_t *clt);

/**
 * @brief  The active diagnostic session.
 *
 * Learned from a 0x10 positive response; before one arrives the client assumes
 * the default session.
 *
 * @param  clt Instance, must not be NULL.
 * @return An nx_uds_session_t value; the default session when @p clt is NULL.
 */
uint8_t nx_uds_client_session(const nx_uds_client_t *clt);

/**
 * @brief  The wait windows in use, in microseconds.
 *
 * Reports the effective values - the configured ones until a 0x10 positive
 * response publishes its own, and the published ones from then on unless
 * @c fixed_timing is set.
 *
 * @param  clt      Instance, must not be NULL.
 * @param  p2       Where to store the ordinary window; may be NULL.
 * @param  p2_star  Where to store the window a pending notification extends it
 *                  to; may be NULL.
 */
void nx_uds_client_timing(const nx_uds_client_t *clt, uint32_t *p2,
                          uint32_t *p2_star);

/**
 * @brief  Change the wait windows, in microseconds.
 *
 * Sets both the configured values and the ones in use, so the change affects the
 * next window. Why an application does this: a server that never publishes timing
 * (one that answers a session request without it, or a conversation that never
 * leaves the default session) is awaited with whatever the application says here.
 *
 * @param  clt      Instance, must not be NULL.
 * @param  p2_us    The ordinary window.
 * @param  p2_star_us The window a pending notification extends it to.
 */
void nx_uds_client_set_timing(nx_uds_client_t *clt, uint32_t p2_us,
                              uint32_t p2_star_us);

/**
 * @brief  Bytes of the response buffer that are valid.
 *
 * The length of the response the last transaction resolved with, read from the
 * result callback. 0 where no response was received.
 *
 * @param  clt Instance, must not be NULL.
 * @return The length in bytes, or 0.
 */
uint32_t nx_uds_client_resp_len(const nx_uds_client_t *clt);

/**
 * @brief  Change where finished requests go.
 *
 * What a binding calls to make itself the client's send path, so that the two
 * need not be configured with each other's addresses in a particular order: the
 * client is initialised, then whatever carries it attaches. A configuration that
 * names no send path at all is accepted for exactly this reason, and a client
 * left without one never gets a request onto the link and times out.
 *
 * @param  clt  Instance, must not be NULL.
 * @param  fn   The callback to hand requests to, must not be NULL.
 * @param  user Passed to it untouched.
 * @return NX_UDS_CLIENT_OK, or ERR_PARAM where either pointer is NULL.
 */
nx_uds_client_ret_t nx_uds_client_set_send(nx_uds_client_t *clt,
                                           nx_uds_client_send_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_CLIENT_H */
