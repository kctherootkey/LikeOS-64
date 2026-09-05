// LikeOS-64 -- /proc: processes and a few system facts, as files.
//
// The process listing itself lives behind SYS_GETPROCINFO; this tree
// carries the per-process paths programs open by name -- /proc/self/exe,
// /proc/self/fd/N, /proc/<pid>/maps, cmdline -- and the classic system
// files (uptime, meminfo, version, cpuinfo).
#include <kernel/fs/pseudofs.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/file.h>
#include <kernel/fs/devfs.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/cred.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/ke/pipe.h>
#include <kernel/ke/smp.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>

static struct pfs g_procfs;

extern int g_next_id;

static int parse_pid(const char *s, int *out)
{
	int v = 0;

	if (!*s)
		return 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return 0;
		v = v * 10 + (*s - '0');
		if (v > 1000000)
			return 0;
	}
	*out = v;
	return 1;
}

/* A process is listed when it is a thread-group leader the caller may see. */
static task_t *proc_task(int pid)
{
	task_t *t = sched_find_task_by_id(pid);

	if (!t || sched_task_hidden(t))
		return NULL;
	return t;
}

/* ---- /proc/<pid>/... files ---- */

static long show_cmdline(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);
	long p = 0;

	if (!t || task_may_access(t, ACCESS_READ) != 0)
		return 0;
	/* argv joined by spaces in the kernel; the file wants NUL
	 * separators. */
	for (const char *q = t->cmdline; *q; q++) {
		if (p < cap)
			buf[p] = (*q == ' ') ? 0 : *q;
		p++;
	}
	if (p < cap)
		buf[p] = 0;
	return p + 1;
}

static long show_comm(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);

	if (!t)
		return 0;
	return pfs_printf(buf, cap, 0, "%s\n", t->comm);
}

static const char *state_letter(task_t *t)
{
	switch (t->state) {
	case TASK_READY:
	case TASK_RUNNING:
		return "R";
	case TASK_BLOCKED:
		return "S";
	case TASK_STOPPED:
		return "T";
	case TASK_ZOMBIE:
		return "Z";
	default:
		return "?";
	}
}

static long show_stat(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);

	if (!t)
		return 0;
	uint64_t hz = timer_get_frequency();
	return pfs_printf(buf, cap, 0,
			  "%d (%s) %s %d %d %d 0 -1 0 0 0 0 0 %llu %llu 0 0 20 0 %d 0 %llu 0 0\n",
			  t->id, t->comm, state_letter(t), sched_get_ppid(t),
			  t->pgid, t->sid, (unsigned long long)t->utime_ticks,
			  (unsigned long long)t->stime_ticks,
			  t->group_leader ? t->group_leader->nr_threads : 1,
			  (unsigned long long)(t->start_tick / (hz ? hz : 100)));
}

static long show_status(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);
	long p = 0;

	if (!t)
		return 0;
	p = pfs_printf(buf, cap, p, "Name:\t%s\n", t->comm);
	p = pfs_printf(buf, cap, p, "State:\t%s\n", state_letter(t));
	p = pfs_printf(buf, cap, p, "Tgid:\t%d\n", t->tgid);
	p = pfs_printf(buf, cap, p, "Pid:\t%d\n", t->id);
	p = pfs_printf(buf, cap, p, "PPid:\t%d\n", sched_get_ppid(t));
	p = pfs_printf(buf, cap, p, "Uid:\t%u\t%u\t%u\t%u\n", t->cred.uid,
		       t->cred.euid, t->cred.suid, t->cred.fsuid);
	p = pfs_printf(buf, cap, p, "Gid:\t%u\t%u\t%u\t%u\n", t->cred.gid,
		       t->cred.egid, t->cred.sgid, t->cred.fsgid);
	p = pfs_printf(buf, cap, p, "Threads:\t%d\n",
		       t->group_leader ? t->group_leader->nr_threads : 1);
	return p;
}

