/**
 * @file    nx_uds_svc_std.h
 * @brief   Handlers for the diagnostic services every server needs.
 *
 * Three services that a diagnostic server is expected to answer whatever else it
 * implements: 0x10 DiagnosticSessionControl, 0x11 ECUReset and 0x3E
 * TesterPresent. Each is an ordinary handler occupying an ordinary service table
 * row, wired into a table alongside the application's own services and reached
 * the same way.
 *
 * Each takes its own configuration struct through the row's @c user pointer,
 * which is where the instance it acts on and the application behaviour it needs
 * are named. Nothing here keeps state of its own.
 *
 * What each row must declare is written on the handler, and a row that declares
 * something else is not corrected: a length bound the service does not expect, or
 * a sub-function list naming what the product cannot do, produces a server that
 * answers wrongly rather than one that refuses to start.
 */
#ifndef NX_UDS_SVC_STD_H
#define NX_UDS_SVC_STD_H

#include <stdbool.h>
#include <stdint.h>

#include "nx_uds.h"
#include "nx_uds_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* 0x10 DiagnosticSessionControl                                         */
/* ===================================================================== */

/**
 * @brief Whether the session asked for can be entered right now.
 *
 * Called once, before anything is answered, for a request that the table has
 * already found acceptable. Returning false refuses it with
 * @c NX_UDS_NRC_CONDITIONS_NOT_CORRECT unless @c nrc is written with a reason of
 * the application's own.
 *
 * This is where a product refuses to enter programming while it is driving. A
 * session the product never enters belongs in the row's @c subs instead, where it
 * is refused as unsupported rather than as momentarily impossible.
 *
 * @param  user    The @c user field of nx_uds_svc_std_session_cfg_t.
 * @param  from    The session active now; an nx_uds_session_t value.
 * @param  to      The session asked for; an nx_uds_session_t value.
 * @param  nrc     Optional reason to refuse with, when refusing.
 * @return true to allow the session to be entered.
 */
typedef bool (*nx_uds_svc_std_session_allow_fn)(void *user, uint8_t from, uint8_t to,
                                           uint8_t *nrc);

/**
 * @brief Configuration for the 0x10 handler, held in its row's @c user field.
 *
 * @note  The row must declare @c min_len 2, @c max_len 2,
 *        @c NX_UDS_SVC_HAS_SUB_FUNCTION, a @c subs list of the sessions the
 *        product enters, and a @c session_mask reaching every session, since a
 *        server that cannot be asked to leave a session it is in can only be
 *        power-cycled out of it.
 */
typedef struct {
    nx_uds_server_t *srv;      /**< The instance whose session is being changed.
                                Required. */
    nx_uds_svc_std_session_allow_fn allow_fn;
                               /**< Consulted before the request is accepted; NULL
                                accepts every session the row lists. */
    void            *user;     /**< Passed to @c allow_fn untouched. */
} nx_uds_svc_std_session_cfg_t;

/**
 * @brief  0x10 DiagnosticSessionControl.
 *
 * Answers with the session echoed back and the two response windows the server
 * holds itself to, then enters the session once that answer has reached the
 * client. Entering relocks security, including where the session entered is the
 * one already active: a request to enter a session is answered from a server in
 * that session with nothing unlocked, so a client cannot keep a level across a
 * request that names the session it already has.
 *
 * The windows published are the ones the server enforces, read from it rather
 * than configured here. They are announced before the session takes effect, and
 * describe the server as it will be once it does.
 *
 * A request whose positive response is suppressed still enters the session, at
 * the point the answer would have been sent. A request that earns a refusal never
 * does, whether that refusal was sent or withheld.
 */
nx_uds_disposition_t nx_uds_svc_std_session_control(nx_uds_ctx_t *ctx, void *user);

/* ===================================================================== */
/* 0x11 ECUReset                                                         */
/* ===================================================================== */

