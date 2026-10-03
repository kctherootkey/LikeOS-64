// LikeOS -- small helpers shared by the graphics drivers.
//
// The display-manager core, the VMware SVGA driver and the SVGA II hardware
// layer carry a lot of code that is easiest to read in one vocabulary: sized
// integer names (u8 .. u64), bit and field macros for register layouts,
// type-checked min/max, rounding helpers and an intrusive doubly-linked list.
// They are collected here so every file spells them the same way.
//
// Everything is guarded: a definition that already exists (a driver header
// that brought its own DIV_ROUND_UP, say) is left alone, and an identical
// redefinition is harmless anyway.  Nothing here allocates, sleeps or
// touches the FPU.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_GPU_UTIL_H
#define KERNEL_DEV_GPU_GPU_UTIL_H

#include <kernel/uapi/types.h>

/* ---- sized integers ---------------------------------------------------- */

#ifndef GPU_UTIL_HAVE_SIZED_TYPES
#define GPU_UTIL_HAVE_SIZED_TYPES
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef signed char s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

/* ---- compiler -------------------------------------------------------- */

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif

#ifndef fallthrough
#define fallthrough __attribute__((fallthrough))
#endif

/* ---- bits and fields --------------------------------------------------- */

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n) (1ULL << (n))
#endif
/* Bits h..l inclusive, h >= l. */
#ifndef GENMASK
#define GENMASK(h, l) ((~0UL << (l)) & (~0UL >> (63 - (h))))
#endif
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) ((~0ULL << (l)) & (~0ULL >> (63 - (h))))
#endif
/* Place `val' into, or take it out of, the field `mask' describes.  The mask
 * must be a non-zero constant. */
#ifndef FIELD_PREP
#define FIELD_PREP(mask, val) \
	(((u64)(val) << __builtin_ctzll(mask)) & (mask))
#endif
#ifndef FIELD_GET
#define FIELD_GET(mask, reg) \
	(((u64)(reg) & (mask)) >> __builtin_ctzll(mask))
#endif
#ifndef FIELD_MAX
#define FIELD_MAX(mask) ((mask) >> __builtin_ctzll(mask))
#endif

#ifndef lower_32_bits
#define lower_32_bits(n) ((u32)((n) & 0xffffffffULL))
#endif
#ifndef upper_32_bits
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#endif

/* Index of the most significant set bit, counting from 1; 0 for 0. */
static inline int gpu_fls(u32 x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}

static inline int gpu_fls64(u64 x)
{
	return x ? 64 - __builtin_clzll(x) : 0;
}

/* Index of the least significant set bit, counting from 1; 0 for 0. */
static inline int gpu_ffs(u32 x)
{
	return __builtin_ffs((int)x);
}

/* Set bits, counted without the compiler's population-count builtin: the
 * kernel is built for any x86-64, so that builtin becomes a call into the
 * compiler's support library, which the kernel does not link. */
static inline unsigned int gpu_hweight32(u32 x)
{
	x = x - ((x >> 1) & 0x55555555u);
	x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
	x = (x + (x >> 4)) & 0x0f0f0f0fu;
	return (unsigned int)((x * 0x01010101u) >> 24);
}

static inline unsigned int gpu_hweight64(u64 x)
{
	return gpu_hweight32((u32)x) + gpu_hweight32((u32)(x >> 32));
}

static inline bool is_power_of_2(u64 n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

/* Smallest power of two >= n (n >= 1, result fits in 64 bits). */
static inline u64 gpu_roundup_pow_of_two(u64 n)
{
	return n <= 1 ? 1 : 1ULL << gpu_fls64(n - 1);
}

/* ---- min / max / clamp ------------------------------------------------- */

/* Both operands are evaluated once; mixing types is a compile-time
 * diagnostic (comparison of distinct pointer types), which is the point --
 * use min_t() to say which type the comparison is made in. */
#ifndef min
#define min(a, b)                                    \
	({                                           \
		__typeof__(a) _min_a = (a);          \
		__typeof__(b) _min_b = (b);          \
		(void)(&_min_a == &_min_b);          \
		_min_a < _min_b ? _min_a : _min_b;   \
	})
#endif
#ifndef max
#define max(a, b)                                    \
	({                                           \
		__typeof__(a) _max_a = (a);          \
		__typeof__(b) _max_b = (b);          \
		(void)(&_max_a == &_max_b);          \
		_max_a > _max_b ? _max_a : _max_b;   \
	})
#endif
#ifndef min_t
#define min_t(type, a, b)                            \
	({                                           \
		type _mint_a = (a);                  \
		type _mint_b = (b);                  \
		_mint_a < _mint_b ? _mint_a : _mint_b; \
	})
#endif
#ifndef max_t
#define max_t(type, a, b)                            \
	({                                           \
		type _maxt_a = (a);                  \
		type _maxt_b = (b);                  \
		_maxt_a > _maxt_b ? _maxt_a : _maxt_b; \
	})
#endif
#ifndef clamp_t
#define clamp_t(type, val, lo, hi) \
	min_t(type, max_t(type, val, lo), hi)
#endif
#ifndef clamp
#define clamp(val, lo, hi) min(max(val, lo), hi)
#endif
#ifndef clamp_val
#define clamp_val(val, lo, hi) clamp_t(__typeof__(val), val, lo, hi)
#endif
#ifndef swap
#define swap(a, b)                              \
	do {                                    \
		__typeof__(a) _swap_t = (a);    \
		(a) = (b);                      \
		(b) = _swap_t;                  \
	} while (0)
#endif

/* ---- rounding and division --------------------------------------------- */

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#ifndef DIV_ROUND_UP_ULL
#define DIV_ROUND_UP_ULL(n, d) \
	((u64)(((u64)(n) + (u64)(d) - 1) / (u64)(d)))
#endif
/* Nearest, halves away from zero; both operands non-negative. */
#ifndef DIV_ROUND_CLOSEST
#define DIV_ROUND_CLOSEST(n, d) (((n) + ((d) / 2)) / (d))
#endif
#ifndef DIV_ROUND_CLOSEST_ULL
#define DIV_ROUND_CLOSEST_ULL(n, d) \
	((u64)(((u64)(n) + (u64)(d) / 2) / (u64)(d)))
#endif
#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) ((x) - ((x) % (y)))
#endif
/* Power-of-two alignment. */
#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#endif
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#endif
#ifndef IS_ALIGNED
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#endif
/* x * n / d without overflowing the intermediate product. */
#ifndef mult_frac
#define mult_frac(x, n, d)                                   \
	({                                                   \
		__typeof__(x) _mf_q = (x) / (d);             \
		__typeof__(x) _mf_r = (x) % (d);             \
		(_mf_q * (n)) + ((_mf_r * (n)) / (d));       \
	})
#endif

static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return dividend / divisor;
}

