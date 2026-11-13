// LikeOS - the device major registry (chrdev.h)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/chrdev.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>

struct chrdev_ent {
	uint32_t major;
	uint32_t refs; /* 0: free slot */
	int is_blk;
	char name[CHRDEV_NAME_LEN];
};

/* The built-in majors, in the order /proc/devices lists them. */
static const struct {
	uint32_t major;
	int is_blk;
	const char *name;
} g_builtin[] = {
	{ 1, 0, "mem" },	  { 4, 0, "/dev/vc/0" },     { 4, 0, "tty" },
	{ 5, 0, "/dev/tty" },	  { 5, 0, "/dev/console" },  { 5, 0, "/dev/ptmx" },
	{ 13, 0, "input" },	  { 14, 0, "sound" },	     { 29, 0, "fb" },
	{ 136, 0, "pts" },	  { 226, 0, "drm" },	     { 8, 1, "sd" },
	{ 11, 1, "sr" },	  { 65, 1, "sd" },	     { 66, 1, "sd" },
	{ 67, 1, "sd" },	  { 68, 1, "sd" },	     { 69, 1, "sd" },
	{ 70, 1, "sd" },	  { 71, 1, "sd" },	     { 259, 1, "blkext" },
};
#define NR_BUILTIN ((int)(sizeof(g_builtin) / sizeof(g_builtin[0])))

static spinlock_t g_chrdev_lock = SPINLOCK_INIT("chrdev");
static struct chrdev_ent g_dyn[CHRDEV_MAX_DYNAMIC];

