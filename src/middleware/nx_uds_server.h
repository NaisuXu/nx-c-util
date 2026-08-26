/**
 * @file    nx_uds_server.h
 * @brief   ISO 14229 diagnostic server (the ECU side), in pure C.
 *
 * Takes a request A_PDU, finds the service that implements it, and produces the
 * response A_PDU. What carried the request is not described here and not known
 * to this module: a request enters through nx_uds_server_indicate() as plain
 * bytes plus how it was addressed, and a response leaves through a callback the
 * application wires to whatever it is speaking over.
 *
 * The set of services is the application's, held as a table of
 * nx_uds_service_t rows. The server dispatches into it and knows nothing about
 * any particular service; the ones this library provides are exported handler
 * functions that occupy rows like any other. A service is added by writing a
 * handler and adding a row, and nothing in this module is edited.
 *
 * What the server owns is what every service shares: finding the row, checking
 * the request against it (session, sub-function, length, security), the session
 * itself and the S3 timer that drops it, the response timing ISO 14229-2
 * prescribes (P2, P2*, P4), the pending notification that extends it, and the
 * negative responses that belong to none of the services.
 *
 * One transaction at a time. A request is accepted, run to its response, and
 * only then is the next one taken - so nx_uds_server_indicate() reports busy
 * rather than starting a second, and a handler that needs several cycles keeps
 * the transaction until it is finished.
 *
 * Timing is supplied by the caller through the @c get_us callback, read on
 * demand inside process(). Nothing is allocated: the transaction's response
 * buffer is caller-provided, and the request is read in place.
 */
#ifndef NX_UDS_SERVER_H
#define NX_UDS_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "nx_uds.h"
#include "nx_tp_sdu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return codes for module operations.
 */
typedef enum {
    NX_UDS_SERVER_OK = 0,       /**< Operation succeeded */
    NX_UDS_SERVER_ERR_PARAM,    /**< Invalid argument (NULL pointer, bad config, bad length) */
    NX_UDS_SERVER_ERR_BUSY,     /**< A transaction is already running */
    NX_UDS_SERVER_ERR_STATE     /**< Wrong state to accept the operation */
} nx_uds_server_ret_t;

/** @brief P2_server default: how long an answer may take, in us. */
#define NX_UDS_SERVER_DEFAULT_P2_US        50000u

/** @brief P2*_server default: how long one extension may take, in us. */
#define NX_UDS_SERVER_DEFAULT_P2_STAR_US   5000000u

/** @brief P4_server default: how long the whole transaction may take, in us. */
#define NX_UDS_SERVER_DEFAULT_P4_US        20000000u

/** @brief S3_server default: quiet time that ends a non-default session, in us. */
#define NX_UDS_SERVER_DEFAULT_S3_US        5000000u

/** @brief Default limit on how often one transaction may say it is not finished. */
#define NX_UDS_SERVER_DEFAULT_MAX_PENDING  32u

/**
 * @brief  Hand a finished response to whatever is carrying this conversation.
 *
 * Called from process() with a complete response A_PDU. The bytes belong to the
 * server and are valid only for the duration of the call, so an implementation
 * that queues them copies them.
 *
 * @param  user    The configuration's @c out_user, untouched by the server.
 * @param  link    The connection the request came in on.
 * @param  rsp     The response, response identifier included.
 * @param  len     Its length in bytes.
 * @param  ta_type How the request was addressed; an nx_tp_ta_type_t value.
 *
 * @return true if the response was taken. false means the carrier could not take
 *         it now, and the server offers the same response again on later
 *         process() calls until P4 runs out.
 */
typedef bool (*nx_uds_output_fn)(void *user, uint8_t link, const uint8_t *rsp,
                                uint32_t len, uint8_t ta_type);

/**
 * @brief  Called when the session changes, so the application can follow it.
 *
 * Reports what the session became, after the change has taken effect. Optional.
 *
 * @param  user The configuration's @c out_user, untouched by the server.
 * @param  from The session that ended; an nx_uds_session_t value.
 * @param  to   The session now active; an nx_uds_session_t value.
 */
