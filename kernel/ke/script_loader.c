// LikeOS-64 - shebang (#!) script loader
//
// Turns execve() of a "#!interpreter [arg]" file into execve() of the
// interpreter with the script path appended to argv, following the
// conventional Unix interpreter-script semantics.  Uses only the
// fs-independent vfs_* API and the existing VFS synchronization; no locks
// or persistent state of its own.
#include <kernel/ke/script_loader.h>
#include <kernel/ke/syscall.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

// Exec argv ceiling shared with sys_execve's copy_user_string_array(128)
// and elf_setup_stack's av[128] table.
#define SCRIPT_MAX_ARGC 127

// Local ST_* -> errno map for open/read failures; the canonical converter
// is static in syscall.c.  An unresolvable exec target reads as ENOENT.
static int script_st_errno(int st)
{
	switch (st) {
	case ST_ACCESS:
	case ST_PERM:
		return -EACCES;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_IO:
		return -EIO;
	default:
		return -ENOENT;
	}
}

static char *script_strdup(const char *s)
{
	size_t len = kstrlen(s);
	char *d = kalloc(len + 1);
	if (!d)
		return NULL;
	mm_memcpy(d, s, len + 1);
	return d;
}

static int is_blank(char c)
{
	return c == ' ' || c == '\t';
}

int script_parse_shebang(const char *buf, size_t len, char *interp,
			 size_t interp_sz, char *optarg_buf, size_t optarg_sz,
			 int *has_arg)
{
	*has_arg = 0;
	if (len < 2 || buf[0] != '#' || buf[1] != '!')
		return 0;

	// Locate the end of the #! line.  A file shorter than the buffer may
	// legitimately lack a newline (EOF terminates the line); a full
	// buffer without one means the line was truncated and is rejected
	// rather than silently clipped.
	size_t end = 2;
	while (end < len && buf[end] != '\n')
		end++;
	if (end == len && len == SCRIPT_BUF_SIZE)
		return -ENOEXEC;

	// Right-trim trailing blanks and CR (covers "\r\n" line endings).
	while (end > 2 &&
	       (is_blank(buf[end - 1]) || buf[end - 1] == '\r'))
		end--;

	size_t p = 2;
	while (p < end && is_blank(buf[p]))
		p++;
	if (p == end)
		return -ENOEXEC; // "#!" with no interpreter

	size_t istart = p;
	while (p < end && !is_blank(buf[p]))
		p++;
	size_t ilen = p - istart;
	if (ilen >= interp_sz)
		return -ENAMETOOLONG;
	mm_memcpy(interp, buf + istart, ilen);
	interp[ilen] = '\0';

	while (p < end && is_blank(buf[p]))
		p++;
	if (p < end) {
		// Everything up to the (trimmed) end of line is ONE argument;
		// internal blanks are preserved.
		size_t alen = end - p;
		if (alen >= optarg_sz)
			alen = optarg_sz - 1; // unreachable: line < optarg_sz
		mm_memcpy(optarg_buf, buf + p, alen);
		optarg_buf[alen] = '\0';
		*has_arg = 1;
	}
	return 1;
}

// Read the first SCRIPT_BUF_SIZE bytes of path.  vfs_open enforces read
// permission for non-root callers and resolves relative paths against the
// task cwd - the same rules elf_exec_replace's own open applies later.
static int script_read_head(const char *path, char *buf, long *out_len)
{
	vfs_file_t *f = NULL;
	int ret = vfs_open(path, 0, &f);
	if (ret != ST_OK)
		return script_st_errno(ret);
	long n = vfs_read(f, buf, SCRIPT_BUF_SIZE);
	vfs_close(f);
	if (n < 0)
		return -EIO;
	*out_len = n;
	return 0;
}

int script_load_rewrite(char **kpath_io, char ***kargv_io, int depth)
{
	char head[SCRIPT_BUF_SIZE];
	char interp[VFS_MAX_PATH];
	char sarg[SCRIPT_BUF_SIZE];
	int has_arg = 0;
	long n = 0;

	int ret = script_read_head(*kpath_io, head, &n);
	if (ret < 0)
		return ret;

	ret = script_parse_shebang(head, (size_t)n, interp, sizeof(interp),
				   sarg, sizeof(sarg), &has_arg);
	if (ret <= 0)
		return ret;

	// Only a confirmed script counts against the recursion budget, so a
	// SCRIPT_MAX_DEPTH-deep chain ending in an ELF binary still works.
	if (depth >= SCRIPT_MAX_DEPTH)
		return -ELOOP;

	char **old = *kargv_io;
	int old_argc = 0;
	if (old)
		while (old[old_argc])
			old_argc++;
	int keep = old_argc > 0 ? old_argc - 1 : 0;
	int new_argc = 2 + has_arg + keep;
	if (new_argc > SCRIPT_MAX_ARGC)
		return -E2BIG;

	// Allocate everything new before mutating anything, so every failure
	// path leaves *kpath_io/*kargv_io untouched.
	char *new_path = script_strdup(interp);
	char *argv0 = script_strdup(interp);
	char *argv_opt = has_arg ? script_strdup(sarg) : NULL;
	char **newv = kalloc((size_t)(new_argc + 1) * sizeof(char *));
	if (!new_path || !argv0 || (has_arg && !argv_opt) || !newv) {
		if (new_path)
			kfree(new_path);
		if (argv0)
			kfree(argv0);
		if (argv_opt)
			kfree(argv_opt);
		if (newv)
			kfree(newv);
		return -ENOMEM;
	}

	// New argv layout: interpreter, optional argument,
	// this level's script path, then the old argv tail.  The old argv[0]
	// is dropped at every level.  The script-path slot takes ownership of
	// the old kpath allocation; old argv[1..] strings transfer as-is.
	int idx = 0;
	newv[idx++] = argv0;
	if (has_arg)
		newv[idx++] = argv_opt;
	newv[idx++] = *kpath_io;
	for (int i = 1; i < old_argc; i++)
		newv[idx++] = old[i];
	newv[idx] = NULL;

	if (old) {
		if (old[0])
			kfree(old[0]);
		kfree(old); // array only - tail strings were transferred
	}
	*kpath_io = new_path;
	*kargv_io = newv;
	return 1;
}

int script_check_stack_fit(char *const argv[], char *const envp[])
{
	// Mirror elf_setup_stack's layout arithmetic conservatively: argc
	// slot + argv/envp pointer arrays (incl. NULLs) + worst-case auxv,
	// plus all string bytes, each region 16-byte aligned.
	uint64_t ptrs = 8;
	uint64_t strs = 0;
	int argc = 0, envc = 0;

	if (argv)
		for (; argv[argc]; argc++)
			strs += kstrlen(argv[argc]) + 1;
	if (envp)
		for (; envp[envc]; envc++)
			strs += kstrlen(envp[envc]) + 1;
	ptrs += (uint64_t)(argc + 1) * 8 + (uint64_t)(envc + 1) * 8;
	ptrs += 16 * 16; // auxv entries (16 bytes each, worst case)

	uint64_t total = ((ptrs + 15) & ~15ULL) + ((strs + 15) & ~15ULL);
	if (total > PAGE_SIZE)
		return -E2BIG;
	return 0;
}
