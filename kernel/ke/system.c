// LikeOS-64 -- system-wide information and control.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/io/tty.h>
#include <kernel/ke/smp.h>
#include <kernel/hal/acpi.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/syscalls.h>

// Minimal uname struct (kernel-side)
typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
} k_utsname_t;

int64_t sys_gethostname(uint64_t name, uint64_t len)
{
	const char *host = net_get_hostname();
	size_t hlen = 0;
	while (host[hlen])
		hlen++;
	if (!validate_user_ptr(name, len))
		return -EFAULT;
	if (len < hlen + 1)
		return -EINVAL;
	if (copy_to_user((void *)name, host, hlen + 1) < 0) {
		return -EFAULT;
	}
	return 0;
}

int64_t sys_uname(uint64_t buf)
{
	if (!validate_user_ptr(buf, sizeof(k_utsname_t)))
		return -EFAULT;
	k_utsname_t u;
	mm_memset(&u, 0, sizeof(u));
	const char *sys = "LikeOS";
	const char *node = net_get_hostname();
#ifdef LIKEOS_VERSION
	const char *rel = LIKEOS_VERSION;
#else
	const char *rel = "0.2";
#endif
#ifdef BUILD_DATE
	const char *ver = "preempt-smp " BUILD_DATE;
#else
	const char *ver = "preempt-smp";
#endif
	const char *mach = "x86_64";
	// Copy strings (including null terminators), capped to field size
	for (int i = 0; sys[i] && i < 64; i++)
		u.sysname[i] = sys[i];
	for (int i = 0; node[i] && i < 64; i++)
		u.nodename[i] = node[i];
	for (int i = 0; rel[i] && i < 64; i++)
		u.release[i] = rel[i];
	for (int i = 0; ver[i] && i < 64; i++)
		u.version[i] = ver[i];
	for (int i = 0; mach[i] && i < 64; i++)
		u.machine[i] = mach[i];
	if (copy_to_user((void *)buf, &u, sizeof(u)) < 0) {
		return -EFAULT;
	}
	return 0;
}

// reboot() magic numbers and commands
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793 // 0x28121969
#define LINUX_REBOOT_MAGIC2A 85072278
#define LINUX_REBOOT_MAGIC2B 369367448
#define LINUX_REBOOT_MAGIC2C 537993216

#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON 0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF 0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART2 0xA1B2C3D4

static int g_cad_enabled = 0; // Ctrl-Alt-Del behaviour

