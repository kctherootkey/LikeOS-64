// devd: the device table.
//
// Built from /sys/block (one directory per disk, one subdirectory per
// partition; device/type 5 names an optical drive, ro a write-protected
// medium), /dev/disk/by-id (usb-* links name USB storage), the partition
// tables of built-in disks (firmware and recovery partitions) and the mount
// table.  A rescan builds a complete new table, carries over what is known
// about devices that did not change (probe results, the ejected mark),
// probes the rest, and sends ADDED / REMOVED / CHANGED events for every
// difference -- so the order the kernel's events arrive in does not matter:
// each one only says "look again".
//
// Copyright (C) 2026 The LikeOS Project

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "devd.h"

struct devtab g_tab;
static struct devtab g_new;

#define MAX_PARTS 64
#define MAX_USB_NAMES 256
#define MAX_MOUNTS 512

/* Disks the kernel's events called USB (until they disappear). */
static char g_ev_usb[16][DEVD_NAME_MAX];

static char g_usb_names[MAX_USB_NAMES][DEVD_NAME_MAX];
static int g_nusb;

static struct mnt_ent g_mnts[MAX_MOUNTS];

struct dev *tab_find(const char *name)
{
	for (int i = 0; i < g_tab.n; i++)
		if (strcmp(g_tab.d[i].name, name) == 0)
			return &g_tab.d[i];
	return NULL;
}

int dev_is_volume(const struct dev *d)
{
	return d->kind == DEV_PART || d->nparts == 0;
}

static const char *state_name(int s)
{
	return s == DS_FORMATTING ? "formatting" : s == DS_BUSY ? "busy" :
								  "ready";
}

long dev_format_line(struct devp_line *l, const char *prefix,
		     const struct dev *d)
{
	devp_line_begin(l, prefix);
	devp_line_kv(l, "name", d->name);
	devp_line_kv(l, "disk", d->disk);
	devp_line_kv(l, "kind", d->kind == DEV_DISK ? "disk" : "partition");
	devp_line_kv_u64(l, "size", d->size);
	devp_line_kv_int(l, "removable", d->removable ? 1 : 0);
	devp_line_kv_int(l, "usb", d->usb ? 1 : 0);
	devp_line_kv_int(l, "system", d->system ? 1 : 0);
	devp_line_kv(l, "fstype", d->pr.fstype[0] ? d->pr.fstype : "unknown");
	devp_line_kv_int(l, "supported", d->supported ? 1 : 0);
	devp_line_kv(l, "label", d->pr.label);
	devp_line_kv(l, "uuid", d->pr.uuid);
	devp_line_kv(l, "vendor", d->vendor);
	devp_line_kv(l, "model", d->model);
	devp_line_kv(l, "mounted", d->mounted);
	devp_line_kv_int(l, "mountuid", d->devd_mount ? d->mountuid : -1);
	devp_line_kv_int(l, "ejected", d->ejected ? 1 : 0);
	devp_line_kv(l, "state", state_name(d->state));
	/* added later: at the end, so older clients read the rest as
	 * before */
	devp_line_kv_int(l, "readonly", d->readonly ? 1 : 0);
	devp_line_kv_int(l, "optical", d->optical ? 1 : 0);
	devp_line_kv_int(l, "internal", d->internal ? 1 : 0);
	return devp_line_end(l);
}

/* ---- USB ------------------------------------------------------------------------- */