typedef void (*nx_uds_session_change_fn)(void *user, uint8_t from, uint8_t to);

/**
 * @brief  Configuration (copied into the instance by init).
 *
 * The service table and the response buffer are caller-owned and must outlive
 * the instance. Every timing field treats 0 as "use the documented default".
 */
typedef struct {
    /* ---- what the server implements ---- */
    const nx_uds_service_t *services;  /**< The service table. Required. */
    uint16_t services_count;           /**< How many rows it has. Required. */

    /* ---- where a response goes ---- */
    nx_uds_output_fn out_fn;           /**< Hands a finished response to the
                                        carrier. May be NULL where the carrier
                                        installs itself afterwards with
                                        nx_uds_server_set_output; until one is
                                        installed the server holds its answers
                                        rather than sending them. */
    void            *out_user;         /**< Passed to the callbacks untouched */
    nx_uds_session_change_fn session_fn; /**< Reports a session change; may be NULL */

    /* ---- the buffers ---- */
    uint8_t  *req_buf;                 /**< Where an accepted request is kept for
                                        as long as its transaction runs.
                                        Caller-owned. Required.

                                        The server copies the request here rather
                                        than pointing at the bytes it was handed,
                                        because a transaction outlives the call
                                        that started it: a handler that needs
                                        several cycles reads its request on each
                                        of them, and whatever delivered the
                                        request is free to reuse its storage as
                                        soon as indicate() returns. */
    uint32_t  req_buf_size;            /**< Its size in bytes. Required, at least 1.
                                        A request longer than this is refused with
                                        0x13, so it also bounds what this server
                                        accepts. */
    uint8_t  *out_buf;                 /**< Where a response is assembled.
                                        Caller-owned. Required. */
    uint32_t  out_buf_size;            /**< Its size in bytes. Required, and at
                                        least NX_UDS_NEG_RSP_LEN so a negative
                                        response always fits. */

    /* ---- what this server can carry ---- */
    uint32_t  max_req_apdu;            /**< Longest request this server accepts, in
                                        bytes. A longer one is refused with 0x13.
                                        The application derives it from what its
                                        own receive path and its carrier allow.
                                        0 = accept whatever arrives. */
    uint32_t  max_resp_apdu;           /**< Longest response this server produces,
                                        in bytes. Caps what a handler may write,
                                        and a handler that needs more is answered
                                        for it with 0x14. 0 = the buffer's size. */

    /* ---- identity ---- */
    uint8_t   link;                    /**< The connection this instance serves.
                                        An indication from another is refused, so
                                        one instance stays one conversation. */

    /* ---- what to do with a request that is not one ---- */
    bool      accept_rsp_range_sid;    /**< true: treat an identifier from the
                                        response ranges (0x40..0x7F, 0xC0..0xFF)
                                        as an ordinary request, which the service
                                        table will then refuse with 0x11.
                                        false: discard it without answering.

                                        Those identifiers name responses rather
                                        than requests, so one arriving is a
                                        response that has come back around. Two
                                        servers sharing a functional address will
                                        answer each other's refusals indefinitely
                                        if each treats the other's as a request,
                                        which is why the quiet reading is the
                                        default. */

    /* ---- timing ---- */
    uint32_t (*get_us)(void);          /**< Free-running microsecond counter (wrap-safe).
                                        Required. */
    uint32_t  p2_us;                   /**< Time an answer may take before the
                                        server says it is still coming, in us.
                                        0 = 50 ms. */
    uint32_t  p2_star_us;              /**< Time one extension may take before the
                                        server says so again, in us. 0 = 5 s. */
    uint32_t  p2_adjust_us;            /**< How far ahead of those windows to say
                                        the answer is still coming, in us. 0 = at
                                        the window itself.

                                        The client measures its wait from when the
                                        request went out, and the notification has
                                        to be on the wire before that wait is up -
                                        so a server that starts sending exactly at
                                        the limit is already late by however long
                                        the carrier takes. */
    uint32_t  p4_us;                   /**< Time the whole transaction may take,
                                        extensions included, in us. 0 = 20 s. */
    uint8_t   p4_nrc;                  /**< What to answer when a transaction runs
                                        past P4 and the service names nothing of
                                        its own. 0 = 0x21 busyRepeatRequest. */
    uint32_t  s3_us;                   /**< Quiet time after which a non-default
                                        session returns to default, in us.
                                        0 = 5 s. */
    uint8_t   max_pending;             /**< How many times a transaction may say
                                        its answer is still coming before it is
                                        given up on. 0 = 32.

                                        A limit on time alone does not bound the
                                        traffic: a handler that is not finished
                                        after every short interval would keep
                                        saying so, filling the link with
                                        notifications for as long as the whole
                                        transaction is allowed to last. Whichever
                                        of the two limits is reached first ends
                                        the transaction, and both answer with
                                        @c p4_nrc. */
} nx_uds_server_cfg_t;