static long show_maps(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);
	long p = 0;

	if (!t || task_may_access(t, ACCESS_READ) != 0)
		return 0;
	task_t *mm = task_mm_owner(t);
	if (!mm || !mm->mmap_regions)
		return 0;
	for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
		mmap_region_t *r = &mm->mmap_regions[i];

		if (!r->in_use)
			continue;
		char path[160] = "";
		if (r->file && r->file->at_path)
			ksnprintf(path, sizeof(path), "%s", r->file->at_path);
		p = pfs_printf(buf, cap, p, "%012llx-%012llx %c%c%c%c %08llx 00:00 0 %s\n",
			       (unsigned long long)r->start,
			       (unsigned long long)(r->start + r->length),
			       (r->prot & PROT_READ) ? 'r' : '-',
			       (r->prot & PROT_WRITE) ? 'w' : '-',
			       (r->prot & PROT_EXEC) ? 'x' : '-',
			       (r->flags & MAP_SHARED) ? 's' : 'p',
			       (unsigned long long)r->offset, path);
	}
	return p;
}

/* /proc/<pid>/statm: "size resident shared text lib data dt", in pages.
 * Resident is counted from the page tables the way ps does it (system.c),
 * and size is the estimate ps reports as VSZ.  The other five are not
 * tracked here and read as zero, as lib and dt do on the reference system
 * too.  WebKit's memoryFootprint() is what this was added for: it takes the
 * second field, and its relief logger subtracts the third. */
static long show_statm(struct pfs_node *n, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);
	uint64_t size = 0, resident;

	if (!t || task_may_access(t, ACCESS_READ) != 0)
		return 0;
	task_t *mm = task_mm_owner(t);
	if (!mm)
		return 0;
	if (mm->brk > mm->brk_start)
		size += mm->brk - mm->brk_start;
	size += 2 * 1024 * 1024; /* user stack, the figure system.c assumes */
	if (mm->mmap_regions) {
		for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
			if (mm->mmap_regions[i].in_use)
				size += mm->mmap_regions[i].length;
		}
	}
	/* The walk reads page-table pages, and the write side of mmap_lock
	 * is what frees those (munmap, exit): hold the read side across it. */
	mm_read_lock(&mm->mmap_lock);
	resident = mm_count_resident_pages(mm->pml4);
	mm_read_unlock(&mm->mmap_lock);
	return pfs_printf(buf, cap, 0, "%llu %llu 0 0 0 0 0\n",
			  (unsigned long long)(size / PAGE_SIZE),
			  (unsigned long long)resident);
}

/* /proc/<pid>/mem: the process's address space, read at the offset asked for.
 *
 * The same thing PTRACE_PEEKDATA gives one word at a time, as a file -- which
 * is the only practical way to read a whole mapping, and why the reference has
 * both.  A hundred megabytes of library text is twelve million PEEKs; here it
 * is a read().
 *
 * Read-only on purpose, and behind the same permission check every other file
 * in this directory uses: the contents of another process's memory are exactly
 * as private as the map that says where they are.
 *
 * No ptrace-stop is required.  A running process gives a torn answer, which is
 * the caller's problem to solve (by stopping it) and not a reason to refuse
 * the read -- the reference does not refuse it either. */
extern int64_t ptrace_xfer_mem(task_t *mm, uint64_t addr, void *kbuf,
			       size_t len, bool write);

/* /proc/<pid>/auxv -- the auxiliary vector the kernel pushed onto this
 * image's stack at exec.
 *
 * A debugger needs it before it can say anything useful about a
 * position-independent program.  It compares AT_ENTRY against e_entry in the
 * file on disk to work out the bias the image was loaded at, and follows
 * AT_PHDR to the program headers, PT_DYNAMIC and the dynamic linker's list of
 * shared objects.  Without it every address it computes is an offset from
 * nowhere: gdb assumes a bias of zero, resolves a libc symbol to its
 * unrelocated value, and fails to plant a breakpoint on an address no mapping
 * covers ("Cannot access memory at address 0x600").
 *
 * The kernel already recorded where the vector landed (task->auxv_addr /
 * auxv_len, set by elf_setup_stack) and already exposes it to PTRACE_GETAUXV.
 * This is the same data by the name a debugger actually looks for. */
