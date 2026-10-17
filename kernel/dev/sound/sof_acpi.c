// LikeOS Intel audio DSP - machine discovery.
//
// Before the DSP firmware can be loaded we must know what the machine is
// built from: which HD Audio codecs answer on the HDA link (HDMI on most
// DSP laptops), what the BIOS says about the non-HDA audio links in the
// NHLT table (digital microphones, Bluetooth and I2S over SSP), and which
// SoundWire peripherals sit on which link.  The SoundWire part comes from
// the DSDT: below the HD Audio device's ACPI node (\_SB.PCI0.HDAS on most
// machines) there is a "SNDW" device with _ADR 0x40000000 whose _DSD lists
// the links (MIPI DisCo properties), and its children are the peripherals,
// their _ADR encoding link, version, unique id, manufacturer, part and class.
//
// From that we choose the firmware image, the firmware library path and
// the topology file, the way a machine table does: exact entries for the
// SoundWire laptop configurations the SOF binary release has topologies
// for (Tiger/Alder/Raptor/Meteor/Arrow/Lunar/Panther Lake with Cirrus
// Logic CS42L43 + CS35L56, Realtek RT7xx + RT13xx, Maxim MAX98363), the
// per-function topologies for other ACE SoundWire machines (sof_pci.c),
// and the generic HDA topology as the last resort.  Every name asked for
// is one that is published, so a request never goes out for a file that
// cannot exist.  The firmware directory follows the signing key the DSP
// is fused with: SMBIOS tells community-key machines (Chromebooks, UP
// boards) from the retail ones.
//
// Everything read from firmware is bounds-checked; a broken table means
// "no information", never a crash.
//
// Copyright (C) 2026 The LikeOS Project

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <acpi.h>
#pragma GCC diagnostic pop

#include "sdw.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

#define SOF_ACPI_MAX_DEVS 4
#define SOF_ACPI_MAX_CAND 24
#define SOF_ACPI_NAME_LEN 96

/* the controller's _ADR: bits 31..28 = 4 (SoundWire link type) */
#define SNDW_CTRL_ADR 0x40000000ull
#define SNDW_LINK_TYPE 4

/* ==========================================================================
 * Per-device private state
 * ========================================================================== */

struct sof_nhlt_info {
	int valid;
	uint32_t endpoints;
	uint32_t dmic_eps, dmic_count, dmic_max_ch;
	uint32_t ssp_bt_mask, ssp_i2s_mask, ssp_other_mask;
	uint32_t sdw_eps, hda_eps;
	uint32_t render_eps, capture_eps;
	uint32_t max_rate;
};

struct sof_acpi_info {
	struct sof_dev *sdev;
	ACPI_HANDLE hdas;
	ACPI_HANDLE sndw;
	ACPI_HANDLE periph[SOF_MAX_SDW_PERIPHERALS];
	char hdas_path[64];
	uint32_t link_count;
	uint32_t link_list;
	struct sdw_master_props mprops[SOF_MAX_SDW_LINKS];
	struct sof_nhlt_info nhlt;
	char plat[16]; /* "arl", "mtl", "lnl" ... */
	int ace; /* ACE generation (0: cAVS) */
	int ncand;
	char cand[SOF_ACPI_MAX_CAND][SOF_ACPI_NAME_LEN];
	char tplg_dir[40];
	int community; /* DSP fused with the community key */
	int machine_first; /* machine topology before function ones */
};

static struct sof_acpi_info g_acpi[SOF_ACPI_MAX_DEVS];
static spinlock_t g_acpi_lock = SPINLOCK_INIT("sof_acpi");

static struct sof_acpi_info *acpi_info_get(struct sof_dev *sdev, int create)
{
	struct sof_acpi_info *found = NULL;
	uint64_t fl;

	if (!sdev)
		return NULL;
	spin_lock_irqsave(&g_acpi_lock, &fl);
	for (int i = 0; i < SOF_ACPI_MAX_DEVS; i++) {
		if (g_acpi[i].sdev == sdev) {
			found = &g_acpi[i];
			break;
		}
	}
	if (!found && create) {
		for (int i = 0; i < SOF_ACPI_MAX_DEVS; i++) {
			if (!g_acpi[i].sdev) {
				kmemset(&g_acpi[i], 0, sizeof(g_acpi[i]));
				g_acpi[i].sdev = sdev;
				found = &g_acpi[i];
				break;
			}
		}
	}
	spin_unlock_irqrestore(&g_acpi_lock, fl);
	return found;
}

static void acpi_strlcpy(char *dst, const char *src, size_t len)
{
	size_t i = 0;

	if (!dst || !len)
		return;
	if (src) {
		for (; i + 1 < len && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = 0;
}

size_t sdw_dump_printf(char *buf, size_t len, size_t pos, const char *fmt,
		       ...)
{
	va_list ap;
	int n;

	if (!buf || pos >= len)
		return pos;
	__builtin_va_start(ap, fmt);
	n = kvsnprintf(buf + pos, len - pos, fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return pos;
	if ((size_t)n >= len - pos)
		return len - 1;
	return pos + (size_t)n;
}

/* ==========================================================================
 * Small ACPICA helpers
 * ========================================================================== */

/* Evaluate an integer-valued object (Name or Method) below `h'. */
static int acpi_eval_int(ACPI_HANDLE h, const char *name, uint64_t *out)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE tmp;
	ACPI_STATUS st;
	int rc = -ENOENT;

	if (!h)
		return -EINVAL;
	if (ACPI_FAILURE(AcpiGetHandle(h, (char *)name, &tmp)))
		return -ENOENT;
	st = AcpiEvaluateObject(h, (char *)name, NULL, &buf);
	if (ACPI_FAILURE(st))
		return -EIO;
	if (buf.Pointer) {
		ACPI_OBJECT *o = (ACPI_OBJECT *)buf.Pointer;
		if (o->Type == ACPI_TYPE_INTEGER) {
			*out = o->Integer.Value;
			rc = 0;
		} else if (o->Type == ACPI_TYPE_BUFFER &&
			   o->Buffer.Length >= 1 && o->Buffer.Length <= 8) {
			/* some firmware returns _ADR as a buffer */
			uint64_t v = 0;
			for (uint32_t i = 0; i < o->Buffer.Length; i++)
				v |= (uint64_t)o->Buffer.Pointer[i] << (8 * i);
			*out = v;
			rc = 0;
		} else {
			rc = -EINVAL;
		}
		AcpiOsFree(buf.Pointer);
	}
	return rc;
}

/* _STA says the device is present (no _STA: present). */
static int acpi_dev_present(ACPI_HANDLE h)
{
	uint64_t sta;
	int rc = acpi_eval_int(h, "_STA", &sta);

	if (rc == -ENOENT)
		return 1;
	if (rc)
		return 1; /* broken _STA: assume present */
	return (sta & 1) ? 1 : 0;
}

static void acpi_path_of(ACPI_HANDLE h, char *out, size_t len)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };

	if (!out || !len)
		return;
	out[0] = 0;
	if (!h)
		return;
	if (ACPI_SUCCESS(AcpiGetName(h, ACPI_FULL_PATHNAME, &buf)) &&
	    buf.Pointer) {
		acpi_strlcpy(out, (const char *)buf.Pointer, len);
		AcpiOsFree(buf.Pointer);
	}
}

int sdw_acpi_children(void *handle, void **child, uint64_t *adr, int max)
{
	ACPI_HANDLE parent = (ACPI_HANDLE)handle;
	ACPI_HANDLE c = NULL;
	int n = 0, guard = 0;

	if (!parent || !child || max <= 0)
		return 0;
	while (n < max && guard++ < 512) {
		ACPI_HANDLE next;
		uint64_t a = 0;

		if (ACPI_FAILURE(AcpiGetNextObject(ACPI_TYPE_DEVICE, parent, c,
						   &next)))
			break;
		c = next;
		if (acpi_eval_int(c, "_ADR", &a))
			continue;
		if (!acpi_dev_present(c))
			continue;
		child[n] = c;
		if (adr)
			adr[n] = a;
		n++;
	}
	return n;
}

/* ==========================================================================
 * _DSD (Device Specific Data) parsing
 *
 * A _DSD is a package of (UUID, package) pairs.  The "device properties"
 * UUID carries (name, value) pairs; the "hierarchical data extension" UUID
 * carries (name, link) pairs whose link names a data-only object (a
 * Package in the device's scope, by string or by reference) that has the
 * same layout again.  MIPI DisCo builds its link and port descriptions as
 * such trees.
 * ========================================================================== */

/* daffd814-6eba-4d8c-8a91-bc9bbf4aa301 */
static const uint8_t g_uuid_props[16] = { 0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e,
					  0x8c, 0x4d, 0x8a, 0x91, 0xbc, 0x9b,
					  0xbf, 0x4a, 0xa3, 0x01 };
/* dbb8e3e6-5886-4ba6-8795-1319f52a966b */
static const uint8_t g_uuid_hier[16] = { 0xe6, 0xe3, 0xb8, 0xdb, 0x86, 0x58,
					 0xa6, 0x4b, 0x87, 0x95, 0x13, 0x19,
					 0xf5, 0x2a, 0x96, 0x6b };

struct sdw_dsd {
	void *mem; /* owning ACPI buffer */
	ACPI_OBJECT *pkg; /* the property-set package */
	ACPI_HANDLE scope; /* where relative names resolve */
	ACPI_HANDLE dev; /* the device the tree started at */
	int depth;
	char name[16]; /* this node's object name ("" for _DSD) */
};

#define SDW_DSD_MAX_DEPTH 8

static int dsd_valid_pkg(const ACPI_OBJECT *o)
{
	return o && o->Type == ACPI_TYPE_PACKAGE && o->Package.Count < 4096 &&
	       (o->Package.Count == 0 || o->Package.Elements);
}

static struct sdw_dsd *dsd_from_buffer(ACPI_BUFFER *buf, ACPI_HANDLE scope,
				       ACPI_HANDLE dev, int depth)
{
	ACPI_OBJECT *o = (ACPI_OBJECT *)buf->Pointer;
	struct sdw_dsd *d;

	if (!o || !dsd_valid_pkg(o)) {
		if (buf->Pointer)
			AcpiOsFree(buf->Pointer);
		return NULL;
	}
	d = kcalloc(1, sizeof(*d));
	if (!d) {
		AcpiOsFree(buf->Pointer);
		return NULL;
	}
	d->mem = buf->Pointer;
	d->pkg = o;
	d->scope = scope;
	d->dev = dev;
	d->depth = depth;
	return d;
}

struct sdw_dsd *sdw_dsd_open(void *handle)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE h = (ACPI_HANDLE)handle;
	ACPI_HANDLE tmp;

	if (!h)
		return NULL;
	if (ACPI_FAILURE(AcpiGetHandle(h, "_DSD", &tmp)))
		return NULL;
	if (ACPI_FAILURE(AcpiEvaluateObject(h, "_DSD", NULL, &buf)))
		return NULL;
	return dsd_from_buffer(&buf, h, h, 0);
}

void sdw_dsd_close(struct sdw_dsd *d)
{
	if (!d)
		return;
	if (d->mem)
		AcpiOsFree(d->mem);
	kfree(d);
}

static int dsd_uuid_is(const ACPI_OBJECT *o, const uint8_t *uuid)
{
	if (!o || o->Type != ACPI_TYPE_BUFFER || o->Buffer.Length != 16 ||
	    !o->Buffer.Pointer)
		return 0;
	return kmemcmp(o->Buffer.Pointer, uuid, 16) == 0;
}

static int dsd_name_is(const ACPI_OBJECT *o, const char *name)
{
	size_t n;

	if (!o || o->Type != ACPI_TYPE_STRING || !o->String.Pointer)
		return 0;
	n = kstrlen(name);
	if (o->String.Length != n)
		return 0;
	return kstrncmp(o->String.Pointer, name, n) == 0;
}

