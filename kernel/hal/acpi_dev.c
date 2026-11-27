// LikeOS ACPI - device layer over the ACPI namespace
//
// The functions declared in acpi_dev.h, implemented on ACPICA.  Three
// conversions do most of the work here:
//
//   * namespace objects to struct acpi_device: AcpiGetObjectInfo gives
//     _HID, _UID, _ADR and the _CID list in one evaluation, AcpiGetName
//     the full path; _STA is evaluated on its own (a device without _STA
//     is present, enabled and functioning by definition);
//   * ACPI_OBJECT trees to struct acpi_value trees and back: arguments go
//     in as ACPI_OBJECT arrays built on the stack or in one allocation,
//     results come out of ACPICA's own buffer and are copied into one
//     allocation of ours (values, element arrays, string and buffer bytes
//     packed together), so a caller frees a whole result with one call;
//   * ACPI_RESOURCE descriptors to struct acpi_res: every descriptor in
//     _CRS order, interrupts expanded to one entry per interrupt, ISA
//     IRQ descriptors translated through the MADT interrupt overrides as
//     the firmware tables say they must be.
//
// Power: devices are moved between D-states the way the ACPI spec lays
// it out (and the established OS implementation does it): power
// resources are shared objects with a reference count; turning a device
// on references the resources its target state lists (_PRx) before _PSx
// runs, turning it off runs _PSx first and lets go of its holds after.
// The first reference of a resource runs its _ON (unless the firmware
// has it on already), the last release its _OFF; the references start
// from every present device in a powered state, the first time power
// management is used.  Per-resource and per-device locks (AML sleeps).
//
// _DEP: a supplier is only waited for when it is one of the kinds that
// really gate their consumers (camera PMICs and the vision controller);
// everything else -- in particular the GPIO and I2C controllers touchpads
// list -- is handled by deferring the consumer's probe until its resources
// can be resolved, not by holding enumeration back.
//
// Copyright (C) 2026 The LikeOS Project

