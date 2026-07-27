// LikeOS-64 - shebang (#!) script loader for execve()
#ifndef _KERNEL_SCRIPT_LOADER_H_
#define _KERNEL_SCRIPT_LOADER_H_

#include <kernel/uapi/types.h>

// Maximum length of the #! line examined, including the newline.  A full
// buffer with no newline is rejected (ENOEXEC).
#define SCRIPT_BUF_SIZE 256

// Maximum number of interpreter-is-itself-a-script rewrites before ELOOP.
#define SCRIPT_MAX_DEPTH 4

/* Pure #! line parser, no I/O (unit-testable in isolation).
 * buf[0..len) holds the first bytes of the file, len <= SCRIPT_BUF_SIZE.
 * Returns 1  = shebang parsed: interp is NUL-terminated; if *has_arg is set,
 *              optarg_buf holds the single optional argument (everything
 *              after the interpreter, internal blanks preserved, trailing
 *              space/tab/CR/LF trimmed).
 *         0  = not a script (len < 2 or no leading "#!").
 *        <0  = -ENOEXEC (empty interpreter, or no newline within a full
 *              SCRIPT_BUF_SIZE buffer), -ENAMETOOLONG (interpreter path
 *              does not fit interp_sz). */
int script_parse_shebang(const char *buf, size_t len, char *interp,
			 size_t interp_sz, char *optarg_buf, size_t optarg_sz,
			 int *has_arg);

/* One shebang rewrite step for execve.  *kpath_io / *kargv_io are the
 * kernel copies owned by sys_execve (kalloc'd string / NULL-terminated
 * array of kalloc'd strings).
 * Returns 1  = file was a script: *kpath_io now names the interpreter and
 *              *kargv_io was rewritten to
 *              [interp, optarg?, script-path, old argv[1..]]
 *              (old allocations consumed or freed; the results remain
 *              individually owned exactly like copy_user_string_array's).
 *         0  = not a script; inputs untouched.
 *        <0  = negative errno; inputs untouched: -ENOENT/-EACCES/-EIO
 *              (open/read failed), -ENOEXEC/-ENAMETOOLONG (bad #! line),
 *              -ELOOP (script at depth >= SCRIPT_MAX_DEPTH), -E2BIG
 *              (rewritten argv would exceed the exec argv limit),
 *              -ENOMEM. */
int script_load_rewrite(char **kpath_io, char ***kargv_io, int depth);

/* Conservative pre-check that argv/envp will fit elf_setup_stack's
 * one-page initial-stack budget.  Returns 0 or -E2BIG. */
int script_check_stack_fit(char *const argv[], char *const envp[]);

#endif // _KERNEL_SCRIPT_LOADER_H_