/* Find entry `name' in the section of `uuid'; returns the value object. */
static const ACPI_OBJECT *dsd_find(struct sdw_dsd *d, const uint8_t *uuid,
				   const char *name)
{
	const ACPI_OBJECT *top;

	if (!d || !d->pkg || !name)
		return NULL;
	top = d->pkg;
	for (uint32_t i = 0; i + 1 < top->Package.Count; i += 2) {
		const ACPI_OBJECT *u = &top->Package.Elements[i];
		const ACPI_OBJECT *p = &top->Package.Elements[i + 1];

		if (!dsd_uuid_is(u, uuid) || !dsd_valid_pkg(p))
			continue;
		for (uint32_t j = 0; j < p->Package.Count; j++) {
			const ACPI_OBJECT *e = &p->Package.Elements[j];

			if (!dsd_valid_pkg(e) || e->Package.Count < 2)
				continue;
			if (dsd_name_is(&e->Package.Elements[0], name))
				return &e->Package.Elements[1];
		}
	}
	return NULL;
}

int sdw_dsd_u64(struct sdw_dsd *d, const char *name, uint64_t *out)
{
	const ACPI_OBJECT *v = dsd_find(d, g_uuid_props, name);

	if (!v)
		return -ENOENT;
	if (v->Type == ACPI_TYPE_INTEGER) {
		*out = v->Integer.Value;
		return 0;
	}
	/* a one-element package is accepted as a scalar */
	if (v->Type == ACPI_TYPE_PACKAGE && v->Package.Count >= 1 &&
	    v->Package.Elements &&
	    v->Package.Elements[0].Type == ACPI_TYPE_INTEGER) {
		*out = v->Package.Elements[0].Integer.Value;
		return 0;
	}
	return -EINVAL;
}

int sdw_dsd_u32(struct sdw_dsd *d, const char *name, uint32_t *out)
{
	uint64_t v;
	int rc = sdw_dsd_u64(d, name, &v);

	if (rc)
		return rc;
	if (v > 0xffffffffull)
		return -ERANGE;
	*out = (uint32_t)v;
	return 0;
}

int sdw_dsd_bool(struct sdw_dsd *d, const char *name)
{
	uint64_t v;

	if (sdw_dsd_u64(d, name, &v))
		return 0;
	return v ? 1 : 0;
}

int sdw_dsd_u32_array(struct sdw_dsd *d, const char *name, uint32_t *out,
		      int max, int *total)
{
	const ACPI_OBJECT *v = dsd_find(d, g_uuid_props, name);
	int n = 0;

	if (total)
		*total = 0;
	if (!v)
		return -ENOENT;
	if (v->Type == ACPI_TYPE_INTEGER) {
		if (total)
			*total = 1;
		if (max > 0 && out) {
			out[0] = (uint32_t)v->Integer.Value;
			return 1;
		}
		return 0;
	}
	if (v->Type == ACPI_TYPE_BUFFER && v->Buffer.Pointer) {
		uint32_t cnt = v->Buffer.Length / 4;
		if (total)
			*total = (int)cnt;
		for (uint32_t i = 0; i < cnt && n < max && out; i++) {
			const uint8_t *b = v->Buffer.Pointer + 4 * i;
			out[n++] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
				   ((uint32_t)b[2] << 16) |
				   ((uint32_t)b[3] << 24);
		}
		return n;
	}
	if (!dsd_valid_pkg(v))
		return -EINVAL;
	if (total)
		*total = (int)v->Package.Count;
	for (uint32_t i = 0; i < v->Package.Count && n < max && out; i++) {
		const ACPI_OBJECT *e = &v->Package.Elements[i];
		if (e->Type != ACPI_TYPE_INTEGER)
			return -EINVAL;
		out[n++] = (uint32_t)e->Integer.Value;
	}
	return n;
}

int sdw_dsd_u8_array(struct sdw_dsd *d, const char *name, uint8_t *out,
		     int max, int *total)
{
	const ACPI_OBJECT *v = dsd_find(d, g_uuid_props, name);
	int n = 0;

	if (total)
		*total = 0;
	if (!v)
		return -ENOENT;
	if (v->Type == ACPI_TYPE_BUFFER) {
		if (!v->Buffer.Pointer && v->Buffer.Length)
			return -EINVAL;
		if (total)
			*total = (int)v->Buffer.Length;
		for (uint32_t i = 0; i < v->Buffer.Length && n < max && out;
		     i++)
			out[n++] = v->Buffer.Pointer[i];
		return n;
	}
	if (v->Type == ACPI_TYPE_INTEGER) {
		if (total)
			*total = 1;
		if (max > 0 && out) {
			out[0] = (uint8_t)v->Integer.Value;
			return 1;
		}
		return 0;
	}
	if (!dsd_valid_pkg(v))
		return -EINVAL;
	if (total)
		*total = (int)v->Package.Count;
	for (uint32_t i = 0; i < v->Package.Count && n < max && out; i++) {
		const ACPI_OBJECT *e = &v->Package.Elements[i];
		if (e->Type != ACPI_TYPE_INTEGER)
			return -EINVAL;
		out[n++] = (uint8_t)e->Integer.Value;
	}
	return n;
}

int sdw_dsd_string(struct sdw_dsd *d, const char *name, char *out,
		   size_t len)
{
	const ACPI_OBJECT *v = dsd_find(d, g_uuid_props, name);
	size_t n;

	if (!out || !len)
		return -EINVAL;
	out[0] = 0;
	if (!v)
		return -ENOENT;
	if (v->Type != ACPI_TYPE_STRING || !v->String.Pointer)
		return -EINVAL;
	n = v->String.Length;
	if (n >= len)
		n = len - 1;
	kmemcpy(out, v->String.Pointer, n);
	out[n] = 0;
	return 0;
}

/* Resolve the link `name' to a handle. */
static int dsd_link_handle(struct sdw_dsd *d, const char *name,
			   ACPI_HANDLE *out, char *tname, size_t tlen)
{
	const ACPI_OBJECT *v = dsd_find(d, g_uuid_hier, name);
	ACPI_HANDLE h = NULL;

	if (tname && tlen)
		tname[0] = 0;
	if (!v)
		return -ENOENT;
	if (v->Type == ACPI_TYPE_LOCAL_REFERENCE) {
		h = v->Reference.Handle;
		if (h && tname)
			acpi_path_of(h, tname, tlen);
	} else if (v->Type == ACPI_TYPE_STRING && v->String.Pointer &&
		   v->String.Length > 0 && v->String.Length < 64) {
		char path[64];
		acpi_strlcpy(path, v->String.Pointer,
			     (size_t)v->String.Length + 1);
		if (tname)
			acpi_strlcpy(tname, path, tlen);
		if (ACPI_FAILURE(AcpiGetHandle(d->scope, path, &h)) &&
		    ACPI_FAILURE(AcpiGetHandle(d->dev, path, &h)) &&
		    ACPI_FAILURE(AcpiGetHandle(NULL, path, &h)))
			h = NULL;
	} else {
		return -EINVAL;
	}
	if (!h)
		return -ENOENT;
	*out = h;
	return 0;
}

int sdw_dsd_link_target(struct sdw_dsd *d, const char *name, char *out,
			size_t len)
{
	ACPI_HANDLE h;
	int rc = dsd_link_handle(d, name, &h, NULL, 0);

	if (rc)
		return rc;
	acpi_path_of(h, out, len);
	return out && len && out[0] ? 0 : -ENOENT;
}

struct sdw_dsd *sdw_dsd_child(struct sdw_dsd *d, const char *name)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE h;
	ACPI_HANDLE parent = NULL;
	struct sdw_dsd *c;
	char tname[16];

	if (!d || d->depth >= SDW_DSD_MAX_DEPTH)
		return NULL;
	if (dsd_link_handle(d, name, &h, tname, sizeof(tname)))
		return NULL;
	if (ACPI_FAILURE(AcpiEvaluateObject(h, NULL, NULL, &buf)))
		return NULL;
	if (ACPI_FAILURE(AcpiGetParent(h, &parent)))
		parent = d->scope;
	c = dsd_from_buffer(&buf, parent ? parent : d->scope, d->dev,
			    d->depth + 1);
	if (c)
		acpi_strlcpy(c->name, tname, sizeof(c->name));
	return c;
}

/* ==========================================================================
 * Finding the HD Audio device's ACPI companion and the SNDW controller
 * ========================================================================== */

struct hda_find_ctx {
	uint32_t adr;
	ACPI_HANDLE match[8];
	int n;
};

static ACPI_STATUS hda_find_cb(ACPI_HANDLE obj, UINT32 level, void *ctx,
			       void **ret)
{
	struct hda_find_ctx *f = ctx;
	uint64_t a;

	(void)level;
	(void)ret;
	if (f->n >= 8)
		return AE_CTRL_TERMINATE;
	if (acpi_eval_int(obj, "_ADR", &a))
		return AE_OK;
	if ((uint32_t)a == f->adr && (a >> 32) == 0)
		f->match[f->n++] = obj;
	return AE_OK;
}

/* Is `h' (or one of its children) the SoundWire controller? */
static ACPI_HANDLE find_sndw_below(ACPI_HANDLE hdas)
{
	void *kids[32];
	uint64_t adr[32];
	int n = sdw_acpi_children(hdas, kids, adr, 32);

	/* depth 1 */
	for (int i = 0; i < n; i++) {
		if (((adr[i] >> 28) & 0xf) == SNDW_LINK_TYPE &&
		    adr[i] == SNDW_CTRL_ADR)
			return (ACPI_HANDLE)kids[i];
	}
	/* depth 2: some firmware nests SNDW one level deeper */
	for (int i = 0; i < n; i++) {
		void *kk[16];
		uint64_t aa[16];
		int m = sdw_acpi_children(kids[i], kk, aa, 16);
		for (int j = 0; j < m; j++) {
			if (aa[j] == SNDW_CTRL_ADR)
				return (ACPI_HANDLE)kk[j];
		}
	}
	return NULL;
}

static void find_hda_companion(struct sof_acpi_info *ai,
			       const pci_device_t *pci)
{
	struct hda_find_ctx f;
	static const char *const common[] = {
		"\\_SB.PCI0.HDAS", "\\_SB.PC00.HDAS", "\\_SB.PCI0.HDEF",
		"\\_SB.PC00.HDEF", "\\_SB.PCI0.HDAU", "\\_SB.PC00.HDAU",
		"\\_SB.PC02.HDAS", "\\_SB.PCI0.AZAL",
	};

	ai->hdas = NULL;
	ai->sndw = NULL;
	kmemset(&f, 0, sizeof(f));
	f.adr = ((uint32_t)pci->device << 16) | pci->function;
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, 16, hda_find_cb,
			  NULL, &f, NULL);

	/* prefer the match that has a SoundWire controller below it */
	for (int i = 0; i < f.n; i++) {
		ACPI_HANDLE s = find_sndw_below(f.match[i]);
		if (s) {
			ai->hdas = f.match[i];
			ai->sndw = s;
			break;
		}
	}
	if (!ai->hdas) {
		/* the well-known names, if they answer to our address */
		for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]);
		     i++) {
			ACPI_HANDLE h;
			uint64_t a;
			if (ACPI_FAILURE(AcpiGetHandle(NULL, (char *)common[i],
						       &h)))
				continue;
			if (acpi_eval_int(h, "_ADR", &a) ||
			    (uint32_t)a != f.adr)
				continue;
			ai->hdas = h;
			ai->sndw = find_sndw_below(h);
			break;
		}
	}
	if (!ai->hdas && f.n > 0)
		ai->hdas = f.match[0];
	if (ai->hdas)
		acpi_path_of(ai->hdas, ai->hdas_path, sizeof(ai->hdas_path));
}

/* ==========================================================================
 * Link (master) properties
 * ========================================================================== */

static void master_props_defaults(struct sdw_master_props *m, int ace)
{
	kmemset(m, 0, sizeof(*m));
	m->ip_clock = 38400000;
	m->mclk_freq = 19200000;
	m->max_clk_freq = 4800000;
	m->clk_freq[0] = 4800000;
	m->nclk_freq = 1;
	m->default_frame_rate = 48000;
	m->default_row = 50;
	m->default_col = 4;
	m->doaise = 0x1;
	m->doais = 0x3;
	m->dodse = 0x0;
	m->dods = 0x1;
	(void)ace;
}

static void parse_master_props(struct sdw_dsd *ctrl, int link,
			       struct sdw_master_props *m, int ace)
{
	char name[48];
	struct sdw_dsd *ln;
	uint32_t v;
	uint32_t arr[SDW_MAX_CLK_FREQS];
	int n, total;

