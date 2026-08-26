/**
 * @file    nx_uds.h
 * @brief   ISO 14229 (UDS) vocabulary: service identifiers, response codes,
 *          sessions and the shapes a server and a client both speak in.
 *
 * Header-only and state-free. Everything here is a name for something the
 * standard defines, so a server, a client, a service handler and an application
 * all agree on one spelling of it without any of them depending on the others.
 *
 * The protocol is carried as a request A_PDU and a response A_PDU: a service
 * identifier followed by that service's data. What moves those bytes is not
 * described here and not known to this layer.
 */
#ifndef NX_UDS_H
#define NX_UDS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Service identifiers                                                   */
/* ===================================================================== */
/**
 * @brief Service identifiers, as they appear in the first byte of a request.
 *
 * A positive response echoes the request identifier with bit 6 set, which
 * NX_UDS_SID_TO_POS_RSP() computes. A negative response instead carries
 * NX_UDS_NEG_RSP_SID and names the service in its second byte.
 */
typedef enum {
    /* diagnostic and communication management */
    NX_UDS_SID_DIAGNOSTIC_SESSION_CONTROL = 0x10,
    NX_UDS_SID_ECU_RESET                  = 0x11,
    NX_UDS_SID_CLEAR_DIAGNOSTIC_INFO      = 0x14,
    NX_UDS_SID_READ_DTC_INFORMATION       = 0x19,
    NX_UDS_SID_SECURITY_ACCESS            = 0x27,
    NX_UDS_SID_COMMUNICATION_CONTROL      = 0x28,
    NX_UDS_SID_TESTER_PRESENT             = 0x3E,
    NX_UDS_SID_ACCESS_TIMING_PARAMETER    = 0x83,
    NX_UDS_SID_SECURED_DATA_TRANSMISSION  = 0x84,
    NX_UDS_SID_CONTROL_DTC_SETTING        = 0x85,
    NX_UDS_SID_RESPONSE_ON_EVENT          = 0x86,
    NX_UDS_SID_LINK_CONTROL               = 0x87,

    /* data transmission */
    NX_UDS_SID_READ_DATA_BY_IDENTIFIER    = 0x22,
    NX_UDS_SID_READ_MEMORY_BY_ADDRESS     = 0x23,
    NX_UDS_SID_READ_SCALING_DATA_BY_ID    = 0x24,
    NX_UDS_SID_READ_DATA_BY_PERIODIC_ID   = 0x2A,
    NX_UDS_SID_DYNAMICALLY_DEFINE_DATA_ID = 0x2C,
    NX_UDS_SID_WRITE_DATA_BY_IDENTIFIER   = 0x2E,
    NX_UDS_SID_WRITE_MEMORY_BY_ADDRESS    = 0x3D,

    /* stored data transmission and input/output control */
    NX_UDS_SID_INPUT_OUTPUT_CONTROL_BY_ID = 0x2F,

    /* routine */
    NX_UDS_SID_ROUTINE_CONTROL            = 0x31,

    /* upload and download */
    NX_UDS_SID_REQUEST_DOWNLOAD           = 0x34,
    NX_UDS_SID_REQUEST_UPLOAD             = 0x35,
    NX_UDS_SID_TRANSFER_DATA              = 0x36,
    NX_UDS_SID_REQUEST_TRANSFER_EXIT      = 0x37,
    NX_UDS_SID_REQUEST_FILE_TRANSFER      = 0x38
} nx_uds_sid_t;

/** @brief First byte of every negative response. */
#define NX_UDS_NEG_RSP_SID          0x7Fu

/** @brief The positive response identifier for a request identifier. */
#define NX_UDS_SID_TO_POS_RSP(sid)  ((uint8_t)((sid) | 0x40u))

/** @brief The request identifier a positive response identifier answers. */
#define NX_UDS_POS_RSP_TO_SID(rsp)  ((uint8_t)((rsp) & ~0x40u))

/** @brief Whether a first response byte is a negative response. */
#define NX_UDS_IS_NEG_RSP(b)        ((uint8_t)(b) == NX_UDS_NEG_RSP_SID)

/* ===================================================================== */
/* Negative response codes                                               */
/* ===================================================================== */
/**
 * @brief Negative response codes, as they appear in a negative response's
 *        third byte.
 *
 * Zero is not a response code, so it doubles as "no code" wherever one of these
 * is stored: NX_UDS_NRC_NONE names that use.
 */