/* The ACPICA headers carry unused parameters in their stubs. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "acpica/include/acpi.h"
#pragma GCC diagnostic pop

#include <kernel/hal/acpi.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/irq.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define ACPI_DEV_MAX_ARGS 7
#define ACPI_VALUE_MAX_DEPTH 8 /* nested packages in a result */
#define ACPI_POWER_MAX_RES 16 /* power resources per _PRx list */
#define ACPI_SUPPLIERS_MAX 64

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static void str_copy(char *dst, size_t cap, const char *src, size_t srclen)
{
	size_t i = 0;

	if (cap == 0)
		return;
	if (src)
		for (; i < srclen && src[i] && i + 1 < cap; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
	return kstrcmp(a, b) == 0;
}

static int status_to_errno(ACPI_STATUS st)
{
	if (ACPI_SUCCESS(st))
		return 0;
	switch (st) {
	case AE_NOT_FOUND:
	case AE_NOT_EXIST:
		return -ENOENT;
	case AE_NO_MEMORY:
		return -ENOMEM;
	case AE_BAD_PARAMETER:
	case AE_TYPE:
	case AE_BAD_DATA:
		return -EINVAL;
	case AE_BUFFER_OVERFLOW:
		return -EOVERFLOW;
	case AE_TIME:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

int acpi_dev_ready(void)
{
	return acpi_namespace_ready();
}

int acpi_fadt_low_power_s0(void)
{
	return acpi_dev_ready() &&
	       (AcpiGbl_FADT.Flags & ACPI_FADT_LOW_POWER_S0) != 0;
}

/* ==========================================================================
 * Handles and device snapshots
 * ========================================================================== */

void *acpi_get_handle_by_path(void *scope, const char *path)
{
	ACPI_HANDLE h = NULL;

	if (!acpi_dev_ready() || !path || !path[0])
		return NULL;
	if (ACPI_FAILURE(AcpiGetHandle((ACPI_HANDLE)scope, (char *)path, &h)))
		return NULL;
	return h;
}

void *acpi_handle_parent(void *handle)
{
	ACPI_HANDLE p = NULL;

	if (!acpi_dev_ready() || !handle)
		return NULL;
	if (ACPI_FAILURE(AcpiGetParent((ACPI_HANDLE)handle, &p)))
		return NULL;
	return p;
}

int acpi_handle_path(void *handle, char *buf, size_t len)
{
	ACPI_BUFFER b;

	if (!acpi_dev_ready())
		return -ENODEV;
	if (!handle || !buf || len < 2)
		return -EINVAL;
	b.Length = len;
	b.Pointer = buf;
	return status_to_errno(AcpiGetName((ACPI_HANDLE)handle,
					   ACPI_FULL_PATHNAME, &b));
}

/* _STA of a handle; a device without _STA is fully there. */
static uint32_t handle_sta(ACPI_HANDLE h)
{
	ACPI_OBJECT obj;
	ACPI_BUFFER buf = { sizeof(obj), &obj };
	ACPI_STATUS st;

	st = AcpiEvaluateObject(h, (char *)"_STA", NULL, &buf);
	if (st == AE_NOT_FOUND)
		return ACPI_DEV_STA_DEFAULT;
	if (ACPI_FAILURE(st) || obj.Type != ACPI_TYPE_INTEGER)
		return 0; /* _STA failed: treat as absent */
	return (uint32_t)obj.Integer.Value;
}

static void fill_device(struct acpi_device *adev, ACPI_HANDLE h,
			const ACPI_DEVICE_INFO *info)
{
	adev->handle = h;
	(void)acpi_handle_path(h, adev->path, sizeof(adev->path));
	if (info->Valid & ACPI_VALID_HID) {
		str_copy(adev->hid, sizeof(adev->hid), info->HardwareId.String,
			 info->HardwareId.Length);
		adev->has_hid = 1;
	}
	if (info->Valid & ACPI_VALID_UID) {
		str_copy(adev->uid, sizeof(adev->uid), info->UniqueId.String,
			 info->UniqueId.Length);
		adev->has_uid = 1;
	}
	if (info->Valid & ACPI_VALID_ADR) {
		adev->adr = info->Address;
		adev->has_adr = 1;
	}
	if (info->Valid & ACPI_VALID_CID) {
		uint32_t n = info->CompatibleIdList.Count;

		for (uint32_t i = 0; i < n && adev->ncids < ACPI_DEV_MAX_CIDS;
		     i++)
			str_copy(adev->cid[adev->ncids++],
				 sizeof(adev->cid[0]),
				 info->CompatibleIdList.Ids[i].String,
				 info->CompatibleIdList.Ids[i].Length);
	}
}

struct acpi_device *acpi_dev_from_handle(void *handle)
{
	ACPI_DEVICE_INFO *info = NULL;
	ACPI_OBJECT_TYPE type = ACPI_TYPE_ANY;
	struct acpi_device *adev;

	if (!acpi_dev_ready() || !handle)
		return NULL;
	if (ACPI_FAILURE(AcpiGetType((ACPI_HANDLE)handle, &type)) ||
	    (type != ACPI_TYPE_DEVICE && type != ACPI_TYPE_PROCESSOR))
		return NULL;
	if (ACPI_FAILURE(AcpiGetObjectInfo((ACPI_HANDLE)handle, &info)) ||
	    !info)
		return NULL;
	adev = kcalloc(1, sizeof(*adev));
	if (adev) {
		fill_device(adev, (ACPI_HANDLE)handle, info);
		adev->sta = handle_sta((ACPI_HANDLE)handle);
		refcount_set(&adev->ref, 1);
	}
	AcpiOsFree(info);
	return adev;
}

struct acpi_device *acpi_dev_from_path(void *scope, const char *path)
{
	void *h = acpi_get_handle_by_path(scope, path);

	return h ? acpi_dev_from_handle(h) : NULL;
}

struct acpi_device *acpi_dev_get(struct acpi_device *adev)
{
	if (adev)
		refcount_inc(&adev->ref);
	return adev;
}

void acpi_dev_put(struct acpi_device *adev)
{
	if (adev && refcount_dec_and_test(&adev->ref))
		kfree(adev);
}

uint32_t acpi_dev_update_status(struct acpi_device *adev)
{
	if (!adev || !acpi_dev_ready())
		return 0;
	adev->sta = handle_sta((ACPI_HANDLE)adev->handle);
	return adev->sta;
}

int acpi_dev_has_id(const struct acpi_device *adev, const char *id)
{
	if (!adev || !id || !id[0])
		return 0;
	if (adev->has_hid && str_eq(adev->hid, id))
		return 1;
	for (int i = 0; i < adev->ncids; i++)
		if (str_eq(adev->cid[i], id))
			return 1;
	return 0;
}

const struct acpi_device_id *
acpi_match_device_ids(const struct acpi_device *adev,
		      const struct acpi_device_id *ids)
{
	if (!adev || !ids)
		return NULL;
	/* Each id of the device in order (_HID, then the _CIDs) against
	 * the whole table: the most specific id the driver knows wins. */
	if (adev->has_hid)
		for (const struct acpi_device_id *id = ids; id->id[0]; id++)
			if (str_eq(adev->hid, id->id))
				return id;
	for (int i = 0; i < adev->ncids; i++)
		for (const struct acpi_device_id *id = ids; id->id[0]; id++)
			if (str_eq(adev->cid[i], id->id))
				return id;
	return NULL;
}

/* ==========================================================================
 * Enumeration
 * ========================================================================== */

struct match_walk {
	const struct acpi_device_id *ids; /* NULL: every device */
	int present_only;
	acpi_dev_match_cb cb;
	void *ctx;
	int count;
	int stop;
};

static ACPI_STATUS match_walk_cb(ACPI_HANDLE h, UINT32 level, void *context,
				 void **ret)
{
	struct match_walk *mw = context;
	ACPI_DEVICE_INFO *info = NULL;
	struct acpi_device *adev;
	const struct acpi_device_id *id;
	uint32_t sta;

	(void)level;
	(void)ret;
	if (mw->stop)
		return AE_CTRL_TERMINATE;
	sta = handle_sta(h);
	/* Neither present nor functioning: the device and everything below
	 * it are absent (a device can be not present but functioning --
	 * a docking station's children -- and is then searched). */
	if (mw->present_only && !(sta & ACPI_DEV_STA_PRESENT) &&
	    !(sta & ACPI_DEV_STA_FUNCTIONING))
		return AE_CTRL_DEPTH;
	if (ACPI_FAILURE(AcpiGetObjectInfo(h, &info)) || !info)
		return AE_OK;
	if (!(info->Valid & (ACPI_VALID_HID | ACPI_VALID_CID))) {
		AcpiOsFree(info);
		return AE_OK;
	}
	adev = kcalloc(1, sizeof(*adev));
	if (!adev) {
		AcpiOsFree(info);
		return AE_NO_MEMORY;
	}
	fill_device(adev, h, info);
	AcpiOsFree(info);
	adev->sta = sta;
	refcount_set(&adev->ref, 1);
	id = mw->ids ? acpi_match_device_ids(adev, mw->ids) : NULL;
	if ((mw->ids && !id) ||
	    (mw->present_only && !acpi_dev_is_usable(adev))) {
		acpi_dev_put(adev);
		return AE_OK;
	}
	mw->count++;
	if (mw->cb(adev, id, mw->ctx))
		mw->stop = 1;
	acpi_dev_put(adev);
	return mw->stop ? AE_CTRL_TERMINATE : AE_OK;
}

int acpi_dev_for_each_match(const struct acpi_device_id *ids,
			    int present_only, acpi_dev_match_cb cb, void *ctx)
{
	struct match_walk mw = { ids, present_only, cb, ctx, 0, 0 };
	ACPI_STATUS st;

	might_sleep();
	if (!acpi_dev_ready())
		return -ENODEV;
	if (!cb)
		return -EINVAL;
	st = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
			       ACPI_UINT32_MAX, match_walk_cb, NULL, &mw, NULL);
	if (ACPI_FAILURE(st) && st != AE_CTRL_TERMINATE)
		return status_to_errno(st);
	return mw.count;
}

struct collect_ctx {
	struct acpi_device **out;
	int max, n;
};

static int collect_cb(struct acpi_device *adev,
		      const struct acpi_device_id *id, void *ctx)
{
	struct collect_ctx *cc = ctx;

	(void)id;
	if (cc->n >= cc->max)
		return 1;
	cc->out[cc->n++] = acpi_dev_get(adev);
	return 0;
}

int acpi_dev_collect_matches(const struct acpi_device_id *ids,
			     int present_only, struct acpi_device **out,
			     int max)
{
	struct collect_ctx cc = { out, max, 0 };
	int rc;

	if (!out || max <= 0)
		return -EINVAL;
	rc = acpi_dev_for_each_match(ids, present_only, collect_cb, &cc);
	if (rc < 0) {
		while (cc.n)
			acpi_dev_put(out[--cc.n]);
		return rc;
	}
	return cc.n;
}

struct present_ctx {
	const char *uid;
	int64_t hrv;
	int found;
};

static int present_cb(struct acpi_device *adev,
		      const struct acpi_device_id *id, void *ctx)
{
	struct present_ctx *pc = ctx;
	uint64_t hrv;

	(void)id;
	if (pc->uid && (!adev->has_uid || !str_eq(adev->uid, pc->uid)))
		return 0;
	if (pc->hrv >= 0) {
		if (acpi_evaluate_integer(adev->handle, "_HRV", NULL, 0,
					  &hrv) != 0 ||
		    (int64_t)hrv != pc->hrv)
			return 0;
	}
	pc->found = 1;
	return 1;
}

int acpi_dev_present(const char *hid, const char *uid, int64_t hrv)
{
	struct acpi_device_id ids[2];
	struct present_ctx pc = { uid, hrv, 0 };

	if (!hid || !hid[0])
		return 0;
	kmemset(ids, 0, sizeof(ids));
	str_copy(ids[0].id, sizeof(ids[0].id), hid, ACPI_ID_LEN);
	if (acpi_dev_for_each_match(ids, 1, present_cb, &pc) < 0)
		return 0;
	return pc.found;
}

int acpi_dev_for_each_child(struct acpi_device *adev, acpi_dev_child_cb cb,
			    void *ctx)
{
	ACPI_HANDLE child = NULL;
	int count = 0;

	might_sleep();
	if (!acpi_dev_ready())
		return -ENODEV;
	if (!adev || !cb)
		return -EINVAL;
	for (;;) {
		struct acpi_device *c;
		int stop;

		if (ACPI_FAILURE(AcpiGetNextObject(ACPI_TYPE_DEVICE,
						   (ACPI_HANDLE)adev->handle,
						   child, &child)))
			break;
		c = acpi_dev_from_handle(child);
		if (!c)
			continue;
		count++;
		stop = cb(c, ctx);
		acpi_dev_put(c);
		if (stop)
			break;
	}
	return count;
}

/* ---- PCI companions ------------------------------------------------------ */

static const struct acpi_device_id g_host_bridge_ids[] = {
	{ "PNP0A08", 0 }, /* PCI Express root bridge */
	{ "PNP0A03", 0 }, /* PCI root bridge */
	{ "", 0 },
};

struct host_bridge_ctx {
	uint32_t segment;
	uint32_t bus;
	struct acpi_device *found;
};

static int host_bridge_cb(struct acpi_device *adev,
			  const struct acpi_device_id *id, void *ctx)
{
	struct host_bridge_ctx *hb = ctx;
	uint64_t seg = 0, bbn = 0;

	(void)id;
	/* Both default to 0 when absent (a single-segment system with the
	 * root bridge at bus 0). */
	(void)acpi_evaluate_integer(adev->handle, "_SEG", NULL, 0, &seg);
	(void)acpi_evaluate_integer(adev->handle, "_BBN", NULL, 0, &bbn);
	if ((uint32_t)seg != hb->segment || (uint32_t)bbn != hb->bus)
		return 0;
	hb->found = acpi_dev_get(adev);
	return 1;
}

struct adr_child_ctx {
	uint64_t adr;
	struct acpi_device *found; /* present match preferred */
};

static int adr_child_cb(struct acpi_device *child, void *ctx)
{
	struct adr_child_ctx *ac = ctx;

	if (!child->has_adr || child->adr != ac->adr)
		return 0;
	if (!ac->found) {
		ac->found = acpi_dev_get(child);
	} else if (!acpi_dev_is_present(ac->found) &&
		   acpi_dev_is_present(child)) {
		/* Firmware sometimes declares one address twice, once for
		 * each configuration; the one that is there wins. */
		acpi_dev_put(ac->found);
		ac->found = acpi_dev_get(child);
	}
	return acpi_dev_is_present(ac->found) ? 1 : 0;
}

struct acpi_device *acpi_pci_find_companion(const struct pci_device *pdev)
{
	struct acpi_device *parent;
	struct adr_child_ctx ac;

	might_sleep();
	if (!acpi_dev_ready() || !pdev)
		return NULL;
	if (pdev->segment >= 0x10000)
		return NULL; /* behind a VMD domain: not in the namespace */
	if (pdev->bus_self) {
		parent = acpi_pci_find_companion(pdev->bus_self);
	} else {
		struct host_bridge_ctx hb = { pdev->segment, pdev->bus, NULL };

		(void)acpi_dev_for_each_match(g_host_bridge_ids, 0,
					      host_bridge_cb, &hb);
		parent = hb.found;
	}
	if (!parent)
		return NULL;
	ac.adr = ((uint64_t)pdev->device << 16) | pdev->function;
	ac.found = NULL;
	(void)acpi_dev_for_each_child(parent, adr_child_cb, &ac);
	acpi_dev_put(parent);
	return ac.found;
}

/* ==========================================================================
 * Values
 * ========================================================================== */

/* Bytes needed for the acpi_value tree of `obj' (values + payload). */
static size_t value_size(const ACPI_OBJECT *obj, int depth)
{
	size_t sz = 0;

	if (depth > ACPI_VALUE_MAX_DEPTH)
		return 0;
	switch (obj->Type) {
	case ACPI_TYPE_STRING:
		return (size_t)obj->String.Length + 1;
	case ACPI_TYPE_BUFFER:
		return (size_t)obj->Buffer.Length;
	case ACPI_TYPE_PACKAGE:
		/* + alignment of the element array (see value_fill) */
		sz = 7 + (size_t)obj->Package.Count * sizeof(struct acpi_value);
		for (uint32_t i = 0; i < obj->Package.Count; i++)
			sz += value_size(&obj->Package.Elements[i], depth + 1);
		return sz;
	default:
		return 0;
	}
}

/* Fill `v' from `obj', taking payload space from *pool. */
static void value_fill(struct acpi_value *v, const ACPI_OBJECT *obj,
		       uint8_t **pool, int depth)
{
	kmemset(v, 0, sizeof(*v));
	if (depth > ACPI_VALUE_MAX_DEPTH)
		return;
	switch (obj->Type) {
	case ACPI_TYPE_INTEGER:
		v->type = ACPI_VALUE_INTEGER;
		v->integer = obj->Integer.Value;
		break;
	case ACPI_TYPE_STRING:
		v->type = ACPI_VALUE_STRING;
		v->length = obj->String.Length;
		v->string = (char *)*pool;
		if (obj->String.Length)
			kmemcpy(v->string, obj->String.Pointer,
				obj->String.Length);
		v->string[obj->String.Length] = '\0';
		*pool += obj->String.Length + 1;
		break;
	case ACPI_TYPE_BUFFER:
		v->type = ACPI_VALUE_BUFFER;
		v->length = obj->Buffer.Length;
		v->buffer = *pool;
		if (obj->Buffer.Length)
			kmemcpy(v->buffer, obj->Buffer.Pointer,
				obj->Buffer.Length);
		*pool += obj->Buffer.Length;
		break;
	case ACPI_TYPE_PACKAGE:
		v->type = ACPI_VALUE_PACKAGE;
		v->length = obj->Package.Count;
		/* Strings and buffers before it leave the pool unaligned. */
		*pool = (uint8_t *)(((uint64_t)*pool + 7) & ~(uint64_t)7);
		v->elements = (struct acpi_value *)*pool;
		*pool += (size_t)obj->Package.Count * sizeof(struct acpi_value);
		for (uint32_t i = 0; i < obj->Package.Count; i++)
			value_fill(&v->elements[i], &obj->Package.Elements[i],
				   pool, depth + 1);
		break;
	case ACPI_TYPE_LOCAL_REFERENCE:
		v->type = ACPI_VALUE_REFERENCE;
		v->handle = obj->Reference.Handle;
		break;
	default:
		v->type = ACPI_VALUE_NONE;
		break;
	}
}

/* A copy of an ACPICA result in one allocation of ours. */
static struct acpi_value *value_from_object(const ACPI_OBJECT *obj)
{
	size_t payload = value_size(obj, 0);
	struct acpi_value *v;
	uint8_t *pool;

	/* The payload follows the root value; value_size counted the
	 * alignment padding of every element array. */
	BUILD_BUG_ON(sizeof(struct acpi_value) % 8 != 0);
	v = kcalloc(1, sizeof(*v) + payload);
	if (!v)
		return NULL;
	pool = (uint8_t *)(v + 1);
	value_fill(v, obj, &pool, 0);
	return v;
}

void acpi_value_free(struct acpi_value *v)
{
	kfree(v);
}

/* Argument conversion: count the ACPI_OBJECTs a value needs (packages
 * need their element arrays). */
static size_t arg_objects(const struct acpi_value *v, int depth)
{
	size_t n = 1;

	if (depth > ACPI_VALUE_MAX_DEPTH)
		return 1;
	if (v->type == ACPI_VALUE_PACKAGE)
		for (uint32_t i = 0; i < v->length; i++)
			n += arg_objects(&v->elements[i], depth + 1);
	return n;
}

static int arg_fill(ACPI_OBJECT *o, const struct acpi_value *v,
		    ACPI_OBJECT **pool, int depth)
{
	if (depth > ACPI_VALUE_MAX_DEPTH)
		return -EINVAL;
	kmemset(o, 0, sizeof(*o));
	switch (v->type) {
	case ACPI_VALUE_INTEGER:
		o->Type = ACPI_TYPE_INTEGER;
		o->Integer.Value = v->integer;
		return 0;
	case ACPI_VALUE_STRING:
		o->Type = ACPI_TYPE_STRING;
		o->String.Length = v->length;
		o->String.Pointer = v->string;
		return 0;
	case ACPI_VALUE_BUFFER:
		o->Type = ACPI_TYPE_BUFFER;
		o->Buffer.Length = v->length;
		o->Buffer.Pointer = v->buffer;
		return 0;
	case ACPI_VALUE_PACKAGE:
		o->Type = ACPI_TYPE_PACKAGE;
		o->Package.Count = v->length;
		o->Package.Elements = v->length ? *pool : NULL;
		{
			ACPI_OBJECT *elems = *pool;

			*pool += v->length;
			for (uint32_t i = 0; i < v->length; i++) {
				int rc = arg_fill(&elems[i], &v->elements[i],
						  pool, depth + 1);
				if (rc)
					return rc;
			}
		}
		return 0;
	case ACPI_VALUE_REFERENCE:
		o->Type = ACPI_TYPE_LOCAL_REFERENCE;
		o->Reference.ActualType = ACPI_TYPE_ANY;
		o->Reference.Handle = v->handle;
		return 0;
	default:
		return -EINVAL;
	}
}

int acpi_evaluate(void *handle, const char *method,
		  const struct acpi_value *args, int nargs,
		  struct acpi_value **out)
{
	ACPI_OBJECT top[ACPI_DEV_MAX_ARGS];
	ACPI_OBJECT *extra = NULL, *pool;
	ACPI_OBJECT_LIST list;
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_STATUS st;
	size_t need = 0;
	int rc = 0;

	might_sleep();
	if (out)
		*out = NULL;
	if (!acpi_dev_ready())
		return -ENODEV;
	if (!handle && (!method || method[0] != '\\'))
		return -EINVAL;
	if (nargs < 0 || nargs > ACPI_DEV_MAX_ARGS || (nargs && !args))
		return -EINVAL;
	for (int i = 0; i < nargs; i++)
		need += arg_objects(&args[i], 0) - 1;
	if (need) {
		extra = kcalloc(need, sizeof(ACPI_OBJECT));
		if (!extra)
			return -ENOMEM;
	}
	pool = extra;
	for (int i = 0; i < nargs && rc == 0; i++)
		rc = arg_fill(&top[i], &args[i], &pool, 0);
	if (rc) {
		kfree(extra);
		return rc;
	}
	list.Count = (UINT32)nargs;
	list.Pointer = nargs ? top : NULL;
	st = AcpiEvaluateObject((ACPI_HANDLE)handle, (char *)method,
				nargs ? &list : NULL, out ? &buf : NULL);
	kfree(extra);
	if (ACPI_FAILURE(st))
		return status_to_errno(st);
	if (out && buf.Pointer) {
		*out = value_from_object((ACPI_OBJECT *)buf.Pointer);
		AcpiOsFree(buf.Pointer);
		if (!*out)
			return -ENOMEM;
	}
	return 0;
}

int acpi_evaluate_integer(void *handle, const char *method,
			  const struct acpi_value *args, int nargs,
			  uint64_t *val)
{
	struct acpi_value *v = NULL;
	int rc;

	if (!val)
		return -EINVAL;
	rc = acpi_evaluate(handle, method, args, nargs, &v);
	if (rc)
		return rc;
	if (!v) {
		rc = -EPROTO;
	} else if (v->type == ACPI_VALUE_INTEGER) {
		*val = v->integer;
	} else if (v->type == ACPI_VALUE_BUFFER && v->length <= 8) {
		uint64_t x = 0;

		for (uint32_t i = 0; i < v->length; i++)
			x |= (uint64_t)v->buffer[i] << (8 * i);
		*val = x;
	} else {
		rc = -EPROTO;
	}
	acpi_value_free(v);
	return rc;
}

int acpi_has_method(void *handle, const char *name)
{
	ACPI_HANDLE h;

	if (!acpi_dev_ready() || !handle || !name)
		return 0;
	return ACPI_SUCCESS(AcpiGetHandle((ACPI_HANDLE)handle, (char *)name,
					  &h));
}

int acpi_evaluate_u16_package(void *handle, const char *method,
			      uint16_t *vals, int n)
{
	struct acpi_value *v = NULL;
	int rc, i;

	if (!vals || n <= 0)
		return -EINVAL;
	rc = acpi_evaluate(handle, method, NULL, 0, &v);
	if (rc)
		return rc;
	if (!v || v->type != ACPI_VALUE_PACKAGE) {
		acpi_value_free(v);
		return -EPROTO;
	}
	for (i = 0; i < n && (uint32_t)i < v->length; i++) {
		if (v->elements[i].type != ACPI_VALUE_INTEGER)
			break;
		vals[i] = (uint16_t)v->elements[i].integer;
	}
	acpi_value_free(v);
	return i;
}

int acpi_evaluate_void(void *handle, const char *method)
{
	if (!acpi_has_method(handle, method))
		return acpi_dev_ready() ? -ENOENT : -ENODEV;
	return acpi_evaluate(handle, method, NULL, 0, NULL);
}

/* ==========================================================================
 * _DSM
 * ========================================================================== */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int acpi_uuid_parse(const char *str, uint8_t out[16])
{
	/* Text order of the 16 bytes -> their position in the buffer: the
	 * first three fields are stored little-endian, the last two as
	 * written. */
	static const uint8_t pos[16] = { 3, 2,  1,  0,	5,  4,	7,  6,
					 8, 9, 10, 11, 12, 13, 14, 15 };
	int bi = 0;

	if (!str || !out)
		return -EINVAL;
	for (int i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (str[i] != '-')
				return -EINVAL;
			continue;
		}
		int hi = hexval(str[i]), lo;

		if (hi < 0)
			return -EINVAL;
		lo = hexval(str[++i]);
		if (lo < 0)
			return -EINVAL;
		out[pos[bi++]] = (uint8_t)(hi << 4 | lo);
	}
	return str[36] == '\0' ? 0 : -EINVAL;
}

int acpi_evaluate_dsm(void *handle, const uint8_t uuid[16], uint64_t rev,
		      uint64_t func, const struct acpi_value *arg,
		      struct acpi_value **out)
{
	struct acpi_value args[4];
	uint8_t guid[16];

	if (out)
		*out = NULL;
	if (!handle || !uuid)
		return -EINVAL;
	kmemcpy(guid, uuid, 16);
	kmemset(args, 0, sizeof(args));
	args[0].type = ACPI_VALUE_BUFFER;
	args[0].length = 16;
	args[0].buffer = guid;
	args[1].type = ACPI_VALUE_INTEGER;
	args[1].integer = rev;
	args[2].type = ACPI_VALUE_INTEGER;
	args[2].integer = func;
	if (arg) {
		args[3] = *arg;
	} else {
		args[3].type = ACPI_VALUE_PACKAGE;
		args[3].length = 0;
		args[3].elements = NULL;
	}
	return acpi_evaluate(handle, "_DSM", args, 4, out);
}

int acpi_evaluate_dsm_typed(void *handle, const uint8_t uuid[16],
			    uint64_t rev, uint64_t func,
			    const struct acpi_value *arg,
			    enum acpi_value_type type,
			    struct acpi_value **out)
{
	struct acpi_value *v = NULL;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	rc = acpi_evaluate_dsm(handle, uuid, rev, func, arg, &v);
	if (rc)
		return rc;
	if (!v || v->type != type) {
		acpi_value_free(v);
		return -EPROTO;
	}
	*out = v;
	return 0;
}

int acpi_check_dsm(void *handle, const uint8_t uuid[16], uint64_t rev,
		   uint64_t funcs)
{
	struct acpi_value *v = NULL;
	uint64_t mask = 0;

	if (funcs == 0)
		return 0;
	if (acpi_evaluate_dsm(handle, uuid, rev, 0, NULL, &v) != 0 || !v)
		return 0;
	if (v->type == ACPI_VALUE_INTEGER) {
		mask = v->integer;
	} else if (v->type == ACPI_VALUE_BUFFER) {
		for (uint32_t i = 0; i < v->length && i < 8; i++)
			mask |= (uint64_t)v->buffer[i] << (i * 8);
	}
	acpi_value_free(v);
	/* Bit 0: the function set is implemented at all. */
	return (mask & 1) && (mask & funcs) == funcs;
}

/* ==========================================================================
 * _DSD device properties
 * ========================================================================== */

static const char g_dsd_props_uuid[] = "daffd814-6eba-4d8c-8a91-bc9bbf4aa301";

/* The value of property `name' inside a _DSD result, or NULL. */
static const struct acpi_value *dsd_find(const struct acpi_value *dsd,
					 const char *name)
{
	uint8_t uuid[16];

	if (!dsd || dsd->type != ACPI_VALUE_PACKAGE ||
	    acpi_uuid_parse(g_dsd_props_uuid, uuid) != 0)
		return NULL;
	/* { UUID, Package, UUID, Package, ... } */
	for (uint32_t i = 0; i + 1 < dsd->length; i += 2) {
		const struct acpi_value *u = &dsd->elements[i];
		const struct acpi_value *p = &dsd->elements[i + 1];

		if (u->type != ACPI_VALUE_BUFFER || u->length != 16 ||
		    p->type != ACPI_VALUE_PACKAGE)
			continue;
		if (kmemcmp(u->buffer, uuid, 16) != 0)
			continue;
		for (uint32_t j = 0; j < p->length; j++) {
			const struct acpi_value *kv = &p->elements[j];

			if (kv->type != ACPI_VALUE_PACKAGE || kv->length != 2 ||
			    kv->elements[0].type != ACPI_VALUE_STRING)
				continue;
			if (str_eq(kv->elements[0].string, name))
				return &kv->elements[1];
		}
	}
	return NULL;
}

static int dsd_get(void *handle, const char *name, struct acpi_value **dsd,
		   const struct acpi_value **val)
{
	int rc;

	*dsd = NULL;
	*val = NULL;
	if (!handle || !name)
		return -EINVAL;
	rc = acpi_evaluate(handle, "_DSD", NULL, 0, dsd);
	if (rc)
		return rc == -ENOENT ? -ENOENT : rc;
	*val = dsd_find(*dsd, name);
	if (!*val) {
		acpi_value_free(*dsd);
		*dsd = NULL;
		return -ENOENT;
	}
	return 0;
}

int acpi_dev_get_property_u64(void *handle, const char *name, uint64_t *val)
{
	struct acpi_value *dsd;
	const struct acpi_value *v;
	int rc = dsd_get(handle, name, &dsd, &v);

	if (rc)
		return rc;
	/* A one-element package counts as the integer itself. */
	if (v->type == ACPI_VALUE_PACKAGE && v->length == 1)
		v = &v->elements[0];
	if (v->type == ACPI_VALUE_INTEGER)
		*val = v->integer;
	else
		rc = -EPROTO;
	acpi_value_free(dsd);
	return rc;
}

static int prop_uint(void *handle, const char *name, uint64_t max,
		     uint64_t *val)
{
	uint64_t x;
	int rc = acpi_dev_get_property_u64(handle, name, &x);

	if (rc)
		return rc;
	if (x > max)
		return -EOVERFLOW;
	*val = x;
	return 0;
}

int acpi_dev_get_property_u32(void *handle, const char *name, uint32_t *val)
{
	uint64_t x;
	int rc = prop_uint(handle, name, 0xffffffffull, &x);

	if (rc == 0)
		*val = (uint32_t)x;
	return rc;
}

int acpi_dev_get_property_u16(void *handle, const char *name, uint16_t *val)
{
	uint64_t x;
	int rc = prop_uint(handle, name, 0xffffull, &x);

	if (rc == 0)
		*val = (uint16_t)x;
	return rc;
}

int acpi_dev_get_property_u8(void *handle, const char *name, uint8_t *val)
{
	uint64_t x;
	int rc = prop_uint(handle, name, 0xffull, &x);

	if (rc == 0)
		*val = (uint8_t)x;
	return rc;
}

int acpi_dev_get_property_string(void *handle, const char *name, char *buf,
				 size_t len)
{
	struct acpi_value *dsd;
	const struct acpi_value *v;
	int rc;

	if (!buf || len == 0)
		return -EINVAL;
	rc = dsd_get(handle, name, &dsd, &v);
	if (rc)
		return rc;
	if (v->type == ACPI_VALUE_PACKAGE && v->length == 1)
		v = &v->elements[0];
	if (v->type == ACPI_VALUE_STRING)
		str_copy(buf, len, v->string, v->length);
	else
		rc = -EPROTO;
	acpi_value_free(dsd);
	return rc;
}

int acpi_dev_get_property_u32_array(void *handle, const char *name,
				    uint32_t *vals, int n)
{
	struct acpi_value *dsd;
	const struct acpi_value *v;
	int rc, count = 0;

	if (!vals || n <= 0)
		return -EINVAL;
	rc = dsd_get(handle, name, &dsd, &v);
	if (rc)
		return rc;
	if (v->type == ACPI_VALUE_INTEGER) {
		if (v->integer > 0xffffffffull)
			rc = -EOVERFLOW;
		else
			vals[count++] = (uint32_t)v->integer;
	} else if (v->type == ACPI_VALUE_PACKAGE) {
		for (uint32_t i = 0; i < v->length && count < n; i++) {
			const struct acpi_value *e = &v->elements[i];

			if (e->type != ACPI_VALUE_INTEGER) {
				rc = -EPROTO;
				break;
			}
			if (e->integer > 0xffffffffull) {
				rc = -EOVERFLOW;
				break;
			}
			vals[count++] = (uint32_t)e->integer;
		}
	} else {
		rc = -EPROTO;
	}
	acpi_value_free(dsd);
	return rc ? rc : count;
}

int acpi_dev_has_property(void *handle, const char *name)
{
	struct acpi_value *dsd;
	const struct acpi_value *v;

	if (dsd_get(handle, name, &dsd, &v) != 0)
		return 0;
	acpi_value_free(dsd);
	return 1;
}

int acpi_dev_property_match_string(void *handle, const char *name,
				   const char *str)
{
	struct acpi_value *dsd;
	const struct acpi_value *v;
	int rc = dsd_get(handle, name, &dsd, &v);

	if (rc)
		return rc;
	if (!str) {
		rc = -EINVAL;
	} else if (v->type == ACPI_VALUE_STRING) {
		rc = str_eq(v->string, str) ? 0 : -ENODATA;
	} else if (v->type == ACPI_VALUE_PACKAGE) {
		rc = -ENODATA;
		for (uint32_t i = 0; i < v->length; i++) {
			if (v->elements[i].type != ACPI_VALUE_STRING) {
				rc = -EPROTO;
				break;
			}
			if (str_eq(v->elements[i].string, str)) {
				rc = (int)i;
				break;
			}
		}
	} else {
		rc = -EPROTO;
	}
	acpi_value_free(dsd);
	return rc;
}

/* ---- data-only child nodes ---------------------------------------------- */

static const char g_dsd_data_uuid[] = "dbb8e3e6-5886-4ba6-8795-1319f52a966b";

/* A target that is not a package in place: evaluate the object it names
 * (a path or a namespace reference); it has to give a package. */
static struct acpi_value *dsd_node_eval(void *target)
{
	struct acpi_value *v = NULL;

	if (!target || acpi_evaluate(target, NULL, NULL, 0, &v) != 0)
		return NULL;
	if (!v || v->type != ACPI_VALUE_PACKAGE) {
		acpi_value_free(v);
		return NULL;
	}
	return v;
}

void acpi_dev_free_data_nodes(struct acpi_dsd_nodes *nodes)
{
	if (!nodes)
		return;
	for (int i = 0; i < nodes->count && i < ACPI_DSD_MAX_NODES; i++)
		acpi_value_free(nodes->node[i].owned);
	acpi_value_free(nodes->dsd);
	kmemset(nodes, 0, sizeof(*nodes));
}

int acpi_dev_get_data_nodes(void *handle, struct acpi_dsd_nodes *nodes)
{
	uint8_t uuid[16];
	const struct acpi_value *dsd;
	int rc;

	might_sleep();
	if (!nodes)
		return -EINVAL;
	kmemset(nodes, 0, sizeof(*nodes));
	if (!handle || acpi_uuid_parse(g_dsd_data_uuid, uuid) != 0)
		return -EINVAL;
	rc = acpi_evaluate(handle, "_DSD", NULL, 0, &nodes->dsd);
	if (rc)
		return rc;
	dsd = nodes->dsd;
	if (!dsd || dsd->type != ACPI_VALUE_PACKAGE)
		return -EPROTO;
	/* { UUID, Package, UUID, Package, ... }: every hierarchical data
	 * block, in order. */
	for (uint32_t i = 0; i + 1 < dsd->length; i += 2) {
		const struct acpi_value *u = &dsd->elements[i];
		const struct acpi_value *p = &dsd->elements[i + 1];

		if (u->type != ACPI_VALUE_BUFFER || u->length != 16 ||
		    p->type != ACPI_VALUE_PACKAGE ||
		    kmemcmp(u->buffer, uuid, 16) != 0)
			continue;
		for (uint32_t j = 0; j < p->length; j++) {
			const struct acpi_value *link = &p->elements[j];
			const struct acpi_value *name, *target;
			const struct acpi_value *data = NULL;
			struct acpi_value *owned = NULL;
			int n = nodes->count;

			/* Two elements, the first a string. */
			if (link->type != ACPI_VALUE_PACKAGE ||
			    link->length != 2 ||
			    link->elements[0].type != ACPI_VALUE_STRING)
				continue;
			name = &link->elements[0];
			target = &link->elements[1];
			switch (target->type) {
			case ACPI_VALUE_STRING:
				owned = dsd_node_eval(acpi_get_handle_by_path(
					handle, target->string));
				data = owned;
				break;
			case ACPI_VALUE_REFERENCE:
				owned = dsd_node_eval(target->handle);
				data = owned;
				break;
			case ACPI_VALUE_PACKAGE:
				data = target;
				break;
			default:
				break;
			}
			if (!data)
				continue;
			if (n >= ACPI_DSD_MAX_NODES) {
				acpi_value_free(owned);
				return -E2BIG;
			}
			str_copy(nodes->node[n].name, ACPI_DSD_NAME_LEN,
				 name->string, name->length);
			nodes->node[n].data = data;
			nodes->node[n].owned = owned;
			nodes->count = n + 1;
		}
	}
	return nodes->count;
}

int acpi_dsd_data_get_u32(const struct acpi_value *data, const char *name,
			  uint32_t *val)
{
	const struct acpi_value *v = dsd_find(data, name);

	if (!v || !val)
		return -ENOENT;
	if (v->type == ACPI_VALUE_PACKAGE && v->length == 1)
		v = &v->elements[0];
	if (v->type != ACPI_VALUE_INTEGER)
		return -EPROTO;
	if (v->integer > 0xffffffffull)
		return -EOVERFLOW;
	*val = (uint32_t)v->integer;
	return 0;
}

int acpi_dsd_data_get_string(const struct acpi_value *data, const char *name,
			     char *buf, size_t len)
{
	const struct acpi_value *v = dsd_find(data, name);

	if (!buf || len == 0)
		return -EINVAL;
	if (!v)
		return -ENOENT;
	if (v->type == ACPI_VALUE_PACKAGE && v->length == 1)
		v = &v->elements[0];
	if (v->type != ACPI_VALUE_STRING)
		return -EPROTO;
	str_copy(buf, len, v->string, v->length);
	return 0;
}

/* ==========================================================================
 * Resources
 * ========================================================================== */

static void copy_source(char *dst, const ACPI_RESOURCE_SOURCE *src)
{
	if (src->StringPtr && src->StringLength)
		str_copy(dst, ACPI_PATH_LEN, src->StringPtr,
			 src->StringLength);
	else
		dst[0] = '\0';
}

/* A legacy (ISA) IRQ descriptor: the number is an ISA IRQ; the MADT may
 * route it to another GSI and with another trigger, and the override is
 * what the hardware is wired as. */
static void isa_irq_to_res(uint8_t isa, uint8_t trig, uint8_t pol,
			   struct acpi_res_irq *r)
{
	irq_override_t *ov = acpi_get_irq_override(isa);

	r->gsi = isa;
	r->triggering = trig;
	r->polarity = pol;
	if (!ov)
		return;
	r->gsi = ov->gsi;
	if ((ov->trigger_mode & 3) == 3)
		r->triggering = ACPI_RES_LEVEL;
	else if ((ov->trigger_mode & 3) == 1)
		r->triggering = ACPI_RES_EDGE;
	if ((ov->polarity & 3) == 3)
		r->polarity = ACPI_RES_ACTIVE_LOW;
	else if ((ov->polarity & 3) == 1)
		r->polarity = ACPI_RES_ACTIVE_HIGH;
}

/* Address16/32/64 and the extended form into a memory or I/O entry. */
static int decode_address(const ACPI_RESOURCE *ares, struct acpi_res *out)
{
	ACPI_RESOURCE_ADDRESS64 a;
	uint64_t len;

	if (ACPI_FAILURE(AcpiResourceToAddress64((ACPI_RESOURCE *)ares, &a)))
		return -ENOENT;
	len = a.Address.AddressLength;
	if (len == 0 && a.Address.Maximum >= a.Address.Minimum)
		len = a.Address.Maximum - a.Address.Minimum + 1;
	switch (a.ResourceType) {
	case ACPI_MEMORY_RANGE:
		out->type = ACPI_RES_MEM;
		out->mem.start = a.Address.Minimum;
		if (a.ProducerConsumer == ACPI_PRODUCER)
			out->mem.start += a.Address.TranslationOffset;
		out->mem.length = len;
		out->mem.writeable = a.Info.Mem.WriteProtect ==
				     ACPI_READ_WRITE_MEMORY;
		out->mem.producer = a.ProducerConsumer == ACPI_PRODUCER;
		out->mem.cacheable = a.Info.Mem.Caching !=
				     ACPI_NON_CACHEABLE_MEMORY;
		return 0;
	case ACPI_IO_RANGE:
		out->type = ACPI_RES_IO;
		out->io.start = a.Address.Minimum;
		if (a.ProducerConsumer == ACPI_PRODUCER)
			out->io.start += a.Address.TranslationOffset;
		out->io.length = len;
		return 0;
	default:
		out->type = ACPI_RES_OTHER; /* bus numbers */
		return 0;
	}
}

static void decode_gpio(const ACPI_RESOURCE_GPIO *g, struct acpi_res_gpio *r)
{
	r->conn_type = g->ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT ?
			       ACPI_RES_GPIO_INT :
			       ACPI_RES_GPIO_IO;
	r->producer = g->ProducerConsumer == ACPI_PRODUCER;
	r->pin_config = g->PinConfig;
	r->shareable = g->Shareable == ACPI_SHARED;
	r->wake_capable = g->WakeCapable == ACPI_WAKE_CAPABLE;
	r->io_restriction = g->IoRestriction;
	if (r->conn_type == ACPI_RES_GPIO_INT) {
		r->triggering = g->Triggering;
		r->polarity = g->Polarity;
	}
	r->drive_strength = g->DriveStrength;
	r->debounce_timeout = g->DebounceTimeout;
	r->npins = g->PinTableLength;
	for (uint16_t i = 0;
	     i < g->PinTableLength && i < ACPI_RES_GPIO_MAX_PINS && g->PinTable;
	     i++)
		r->pins[i] = g->PinTable[i];
	copy_source(r->source, &g->ResourceSource);
	r->source_index = g->ResourceSource.Index;
}

int acpi_res_decode_one(const void *p, struct acpi_res *out)
{
	const ACPI_RESOURCE *ares = p;

	kmemset(out, 0, sizeof(*out));
	switch (ares->Type) {
	case ACPI_RESOURCE_TYPE_MEMORY24: {
		const ACPI_RESOURCE_MEMORY24 *m = &ares->Data.Memory24;

		/* 24-bit descriptors count in 256-byte units. */
		out->type = ACPI_RES_MEM;
		out->mem.start = (uint64_t)m->Minimum << 8;
		out->mem.length = (uint64_t)m->AddressLength << 8;
		out->mem.writeable = m->WriteProtect == ACPI_READ_WRITE_MEMORY;
		return 0;
	}
	case ACPI_RESOURCE_TYPE_MEMORY32: {
		const ACPI_RESOURCE_MEMORY32 *m = &ares->Data.Memory32;

		out->type = ACPI_RES_MEM;
		out->mem.start = m->Minimum;
		out->mem.length = m->AddressLength;
		out->mem.writeable = m->WriteProtect == ACPI_READ_WRITE_MEMORY;
		return 0;
	}
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32: {
		const ACPI_RESOURCE_FIXED_MEMORY32 *m =
			&ares->Data.FixedMemory32;

		out->type = ACPI_RES_MEM;
		out->mem.start = m->Address;
		out->mem.length = m->AddressLength;
		out->mem.writeable = m->WriteProtect == ACPI_READ_WRITE_MEMORY;
		return 0;
	}
	case ACPI_RESOURCE_TYPE_ADDRESS16:
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64:
	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		return decode_address(ares, out);
	case ACPI_RESOURCE_TYPE_IO:
		out->type = ACPI_RES_IO;
		out->io.start = ares->Data.Io.Minimum;
		out->io.length = ares->Data.Io.AddressLength;
		return 0;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		out->type = ACPI_RES_IO;
		out->io.start = ares->Data.FixedIo.Address;
		out->io.length = ares->Data.FixedIo.AddressLength;
		return 0;
	case ACPI_RESOURCE_TYPE_IRQ: {
		const ACPI_RESOURCE_IRQ *q = &ares->Data.Irq;

		out->type = ACPI_RES_IRQ;
		out->irq.shareable = q->Shareable == ACPI_SHARED;
		out->irq.wake_capable = q->WakeCapable == ACPI_WAKE_CAPABLE;
		if (q->InterruptCount == 0)
			return -ENOENT;
		isa_irq_to_res(q->Interrupts[0], q->Triggering, q->Polarity,
			       &out->irq);
		return 0;
	}
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
		const ACPI_RESOURCE_EXTENDED_IRQ *q = &ares->Data.ExtendedIrq;

		out->type = ACPI_RES_IRQ;
		if (q->InterruptCount == 0)
			return -ENOENT;
		out->irq.gsi = q->Interrupts[0];
		out->irq.triggering = q->Triggering;
		out->irq.polarity = q->Polarity;
		out->irq.shareable = q->Shareable == ACPI_SHARED;
		out->irq.wake_capable = q->WakeCapable == ACPI_WAKE_CAPABLE;
		out->irq.extended = 1;
		out->irq.producer = q->ProducerConsumer == ACPI_PRODUCER;
		copy_source(out->irq.source, &q->ResourceSource);
		return 0;
	}
	case ACPI_RESOURCE_TYPE_DMA:
		out->type = ACPI_RES_DMA;
		if (ares->Data.Dma.ChannelCount == 0)
			return -ENOENT;
		out->dma.channel = ares->Data.Dma.Channels[0];
		out->dma.width = ares->Data.Dma.Transfer;
		return 0;
	case ACPI_RESOURCE_TYPE_FIXED_DMA:
		out->type = ACPI_RES_DMA;
		out->dma.channel = ares->Data.FixedDma.Channels;
		out->dma.request_line = ares->Data.FixedDma.RequestLines;
		out->dma.width = ares->Data.FixedDma.Width;
		return 0;
	case ACPI_RESOURCE_TYPE_GPIO:
		out->type = ACPI_RES_GPIO;
		decode_gpio(&ares->Data.Gpio, &out->gpio);
		return 0;
	case ACPI_RESOURCE_TYPE_SERIAL_BUS: {
		const ACPI_RESOURCE_COMMON_SERIALBUS *c =
			&ares->Data.CommonSerialBus;

		switch (c->Type) {
		case ACPI_RESOURCE_SERIAL_TYPE_I2C: {
			const ACPI_RESOURCE_I2C_SERIALBUS *i2c =
				&ares->Data.I2cSerialBus;

			out->type = ACPI_RES_I2C;
			out->i2c.address = i2c->SlaveAddress;
			out->i2c.ten_bit = i2c->AccessMode ==
					   ACPI_I2C_10BIT_MODE;
			out->i2c.speed_hz = i2c->ConnectionSpeed;
			out->i2c.device_initiated = i2c->SlaveMode ==
						    ACPI_DEVICE_INITIATED;
			out->i2c.producer = i2c->ProducerConsumer ==
					    ACPI_PRODUCER;
			out->i2c.shared = i2c->ConnectionSharing ==
					  ACPI_SHARED;
			out->i2c.revision = i2c->RevisionId;
			copy_source(out->i2c.source, &i2c->ResourceSource);
			out->i2c.source_index = i2c->ResourceSource.Index;
			return 0;
		}
		case ACPI_RESOURCE_SERIAL_TYPE_SPI: {
			const ACPI_RESOURCE_SPI_SERIALBUS *spi =
				&ares->Data.SpiSerialBus;

			out->type = ACPI_RES_SPI;
			out->spi.speed_hz = spi->ConnectionSpeed;
			out->spi.chip_select = spi->DeviceSelection;
			out->spi.mode =
				(spi->ClockPhase == ACPI_SPI_SECOND_PHASE ?
					 1 :
					 0) |
				(spi->ClockPolarity == ACPI_SPI_START_HIGH ?
					 2 :
					 0);
			out->spi.word_bits = spi->DataBitLength;
			out->spi.wire3 = spi->WireMode == ACPI_SPI_3WIRE_MODE;
			out->spi.cs_active_high = spi->DevicePolarity ==
						  ACPI_SPI_ACTIVE_HIGH;
			out->spi.device_initiated = spi->SlaveMode ==
						    ACPI_DEVICE_INITIATED;
			copy_source(out->spi.source, &spi->ResourceSource);
			return 0;
		}
		case ACPI_RESOURCE_SERIAL_TYPE_UART: {
			const ACPI_RESOURCE_UART_SERIALBUS *u =
				&ares->Data.UartSerialBus;

			out->type = ACPI_RES_UART;
			out->uart.baud = u->DefaultBaudRate;
			out->uart.data_bits = u->DataBits;
			out->uart.stop_bits = u->StopBits;
			out->uart.parity = u->Parity;
			out->uart.flow_control = u->FlowControl;
			copy_source(out->uart.source, &u->ResourceSource);
			return 0;
		}
		default:
			out->type = ACPI_RES_OTHER;
			return 0;
		}
	}
	case ACPI_RESOURCE_TYPE_END_TAG:
	case ACPI_RESOURCE_TYPE_START_DEPENDENT:
	case ACPI_RESOURCE_TYPE_END_DEPENDENT:
		return -ENOENT;
	default:
		out->type = ACPI_RES_OTHER;
		return 0;
	}
}