	master_props_defaults(m, ace);
	ksnprintf(name, sizeof(name), "mipi-sdw-link-%d-subproperties", link);
	ln = sdw_dsd_child(ctrl, name);
	if (!ln)
		return;
	m->valid = 1;
	m->clk_stop_mode0 =
		sdw_dsd_bool(ln, "mipi-sdw-clock-stop-mode0-supported");
	m->clk_stop_mode1 =
		sdw_dsd_bool(ln, "mipi-sdw-clock-stop-mode1-supported");
	if (!sdw_dsd_u32(ln, "mipi-sdw-max-clock-frequency", &v) && v &&
	    v <= 24576000)
		m->max_clk_freq = v;
	n = sdw_dsd_u32_array(ln, "mipi-sdw-clock-frequencies-supported", arr,
			      SDW_MAX_CLK_FREQS, &total);
	if (n > 0) {
		int k = 0;
		uint32_t maxf = 0;
		for (int i = 0; i < n; i++) {
			if (!arr[i] || arr[i] > 24576000)
				continue;
			m->clk_freq[k++] = arr[i];
			if (arr[i] > maxf)
				maxf = arr[i];
		}
		if (k) {
			/* keep them sorted ascending for the selection */
			for (int i = 1; i < k; i++) {
				uint32_t x = m->clk_freq[i];
				int j = i - 1;
				while (j >= 0 && m->clk_freq[j] > x) {
					m->clk_freq[j + 1] = m->clk_freq[j];
					j--;
				}
				m->clk_freq[j + 1] = x;
			}
			m->nclk_freq = k;
			if (sdw_dsd_u32(ln, "mipi-sdw-max-clock-frequency",
					&v))
				m->max_clk_freq = maxf;
		}
	}
	n = sdw_dsd_u32_array(ln, "mipi-sdw-supported-clock-scales", arr,
			      SDW_MAX_CLK_FREQS, &total);
	if (n <= 0)
		n = sdw_dsd_u32_array(ln, "mipi-sdw-supported-clock-gears", arr,
				      SDW_MAX_CLK_FREQS, &total);
	for (int i = 0; i < n && i < SDW_MAX_CLK_FREQS; i++) {
		if (arr[i] < 16)
			m->clk_gears[m->nclk_gears++] = arr[i];
	}
	if (!sdw_dsd_u32(ln, "mipi-sdw-default-frame-rate", &v) && v >= 8000 &&
	    v <= 192000)
		m->default_frame_rate = v;
	if (!sdw_dsd_u32(ln, "mipi-sdw-default-frame-row-size", &v) &&
	    v >= 48 && v <= 256)
		m->default_row = v;
	if (!sdw_dsd_u32(ln, "mipi-sdw-default-frame-col-size", &v) &&
	    v >= 2 && v <= 16)
		m->default_col = v;
	m->dynamic_frame = sdw_dsd_bool(ln, "mipi-sdw-dynamic-frame-shape");
	sdw_dsd_u32(ln, "mipi-sdw-command-error-threshold", &m->err_threshold);
	if (!sdw_dsd_u32(ln, "intel-sdw-ip-clock", &v) && v >= 6000000 &&
	    v <= 192000000) {
		m->ip_clock = v;
		/* the firmware reports twice the master clock */
		m->mclk_freq = v / 2;
	}
	sdw_dsd_u32(ln, "intel-quirk-mask", &m->quirk_mask);
#define RD16(prop, field)                                     \
	do {                                                  \
		if (!sdw_dsd_u32(ln, prop, &v) && v <= 0xffff) \
			m->field = (uint16_t)v;               \
	} while (0)
	RD16("intel-sdw-clde", clde);
	RD16("intel-sdw-doaise2", doaise2);
	RD16("intel-sdw-dodse2", dodse2);
	RD16("intel-sdw-clds", clds);
	RD16("intel-sdw-clss", clss);
	RD16("intel-sdw-doaise", doaise);
	RD16("intel-sdw-doais", doais);
	RD16("intel-sdw-dodse", dodse);
	RD16("intel-sdw-dods", dods);
#undef RD16
	sdw_dsd_close(ln);
}

int sof_acpi_master_props(struct sof_dev *sdev, int link,
			  struct sdw_master_props *out)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);

	if (!out || link < 0 || link >= SOF_MAX_SDW_LINKS)
		return -EINVAL;
	if (!ai) {
		master_props_defaults(out, 0);
		return -ENOENT;
	}
	*out = ai->mprops[link];
	if (!out->nclk_freq)
		master_props_defaults(out, ai->ace);
	return out->valid ? 0 : -ENOENT;
}

/* ==========================================================================
 * Peripheral properties
 * ========================================================================== */

static void slave_props_defaults(struct sdw_slave_props *p)
{
	kmemset(p, 0, sizeof(*p));
	p->scp_int1_mask = SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY;
	for (int i = 1; i < SDW_MAX_PORTS; i++) {
		p->dp[i].type = SDW_DPN_FULL;
		p->dp[i].ch_prep_timeout_ms = 50;
		p->dp[i].max_word = 32;
		p->dp[i].min_word = 1;
		p->dp[i].max_ch = 8;
	}
}

static void parse_dpn(struct sdw_dsd *pd, int port, int source,
		      struct sdw_slave_props *p)
{
	char name[48];
	struct sdw_dsd *d;
	struct sdw_dpn_props *dp = &p->dp[port];
	uint32_t v;
	uint32_t arr[16];
	int n, total;

	ksnprintf(name, sizeof(name), "mipi-sdw-dp-%d-%s-subproperties", port,
		  source ? "source" : "sink");
	d = sdw_dsd_child(pd, name);
	dp->valid = 1;
	dp->source = (uint8_t)source;
	if (!d)
		return;
	if (!sdw_dsd_u32(d, "mipi-sdw-port-max-wordlength", &v) && v &&
	    v <= 64)
		dp->max_word = v;
	if (!sdw_dsd_u32(d, "mipi-sdw-port-min-wordlength", &v) && v &&
	    v <= 64)
		dp->min_word = v;
	if (!sdw_dsd_u32(d, "mipi-sdw-data-port-type", &v) && v <= 2)
		dp->type = (uint8_t)v;
	dp->simple_ch_prep_sm =
		(uint8_t)(sdw_dsd_bool(d, "mipi-sdw-simplified-channelprepare-sm") ||
			  sdw_dsd_bool(d, "mipi-sdw-simplified-channel-prepare-sm"));
	if ((!sdw_dsd_u32(d, "mipi-sdw-port-channelprepare-timeout", &v) ||
	     !sdw_dsd_u32(d, "mipi-sdw-peripheral-channelprepare-timeout",
			  &v)) &&
	    v && v <= 2000)
		dp->ch_prep_timeout_ms = v;
	if (!sdw_dsd_u32(d, "mipi-sdw-imp-def-dpn-interrupts-supported", &v))
		dp->imp_def_interrupts = v & SDW_DPN_INT_IMPDEF;
	if (!sdw_dsd_u32(d, "mipi-sdw-max-channel-number", &v) && v && v <= 16)
		dp->max_ch = v;
	n = sdw_dsd_u32_array(d, "mipi-sdw-channel-number-list", arr, 16,
			      &total);
	if (n > 0) {
		uint32_t mx = 0;
		for (int i = 0; i < n; i++)
			if (arr[i] > mx && arr[i] <= 16)
				mx = arr[i];
		if (mx)
			dp->max_ch = mx;
	}
	n = sdw_dsd_u32_array(d, "mipi-sdw-lane-list", arr, 16, &total);
	dp->lane_control = n > 0 ? 1 : 0;
	sdw_dsd_close(d);
}

static void parse_slave_props(ACPI_HANDLE h, struct sdw_slave_props *p)
{
	struct sdw_dsd *d;
	uint32_t v;

	slave_props_defaults(p);
	d = sdw_dsd_open(h);
	if (!d)
		return;
	p->valid = 1;
	sdw_dsd_u32(d, "mipi-sdw-sw-interface-revision", &p->sdw_rev);
	sdw_dsd_u32(d, "mipi-sdw-sdca-interface-revision", &p->sdca_rev);
	p->paging = sdw_dsd_bool(d, "mipi-sdw-paging-supported");
	p->wake_capable = !sdw_dsd_bool(d, "mipi-sdw-wake-up-unavailable");
	p->simple_clk_stop =
		sdw_dsd_bool(d, "mipi-sdw-simplified-clockstopprepare-sm-supported");
	p->high_phy = sdw_dsd_bool(d, "mipi-sdw-highPHY-capable");
	if (!sdw_dsd_u32(d, "mipi-sdw-source-port-list", &v))
		p->source_ports = v & 0x7ffe;
	if (!sdw_dsd_u32(d, "mipi-sdw-sink-port-list", &v))
		p->sink_ports = v & 0x7ffe;
	for (int port = 1; port < SDW_MAX_PORTS; port++) {
		if (p->source_ports & (1u << port))
			parse_dpn(d, port, 1, p);
		else if (p->sink_ports & (1u << port))
			parse_dpn(d, port, 0, p);
	}
	sdw_dsd_close(d);
}

int sof_acpi_slave_props(struct sof_dev *sdev, int periph_idx,
			 struct sdw_slave_props *out)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);

	if (!out)
		return -EINVAL;
	if (!ai || periph_idx < 0 || periph_idx >= SOF_MAX_SDW_PERIPHERALS ||
	    !ai->periph[periph_idx]) {
		slave_props_defaults(out);
		return -ENOENT;
	}
	parse_slave_props(ai->periph[periph_idx], out);
	return out->valid ? 0 : -ENOENT;
}

void *sof_acpi_controller_handle(struct sof_dev *sdev)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);
	return ai ? (void *)ai->sndw : NULL;
}

void *sof_acpi_periph_handle(struct sof_dev *sdev, int periph_idx)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);

	if (!ai || periph_idx < 0 || periph_idx >= SOF_MAX_SDW_PERIPHERALS)
		return NULL;
	return (void *)ai->periph[periph_idx];
}

/* ==========================================================================
 * SDCA functions of a peripheral
 * ========================================================================== */

/* The function type a function node declares in its entity-0 control 0x5
 * ("function topology"), translated from the numbering of interface
 * revisions before 0.8.1. */
int sdw_sdca_function_type(void *fn_handle, uint32_t if_rev)
{
	struct sdw_dsd *d = sdw_dsd_open(fn_handle);
	struct sdw_dsd *c5;
	uint32_t t = 0;
	int rc = -ENOENT;

	if (!d)
		return -ENOENT;
	c5 = sdw_dsd_child(d, "mipi-sdca-control-0x5-subproperties");
	if (c5) {
		if (!sdw_dsd_u32(c5, "mipi-sdca-control-dc-value", &t))
			rc = 0;
		sdw_dsd_close(c5);
	}
	sdw_dsd_close(d);
	if (rc)
		return rc;
	if (if_rev && if_rev < 0x0801) {
		static const uint8_t old[] = { 0, 0x01, 0x03, 0x05, 0x06, 0x07,
					       0x0a };
		if (t >= sizeof(old) || !old[t])
			return -EINVAL;
		t = old[t];
	}
	if (t == 0 || t > 0x1f)
		return -EINVAL;
	return (int)t;
}

/* bit n = SDCA function type n present */
static uint32_t sdca_function_bits(ACPI_HANDLE periph)
{
	void *kids[SDW_MAX_FUNCTIONS];
	uint64_t adr[SDW_MAX_FUNCTIONS];
	struct sdw_dsd *pd;
	uint32_t rev = 0, bits = 0;
	int n;

	pd = sdw_dsd_open(periph);
	if (pd) {
		sdw_dsd_u32(pd, "mipi-sdw-sdca-interface-revision", &rev);
		sdw_dsd_close(pd);
	}
	n = sdw_acpi_children(periph, kids, adr, SDW_MAX_FUNCTIONS);
	for (int i = 0; i < n; i++) {
		int t;
		if (!adr[i] || adr[i] > 7)
			continue;
		t = sdw_sdca_function_type(kids[i], rev);
		if (t > 0 && t < 32)
			bits |= 1u << t;
	}
	return bits;
}

/* ==========================================================================
 * SoundWire controller scan
 * ========================================================================== */