void tab_note_usb(const char *name)
{
	int free_slot = -1;

	if (!devname_ok(name))
		return;
	for (int i = 0; i < 16; i++) {
		if (strcmp(g_ev_usb[i], name) == 0)
			return;
		if (!g_ev_usb[i][0] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0)
		strlcpy_d(g_ev_usb[free_slot], name, DEVD_NAME_MAX);
}

static int ev_usb_has(const char *name)
{
	for (int i = 0; i < 16; i++)
		if (g_ev_usb[i][0] && strcmp(g_ev_usb[i], name) == 0)
			return 1;
	return 0;
}

/* The device names the usb-* links of /dev/disk/by-id point at. */
static void read_usb_links(void)
{
	DIR *dir = opendir("/dev/disk/by-id");
	struct dirent *e;

	g_nusb = 0;
	if (!dir)
		return;
	while ((e = readdir(dir)) && g_nusb < MAX_USB_NAMES) {
		char path[DEVD_PATH_MAX], target[DEVD_PATH_MAX];
		const char *base;
		ssize_t n;

		if (strncmp(e->d_name, "usb-", 4) != 0)
			continue;
		if (snprintf(path, sizeof(path), "/dev/disk/by-id/%s",
			     e->d_name) >= (int)sizeof(path))
			continue;
		n = readlink(path, target, sizeof(target) - 1);
		if (n <= 0)
			continue;
		target[n] = '\0';
		base = strrchr(target, '/');
		base = base ? base + 1 : target;
		if (devname_ok(base))
			strlcpy_d(g_usb_names[g_nusb++], base, DEVD_NAME_MAX);
	}
	closedir(dir);
}

static int usb_link_has(const char *name)
{
	for (int i = 0; i < g_nusb; i++)
		if (strcmp(g_usb_names[i], name) == 0)
			return 1;
	return 0;
}

/* ---- sysfs ----------------------------------------------------------------------- */

static int parse_devnum(const char *s, uint32_t *maj, uint32_t *min)
{
	char tmp[32];
	char *colon;
	uint64_t a, b;

	if (strlcpy_d(tmp, s, sizeof(tmp)) >= sizeof(tmp))
		return -1;
	colon = strchr(tmp, ':');
	if (!colon)
		return -1;
	*colon = '\0';
	if (devp_parse_u64(tmp, 0xFFFFFFFFu, &a) ||
	    devp_parse_u64(colon + 1, 0xFFFFFFFFu, &b))
		return -1;
	*maj = (uint32_t)a;
	*min = (uint32_t)b;
	return 0;
}

static int sys_u64(const char *path, uint64_t *v)
{
	char buf[32];

	if (read_attr(path, buf, sizeof(buf)) <= 0)
		return -1;
	return devp_parse_u64(buf, UINT64_MAX, v);
}

static void sys_ident(const char *path, char *out, size_t cap)
{
	char buf[128];
	long n = read_attr(path, buf, sizeof(buf));

	out[0] = '\0';
	if (n > 0)
		label_display(out, cap, (const uint8_t *)buf, (size_t)n);
}

static uint64_t fnv(uint64_t h, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		h ^= (v >> (i * 8)) & 0xFF;
		h *= 1099511628211ULL;
	}
	return h;
}

/* The same device in the previous table: name and device numbers.  The
 * disk sequence number is not part of it -- it changes with every partition
 * table re-read, and a drive whose table was re-read (after a format, say)
 * has not been unplugged.  It decides what is known about the medium. */
static struct dev *old_find(const char *name, uint32_t maj, uint32_t min)
{
	for (int i = 0; i < g_tab.n; i++) {
		struct dev *o = &g_tab.d[i];
		if (o->major == maj && o->minor == min &&
		    strcmp(o->name, name) == 0)
			return o;
	}
	return NULL;
}

static void probe_one(struct dev *d)
{
	char path[DEVD_PATH_MAX];

	memset(&d->pr, 0, sizeof(d->pr));
	if (d->kind == DEV_DISK && d->nparts > 0) {
		strlcpy_d(d->pr.fstype, "none", sizeof(d->pr.fstype));
	} else if (d->size == 0) {
		strlcpy_d(d->pr.fstype, "none", sizeof(d->pr.fstype));
	} else {
		snprintf(path, sizeof(path), "/dev/%s", d->name);
		if (probe_device(path, &d->pr) < 0)
			strlcpy_d(d->pr.fstype, "unknown",
				  sizeof(d->pr.fstype));
	}
	d->probed = 1;
}

struct part_tmp {
	char name[DEVD_NAME_MAX];
	uint32_t maj, min;
	uint64_t size;
	uint64_t start;
	int partno;
	int ro;
};

/* SCSI peripheral device type of a CD/DVD drive */
#define SCSI_TYPE_ROM 5

static int part_cmp(const void *a, const void *b)
{
	const struct part_tmp *x = a, *y = b;
	return x->partno - y->partno;
}