int acpi_res_decode_buffer(const uint8_t *aml, uint16_t len,
			   struct acpi_res *out)
{
	ACPI_RESOURCE *ares = NULL;
	int rc;

	if (!aml || !len || !out)
		return -EINVAL;
	if (!acpi_dev_ready())
		return -ENODEV;
	if (ACPI_FAILURE(AcpiBufferToResource((UINT8 *)aml, len, &ares)) ||
	    !ares)
		return -EINVAL;
	rc = acpi_res_decode_one(ares, out);
	AcpiOsFree(ares);
	return rc;
}

struct res_walk {
	struct acpi_res_list *list;
	int cap;
	int err;
};

static int res_append(struct res_walk *rw, const struct acpi_res *r)
{
	struct acpi_res_list *l = rw->list;

	if (l->count == rw->cap) {
		int ncap = rw->cap ? rw->cap * 2 : 8;
		struct acpi_res *n = kcalloc((size_t)ncap, sizeof(*n));

		if (!n)
			return -ENOMEM;
		if (l->res) {
			kmemcpy(n, l->res, sizeof(*n) * (size_t)l->count);
			kfree(l->res);
		}
		l->res = n;
		rw->cap = ncap;
	}
	l->res[l->count++] = *r;
	return 0;
}

static ACPI_STATUS res_walk_cb(ACPI_RESOURCE *ares, void *context)
{
	struct res_walk *rw = context;
	struct acpi_res r;

	if (acpi_res_decode_one(ares, &r) != 0)
		return AE_OK;
	rw->err = res_append(rw, &r);
	if (rw->err)
		return AE_NO_MEMORY;
	/* One entry per interrupt of a multi-interrupt descriptor. */
	if (ares->Type == ACPI_RESOURCE_TYPE_IRQ) {
		for (uint8_t i = 1; i < ares->Data.Irq.InterruptCount; i++) {
			isa_irq_to_res(ares->Data.Irq.Interrupts[i],
				       ares->Data.Irq.Triggering,
				       ares->Data.Irq.Polarity, &r.irq);
			rw->err = res_append(rw, &r);
			if (rw->err)
				return AE_NO_MEMORY;
		}
	} else if (ares->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
		for (uint8_t i = 1; i < ares->Data.ExtendedIrq.InterruptCount;
		     i++) {
			r.irq.gsi = ares->Data.ExtendedIrq.Interrupts[i];
			rw->err = res_append(rw, &r);
			if (rw->err)
				return AE_NO_MEMORY;
		}
	}
	return AE_OK;
}