/**
 * @brief State of the single transaction in flight.
 *
 * @note  Internal state; do not access directly.
 */
typedef struct {
    uint8_t  state;        /**< Transaction state machine position */
    uint16_t row;          /**< Which service table row is running it, or the row
                            count when the request matched none - a refusal still
                            needs a transaction to carry it, and that one has no
                            row behind it. Wide enough for the whole table, so a
                            long one cannot fold a high row onto a low one. */
    uint8_t  pend_count;   /**< Pending notifications sent so far */
    bool     pending_sent; /**< Whether the client has been told to wait, which
                            obliges the server to answer even where it would
                            otherwise have stayed quiet */
    bool     pend_stuck;   /**< A notification is due and the carrier has not taken
                            one yet; @c pend_give_up is when to stop trying */
    uint32_t pend_give_up; /**< get_us() by which a notification must have been
                            taken, or the client is waiting on silence */
    bool     answered;     /**< The response is assembled and waiting for the link */
    uint32_t started;      /**< get_us() when the request was accepted */
    uint32_t deadline;     /**< get_us() by which the current wait must be over:
                            the answer under P2 or P2*, or the link taking the
                            response */
    nx_uds_ctx_t ctx;      /**< The transaction as its handler sees it */
} nx_uds_txn_t;

/**
 * @brief Instance of a diagnostic server (one conversation per instance).
 *
 * Declare one in static storage and hand it to nx_uds_server_init.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed
 *        to nx_uds_server_init.
 */
typedef struct nx_uds_server {
    nx_uds_server_cfg_t cfg;      /**< Copied configuration */
    struct {
        uint8_t  session;         /**< Active session; an nx_uds_session_t value */
        uint8_t  sec_level;       /**< Unlocked security level, 0 when locked */
        bool     s3_armed;        /**< Whether the quiet timer is running */
        uint32_t s3_deadline;     /**< get_us() at which the session drops */
        nx_uds_txn_t txn;         /**< The transaction in flight */
    } run;                        /**< Internal runtime state */
} nx_uds_server_t;

/**
 * @brief  Initialize an instance from a configuration.
 *
 * The server starts in the default session with nothing unlocked and no
 * transaction running.
 *
 * @param  srv Instance to initialize, must not be NULL.
 * @param  cfg Configuration, must not be NULL; @c services with
 *             @c services_count > 0 and every row carrying a handler;
 *             @c out_buf and @c get_us required; @c out_buf_size at
 *             least NX_UDS_NEG_RSP_LEN; @c max_resp_apdu, when non-zero, no
 *             larger than @c out_buf_size. @c out_fn may be left NULL and
 *             supplied later with nx_uds_server_set_output, which is what a
 *             carrier needing the server's address to attach itself does.
 *
 * @return true on success; false on any invalid argument.
 */
bool nx_uds_server_init(nx_uds_server_t *srv, const nx_uds_server_cfg_t *cfg);