typedef enum {
    NX_UDS_NRC_NONE                            = 0x00, /**< Not a response code: nothing to report */
    NX_UDS_NRC_GENERAL_REJECT                  = 0x10,
    NX_UDS_NRC_SERVICE_NOT_SUPPORTED           = 0x11,
    NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED      = 0x12,
    NX_UDS_NRC_INCORRECT_LENGTH_OR_FORMAT      = 0x13,
    NX_UDS_NRC_RESPONSE_TOO_LONG               = 0x14,
    NX_UDS_NRC_BUSY_REPEAT_REQUEST             = 0x21,
    NX_UDS_NRC_CONDITIONS_NOT_CORRECT          = 0x22,
    NX_UDS_NRC_REQUEST_SEQUENCE_ERROR          = 0x24,
    NX_UDS_NRC_NO_RESPONSE_FROM_SUBNET         = 0x25,
    NX_UDS_NRC_FAILURE_PREVENTS_EXECUTION      = 0x26,
    NX_UDS_NRC_REQUEST_OUT_OF_RANGE            = 0x31,
    NX_UDS_NRC_SECURITY_ACCESS_DENIED          = 0x33,
    NX_UDS_NRC_AUTHENTICATION_REQUIRED         = 0x34,
    NX_UDS_NRC_INVALID_KEY                     = 0x35,
    NX_UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS     = 0x36,
    NX_UDS_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED = 0x37,
    NX_UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPTED    = 0x70,
    NX_UDS_NRC_TRANSFER_DATA_SUSPENDED         = 0x71,
    NX_UDS_NRC_GENERAL_PROGRAMMING_FAILURE     = 0x72,
    NX_UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER    = 0x73,
    NX_UDS_NRC_RESPONSE_PENDING                = 0x78, /**< Received correctly, answer still coming */
    NX_UDS_NRC_SUB_FUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION = 0x7E,
    NX_UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION      = 0x7F
} nx_uds_nrc_t;

/** @brief Bytes a negative response occupies: the code, the SID, the reason. */
#define NX_UDS_NEG_RSP_LEN          3u

/* ===================================================================== */
/* Diagnostic sessions                                                   */
/* ===================================================================== */
/**
 * @brief Diagnostic session types, as they appear in a 0x10 request.
 *
 * The default session is the one a server starts in and returns to.
 */
typedef enum {
    NX_UDS_SESSION_DEFAULT       = 0x01,
    NX_UDS_SESSION_PROGRAMMING   = 0x02,
    NX_UDS_SESSION_EXTENDED      = 0x03,
    NX_UDS_SESSION_SAFETY_SYSTEM = 0x04
} nx_uds_session_t;

/**
 * @brief Highest session value a mask can name.
 *
 * The mask is 32 bits and a session's bit is its own value, so the values a mask
 * reaches stop here. The sessions ISO 14229 defines are all far below it; the
 * manufacturer and supplier ranges, which run to 0x7E, are not reachable and a
 * server is not asked to enter one.
 */
#define NX_UDS_SESSION_MAX 31u

/**
 * @brief  The set-membership bit for a session type.
 *
 * A service is available in a set of sessions, and the set is a bitmask so the
 * test is one AND. A session type is a small number, and the bit is that number,
 * so the default session is bit 1 and there is no bit 0.
 *
 * A value past what the mask reaches yields no bit rather than a shift the width
 * of the type, which has no defined result: it would land on whatever the machine
 * happened to produce, which on one target is the bit no mask sets and on another
 * is some service's own.
 */
#define NX_UDS_SESSION_BIT(s)     ((((uint32_t)(s)) <= NX_UDS_SESSION_MAX)          ? ((uint32_t)1u << (uint32_t)(s))          : 0u)

/**
 * @brief  Whether a session value is one a mask can name.
 *
 * @param  s Session value.
 */
#define NX_UDS_SESSION_IN_RANGE(s)     (((uint32_t)(s)) != 0u && ((uint32_t)(s)) <= NX_UDS_SESSION_MAX)

/** @brief Session sets a service table row commonly wants. */
#define NX_UDS_SESSION_MASK_DEFAULT     NX_UDS_SESSION_BIT(NX_UDS_SESSION_DEFAULT)
#define NX_UDS_SESSION_MASK_PROGRAMMING NX_UDS_SESSION_BIT(NX_UDS_SESSION_PROGRAMMING)
#define NX_UDS_SESSION_MASK_EXTENDED    NX_UDS_SESSION_BIT(NX_UDS_SESSION_EXTENDED)

/** @brief Every session, for a service that is always available. */
#define NX_UDS_SESSION_MASK_ALL         ((uint32_t)0xFFFFFFFEu)

/** @brief Every session but the default one. */
#define NX_UDS_SESSION_MASK_NON_DEFAULT (NX_UDS_SESSION_MASK_ALL & ~NX_UDS_SESSION_MASK_DEFAULT)

/* ===================================================================== */
/* Sub-function conventions                                              */
/* ===================================================================== */
/**
 * @brief  The bit a request sets to ask for no positive response.
 *
 * Services whose first data byte is a sub-function carry this request in its top
 * bit. A negative response is still sent when one is due, so the bit suppresses
 * only the positive answer.
 */