int acpi_get_resources(void *handle, const char *method,
		       struct acpi_res_list *list)
{
	struct res_walk rw;
	ACPI_STATUS st;

	might_sleep();
	if (!list)
		return -EINVAL;
	list->count = 0;
	list->res = NULL;
	if (!acpi_dev_ready())
		return -ENODEV;
	if (!handle)
		return -EINVAL;
	if (!acpi_has_method(handle, method ? method : "_CRS"))
		return -ENOENT;
	rw.list = list;
	rw.cap = 0;
	rw.err = 0;
	st = AcpiWalkResources((ACPI_HANDLE)handle,
			       (char *)(method ? method : "_CRS"), res_walk_cb,
			       &rw);
	if (ACPI_FAILURE(st) || rw.err) {
		acpi_dev_free_resource_list(list);
		return rw.err ? rw.err : status_to_errno(st);
	}
	return 0;
}

void acpi_dev_free_resource_list(struct acpi_res_list *list)
{
	if (!list)
		return;
	kfree(list->res);
	list->res = NULL;
	list->count = 0;
}

const struct acpi_res *acpi_res_find(const struct acpi_res_list *list,
				     enum acpi_res_type type, int index)
{
	if (!list || index < 0)
		return NULL;
	for (int i = 0; i < list->count; i++)
		if (list->res[i].type == type && index-- == 0)
			return &list->res[i];
	return NULL;
}

