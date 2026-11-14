// LikeOS -- /sys/firmware/efi: the firmware's variables as files.
//
// Present only when the UEFI runtime services work (kernel/hal/efi.c), so
// a program asks "can I manage boot entries here?" by testing for the
// directory.
//
//   /sys/firmware/efi/fw_platform_size   "64": the firmware's word size
//   /sys/firmware/efi/fw_vendor          the firmware vendor's name
//   /sys/firmware/efi/efivars/<Name>-<GUID>
//
// One file per variable, named by the variable name (UTF-8) and the vendor
// GUID (canonical, lower case): Boot0001-8be4df61-93ca-11d2-aa0d-00e098032b8c.
//
//   read     4 bytes of attributes (little-endian) followed by the data
//   write    the same layout, in ONE write(); creates or replaces the
//            variable.  With EFI_VARIABLE_APPEND_WRITE the data is
//            appended.  Exactly 4 bytes (attributes, no data) deletes it.
//   create   open(O_CREAT) of a name not listed yet; the variable exists
//            from the first write on
//   unlink   deletes the variable
//   readdir  every variable the firmware enumerates
//
// Only root writes (the files are 0644 root:root and the directory 0755;
// the store callback checks the privilege again).  Two safety rules on
// top of what the firmware itself enforces:
//
//   * of the variables under the global GUID only the boot manager's own
//     can be deleted (Boot####, Driver####, SysPrep####, Key####,
//     BootOrder, BootNext, Timeout, DriverOrder, SysPrepOrder): the
//     others describe the platform, and some firmware does not start
//     again without them.  Variables under other GUIDs can be deleted.
//   * what the boot manager reads is checked before it is written: a
//     Boot#### must be a well-formed load option with a well-formed
//     device path, an order list a whole number of 16-bit entries,
//     BootNext and Timeout exactly 16 bits.  Firmware has been known to
//     stop booting over a malformed boot entry.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/sysfs.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/hal/efi.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>
#include <boot/efi_loadopt.h>

#define EFIVARS_DIR "firmware/efi/efivars"
#define EFIVARS_GUID_TEXT 36

/* A variable's identity, hung on its file node. */
struct efivar_id {
	uint16_t name[EFI_VAR_NAME_MAX];
	efi_guid_t guid;
};

static const efi_guid_t g_global_guid = EFI_GLOBAL_VARIABLE_GUID;

/* ---- names ------------------------------------------------------------ */

/* "<Name>-<GUID>" -> identity.  0, or -EINVAL for a name that cannot be a
 * variable's file. */
static int efivars_parse(const char *fname, struct efivar_id *id)
{
	char nm[PFS_NAME_MAX];
	size_t len = 0;

	while (fname[len])
		len++;
	if (len < EFIVARS_GUID_TEXT + 2 || len >= sizeof(nm) ||
	    fname[len - EFIVARS_GUID_TEXT - 1] != '-')
		return -EINVAL;
	if (elo_guid_from_text(fname + len - EFIVARS_GUID_TEXT, id->guid.b))
		return -EINVAL;
	/* elo_guid_from_text takes braces too; a file name has none */
	if (fname[len - EFIVARS_GUID_TEXT] == '{')
		return -EINVAL;
	mm_memcpy(nm, fname, len - EFIVARS_GUID_TEXT - 1);
	nm[len - EFIVARS_GUID_TEXT - 1] = 0;
	if (!elo_utf8_to_ucs2(nm, (elo_u8 *)id->name, sizeof(id->name)))
		return -EINVAL;
	if (!id->name[0])
		return -EINVAL;
	return 0;
}

/* Identity -> "<Name>-<GUID>"; 0, or -ENAMETOOLONG / -EINVAL when the
 * name cannot be a file name here (too long, contains '/'). */