static int scan_sdw_controller(struct sof_dev *sdev, struct sof_acpi_info *ai)
{
	struct sof_machine *m = &sdev->machine;
	struct sdw_dsd *ctrl;
	uint32_t v, list = 0, count = 0;
	void *kids[32];
	uint64_t adr[32];
	int n;

	if (!ai->sndw)
		return -ENODEV;
	ctrl = sdw_dsd_open(ai->sndw);
	if (!ctrl) {
		snd_info("sof", "SoundWire controller without _DSD\n");
		return -ENODEV;
	}
	if (!sdw_dsd_u32(ctrl, "mipi-sdw-manager-list", &v) && v) {
		list = v & ((1u << SOF_MAX_SDW_LINKS) - 1);
		count = sdw_hweight32(list);
	} else if (!sdw_dsd_u32(ctrl, "mipi-sdw-master-count", &v) && v) {
		count = v;
		if (count > SOF_MAX_SDW_LINKS) {
			snd_info("sof",
				 "firmware claims %u SoundWire links, using %d\n",
				 count, SOF_MAX_SDW_LINKS);
			count = SOF_MAX_SDW_LINKS;
		}
		list = (1u << count) - 1;
	} else {
		snd_info("sof", "SoundWire controller lists no links\n");
		sdw_dsd_close(ctrl);
		return -ENODEV;
	}
	ai->link_count = count;
	ai->link_list = list;

	for (int i = 0; i < SOF_MAX_SDW_LINKS; i++) {
		if (!(list & (1u << i))) {
			master_props_defaults(&ai->mprops[i], ai->ace);
			continue;
		}
		parse_master_props(ctrl, i, &ai->mprops[i], ai->ace);
		if (!ai->mprops[i].valid) {
			/* without its subproperties a link is not wired */
			list &= ~(1u << i);
			continue;
		}
		if (ai->mprops[i].quirk_mask & SDW_INTEL_QUIRK_BUS_DISABLE) {
			snd_info("sof", "SoundWire link %d disabled by firmware\n",
				 i);
			list &= ~(1u << i);
		}
	}
	sdw_dsd_close(ctrl);

	/* peripherals: the controller's children with a 64-bit _ADR */
	n = sdw_acpi_children(ai->sndw, kids, adr, 32);
	for (int i = 0; i < n; i++) {
		uint64_t a = adr[i];
		int link = (int)((a >> 48) & 0xf);
		struct sof_sdw_peripheral *p;

		if (!(a >> 32))
			continue; /* not a SoundWire peripheral address */
		if (link >= SOF_MAX_SDW_LINKS || !(list & (1u << link))) {
			snd_info("sof",
				 "SoundWire peripheral %08x%08x on unused link %d ignored\n",
				 (uint32_t)(a >> 32), (uint32_t)a, link);
			continue;
		}
		if (m->nperiph >= SOF_MAX_SDW_PERIPHERALS) {
			snd_info("sof", "too many SoundWire peripherals\n");
			break;
		}
		p = &m->periph[m->nperiph];
		kmemset(p, 0, sizeof(*p));
		p->link = link;
		p->version = (uint8_t)((a >> 44) & 0xf);
		p->unique_id = (uint8_t)((a >> 40) & 0xf);
		p->mfg_id = (uint16_t)((a >> 24) & 0xffff);
		p->part_id = (uint16_t)((a >> 8) & 0xffff);
		p->class_id = (uint8_t)(a & 0xff);
		ai->periph[m->nperiph] = (ACPI_HANDLE)kids[i];
		p->functions = sdca_function_bits((ACPI_HANDLE)kids[i]);
		m->nperiph++;
		m->sdw_link_mask |= 1u << link;
	}
	return 0;
}

/* ==========================================================================
 * NHLT
 * ========================================================================== */

/* on-disk layout, packed little endian */
#define NHLT_EP_HDR 23u /* length..virtual_bus_id */
#define NHLT_WAVEFMT 40u /* WAVEFORMATEXTENSIBLE */
#define NHLT_LINK_HDA 0
#define NHLT_LINK_DSP 1
#define NHLT_LINK_PDM 2
#define NHLT_LINK_SSP 3
#define NHLT_LINK_SDW 5
#define NHLT_DEV_BT 0
#define NHLT_DEV_I2S 4

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void parse_nhlt(struct sof_dev *sdev, struct sof_acpi_info *ai)
{
	ACPI_TABLE_HEADER *hdr = NULL;
	struct sof_nhlt_info *ni = &ai->nhlt;
	const uint8_t *t, *end, *ep;
	uint32_t count, dmic_geo = 0;

	kmemset(ni, 0, sizeof(*ni));
	if (ACPI_FAILURE(AcpiGetTable((char *)ACPI_SIG_NHLT, 1, &hdr)) || !hdr)
		return;
	if (hdr->Length < sizeof(ACPI_TABLE_HEADER) + 1 ||
	    hdr->Length > (1u << 20)) {
		snd_info("sof", "NHLT has a bad length (%u)\n", hdr->Length);
		return;
	}
	t = (const uint8_t *)hdr;
	end = t + hdr->Length;
	count = t[sizeof(ACPI_TABLE_HEADER)];
	ep = t + sizeof(ACPI_TABLE_HEADER) + 1;
	ni->valid = 1;

	for (uint32_t i = 0; i < count && i < 64; i++) {
		uint32_t len, caps_size, fmt_count;
		uint8_t linktype, devtype, dir, vbus;
		const uint8_t *caps, *fp, *ep_end;
		uint32_t max_ch = 0;

		if ((size_t)(end - ep) < NHLT_EP_HDR + 4)
			break;
		len = rd_le32(ep);
		if (len < NHLT_EP_HDR + 4 || len > (uint32_t)(end - ep))
			break;
		ep_end = ep + len;
		linktype = ep[4];
		devtype = ep[16];
		dir = ep[17];
		vbus = ep[18];
		caps_size = rd_le32(ep + 19);
		caps = ep + NHLT_EP_HDR;
		if (caps_size > (uint32_t)(ep_end - caps)) {
			ep = ep_end;
			continue;
		}
		ni->endpoints++;
		if (dir == 0)
			ni->render_eps++;
		else
			ni->capture_eps++;

		/* formats follow the device specific config */
		fp = caps + caps_size;
		fmt_count = fp < ep_end ? fp[0] : 0;
		fp++;
		for (uint32_t f = 0; f < fmt_count && f < 64; f++) {
			uint32_t fcaps;
			uint16_t ch;
			uint32_t rate;

			if ((size_t)(ep_end - fp) < NHLT_WAVEFMT + 4)
				break;
			ch = rd_le16(fp + 2);
			rate = rd_le32(fp + 4);
			if (ch > max_ch && ch <= 32)
				max_ch = ch;
			if (rate > ni->max_rate && rate <= 384000)
				ni->max_rate = rate;
			fcaps = rd_le32(fp + NHLT_WAVEFMT);
			if (fcaps > (uint32_t)(ep_end - fp) - NHLT_WAVEFMT - 4)
				break;
			fp += NHLT_WAVEFMT + 4 + fcaps;
		}

		switch (linktype) {
		case NHLT_LINK_PDM: {
			uint32_t geo = max_ch;
			ni->dmic_eps++;
			if (max_ch > ni->dmic_max_ch)
				ni->dmic_max_ch = max_ch;
			/* device config: virtual slot, config type, array */
			if (caps_size >= 3 && caps[1] == 1) {
				switch (caps[2] & 0x0f) {
				case 0x0a:
				case 0x0b:
					geo = 2;
					break;
				case 0x0c:
				case 0x0d:
				case 0x0e:
					geo = 4;
					break;
				case 0x0f:
					if (caps_size >= 4 && caps[3] <= 8)
						geo = caps[3];
					break;
				default:
					break;
				}
			}
			if (geo > dmic_geo)
				dmic_geo = geo;
			break;
		}
		case NHLT_LINK_SSP:
			if (vbus < 32) {
				if (devtype == NHLT_DEV_BT)
					ni->ssp_bt_mask |= 1u << vbus;
				else if (devtype == NHLT_DEV_I2S)
					ni->ssp_i2s_mask |= 1u << vbus;
				else
					ni->ssp_other_mask |= 1u << vbus;
			}
			break;
		case NHLT_LINK_SDW:
			ni->sdw_eps++;
			break;
		case NHLT_LINK_HDA:
			ni->hda_eps++;
			break;
		default:
			break;
		}
		ep = ep_end;
	}
	if (dmic_geo > 8)
		dmic_geo = 8;
	ni->dmic_count = dmic_geo;
	sdev->machine.dmic_count = dmic_geo;
	sdev->machine.ssp_mask =
		ni->ssp_bt_mask | ni->ssp_i2s_mask | ni->ssp_other_mask;
}

/* ==========================================================================
 * HDA codec mask: GCTL/STATESTS on BAR0
 * ========================================================================== */

#define HDA_GCAP 0x00
#define HDA_GCTL 0x08
#define HDA_GCTL_CRST 0x1u
#define HDA_WAKEEN 0x0c
#define HDA_STATESTS 0x0e
#define HDA_LLCH 0x14
#define HDA_CAP_ID(h) (((h) >> 16) & 0xfffu)
#define HDA_CAP_NEXT(h) ((h) & 0xffffu)
#define HDA_ML_CAP_ID 0x2
#define HDA_ML_MLCD 0x04
#define HDA_ML_BASE 0x40
#define HDA_ML_INTERVAL 0x40
#define HDA_ML_LCAP 0x00
#define HDA_ML_LCTL 0x04
#define HDA_ML_LCAP_ALT (1u << 28)
#define HDA_ML_LCTL_SPA (1u << 16)
#define HDA_ML_LCTL_CPA (1u << 23)

static inline uint32_t hda_rd32(struct sof_dev *s, uint32_t off)
{
	return *(volatile uint32_t *)(s->hda + off);
}

static inline void hda_wr32(struct sof_dev *s, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(s->hda + off) = v;
}

static inline uint16_t hda_rd16(struct sof_dev *s, uint32_t off)
{
	return *(volatile uint16_t *)(s->hda + off);
}

static inline void hda_wr16(struct sof_dev *s, uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(s->hda + off) = v;
}

/* Find the multi-link capability; 0 if absent. */
static uint32_t hda_find_ml(struct sof_dev *sdev)
{
	uint32_t off;
	int guard = 0;

	if (!sdev->hda || sdev->hda_size < 0x100)
		return 0;
	off = hda_rd16(sdev, HDA_LLCH);
	while (off && off != 0xffff && guard++ < 16) {
		uint32_t h;
		if (off + 8 > sdev->hda_size || (off & 3))
			break;
		h = hda_rd32(sdev, off);
		if (h == 0xffffffffu)
			break;
		if (HDA_CAP_ID(h) == HDA_ML_CAP_ID)
			return off;
		off = HDA_CAP_NEXT(h);
	}
	return 0;
}

/* ACE: the HDA link (the non-extended ML link) needs power before the
 * codecs can answer. */
static void hda_power_hda_link(struct sof_dev *sdev)
{
	uint32_t ml = hda_find_ml(sdev);
	uint32_t nlinks;

	if (!ml)
		return;
	nlinks = (hda_rd32(sdev, ml + HDA_ML_MLCD) & 0xf) + 1;
	for (uint32_t i = 0; i < nlinks && i < 16; i++) {
		uint32_t base = ml + HDA_ML_BASE + HDA_ML_INTERVAL * i;
		uint32_t lcap, lctl;

		if (base + 0x40 > sdev->hda_size)
			break;
		lcap = hda_rd32(sdev, base + HDA_ML_LCAP);
		if (lcap == 0xffffffffu || (lcap & HDA_ML_LCAP_ALT))
			continue; /* an extended (non-HDA) link */
		lctl = hda_rd32(sdev, base + HDA_ML_LCTL);
		if (lctl & HDA_ML_LCTL_CPA)
			continue;
		hda_wr32(sdev, base + HDA_ML_LCTL, lctl | HDA_ML_LCTL_SPA);
		for (int t = 0; t < 100; t++) {
			if (hda_rd32(sdev, base + HDA_ML_LCTL) &
			    HDA_ML_LCTL_CPA)
				break;
			snd_udelay(10);
		}
	}
}

static int hda_wait_crst(struct sof_dev *sdev, int set)
{
	for (int t = 0; t < 1000; t++) {
		uint32_t g = hda_rd32(sdev, HDA_GCTL);
		if (g == 0xffffffffu)
			return -ENODEV;
		if (!!(g & HDA_GCTL_CRST) == !!set)
			return 0;
		snd_udelay(10);
	}
	return -ETIMEDOUT;
}