/* One disk and its partitions into g_new. */
static void scan_disk(const char *name, const char *force, int all)
{
	char path[DEVD_PATH_MAX], buf[64];
	struct part_tmp parts[MAX_PARTS];
	int np = 0;
	struct dev *d, *od;
	uint64_t sectors, v;
	DIR *dir;
	struct dirent *e;
	int reprobe, optical, disk_ro;
	uint64_t lbs = 512;
	struct pt_reserved pt;

	memset(&pt, 0, sizeof(pt));
	if (g_new.n >= DEVD_MAX_DEVS)
		return;
	d = &g_new.d[g_new.n];
	memset(d, 0, sizeof(*d));
	strlcpy_d(d->name, name, sizeof(d->name));
	strlcpy_d(d->disk, name, sizeof(d->disk));
	d->kind = DEV_DISK;
	d->mountuid = -1;
	snprintf(path, sizeof(path), "/sys/block/%s/dev", name);
	if (read_attr(path, buf, sizeof(buf)) <= 0 ||
	    parse_devnum(buf, &d->major, &d->minor))
		return; /* going away */
	snprintf(path, sizeof(path), "/sys/block/%s/size", name);
	if (sys_u64(path, &sectors) || sectors > (UINT64_MAX >> 9))
		return;
	d->size = sectors << 9;
	snprintf(path, sizeof(path), "/sys/block/%s/removable", name);
	d->removable = sys_u64(path, &v) == 0 && v == 1;
	snprintf(path, sizeof(path), "/sys/block/%s/diskseq", name);
	if (sys_u64(path, &d->diskseq))
		d->diskseq = 0;
	snprintf(path, sizeof(path), "/sys/block/%s/ro", name);
	disk_ro = sys_u64(path, &v) == 0 && v != 0;
	snprintf(path, sizeof(path), "/sys/block/%s/device/type", name);
	optical = sys_u64(path, &v) == 0 && v == SCSI_TYPE_ROM;
	snprintf(path, sizeof(path), "/sys/block/%s/queue/logical_block_size",
		 name);
	if (sys_u64(path, &lbs) || lbs < 512 || lbs > 4096)
		lbs = 512;
	snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", name);
	sys_ident(path, d->vendor, sizeof(d->vendor));
	snprintf(path, sizeof(path), "/sys/block/%s/device/model", name);
	sys_ident(path, d->model, sizeof(d->model));

	snprintf(path, sizeof(path), "/sys/block/%s", name);
	dir = opendir(path);
	if (!dir)
		return;
	while ((e = readdir(dir)) && np < MAX_PARTS) {
		struct part_tmp *p = &parts[np];
		size_t nl = strlen(name);

		if (strncmp(e->d_name, name, nl) != 0 || !e->d_name[nl] ||
		    !devname_ok(e->d_name))
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/partition",
			 name, e->d_name);
		if (sys_u64(path, &v) || v == 0 || v > 4096)
			continue;
		p->partno = (int)v;
		strlcpy_d(p->name, e->d_name, sizeof(p->name));
		snprintf(path, sizeof(path), "/sys/block/%s/%s/dev", name,
			 e->d_name);
		if (read_attr(path, buf, sizeof(buf)) <= 0 ||
		    parse_devnum(buf, &p->maj, &p->min))
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/size", name,
			 e->d_name);
		if (sys_u64(path, &v) || v > (UINT64_MAX >> 9))
			continue;
		p->size = v << 9;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/start", name,
			 e->d_name);
		if (sys_u64(path, &p->start))
			p->start = 0;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/ro", name,
			 e->d_name);
		p->ro = sys_u64(path, &v) == 0 && v != 0;
		np++;
	}
	closedir(dir);
	qsort(parts, (size_t)np, sizeof(parts[0]), part_cmp);
	if (g_new.n + 1 + np > DEVD_MAX_DEVS) {
		log_warn("too many devices; %s left out", name);
		return;
	}
	d->nparts = np;
	d->sig = fnv(fnv(fnv(fnv(1469598103934665603ULL, d->diskseq), d->size),
			 (uint64_t)np),
		     ((uint64_t)d->major << 32) | d->minor);
	for (int i = 0; i < np; i++)
		d->sig = fnv(fnv(d->sig, parts[i].size),
			     ((uint64_t)parts[i].maj << 32) | parts[i].min);

	od = old_find(name, d->major, d->minor);
	reprobe = all || !od || od->sig != d->sig || !od->probed ||
		  (force && strcmp(force, name) == 0);
	if (od) {
		/* a new medium (card readers) is no longer ejected */
		d->ejected = od->diskseq == d->diskseq ? od->ejected : 0;
		d->ev_usb = od->ev_usb;
	}
	d->ev_usb |= ev_usb_has(name);
	d->usb = d->ev_usb || usb_link_has(name);
	for (int i = 0; i < np; i++)
		if (usb_link_has(parts[i].name))
			d->usb = 1;
	dev_classify(d, d->usb, optical, disk_ro, 0);
	/* the firmware's and the installed systems' partitions of a
	 * built-in disk, from its partition table (read again whenever the
	 * disk is probed again) */
	if (d->internal && np > 0 && reprobe) {
		snprintf(path, sizeof(path), "/dev/%s", name);
		if (dev_node_ok(d) &&
		    pt_reserved_device(path, (uint32_t)lbs, d->size, &pt) < 0)
			log_debug("cannot read the partition table of %s",
				  name);
	}
	if (format_busy(name) && od) {
		d->pr = od->pr;
		d->probed = 1;
	} else if (!reprobe) {
		d->pr = od->pr;
		d->probed = 1;
	} else {
		probe_one(d);
	}
	g_new.n++;

	for (int i = 0; i < np; i++) {
		struct dev *p = &g_new.d[g_new.n];
		struct dev *op = old_find(parts[i].name, parts[i].maj,
					  parts[i].min);

		memset(p, 0, sizeof(*p));
		strlcpy_d(p->name, parts[i].name, sizeof(p->name));
		strlcpy_d(p->disk, name, sizeof(p->disk));
		p->kind = DEV_PART;
		p->major = parts[i].maj;
		p->minor = parts[i].min;
		p->size = parts[i].size;
		p->partno = parts[i].partno;
		p->diskseq = d->diskseq;
		p->removable = d->removable;
		p->start = parts[i].start;
		dev_classify(p, d->usb, d->optical, disk_ro, parts[i].ro);
		if (d->internal)
			p->reserved = reprobe || !op ?
					      pt_reserved_has(&pt, p->start) :
					      op->reserved;
		p->ejected = d->ejected;
		p->mountuid = -1;
		strlcpy_d(p->vendor, d->vendor, sizeof(p->vendor));
		strlcpy_d(p->model, d->model, sizeof(p->model));
		if (op && (format_busy(p->name) || !reprobe) && op->probed) {
			p->pr = op->pr;
			p->probed = 1;
		} else {
			probe_one(p);
		}
		g_new.n++;
	}
}