static int efivars_format(const uint16_t *name, const efi_guid_t *guid,
			  char *out, size_t cap)
{
	char g[37];
	size_t n, units = 0;

	while (units < EFI_VAR_NAME_MAX && name[units])
		units++;
	n = elo_ucs2_to_utf8((const elo_u8 *)name, units, out, cap);
	if (n + 1 + EFIVARS_GUID_TEXT + 1 > cap)
		return -ENAMETOOLONG;
	/* A cut name would fit and still be wrong: the converted length
	 * must be the whole name's.  Characters beyond U+FFFF (surrogate
	 * pairs) are not taken: a file name could not be turned back into
	 * the variable's name (elo_utf8_to_ucs2 stops at U+FFFF). */
	{
		size_t need = 0;

		for (size_t i = 0; i < units; i++) {
			if (name[i] >= 0xd800 && name[i] <= 0xdfff)
				return -EINVAL;
			need += name[i] < 0x80 ? 1 : name[i] < 0x800 ? 2 : 3;
		}
		if (need != n)
			return -ENAMETOOLONG;
	}
	for (size_t i = 0; i < n; i++)
		if (out[i] == '/')
			return -EINVAL;
	if (n == 0)
		return -EINVAL;
	elo_guid_to_text(guid->b, g);
	out[n] = '-';
	mm_memcpy(out + n + 1, g, EFIVARS_GUID_TEXT + 1);
	return 0;
}

static int efivars_is_global(const efi_guid_t *g)
{
	for (int i = 0; i < 16; i++)
		if (g->b[i] != g_global_guid.b[i])
			return 0;
	return 1;
}

/* The UCS-2 name equals the ASCII string. */
static int efivars_name_is(const uint16_t *n, const char *s)
{
	size_t i = 0;

	for (; s[i]; i++)
		if (n[i] != (uint16_t)(unsigned char)s[i])
			return 0;
	return n[i] == 0;
}

/* The UCS-2 name is `prefix' and four upper-case hex digits. */
static int efivars_name_numbered(const uint16_t *n, const char *prefix)
{
	size_t i = 0;

	for (; prefix[i]; i++)
		if (n[i] != (uint16_t)(unsigned char)prefix[i])
			return 0;
	for (int k = 0; k < 4; k++, i++)
		if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'A' && n[i] <= 'F')))
			return 0;
	return n[i] == 0;
}

static int efivars_is_load_option(const uint16_t *n)
{
	return efivars_name_numbered(n, "Boot") ||
	       efivars_name_numbered(n, "Driver") ||
	       efivars_name_numbered(n, "SysPrep");
}

static int efivars_is_order(const uint16_t *n)
{
	return efivars_name_is(n, "BootOrder") ||
	       efivars_name_is(n, "DriverOrder") ||
	       efivars_name_is(n, "SysPrepOrder");
}

/* May this variable be deleted? */
static int efivars_removable(const struct efivar_id *id)
{
	if (!efivars_is_global(&id->guid))
		return 1;
	return efivars_is_load_option(id->name) || efivars_is_order(id->name) ||
	       efivars_name_numbered(id->name, "Key") ||
	       efivars_name_is(id->name, "BootNext") ||
	       efivars_name_is(id->name, "Timeout");
}

/* Is `data' fit to be stored in this variable?  0 or -EINVAL. */
static int efivars_validate(const struct efivar_id *id, uint32_t attr,
			    const uint8_t *data, uint64_t len)
{
	if (!efivars_is_global(&id->guid))
		return 0;
	if (efivars_is_order(id->name))
		return (len & 1) ? -EINVAL : 0;
	if (attr & EFI_VARIABLE_APPEND_WRITE)
		return 0; /* a fragment; the firmware joins it */
	if (efivars_is_load_option(id->name)) {
		struct elo_load_option lo;

		if (elo_load_option_parse(data, len, &lo) ||
		    !elo_dp_valid(lo.dp, lo.dp_len))
			return -EINVAL;
		return 0;
	}
	if (efivars_name_is(id->name, "BootNext") ||
	    efivars_name_is(id->name, "Timeout"))
		return len == 2 ? 0 : -EINVAL;
	return 0;
}