static void detect_hda_codecs(struct sof_dev *sdev)
{
	uint16_t sts;
	uint32_t g;

	sdev->machine.hda_codec_mask = 0;
	if (!sdev->hda || sdev->hda_size < 0x20)
		return;
	g = hda_rd32(sdev, HDA_GCTL);
	if (g == 0xffffffffu) {
		snd_info("sof", "HDA registers read all ones\n");
		return;
	}
	hda_power_hda_link(sdev);

	sts = hda_rd16(sdev, HDA_STATESTS) & 0x7fff;
	if (!(g & HDA_GCTL_CRST) || !sts) {
		/* A link reset makes every codec report itself again:
		 * enter reset, leave it, and give the codecs the 521 us
		 * the specification allows plus margin. */
		hda_wr16(sdev, HDA_STATESTS, 0x7fff);
		hda_wr32(sdev, HDA_GCTL, g & ~HDA_GCTL_CRST);
		if (hda_wait_crst(sdev, 0)) {
			snd_info("sof", "HDA controller did not enter reset\n");
		}
		snd_udelay(100);
		hda_wr32(sdev, HDA_GCTL, hda_rd32(sdev, HDA_GCTL) |
						 HDA_GCTL_CRST);
		if (hda_wait_crst(sdev, 1)) {
			snd_info("sof", "HDA controller did not leave reset\n");
			return;
		}
		/* poll a little longer than the minimum for slow codecs */
		for (int t = 0; t < 20; t++) {
			snd_udelay(100);
			sts = hda_rd16(sdev, HDA_STATESTS) & 0x7fff;
			if (sts && t >= 6)
				break;
		}
	}
	if (sts == 0x7fff)
		sts = 0; /* a dead link reads all ones */
	sdev->machine.hda_codec_mask = sts;
}

/* ==========================================================================
 * Machine selection
 * ========================================================================== */

#define MFG_CIRRUS 0x01fa
#define MFG_REALTEK 0x025d
#define MFG_MAXIM 0x019f
#define MFG_TI 0x0102
#define MFG_EVEREST 0x00b3 /* placeholder for completeness */

/* short codec names used in topology file names */
struct sdw_codec_name {
	uint16_t mfg, part;
	const char *name;
	int amp; /* speaker amplifier */
	int mic; /* microphone-only codec */
};

static const struct sdw_codec_name g_codec_names[] = {
	{ MFG_CIRRUS, 0x4243, "cs42l43", 0, 0 },
	{ MFG_CIRRUS, 0x2a3b, "cs42l43", 0, 0 },
	{ MFG_CIRRUS, 0x4242, "cs42l42", 0, 0 },
	{ MFG_CIRRUS, 0x4245, "cs42l45", 0, 0 },
	{ MFG_CIRRUS, 0x3556, "cs35l56", 1, 0 },
	{ MFG_CIRRUS, 0x3557, "cs35l57", 1, 0 },
	{ MFG_CIRRUS, 0x3563, "cs35l63", 1, 0 },
	{ MFG_CIRRUS, 0x3554, "cs35l54", 1, 0 },
	{ MFG_REALTEK, 0x0700, "rt700", 0, 0 },
	{ MFG_REALTEK, 0x0711, "rt711", 0, 0 },
	{ MFG_REALTEK, 0x0712, "rt712", 0, 0 },
	{ MFG_REALTEK, 0x0713, "rt713", 0, 0 },
	{ MFG_REALTEK, 0x0714, "rt714", 0, 1 },
	{ MFG_REALTEK, 0x0715, "rt715", 0, 1 },
	{ MFG_REALTEK, 0x0716, "rt716", 0, 1 },
	{ MFG_REALTEK, 0x0721, "rt721", 0, 0 },
	{ MFG_REALTEK, 0x0722, "rt722", 0, 0 },
	{ MFG_REALTEK, 0x5682, "rt5682", 0, 0 },
	{ MFG_REALTEK, 0x1308, "rt1308", 1, 0 },
	{ MFG_REALTEK, 0x1316, "rt1316", 1, 0 },
	{ MFG_REALTEK, 0x1318, "rt1318", 1, 0 },
	{ MFG_REALTEK, 0x1320, "rt1320", 1, 0 },
	{ MFG_REALTEK, 0x1321, "rt1320", 1, 0 },
	{ MFG_REALTEK, 0x1712, "rt1712", 0, 1 },
	{ MFG_REALTEK, 0x1713, "rt1713", 0, 1 },
	{ MFG_REALTEK, 0x1717, "rt1717", 0, 1 },
	{ MFG_MAXIM, 0x8363, "max98363", 1, 0 },
	{ MFG_MAXIM, 0x8373, "max98373", 1, 0 },
	{ MFG_TI, 0x2783, "tas2783", 1, 0 },
	{ MFG_TI, 0x5572, "tac5572", 0, 0 },
	{ MFG_TI, 0x5672, "tac5672", 0, 0 },
	{ MFG_TI, 0x5682, "tac5682", 0, 0 },
};

static const struct sdw_codec_name *codec_name_of(uint16_t mfg, uint16_t part)
{
	for (unsigned i = 0; i < sizeof(g_codec_names) / sizeof(g_codec_names[0]);
	     i++)
		if (g_codec_names[i].mfg == mfg && g_codec_names[i].part == part)
			return &g_codec_names[i];
	return NULL;
}

/* An exact machine entry: every listed (codec, link) must be present and
 * the SoundWire link mask must match.  The table holds only topologies the
 * SOF binary release publishes, each with the variants it publishes (plain,
 * "-2ch" and "-4ch" for two or four PCH digital microphones), so no
 * request goes out for a file that cannot exist.  A machine without an
 * entry is served by the function topologies (ACE) or the HDA generic
 * topology. */
struct sof_mach_req {
	uint16_t mfg, part;
	uint8_t link_mask; /* links the codec may appear on (any of) */
	uint8_t min_count; /* at least this many instances */
};

/* published variants of a machine topology, and how to use it */
#define TV_PLAIN (1u << 0) /* <stem>.tplg */
#define TV_2CH (1u << 1) /* <stem>-2ch.tplg */
#define TV_4CH (1u << 2) /* <stem>-4ch.tplg */
#define TV_MACH_FIRST (1u << 3) /* prefer it to the function topologies */
#define TV_ALL (TV_PLAIN | TV_2CH | TV_4CH)

struct sof_mach_entry {
	const char *plat; /* topology family: "arl", "mtl", "lnl", "ptl", ... */
	uint32_t link_mask; /* exact SoundWire link mask */
	struct sof_mach_req req[4];
	const char *stem; /* file name without ".tplg" */
	uint32_t flags; /* TV_* */
};

#define B(n) (1u << (n))
#define RT(part, links, n) { MFG_REALTEK, (part), (links), (n) }
#define CS(part, links, n) { MFG_CIRRUS, (part), (links), (n) }
#define MX(part, links, n) { MFG_MAXIM, (part), (links), (n) }