/**
 * @brief  Give the server a request that arrived.
 *
 * Validates the request against the service table and starts a transaction for
 * it. A request the table refuses still starts one, because the negative
 * response it earns has to be sent; a functionally addressed request that earns
 * a negative response starts one too, and the response is dropped rather than
 * sent, so the handler sees the same phases either way.
 *
 * Nothing is sent from inside this call: the response leaves from process(),
 * which is also where the timers are read. A handler is invoked here for its
 * first phase, so a service that answers immediately has its answer ready by the
 * time this returns.
 *
 * @param  srv     Instance, must not be NULL.
 * @param  req     The request, service identifier included, must not be NULL.
 * @param  len     Its length in bytes; must be > 0.
 * @param  ta_type How it was addressed; an nx_tp_ta_type_t value.
 * @param  link    Which connection it arrived on; must be the configured one.
 *
 * A request that arrives while a transaction is running is refused with
 * ERR_BUSY and the running transaction is left strictly alone. The alternative -
 * cancelling what is running to take the newcomer - loses the work in progress to
 * any request at all, including a broadcast tester-present that was never
 * addressed to this server in particular; a flashing conversation would be torn
 * down mid-block by traffic meant for somebody else. The caller decides what to
 * do with the refused request: answering it with 0x21 busyRepeatRequest asks the
 * client to send it again, and dropping it is also correct for a request that was
 * functionally addressed.
 *
 * @return NX_UDS_SERVER_OK if a transaction was started; ERR_PARAM on bad
 *         arguments or a link that is not this instance's; ERR_BUSY when a
 *         transaction is already running.
 */
nx_uds_server_ret_t nx_uds_server_indicate(nx_uds_server_t *srv, const uint8_t *req,
                                           uint32_t len, uint8_t ta_type, uint8_t link);

/**
 * @brief  Report what became of a response the server handed to the carrier.
 *
 * The transaction is not over when the response is handed down - it is over when
 * the response has actually gone out, and that is what this reports. A handler
 * whose work depends on its answer having been delivered (a reset that must not
 * happen before the client hears the acknowledgement) acts in the CONFIRM phase,
 * which this call delivers.
 *
 * @param  srv    Instance, must not be NULL.
 * @param  link   Which connection the report concerns.
 * @param  result The carrier's outcome; an nx_tp_result_t value. Anything other
 *                than NX_TP_N_OK ends the transaction through LINK_ERROR.
 *
 * @return NX_UDS_SERVER_OK when the report was applied; ERR_PARAM on bad
 *         arguments; ERR_STATE when no response was awaiting confirmation, which
 *         a carrier confirming twice or confirming late will see.
 */
nx_uds_server_ret_t nx_uds_server_confirm(nx_uds_server_t *srv, uint8_t link,
                                          uint8_t result);

/**
 * @brief  Drive the server once; call periodically from the main loop.
 *
 * Each call re-enters a handler that has not finished, offers a finished response
 * to the carrier, emits the pending notification that keeps a slow transaction
 * alive past P2, ends a transaction that has run past P4, and drops the session
 * when S3 has elapsed.
 *
 * A response the carrier would not take is offered again on the next call, so a
 * transmit path that fills up briefly costs the transaction nothing but the time
 * it takes to drain.
 *
 * @param  srv Instance, must not be NULL.
 */
void nx_uds_server_process(nx_uds_server_t *srv);

/**
 * @brief  Whether a transaction is running.
 *
 * @param  srv Instance, must not be NULL.
 * @return true while a request is being answered.
 */
bool nx_uds_server_is_busy(const nx_uds_server_t *srv);

/**
 * @brief  The active diagnostic session.
 *
 * @param  srv Instance, must not be NULL.
 * @return An nx_uds_session_t value; the default session when @p srv is NULL.
 */
uint8_t nx_uds_server_session(const nx_uds_server_t *srv);

/**
 * @brief  The unlocked security level, 0 when nothing is unlocked.
 *
 * @param  srv Instance, must not be NULL.
 * @return The level a service row's @c sec_level is compared against.
 */
uint8_t nx_uds_server_sec_level(const nx_uds_server_t *srv);