static long show_auxv(struct pfs_node *n, uint64_t off, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);

	if (!t || task_may_access(t, ACCESS_READ) != 0)
		return -EACCES;
	task_t *mm = task_mm_owner(t);

	if (!mm || !mm->pml4)
		return -ESRCH;
	/* The leader's fields, not the named thread's: only the task that
	 * exec'd carries the vector.  See the same fix in PT_GETAUXV. */
	if (!mm->auxv_addr || !mm->auxv_len)
		return 0; /* never exec'd: an empty vector, not an error */
	if (cap <= 0 || off >= mm->auxv_len)
		return 0;

	uint64_t avail = mm->auxv_len - off;
	long want = (avail < (uint64_t)cap) ? (long)avail : cap;

	mm_read_lock(&mm->mmap_lock);
	int64_t r = ptrace_xfer_mem(mm, mm->auxv_addr + off, buf, (size_t)want,
				    false);
	mm_read_unlock(&mm->mmap_lock);
	return r == 0 ? want : -EIO;
}

static long show_mem(struct pfs_node *n, uint64_t off, char *buf, long cap)
{
	task_t *t = proc_task((int)n->arg2);

	if (!t || task_may_access(t, ACCESS_READ) != 0)
		return -EACCES;
	task_t *mm = task_mm_owner(t);

	if (!mm || !mm->pml4)
		return -ESRCH;
	if (cap <= 0)
		return 0;

	/* One page at a time into a kernel bounce, so a caller asking for a
	 * megabyte cannot pin a megabyte here, and so a hole in the middle of
	 * the request ends the read at the hole rather than failing all of
	 * it -- short reads are how a mapping's end is discovered. */
	char page[PAGE_SIZE];
	long done = 0;

	while (done < cap) {
		long chunk = cap - done;

		if (chunk > (long)sizeof(page))
			chunk = (long)sizeof(page);
		mm_read_lock(&mm->mmap_lock);
		int64_t r = ptrace_xfer_mem(mm, off + (uint64_t)done, page,
					    (size_t)chunk, false);
		mm_read_unlock(&mm->mmap_lock);
		if (r != 0)
			break;
		smap_disable();
		mm_memcpy(buf + done, page, (size_t)chunk);
		smap_enable();
		done += chunk;
	}
	return done ? done : -EIO;
}

/* /proc/<pid>/fd/<n>: symlink to what the descriptor refers to. */
static void fd_target(task_t *t, int fd, char *out, size_t cap)
{
	vfs_file_t *f = fdget(t, fd);

	if (!f) {
		if (task_fd_is_console(t, fd))
			ksnprintf(out, cap, "/dev/console");
		else
			out[0] = 0;
		return;
	}
	uintptr_t mk = (uintptr_t)f;
	if (mk >= 1 && mk <= 3) {
		ksnprintf(out, cap, "/dev/console");
	} else if (IS_SOCKET_FD(f) || unix_sock_is(f)) {
		ksnprintf(out, cap, "socket:[%d]", fd);
	} else if (IS_EPOLL_FD(f)) {
		ksnprintf(out, cap, "anon_inode:[eventpoll]");
	} else if (pipe_is_end(f)) {
		ksnprintf(out, cap, "pipe:[%d]", fd);
	} else if (devfs_fpath(f, out, cap) >= 0) {
		/* a /dev node or an anonymous device file */
	} else if (f->at_path) {
		ksnprintf(out, cap, "%s", f->at_path);
	} else {
		ksnprintf(out, cap, "anon_inode:[unknown]");
	}
	fdput(f);
}

static int fd_list(struct pfs_node *dir, unsigned index, char *name, long cap,
		   int *type)
{
	task_t *t = proc_task((int)dir->arg2);
	unsigned seen = 0;

	if (!t)
		return 0;
	for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
		if (!task_fds(t)[fd] && !task_fd_is_console(t, fd))
			continue;
		if (seen++ == index) {
			ksnprintf(name, (size_t)cap, "%d", fd);
			*type = DT_LNK;
			return 1;
		}
	}
	return 0;
}