int acpi_res_count(const struct acpi_res_list *list, enum acpi_res_type type)
{
	int n = 0;

	if (!list)
		return 0;
	for (int i = 0; i < list->count; i++)
		if (list->res[i].type == type)
			n++;
	return n;
}

const struct acpi_res_gpio *acpi_res_find_gpio(const struct acpi_res_list *list,
					       int conn_type, int index)
{
	if (!list || index < 0)
		return NULL;
	for (int i = 0; i < list->count; i++) {
		const struct acpi_res *r = &list->res[i];

		if (r->type != ACPI_RES_GPIO)
			continue;
		if (conn_type >= 0 && r->gpio.conn_type != conn_type)
			continue;
		if (index-- == 0)
			return &r->gpio;
	}
	return NULL;
}

int acpi_dev_gpio_int(struct acpi_device *adev, int index, void **ctrl_handle,
		      unsigned int *pin, struct acpi_res_gpio *desc)
{
	struct acpi_res_list list;
	const struct acpi_res_gpio *g;
	void *ctrl;
	int rc;

	if (!adev)
		return -EINVAL;
	rc = acpi_dev_get_resources(adev, &list);
	if (rc)
		return rc;
	g = acpi_res_find_gpio(&list, ACPI_RES_GPIO_INT, index);
	if (!g || g->npins == 0) {
		acpi_dev_free_resource_list(&list);
		return -ENOENT;
	}
	ctrl = acpi_get_handle_by_path(adev->handle, g->source);
	if (!ctrl) {
		acpi_dev_free_resource_list(&list);
		return -ENODEV;
	}
	if (ctrl_handle)
		*ctrl_handle = ctrl;
	if (pin)
		*pin = g->pins[0];
	if (desc)
		*desc = *g;
	acpi_dev_free_resource_list(&list);
	return 0;
}