static const struct sof_mach_entry g_machines[] = {
	/* ---- Tiger Lake (cAVS 2.5) ---------------------------------------- */
	{ "tgl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1316, B(1), 1), RT(0x1316, B(2), 1),
	    RT(0x0714, B(3), 1) },
	  "sof-tgl-rt711-rt1316-rt714", TV_PLAIN },
	{ "tgl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1308, B(1), 1), RT(0x1308, B(2), 1),
	    RT(0x0715, B(3), 1) },
	  "sof-tgl-rt711-rt1308-rt715", TV_PLAIN },
	{ "tgl", B(0) | B(1) | B(2),
	  { RT(0x0715, B(0), 1), RT(0x0711, B(1), 1), RT(0x1308, B(2), 1) },
	  "sof-tgl-rt715-rt711-rt1308-mono", TV_PLAIN },
	{ "tgl", B(0) | B(1), { RT(0x0711, B(0), 1), RT(0x1308, B(1), 1) },
	  "sof-tgl-rt711-rt1308", TV_4CH },
	{ "tgl", B(0) | B(1) | B(3),
	  { CS(0x4243, B(3), 1), CS(0x3556, B(0), 1), CS(0x3556, B(1), 1) },
	  "sof-tgl-cs42l43-l3-cs35l56-l01", TV_PLAIN },
	{ "tgl", B(0) | B(1), { CS(0x3556, B(0), 4), CS(0x3556, B(1), 4) },
	  "sof-tgl-cs35l56-l01-fb8", TV_PLAIN },
	{ "tgl", B(0) | B(1), { RT(0x0712, B(0), 1), RT(0x1712, B(1), 1) },
	  "sof-tgl-rt712", TV_PLAIN },
	{ "tgl", B(0), { RT(0x0712, B(0), 1) }, "sof-tgl-rt712", TV_PLAIN },

	/* ---- Alder Lake (cAVS 2.5) ---------------------------------------- */
	{ "adl", B(0) | B(2) | B(3),
	  { CS(0x4243, B(0), 1), CS(0x3556, B(2), 1), CS(0x3556, B(3), 1) },
	  "sof-adl-cs42l43-l0-cs35l56-l23", TV_PLAIN },
	{ "adl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1308, B(1), 1), RT(0x1308, B(2), 1),
	    RT(0x0715, B(3), 1) },
	  "sof-adl-rt711-l0-rt1308-l12-rt715-l3", TV_PLAIN },
	{ "adl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1316, B(1), 1), RT(0x1316, B(2), 1),
	    RT(0x0714, B(3), 1) },
	  "sof-adl-rt711-l0-rt1316-l12-rt714-l3", TV_PLAIN },
	{ "adl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1316, B(1), 1), RT(0x1316, B(3), 1),
	    RT(0x0714, B(2), 1) },
	  "sof-adl-rt711-l0-rt1316-l13-rt714-l2", TV_PLAIN },
	{ "adl", B(0), { RT(0x0711, B(0), 1) }, "sof-adl-rt711", TV_4CH },

	/* ---- Raptor Lake (cAVS 2.5) --------------------------------------- */
	{ "rpl", B(0), { CS(0x4243, B(0), 1) }, "sof-rpl-cs42l43-l0", TV_PLAIN },

	/* ---- Meteor Lake (ACE 1.0) ---------------------------------------- */
	{ "mtl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0713, B(0), 1), RT(0x1316, B(1), 1), RT(0x1316, B(2), 1),
	    RT(0x1713, B(3), 1) },
	  "sof-mtl-rt713-l0-rt1316-l12-rt1713-l3", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0713, B(0), 1), RT(0x1318, B(1), 1), RT(0x1318, B(2), 1),
	    RT(0x1713, B(3), 1) },
	  "sof-mtl-rt713-l0-rt1318-l12-rt1713-l3", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(3),
	  { RT(0x0713, B(0), 1), RT(0x1318, B(1), 1), RT(0x1713, B(3), 1) },
	  "sof-mtl-rt713-l0-rt1318-l1-rt1713-l3", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(2),
	  { RT(0x0713, B(0), 1), RT(0x1316, B(1), 1), RT(0x1316, B(2), 1) },
	  "sof-mtl-rt713-l0-rt1316-l12", TV_PLAIN },
	{ "mtl", B(0) | B(3), { RT(0x0712, B(0), 1), RT(0x1712, B(3), 1) },
	  "sof-mtl-rt712-l0-rt1712-l3", TV_PLAIN },
	{ "mtl", B(0), { RT(0x0712, B(0), 1) }, "sof-mtl-rt712-l0", TV_ALL },
	{ "mtl", B(0) | B(1) | B(2),
	  { RT(0x1318, B(1), 1), RT(0x1318, B(2), 1), RT(0x0714, B(0), 1) },
	  "sof-mtl-rt1318-l12-rt714-l0", TV_PLAIN },
	{ "mtl", B(0) | B(2) | B(3),
	  { CS(0x4243, B(0), 1), CS(0x3556, B(2), 1), CS(0x3556, B(3), 1) },
	  "sof-mtl-cs42l43-l0-cs35l56-l23", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(3),
	  { CS(0x4243, B(3), 1), CS(0x3556, B(0), 1), CS(0x3556, B(1), 1) },
	  "sof-mtl-cs42l43-l3-cs35l56-l01-spkagg", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(2),
	  { CS(0x4243, B(0), 1), CS(0x3556, B(1), 1), CS(0x3556, B(2), 1) },
	  "sof-mtl-cs42l43-l0-cs35l56-l12", TV_PLAIN },
	/* amplifier-only boards: eight or six CS35L56 on links 0 and 1 */
	{ "mtl", B(0) | B(1), { CS(0x3556, B(0), 4), CS(0x3556, B(1), 4) },
	  "sof-mtl-cs35l56-l01-fb8", TV_PLAIN },
	{ "mtl", B(0) | B(1), { CS(0x3556, B(0), 3), CS(0x3556, B(1), 3) },
	  "sof-mtl-cs35l56-l01-fb6", TV_PLAIN },
	{ "mtl", B(0), { CS(0x4243, B(0), 1) }, "sof-mtl-cs42l43-l0", TV_PLAIN },
	{ "mtl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1316, B(2), 1), RT(0x1316, B(3), 1),
	    RT(0x0714, B(1), 1) },
	  "sof-mtl-rt711-l0-rt1316-l23-rt714-l1", TV_PLAIN },
	{ "mtl", B(0) | B(3), { RT(0x0711, B(0), 1), RT(0x1316, B(3), 1) },
	  "sof-mtl-rt711-l0-rt1316-l3", TV_ALL },
	{ "mtl", B(0), { RT(0x0722, B(0), 1) }, "sof-mtl-rt722-l0", TV_PLAIN },
	/* only the DMIC variants exist; without DMICs the function
	 * topologies serve this machine */
	{ "mtl", B(0), { RT(0x0711, B(0), 1) }, "sof-mtl-rt711",
	  TV_2CH | TV_4CH },
	{ "mtl", B(0) | B(2), { CS(0x4242, B(0), 1), MX(0x8363, B(2), 1) },
	  "sof-mtl-sdw-cs42l42-l0-max98363-l2", TV_PLAIN },

	/* ---- Arrow Lake (ACE 1.0, Meteor Lake firmware) ------------------- */
	{ "arl", B(0) | B(2) | B(3),
	  { CS(0x4243, B(0), 1), CS(0x3556, B(2), 1), CS(0x3556, B(3), 1) },
	  "sof-arl-cs42l43-l0-cs35l56-l23", TV_PLAIN },
	{ "arl", B(0) | B(2), { CS(0x4243, B(0), 1), CS(0x3556, B(2), 1) },
	  "sof-arl-cs42l43-l0-cs35l56-l2", TV_PLAIN | TV_2CH },
	{ "arl", B(2) | B(3), { CS(0x4243, B(2), 1), CS(0x3556, B(3), 1) },
	  "sof-arl-cs42l43-l2-cs35l56-l3", TV_PLAIN | TV_2CH },
	{ "arl", B(0) | B(2), { RT(0x0722, B(0), 1), RT(0x1320, B(2), 1) },
	  "sof-arl-rt722-l0_rt1320-l2", TV_PLAIN },
	{ "arl", B(0) | B(3), { RT(0x0711, B(0), 1), RT(0x1316, B(3), 1) },
	  "sof-arl-rt711-l0-rt1316-l3", TV_ALL },
	{ "arl", B(2), { CS(0x4243, B(2), 1) }, "sof-arl-cs42l43-l2", TV_PLAIN },
	{ "arl", B(0), { CS(0x4243, B(0), 1) }, "sof-arl-cs42l43-l0", TV_PLAIN },
	{ "arl", B(0), { RT(0x0711, B(0), 1) }, "sof-arl-rt711-l0", TV_ALL },

	/* ---- Lunar Lake (ACE 2.0) ----------------------------------------- */
	{ "lnl", B(0) | B(2) | B(3),
	  { CS(0x4243, B(0), 1), CS(0x3556, B(2), 1), CS(0x3556, B(3), 1) },
	  "sof-lnl-cs42l43-l0-cs35l56-l23", TV_ALL },
	{ "lnl", B(0) | B(3), { CS(0x4243, B(0), 1), CS(0x3556, B(3), 1) },
	  "sof-lnl-cs42l43-l0-cs35l56-l3", TV_PLAIN | TV_2CH },
	/* six CS35L56, three on link 1 and three on link 3 */
	{ "lnl", B(1) | B(2) | B(3),
	  { CS(0x4243, B(2), 1), CS(0x3556, B(1), 3), CS(0x3556, B(3), 3) },
	  "sof-lnl-cs42l43-l2-cs35l56x6-l13", TV_PLAIN },
	{ "lnl", B(0), { CS(0x4243, B(0), 1) }, "sof-lnl-cs42l43-l0", TV_ALL },
	{ "lnl", B(0), { RT(0x0722, B(0), 1) }, "sof-lnl-rt722-l0", TV_PLAIN },
	{ "lnl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1316, B(2), 1), RT(0x1316, B(3), 1),
	    RT(0x0714, B(1), 1) },
	  "sof-lnl-rt711-l0-rt1316-l23-rt714-l1", TV_PLAIN },
	{ "lnl", B(0), { RT(0x0711, B(0), 1) }, "sof-lnl-rt711", TV_4CH },
	{ "lnl", B(2) | B(3), { RT(0x0712, B(2), 1), RT(0x1712, B(3), 1) },
	  "sof-lnl-rt712-l2-rt1712-l3", TV_PLAIN },
	{ "lnl", B(1) | B(2), { RT(0x0712, B(2), 1), RT(0x1320, B(1), 1) },
	  "sof-lnl-rt712-l2-rt1320-l1", TV_PLAIN },
	{ "lnl", B(1) | B(2) | B(3),
	  { RT(0x0713, B(2), 1), RT(0x1320, B(1), 1), RT(0x1320, B(3), 1) },
	  "sof-lnl-rt713-l2-rt1320-l13", TV_PLAIN },
	{ "lnl", B(0) | B(1), { RT(0x0713, B(0), 1), RT(0x1318, B(1), 1) },
	  "sof-lnl-rt713-l0-rt1318-l1", TV_PLAIN | TV_2CH },
	{ "lnl", B(0) | B(1) | B(2),
	  { RT(0x1318, B(1), 1), RT(0x1318, B(2), 1), RT(0x0714, B(0), 1) },
	  "sof-lnl-rt1318-l12-rt714-l0", TV_PLAIN },
	{ "lnl", B(0) | B(1) | B(2),
	  { RT(0x1320, B(1), 1), RT(0x1320, B(2), 1), RT(0x0714, B(0), 1) },
	  "sof-lnl-rt1320-l12-rt714-l0", TV_PLAIN },

	/* ---- Panther Lake and Wildcat Lake (ACE 3.0) ---------------------- */
	/* CS42L43 jack on link 3 with its speaker path aggregated with two
	 * CS35L56 on link 2: only the machine topology describes that */
	{ "ptl", B(2) | B(3), { CS(0x4243, B(3), 1), CS(0x3556, B(2), 1) },
	  "sof-ptl-cs42l43-agg-l3-cs35l56-l2",
	  TV_PLAIN | TV_4CH | TV_MACH_FIRST },
	{ "ptl", B(1) | B(2) | B(3),
	  { CS(0x4243, B(2), 1), CS(0x3556, B(1), 3), CS(0x3556, B(3), 3) },
	  "sof-ptl-cs42l43-l2-cs35l56x6-l13", TV_PLAIN },
	{ "ptl", B(0) | B(2) | B(3),
	  { RT(0x0722, B(0), 1), RT(0x1320, B(2), 1), RT(0x1320, B(3), 1) },
	  "sof-ptl-rt722-l0-rt1320-l23", TV_ALL | TV_MACH_FIRST },
	{ "ptl", B(0), { RT(0x0722, B(0), 1) }, "sof-ptl-rt722", TV_ALL },
	{ "ptl", B(3), { RT(0x0722, B(3), 1) }, "sof-ptl-rt722", TV_ALL },
	{ "ptl", B(0), { RT(0x0721, B(0), 1) }, "sof-ptl-rt721",
	  TV_PLAIN | TV_4CH },
	{ "ptl", B(0), { RT(0x0711, B(0), 1) }, "sof-ptl-rt711", TV_ALL },
	{ "ptl", B(0) | B(1) | B(2) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1308, B(1), 1), RT(0x1308, B(2), 1),
	    RT(0x0715, B(3), 1) },
	  "sof-ptl-rt711-rt1308-rt715", TV_PLAIN },
	{ "ptl", B(0) | B(1) | B(3),
	  { RT(0x0711, B(0), 1), RT(0x1308, B(1), 1), RT(0x0715, B(3), 1) },
	  "sof-ptl-rt711-rt1308-mono-rt715", TV_PLAIN },
	{ "ptl", B(0) | B(1) | B(2),
	  { RT(0x0715, B(0), 1), RT(0x0711, B(1), 1), RT(0x1308, B(2), 1) },
	  "sof-ptl-rt715-rt711-rt1308-mono", TV_PLAIN },
	{ "ptl", B(1) | B(2) | B(3),
	  { RT(0x0713, B(2), 1), RT(0x1320, B(1), 1), RT(0x1320, B(3), 1) },
	  "sof-ptl-rt713-l2-rt1320-l13", TV_PLAIN },
	{ "ptl", B(1) | B(2) | B(3),
	  { RT(0x0713, B(3), 1), RT(0x1320, B(1), 1), RT(0x1320, B(2), 1) },
	  "sof-ptl-rt713-l3-rt1320-l12", TV_PLAIN },
	{ "ptl", B(1) | B(3), { RT(0x0713, B(3), 1), RT(0x1320, B(1), 1) },
	  "sof-ptl-rt713-l3-rt1320-l1", TV_2CH },
	{ "ptl", B(1) | B(2), { RT(0x0712, B(2), 1), RT(0x1320, B(1), 1) },
	  "sof-ptl-rt712-l2-rt1320-l1", TV_PLAIN },
	{ "ptl", B(2) | B(3), { RT(0x0712, B(3), 1), RT(0x1320, B(2), 1) },
	  "sof-ptl-rt712-l3-rt1320-l2", TV_PLAIN },
	{ "ptl", B(3), { RT(0x0712, B(3), 1), RT(0x1320, B(3), 1) },
	  "sof-ptl-rt712-l3-rt1320-l3", TV_PLAIN | TV_4CH },
};

#undef RT
#undef CS
#undef MX

struct sof_plat_id {
	uint16_t device;
	const char *plat; /* firmware directory / name stem */
	int ace; /* 0 cAVS, 1 ACE1.x, 2 ACE2.x, 3 ACE3.x */
};

static const struct sof_plat_id g_plats[] = {
	/* cAVS 1.8 / 2.0 */
	{ 0x9dc8, "cnl", 0 },
	{ 0x02c8, "cml", 0 },
	{ 0x06c8, "cml", 0 },
	{ 0xa3f0, "cml", 0 },
	{ 0x34c8, "icl", 0 },
	{ 0x38c8, "icl", 0 },
	{ 0x4dc8, "jsl", 0 },
	{ 0x4b55, "ehl", 0 },
	{ 0x4b58, "ehl", 0 },
	/* cAVS 2.5 */
	{ 0xa0c8, "tgl", 0 },
	{ 0x43c8, "tgl-h", 0 },
	{ 0xf1c8, "tgl-h", 0 }, /* Rocket Lake-S: the Tiger Lake-H DSP */
	{ 0x51c8, "adl", 0 },
	{ 0x51c9, "adl", 0 },
	{ 0x51cc, "adl", 0 },
	{ 0x51cd, "adl", 0 },
	{ 0x7ad0, "adl-s", 0 },
	{ 0x54c8, "adl-n", 0 },
	{ 0x51ca, "rpl", 0 },
	{ 0x51cb, "rpl", 0 },
	{ 0x51ce, "rpl", 0 },
	{ 0x51cf, "rpl", 0 }, /* Raptor Lake-PX */
	{ 0x7a50, "rpl-s", 0 },
	/* ACE 1.x */
	{ 0x7e28, "mtl", 1 },
	{ 0x7728, "arl", 1 },
	{ 0x7f50, "arl-s", 1 },
	/* ACE 2.x */
	{ 0xa828, "lnl", 2 },
	/* ACE 3.x */
	{ 0xe428, "ptl", 3 },
	{ 0xe328, "ptl", 3 },
	{ 0x4d28, "wcl", 3 },
};