int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd,
		   uint64_t arg)
{
	// Validate magic numbers
	if ((uint32_t)magic1 != LINUX_REBOOT_MAGIC1)
		return -EINVAL;

	uint32_t m2 = (uint32_t)magic2;
	if (m2 != LINUX_REBOOT_MAGIC2 && m2 != LINUX_REBOOT_MAGIC2A &&
	    m2 != LINUX_REBOOT_MAGIC2B && m2 != LINUX_REBOOT_MAGIC2C)
		return -EINVAL;

	// Halting, powering off or rebooting the machine is privileged: only a
	// process with an effective uid of 0 (root) may do it.
	if (!capable())
		return -EPERM;

	// Flush filesystems (pagecache + journal) before going down so a journalled
	// root isn't left dirty (which would force a replay on the next boot).
	switch ((uint32_t)cmd) {
	case LINUX_REBOOT_CMD_RESTART:
	case LINUX_REBOOT_CMD_HALT:
	case LINUX_REBOOT_CMD_POWER_OFF:
	case LINUX_REBOOT_CMD_RESTART2:
		sys_sync();
		break;
	default:
		break;
	}

	switch ((uint32_t)cmd) {
	case LINUX_REBOOT_CMD_RESTART:
		kprintf("[REBOOT] System is going down for reboot NOW!\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_reset();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_HALT:
		kprintf("[HALT] System halted.\n");
		__asm__ volatile("cli");
		smp_halt_others();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_POWER_OFF:
		kprintf("[POWEROFF] Power down.\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_poweroff();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_CAD_ON:
		g_cad_enabled = 1;
		return 0;

	case LINUX_REBOOT_CMD_CAD_OFF:
		g_cad_enabled = 0;
		return 0;

	case LINUX_REBOOT_CMD_RESTART2: {
		// arg is a pointer to a command string (ignored in our impl)
		kprintf("[REBOOT] System is going down for reboot NOW!\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_reset();
		for (;;)
			__asm__ volatile("hlt");
	}

	default:
		return -EINVAL;
	}
}

int64_t sys_getprocmaps(uint64_t pid, uint64_t info_ptr,
			uint64_t buf_ptr, uint64_t max_count)
{
	procmapinfo_t kinfo;
	procmap_t *kbuf = NULL;
	size_t buf_size = 0;
	uint64_t flags;
	int count = 0;
	int64_t ret;

	if (!validate_user_ptr(info_ptr, sizeof(procmapinfo_t)))
		return -EFAULT;
	if (max_count > 65536)
		max_count = 65536;
	if (max_count) {
		if (!validate_user_ptr(buf_ptr, max_count * sizeof(procmap_t)))
			return -EFAULT;
		buf_size = (size_t)max_count * sizeof(procmap_t);
		kbuf = (procmap_t *)kalloc(buf_size);
		if (!kbuf)
			return -ENOMEM;
		mm_memset(kbuf, 0, buf_size);
	}
	mm_memset(&kinfo, 0, sizeof(kinfo));

	spin_lock_irqsave(&g_task_list_lock, &flags);
	{
		task_t *t = sched_find_task_by_id_locked((uint32_t)pid);
		task_t *mm = t ? task_mm_owner(t) : NULL;
		int perm;

		if (!t || !mm) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			if (kbuf)
				kfree(kbuf);
			return -ESRCH;
		}
		/* Evaluated across the whole thread group rather than the one
		 * thread named.  What is being handed out is the layout of an
		 * address space every thread of the group shares, so the
		 * weakest thread must not be the way in -- credentials here are
		 * per-task, so they can differ between siblings (see cred.h). */
		perm = task_may_access(t, ACCESS_READ);
		if (perm != 0) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			if (kbuf)
				kfree(kbuf);
			return perm;
		}
		kinfo.pid = (int)t->id;
		kinfo.tgid = t->tgid;
		kinfo.brk_start = mm->brk_start;
		kinfo.brk = mm->brk;
		kinfo.mmap_base = mm->mmap_base;
		kinfo.capacity = mm->mmap_capacity;

		for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
			mmap_region_t *r = &mm->mmap_regions[i];

			if (!r->in_use)
				continue;
			kinfo.n_regions++;
			kinfo.total_bytes += r->length;
			if (kbuf && count < (int)max_count) {
				kbuf[count].start = r->start;
				kbuf[count].length = r->length;
				kbuf[count].prot = r->prot;
				kbuf[count].flags = r->flags;
				kbuf[count].offset = r->offset;
				kbuf[count].file_backed = r->file ? 1 : 0;
				kbuf[count].lazy = r->lazy ? 1 : 0;
				kbuf[count].device = r->device ? 1 : 0;
				count++;
			}
		}
	}
	spin_unlock_irqrestore(&g_task_list_lock, flags);

	ret = count;
	if (copy_to_user((void *)info_ptr, &kinfo, sizeof(kinfo)) < 0)
		ret = -EFAULT;
	else if (kbuf && count &&
		 copy_to_user((void *)buf_ptr, kbuf,
			      (size_t)count * sizeof(procmap_t)) < 0)
		ret = -EFAULT;
	if (kbuf)
		kfree(kbuf);
	return ret;
}