/**
 * @brief Carry out the reset that was asked for.
 *
 * Called once the answer has left, or once it is settled that no answer will be
 * sent. Whatever this does happens after the client has been told the request was
 * accepted.
 *
 * Two of the reset types perform no reset: 0x04 and 0x05 record how the next
 * power-down should behave, and 0x02 arms behaviour for the next power cycle. What
 * each of them means is the product's, and this callback is where that lives.
 *
 * Any settling a reset needs before it takes effect belongs here too. Reaching
 * this point means the answer was handed over and accepted, which is not the same
 * as the client having read it.
 *
 * @param  user       The @c user field of nx_uds_svc_std_reset_cfg_t.
 * @param  reset_type What was asked for; an nx_uds_reset_type_t value.
 */
typedef void (*nx_uds_svc_std_reset_do_fn)(void *user, uint8_t reset_type);

/**
 * @brief Whether the reset asked for can be carried out right now.
 *
 * Returning false refuses the request with
 * @c NX_UDS_NRC_CONDITIONS_NOT_CORRECT unless @c nrc names another reason.
 *
 * @param  user       The @c user field of nx_uds_svc_std_reset_cfg_t.
 * @param  reset_type What was asked for; an nx_uds_reset_type_t value.
 * @param  nrc        Optional reason to refuse with, when refusing.
 * @return true to allow the reset.
 */
typedef bool (*nx_uds_svc_std_reset_allow_fn)(void *user, uint8_t reset_type,
                                          uint8_t *nrc);

/**
 * @brief Configuration for the 0x11 handler, held in its row's @c user field.
 *
 * @note  The row must declare @c min_len 2, @c max_len 2,
 *        @c NX_UDS_SVC_HAS_SUB_FUNCTION, and a @c subs list naming only the reset
 *        types the product performs. A type listed but not implemented is
 *        answered positively and then does nothing.
 */
typedef struct {
    nx_uds_svc_std_reset_do_fn    do_fn;    /**< Performs the reset. Required. */
    nx_uds_svc_std_reset_allow_fn allow_fn; /**< Consulted first; NULL allows every
                                         type the row lists. */
    void    *user;              /**< Passed to both callbacks untouched. */
    uint8_t  power_down_time;   /**< Seconds the client must wait for the power to
                                 be down, answered only to 0x04. 0xFF says the
                                 number is not available. */
} nx_uds_svc_std_reset_cfg_t;

/**
 * @brief  0x11 ECUReset.
 *
 * Answers with the reset type echoed back, and 0x04 additionally with the time
 * the power stays down, then performs the reset once that answer has reached the
 * client. A reset carried out before its answer is sent looks to a client like a
 * server that rebooted on its own.
 *
 * A request whose positive response is suppressed is still carried out, at the
 * point the answer would have been sent. A request whose answer never reached the
 * link is not: the client never learned the reset was accepted, so a reset would
 * be a reboot it did not ask for.
 */
nx_uds_disposition_t nx_uds_svc_std_ecu_reset(nx_uds_ctx_t *ctx, void *user);

/* ===================================================================== */
/* 0x3E TesterPresent                                                    */
/* ===================================================================== */

/** @brief The only sub-function 0x3E has. */
#define NX_UDS_SVC_STD_TESTER_PRESENT_SUB 0x00u

/**
 * @brief  0x3E TesterPresent.
 *
 * Answers with the sub-function echoed back, and does nothing else. What the
 * service is for is the arrival of the request: accepting one restarts the quiet
 * timer, which the server does for every request it accepts, so a client that has
 * nothing to ask sends this to keep the session.
 *
 * Takes no configuration; its row's @c user field is unused and may be NULL.
 *
 * @note  The row must declare @c min_len 2, @c max_len 2,
 *        @c NX_UDS_SVC_HAS_SUB_FUNCTION, a @c subs list of exactly
 *        @c NX_UDS_SVC_STD_TESTER_PRESENT_SUB, and a @c session_mask reaching every
 *        session. It is also the request a client is most likely to broadcast, so
 *        the row wants @c NX_UDS_SVC_ANSWER_FUNCTIONAL left clear unless every
 *        server on the link should answer it at once.
 */
nx_uds_disposition_t nx_uds_svc_std_tester_present(nx_uds_ctx_t *ctx, void *user);

/* PLACEHOLDER_BODY */

#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_SVC_STD_H */