/* ---- the listing ------------------------------------------------------ */

/* readdir resumes by position, and the firmware enumerates by "the one
 * after this name".  Each open directory handle lists from a snapshot of its
 * own, taken at its first readdir and again only when it is rewound: a
 * variable written or deleted meanwhile neither shifts its positions nor
 * makes it list a name twice or skip one.  The snapshot is freed when the
 * handle is closed.
 *
 * The names are packed, each NUL-terminated; a snapshot holds at most
 * EFIVARS_SNAP_BYTES of them, and all snapshots alive together at most
 * EFIVARS_SNAP_TOTAL -- a reader that opens the directory many times over
 * gets ENOMEM rather than the kernel's memory. */
#define EFIVARS_SNAP_BYTES (256 * 1024)
#define EFIVARS_SNAP_TOTAL (2 * 1024 * 1024)
#define EFIVARS_SNAP_NAMES 8192

struct efivars_snap {
	unsigned n; /* names */
	unsigned ncap; /* room in `off' */
	size_t used; /* bytes of `names' in use */
	size_t cap; /* bytes `names' holds */
	unsigned *off; /* n offsets into names */
	char *names;
};

/* Serialises the snapshots (taking, reading, freeing) and the total. */
static DEFINE_MUTEX(g_efivars_snap_lock);
static size_t g_snap_total; /* bytes held by all snapshots */

static size_t efivars_snap_bytes(const struct efivars_snap *sn)
{
	return sn->cap + (size_t)sn->ncap * sizeof(unsigned);
}

/* g_efivars_snap_lock held. */
static void efivars_snap_free(struct efivars_snap *sn)
{
	if (!sn)
		return;
	g_snap_total -= efivars_snap_bytes(sn);
	kfree(sn->off);
	kfree(sn->names);
	kfree(sn);
}

/* Room for one more name of `len' bytes (NUL included), growing the
 * snapshot within its own limit and the shared one.  0 or -ENOMEM.
 * g_efivars_snap_lock held. */
static int efivars_snap_room(struct efivars_snap *sn, size_t len)
{
	if (sn->n == sn->ncap) {
		unsigned ncap = sn->ncap * 2;
		size_t more = (size_t)(ncap - sn->ncap) * sizeof(unsigned);
		unsigned *bigger;

		if (ncap > EFIVARS_SNAP_NAMES ||
		    g_snap_total + more > EFIVARS_SNAP_TOTAL)
			return -ENOMEM;
		bigger = kalloc((size_t)ncap * sizeof(unsigned));
		if (!bigger)
			return -ENOMEM;
		mm_memcpy(bigger, sn->off, (size_t)sn->n * sizeof(unsigned));
		kfree(sn->off);
		sn->off = bigger;
		sn->ncap = ncap;
		g_snap_total += more;
	}
	if (sn->used + len > sn->cap) {
		size_t cap = sn->cap * 2;
		char *bigger;

		while (cap < sn->used + len)
			cap *= 2;
		if (cap > EFIVARS_SNAP_BYTES ||
		    g_snap_total + (cap - sn->cap) > EFIVARS_SNAP_TOTAL)
			return -ENOMEM;
		bigger = kalloc(cap);
		if (!bigger)
			return -ENOMEM;
		mm_memcpy(bigger, sn->names, sn->used);
		kfree(sn->names);
		g_snap_total += cap - sn->cap;
		sn->names = bigger;
		sn->cap = cap;
	}
	return 0;
}

static int efivars_same_id(const struct efivar_id *a,
			   const struct efivar_id *b)
{
	for (int i = 0; i < 16; i++)
		if (a->guid.b[i] != b->guid.b[i])
			return 0;
	for (size_t i = 0; i < EFI_VAR_NAME_MAX; i++) {
		if (a->name[i] != b->name[i])
			return 0;
		if (!a->name[i])
			return 1;
	}
	return 1;
}

