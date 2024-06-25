/* Copyright (c) 2022-2024, Evgeniy Baskov. All rights reserved */

#ifndef LIST_H_
#define LIST_H_ 1

#include <stdbool.h>

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_FOREACH(it__, head__) \
    for (struct list_head *it__ = (head__)->next; \
         (it__) != (head__); \
         (it__) = (it__)->next)

#define LIST_FOREACH_SAFE(it__, head__) \
    for (struct list_head *it__ = (head__)->next, *next__ = (head__)->next->next; \
         (it__) != (head__); \
         (it__) = next__, next__ = (it__)->next)

#define LIST_FOREACH_CONTINUTE_SAFE(it__, continue__, head__) \
    for (struct list_head *it__ = (continue__), *next__ = (continue__)->next; \
         (it__) != (head__); \
         (it__) = next__, next__ = (it__)->next)

static inline struct list_head *list_remove(struct list_head *head) {
    head->next->prev = head->prev;
    head->prev->next = head->next;
    return head;
}

static inline void list_init(struct list_head *head) {
    head->next = head->prev = head;
}

static inline bool list_empty(struct list_head *head) {
    return !head || head->next == head;
}

static inline struct list_head *list_add(struct list_head *head, struct list_head *prev, struct list_head *next) {
    prev->next = head;
    next->prev = head;

    head->next = next;
    head->prev = prev;
    return head;
}

static inline struct list_head *list_insert_after(struct list_head *head, struct list_head *elem) {
    return list_add(elem, head, head->next);
}

static inline struct list_head *list_insert_before(struct list_head *head, struct list_head *elem) {
    return list_add(elem, head->prev, head);
}

static inline struct list_head *list_add_range(struct list_head *first, struct list_head *last, struct list_head *prev, struct list_head *next) {
    first->prev = prev;
    last->next = next;

    prev->next = first;
    next->prev = last;
    return first;
}

static inline struct list_head *list_insert_range_after(struct list_head *head, struct list_head *first, struct list_head *last) {
    return list_add_range(first, last, head, head->next);
}

static inline struct list_head *list_insert_range_before(struct list_head *head, struct list_head *first, struct list_head *last) {
    return list_add_range(first, last, head->prev, head);
}

#endif