int64_t sys_getprocinfo(uint64_t buf_ptr, uint64_t max_count)
{
	if (max_count == 0)
		return 0;
	if (!validate_user_ptr(buf_ptr, max_count * sizeof(procinfo_t)))
		return -EFAULT;

	// Allocate a kernel-side buffer (limit to prevent DoS)
	if (max_count > 4096)
		max_count = 4096;
	size_t buf_size = max_count * sizeof(procinfo_t);
	procinfo_t *kbuf = (procinfo_t *)kalloc(buf_size);
	if (!kbuf)
		return -ENOMEM;
	mm_memset(kbuf, 0, buf_size);

	uint64_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;

	uint64_t flags;
	int count = 0;
	spin_lock_irqsave(&g_task_list_lock, &flags);

	// g_task_list_head is declared static in sched.c, but we can
	// iterate using sched_find_task_by_id or we use extern.
	// Actually we declared g_task_list_lock extern in sched.h,
	// but not g_task_list_head. Let's just use a different approach:
	// iterate IDs from 0 upward.
	// Actually, let's access the list directly. We need to declare it extern.
	// For now, use the approach of iterating via sched_find_task_by_id
	// which acquires its own lock... but we already hold the lock.
	// Better: we declared an extern iterator in the header or iterate by PID.

	// We'll iterate PIDs. Not ideal but safe. sched_find_task_by_id
	// acquires the lock internally, so we must NOT hold it here.
	spin_unlock_irqrestore(&g_task_list_lock, flags);

	// Iterate all possible PIDs (g_next_id is the next ID to assign)
	extern int g_next_id;
	int max_pid = g_next_id;

	for (int pid = 0; pid < max_pid && count < (int)max_count; pid++) {
		spin_lock_irqsave(&g_task_list_lock, &flags);
		task_t *t = sched_find_task_by_id_locked(pid);
		if (!t || sched_task_hidden(t)) {
			/* Skip empty slots and the kernel's swapper-class tasks
			 * (bootstrap + idle), which are not real processes. */
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			continue;
		}

		/* Two tiers of visibility.
		 *
		 * Everything a process listing needs -- pid, parentage, state,
		 * the program name, sizes -- is world-readable, because ps
		 * showing every user's processes is how a shared machine is
		 * meant to work, and none of it is private.
		 *
		 * The command line, the environment, the working directory and
		 * the kernel address a task is blocked at are NOT.  An
		 * environment routinely carries passwords, tokens and session
		 * keys; a command line carries arguments meant for one user;
		 * wchan is a kernel text address, which is an oracle for the
		 * kernel's layout.  Those follow the same rule as the address
		 * space itself (sys_getprocmaps), so one user cannot take them
		 * from another. */
		int may_read = (task_may_access(t, ACCESS_READ) == 0);

		procinfo_t *p = &kbuf[count];
		p->pid = t->id;
		p->ppid = sched_get_ppid(t);
		p->tgid = t->tgid;
		p->pgid = t->pgid;
		p->sid = t->sid;
		p->state = (int)t->state;
		p->nice = 0;
		p->nr_threads =
			t->group_leader ? t->group_leader->nr_threads : 1;
		p->on_cpu = t->on_cpu;
		p->exit_code = t->exit_code;
		/* Encode tty_nr consistently with ps/top:
         *   0        = no controlling terminal
         *   1        = console (g_console_tty.id == 1, is_pty == 0)
         *   2+       = pts/(tty_nr - 2)  (PTY slave id is 0-based, +2 avoids
         *              collision with 0="none" and 1="console") */
		if (!t->ctty)
			p->tty_nr = 0;
		else if (t->ctty->is_pty)
			p->tty_nr = t->ctty->id + 2;
		else
			p->tty_nr = t->ctty->id;
		p->is_kernel = (t->privilege == TASK_KERNEL) ? 1 : 0;
		p->start_tick = t->start_tick;
		p->utime_ticks = t->utime_ticks;
		p->stime_ticks = t->stime_ticks;
		/* Only meaningful while the task is actually asleep; a running
		 * task's last blocking site would be a stale answer.  Withheld
		 * from callers who may not read the process: it is a kernel
		 * text address. */
		p->wchan = (may_read && t->state == TASK_BLOCKED) ? t->wchan_rip :
								    0;

		// Real and effective credentials of the process.
		p->uid = (int)t->cred.uid;
		p->gid = (int)t->cred.gid;
		p->euid = (int)t->cred.euid;
		p->egid = (int)t->cred.egid;

		// VSZ: count pages mapped in user space (rough estimate)
		p->vsz = 0;
		p->rss = 0;
		if (t->privilege == TASK_USER) {
			/* Read through the mm owner, not the thread.  The
			 * bookkeeping lives on the thread-group leader and a
			 * thread's own copy is left empty, so asking the thread
			 * reported every non-leader as having no address space
			 * at all -- which is also what sys_getprocmaps does,
			 * and the two disagreed. */
			task_t *mm = task_mm_owner(t);

			// Estimate from brk and mmap
			if (mm->brk > mm->brk_start)
				p->vsz += (mm->brk - mm->brk_start);
			// User stack (assume 2MB)
			p->vsz += 2 * 1024 * 1024;
			// mmap regions
			for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
				if (mm->mmap_regions[i].in_use)
					p->vsz += mm->mmap_regions[i].length;
			}
			/* RSS: the pages actually resident, counted from the
			 * page tables -- not VSZ restated, which is what this
			 * used to report.  A process that returns physical
			 * memory but keeps its address space looked like an
			 * unbounded leak under the old number.
			 *
			 * KNOWN COST: this walks the page tables with
			 * g_task_list_lock held and interrupts off, for a time
			 * proportional to the process's resident set.  Moving
			 * it outside the lock needs something to keep the
			 * address space alive once the lock is dropped, and no
			 * such reference exists yet -- doing it without one
			 * would trade a long lock hold for a use-after-free on
			 * the page tables, which is worse.  Left here
			 * deliberately until address-space lifetime is
			 * refcounted. */
			p->rss = mm_count_resident_pages(mm->pml4);
		}

		// Copy comm
		for (int i = 0; i < 255 && t->comm[i]; i++)
			p->comm[i] = t->comm[i];
		p->comm[255] = '\0';

		/* Copy cmdline / environ / cwd only for a caller entitled to
		 * read this process.  Left as the empty string otherwise, which
		 * is what ps already shows for a task that has none -- no new
		 * failure mode, and nothing leaks by omission. */
		if (may_read) {
			// Copy cmdline
			for (int i = 0; i < 1023 && t->cmdline[i]; i++)
				p->cmdline[i] = t->cmdline[i];
			p->cmdline[1023] = '\0';

			// Copy environ
			for (int i = 0; i < 2047 && t->environ[i]; i++)
				p->environ[i] = t->environ[i];
			p->environ[2047] = '\0';

			// Copy cwd
			for (int i = 0; i < 255 && t->cwd[i]; i++)
				p->cwd[i] = t->cwd[i];
			p->cwd[255] = '\0';
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		count++;
	}

	// Copy to user space
	int err =
		copy_to_user((void *)buf_ptr, kbuf, count * sizeof(procinfo_t));
	kfree(kbuf);

	if (err)
		return err;
	return count;
}

