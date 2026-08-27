/**
 * @file    nx_uds_svc_sec.h
 * @brief   0x27 SecurityAccess: the seed/key exchange that unlocks a level.
 *
 * A handler occupying an ordinary service table row, plus the state that exchange
 * needs: which seed was issued and to whom, how many keys have been wrong, and how
 * long a client that has run out of attempts must wait.
 *
 * The algorithm is not here. The application produces the seed and judges the key,
 * through two callbacks, and this module never sees a secret or invents a random
 * number. What it owns is the sequence: that a key is only meaningful after a seed,
 * that a seed is spent once a key has been judged against it, and that a client
 * cannot go on guessing.
 *
 * The exchange is a pair of sub-functions per level, an odd one asking for the seed
 * and the even one after it presenting the key: level 1 is 0x01 and 0x02, level 2
 * is 0x03 and 0x04, and so on. Levels are numbered from 1 and are what a service
 * row's @c sec_level names; the sub-function values are this module's business.
 *
 * The attempt counter and the waiting period outlast a session change, so a client
 * that has run out cannot start again by asking for a session. They outlast a
 * relock for the same reason.
 */
#ifndef NX_UDS_SVC_SEC_H
#define NX_UDS_SVC_SEC_H

#include <stdbool.h>
#include <stdint.h>

#include "nx_uds.h"
#include "nx_uds_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Attempts allowed before the waiting period starts, absent a choice. */
#define NX_UDS_SVC_SEC_DEFAULT_ATTEMPTS 3u

/** @brief How long that period lasts, absent a choice: ten seconds. */
#define NX_UDS_SVC_SEC_DEFAULT_DELAY_US 10000000u

/** @brief Highest level the sub-function pairs reach. */
#define NX_UDS_SVC_SEC_MAX_LEVEL 32u

/**
 * @brief  The sub-function that asks for a level's seed.
 * @param  level Level, 1..NX_UDS_SVC_SEC_MAX_LEVEL.
 */
#define NX_UDS_SVC_SEC_SEED_SUB(level) ((uint8_t)((level) * 2u - 1u))

/**
 * @brief  The sub-function that presents a level's key.
 * @param  level Level, 1..NX_UDS_SVC_SEC_MAX_LEVEL.
 */
#define NX_UDS_SVC_SEC_KEY_SUB(level)  ((uint8_t)((level) * 2u))

/**
 * @brief Produce the seed a level's key will be computed from.
 *
 * Called only for a level that is locked and reachable, so a callback need not
 * check either. What the seed is made of is the application's: it owns whatever
 * randomness it has, and whatever it must remember to judge the key later, though
 * remembering is usually unnecessary since the seed is handed back to
 * nx_uds_svc_sec_verify_fn.
 *
 * Returning false refuses to issue one, answered as
 * @c NX_UDS_NRC_CONDITIONS_NOT_CORRECT. A refusal is not a wrong key and is not
 * counted as an attempt.
 *
 * @param  user      The @c user field of nx_uds_svc_sec_cfg_t.
 * @param  level     Level being asked about, 1..NX_UDS_SVC_SEC_MAX_LEVEL.
 * @param  record    Bytes the request carried after the sub-function, or NULL.
 * @param  record_len How many, 0 when there were none.
 * @param  seed      Where to write the seed.
 * @param  seed_cap  Room there; at least the level's declared seed length.
 * @param  seed_len  Where to report how many bytes were written.
 * @return true when a seed was produced.
 */
typedef bool (*nx_uds_svc_sec_seed_fn)(void *user, uint8_t level,
                                   const uint8_t *record, uint32_t record_len,
                                   uint8_t *seed, uint32_t seed_cap,
                                   uint32_t *seed_len);

/**
 * @brief Judge a key against the seed that was issued.
 *
 * The whole of the algorithm, and the only place it exists. The seed handed here is
 * the one this module gave out, so a callback keeps no record of its own.
 *
 * Asked to judge, not to compute: an application can check a key without ever
 * writing the expected one into memory, and a scheme whose check is not a byte
 * comparison fits the same callback.
 *
 * Returning false is a wrong key, and is the one thing counted as an attempt.
 *
 * @param  user     The @c user field of nx_uds_svc_sec_cfg_t.
 * @param  level    Level being unlocked, 1..NX_UDS_SVC_SEC_MAX_LEVEL.
 * @param  seed     The seed that was issued for it.
 * @param  seed_len Its length.
 * @param  key      The key the request carried.
 * @param  key_len  Its length, the level's declared key length.
 * @return true when the key is right for that seed at that level.
 */
typedef bool (*nx_uds_svc_sec_verify_fn)(void *user, uint8_t level,
                                     const uint8_t *seed, uint32_t seed_len,
                                     const uint8_t *key, uint32_t key_len);

/**
 * @brief Act on a level having been unlocked.
 *
 * Called after the unlock is recorded and its answer assembled, which is where
 * whatever unlocking permits gets started. Kept apart from
 * nx_uds_svc_sec_verify_fn so that judging a key stays a question that can be asked
 * and answered with nothing happening.
 *
 * @param  user  The @c user field of nx_uds_svc_sec_cfg_t.
 * @param  level Level now unlocked.
 */
typedef void (*nx_uds_svc_sec_granted_fn)(void *user, uint8_t level);

/**
 * @brief One level the product offers, and the byte counts its exchange uses.
 *
 * A level absent from the list does not exist, whatever sub-function names it.
 */
typedef struct {
    uint8_t  level;    /**< Level number, 1..NX_UDS_SVC_SEC_MAX_LEVEL, as a service
                        row's @c sec_level names it. */
    uint32_t seed_len; /**< Bytes of seed, at least 1. Fixed for the level: the
                        answer is this long whether the seed was computed or the
                        level was already unlocked. */
    uint32_t key_len;  /**< Bytes of key the level's request must carry, exactly.
                        At least 1. */
} nx_uds_svc_sec_level_t;