/**
 * @brief  The response windows the server holds itself to, in microseconds.
 *
 * A service that publishes the timing to the client reads it here rather than
 * being configured with its own copy, so that what is announced is what is
 * actually enforced.
 *
 * @param  srv     Instance, must not be NULL.
 * @param  p2      Where to store the ordinary window; may be NULL.
 * @param  p2_star Where to store the window a pending notification extends it
 *                 to; may be NULL.
 */
void nx_uds_server_timing(const nx_uds_server_t *srv, uint32_t *p2,
                          uint32_t *p2_star);

/**
 * @brief  The clock the server measures its own windows against.
 *
 * A service keeping a deadline of its own reads the time here, so that it and the
 * server cannot end up on two clocks that drift apart.
 *
 * @param  srv Instance, must not be NULL.
 * @return The microsecond count, which wraps; compare differences, not values.
 */
uint32_t nx_uds_server_now(const nx_uds_server_t *srv);

/**
 * @brief  The longest request and response this server carries, in bytes.
 *
 * A service that announces a size to the client reads it here, so that what is
 * announced cannot exceed what the server would accept or produce.
 *
 * @param  srv Instance, must not be NULL.
 * @param  req Where to store the request limit; may be NULL.
 * @param  rsp Where to store the response limit; may be NULL.
 */
void nx_uds_server_apdu_limits(const nx_uds_server_t *srv, uint32_t *req,
                               uint32_t *rsp);

/**
 * @brief  Change the session from the application's side.
 *
 * What a 0x10 handler calls to make the session it was asked for take effect, and
 * what an application calls to drop out of a session on its own account.
 *
 * Entering a session relocks the server, whichever session it is - including the
 * one already active. A level is unlocked for the session it was unlocked in, so
 * asking again for the session you are in is a way to relock, and it is not
 * treated as nothing to do.
 *
 * A 0x10 handler calls this from its CONFIRM phase rather than when it produces
 * the response, because the response belongs to the session that is ending: a
 * server that has already switched may no longer be able to send it, and a client
 * that never hears it would be left believing the old session still runs.
 *
 * @param  srv     Instance, must not be NULL.
 * @param  session The session to enter; an nx_uds_session_t value.
 *
 * @return NX_UDS_SERVER_OK on success; ERR_PARAM on a NULL server or a session
 *         of 0, which names no session.
 */
nx_uds_server_ret_t nx_uds_server_set_session(nx_uds_server_t *srv, uint8_t session);

/**
 * @brief  Set or clear the unlocked security level.
 *
 * What a 0x27 handler calls once it has judged a key. A level of 0 locks the
 * server again.
 *
 * @param  srv   Instance, must not be NULL.
 * @param  level The level now unlocked, as a service row's @c sec_level names it.
 *
 * @return NX_UDS_SERVER_OK on success; ERR_PARAM on a NULL server.
 */
nx_uds_server_ret_t nx_uds_server_set_sec_level(nx_uds_server_t *srv, uint8_t level);

/**
 * @brief  Restart the quiet timer that ends a non-default session.
 *
 * The server restarts it whenever it accepts a request, so a conversation that
 * keeps talking keeps its session. This is for an application that has its own
 * reason to consider the tester present.
 *
 * @param  srv Instance, must not be NULL.
 */
void nx_uds_server_touch(nx_uds_server_t *srv);

/**
 * @brief  Change where finished responses go.
 *
 * What a binding calls to make itself the server's output path, so that the two need
 * not be configured with each other's addresses in a particular order: the server is
 * initialised, then whatever carries it attaches. A configuration that names no
 * output at all is accepted for exactly this reason, and a server left without one
 * holds every answer it produces until this is called.
 *
 * @param  srv  Instance, must not be NULL.
 * @param  fn   The callback to hand responses to, must not be NULL.
 * @param  user Passed to it untouched.
 * @return NX_UDS_SERVER_OK, or ERR_PARAM where either pointer is NULL.
 */
nx_uds_server_ret_t nx_uds_server_set_output(nx_uds_server_t *srv,
                                            nx_uds_output_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_SERVER_H */