static void pick_platform(struct sof_dev *sdev, struct sof_acpi_info *ai)
{
	const char *plat = NULL;
	int ace = -1;

	if (sdev->pci) {
		for (unsigned i = 0; i < sizeof(g_plats) / sizeof(g_plats[0]);
		     i++) {
			if (g_plats[i].device == sdev->pci->device_id) {
				plat = g_plats[i].plat;
				ace = g_plats[i].ace;
				break;
			}
		}
	}
	if (!plat) {
		switch (sdev->platform) {
		case SOF_PLAT_PTL:
			plat = "ptl";
			ace = 3;
			break;
		case SOF_PLAT_LNL:
			plat = "lnl";
			ace = 2;
			break;
		case SOF_PLAT_MTL:
			plat = "mtl";
			ace = 1;
			break;
		case SOF_PLAT_TGL:
			plat = "tgl";
			ace = 0;
			break;
		case SOF_PLAT_ICL:
			plat = "icl";
			ace = 0;
			break;
		case SOF_PLAT_CNL:
			plat = "cnl";
			ace = 0;
			break;
		case SOF_PLAT_APL:
			plat = "apl";
			ace = 0;
			break;
		case SOF_PLAT_SKL:
			plat = "skl";
			ace = 0;
			break;
		default:
			plat = "mtl";
			ace = 1;
			break;
		}
	}
	acpi_strlcpy(ai->plat, plat, sizeof(ai->plat));
	ai->ace = ace < 0 ? 0 : ace;
}

/* Topology family of a platform: "arl-s" -> "arl", "tgl-h" -> "tgl".
 * Wildcat Lake has no topologies of its own: it is a Panther Lake
 * derivative and its machines use the Panther Lake ones. */
static void plat_family(const char *plat, char *out, size_t len)
{
	size_t i = 0;

	if (!kstrcmp(plat, "wcl")) {
		acpi_strlcpy(out, "ptl", len);
		return;
	}
	for (; plat[i] && plat[i] != '-' && i + 1 < len; i++)
		out[i] = plat[i];
	out[i] = 0;
}

static int mach_count(const struct sof_machine *m, uint16_t mfg,
		      uint16_t part, uint32_t links)
{
	int n = 0;

	for (int i = 0; i < m->nperiph; i++) {
		const struct sof_sdw_peripheral *p = &m->periph[i];
		uint16_t pp = p->part_id;
		/* the CS42L43B variant shares the topologies */
		if (mfg == MFG_CIRRUS && part == 0x4243 && pp == 0x2a3b)
			pp = 0x4243;
		/* RT1320 has two part numbers */
		if (mfg == MFG_REALTEK && part == 0x1320 && pp == 0x1321)
			pp = 0x1320;
		if (p->mfg_id == mfg && pp == part &&
		    (links & (1u << p->link)))
			n++;
	}
	return n;
}

static const struct sof_mach_entry *match_machine(const struct sof_machine *m,
						  const char *family)
{
	for (unsigned i = 0; i < sizeof(g_machines) / sizeof(g_machines[0]);
	     i++) {
		const struct sof_mach_entry *e = &g_machines[i];
		int ok = 1;

		if (kstrcmp(e->plat, family))
			continue;
		if (e->link_mask != m->sdw_link_mask)
			continue;
		for (int r = 0; r < 4 && e->req[r].mfg; r++) {
			if (mach_count(m, e->req[r].mfg, e->req[r].part,
				       e->req[r].link_mask) <
			    e->req[r].min_count) {
				ok = 0;
				break;
			}
		}
		if (ok)
			return e;
	}
	return NULL;
}

static void add_cand(struct sof_acpi_info *ai, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void add_cand(struct sof_acpi_info *ai, const char *fmt, ...)
{
	va_list ap;
	char tmp[SOF_ACPI_NAME_LEN];

	if (ai->ncand >= SOF_ACPI_MAX_CAND)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(tmp, sizeof(tmp), fmt, ap);
	__builtin_va_end(ap);
	for (int i = 0; i < ai->ncand; i++)
		if (!kstrcmp(ai->cand[i], tmp))
			return;
	acpi_strlcpy(ai->cand[ai->ncand++], tmp, SOF_ACPI_NAME_LEN);
}

/* "cs42l43-l0-cs35l56-l23" from the found peripherals: the jack codec
 * first, then the amplifiers, then the microphone codecs, each with the
 * links it sits on.  Only for the log: it says which machine topology a
 * board without a table entry would want. */
static void build_generic_name(const struct sof_machine *m, char *out,
			       size_t len)
{
	size_t pos = 0;
	uint32_t done = 0; /* periph indexes already named */

	out[0] = 0;
	for (int pass = 0; pass < 3; pass++) {
		for (int i = 0; i < m->nperiph; i++) {
			const struct sdw_codec_name *cn;
			uint32_t links = 0;
			char lbuf[8];
			int k = 0;

			if (done & (1u << i))
				continue;
			cn = codec_name_of(m->periph[i].mfg_id,
					   m->periph[i].part_id);
			if (!cn)
				continue;
			if ((pass == 0 && (cn->amp || cn->mic)) ||
			    (pass == 1 && !cn->amp) || (pass == 2 && !cn->mic))
				continue;
			/* collect every instance of the same codec */
			for (int j = 0; j < m->nperiph; j++) {
				const struct sdw_codec_name *c2 = codec_name_of(
					m->periph[j].mfg_id,
					m->periph[j].part_id);
				if (c2 && !kstrcmp(c2->name, cn->name)) {
					links |= 1u << m->periph[j].link;
					done |= 1u << j;
				}
			}
			for (int l = 0; l < SOF_MAX_SDW_LINKS && k < 6; l++)
				if (links & (1u << l))
					lbuf[k++] = (char)('0' + l);
			lbuf[k] = 0;
			pos = sdw_dump_printf(out, len, pos, "%s%s-l%s",
					      pos ? "-" : "", cn->name, lbuf);
		}
	}
}

/* ==========================================================================
 * Signing key: SMBIOS identification
 *
 * The audio DSP of every generation handled here boots only images signed
 * with the key fused into the part.  Retail machines carry the Intel
 * production key and run the Intel-signed images; Chromebooks (whose
 * firmware the vendor writes itself) and the AAEON UP boards carry the
 * community key and run the images in the "community" directories.  The
 * fuses cannot be read from the host, so the machine is recognised from
 * its SMBIOS strings:
 *   - system or baseboard manufacturer containing "AAEON" (UP boards),
 *   - system family containing "Google" (Chromebooks),
 *   - BIOS version containing "Google" (Google firmware builds).
 *
 * The SMBIOS entry point is the one the bootloader took from the UEFI
 * configuration tables, else it is looked up in the legacy BIOS area
 * (0xF0000-0xFFFFF, 16-byte aligned anchors "_SM3_" and "_SM_"), where
 * PC firmware and the Chromebook firmware place it.
 * ========================================================================== */

#define SMBIOS_SCAN_BASE 0xF0000ull
#define SMBIOS_SCAN_LEN 0x10000u
#define SMBIOS_TABLE_MAX (1024u * 1024u) /* sanity bound on the table */
#define SMBIOS_STR_LEN 64

struct smbios_ids {
	char bios_version[SMBIOS_STR_LEN];
	char sys_vendor[SMBIOS_STR_LEN];
	char product_family[SMBIOS_STR_LEN];
	char board_vendor[SMBIOS_STR_LEN];
};

/* 0: not looked at yet, 1: production key, 2: community key,
 * 3: no SMBIOS found (production key assumed) */
static volatile int g_key_state;

static int smbios_checksum_ok(const uint8_t *p, uint32_t len)
{
	uint8_t sum = 0;

	for (uint32_t i = 0; i < len; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum == 0;
}

/* Copy string number `idx' (1-based) of the structure whose formatted
 * area is [s, s + flen) and whose string set ends before `end'. */
static void smbios_string(const uint8_t *s, uint32_t flen, const uint8_t *end,
			  uint8_t idx, char *out, size_t outlen)
{
	const uint8_t *p = s + flen;
	uint8_t n = 1;
	size_t k = 0;

	out[0] = 0;
	if (!idx || outlen == 0)
		return;
	while (p < end && *p) {
		if (n == idx) {
			while (p < end && *p && k + 1 < outlen) {
				uint8_t c = *p++;
				out[k++] = (c >= 32 && c < 127) ? (char)c : '?';
			}
			out[k] = 0;
			return;
		}
		while (p < end && *p)
			p++;
		p++; /* the string's NUL */
		n++;
	}
}

/* Walk the structure table; fills `ids' from types 0, 1 and 2. */
static void smbios_walk(const uint8_t *t, uint32_t len, uint32_t max_count,
			struct smbios_ids *ids)
{
	uint32_t pos = 0, count = 0;

	while (pos + 4 <= len && (!max_count || count < max_count)) {
		uint8_t type = t[pos], flen = t[pos + 1];
		const uint8_t *s = t + pos;
		const uint8_t *end = t + len;
		uint32_t next;

		if (flen < 4 || pos + flen > len)
			break;
		/* the string set ends with two NULs */
		next = pos + flen;
		while (next + 1 < len && (t[next] || t[next + 1]))
			next++;
		if (next + 1 >= len)
			end = t + len;
		else
			end = t + next + 1;

		switch (type) {
		case 0: /* BIOS information */
			if (flen > 0x05)
				smbios_string(s, flen, end, s[0x05],
					      ids->bios_version,
					      sizeof(ids->bios_version));
			break;
		case 1: /* system information */
			if (flen > 0x04)
				smbios_string(s, flen, end, s[0x04],
					      ids->sys_vendor,
					      sizeof(ids->sys_vendor));
			if (flen > 0x1a)
				smbios_string(s, flen, end, s[0x1a],
					      ids->product_family,
					      sizeof(ids->product_family));
			break;
		case 2: /* baseboard information */
			if (flen > 0x04)
				smbios_string(s, flen, end, s[0x04],
					      ids->board_vendor,
					      sizeof(ids->board_vendor));
			break;
		default:
			break;
		}
		if (type == 127)
			break; /* end of table */
		pos = next + 2;
		count++;
	}
}

/* Map and walk the structure table at `phys'. */
static int smbios_read_table(uint64_t phys, uint32_t len, uint32_t count,
			     struct smbios_ids *ids)
{
	const uint8_t *t;

	if (!phys || len < 4)
		return -1;
	if (len > SMBIOS_TABLE_MAX)
		len = SMBIOS_TABLE_MAX;
	t = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)phys, len);
	if (!t)
		return -1;
	smbios_walk(t, len, count, ids);
	AcpiOsUnmapMemory((void *)t, len);
	return 0;
}

/* Decode an entry point at `e' (`avail' readable bytes): the 64-bit
 * "_SM3_" one, or the 32-bit "_SM_" one with its "_DMI_" anchor.  Fills
 * the structure table's address, length and count (0 = up to type 127). */
static int smbios_parse_entry(const uint8_t *e, uint32_t avail, int want3,
			      uint64_t *table, uint32_t *tlen, uint32_t *tcount)
{
	if (want3) {
		uint8_t elen;

		if (avail < 0x18 || kmemcmp(e, "_SM3_", 5))
			return 0;
		elen = e[6];
		if (elen < 0x18 || elen > avail || !smbios_checksum_ok(e, elen))
			return 0;
		*tlen = (uint32_t)e[0x0c] | ((uint32_t)e[0x0d] << 8) |
			((uint32_t)e[0x0e] << 16) | ((uint32_t)e[0x0f] << 24);
		*table = 0;
		for (int b = 7; b >= 0; b--)
			*table = (*table << 8) | e[0x10 + b];
		*tcount = 0;
		return 1;
	} else {
		uint8_t elen;

		if (avail < 0x1f || kmemcmp(e, "_SM_", 4))
			return 0;
		elen = e[5];
		if (elen < 0x1f || elen > avail || !smbios_checksum_ok(e, elen))
			return 0;
		/* the intermediate "_DMI_" anchor, own checksum */
		if (kmemcmp(e + 0x10, "_DMI_", 5) ||
		    !smbios_checksum_ok(e + 0x10, 0x0f))
			return 0;
		*tlen = (uint32_t)e[0x16] | ((uint32_t)e[0x17] << 8);
		*table = (uint64_t)e[0x18] | ((uint64_t)e[0x19] << 8) |
			 ((uint64_t)e[0x1a] << 16) | ((uint64_t)e[0x1b] << 24);
		*tcount = (uint32_t)e[0x1c] | ((uint32_t)e[0x1d] << 8);
		return 1;
	}
}

