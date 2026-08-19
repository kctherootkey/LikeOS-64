// LikeOS-64 -- user-memory access primitives.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	if (ptr < 0x10000)
		return false; // Reject low addresses (NULL deref protection)
	if (ptr >= 0x7FFFFFFFFFFF)
		return false; // Beyond user space
	if (ptr + len < ptr)
		return false; // Overflow check
	return true;
}

// SMAP-aware copy from user space to kernel space
// Returns 0 on success, -EFAULT on failure
int copy_from_user(void *kernel_dst, const void *user_src, size_t len)
{
	/* Destination must be a kernel address, not user space */
	WARN_ON((uint64_t)kernel_dst < 0x8000000000000000UL &&
		(uint64_t)kernel_dst >= 0x1000UL);
	if (!validate_user_ptr((uint64_t)user_src, len)) {
		return -EFAULT;
	}
	if (!kernel_dst || len == 0) {
		return (len == 0) ? 0 : -EFAULT;
	}

	/* Same kill-mid-syscall safety net as in copy_to_user — see the long
     * comment there.  If the current task can't own user memory (zombied,
     * exited, or pml4 freed), CR3 holds the kernel-only PML4 and any
     * dereference of user_src would page-fault into kernel_oops.  Return
     * -EFAULT so the caller's normal error path runs instead. */
	{
		task_t *cur = sched_current();
		if (!cur || cur->privilege != TASK_USER || cur->pml4 == NULL ||
		    cur->has_exited || cur->state == TASK_ZOMBIE) {
			return -EFAULT;
		}
	}

	// Temporarily allow supervisor access to user pages (SMAP bypass)
	smap_disable();
	mm_memcpy(kernel_dst, user_src, len);
	// Re-enable SMAP protection
	smap_enable();
	return 0;
}

// SMAP-aware copy from kernel space to user space
// Returns 0 on success, -EFAULT on failure
int copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	/* Destination must be a user-space address */
	WARN_ON((uint64_t)user_dst >= 0x8000000000000000UL);
	if (!validate_user_ptr((uint64_t)user_dst, len)) {
		return -EFAULT;
	}
	if (!kernel_src || len == 0) {
		return (len == 0) ? 0 : -EFAULT;
	}

	/* Safety net for the "task was killed mid-syscall" race:
     *
     * If a user task is SIGKILL'd (or otherwise zombied) while suspended
     * inside a syscall via sched_schedule(), its pml4 may have been freed
     * by mm_destroy_address_space() on the killer's CPU.  The scheduler's
     * switch_address_space() then falls back to g_kernel_pml4 on resume
     * (cur->pml4 ? cur->pml4 : g_kernel_pml4), so CR3 holds the kernel-only
     * PML4 with no user mappings.  The original syscall handler still has
     * the user pointer in registers and calls copy_to_user() — which would
     * page-fault in kernel mode on a user address (cr2 < kernel base),
     * with current_task pointing to a kernel-thread fallback so
     * exception_handler can't even route it to SIGSEGV: that path
     * matches "kernel-mode + user addr + current_task is kernel thread",
     * goes to kernel_oops, and we lose the whole system.
     *
     * sched_schedule()'s zombie self-check above is the primary guard;
     * this is a backstop for any path that somehow reaches us with a
     * current task that can't own user memory (no pml4, exited, or
     * zombied since the syscall began).  Fail with -EFAULT — the caller
     * just sees a normal copy failure instead of an oops. */
	{
		task_t *cur = sched_current();
		if (!cur || cur->privilege != TASK_USER || cur->pml4 == NULL ||
		    cur->has_exited || cur->state == TASK_ZOMBIE) {
			return -EFAULT;
		}
	}

	// Temporarily allow supervisor access to user pages (SMAP bypass)
	smap_disable();
	mm_memcpy(user_dst, kernel_src, len);
	// Re-enable SMAP protection
	smap_enable();
	return 0;
}

// Safe string length (bounded) from user space (SMAP-aware)
int user_strnlen(const char *user_str, size_t max_len, size_t *out_len)
{
	if (!user_str || !out_len) {
		return -EFAULT;
	}
	// Validate entire potential range first
	if (!validate_user_ptr((uint64_t)user_str, max_len)) {
		return -EFAULT;
	}
	// Temporarily allow user memory access
	smap_disable();
	size_t i;
	for (i = 0; i < max_len; i++) {
		if (user_str[i] == '\0') {
			*out_len = i;
			smap_enable();
			return 0;
		}
	}
	smap_enable();
	return -EINVAL; // Too long
}

int copy_user_string(const char *user_str, size_t max_len,
		     char **out_str, size_t *out_len)
{
	if (!user_str || !out_str) {
		return -EFAULT;
	}

	size_t len = 0;
	int ret = user_strnlen(user_str, max_len, &len);
	if (ret != 0) {
		return ret;
	}

	char *kstr = (char *)kalloc(len + 1);
	if (!kstr) {
		return -ENOMEM;
	}
	// Use copy_from_user for SMAP-aware copy
	if (copy_from_user(kstr, user_str, len) != 0) {
		kfree(kstr);
		return -EFAULT;
	}
	kstr[len] = '\0';

	*out_str = kstr;
	if (out_len) {
		*out_len = len;
	}
	return 0;
}

// Helper: Copy user path string directly into fixed kernel buffer (no allocation)
// Returns 0 on success, negative error on failure
int copy_user_path(const char *user_path, char *kbuf, size_t kbuf_size)
{
	if (!user_path || !kbuf || kbuf_size < 2) {
		return -EINVAL;
	}
	char *kstr = NULL;
	size_t len = 0;
	int ret = copy_user_string(user_path, kbuf_size - 1, &kstr, &len);
	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i <= len; i++) {
		kbuf[i] = kstr[i];
	}
	kfree(kstr);
	return 0;
}

void free_user_string_array(char **arr)
{
	if (!arr) {
		return;
	}
	for (size_t i = 0; arr[i]; i++) {
		kfree(arr[i]);
	}
	kfree(arr);
}

int copy_user_string_array(const char *const *user_arr, size_t max_count,
			   size_t max_str_len, size_t max_total_bytes,
			   char ***out_arr)
{
	if (!out_arr) {
		return -EFAULT;
	}
	*out_arr = NULL;

	if (!user_arr) {
		return 0;
	}

	if (!validate_user_ptr((uint64_t)user_arr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	char **karr = (char **)kalloc((max_count + 1) * sizeof(char *));
	if (!karr) {
		return -ENOMEM;
	}
	mm_memset(karr, 0, (max_count + 1) * sizeof(char *));

	size_t total = 0;
	for (size_t i = 0; i < max_count; i++) {
		// SMAP-aware read of user array element
		const char *user_str;
		smap_disable();
		user_str = user_arr[i];
		smap_enable();
		if (!user_str) {
			karr[i] = NULL;
			*out_arr = karr;
			return 0;
		}
		if (!validate_user_ptr((uint64_t)user_arr +
					       (i * sizeof(uint64_t)),
				       sizeof(uint64_t))) {
			free_user_string_array(karr);
			return -EFAULT;
		}

		char *kstr = NULL;
		size_t len = 0;
		int ret = copy_user_string(user_str, max_str_len, &kstr, &len);
		if (ret != 0) {
			free_user_string_array(karr);
			return ret;
		}

		total += len + 1;
		if (total > max_total_bytes) {
			kfree(kstr);
			free_user_string_array(karr);
			return -EINVAL;
		}

		karr[i] = kstr;
	}

	free_user_string_array(karr);
	return -EINVAL; // Too many entries
}