static void chrdev_copy_name(char *dst, const char *src)
{
	int i = 0;

	for (; src && src[i] && i < CHRDEV_NAME_LEN - 1; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

static int chrdev_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Who owns major `m' of this kind?  Lock held. */
static const char *chrdev_owner_locked(uint32_t m, int is_blk)
{
	for (int i = 0; i < NR_BUILTIN; i++)
		if (g_builtin[i].major == m && g_builtin[i].is_blk == is_blk)
			return g_builtin[i].name;
	for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++)
		if (g_dyn[i].refs && g_dyn[i].major == m &&
		    g_dyn[i].is_blk == is_blk)
			return g_dyn[i].name;
	return NULL;
}

static int chrdev_pick_dynamic_locked(void)
{
	for (uint32_t m = CHRDEV_DYN_FIRST; m >= CHRDEV_DYN_LAST; m--)
		if (!chrdev_owner_locked(m, 0))
			return (int)m;
	for (uint32_t m = CHRDEV_DYN_EXT_FIRST; m >= CHRDEV_DYN_EXT_LAST; m--)
		if (!chrdev_owner_locked(m, 0))
			return (int)m;
	return -ENOSPC;
}

static int major_register(uint32_t major, const char *name, int is_blk)
{
	uint64_t fl;
	int r, slot = -1;

	if (!name || !name[0] || major > KDEV_MAJOR_MAX)
		return -EINVAL;
	if (is_blk && major == 0)
		return -EINVAL; /* block majors are fixed (259 is dynamic by minor) */
	spin_lock_irqsave(&g_chrdev_lock, &fl);
	if (major == 0) {
		r = chrdev_pick_dynamic_locked();
		if (r < 0)
			goto out;
		major = (uint32_t)r;
	} else {
		const char *owner = chrdev_owner_locked(major, is_blk);

		if (owner && !chrdev_streq(owner, name)) {
			r = -EBUSY;
			goto out;
		}
		for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++) {
			if (g_dyn[i].refs && g_dyn[i].major == major &&
			    g_dyn[i].is_blk == is_blk &&
			    chrdev_streq(g_dyn[i].name, name)) {
				g_dyn[i].refs++;
				r = (int)major;
				goto out;
			}
		}
		if (owner) {
			/* A built-in name re-registered: nothing to record. */
			r = (int)major;
			goto out;
		}
	}
	for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++) {
		if (!g_dyn[i].refs) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		r = -ENOSPC;
		goto out;
	}
	g_dyn[slot].major = major;
	g_dyn[slot].is_blk = is_blk;
	g_dyn[slot].refs = 1;
	chrdev_copy_name(g_dyn[slot].name, name);
	r = (int)major;
out:
	spin_unlock_irqrestore(&g_chrdev_lock, fl);
	return r;
}

static void major_unregister(uint32_t major, const char *name, int is_blk)
{
	uint64_t fl;
	int found = 0;

	spin_lock_irqsave(&g_chrdev_lock, &fl);
	for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++) {
		if (g_dyn[i].refs && g_dyn[i].major == major &&
		    g_dyn[i].is_blk == is_blk &&
		    (!name || chrdev_streq(g_dyn[i].name, name))) {
			g_dyn[i].refs--;
			found = 1;
			break;
		}
	}
	if (!found)
		for (int i = 0; i < NR_BUILTIN; i++)
			if (g_builtin[i].major == major &&
			    g_builtin[i].is_blk == is_blk)
				found = 1;
	spin_unlock_irqrestore(&g_chrdev_lock, fl);
	WARN(!found, "unregistering %s major %u (%s) that is not registered",
	     is_blk ? "block" : "char", major, name ? name : "?");
}

int chrdev_register(uint32_t major, const char *name)
{
	return major_register(major, name, 0);
}

void chrdev_unregister(uint32_t major, const char *name)
{
	major_unregister(major, name, 0);
}

int blkdev_major_register(uint32_t major, const char *name)
{
	return major_register(major, name, 1);
}

void blkdev_major_unregister(uint32_t major, const char *name)
{
	major_unregister(major, name, 1);
}

static const char *major_name(uint32_t major, int is_blk)
{
	uint64_t fl;
	const char *n;

	spin_lock_irqsave(&g_chrdev_lock, &fl);
	n = chrdev_owner_locked(major, is_blk);
	spin_unlock_irqrestore(&g_chrdev_lock, fl);
	return n;
}

const char *chrdev_major_name(uint32_t major)
{
	return major_name(major, 0);
}

const char *blkdev_major_name(uint32_t major)
{
	return major_name(major, 1);
}

/* Print every registration of one kind in ascending major order (the
 * built-in entries keep their table order among equal majors). */
static long chrdev_show_kind(char *buf, long cap, long p, int is_blk)
{
	uint32_t last = 0;

	for (;;) {
		uint32_t next = 0xffffffffu;
		uint64_t fl;

		spin_lock_irqsave(&g_chrdev_lock, &fl);
		for (int i = 0; i < NR_BUILTIN; i++)
			if (g_builtin[i].is_blk == is_blk &&
			    g_builtin[i].major > last && g_builtin[i].major < next)
				next = g_builtin[i].major;
		for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++)
			if (g_dyn[i].refs && g_dyn[i].is_blk == is_blk &&
			    g_dyn[i].major > last && g_dyn[i].major < next)
				next = g_dyn[i].major;
		spin_unlock_irqrestore(&g_chrdev_lock, fl);
		if (next == 0xffffffffu)
			return p;
		for (int i = 0; i < NR_BUILTIN; i++)
			if (g_builtin[i].is_blk == is_blk &&
			    g_builtin[i].major == next)
				p = pfs_printf(buf, cap, p, "%3u %s\n", next,
					       g_builtin[i].name);
		spin_lock_irqsave(&g_chrdev_lock, &fl);
		for (int i = 0; i < CHRDEV_MAX_DYNAMIC; i++) {
			char nm[CHRDEV_NAME_LEN];

			if (!g_dyn[i].refs || g_dyn[i].is_blk != is_blk ||
			    g_dyn[i].major != next)
				continue;
			chrdev_copy_name(nm, g_dyn[i].name);
			spin_unlock_irqrestore(&g_chrdev_lock, fl);
			p = pfs_printf(buf, cap, p, "%3u %s\n", next, nm);
			spin_lock_irqsave(&g_chrdev_lock, &fl);
		}
		spin_unlock_irqrestore(&g_chrdev_lock, fl);
		last = next;
	}
}

long chrdev_proc_devices_show(struct pfs_node *n, char *buf, long cap)
{
	long p;

	(void)n;
	p = pfs_printf(buf, cap, 0, "Character devices:\n");
	p = chrdev_show_kind(buf, cap, p, 0);
	p = pfs_printf(buf, cap, p, "\nBlock devices:\n");
	return chrdev_show_kind(buf, cap, p, 1);
}
