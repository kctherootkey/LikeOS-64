// LikeOS - intrusive doubly linked lists
//
// The conventional circular list: a struct list_head embedded in each
// object, a head that is its own neighbour when empty, list_entry() /
// container_of() to get from the link back to the object.  No locking:
// the owner of a list protects it with its own lock.  list_del() leaves
// the entry pointing at itself (not at poison), so a second list_del() or
// a list_empty() on a removed entry is harmless.
//
// Note: the DRM port (kernel/dev/video/gpu/drm/gpu_util.h) carries its own
// definition of the same names; a file must not include both headers.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_LIST_H
#define KERNEL_KE_LIST_H

#include <kernel/uapi/types.h>

#ifndef container_of
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *entry, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

static inline void list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}

static inline void list_add_tail(struct list_head *entry,
				 struct list_head *head)
{
	__list_add(entry, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = entry;
	entry->prev = entry;
}

static inline void list_del_init(struct list_head *entry)
{
	list_del(entry);
}

static inline void list_move_tail(struct list_head *entry,
				  struct list_head *head)
{
	list_del(entry);
	list_add_tail(entry, head);
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && head->next == head->prev;
}

/* Move every entry of `list' to the end of `head'; `list' ends empty. */
static inline void list_splice_tail_init(struct list_head *list,
					 struct list_head *head)
{
	if (list_empty(list))
		return;
	list->next->prev = head->prev;
	head->prev->next = list->next;
	list->prev->next = head;
	head->prev = list->prev;
	INIT_LIST_HEAD(list);
}

static inline size_t list_count_nodes(const struct list_head *head)
{
	size_t n = 0;

	for (const struct list_head *p = head->next; p != head; p = p->next)
		n++;
	return n;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)
#define list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)
#define list_first_entry_or_null(head, type, member) \
	(list_empty(head) ? NULL : list_first_entry(head, type, member))
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_entry_is_head(pos, head, member) (&(pos)->member == (head))

#define list_for_each(pos, head) \
	for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define list_for_each_entry(pos, head, member)                         \
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member); \
	     !list_entry_is_head(pos, head, member);                   \
	     (pos) = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member)                 \
	for ((pos) = list_last_entry(head, __typeof__(*(pos)), member); \
	     !list_entry_is_head(pos, head, member);                   \
	     (pos) = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member)                 \
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member), \
	    (n) = list_next_entry(pos, member);                        \
	     !list_entry_is_head(pos, head, member);                   \
	     (pos) = (n), (n) = list_next_entry(n, member))

#endif /* KERNEL_KE_LIST_H */
