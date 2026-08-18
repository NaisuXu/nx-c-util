/**
 * @file    nx_tp_sdu.h
 * @brief   Transport-layer service data unit: the object a transport exchanges
 *          with the layer above it.
 *
 * A diagnostic transport delivers a complete message plus the few facts about
 * how it travelled that the layer above must know to answer correctly: whether
 * it was addressed to one receiver or to many, which link it belongs to, and -
 * for a transmission - whether it actually completed. Those facts are the same
 * whatever carries the message, so they are described once here and every
 * transport fills the same structure.
 *
 * The payload is the flexible-array part of an nx_ref_msg, so a message is one
 * allocation from a pool and travels by pointer through the caller's queues.
 *
 * Direction is carried by the queue a message arrives on, not by a field: a
 * transport reads send requests from one queue and publishes what it received
 * and what it finished sending to another. @c kind then separates the two things
 * that share the outbound queue - a message that arrived, and the outcome of a
 * message that was sent.
 */
#ifndef NX_TP_SDU_H
#define NX_TP_SDU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a message addresses one receiver or every receiver on the link.
 */
typedef enum {
    NX_TP_TA_PHYSICAL   = 0,   /**< 1-to-1: addressed to a single receiver */
    NX_TP_TA_FUNCTIONAL = 1    /**< 1-to-n: addressed to every receiver at once */
} nx_tp_ta_type_t;

/**
 * @brief What a service data unit reports.
 */
typedef enum {
    NX_TP_SDU_INDICATION = 0,  /**< A message arrived; @c data holds it */
    NX_TP_SDU_CONFIRM    = 1   /**< A transmission finished; @c result holds its outcome */
} nx_tp_sdu_kind_t;

/**
 * @brief Outcome of a transport operation.
 *
 * @note  The order is the priority order the transport layer resolves these in,
 *        so a receiver may compare values. Do not rearrange the enumerators.
 */
typedef enum {
    NX_TP_N_OK = 0,          /**< Completed as requested */
    NX_TP_N_TIMEOUT_A,       /**< A frame could not be handed to, or taken from, the link in time */
    NX_TP_N_TIMEOUT_BS,      /**< The peer's flow control did not arrive in time */
    NX_TP_N_TIMEOUT_CR,      /**< The peer's next consecutive frame did not arrive in time */
    NX_TP_N_WRONG_SN,        /**< A consecutive frame carried an unexpected sequence number */
    NX_TP_N_INVALID_FS,      /**< Flow control carried a flow status that is not defined */
    NX_TP_N_UNEXP_PDU,       /**< A protocol data unit arrived that the current state does not allow */
    NX_TP_N_WFT_OVRN,        /**< The peer asked to wait more times than are tolerated */
    NX_TP_N_BUFFER_OVFLW,    /**< The message does not fit the space available to receive it */
    NX_TP_N_ERROR            /**< Failed for a reason none of the above names */
} nx_tp_result_t;

/**
 * @brief One message exchanged between a transport and the layer above it.
 *
 * Sits in the flexible-array part of an nx_ref_msg: one allocation carries the
 * header and the payload together.
 *
 * A zero-initialized instance describes a physically addressed indication that
 * succeeded, so only the fields that depart from that need setting.
 */
typedef struct {
    uint32_t len;      /**< Payload length in bytes. Bounded by the configuration
                        and the memory behind it, never by this field's width. */
    uint8_t  link;     /**< Which connection this belongs to, as numbered by the
                        application. A transport copies its configured value into
                        everything it publishes and never interprets it. */
    uint8_t  kind;     /**< What this reports; see nx_tp_sdu_kind_t. */
    uint8_t  ta_type;  /**< How it was addressed; see nx_tp_ta_type_t. */
    uint8_t  result;   /**< Outcome; see nx_tp_result_t. Meaningful on a confirm,
                        and NX_TP_N_OK on an indication. */
    uint8_t  data[];   /**< Payload, @c len bytes. */
} nx_tp_sdu_t;

#ifdef __cplusplus
}
#endif

#endif /* NX_TP_SDU_H */