static struct pfs_node *fd_lookup(struct pfs_node *dir, const char *name)
{
	task_t *t = proc_task((int)dir->arg2);
	int fd;

	if (!t || !parse_pid(name, &fd) || fd >= TASK_MAX_FDS)
		return NULL;
	if (!task_fds(t)[fd] && !task_fd_is_console(t, fd))
		return NULL;
	if (task_may_access(t, ACCESS_READ) != 0)
		return NULL;
	struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
	if (!n)
		return NULL;
	fd_target(t, fd, n->link, sizeof(n->link));
	return n;
}

/* /proc/<pid>: a transient directory with static-looking children made
 * on lookup. */
static int pid_list(struct pfs_node *dir, unsigned index, char *name,
		    long cap, int *type)
{
	static const char *const names[] = { "cmdline", "comm",	 "stat",
					     "status",	"maps",	 "mem",
					     "auxv",	"exe",	 "cwd",
					     "fd",	"statm" };
	(void)dir;
	if (index >= 11)
		return 0;
	ksnprintf(name, (size_t)cap, "%s", names[index]);
	*type = index == 9 ? DT_DIR : (index >= 7 ? DT_LNK : DT_REG);
	return 1;
}

static struct pfs_node *pid_lookup(struct pfs_node *dir, const char *name)
{
	int pid = (int)dir->arg2;
	task_t *t = proc_task(pid);

	if (!t)
		return NULL;
	struct pfs_node *n;
	if (kstrcmp(name, "fd") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_DIR);
		if (n)
			pfs_set_dynamic(n, fd_list, fd_lookup);
	} else if (kstrcmp(name, "exe") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
		if (n) {
			/* argv[0] is the best record of the image there is. */
			size_t k = 0;
			while (t->cmdline[k] && t->cmdline[k] != ' ' &&
			       k < sizeof(n->link) - 1) {
				n->link[k] = t->cmdline[k];
				k++;
			}
			n->link[k] = 0;
		}
	} else if (kstrcmp(name, "cwd") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
		if (n)
			ksnprintf(n->link, sizeof(n->link), "%s",
				  t->cwd[0] ? t->cwd : "/");
	} else if (kstrcmp(name, "cmdline") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_cmdline;
	} else if (kstrcmp(name, "comm") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_comm;
	} else if (kstrcmp(name, "stat") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_stat;
	} else if (kstrcmp(name, "status") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_status;
	} else if (kstrcmp(name, "maps") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_maps;
	} else if (kstrcmp(name, "statm") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n)
			n->show = show_statm;
	} else if (kstrcmp(name, "mem") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n) {
			n->read_at = show_mem;
			n->mode = 0400;
		}
	} else if (kstrcmp(name, "auxv") == 0) {
		n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
		if (n) {
			n->read_at = show_auxv;
			n->mode = 0400;
		}
	} else {
		return NULL;
	}
	if (n)
		n->arg2 = (uint64_t)pid;
	return n;
}

/* /proc root: dynamic pid directories + "self". */
static int root_list(struct pfs_node *dir, unsigned index, char *name,
		     long cap, int *type)
{
	unsigned seen = 0;
	(void)dir;

	for (int pid = 1; pid < g_next_id; pid++) {
		task_t *t = sched_find_task_by_id(pid);
		if (!t || sched_task_hidden(t) || t->tgid != t->id)
			continue;
		if (seen++ == index) {
			ksnprintf(name, (size_t)cap, "%d", pid);
			*type = DT_DIR;
			return 1;
		}
	}
	return 0;
}

static struct pfs_node *root_lookup(struct pfs_node *dir, const char *name)
{
	int pid;

	if (kstrcmp(name, "self") == 0 || kstrcmp(name, "thread-self") == 0) {
		task_t *cur = sched_current();
		struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
		if (n && cur)
			ksnprintf(n->link, sizeof(n->link), "%d",
				  kstrcmp(name, "self") == 0 ? cur->tgid : cur->id);
		return n;
	}
	if (!parse_pid(name, &pid) || !proc_task(pid))
		return NULL;
	struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_DIR);
	if (!n)
		return NULL;
	n->arg2 = (uint64_t)pid;
	pfs_set_dynamic(n, pid_list, pid_lookup);
	return n;
}