static inline u64 div64_u64(u64 dividend, u64 divisor)
{
	return dividend / divisor;
}

static inline s64 div_s64(s64 dividend, s32 divisor)
{
	return dividend / divisor;
}

static inline s64 div64_s64(s64 dividend, s64 divisor)
{
	return dividend / divisor;
}

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = (u32)(dividend % divisor);
	return dividend / divisor;
}

/* ---- intrusive doubly-linked list -------------------------------------- */

/* A circular list with a sentinel head.  An entry is embedded in the object
 * it links; list_entry() gets the object back.  No locking: the caller
 * serialises. */
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

static inline void __list_add(struct list_head *entry,
			      struct list_head *prev, struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

/* Insert after `head' (stack order). */
static inline void list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}

/* Insert before `head' (queue order). */
static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

/* Unlink; the entry is left pointing nowhere useful (NULL), so a second
 * list_del() faults instead of corrupting a neighbour. */
static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = NULL;
	entry->prev = NULL;
}

/* Unlink and leave the entry as an empty list of its own, so list_empty()
 * on it is true and deleting it again is harmless. */
static inline void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

static inline void list_replace(struct list_head *old, struct list_head *entry)
{
	entry->next = old->next;
	entry->next->prev = entry;
	entry->prev = old->prev;
	entry->prev->next = entry;
}

static inline void list_replace_init(struct list_head *old,
				     struct list_head *entry)
{
	list_replace(old, entry);
	INIT_LIST_HEAD(old);
}

static inline void list_move(struct list_head *entry, struct list_head *head)
{
	__list_del(entry->prev, entry->next);
	list_add(entry, head);
}

static inline void list_move_tail(struct list_head *entry,
				  struct list_head *head)
{
	__list_del(entry->prev, entry->next);
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

static inline int list_is_last(const struct list_head *entry,
			       const struct list_head *head)
{
	return entry->next == head;
}

static inline void __list_splice(const struct list_head *list,
				 struct list_head *prev, struct list_head *next)
{
	struct list_head *first = list->next;
	struct list_head *last = list->prev;

	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}

/* Move every entry of `list' to the front of `head'; `list' becomes empty. */
static inline void list_splice_init(struct list_head *list,
				    struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}

/* Move every entry of `list' to the back of `head'; `list' becomes empty. */
static inline void list_splice_tail_init(struct list_head *list,
					 struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

static inline size_t list_count_nodes(const struct list_head *head)
{
	size_t n = 0;
	const struct list_head *p;

	for (p = head->next; p != head; p = p->next)
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
#define list_for_each_safe(pos, n, head)                            \
	for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head); \
	     (pos) = (n), (n) = (pos)->next)
#define list_for_each_entry(pos, head, member)                         \
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member); \
	     !list_entry_is_head(pos, head, member);                    \
	     (pos) = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member)                \
	for ((pos) = list_last_entry(head, __typeof__(*(pos)), member); \
	     !list_entry_is_head(pos, head, member);                   \
	     (pos) = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member)                 \
	for ((pos) = list_first_entry(head, __typeof__(*(pos)), member), \
	    (n) = list_next_entry(pos, member);                         \
	     !list_entry_is_head(pos, head, member);                    \
	     (pos) = (n), (n) = list_next_entry(n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member)        \
	for ((pos) = list_last_entry(head, __typeof__(*(pos)), member), \
	    (n) = list_prev_entry(pos, member);                         \
	     !list_entry_is_head(pos, head, member);                    \
	     (pos) = (n), (n) = list_prev_entry(n, member))

#endif /* KERNEL_DEV_GPU_GPU_UTIL_H */