/* Every variable's file name, packed.  g_efivars_snap_lock held.  NULL with
 * *err set when there is no memory for it (within the limits above). */
static struct efivars_snap *efivars_snap_take(int *err)
{
	struct efivars_snap *sn = kalloc(sizeof(*sn));
	struct efivar_id *cur = kalloc(sizeof(*cur));
	struct efivar_id *prev = kalloc(sizeof(*prev));
	char *fname = kalloc(PFS_NAME_MAX);
	size_t first = 4096 + 64 * sizeof(unsigned);

	if (sn) {
		mm_memset(sn, 0, sizeof(*sn));
		if (g_snap_total + first <= EFIVARS_SNAP_TOTAL) {
			sn->names = kalloc(4096);
			sn->off = kalloc(64 * sizeof(unsigned));
		}
	}
	if (!sn || !cur || !prev || !fname || !sn->names || !sn->off) {
		if (sn) {
			kfree(sn->names);
			kfree(sn->off);
			kfree(sn);
		}
		kfree(cur);
		kfree(prev);
		kfree(fname);
		*err = -ENOMEM;
		return NULL;
	}
	sn->cap = 4096;
	sn->ncap = 64;
	g_snap_total += first;
	mm_memset(cur, 0, sizeof(*cur));
	mm_memset(prev, 0, sizeof(*prev));
	for (unsigned iter = 0; iter < EFIVARS_SNAP_NAMES; iter++) {
		uint64_t size = sizeof(cur->name);
		efi_status_t st =
			efi_get_next_variable_name(&size, cur->name, &cur->guid);
		size_t len;

		if (st == EFI_NOT_FOUND)
			break;
		if (st != EFI_SUCCESS) {
			/* A name longer than EFI_VAR_NAME_MAX stops the walk:
			 * the next call would need it as its starting point. */
			kprintf("efivars: variable enumeration stopped (status %llx) after %u names\n",
				(unsigned long long)st, sn->n);
			break;
		}
		/* Firmware that hands out the same variable again would keep
		 * this going for ever. */
		if (iter && efivars_same_id(cur, prev)) {
			kprintf("efivars: the firmware repeats a variable name; enumeration stopped after %u names\n",
				sn->n);
			break;
		}
		mm_memcpy(prev, cur, sizeof(*prev));
		mm_memset(fname, 0, PFS_NAME_MAX);
		if (efivars_format(cur->name, &cur->guid, fname, PFS_NAME_MAX))
			continue; /* not representable as a file name */
		len = 0;
		while (fname[len])
			len++;
		if (efivars_snap_room(sn, len + 1) != 0) {
			kprintf("efivars: listing cut at %u names (out of room)\n",
				sn->n);
			break;
		}
		sn->off[sn->n++] = (unsigned)sn->used;
		mm_memcpy(sn->names + sn->used, fname, len + 1);
		sn->used += len + 1;
	}
	kfree(cur);
	kfree(prev);
	kfree(fname);
	return sn;
}

static int efivars_list(struct pfs_node *dir, void **snap, int refresh,
			unsigned *cursor, char *name, long cap, int *type)
{
	struct efivars_snap *sn;
	int found = 0;

	(void)dir;
	mutex_lock(&g_efivars_snap_lock);
	sn = *snap;
	if (sn && refresh) {
		efivars_snap_free(sn);
		*snap = sn = NULL;
	}
	if (!sn) {
		int err = 0;

		sn = efivars_snap_take(&err);
		if (!sn) {
			mutex_unlock(&g_efivars_snap_lock);
			return err;
		}
		*snap = sn;
	}
	if (*cursor < sn->n) {
		const char *s = sn->names + sn->off[*cursor];
		long k = 0;

		while (s[k] && k + 1 < cap) {
			name[k] = s[k];
			k++;
		}
		name[k] = 0;
		*type = DT_REG;
		*cursor += 1;
		found = 1;
	}
	mutex_unlock(&g_efivars_snap_lock);
	return found;
}

