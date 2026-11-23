/*
 * User-memory access primitives shared by the syscall layer.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _KERNEL_KE_UACCESS_H
#define _KERNEL_KE_UACCESS_H

#include <kernel/uapi/types.h>

/* NOTE: futex.c, sched.c, io/tty.c, dev/video/fbdev.c and mm/memory.c carry
 * their own private static copies of some of these; do NOT include this
 * header there -- the declarations would collide, deliberately. */

int copy_from_user(void *kernel_dst, const void *user_src, size_t len);
int copy_to_user(void *user_dst, const void *kernel_src, size_t len);
int copy_user_path(const char *user_path, char *kbuf, size_t kbuf_size);
int copy_user_string(const char *user_str, size_t max_len,
		     char **out_str, size_t *out_len);
/* A NULL-terminated user array of strings into a kalloc'd array of kalloc'd
 * strings (*out_arr; NULL for a NULL user_arr), taking its strings from the
 * budgets *count_left (entries) and *bytes_left (bytes, each NUL included),
 * which shrink by what was taken -- shared between argv and envp by exec.
 * 0, -EFAULT, -ENOMEM, or -E2BIG when the array does not fit the budgets.
 * The user array is read once for its length and once for its strings; an
 * entry that turns NULL meanwhile ends it there. */
int copy_user_string_array(const char *const *user_arr, size_t *count_left,
			   size_t *bytes_left, char ***out_arr);
void free_user_string_array(char **arr);
int user_strnlen(const char *user_str, size_t max_len, size_t *out_len);
bool validate_user_ptr(uint64_t ptr, size_t len);

#endif /* _KERNEL_KE_UACCESS_H */