unsigned int acpi_res_irq_type(uint8_t triggering, uint8_t polarity)
{
	switch (polarity) {
	case ACPI_RES_ACTIVE_LOW:
		return triggering == ACPI_RES_EDGE ? IRQ_TYPE_EDGE_FALLING :
						     IRQ_TYPE_LEVEL_LOW;
	case ACPI_RES_ACTIVE_HIGH:
		return triggering == ACPI_RES_EDGE ? IRQ_TYPE_EDGE_RISING :
						     IRQ_TYPE_LEVEL_HIGH;
	case ACPI_RES_ACTIVE_BOTH:
		if (triggering == ACPI_RES_EDGE)
			return IRQ_TYPE_EDGE_BOTH;
		return IRQ_TYPE_NONE; /* "both" levels is no trigger */
	default:
		return IRQ_TYPE_NONE;
	}
}

/* ==========================================================================
 * Device names
 *
 * "<id>:<NN>", numbered the way the ACPI scan numbers its device objects:
 * <id> is the first id of the device (_HID, else the first _CID), NN the
 * device's instance among the devices of that id (hex, two digits at
 * least), counted in the order the scan creates the objects:
 *   - pass 1 walks the namespace depth first and creates an object for
 *     every device, present or not; a device whose _STA fails gets none,
 *     and neither does anything below it.  A device that has _HID and a
 *     _DEP listing suppliers (other than the power management, mailbox
 *     and firmware update pseudo-devices) is postponed with everything
 *     below it;
 *   - pass 2 then takes the postponed devices in the order they were met
 *     and creates the objects of each one and its subtree.
 * The table is built once, the first time a name is asked for.  The
 * firmware's serial console (SPCR), which the scan hides, is not hidden
 * here.
 * ========================================================================== */

/* _DEP suppliers that never hold a consumer back (nor postpone it in the
 * scan, see the device names). */
static const struct acpi_device_id g_ignore_dep_ids[] = {
	{ "PNP0D80", 0 }, /* system power management controller */
	{ "INT33BD", 0 }, /* SoC mailbox */
	{ "LATT2021", 0 }, /* firmware update client */
	{ "", 0 },
};

struct acpi_name_ent {
	void *handle;
	char id[ACPI_ID_LEN]; /* "" when the device has neither id */
	uint32_t instance;
};

struct name_build {
	struct acpi_name_ent *e;
	int n, cap;
	void **post; /* postponed (pass 2), in the order met */
	int npost, postcap;
	int failed;
};

static DEFINE_MUTEX(g_names_lock);
static struct acpi_name_ent *g_names; /* published once built */
static int g_nnames;
static int g_names_built;

static int name_find(const struct acpi_name_ent *e, int n, void *h)
{
	for (int i = 0; i < n; i++)
		if (e[i].handle == h)
			return i;
	return -1;
}

static uint32_t name_count_id(const struct acpi_name_ent *e, int n,
			      const char *id)
{
	uint32_t c = 0;

	for (int i = 0; i < n; i++)
		if (str_eq(e[i].id, id))
			c++;
	return c;
}

/* The first id of a device (_HID, else its first _CID), "" for none. */
static void name_first_id(const ACPI_DEVICE_INFO *info, char *id)
{
	id[0] = 0;
	if (!info)
		return;
	if (info->Valid & ACPI_VALID_HID)
		str_copy(id, ACPI_ID_LEN, info->HardwareId.String,
			 info->HardwareId.Length);
	else if ((info->Valid & ACPI_VALID_CID) &&
		 info->CompatibleIdList.Count)
		str_copy(id, ACPI_ID_LEN, info->CompatibleIdList.Ids[0].String,
			 info->CompatibleIdList.Ids[0].Length);
}

/* Is a _DEP supplier one the scan does not wait for (it needs a _HID to
 * be matched at all). */
static int dep_supplier_ignored(const ACPI_DEVICE_INFO *info)
{
	if (!(info->Valid & ACPI_VALID_HID))
		return 0;
	for (const struct acpi_device_id *id = g_ignore_dep_ids; id->id[0];
	     id++) {
		if (str_eq(info->HardwareId.String, id->id))
			return 1;
		if (info->Valid & ACPI_VALID_CID)
			for (uint32_t i = 0; i < info->CompatibleIdList.Count;
			     i++)
				if (str_eq(info->CompatibleIdList.Ids[i].String,
					   id->id))
					return 1;
	}
	return 0;
}

/* The suppliers in _DEP the scan postpones a device for: only a device
 * with _HID, only a _DEP of references, the ignored kinds not counted. */
static int name_dep_count(ACPI_HANDLE h)
{
	struct acpi_value *v = NULL;
	int count = 0;

	if (!acpi_has_method(h, "_DEP"))
		return 0;
	if (!acpi_has_method(h, "_HID")) {
		/* (the id may also be reported through the device
		 * information alone) */
		ACPI_DEVICE_INFO *self = NULL;
		int hid;

		if (ACPI_FAILURE(AcpiGetObjectInfo(h, &self)) || !self)
			return 0;
		hid = (self->Valid & ACPI_VALID_HID) != 0;
		AcpiOsFree(self);
		if (!hid)
			return 0;
	}
	if (acpi_evaluate(h, "_DEP", NULL, 0, &v) || !v)
		return 0;
	if (v->type != ACPI_VALUE_PACKAGE || !v->length)
		goto out;
	for (uint32_t i = 0; i < v->length; i++)
		if (v->elements[i].type != ACPI_VALUE_REFERENCE ||
		    !v->elements[i].handle)
			goto out;
	for (uint32_t i = 0; i < v->length; i++) {
		ACPI_DEVICE_INFO *info = NULL;

		if (ACPI_FAILURE(AcpiGetObjectInfo(
			    (ACPI_HANDLE)v->elements[i].handle, &info)) ||
		    !info)
			continue;
		if (!dep_supplier_ignored(info))
			count++;
		AcpiOsFree(info);
	}
out:
	acpi_value_free(v);
	return count;
}

/* Create the "device object" of `h': AE_OK (descend), AE_CTRL_DEPTH
 * (postponed, or no object: skip the subtree). */
static ACPI_STATUS name_add(struct name_build *nb, ACPI_HANDLE h,
			    int first_pass)
{
	ACPI_DEVICE_INFO *info = NULL;
	ACPI_OBJECT sta;
	ACPI_BUFFER buf = { sizeof(sta), &sta };
	ACPI_STATUS st;
	struct acpi_name_ent *ent;

	if (nb->failed)
		return AE_CTRL_TERMINATE;
	if (name_find(nb->e, nb->n, h) >= 0)
		return AE_OK;
	if (first_pass && name_dep_count(h) > 0) {
		if (nb->npost == nb->postcap) {
			int cap = nb->postcap ? nb->postcap * 2 : 16;
			void **np = kcalloc((size_t)cap, sizeof(*np));

			if (!np) {
				nb->failed = 1;
				return AE_CTRL_TERMINATE;
			}
			if (nb->post) {
				kmemcpy(np, nb->post,
					sizeof(*np) * (size_t)nb->npost);
				kfree(nb->post);
			}
			nb->post = np;
			nb->postcap = cap;
		}
		nb->post[nb->npost++] = h;
		return AE_CTRL_DEPTH;
	}
	st = AcpiEvaluateObject(h, (char *)"_STA", NULL, &buf);
	if (st != AE_NOT_FOUND &&
	    (ACPI_FAILURE(st) || sta.Type != ACPI_TYPE_INTEGER))
		return AE_CTRL_DEPTH;
	if (nb->n == nb->cap) {
		int cap = nb->cap ? nb->cap * 2 : 128;
		struct acpi_name_ent *ne = kcalloc((size_t)cap, sizeof(*ne));

		if (!ne) {
			nb->failed = 1;
			return AE_CTRL_TERMINATE;
		}
		if (nb->e) {
			kmemcpy(ne, nb->e, sizeof(*ne) * (size_t)nb->n);
			kfree(nb->e);
		}
		nb->e = ne;
		nb->cap = cap;
	}
	ent = &nb->e[nb->n];
	ent->handle = h;
	if (ACPI_FAILURE(AcpiGetObjectInfo(h, &info)))
		info = NULL;
	name_first_id(info, ent->id);
	if (info)
		AcpiOsFree(info);
	ent->instance = ent->id[0] ? name_count_id(nb->e, nb->n, ent->id) : 0;
	nb->n++;
	return AE_OK;
}

static ACPI_STATUS name_pass1_cb(ACPI_HANDLE h, UINT32 level, void *context,
				 void **ret)
{
	(void)level;
	(void)ret;
	return name_add(context, h, 1);
}

static ACPI_STATUS name_pass2_cb(ACPI_HANDLE h, UINT32 level, void *context,
				 void **ret)
{
	(void)level;
	(void)ret;
	return name_add(context, h, 0);
}

/* Build the table once (not under g_names_lock: building runs AML). */
static void names_build_once(void)
{
	struct name_build nb;
	int built;

	mutex_lock(&g_names_lock);
	built = g_names_built;
	mutex_unlock(&g_names_lock);
	if (built)
		return;
	kmemset(&nb, 0, sizeof(nb));
	(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				ACPI_UINT32_MAX, name_pass1_cb, NULL, &nb,
				NULL);
	for (int i = 0; i < nb.npost && !nb.failed; i++) {
		ACPI_HANDLE p = (ACPI_HANDLE)nb.post[i];

		if (name_find(nb.e, nb.n, p) >= 0)
			continue;
		if (name_add(&nb, p, 0) != AE_OK)
			continue;
		(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, p, ACPI_UINT32_MAX,
					name_pass2_cb, NULL, &nb, NULL);
	}
	kfree(nb.post);
	mutex_lock(&g_names_lock);
	if (!g_names_built && !nb.failed) {
		g_names = nb.e;
		g_nnames = nb.n;
		g_names_built = 1;
		nb.e = NULL;
	}
	mutex_unlock(&g_names_lock);
	if (nb.failed)
		kprintf("ACPI: out of memory naming the devices\n");
	kfree(nb.e);
}

