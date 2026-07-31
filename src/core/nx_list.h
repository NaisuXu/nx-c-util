/**
 * @file    nx_list.h
 * @brief   Intrusive doubly-linked circular list, in pure C.
 *
 * An intrusive list embeds the link node (nx_list_t) directly into the user's
 * structure, rather than wrapping the user data in a separate allocated node.
 * This gives zero-allocation list operations: the node lives wherever the
 * containing structure lives.
 *
 * The list is doubly-linked (each node has prev/next) and circular: the head
 * node (sentinel) forms a ring with itself when empty, and the last node's next
 * points back to the head. This makes insertion and deletion symmetric — no
 * special cases for head/tail.
 *
 * Header-only: all operations are static inline, so nothing needs to be compiled
 * or linked.
 */
#ifndef NX_LIST_H
#define NX_LIST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Intrusive list node.
 *
 * Embed this into your structure:
 * @code
 *   typedef struct {
 *       int payload;
 *       nx_list_t link;
 *   } my_item_t;
 * @endcode
 */
typedef struct nx_list {
    struct nx_list *prev;
    struct nx_list *next;
} nx_list_t;

/**
 * @brief  Initialize a list head (empty list).
 *
 * After this, the head forms a ring with itself: head->prev = head->next = head.
 *
 * @param  head List head, must not be NULL.
 */
static inline void nx_list_init(nx_list_t *head)
{
    if (head != NULL) {
        head->prev = head;
        head->next = head;
    }
}

/**
 * @brief  Insert a node after a given position.
 *
 * @param  pos  The node to insert after, must not be NULL.
 * @param  node The node to insert, must not be NULL.
 */
static inline void nx_list_add(nx_list_t *pos, nx_list_t *node)
{
    if (pos == NULL || node == NULL) {
        return;
    }
    node->prev = pos;
    node->next = pos->next;
    pos->next->prev = node;
    pos->next = node;
}

/**
 * @brief  Insert a node at the head of the list (right after the head sentinel).
 *
 * @param  head List head, must not be NULL.
 * @param  node The node to insert, must not be NULL.
 */
static inline void nx_list_add_head(nx_list_t *head, nx_list_t *node)
{
    nx_list_add(head, node);
}

/**
 * @brief  Insert a node at the tail of the list (right before the head sentinel).
 *
 * Because the list is circular, this is just inserting before the head, i.e.
 * after head->prev.
 *
 * @param  head List head, must not be NULL.
 * @param  node The node to insert, must not be NULL.
 */
static inline void nx_list_add_tail(nx_list_t *head, nx_list_t *node)
{
    if (head != NULL) {
        nx_list_add(head->prev, node);
    }
}

/**
 * @brief  Remove a node from the list.
 *
 * The node's prev/next are left pointing to the now-detached neighbors; the
 * caller should either re-insert the node or consider it unlinked.
 *
 * @param  node The node to remove, must not be NULL.
 */
static inline void nx_list_del(nx_list_t *node)
{
    if (node == NULL) {
        return;
    }
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

/**
 * @brief  Check whether a list is empty.
 *
 * An empty list has head->next == head (the ring contains only the sentinel).
 *
 * @param  head List head, must not be NULL.
 * @return true if empty, false otherwise. A NULL head is treated as empty.
 */
static inline bool nx_list_is_empty(const nx_list_t *head)
{
    return (head == NULL) || (head->next == head);
}

/**
 * @brief  Get the first node in the list.
 *
 * @param  head List head, must not be NULL.
 * @return Pointer to the first node, or @p head itself if the list is empty.
 *
 * @note   Check nx_list_is_empty first, or compare the result against @p head.
 */
static inline nx_list_t *nx_list_first(const nx_list_t *head)
{
    return head->next;
}

/**
 * @brief  Get the last node in the list.
 *
 * @param  head List head, must not be NULL.
 * @return Pointer to the last node, or @p head itself if the list is empty.
 *
 * @note   Check nx_list_is_empty first, or compare the result against @p head.
 */
static inline nx_list_t *nx_list_last(const nx_list_t *head)
{
    return head->prev;
}

/**
 * @brief  Get a pointer to the structure that contains a list node.
 *
 * Given a pointer to an nx_list_t member inside a structure, this computes the
 * pointer to the containing structure.
 *
 * @param  ptr    Pointer to the nx_list_t member.
 * @param  type   The type of the containing structure.
 * @param  member The name of the nx_list_t member within that structure.
 * @return Pointer to the containing structure.
 */
#define nx_list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @brief  Iterate over a list.
 *
 * @param  pos  Iterator variable (nx_list_t *), assigned to each node in turn.
 * @param  head List head (the sentinel; iteration skips it).
 *
 * @note   Do not delete @p pos inside the loop; use nx_list_for_each_safe instead.
 */
#define nx_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * @brief  Iterate over a list, safe for deletion.
 *
 * @param  pos  Iterator variable (nx_list_t *).
 * @param  n    Temporary variable (nx_list_t *) to hold the next node.
 * @param  head List head (the sentinel; iteration skips it).
 *
 * @note   It is safe to delete @p pos inside the loop.
 */
#define nx_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#ifdef __cplusplus
}
#endif

#endif /* NX_LIST_H */
