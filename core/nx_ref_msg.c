/**
 * @file    nx_ref_msg.c
 * @brief   Implementation of nx_ref_msg: reference-counted messages with multi-queue publish.
 */
#include "core/nx_ref_msg.h"

nx_queue_ret_t nx_ref_msg_queue_init(nx_queue_t *q,
                                     void       *buffer,
                                     size_t      capacity)
{
    /* A reference-message queue's element is a "pointer to the message object";
     * the on-full policy is fixed to REJECT so OVERWRITE cannot silently drop an
     * enqueued message and leak its reference. */
    return nx_queue_init(q, buffer, sizeof(nx_ref_msg_t *), capacity,
                         NX_QUEUE_ON_FULL_REJECT);
}

nx_ref_msg_t *nx_ref_msg_alloc(nx_tiered_mem_pool_t *pool, size_t len)
{
    if (pool == NULL || len == 0u) {
        return NULL;
    }

    /* Single allocation: header + len bytes of data. data is a flexible array
     * right after the header. */
    nx_ref_msg_t *msg =
        (nx_ref_msg_t *)nx_tiered_mem_pool_alloc(pool, sizeof(nx_ref_msg_t) + len);
    if (msg == NULL) {
        return NULL;
    }

    msg->pool     = pool;
    msg->len      = len;
    msg->refcount = 1u;   /* producer reference */

    return msg;
}

nx_ref_msg_ret_t nx_ref_msg_publish(nx_ref_msg_t *msg, nx_queue_t *q)
{
    if (msg == NULL || q == NULL) {
        return NX_REF_MSG_ERR_PARAM;
    }

    /* The enqueued element is the "message pointer". */
    nx_queue_ret_t r = nx_queue_push(q, &msg);
    if (r == NX_QUEUE_ERR_FULL) {
        return NX_REF_MSG_ERR_FULL;
    }
    if (r != NX_QUEUE_OK) {
        return NX_REF_MSG_ERR_PARAM;
    }

    /* Increment the reference count only after a successful enqueue. */
    msg->refcount++;
    return NX_REF_MSG_OK;
}

nx_ref_msg_ret_t nx_ref_msg_release(nx_ref_msg_t *msg)
{
    if (msg == NULL) {
        return NX_REF_MSG_ERR_PARAM;
    }
    if (msg->refcount == 0u) {
        return NX_REF_MSG_ERR_STATE;   /* double release */
    }

    msg->refcount--;
    if (msg->refcount == 0u) {
        /* Return the whole block (header + data) to the pool in one call. */
        (void)nx_tiered_mem_pool_free(msg->pool, msg);
    }

    return NX_REF_MSG_OK;
}

nx_ref_msg_ret_t nx_ref_msg_publish_multi(nx_ref_msg_t      *msg,
                                          nx_queue_t *const *queues,
                                          size_t            *out_delivered,
                                          size_t            *out_first_failed)
{
    if (msg == NULL || queues == NULL) {
        return NX_REF_MSG_ERR_PARAM;
    }

    size_t total       = 0u;
    size_t delivered   = 0u;
    size_t first_fail  = 0u;   /* set to first failing index; stays == total if none */
    bool   have_fail   = false;

    /* NULL-terminated: stop at the first NULL entry. Best-effort: keep going
     * after a full queue so the others still receive the message. */
    for (size_t i = 0u; queues[i] != NULL; i++) {
        total++;
        if (nx_ref_msg_publish(msg, queues[i]) == NX_REF_MSG_OK) {
            delivered++;
        } else if (!have_fail) {
            /* Record only the first failure; on-full queues are REJECT, so a
             * non-OK result here means this queue genuinely did not accept it. */
            first_fail = i;
            have_fail  = true;
        }
    }

    if (out_delivered != NULL) {
        *out_delivered = delivered;
    }
    if (out_first_failed != NULL) {
        /* No failure -> one past the last valid index (i.e. the queue count). */
        *out_first_failed = have_fail ? first_fail : total;
    }

    if (delivered == total) {
        return NX_REF_MSG_OK;              /* all accepted (incl. empty list) */
    }
    return (delivered == 0u) ? NX_REF_MSG_ERR_FULL   /* nobody took it */
                             : NX_REF_MSG_PARTIAL;    /* some queues were full */
}