int acpi_dev_name(void *handle, char *buf, size_t len)
{
	ACPI_DEVICE_INFO *info = NULL;
	char id[ACPI_ID_LEN];
	uint32_t inst = 0;
	int i, rc = 0;

	might_sleep();
	if (!acpi_dev_ready())
		return -ENODEV;
	if (!handle || !buf || len < 2)
		return -EINVAL;
	names_build_once();
	/* The id comes from the device (the table has it too, but a device
	 * the scan never met is named all the same). */
	if (ACPI_FAILURE(AcpiGetObjectInfo((ACPI_HANDLE)handle, &info)))
		info = NULL;
	name_first_id(info, id);
	if (info)
		AcpiOsFree(info);
	if (!id[0])
		return -ENOENT;
	mutex_lock(&g_names_lock);
	i = g_names_built ? name_find(g_names, g_nnames, handle) : -1;
	if (i >= 0 && str_eq(g_names[i].id, id))
		inst = g_names[i].instance;
	else if (g_names_built)
		/* Not in the table: the number the scan would give a device
		 * of this id added now. */
		inst = name_count_id(g_names, g_nnames, id);
	else
		rc = -ENOMEM;
	mutex_unlock(&g_names_lock);
	if (rc)
		return rc;
	ksnprintf(buf, len, "%s:%02x", id, (unsigned int)inst);
	return 0;
}

/* ==========================================================================
 * Power: resources and D-states
 *
 * The way the ACPI scan does it:
 *   - a power resource has a reference count and a state; the state starts
 *     as the firmware left it (_STA, read when the resource is first looked
 *     at) and is then what _ON/_OFF made it;
 *   - the references start from the devices, not from first contact: when
 *     power management is first used, every present device with power
 *     resources is looked at once (its state inferred from its lists and
 *     _PSC) and references the resources of the state it is in.  A
 *     resource is switched off only when its last reference goes, so one
 *     that a device the kernel never touched still uses stays on.  One that
 *     is on although no device found in a powered state lists it is left
 *     to the firmware: it keeps a reference of its own and is never
 *     switched off;
 *   - each resource has its own lock, held across its _ON/_OFF, and each
 *     device its own, held across a whole transition (_PSx may Sleep() for
 *     hundreds of milliseconds): other devices change state meanwhile.
 *     Order: device -> resource; the tables' lock only covers the lists.
 * ========================================================================== */

#define PRES_OFF 0
#define PRES_ON 1
#define PRES_UNKNOWN 2

struct acpi_power_res {
	struct acpi_power_res *next;
	void *handle;
	struct mutex lock;
	unsigned int refs; /* devices holding it on (+1 for the firmware) */
	int state; /* PRES_* */
	int fw_hold; /* on with no device found using it: never _OFF */
};

struct acpi_pm_dev {
	struct acpi_pm_dev *next;
	void *handle;
	struct mutex lock; /* the device's transitions */
	int state; /* ACPI_DEV_*, UNKNOWN until found or set */
	/* The resources this device holds references on (the list of the
	 * state it is in). */
	struct acpi_power_res *held[ACPI_POWER_MAX_RES];
	int nheld;
};

static DEFINE_MUTEX(g_power_tables_lock); /* the two lists only */
static DEFINE_MUTEX(g_power_init_lock); /* the first-use scan */
static struct acpi_power_res *g_power_res;
static struct acpi_pm_dev *g_pm_devs;
static int g_power_inited; /* g_power_init_lock */

static struct acpi_power_res *power_res_get(void *handle)
{
	struct acpi_power_res *r;

	mutex_lock(&g_power_tables_lock);
	for (r = g_power_res; r; r = r->next)
		if (r->handle == handle)
			goto out;
	r = kcalloc(1, sizeof(*r));
	if (r) {
		r->handle = handle;
		mutex_init(&r->lock, "acpi_power_res");
		r->state = PRES_UNKNOWN;
		r->next = g_power_res;
		g_power_res = r;
	}
out:
	mutex_unlock(&g_power_tables_lock);
	return r;
}

static struct acpi_pm_dev *pm_dev_get(void *handle)
{
	struct acpi_pm_dev *pd;

	mutex_lock(&g_power_tables_lock);
	for (pd = g_pm_devs; pd; pd = pd->next)
		if (pd->handle == handle)
			goto out;
	pd = kcalloc(1, sizeof(*pd));
	if (pd) {
		pd->handle = handle;
		mutex_init(&pd->lock, "acpi_pm_dev");
		pd->state = ACPI_DEV_STATE_UNKNOWN;
		pd->next = g_pm_devs;
		g_pm_devs = pd;
	}
out:
	mutex_unlock(&g_power_tables_lock);
	return pd;
}

/* The power resources listed in _PRx; count or -errno (-ENOENT when the
 * device has no such list). */
static int power_list(void *handle, int state, struct acpi_power_res **out,
		      int max)
{
	char name[5] = { '_', 'P', 'R', (char)('0' + state), 0 };
	struct acpi_value *v = NULL;
	int n = 0, rc;

	if (!acpi_has_method(handle, name))
		return -ENOENT;
	rc = acpi_evaluate(handle, name, NULL, 0, &v);
	if (rc)
		return rc;
	if (!v || v->type != ACPI_VALUE_PACKAGE) {
		acpi_value_free(v);
		return -EPROTO;
	}
	for (uint32_t i = 0; i < v->length && n < max; i++) {
		const struct acpi_value *e = &v->elements[i];
		void *h = NULL;

		if (e->type == ACPI_VALUE_REFERENCE)
			h = e->handle;
		else if (e->type == ACPI_VALUE_STRING)
			h = acpi_get_handle_by_path(handle, e->string);
		if (h) {
			struct acpi_power_res *r = power_res_get(h);

			if (!r) {
				acpi_value_free(v);
				return -ENOMEM;
			}
			out[n++] = r;
		}
	}
	acpi_value_free(v);
	return n;
}

/* The resource's state, from _STA the first time (r->lock held). */
static int power_res_state_locked(struct acpi_power_res *r)
{
	uint64_t sta;

	mutex_assert_held(&r->lock);
	if (r->state == PRES_UNKNOWN &&
	    acpi_evaluate_integer(r->handle, "_STA", NULL, 0, &sta) == 0)
		r->state = (sta & 1) ? PRES_ON : PRES_OFF;
	return r->state;
}

/* Take a reference: the first one switches the resource on (unless the
 * firmware has it on already). */
static int power_res_ref(struct acpi_power_res *r)
{
	int rc = 0;

	mutex_lock(&r->lock);
	if (r->refs++ == 0 && power_res_state_locked(r) != PRES_ON) {
		rc = acpi_evaluate(r->handle, "_ON", NULL, 0, NULL);
		if (rc) {
			r->refs--;
			r->state = PRES_UNKNOWN;
			kprintf("ACPI: power resource %p _ON failed (%d)\n",
				r->handle, rc);
			rc = -EIO;
		} else {
			r->state = PRES_ON;
		}
	}
	mutex_unlock(&r->lock);
	return rc;
}

/* Drop a reference: the last one switches the resource off. */
static void power_res_unref(struct acpi_power_res *r)
{
	int rc;

	mutex_lock(&r->lock);
	if (WARN(r->refs == 0, "ACPI: power resource %p unbalanced",
		 r->handle))
		goto out;
	if (--r->refs > 0)
		goto out;
	rc = acpi_evaluate(r->handle, "_OFF", NULL, 0, NULL);
	if (rc) {
		/* Still on as far as anyone can tell: keep it counted. */
		r->refs++;
		r->state = PRES_UNKNOWN;
		kprintf("ACPI: power resource %p _OFF failed (%d)\n",
			r->handle, rc);
	} else {
		r->state = PRES_OFF;
	}
out:
	mutex_unlock(&r->lock);
}

/* Hold references on `list' instead of whatever the device holds now:
 * the new ones are taken first (in list order), the old ones dropped
 * after (in reverse order), so a resource in both never blinks off.
 * pd->lock held. */
static int power_transition(struct acpi_pm_dev *pd,
			    struct acpi_power_res **list, int n)
{
	struct acpi_power_res *old[ACPI_POWER_MAX_RES];
	int nold = pd->nheld;

	mutex_assert_held(&pd->lock);
	for (int i = 0; i < n; i++) {
		int rc = power_res_ref(list[i]);

		if (rc) {
			while (i-- > 0)
				power_res_unref(list[i]);
			return rc;
		}
	}
	kmemcpy(old, pd->held, sizeof(old[0]) * (size_t)nold);
	for (int i = 0; i < n; i++)
		pd->held[i] = list[i];
	pd->nheld = n;
	for (int i = nold - 1; i >= 0; i--)
		power_res_unref(old[i]);
	return 0;
}

/* Are all resources of `list' on right now. */
static int power_list_all_on(struct acpi_power_res **list, int n)
{
	for (int i = 0; i < n; i++) {
		int on;

		mutex_lock(&list[i]->lock);
		on = power_res_state_locked(list[i]) == PRES_ON;
		mutex_unlock(&list[i]->lock);
		if (!on)
			return 0;
	}
	return 1;
}

/* First contact with a device: find its state and take references on
 * what that state uses (so later transitions balance).  The state: the
 * shallowest whose power resources are all on, else D3cold; _PSC may say
 * deeper (other devices keep shared resources on); with neither, D0.
 * pd->lock held. */
static void pm_dev_init_locked(struct acpi_pm_dev *pd)
{
	struct acpi_power_res *list[ACPI_POWER_MAX_RES];
	int state = ACPI_DEV_STATE_UNKNOWN, has_res, n;
	uint64_t psc;

	mutex_assert_held(&pd->lock);
	if (pd->state != ACPI_DEV_STATE_UNKNOWN)
		return;
	has_res = acpi_has_method(pd->handle, "_PR0");
	if (has_res) {
		for (int i = ACPI_DEV_D0; i <= ACPI_DEV_D3_HOT; i++) {
			n = power_list(pd->handle, i, list,
				       ACPI_POWER_MAX_RES);
			if (n <= 0)
				continue;
			if (power_list_all_on(list, n)) {
				state = i;
				break;
			}
		}
		if (state == ACPI_DEV_STATE_UNKNOWN)
			state = ACPI_DEV_D3_COLD;
	}
	if (acpi_evaluate_integer(pd->handle, "_PSC", NULL, 0, &psc) == 0 &&
	    psc <= ACPI_DEV_D3_HOT) {
		if (state == ACPI_DEV_STATE_UNKNOWN)
			state = (int)psc;
		else if ((int)psc > state && state < ACPI_DEV_D3_COLD)
			state = (int)psc;
	}
	if (state == ACPI_DEV_STATE_UNKNOWN)
		state = ACPI_DEV_D0;
	pd->state = state;
	if (has_res && state < ACPI_DEV_D3_COLD) {
		n = power_list(pd->handle, state, list, ACPI_POWER_MAX_RES);
		if (n > 0 && power_transition(pd, list, n) != 0)
			pd->state = ACPI_DEV_STATE_UNKNOWN;
	}
}

static ACPI_STATUS power_scan_cb(ACPI_HANDLE h, UINT32 level, void *context,
				 void **ret)
{
	uint32_t sta;
	struct acpi_pm_dev *pd;

	(void)level;
	(void)context;
	(void)ret;
	sta = handle_sta(h);
	/* Not there: neither are its children. */
	if (!(sta & (ACPI_DEV_STA_PRESENT | ACPI_DEV_STA_FUNCTIONING)))
		return AE_CTRL_DEPTH;
	if (!acpi_has_method(h, "_PR0"))
		return AE_OK;
	pd = pm_dev_get(h);
	if (!pd)
		return AE_OK;
	mutex_lock(&pd->lock);
	pm_dev_init_locked(pd);
	mutex_unlock(&pd->lock);
	return AE_OK;
}

