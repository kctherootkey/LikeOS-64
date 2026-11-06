// LikeOS -- LikeOS Image Viewer: the library's allocations, counted.
//
// The in-place operations of the library (iv_adjust_apply, iv_style_apply,
// iv_bg_apply, ...) return nothing: when a buffer they need cannot be had
// they leave the picture as it is, or do only part of their work.  That is
// right for a preview, but a caller writing the result to a file must know
// that it did not get what it asked for.  So every allocation of the
// library goes through the functions here (ivpriv.h maps malloc, calloc
// and realloc onto them), and each failure is counted for the thread that
// saw it; iv_alloc_failures() tells the caller afterwards.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>

#include "ivpriv.h"

/* The real allocator from here on. */
#undef malloc
#undef calloc
#undef realloc

static _Thread_local unsigned failures;

void ivp_note_alloc_failure(void)
{
	if (failures < UINT32_MAX)
		failures++;
}

void *ivp_malloc_counted(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (p == NULL)
		ivp_note_alloc_failure();
	return p;
}

void *ivp_calloc_counted(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);

	if (p == NULL)
		ivp_note_alloc_failure();
	return p;
}

void *ivp_realloc_counted(void *old, size_t n)
{
	void *p = realloc(old, n ? n : 1);

	if (p == NULL)
		ivp_note_alloc_failure();
	return p;
}

void iv_alloc_failures_reset(void)
{
	failures = 0;
}

unsigned iv_alloc_failures(void)
{
	return failures;
}
