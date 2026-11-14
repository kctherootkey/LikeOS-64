// LikeOS -- /dev/kmsg: a program's lines into the kernel log.
//
// Copyright (C) 2026 The LikeOS Project

/*
 * Writing a line to /dev/kmsg puts it into the kernel log -- what dmesg
 * shows, and what a kernel built to log over a serial port sends there --
 * prefixed with the writer's name and process id: "[installer:212] text".
 * That is how a program with no console of its own (the installer, init
 * before the console is up) leaves a trace that survives it.
 *
 * Only the privileged user may write (the node is 0600 root:root, and the
 * open checks again): anything written lands in the log of the whole
 * system.  A write may carry several lines; each is logged on its own, an
 * optional "<N>" priority prefix is dropped, a trailing newline is not
 * repeated, and at most KMSG_RECORD_MAX bytes of a write are taken.  A
 * process that writes more than KMSG_RATE lines in a second has the rest
 * of that second's lines dropped (and is told how many, once the second
 * is over), so a runaway logger cannot flood the log.  Reading is not
 * offered: dmesg reads the log through klogctl().
 */

#include <kernel/fs/vfs.h>
#include <kernel/fs/device.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/cred.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define KMSG_RECORD_MAX 1024 /* bytes of one write that are logged */
#define KMSG_RATE 100 /* lines per process per second */
#define KMSG_RATE_SLOTS 16

/* Per-process rate state, a small table reused round robin. */
static struct {
	uint32_t pid;
	uint64_t second; /* uptime second the count is for */
	unsigned lines;
	unsigned dropped;
} g_kmsg_rate[KMSG_RATE_SLOTS];
static unsigned g_kmsg_next;
static spinlock_t g_kmsg_lock = SPINLOCK_INIT("kmsg");

/* May `pid' log one more line now?  Reports, once, how many lines a
 * process lost in its previous second. */
static int kmsg_allow(uint32_t pid, const char *comm)
{
	uint64_t now = timer_get_uptime();
	uint64_t fl;
	unsigned lost = 0;
	int ok, slot = -1;

	spin_lock_irqsave(&g_kmsg_lock, &fl);
	for (int i = 0; i < KMSG_RATE_SLOTS; i++)
		if (g_kmsg_rate[i].pid == pid) {
			slot = i;
			break;
		}
	if (slot < 0) {
		slot = (int)(g_kmsg_next++ % KMSG_RATE_SLOTS);
		g_kmsg_rate[slot].pid = pid;
		g_kmsg_rate[slot].second = now;
		g_kmsg_rate[slot].lines = 0;
		g_kmsg_rate[slot].dropped = 0;
	}
	if (g_kmsg_rate[slot].second != now) {
		lost = g_kmsg_rate[slot].dropped;
		g_kmsg_rate[slot].second = now;
		g_kmsg_rate[slot].lines = 0;
		g_kmsg_rate[slot].dropped = 0;
	}
	ok = g_kmsg_rate[slot].lines < KMSG_RATE;
	if (ok)
		g_kmsg_rate[slot].lines++;
	else
		g_kmsg_rate[slot].dropped++;
	spin_unlock_irqrestore(&g_kmsg_lock, fl);
	if (lost)
		kprintf("[%s:%u] (%u lines dropped: more than %u a second)\n",
			comm, pid, lost, KMSG_RATE);
	return ok;
}

static int kmsg_open(struct devfs_node *node, vfs_file_t *file, int flags,
		     struct task *cur)
{
	(void)node;
	(void)file;
	(void)flags;
	(void)cur;
	return capable() ? 0 : -EPERM;
}

static long kmsg_read(vfs_file_t *file, void *buf, long bytes, int nonblock)
{
	(void)file;
	(void)buf;
	(void)bytes;
	(void)nonblock;
	return -EINVAL;
}

static void kmsg_log_line(const char *comm, uint32_t pid, char *line,
			  unsigned len)
{
	unsigned s = 0;

	/* "<N>" -- a priority, as the conventional writers put it. */
	if (len >= 3 && line[0] == '<') {
		unsigned k = 1;
		while (k < len && k < 5 && line[k] >= '0' && line[k] <= '9')
			k++;
		if (k > 1 && k < len && line[k] == '>')
			s = k + 1;
	}
	while (len > s && (line[len - 1] == '\r' || line[len - 1] == '\n'))
		len--;
	if (len <= s)
		return;
	line[len] = '\0';
	if (!kmsg_allow(pid, comm))
		return;
	kprintf("[%s:%u] %s\n", comm, pid, line + s);
}

static long kmsg_write(vfs_file_t *file, const void *buf, long bytes,
		       int nonblock)
{
	task_t *cur = sched_current();
	char *kbuf;
	long take;

	(void)file;
	(void)nonblock;
	if (bytes < 0)
		return -EINVAL;
	if (bytes == 0)
		return 0;
	if (!capable())
		return -EPERM;
	take = bytes > KMSG_RECORD_MAX ? KMSG_RECORD_MAX : bytes;
	kbuf = kalloc((unsigned long)take + 1);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, buf, (size_t)take) != 0) {
		kfree(kbuf);
		return -EFAULT;
	}
	kbuf[take] = '\0';

	char comm[32];
	uint32_t pid = 0;
	unsigned i = 0;
	if (cur) {
		pid = (uint32_t)cur->tgid;
		for (; cur->comm[i] && i < sizeof(comm) - 1; i++)
			comm[i] = cur->comm[i];
	}
	comm[i] = '\0';

	/* One log line per line written. */
	long start = 0;
	for (long k = 0; k <= take; k++) {
		if (k == take || kbuf[k] == '\n' || kbuf[k] == '\0') {
			kmsg_log_line(comm, pid, kbuf + start,
				      (unsigned)(k - start));
			start = k + 1;
		}
	}
	kfree(kbuf);
	/* The whole write is taken, as a log takes it: what went beyond a
	 * record's length is cut, not left for a retry. */
	return bytes;
}

static const struct device_ops g_kmsg_ops = {
	.open = kmsg_open,
	.read = kmsg_read,
	.write = kmsg_write,
};

static struct devfs_node g_kmsg_node = {
	.path = "/dev/kmsg",
	.mode = 0600,
	.uid = 0,
	.gid = 0,
	.major = 1,
	.minor = 11,
	.ops = &g_kmsg_ops,
};

void kmsg_init(void)
{
	int r = device_register(&g_kmsg_node);

	if (r)
		kprintf("kmsg: /dev/kmsg not registered (%d)\n", r);
}