static ACPI_STATUS power_res_scan_cb(ACPI_HANDLE h, UINT32 level,
				     void *context, void **ret)
{
	(void)level;
	(void)context;
	(void)ret;
	(void)power_res_get(h);
	return AE_OK;
}

/* Once, before the first transition: every power resource of the
 * namespace, the references of every present device with power
 * resources, then the firmware's hold on resources that are on with no
 * such device using them. */
static void power_init_once(void)
{
	mutex_lock(&g_power_init_lock);
	if (!g_power_inited) {
		struct acpi_power_res *r;

		(void)AcpiWalkNamespace(ACPI_TYPE_POWER, ACPI_ROOT_OBJECT,
					ACPI_UINT32_MAX, power_res_scan_cb,
					NULL, NULL, NULL);
		(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
					ACPI_UINT32_MAX, power_scan_cb, NULL,
					NULL, NULL);
		mutex_lock(&g_power_tables_lock);
		r = g_power_res;
		mutex_unlock(&g_power_tables_lock);
		/* Entries are only ever added at the head: this walk sees
		 * every one the scan created. */
		for (; r; r = r->next) {
			mutex_lock(&r->lock);
			if (r->refs == 0 &&
			    power_res_state_locked(r) == PRES_ON) {
				r->refs = 1;
				r->fw_hold = 1;
			}
			mutex_unlock(&r->lock);
		}
		g_power_inited = 1;
	}
	mutex_unlock(&g_power_init_lock);
}

int acpi_device_power_manageable(struct acpi_device *adev)
{
	if (!adev)
		return 0;
	return acpi_has_method(adev->handle, "_PS0") ||
	       acpi_has_method(adev->handle, "_PR0");
}

int acpi_device_can_wakeup(struct acpi_device *adev)
{
	return adev && acpi_has_method(adev->handle, "_PRW");
}

int acpi_device_get_power(struct acpi_device *adev)
{
	struct acpi_pm_dev *pd;
	int state;

	might_sleep();
	if (!adev || !acpi_dev_ready())
		return ACPI_DEV_STATE_UNKNOWN;
	power_init_once();
	pd = pm_dev_get(adev->handle);
	if (!pd)
		return ACPI_DEV_STATE_UNKNOWN;
	mutex_lock(&pd->lock);
	pm_dev_init_locked(pd);
	state = pd->state;
	mutex_unlock(&pd->lock);
	return state;
}

int acpi_device_set_power(struct acpi_device *adev, int state)
{
	char ps[5] = { '_', 'P', 'S', '0', 0 };
	struct acpi_power_res *list[ACPI_POWER_MAX_RES];
	struct acpi_pm_dev *pd;
	int n = 0, rc = 0, has_ps, cur;

	might_sleep();
	if (!adev)
		return -EINVAL;
	if (!acpi_dev_ready())
		return -ENODEV;
	if (state < ACPI_DEV_D0 || state > ACPI_DEV_D3_COLD)
		return -EINVAL;
	power_init_once();
	pd = pm_dev_get(adev->handle);
	if (!pd)
		return -ENOMEM;
	mutex_lock(&pd->lock);
	pm_dev_init_locked(pd);
	cur = pd->state;
	if (cur == state)
		goto out;
	/* _PSx (both D3 states use _PS3) and _PRx (D3cold: none). */
	ps[3] = (char)('0' + (state == ACPI_DEV_D3_COLD ? 3 : state));
	has_ps = acpi_has_method(adev->handle, ps);
	if (state < ACPI_DEV_D3_COLD) {
		n = power_list(adev->handle, state, list, ACPI_POWER_MAX_RES);
		if (n == -ENOENT)
			n = 0;
		if (n < 0) {
			rc = n;
			goto out;
		}
	}
	/* D1/D2 (and D3hot without _PR3 or _PS3) must be supported
	 * explicitly; D0 and D3cold are always reachable (a device without
	 * any methods is simply left as it is). */
	if (state != ACPI_DEV_D0 && state != ACPI_DEV_D3_COLD && !has_ps &&
	    n == 0) {
		if (state != ACPI_DEV_D3_HOT ||
		    !acpi_has_method(adev->handle, "_PR0")) {
			rc = -EINVAL;
			goto out;
		}
	}
	if (cur == ACPI_DEV_STATE_UNKNOWN || state < cur) {
		/* Going up: power first, then the method. */
		rc = power_transition(pd, list, n);
		if (rc)
			goto out;
		if (has_ps) {
			rc = acpi_evaluate(adev->handle, ps, NULL, 0, NULL);
			if (rc) {
				kprintf("ACPI: %s %s failed (%d)\n", adev->path,
					ps, rc);
				rc = -EIO;
				goto out;
			}
		}
	} else {
		/* Going down: the method first, then release power. */
		if (has_ps) {
			rc = acpi_evaluate(adev->handle, ps, NULL, 0, NULL);
			if (rc) {
				kprintf("ACPI: %s %s failed (%d)\n", adev->path,
					ps, rc);
				rc = -EIO;
				goto out;
			}
		}
		rc = power_transition(pd, list, n);
		if (rc)
			goto out;
	}
	pd->state = state;
out:
	mutex_unlock(&pd->lock);
	return rc;
}

/* ==========================================================================
 * Driver-provided operation regions
 * ========================================================================== */

struct mmio_region_ctx {
	uint8_t *base;
	uint64_t length;
	spinlock_t *lock;
};

static ACPI_STATUS mmio_region_handler(UINT32 function,
				       ACPI_PHYSICAL_ADDRESS address,
				       UINT32 bits, UINT64 *value,
				       void *handler_ctx, void *region_ctx)
{
	struct mmio_region_ctx *m = handler_ctx;
	volatile uint32_t *reg;
	uint64_t fl = 0;

	(void)region_ctx;
	if (!m || !value || bits != 32 || (address & 3) ||
	    address + 4 > m->length)
		return AE_BAD_PARAMETER;
	reg = (volatile uint32_t *)(m->base + address);
	if (m->lock)
		spin_lock_irqsave(m->lock, &fl);
	if (function == ACPI_WRITE)
		*reg = (uint32_t)*value;
	else
		*value = *reg;
	if (m->lock)
		spin_unlock_irqrestore(m->lock, fl);
	return AE_OK;
}

int acpi_install_mmio_region_handler(void *handle, uint8_t space_id,
				     uint8_t *base, uint64_t length,
				     spinlock_t *lock)
{
	struct mmio_region_ctx *m;
	ACPI_STATUS st;

	if (!acpi_dev_ready())
		return -ENODEV;
	if (!handle || !base || length < 4)
		return -EINVAL;
	m = kcalloc(1, sizeof(*m));
	if (!m)
		return -ENOMEM;
	m->base = base;
	m->length = length;
	m->lock = lock;
	/* Installing runs the region's _REG, which is AML: process
	 * context.  The context lives as long as the handler, for good. */
	st = AcpiInstallAddressSpaceHandler((ACPI_HANDLE)handle, space_id,
					    mmio_region_handler, NULL, m);
	if (ACPI_FAILURE(st)) {
		kfree(m);
		return status_to_errno(st);
	}
	return 0;
}

/* ==========================================================================
 * _DEP
 * ========================================================================== */

/* Suppliers a consumer really waits for. */
static const struct acpi_device_id g_honor_dep_ids[] = {
	{ "INT3472", 0 }, /* camera sensor PMIC */
	{ "INTC1059", 0 }, /* vision sensing controller, TGL */
	{ "INTC1095", 0 }, /* vision sensing controller, ADL */
	{ "INTC100A", 0 }, /* vision sensing controller, RPL */
	{ "INTC10CF", 0 }, /* vision sensing controller, MTL */
	{ "", 0 },
};

static spinlock_t g_supplier_lock = SPINLOCK_INIT("acpi_suppliers");
static void *g_suppliers[ACPI_SUPPLIERS_MAX];
static int g_nsuppliers;

int acpi_dev_get_dependencies(struct acpi_device *adev, void **handles,
			      int max)
{
	struct acpi_value *v = NULL;
	int n = 0, rc;

	if (!adev || !handles || max <= 0)
		return -EINVAL;
	if (!acpi_has_method(adev->handle, "_DEP"))
		return 0;
	rc = acpi_evaluate(adev->handle, "_DEP", NULL, 0, &v);
	if (rc)
		return rc;
	if (!v || v->type != ACPI_VALUE_PACKAGE) {
		acpi_value_free(v);
		return -EPROTO;
	}
	for (uint32_t i = 0; i < v->length && n < max; i++) {
		const struct acpi_value *e = &v->elements[i];

		if (e->type == ACPI_VALUE_REFERENCE && e->handle)
			handles[n++] = e->handle;
		else if (e->type == ACPI_VALUE_STRING) {
			void *h = acpi_get_handle_by_path(adev->handle,
							  e->string);
			if (h)
				handles[n++] = h;
		}
	}
	acpi_value_free(v);
	return n;
}

void acpi_dev_supplier_ready(void *handle)
{
	uint64_t fl;

	if (!handle)
		return;
	spin_lock_irqsave(&g_supplier_lock, &fl);
	for (int i = 0; i < g_nsuppliers; i++)
		if (g_suppliers[i] == handle)
			goto out;
	if (!WARN_ON_ONCE(g_nsuppliers >= ACPI_SUPPLIERS_MAX))
		g_suppliers[g_nsuppliers++] = handle;
out:
	spin_unlock_irqrestore(&g_supplier_lock, fl);
}

int acpi_dev_supplier_is_ready(void *handle)
{
	uint64_t fl;
	int r = 0;

	spin_lock_irqsave(&g_supplier_lock, &fl);
	for (int i = 0; i < g_nsuppliers && !r; i++)
		r = g_suppliers[i] == handle;
	spin_unlock_irqrestore(&g_supplier_lock, fl);
	return r;
}

int acpi_dev_ready_for_enumeration(struct acpi_device *adev)
{
	void *deps[16];
	int n;

	if (!adev)
		return 0;
	n = acpi_dev_get_dependencies(adev, deps, 16);
	if (n <= 0)
		return 1;
	for (int i = 0; i < n; i++) {
		struct acpi_device *s = acpi_dev_from_handle(deps[i]);
		int wait;

		if (!s)
			continue;
		wait = !acpi_match_device_ids(s, g_ignore_dep_ids) &&
		       acpi_match_device_ids(s, g_honor_dep_ids) &&
		       acpi_dev_is_present(s) &&
		       !acpi_dev_supplier_is_ready(deps[i]);
		acpi_dev_put(s);
		if (wait)
			return 0;
	}
	return 1;
}

/* ==========================================================================
 * Namespace replaced
 * ========================================================================== */

void acpi_dev_forget_namespace(void)
{
	struct acpi_power_res *r, *rn;
	struct acpi_pm_dev *pd, *pdn;

	mutex_lock(&g_names_lock);
	kfree(g_names);
	g_names = NULL;
	g_nnames = 0;
	g_names_built = 0;
	mutex_unlock(&g_names_lock);

	mutex_lock(&g_power_init_lock);
	mutex_lock(&g_power_tables_lock);
	r = g_power_res;
	pd = g_pm_devs;
	g_power_res = NULL;
	g_pm_devs = NULL;
	g_power_inited = 0;
	mutex_unlock(&g_power_tables_lock);
	mutex_unlock(&g_power_init_lock);
	for (; r; r = rn) {
		rn = r->next;
		kfree(r);
	}
	for (; pd; pd = pdn) {
		pdn = pd->next;
		kfree(pd);
	}
}