/* ---- system files ---- */

static long show_uptime(struct pfs_node *n, char *buf, long cap)
{
	uint64_t us = timer_get_precise_us();
	(void)n;
	return pfs_printf(buf, cap, 0, "%llu.%02llu %llu.%02llu\n",
			  (unsigned long long)(us / 1000000),
			  (unsigned long long)((us / 10000) % 100),
			  (unsigned long long)(us / 1000000),
			  (unsigned long long)((us / 10000) % 100));
}

static long show_version(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0, "LikeOS version %s (%s)\n",
			  LIKEOS_VERSION, BUILD_DATE);
}

static long show_meminfo(struct pfs_node *n, char *buf, long cap)
{
	memory_stats_t ms;
	long p = 0;
	(void)n;

	mm_get_memory_stats(&ms);
	/* Available is what an allocation can still get: the free pages plus
	 * the page cache, which the fault path drops on demand
	 * (mm_alloc_page_for_fault).  It used to equal MemFree, which with a
	 * warm cache made most of RAM look spoken for to anyone deciding when
	 * to shed memory -- WebKit's pressure monitor reads this line. */
	uint64_t cached = ms.pagecache_pages * PAGE_SIZE;
	uint64_t avail = ms.free_memory + cached;
	if (avail > ms.total_memory)
		avail = ms.total_memory;
	p = pfs_printf(buf, cap, p, "MemTotal:       %10llu kB\n",
		       (unsigned long long)(ms.total_memory / 1024));
	p = pfs_printf(buf, cap, p, "MemFree:        %10llu kB\n",
		       (unsigned long long)(ms.free_memory / 1024));
	p = pfs_printf(buf, cap, p, "MemAvailable:   %10llu kB\n",
		       (unsigned long long)(avail / 1024));
	p = pfs_printf(buf, cap, p, "Cached:         %10llu kB\n",
		       (unsigned long long)(cached / 1024));
	p = pfs_printf(buf, cap, p, "SwapTotal:               0 kB\n");
	p = pfs_printf(buf, cap, p, "SwapFree:                0 kB\n");
	return p;
}

static long show_cpuinfo(struct pfs_node *n, char *buf, long cap)
{
	long p = 0;
	uint32_t a, b, c, d;
	char vendor[13];
	(void)n;

	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
	mm_memcpy(vendor, &b, 4);
	mm_memcpy(vendor + 4, &d, 4);
	mm_memcpy(vendor + 8, &c, 4);
	vendor[12] = 0;
	int ncpu = (int)smp_get_cpu_count();
	if (ncpu < 1)
		ncpu = 1;
	for (int i = 0; i < ncpu; i++) {
		p = pfs_printf(buf, cap, p, "processor\t: %d\n", i);
		p = pfs_printf(buf, cap, p, "vendor_id\t: %s\n", vendor);
		p = pfs_printf(buf, cap, p, "cpu MHz\t\t: %llu\n",
			       (unsigned long long)(lapic_get_tsc_freq() / 1000000ULL));
		p = pfs_printf(buf, cap, p, "\n");
	}
	return p;
}

static long show_mounts(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0,
			  "/dev/root / ext4 rw 0 0\ndevfs /dev devfs rw 0 0\nsysfs /sys sysfs ro 0 0\nproc /proc proc ro 0 0\n");
}

void procfs_init(void)
{
	pfs_init(&g_procfs, "/proc");
	pfs_set_dynamic(&g_procfs.root, root_list, root_lookup);
	pfs_add_file(&g_procfs, "uptime", show_uptime, NULL, 0);
	pfs_add_file(&g_procfs, "version", show_version, NULL, 0);
	pfs_add_file(&g_procfs, "meminfo", show_meminfo, NULL, 0);
	pfs_add_file(&g_procfs, "cpuinfo", show_cpuinfo, NULL, 0);
	pfs_add_file(&g_procfs, "mounts", show_mounts, NULL, 0);
	vfs_register_mount("/proc", pfs_ops(&g_procfs));
}
