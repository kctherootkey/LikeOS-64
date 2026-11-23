// LikeOS -- host unit test of execve's argument limits: the shared argv +
// envp budget of copy_user_string_array() (kernel/ke/uaccess.c, at
// EXEC_ARG_MAX bytes and EXEC_ARG_COUNT_MAX strings, E2BIG beyond) and the
// #! loader's stack-fit check (kernel/ke/script_loader.c), at 127, 128 and
// 1000 strings, 128 KiB and 128 KiB + 1.
//
// Built by host/test-vfs-path.sh on the filesystem harness's kernel
// services.  The harness has user-copy stand-ins of its own; the kernel's
// are renamed here so that the code under test uses the real ones.  "User
// memory" is host memory.
//
// Copyright (C) 2026 The LikeOS Project

#define copy_from_user kua_copy_from_user
#define copy_to_user kua_copy_to_user
#define validate_user_ptr kua_validate_user_ptr
#define copy_user_path kua_copy_user_path
#include "kernel/ke/uaccess.c"
#include "kernel/ke/script_loader.c"
#include <kernel/ke/elf.h>

void test_task_init(uint32_t uid, uint32_t gid);
extern long test_kalloc_live;
extern int test_quiet_klog;

static int failures, checks;

#define CHECK(c)                                                        \
	do {                                                            \
		checks++;                                               \
		if (!(c)) {                                             \
			h_printf("FAIL line %d: %s\n", __LINE__, #c);   \
			failures++;                                     \
		}                                                       \
	} while (0)

/* "user" memory: one big area for the strings, one for the pointers */
#define AREA (EXEC_ARG_MAX * 2 + 4096)
static char g_str[AREA];
static const char *g_ptr[EXEC_ARG_COUNT_MAX + 16];

/* n strings of `len' bytes each (the NUL not counted), NULL after them */
static const char *const *mk(int n, unsigned long len, char c)
{
	unsigned long pos = 0;

	for (int i = 0; i < n; i++) {
		if (pos + len + 1 > AREA) {
			h_printf("test-exec-args: area too small\n");
			return NULL;
		}
		g_ptr[i] = g_str + pos;
		for (unsigned long k = 0; k < len; k++)
			g_str[pos++] = c;
		g_str[pos++] = 0;
	}
	g_ptr[n] = NULL;
	return g_ptr;
}

static int count(char **a)
{
	int n = 0;

	while (a && a[n])
		n++;
	return n;
}

/* argv as given, envp NULL: the status, and the strings copied */
static int copy1(const char *const *u, char ***out)
{
	size_t c = EXEC_ARG_COUNT_MAX, b = EXEC_ARG_MAX;

	return copy_user_string_array(u, &c, &b, out);
}

int main(void)
{
	char **a = NULL, **e = NULL;
	long live;
	task_t *cur;

	test_task_init(1000, 1000);
	/* copy_from_user() WARNs that its destination is no kernel address:
	 * here every address is a host one. */
	test_quiet_klog = 1;
	cur = sched_current();
	cur->privilege = TASK_USER;
	cur->pml4 = (uint64_t *)(uintptr_t)0x1000; /* (never dereferenced) */
	live = test_kalloc_live;

	/* 127, 128, 1000 strings: all of them, exactly */
	CHECK(copy1(mk(127, 10, 'a'), &a) == 0 && count(a) == 127);
	free_user_string_array(a);
	CHECK(copy1(mk(128, 10, 'b'), &a) == 0 && count(a) == 128 &&
	      a[127][0] == 'b' && a[127][10] == 0);
	free_user_string_array(a);
	CHECK(copy1(mk(1000, 20, 'c'), &a) == 0 && count(a) == 1000);
	free_user_string_array(a);

	/* bytes: 128 KiB in all (NULs counted) fits, one more does not */
	/* 128 strings of 1023 bytes + NUL = 131072 */
	CHECK(copy1(mk(128, 1023, 'd'), &a) == 0 && count(a) == 128);
	free_user_string_array(a);
	a = (char **)1;
	CHECK(copy1(mk(129, 1023, 'd'), &a) == -E2BIG && a == NULL);
	/* one string of the whole budget: 131071 bytes + NUL */
	CHECK(copy1(mk(1, EXEC_ARG_MAX - 1, 'e'), &a) == 0 &&
	      kstrlen(a[0]) == EXEC_ARG_MAX - 1);
	free_user_string_array(a);
	CHECK(copy1(mk(1, EXEC_ARG_MAX, 'e'), &a) == -E2BIG);

	/* strings: EXEC_ARG_COUNT_MAX empty ones fit, one more is E2BIG */
	CHECK(copy1(mk(EXEC_ARG_COUNT_MAX, 0, 'f'), &a) == 0 &&
	      count(a) == EXEC_ARG_COUNT_MAX);
	free_user_string_array(a);
	CHECK(copy1(mk(EXEC_ARG_COUNT_MAX + 1, 0, 'f'), &a) == -E2BIG);

	/* argv and envp share the budget */
	{
		size_t c = EXEC_ARG_COUNT_MAX, b = EXEC_ARG_MAX;

		CHECK(copy_user_string_array(mk(64, 1023, 'g'), &c, &b, &a) ==
		      0);
		CHECK(b == EXEC_ARG_MAX - 64 * 1024 && c == EXEC_ARG_COUNT_MAX - 64);
		CHECK(copy_user_string_array(mk(64, 1023, 'h'), &c, &b, &e) ==
			      0 &&
		      b == 0 && count(e) == 64);
		free_user_string_array(e);
		e = NULL;
		CHECK(copy_user_string_array(mk(1, 0, 'i'), &c, &b, &e) ==
		      -E2BIG);
		free_user_string_array(a);
		a = NULL;
	}
	/* a desktop environment: 139 variables, 4 KiB of them */
	CHECK(copy1(mk(139, 30, 'v'), &e) == 0 && count(e) == 139);
	free_user_string_array(e);

	/* NULL array: none; a bad element: EFAULT */
	CHECK(copy1(NULL, &a) == 0 && a == NULL);
	mk(3, 5, 'j');
	g_ptr[1] = (const char *)(uintptr_t)0x10;
	CHECK(copy1(g_ptr, &a) == -EFAULT && a == NULL);

	/* the #! loader's check of the final argument block */
	{
		char **av = NULL, **ev = NULL;

		CHECK(copy1(mk(1000, 100, 'k'), &av) == 0);
		CHECK(script_check_stack_fit(av, NULL) == 0);
		free_user_string_array(av);
		CHECK(copy1(mk(128, 1023, 'l'), &av) == 0);
		CHECK(script_check_stack_fit(av, NULL) == 0);
		/* far past what execve takes: refused */
		CHECK(copy1(mk(1, 4000, 'm'), &ev) == 0);
		{
			static char *big[300];
			for (int i = 0; i < 299; i++)
				big[i] = av[i % 128];
			big[299] = NULL;
			CHECK(script_check_stack_fit(big, ev) == -E2BIG);
		}
		free_user_string_array(av);
		free_user_string_array(ev);
	}

	CHECK(test_kalloc_live == live); /* nothing left behind */
	h_printf("test-exec-args: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