/* ---- the mount table ------------------------------------------------------- */

void mnt_unescape(char *s)
{
	char *o = s;

	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' &&
		    s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			int v = (s[1] - '0') * 64 + (s[2] - '0') * 8 +
				(s[3] - '0');
			*o++ = v ? (char)v : '?';
			s += 4;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

#define MNT_FILE_MAX (256 * 1024)

int mnt_read(struct mnt_ent *out, int max)
{
	char *buf = malloc(MNT_FILE_MAX);
	long len;
	int n = 0, info = 1;
	char *line, *next;

	if (!buf)
		return -1;
	len = read_file("/proc/self/mountinfo", buf, MNT_FILE_MAX);
	if (len < 0) {
		info = 0;
		len = read_file("/proc/mounts", buf, MNT_FILE_MAX);
	}
	if (len < 0) {
		free(buf);
		return -1;
	}
	if (len >= MNT_FILE_MAX - 1)
		log_warn("mount table longer than %d bytes; the rest is "
			 "ignored",
			 MNT_FILE_MAX);
	for (line = buf; line && *line && n < max; line = next) {
		char *tok[24];
		int nt = 0;
		char *p = line;
		struct mnt_ent *m = &out[n];
		const char *mp, *src = NULL;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		while (*p && nt < 24) {
			while (*p == ' ')
				p++;
			if (!*p)
				break;
			tok[nt++] = p;
			while (*p && *p != ' ')
				p++;
			if (*p)
				*p++ = '\0';
		}
		memset(m, 0, sizeof(*m));
		if (info) {
			int dash = -1;

			if (nt < 7)
				continue;
			if (parse_devnum(tok[2], &m->major, &m->minor)) {
				m->major = 0;
				m->minor = 0;
			}
			mp = tok[4];
			for (int i = 6; i < nt; i++)
				if (strcmp(tok[i], "-") == 0) {
					dash = i;
					break;
				}
			if (dash >= 0 && dash + 2 < nt)
				src = tok[dash + 2];
		} else {
			if (nt < 3)
				continue;
			src = tok[0];
			mp = tok[1];
		}
		if (strlen(mp) >= sizeof(m->path))
			continue;
		strlcpy_d(m->path, mp, sizeof(m->path));
		mnt_unescape(m->path);
		if (src && strncmp(src, "/dev/", 5) == 0) {
			char sp[DEVD_PATH_MAX];
			struct stat st;

			if (strlen(src) < sizeof(sp)) {
				strlcpy_d(sp, src, sizeof(sp));
				mnt_unescape(sp);
				if (stat(sp, &st) == 0 && S_ISBLK(st.st_mode)) {
					m->src_blk = 1;
					m->src_major = major(st.st_rdev);
					m->src_minor = minor(st.st_rdev);
				}
			}
		}
		n++;
	}
	free(buf);
	return n;
}

/* ---- rescan ---------------------------------------------------------------------- */

static int ent_cmp(const void *a, const void *b)
{
	const struct dev *x = a, *y = b;
	int c = strcmp(x->disk, y->disk);

	if (c)
		return c;
	if (x->kind != y->kind)
		return x->kind - y->kind;
	return x->partno - y->partno;
}

static void assign_mounts(struct devtab *t, const struct mnt_ent *m, int nm)
{
	for (int i = 0; i < t->n; i++) {
		struct dev *d = &t->d[i];

		d->mounted[0] = '\0';
		d->devd_mount = 0;
		d->foreign_mount = 0;
		d->mountuid = -1;
		for (int k = 0; k < nm; k++) {
			long uid;
			int match = (m[k].major && m[k].major == d->major &&
				     m[k].minor == d->minor) ||
				    (m[k].src_blk && m[k].src_major == d->major &&
				     m[k].src_minor == d->minor);
			if (!match)
				continue;
			if (mnt_is_devd_path(m[k].path, &uid)) {
				if (!d->devd_mount) {
					d->devd_mount = 1;
					d->mountuid = uid;
					strlcpy_d(d->mounted, m[k].path,
						  sizeof(d->mounted));
				}
			} else {
				d->foreign_mount = 1;
			}
		}
		if (!d->devd_mount && d->foreign_mount) {
			for (int k = 0; k < nm; k++) {
				if ((m[k].major && m[k].major == d->major &&
				     m[k].minor == d->minor) ||
				    (m[k].src_blk && m[k].src_major == d->major &&
				     m[k].src_minor == d->minor)) {
					strlcpy_d(d->mounted, m[k].path,
						  sizeof(d->mounted));
					break;
				}
			}
		}
	}
}

/* The device "/" is on (0:0 when unknown) and whether the mount table
 * could be read: without it nothing tells which built-in disk the system
 * uses, so all of them count as the system's. */
struct sys_view {
	uint32_t root_major, root_minor;
	int mounts_known;
};

static void compute_flags(struct devtab *t, const struct sys_view *sv)
{
	for (int i = 0; i < t->n; i++) {
		struct dev *disk = &t->d[i];
		int sys = 0;

		if (disk->kind != DEV_DISK)
			continue;
		for (int k = 0; k < t->n; k++) {
			const struct dev *e = &t->d[k];

			if (strcmp(e->disk, disk->name) != 0)
				continue;
			if (e->foreign_mount)
				sys = 1;
			if ((sv->root_major || sv->root_minor) &&
			    e->major == sv->root_major &&
			    e->minor == sv->root_minor)
				sys = 1;
		}
		if (!sv->mounts_known && disk->internal)
			sys = 1;
		for (int k = 0; k < t->n; k++) {
			struct dev *d = &t->d[k];

			if (strcmp(d->disk, disk->name) != 0)
				continue;
			/* a firmware or recovery partition is reported as
			 * the system's: nobody is offered it */
			d->system = sys || d->reserved;
			d->supported = dev_is_volume(d) && fs_supported(&d->pr);
			if (format_busy(d->name))
				d->state = DS_FORMATTING;
			else if (d->kind == DEV_DISK &&
				 format_disk_busy(d->name))
				d->state = DS_BUSY;
			else
				d->state = DS_READY;
		}
	}
}

static int same_ent(const struct dev *a, const struct dev *b)
{
	return a->major == b->major && a->minor == b->minor &&
	       strcmp(a->name, b->name) == 0;
}

static void send_event(const char *what, const struct dev *d)
{
	struct devp_line l;
	char prefix[32];

	snprintf(prefix, sizeof(prefix), "EVENT %s", what);
	if (dev_format_line(&l, prefix, d) < 0) {
		log_warn("event line for %s too long", d->name);
		return;
	}
	clients_broadcast(&l);
}

void tab_rescan(const char *force, int all)
{
	DIR *dir;
	struct dirent *e;
	int nm;
	char names[DEVD_MAX_DEVS][DEVD_NAME_MAX];
	int nn = 0;
	struct sys_view sv;
	struct stat rst;

	dir = opendir("/sys/block");
	if (!dir) {
		log_warn("cannot read /sys/block: %s", strerror(errno));
		return;
	}
	while ((e = readdir(dir)) && nn < DEVD_MAX_DEVS) {
		if (e->d_name[0] == '.' || !devname_ok(e->d_name))
			continue;
		strlcpy_d(names[nn++], e->d_name, DEVD_NAME_MAX);
	}
	closedir(dir);

	read_usb_links();
	g_new.n = 0;
	for (int i = 0; i < nn; i++)
		scan_disk(names[i], force, all);
	qsort(g_new.d, (size_t)g_new.n, sizeof(g_new.d[0]), ent_cmp);

	/* forget the USB notes of disks that are gone */
	for (int i = 0; i < 16; i++) {
		int found = 0;

		if (!g_ev_usb[i][0])
			continue;
		for (int k = 0; k < g_new.n; k++)
			if (strcmp(g_new.d[k].name, g_ev_usb[i]) == 0)
				found = 1;
		if (!found)
			g_ev_usb[i][0] = '\0';
	}

	/* what went away loses its mounts before the table is read again */
	for (int i = 0; i < g_tab.n; i++) {
		int found = 0;

		for (int k = 0; k < g_new.n && !found; k++)
			found = same_ent(&g_tab.d[i], &g_new.d[k]);
		if (!found && g_tab.d[i].devd_mount)
			detach_removed(&g_tab.d[i]);
	}
	/* and what is still there but holds another medium, or none: the
	 * mount is of a disc or a card that went (a new one is probed and
	 * mounted afresh) */
	for (int i = 0; i < g_tab.n; i++) {
		const struct dev *o = &g_tab.d[i];

		if (!o->devd_mount)
			continue;
		for (int k = 0; k < g_new.n; k++) {
			if (!same_ent(o, &g_new.d[k]))
				continue;
			if (dev_medium_changed(o, &g_new.d[k])) {
				log_info("%s: the medium changed", o->name);
				detach_removed(o);
			}
			break;
		}
	}

	memset(&sv, 0, sizeof(sv));
	if (stat("/", &rst) == 0) {
		sv.root_major = major(rst.st_dev);
		sv.root_minor = minor(rst.st_dev);
	}
	nm = mnt_read(g_mnts, MAX_MOUNTS);
	if (nm < 0) {
		log_warn("cannot read the mount table");
		nm = 0;
	} else {
		sv.mounts_known = 1;
	}
	assign_mounts(&g_new, g_mnts, nm);
	compute_flags(&g_new, &sv);

	/* events: removals (partitions first), additions (disks first),
	 * changes */
	for (int i = g_tab.n - 1; i >= 0; i--) {
		int found = 0;

		for (int k = 0; k < g_new.n && !found; k++)
			found = same_ent(&g_tab.d[i], &g_new.d[k]);
		if (!found) {
			log_info("%s removed", g_tab.d[i].name);
			send_event("REMOVED", &g_tab.d[i]);
		}
	}
	for (int k = 0; k < g_new.n; k++) {
		struct dev *o = NULL;

		for (int i = 0; i < g_tab.n && !o; i++)
			if (same_ent(&g_tab.d[i], &g_new.d[k]))
				o = &g_tab.d[i];
		if (!o) {
			log_info("%s added (%s)", g_new.d[k].name,
				 g_new.d[k].pr.fstype);
			send_event("ADDED", &g_new.d[k]);
		}
	}
	for (int k = 0; k < g_new.n; k++) {
		struct dev *o = NULL;
		struct devp_line a, b;

		for (int i = 0; i < g_tab.n && !o; i++)
			if (same_ent(&g_tab.d[i], &g_new.d[k]))
				o = &g_tab.d[i];
		if (!o)
			continue;
		if (dev_format_line(&a, "X", o) != dev_format_line(&b, "X",
								    &g_new.d[k]) ||
		    memcmp(a.buf, b.buf, a.len) != 0)
			send_event("CHANGED", &g_new.d[k]);
	}

	memcpy(g_tab.d, g_new.d, sizeof(g_new.d[0]) * (size_t)g_new.n);
	g_tab.n = g_new.n;
}