/**
 * @brief Configuration of the exchange.
 */
typedef struct {
    nx_uds_server_t *srv;      /**< The instance whose level is being unlocked.
                                Required. */

    const nx_uds_svc_sec_level_t *levels;
                               /**< The levels the product offers. Caller-owned and
                                must outlive the handle, which keeps the pointer
                                rather than copying the list. Required. */
    uint8_t levels_count;      /**< How many, at least 1. */

    nx_uds_svc_sec_seed_fn    seed_fn;   /**< Produces a seed. Required. */
    nx_uds_svc_sec_verify_fn  verify_fn; /**< Judges a key. Required. */
    nx_uds_svc_sec_granted_fn granted_fn;/**< Acts on an unlock; may be NULL. */
    void                 *user;      /**< Passed to all three untouched. */

    uint8_t  *seed_buf;        /**< Where the issued seed is kept until its key is
                                judged. Caller-owned. Required, and at least as
                                long as the longest declared seed. */
    uint32_t  seed_buf_size;   /**< Its length in bytes. */

    uint8_t   max_attempts;    /**< Wrong keys allowed before the waiting period
                                starts. 0 = NX_UDS_SVC_SEC_DEFAULT_ATTEMPTS. Counted
                                across every level together: a client working
                                through levels one at a time is one client. */
    uint32_t  delay_us;        /**< How long that period lasts. 0 =
                                NX_UDS_SVC_SEC_DEFAULT_DELAY_US. */
} nx_uds_svc_sec_cfg_t;

/**
 * @brief The exchange, as far as it has got.
 *
 * Declare one in static storage beside the server and hand it to nx_uds_svc_sec_init.
 * Its address is what the 0x27 row carries as @c user.
 *
 * @note  @c run is internal state; treat the whole object as opaque once passed to
 *        nx_uds_svc_sec_init.
 */
typedef struct {
    nx_uds_svc_sec_cfg_t cfg;         /**< Copied configuration */
    struct {
        uint8_t  seed_level;      /**< Level a seed is outstanding for, 0 for none */
        uint32_t seed_len;        /**< Its length */
        uint8_t  attempts;        /**< Wrong keys so far */
        bool     waiting;         /**< Whether the waiting period is running */
        uint32_t wait_until;      /**< nx_uds_server_now() it ends at */
    } run;                        /**< Internal runtime state */
} nx_uds_svc_sec_t;

/**
 * @brief  Set up the exchange.
 *
 * @param  sec Handle to initialise, must not be NULL.
 * @param  cfg Configuration, copied. Its @c levels list and @c seed_buf must
 *             outlive the handle; the struct itself need not.
 * @return true on success; false where anything required is missing, where a
 *         declared level is outside 1..NX_UDS_SVC_SEC_MAX_LEVEL, where a declared seed
 *         is longer than @c seed_buf holds, or where a level appears twice.
 */
bool nx_uds_svc_sec_init(nx_uds_svc_sec_t *sec, const nx_uds_svc_sec_cfg_t *cfg);

/**
 * @brief  0x27 SecurityAccess.
 *
 * The row's @c user is the nx_uds_svc_sec_t. An odd sub-function is answered with the
 * level's seed and the even one after it judges the key, unlocking the level when
 * it is right.
 *
 * A level already unlocked is answered with a seed of zeros, telling the client
 * there is nothing to compute. Presenting a key against it is a request out of
 * sequence rather than a wrong key, and is not counted as an attempt.
 *
 * A wrong key is counted, and the count reaching what is allowed starts a waiting
 * period in which every request of this service is refused without a callback being
 * consulted. The count and the period outlast both a session change and a relock, so
 * neither is a way around them.
 *
 * @note  The row must declare @c min_len 2, @c NX_UDS_SVC_HAS_SUB_FUNCTION, a
 *        @c subs list holding both sub-functions of every level offered, a
 *        @c max_len covering the longest key or seed-request record, and
 *        @c sec_level 0 - the service that unlocks a level cannot itself be behind
 *        one. Conventionally the @c session_mask excludes the default session, which
 *        is how a request to unlock from it is refused as out of session.
 */
nx_uds_disposition_t nx_uds_svc_security_access(nx_uds_ctx_t *ctx, void *user);

/**
 * @brief  Read the lockout so it can be kept somewhere that survives a restart.
 *
 * The waiting period is expected to hold across a power cycle; a product that
 * restarts out of it lets a client guess as fast as it can reboot the server. This
 * module cannot store anything itself, so it reports what to store.
 *
 * @param  sec       Handle, must not be NULL.
 * @param  attempts  Where to store the wrong keys counted; may be NULL.
 * @param  waiting   Where to store whether a period is running; may be NULL.
 * @param  remaining Where to store how much of it is left, in microseconds; may be
 *                   NULL. 0 when none is running.
 */
void nx_uds_svc_sec_get_lockout(const nx_uds_svc_sec_t *sec, uint8_t *attempts,
                            bool *waiting, uint32_t *remaining);

/**
 * @brief  Restore a lockout that was kept across a restart.
 *
 * @param  sec       Handle, must not be NULL.
 * @param  attempts  Wrong keys counted before.
 * @param  waiting   Whether a period was running.
 * @param  remaining How much of it was left, in microseconds. Measured from now.
 * @return true on success; false where @c sec is NULL.
 */
bool nx_uds_svc_sec_set_lockout(nx_uds_svc_sec_t *sec, uint8_t attempts, bool waiting,
                            uint32_t remaining);

#ifdef __cplusplus
}
#endif

#endif /* NX_UDS_SVC_SEC_H */