static void efivars_list_free(struct pfs_node *dir, void *snap)
{
	(void)dir;
	mutex_lock(&g_efivars_snap_lock);
	efivars_snap_free(snap);
	mutex_unlock(&g_efivars_snap_lock);
}

/* ---- the files -------------------------------------------------------- */

static long efivars_show(struct pfs_node *n, char *buf, long cap)
{
	const struct efivar_id *id = n->arg;
	uint64_t size = cap > 4 ? (uint64_t)(cap - 4) : 0;
	uint32_t attr = 0;
	efi_status_t st;

	/* Straight into the buffer; when it is too small the length that
	 * would be needed makes the caller come back with a bigger one. */
	st = efi_get_variable(id->name, &id->guid, &attr, &size,
			      size ? buf + 4 : NULL);
	if (st == EFI_NOT_FOUND)
		return 0; /* made by O_CREAT and not written yet, or deleted */
	if (st == EFI_BUFFER_TOO_SMALL) {
		/* Larger than any read can carry. */
		if (size > EFI_VAR_READ_MAX)
			return -EFBIG;
		return 4 + (long)size;
	}
	if (st == EFI_OUT_OF_RESOURCES)
		return -ENOMEM; /* no memory to bounce a large read through */
	if (st != EFI_SUCCESS)
		return efi_status_to_errno(st);
	if (cap < 4)
		return 4 + (long)size;
	buf[0] = (char)(attr & 0xff);
	buf[1] = (char)((attr >> 8) & 0xff);
	buf[2] = (char)((attr >> 16) & 0xff);
	buf[3] = (char)((attr >> 24) & 0xff);
	return 4 + (long)size;
}

static int efivars_delete(const struct efivar_id *id)
{
	uint64_t size = 0;
	uint32_t attr = 0;
	efi_status_t st;

	if (!capable())
		return -EPERM;
	if (!efivars_removable(id))
		return -EPERM;
	/* The variable's own attributes: some firmware refuses a delete
	 * that names different ones. */
	st = efi_get_variable(id->name, &id->guid, &attr, &size, NULL);
	if (st == EFI_NOT_FOUND)
		return -ENOENT;
	if (st != EFI_BUFFER_TOO_SMALL && st != EFI_SUCCESS)
		return efi_status_to_errno(st);
	st = efi_set_variable(id->name, &id->guid,
			      attr & ~EFI_VARIABLE_APPEND_WRITE, 0, NULL);
	return efi_status_to_errno(st);
}

static long efivars_store(struct pfs_node *n, const char *buf, long len)
{
	const struct efivar_id *id = n->arg;
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t attr;
	efi_status_t st;
	int r;

	if (!capable())
		return -EPERM;
	if (len < 4)
		return -EINVAL;
	attr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
	if (attr & ~EFI_VARIABLE_ATTRIBUTES_MASK)
		return -EINVAL;
	if (len == 4 && !(attr & EFI_VARIABLE_APPEND_WRITE)) {
		r = efivars_delete(id);
		return r ? r : len;
	}
	/* The firmware also deletes a variable rewritten without access
	 * attributes, whatever the data: such a write is a delete, and
	 * passes the same rules. */
	if (!(attr & (EFI_VARIABLE_BOOTSERVICE_ACCESS |
		      EFI_VARIABLE_RUNTIME_ACCESS))) {
		if (attr & EFI_VARIABLE_APPEND_WRITE)
			return -EINVAL;
		r = efivars_delete(id);
		return r ? r : len;
	}
	if ((uint64_t)(len - 4) > EFI_VAR_DATA_MAX)
		return -EFBIG;
	r = efivars_validate(id, attr, p + 4, (uint64_t)(len - 4));
	if (r)
		return r;
	st = efi_set_variable(id->name, &id->guid, attr, (uint64_t)(len - 4),
			      p + 4);
	if (st != EFI_SUCCESS)
		return efi_status_to_errno(st);
	return len;
}