#define NX_UDS_SUPPRESS_POS_RSP_BIT 0x80u

/** @brief A sub-function byte with the suppression bit removed. */
#define NX_UDS_SUB_FUNCTION(b)      ((uint8_t)((b) & 0x7Fu))

/** @brief Whether a sub-function byte asks for the positive response to be suppressed. */
#define NX_UDS_SUPPRESSES_POS_RSP(b) (((uint8_t)(b) & NX_UDS_SUPPRESS_POS_RSP_BIT) != 0u)

/* ===================================================================== */
/* Reset types (0x11 ECUReset)                                           */
/* ===================================================================== */
/** @brief Reset types, as they appear in the sub-function of a 0x11 request. */
typedef enum {
    NX_UDS_RESET_HARD                     = 0x01,
    NX_UDS_RESET_KEY_OFF_ON               = 0x02,
    NX_UDS_RESET_SOFT                     = 0x03,
    NX_UDS_RESET_ENABLE_RAPID_POWER_SHUT_DOWN  = 0x04,
    NX_UDS_RESET_DISABLE_RAPID_POWER_SHUT_DOWN = 0x05
} nx_uds_reset_type_t;

/* ===================================================================== */
/* The handler contract                                                  */
/* ===================================================================== */
/**
 * @brief Why a handler is being called.
 *
 * A transaction calls its handler more than once, and the phase says which point
 * of the transaction the call is at. Only REQUEST and RESUME may produce a
 * response; the rest report what has happened and their return value is ignored.
 */
typedef enum {
    NX_UDS_PHASE_REQUEST = 0, /**< The request arrived; produce an answer */
    NX_UDS_PHASE_RESUME,      /**< The last call returned PENDING; continue */
    NX_UDS_PHASE_RESPONSE,    /**< The answer is about to be handed to the link */
    NX_UDS_PHASE_CONFIRM,     /**< The answer reached the link successfully */
    NX_UDS_PHASE_LINK_ERROR,  /**< The answer did not reach the link */
    NX_UDS_PHASE_SILENCE,     /**< No answer will be sent, for any of the reasons
                               one is withheld: the request asked for its positive
                               response to be suppressed, it was functionally
                               addressed and earned one of the refusals that go
                               unsent, or the handler produced nothing. A handler
                               that acted on the request learns here that it is
                               over, since no confirmation can follow an answer
                               that was never sent. */
    NX_UDS_PHASE_ABORT        /**< The transaction ended without completing */
} nx_uds_phase_t;

/**
 * @brief What a handler has done with the request.
 *
 * Meaningful from the REQUEST and RESUME phases; ignored from the others, which
 * report rather than ask.
 */
typedef enum {
    NX_UDS_DISPOSITION_DONE = 0,  /**< A positive response is in @c out, @c out_len long */
    NX_UDS_DISPOSITION_PENDING,   /**< Not finished; call me again next process() */
    NX_UDS_DISPOSITION_NEGATIVE,  /**< Refused: @c nrc names why, and @c out is unused */
    NX_UDS_DISPOSITION_NO_RESPONSE/**< Nothing to answer, and nothing went wrong */
} nx_uds_disposition_t;

/**
 * @brief One transaction, as its handler sees it.
 *
 * The request is read in place and the response is written into a buffer the
 * transaction owns, so a handler copies nothing it does not have to.
 *
 * @c req points into the message that arrived and stays valid for as long as the
 * transaction runs, which is to say until the handler returns something other
 * than PENDING.
 */
typedef struct nx_uds_ctx {
    /* ---- what arrived (read-only to the handler) ---- */
    nx_uds_phase_t  phase;    /**< Why this call is happening */
    const uint8_t  *req;      /**< The request, service identifier included */
    uint32_t        req_len;  /**< Its length in bytes, at least 1 */
    uint8_t         sid;      /**< Its service identifier, i.e. @c req[0] */
    uint8_t         sub;      /**< Its sub-function with the suppression bit
                               removed, or 0 when the service has none */
    bool            has_sub;  /**< Whether the service is one with a sub-function */
    bool            suppress_pos; /**< The request asked for no positive response */
    uint8_t         ta_type;  /**< How it was addressed; an nx_tp_ta_type_t value */
    uint8_t         link;     /**< Which connection it came in on */

    /* ---- the session it arrived in (read-only to the handler) ---- */
    uint8_t         session;  /**< Active session; an nx_uds_session_t value */
    uint8_t         sec_level;/**< Unlocked security level, 0 when locked */

    /* ---- what the handler produces ---- */
    uint8_t        *out;      /**< Response buffer, response identifier included.
                               The layer has already written the positive response
                               identifier into @c out[0] and set @c out_len to 1,
                               so a handler appends its data and adds to the
                               length. */
    uint32_t        out_cap;  /**< Bytes @c out holds; already the smaller of the
                               buffer and what the configuration allows */
    uint32_t        out_len;  /**< Bytes of @c out that are filled */
    uint8_t         nrc;      /**< Reason to refuse, when returning NEGATIVE */
    uint8_t         result;   /**< On LINK_ERROR, the transport's nx_tp_result_t */

    /* ---- the handler's own place to keep things across phases ---- */
    void           *state;    /**< Untouched by the layer; a handler that needs to
                               remember something between two calls of one
                               transaction puts it here */
} nx_uds_ctx_t;

