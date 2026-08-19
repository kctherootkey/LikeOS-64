/* User-memory access primitives shared by the syscall layer. */
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
int copy_user_string_array(const char *const *user_arr, size_t max_count,
				  size_t max_str_len, size_t max_total_bytes,
				  char ***out_arr);
void free_user_string_array(char **arr);
int user_strnlen(const char *user_str, size_t max_len, size_t *out_len);
bool validate_user_ptr(uint64_t ptr, size_t len);

#endif /* _KERNEL_KE_UACCESS_H */