/* An entry point the bootloader took from the UEFI configuration tables. */
static int smbios_try_boot_entry(uint64_t phys, int want3, uint64_t *table,
				 uint32_t *tlen, uint32_t *tcount)
{
	const uint8_t *e;
	int ok;

	if (!phys)
		return 0;
	e = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)phys, 0x20);
	if (!e)
		return 0;
	ok = smbios_parse_entry(e, 0x20, want3, table, tlen, tcount);
	AcpiOsUnmapMemory((void *)e, 0x20);
	return ok;
}

static int smbios_find(struct smbios_ids *ids)
{
	const uint8_t *area;
	uint64_t table = 0;
	uint32_t tlen = 0, tcount = 0;
	int found;

	/* UEFI: the entry points the bootloader reported (the 64-bit one
	 * first: it may describe a table above 4 GB) */
	found = smbios_try_boot_entry(g_boot_smbios3_entry, 1, &table, &tlen,
				      &tcount) ||
		smbios_try_boot_entry(g_boot_smbios_entry, 0, &table, &tlen,
				      &tcount);
	if (found)
		return smbios_read_table(table, tlen, tcount, ids);

	/* else the legacy BIOS area, on 16-byte boundaries */
	area = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)SMBIOS_SCAN_BASE,
			       SMBIOS_SCAN_LEN);
	if (!area)
		return -1;
	for (int want3 = 1; want3 >= 0 && !found; want3--)
		for (uint32_t off = 0; off + 0x18 <= SMBIOS_SCAN_LEN && !found;
		     off += 16)
			found = smbios_parse_entry(area + off,
						   SMBIOS_SCAN_LEN - off, want3,
						   &table, &tlen, &tcount);
	AcpiOsUnmapMemory((void *)area, SMBIOS_SCAN_LEN);
	if (!found)
		return -1;
	return smbios_read_table(table, tlen, tcount, ids);
}

/* Case-sensitive substring test, as the vendors write their names. */
static int str_has(const char *s, const char *needle)
{
	size_t n = kstrlen(needle);

	if (!n)
		return 1;
	for (; *s; s++)
		if (!kstrncmp(s, needle, n))
			return 1;
	return 0;
}

/* Nonzero when the DSP of this machine runs community-signed firmware. */
static int community_key_machine(void)
{
	struct smbios_ids ids;
	int state = g_key_state;
	const char *why = NULL;

	if (state)
		return state == 2;
	kmemset(&ids, 0, sizeof(ids));
	if (smbios_find(&ids) < 0) {
		snd_info("sof", "no SMBIOS entry point in the BIOS area; assuming the Intel production key\n");
		g_key_state = 3;
		return 0;
	}
	if (str_has(ids.sys_vendor, "AAEON") ||
	    str_has(ids.board_vendor, "AAEON"))
		why = "UP board";
	else if (str_has(ids.product_family, "Google"))
		why = "Chromebook";
	else if (str_has(ids.bios_version, "Google"))
		why = "Google firmware";
	snd_info("sof",
		 "SMBIOS: vendor \"%s\", board \"%s\", family \"%s\", BIOS \"%s\": %s%s%s\n",
		 ids.sys_vendor, ids.board_vendor, ids.product_family,
		 ids.bios_version, why ? "community key (" : "Intel production key",
		 why ? why : "", why ? ")" : "");
	g_key_state = why ? 2 : 1;
	return why != NULL;
}

/* ==========================================================================
 * Firmware and topology names
 * ========================================================================== */

/* Candidates for a machine table entry, in order: the DMIC variant that
 * fits the microphones (four or more take "-4ch" and fall back to "-2ch",
 * one to three take "-2ch"), then the plain topology.  Only variants the
 * entry says are published are named. */
static void add_machine_cands(struct sof_acpi_info *ai, const char *tdir,
			      const struct sof_mach_entry *e, uint32_t dmic)
{
	if (dmic >= 4 && (e->flags & TV_4CH))
		add_cand(ai, "%s/%s-4ch.tplg", tdir, e->stem);
	if (dmic && (e->flags & TV_2CH))
		add_cand(ai, "%s/%s-2ch.tplg", tdir, e->stem);
	if (e->flags & TV_PLAIN)
		add_cand(ai, "%s/%s.tplg", tdir, e->stem);
}

/* The HDA generic topologies (HDMI and/or an analog HDA codec, PCH DMICs).
 * The architecture variants (-cavs25, -ace1 for ACE 1.x and 2.x, -ace3)
 * carry their own DMIC configuration blob and exist only with DMICs; the
 * plain ones rely on the BIOS NHLT.  Published DMIC variants: "-1ch"
 * (not with "-idisp"), "-2ch", "-4ch". */
static void add_hda_generic_cands(struct sof_acpi_info *ai, const char *tdir,
				  int ace, int hdmi_only, uint32_t dmic)
{
	const char *arch = ace >= 3 ? "-ace3" : (ace >= 1 ? "-ace1" : "-cavs25");
	const char *idisp = hdmi_only ? "-idisp" : "";
	uint32_t ch;

	if (dmic >= 4)
		ch = 4;
	else if (dmic >= 2)
		ch = 2;
	else if (dmic == 1)
		ch = hdmi_only ? 2 : 1; /* no "-idisp-1ch" is published */
	else
		ch = 0;

	if (ch) {
		add_cand(ai, "%s/sof-hda-generic%s%s-%uch.tplg", tdir, arch,
			 idisp, ch);
		add_cand(ai, "%s/sof-hda-generic%s-%uch.tplg", tdir, idisp, ch);
	}
	if (hdmi_only)
		add_cand(ai, "%s/sof-hda-generic-idisp.tplg", tdir);
	add_cand(ai, "%s/sof-hda-generic.tplg", tdir);
}

static void choose_names(struct sof_dev *sdev, struct sof_acpi_info *ai)
{
	struct sof_machine *m = &sdev->machine;
	const struct sof_mach_entry *e = NULL;
	char family[16];
	char generic[72];
	/* every IPC4 platform keeps its topologies in this one directory */
	const char *tdir = "intel/sof-ipc4-tplg";
	/* display codecs sit at address 2 (and up); 0/1 are analog */
	int hdmi_only = m->hda_codec_mask && !(m->hda_codec_mask & 0x3);
	uint32_t dmic = m->dmic_count;

	plat_family(ai->plat, family, sizeof(family));
	ai->community = community_key_machine();
	/* Elkhart Lake has only a community-signed build */
	if (ai->community || !kstrcmp(ai->plat, "ehl")) {
		ksnprintf(m->fw_name, sizeof(m->fw_name),
			  "intel/sof-ipc4/%s/community/sof-%s.ri", ai->plat,
			  ai->plat);
		ksnprintf(m->fw_lib_path, sizeof(m->fw_lib_path),
			  "intel/sof-ipc4-lib/%s/community", ai->plat);
	} else {
		ksnprintf(m->fw_name, sizeof(m->fw_name),
			  "intel/sof-ipc4/%s/sof-%s.ri", ai->plat, ai->plat);
		ksnprintf(m->fw_lib_path, sizeof(m->fw_lib_path),
			  "intel/sof-ipc4-lib/%s", ai->plat);
	}

	acpi_strlcpy(ai->tplg_dir, tdir, sizeof(ai->tplg_dir));
	ai->ncand = 0;
	ai->machine_first = 0;

	if (m->nperiph > 0) {
		e = match_machine(m, family);
		if (e) {
			add_machine_cands(ai, tdir, e, dmic);
			ai->machine_first = (e->flags & TV_MACH_FIRST) != 0;
		} else {
			build_generic_name(m, generic, sizeof(generic));
			snd_info("sof",
				 "no machine topology for sof-%s-%s on links %#x\n",
				 family, generic[0] ? generic : "(unknown codecs)",
				 m->sdw_link_mask);
		}
	}
	add_hda_generic_cands(ai, tdir, ai->ace, hdmi_only, dmic);

	if (ai->ncand > 0)
		acpi_strlcpy(m->tplg_name, ai->cand[0], sizeof(m->tplg_name));
	else
		m->tplg_name[0] = 0;
}

/* Nonzero when the machine topology must be tried before the function
 * topologies (boards whose audio paths only it describes, such as
 * amplifiers aggregated with a headset codec's speaker path). */
int sof_acpi_machine_tplg_first(struct sof_dev *sdev)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);

	return ai ? ai->machine_first : 0;
}

const char *sof_acpi_tplg_candidate(struct sof_dev *sdev, int idx)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);

	if (!ai || idx < 0 || idx >= ai->ncand)
		return NULL;
	return ai->cand[idx];
}

const char *sof_acpi_tplg_dir(struct sof_dev *sdev)
{
	struct sof_acpi_info *ai = acpi_info_get(sdev, 0);
	return ai ? ai->tplg_dir : NULL;
}

/* ==========================================================================
 * Entry point
 * ========================================================================== */

static void log_machine(struct sof_dev *sdev, struct sof_acpi_info *ai)
{
	struct sof_machine *m = &sdev->machine;

	snd_info("sof",
		 "machine: %s (ACE %d), HDA codecs 0x%04x, DMICs %u, SSP 0x%x (BT 0x%x)\n",
		 ai->plat, ai->ace, m->hda_codec_mask, m->dmic_count,
		 m->ssp_mask, ai->nhlt.ssp_bt_mask);
	if (ai->hdas_path[0])
		snd_info("sof", "ACPI companion %s%s\n", ai->hdas_path,
			 ai->sndw ? " with SoundWire controller" : "");
	for (int i = 0; i < m->nperiph; i++) {
		const struct sof_sdw_peripheral *p = &m->periph[i];
		const struct sdw_codec_name *cn =
			codec_name_of(p->mfg_id, p->part_id);
		snd_info("sof",
			 "SoundWire link %d: %s (mfg 0x%04x part 0x%04x class 0x%02x uid %u ver %u, SDCA functions 0x%x)\n",
			 p->link, cn ? cn->name : "unknown", p->mfg_id,
			 p->part_id, p->class_id, p->unique_id, p->version,
			 p->functions);
	}
	for (int l = 0; l < SOF_MAX_SDW_LINKS; l++) {
		const struct sdw_master_props *mp = &ai->mprops[l];
		if (!(m->sdw_link_mask & (1u << l)))
			continue;
		snd_info("sof",
			 "SoundWire link %d: bus %u Hz, mclk %u Hz, frame %u Hz %ux%u\n",
			 l, mp->max_clk_freq, mp->mclk_freq,
			 mp->default_frame_rate, mp->default_row,
			 mp->default_col);
	}
	snd_info("sof", "firmware %s, library %s\n", m->fw_name,
		 m->fw_lib_path);
	snd_info("sof", "topology %s\n",
		 m->tplg_name[0] ? m->tplg_name : "(none)");
}

int sof_acpi_scan(struct sof_dev *sdev)
{
	struct sof_acpi_info *ai;
	struct sof_machine *m;

	if (!sdev)
		return -EINVAL;
	ai = acpi_info_get(sdev, 1);
	if (!ai)
		return -ENOMEM;
	m = &sdev->machine;
	kmemset(m, 0, sizeof(*m));
	kmemset(ai->periph, 0, sizeof(ai->periph));
	ai->hdas = ai->sndw = NULL;
	ai->hdas_path[0] = 0;
	ai->link_count = 0;
	ai->link_list = 0;
	for (int l = 0; l < SOF_MAX_SDW_LINKS; l++)
		master_props_defaults(&ai->mprops[l], 0);

	pick_platform(sdev, ai);

	/* 1. HDA codecs (HDMI, sometimes an analog codec too) */
	detect_hda_codecs(sdev);

	/* 2. NHLT: DMIC count and SSP links */
	parse_nhlt(sdev, ai);

	/* 3. the SoundWire controller and its peripherals */
	if (sdev->pci) {
		find_hda_companion(ai, sdev->pci);
		if (ai->sndw && (sdev->platform == SOF_PLAT_UNKNOWN ||
				 sdev->platform >= SOF_PLAT_CNL)) {
			if (scan_sdw_controller(sdev, ai))
				m->sdw_link_mask = 0;
		} else if (ai->sndw) {
			snd_info("sof",
				 "SoundWire controller ignored on this generation\n");
		}
	}

	/* 4. firmware and topology */
	choose_names(sdev, ai);
	log_machine(sdev, ai);
	return 0;
}