// ============================================================================
// SYS_SYSINFO - Return system information (memory, uptime, load averages)
// ============================================================================
int64_t sys_sysinfo(uint64_t info_ptr)
{
	if (!validate_user_ptr(info_ptr, sizeof(k_sysinfo_t)))
		return -EFAULT;

	k_sysinfo_t info;
	kmemset(&info, 0, sizeof(info));

	// Uptime
	info.uptime = (long)timer_get_uptime();

	// Load averages
	sched_get_loadavg(info.loads);

	// Memory stats
	memory_stats_t mstats;
	mm_get_memory_stats(&mstats);
	info.totalram = mstats.total_memory;
	info.freeram = mstats.free_memory;
	info.sharedram = 0; // no tmpfs/shm accounting
	/* buff/cache, matching what free(1)/top expect the fields to mean:
	 *   bufferram — block/metadata buffers filesystem drivers reported
	 *               via mm_buffercache_account()
	 *   cached    — page cache plus the reclaimable entry caches
	 *               (inode + dentry caches, both LRU-evictable) */
	info.bufferram = mm_buffercache_bytes();
	info.totalswap = 0;
	info.freeswap = 0;
	info.procs = (unsigned short)sched_get_nr_procs();
	info.totalhigh = 0;
	info.freehigh = 0;
	info.mem_unit = 1; // byte granularity
	info.cached = mstats.pagecache_pages * PAGE_SIZE + icache_mem_bytes() +
		      dcache_mem_bytes();
	info.available = info.freeram + info.bufferram + info.cached;

	if (copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
		return -EFAULT;
	return 0;
}

// ============================================================================
// SYS_KLOGCTL - Kernel ring buffer operations (for dmesg)
// ============================================================================
int64_t sys_klogctl(uint64_t type, uint64_t bufp, uint64_t len)
{
	/* Reading or clearing the kernel log is privileged (dmesg is root-only). */
	if (!capable())
		return -EPERM;
	switch ((int)type) {
	case SYSLOG_ACTION_READ:
	case SYSLOG_ACTION_READ_ALL: {
		if (len == 0)
			return 0;
		if (!bufp || !validate_user_ptr(bufp, (size_t)len))
			return -EFAULT;
		int ksize = klog_size();
		int rlen = (int)len;
		if (rlen > ksize)
			rlen = ksize;
		// Allocate kernel temp buffer
		char *tmp = (char *)kalloc(rlen + 1);
		if (!tmp)
			return -ENOMEM;
		int got = klog_read(tmp, rlen);
		if (copy_to_user((void *)bufp, tmp, got) != 0) {
			kfree(tmp);
			return -EFAULT;
		}
		kfree(tmp);
		return got;
	}
	case SYSLOG_ACTION_READ_CLEAR: {
		if (len == 0)
			return 0;
		if (!bufp || !validate_user_ptr(bufp, (size_t)len))
			return -EFAULT;
		int ksize = klog_size();
		int rlen = (int)len;
		if (rlen > ksize)
			rlen = ksize;
		char *tmp = (char *)kalloc(rlen + 1);
		if (!tmp)
			return -ENOMEM;
		int got = klog_read_clear(tmp, rlen);
		if (copy_to_user((void *)bufp, tmp, got) != 0) {
			kfree(tmp);
			return -EFAULT;
		}
		kfree(tmp);
		return got;
	}
	case SYSLOG_ACTION_CLEAR:
		klog_clear();
		return 0;
	case SYSLOG_ACTION_SIZE_BUFFER:
		return klog_size();
	default:
		return -EINVAL;
	}
}

/* SYS_DEBUG_DUMP: root-only.  Emit the same diagnostic tables as the Ctrl+N /
 * Ctrl+D debug hotkeys (TCP connection table, AF_UNIX socket table, PTY table,
 * and the scheduler task list) to the active tty.  Lets userspace capture the
 * kernel state at a chosen moment — e.g. a watchdog that fires when an accept()
 * has hung — without needing a physical keypress.  All four dumps are lock-free
 * best-effort reads with no side effects. */
int64_t sys_debug_dump(void)
{
	if (!capable())
		return -EPERM;
	extern void tcp_dump_table(struct tty * tty);
	extern void unix_dump_sockets(struct tty * tty);
	extern void tty_dump_ptys(struct tty * tty);
	extern void sched_dump_tasks(struct tty * tty);
	tty_t *t = tty_get_active();
	tcp_dump_table(t);
	unix_dump_sockets(t);
	tty_dump_ptys(t);
	sched_dump_tasks(t);
	return 0;
}

int64_t sys_memstats(uint64_t a1, uint64_t a2)
{
	if (!a1)
		return -EFAULT;
	memory_stats_t stats;
	/* a2: also print who owns the allocated pages, to the kernel
	 * log.  It goes there rather than to the caller because it is
	 * a variable-length report meant to be read next to the rest
	 * of the boot output.
	 *
	 * Privileged, and checked before anything is reported: the
	 * page-owner report names the call sites and the address
	 * spaces of every process on the machine, and it lands in the
	 * kernel log, which is itself root-only to read (sys_klogctl).
	 * Handing an unprivileged caller the ability to write to it
	 * would also let anyone push the log's older contents out of
	 * the ring.  The plain statistics above stay open to everyone
	 * -- they are a system-wide total, the same thing free(1)
	 * prints. */
	if (a2 && !capable())
		return -EPERM;
	mm_get_memory_stats(&stats);
	if (copy_to_user((void *)a1, &stats, sizeof(stats)) != 0)
		return -EFAULT;
	if (a2) {
		mm_dump_page_owners();
		sched_dump_task_leaks();
	}
	return 0;
}

int64_t sys_sethostname(uint64_t a1, uint64_t a2)
{
	/* Setting the hostname is a system-wide change: privileged only. */
	if (!capable())
		return -EPERM;
	if (!validate_user_ptr(a1, 1))
		return -EFAULT;
	size_t len = (size_t)a2;
	if (len == 0 || len > 63)
		return -EINVAL;
	char khost[64];
	if (copy_from_user(khost, (const void *)a1, len) < 0)
		return -EFAULT;
	khost[len] = '\0';
	net_set_hostname(khost);
	return 0;
}