/**
 * @brief One service's implementation.
 *
 * @param  ctx  The transaction. Read the request, write the response.
 * @param  user The service table row's @c user pointer, unexamined by the layer.
 *
 * @return What was done with the request; see nx_uds_disposition_t.
 */
typedef nx_uds_disposition_t (*nx_uds_handler_fn)(nx_uds_ctx_t *ctx, void *user);

/* ===================================================================== */
/* The service table                                                     */
/* ===================================================================== */
/**
 * @brief Properties of a service that change how the layer treats it.
 */
typedef enum {
    /** The service's first data byte is a sub-function, so the layer reads the
     *  suppression bit out of it and checks it against @c subs.
     *
     *  Set this only for a service whose first data byte really is a
     *  sub-function. Where it is something else, the top bit of that byte would
     *  be read as a request for silence: a service carrying a counter there
     *  would be answered normally for the first 128 values and silently for the
     *  rest. */
    NX_UDS_SVC_HAS_SUB_FUNCTION = 0x01u,
    /** The service is answered even when it arrived functionally addressed.
     *  Left clear, a functionally addressed request is handled and its positive
     *  response suppressed. */
    NX_UDS_SVC_ANSWER_FUNCTIONAL = 0x04u
} nx_uds_svc_flag_t;

/**
 * @brief One row of the service table: a service the server implements.
 *
 * The table is the whole of what a server supports. A request whose identifier
 * is in no row is refused with 0x11, so the set of services is a property of the
 * application's table rather than of this library.
 *
 * Lengths are of the request as it arrives, service identifier included, so the
 * shortest possible request is 1 byte long.
 */
typedef struct {
    uint8_t  sid;            /**< The service identifier this row implements */
    uint8_t  flags;          /**< Any of nx_uds_svc_flag_t, ORed together */
    uint8_t  sec_level;      /**< Security level the request needs; 0 = none */
    uint8_t  subs_count;     /**< How many entries @c subs has; 0 = accept any */
    const uint8_t *subs;     /**< Sub-functions this service accepts, without the
                              suppression bit. NULL accepts any, which is what a
                              service whose sub-function is only meaningful
                              together with the data after it wants. */
    const uint32_t *sub_session_masks;
                             /**< Sessions each entry of @c subs is available in,
                              one mask per entry, in the same order. NULL makes
                              every sub-function follow the row's own
                              @c session_mask. This is what lets a service be
                              available in a session while one of its
                              sub-functions is not. */
    uint32_t session_mask;   /**< Sessions the service is available in; see
                              NX_UDS_SESSION_BIT. A mask of 0 names no session at
                              all, which would make the service unreachable, so
                              init refuses a row carrying one rather than leave a
                              zero-initialized row silently dead. */
    uint16_t min_len;        /**< Shortest request this service accepts */
    uint16_t max_len;        /**< Longest request this service accepts; 0 = no limit */
    uint32_t p4_us;          /**< Longest this service may take in total, in us;
                              0 = use the server's own value */
    uint8_t  max_pending;    /**< How many times this service may say its answer is
                              still coming before the transaction is given up on.
                              0 = use the server's own value. A service that must
                              answer within the first window sets this to 1 and its
                              @c p4_us to the server's P2. */
    uint8_t  p4_nrc;         /**< What to answer when it takes longer than that;
                              0 = the server's default */
    nx_uds_handler_fn handler; /**< What implements the service. Required. */
    void    *user;           /**< Passed to @c handler untouched */
} nx_uds_service_t;

/* ===================================================================== */
/* 0x10 timing scales                                                    */
/* ===================================================================== */
/**
 * @brief  How the session parameters are scaled in a 0x10 positive response.
 *
 * The response reports the two timing values the client is to use, and they are
 * not in the same unit: P2 is in milliseconds and P2* in tens of milliseconds,
 * both as 16-bit big-endian values.
 */
#define NX_UDS_P2_RESOLUTION_US            1000u
#define NX_UDS_P2_STAR_RESOLUTION_US       10000u

#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_H */
