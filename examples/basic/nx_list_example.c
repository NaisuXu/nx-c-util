/**
 * @file    nx_list_example.c
 * @brief   Example usage of the nx_list intrusive linked list.
 */
#include "core/nx_list.h"

#include <stdio.h>
#include <stddef.h>

/* Example user structure with an embedded list node */
typedef struct task {
    int         id;
    const char *name;
    nx_list_t   link;   /* intrusive list hook */
} task_t;

int nx_list_example_run(void)
{
    printf("########## nx_list examples ##########\n");

    nx_list_t head;
    nx_list_init(&head);

    /* Create some tasks */
    task_t t1 = {1, "Init",     {NULL, NULL}};
    task_t t2 = {2, "Parse",    {NULL, NULL}};
    task_t t3 = {3, "Execute",  {NULL, NULL}};
    task_t t4 = {4, "Cleanup",  {NULL, NULL}};

    /* Add to list */
    nx_list_add_tail(&head, &t1.link);
    nx_list_add_tail(&head, &t2.link);
    nx_list_add_tail(&head, &t3.link);
    nx_list_add_tail(&head, &t4.link);

    printf("Task list:\n");
    nx_list_t *pos;
    nx_list_for_each(pos, &head) {
        task_t *t = nx_list_entry(pos, task_t, link);
        printf("  Task %d: %s\n", t->id, t->name);
    }

    /* Remove a task from the middle */
    nx_list_del(&t2.link);
    printf("\nAfter removing 'Parse':\n");
    nx_list_for_each(pos, &head) {
        task_t *t = nx_list_entry(pos, task_t, link);
        printf("  Task %d: %s\n", t->id, t->name);
    }

    /* Add a new task at head */
    task_t t5 = {5, "PreInit", {NULL, NULL}};
    nx_list_add_head(&head, &t5.link);
    printf("\nAfter adding 'PreInit' at head:\n");
    nx_list_for_each(pos, &head) {
        task_t *t = nx_list_entry(pos, task_t, link);
        printf("  Task %d: %s\n", t->id, t->name);
    }

    /* Safe deletion while iterating */
    printf("\nDeleting all tasks with id > 2:\n");
    nx_list_t *n;
    nx_list_for_each_safe(pos, n, &head) {
        task_t *t = nx_list_entry(pos, task_t, link);
        if (t->id > 2) {
            printf("  Removing Task %d: %s\n", t->id, t->name);
            nx_list_del(&t->link);
        }
    }

    printf("\nFinal task list:\n");
    if (nx_list_is_empty(&head)) {
        printf("  (empty)\n");
    } else {
        nx_list_for_each(pos, &head) {
            task_t *t = nx_list_entry(pos, task_t, link);
            printf("  Task %d: %s\n", t->id, t->name);
        }
    }

    printf("\n");
    return 0;
}