static int efivars_unlink(struct pfs_node *n)
{
	return efivars_delete(n->arg);
}

static void efivars_release(struct pfs_node *n)
{
	kfree(n->arg);
	n->arg = NULL;
}

/* A file node for `fname'; `must_exist' asks the firmware first. */
static struct pfs_node *efivars_node(struct pfs_node *dir, const char *fname,
				     int must_exist)
{
	struct efivar_id *id;
	struct pfs_node *n;

	if (!efi_runtime_available())
		return NULL;
	id = kalloc(sizeof(*id));
	if (!id)
		return NULL;
	mm_memset(id, 0, sizeof(*id));
	if (efivars_parse(fname, id)) {
		kfree(id);
		return NULL;
	}
	if (must_exist) {
		uint64_t size = 0;
		efi_status_t st =
			efi_get_variable(id->name, &id->guid, NULL, &size, NULL);

		if (st != EFI_SUCCESS && st != EFI_BUFFER_TOO_SMALL) {
			kfree(id);
			return NULL;
		}
	}
	n = pfs_node_new(dir->fs, dir, fname, PFS_FILE);
	if (!n) {
		kfree(id);
		return NULL;
	}
	n->mode = 0644;
	n->arg = id;
	n->show = efivars_show;
	n->store = efivars_store;
	n->unlink = efivars_unlink;
	n->release = efivars_release;
	n->store_max = 4 + EFI_VAR_DATA_MAX;
	return n;
}

static struct pfs_node *efivars_lookup(struct pfs_node *dir, const char *name)
{
	return efivars_node(dir, name, 1);
}

static struct pfs_node *efivars_create(struct pfs_node *dir, const char *name)
{
	return efivars_node(dir, name, 0);
}

/* ---- /sys/firmware/efi ------------------------------------------------ */

static long efi_show_platform_size(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0, "64\n");
}

static long efi_show_fw_vendor(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0, "%s\n", efi_fw_vendor());
}

static long efi_show_fw_revision(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0, "0x%x\n", efi_fw_revision());
}

static long efi_show_spec_revision(struct pfs_node *n, char *buf, long cap)
{
	uint32_t r = efi_spec_revision();

	(void)n;
	/* major.minor, the minor written as the specification names it
	 * (2.70 is "2.7") */
	if ((r & 0xffff) % 10)
		return pfs_printf(buf, cap, 0, "%u.%u.%u\n", r >> 16,
				  (r & 0xffff) / 10, (r & 0xffff) % 10);
	return pfs_printf(buf, cap, 0, "%u.%u\n", r >> 16, (r & 0xffff) / 10);
}

void efivars_init(void)
{
	struct pfs_node t;

	if (!efi_runtime_available())
		return;
	sysfs_mkdir("firmware/efi");
	sysfs_add_attr("firmware/efi", "fw_platform_size",
		       efi_show_platform_size, NULL, NULL, 0);
	sysfs_add_attr("firmware/efi", "fw_vendor", efi_show_fw_vendor, NULL,
		       NULL, 0);
	sysfs_add_attr("firmware/efi", "fw_revision", efi_show_fw_revision,
		       NULL, NULL, 0);
	sysfs_add_attr("firmware/efi", "spec_revision", efi_show_spec_revision,
		       NULL, NULL, 0);
	mm_memset(&t, 0, sizeof(t));
	t.mode = 0755;
	t.list_snap = efivars_list;
	t.list_free = efivars_list_free;
	t.lookup = efivars_lookup;
	t.create = efivars_create;
	if (!sysfs_add_node(EFIVARS_DIR, PFS_DIR, &t))
		kprintf("efivars: /sys/" EFIVARS_DIR " could not be made\n");
	else
		kprintf("efivars: firmware variables at /sys/" EFIVARS_DIR "\n");
}
