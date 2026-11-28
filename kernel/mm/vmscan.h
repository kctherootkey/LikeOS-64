// LikeOS -- memory reclaim: watermarks, cache shrinkers and the background
// reclaim thread.
//
// The caches (file pages, inodes, directory entries, the filesystems' own
// per-file records) are allowed to grow into memory nobody else is using,
// and are given back when free memory runs low -- not trimmed to fixed
// sizes while most of the machine sits idle.  Three free-memory levels,
// scaled to the machine, decide when:
//
//   high  reclaim stops once free memory is back above it;
//   low   below it the reclaim thread is woken;
//   min   below it allocations reclaim inline (page cache only).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_MM_VMSCAN_H_
#define _KERNEL_MM_VMSCAN_H_

#include <kernel/uapi/types.h>

/* The free-memory levels, in pages. */
uint64_t vm_wmark_min(void);
uint64_t vm_wmark_low(void);
uint64_t vm_wmark_high(void);

/* A cache that can give memory back.
 *
 *   count  how many objects it could free now (unused ones), cheaply;
 *   scan   free up to `nr' of them, coldest first, and return how many it
 *          freed.  Runs in the reclaim thread: process context, no lock
 *          held, may sleep and take the cache's own locks.
 *
 * Registered while the cache exists; unregistering waits for a scan of it
 * that is in progress, so the cache can be torn down right after. */
struct vm_shrinker {
	const char *name;
	unsigned long (*count)(void *arg);
	unsigned long (*scan)(void *arg, unsigned long nr);
	void *arg;
	struct vm_shrinker *next; /* registry link, owned by vmscan.c */
};

void vm_register_shrinker(struct vm_shrinker *s);
void vm_unregister_shrinker(struct vm_shrinker *s);

/* Free memory is short: let the reclaim thread run.  Callable from any
 * context, with interrupts off and spinlocks held; it never sleeps. */
void vm_wake_reclaim(void);

/* Below the low level: wake the reclaim thread.  For allocation paths. */
void vm_check_free_memory(void);

/* Start the reclaim thread (once the scheduler runs kernel threads). */
void vm_reclaim_start(void);

#endif /* _KERNEL_MM_VMSCAN_H_ */
