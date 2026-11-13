/*
 * The user-address range check shared by every user-memory accessor.
 *
 * A user range [ptr, ptr + len) is acceptable when it lies entirely in
 * the user half: starting at or above the low guard (no NULL-ish
 * pointers) and ending at or below the top of user space.  The check is
 * written so that it cannot overflow -- `len' is compared against the
 * room left above `ptr', never added to it -- so a huge length cannot
 * wrap a range into the kernel half.
 *
 * Kept free of other declarations so that the files with their own
 * private copies of the uaccess helpers (see uaccess.h) can use it too.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _KERNEL_KE_UACCESS_RANGE_H
#define _KERNEL_KE_UACCESS_RANGE_H

#include <kernel/uapi/types.h>

/* Lowest user address accepted (the first 64 KiB are never mapped). */
#define UACCESS_USER_MIN 0x10000ULL
/* Top of user space: no user range reaches past this address (2^47 minus
 * one page). */
#define UACCESS_USER_LIMIT 0x00007FFFFFFFF000ULL

static inline bool user_range_ok(uint64_t ptr, uint64_t len)
{
	if (ptr < UACCESS_USER_MIN || ptr >= UACCESS_USER_LIMIT)
		return false;
	return len <= UACCESS_USER_LIMIT - ptr;
}

/* How many bytes from `ptr' on are user addresses (0 for a bad ptr). */
static inline uint64_t user_range_room(uint64_t ptr)
{
	if (ptr < UACCESS_USER_MIN || ptr >= UACCESS_USER_LIMIT)
		return 0;
	return UACCESS_USER_LIMIT - ptr;
}

#endif /* _KERNEL_KE_UACCESS_RANGE_H */
