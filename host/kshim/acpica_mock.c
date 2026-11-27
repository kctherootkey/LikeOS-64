/* Model of the ACPI namespace behind the ACPICA interfaces (host/kshim);
 * see acpica_mock.h.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include <kernel/hal/acpi_dev.h>

#define MOCK_MAX_NODES 512
#define MOCK_MAX_REGIONS 16

static struct mock_node *g_nodes[MOCK_MAX_NODES];
static int g_nnodes;
static struct mock_node *g_root, *g_sb;
/* The FADT the ACPI device layer looks at (low-power S0 flag). */
ACPI_TABLE_FADT AcpiGbl_FADT;

static struct mock_region g_regions[MOCK_MAX_REGIONS];
static int g_nregions;

static struct mock_node *node_new(struct mock_node *parent, const char *name,
				  ACPI_OBJECT_TYPE type)
{
	struct mock_node *n = kcalloc(1, sizeof(*n));
	size_t i;

	T_ASSERT(n && g_nnodes < MOCK_MAX_NODES);
	for (i = 0; i < 4 && name[i]; i++)
		n->name[i] = name[i];
	n->type = type;
	n->parent = parent;
	if (parent) {
		struct mock_node **pp = &parent->first_child;

		while (*pp)
			pp = &(*pp)->next_sibling;
		*pp = n;
		if (parent == g_root)
			ksnprintf(n->path, sizeof(n->path), "\\%s", n->name);
		else
			ksnprintf(n->path, sizeof(n->path), "%s.%s",
				  parent->path, n->name);
	} else {
		ksnprintf(n->path, sizeof(n->path), "\\");
	}
	g_nodes[g_nnodes++] = n;
	return n;
}

void mock_ns_reset(void)
{
	/* What acpi_dev.c built from the old namespace refers to nodes
	 * freed here. */
	acpi_dev_forget_namespace();
	for (int i = 0; i < g_nnodes; i++)
		kfree(g_nodes[i]);
	g_nnodes = 0;
	g_nregions = 0;
	g_root = node_new(NULL, "\\", ACPI_TYPE_ANY);
	g_sb = node_new(g_root, "_SB_", ACPI_TYPE_DEVICE);
	/* "\_SB" is how firmware strings spell it */
	ksnprintf(g_sb->path, sizeof(g_sb->path), "\\_SB");
}

struct mock_node *mock_root(void)
{
	return g_root;
}

struct mock_node *mock_device(struct mock_node *parent, const char *name,
			      const char *hid, const char *uid)
{
	struct mock_node *n = node_new(parent ? parent : g_sb, name,
				       ACPI_TYPE_DEVICE);

	n->hid = hid;
	n->uid = uid;
	return n;
}

void mock_add_cid(struct mock_node *dev, const char *cid)
{
	T_ASSERT(dev->ncids < 4);
	dev->cids[dev->ncids++] = cid;
}

void mock_set_adr(struct mock_node *dev, UINT64 adr)
{
	dev->has_adr = 1;
	dev->adr = adr;
}

struct mock_node *mock_method(struct mock_node *parent, const char *name,
			      mock_eval_fn eval, void *priv)
{
	struct mock_node *n = node_new(parent, name, ACPI_TYPE_METHOD);

	n->eval = eval;
	n->priv = priv;
	return n;
}

struct mock_node *mock_int(struct mock_node *parent, const char *name,
			   UINT64 value)
{
	struct mock_node *n = node_new(parent, name, ACPI_TYPE_INTEGER);

	n->has_int = 1;
	n->int_value = value;
	return n;
}

struct mock_node *mock_resources(struct mock_node *parent, const char *name,
				 const ACPI_RESOURCE *res, int nres)
{
	struct mock_node *n = node_new(parent, name, ACPI_TYPE_METHOD);

	n->res = res;
	n->nres = nres;
	n->res_stride = (int)sizeof(ACPI_RESOURCE);
	return n;
}

struct mock_node *mock_resources_stride(struct mock_node *parent,
					const char *name, const void *res,
					int nres, int stride)
{
	struct mock_node *n = mock_resources(parent, name, res, nres);

	n->res_stride = stride;
	return n;
}

struct mock_node *mock_power_resource(struct mock_node *parent,
				      const char *name)
{
	return node_new(parent ? parent : g_root, name, ACPI_TYPE_POWER);
}

struct mock_node *mock_lookup(const char *path)
{
	for (int i = 0; i < g_nnodes; i++)
		if (!kstrcmp(g_nodes[i]->path, path))
			return g_nodes[i];
	/* "\_SB_.X" spelled with the padded segment */
	if (!kstrncmp(path, "\\_SB_.", 6)) {
		char p[128];

		ksnprintf(p, sizeof(p), "\\_SB.%s", path + 6);
		return mock_lookup(p);
	}
	return NULL;
}

/* The child named by the first segment of `name' (a segment shorter than
 * four characters is padded with '_', as AML stores names). */
static struct mock_node *child_named(struct mock_node *parent,
				     const char *name)
{
	char seg[4];
	int i;

	for (i = 0; i < 4 && name[i] && name[i] != '.'; i++)
		seg[i] = name[i];
	for (; i < 4; i++)
		seg[i] = '_';
	for (struct mock_node *c = parent->first_child; c; c = c->next_sibling)
		if (!kstrncmp(c->name, seg, 4))
			return c;
	return NULL;
}

static struct mock_node *resolve(struct mock_node *scope, const char *path)
{
	struct mock_node *n;

	if (!path)
		return scope;
	if (path[0] == '\\')
		return mock_lookup(path);
	if (!scope)
		return NULL;
	n = scope;
	/* relative: NameSeg(.NameSeg)* below the scope, no upward search */
	while (*path) {
		n = child_named(n, path);
		if (!n)
			return NULL;
		while (*path && *path != '.')
			path++;
		if (*path == '.')
			path++;
	}
	return n;
}

/* ---- object copies ------------------------------------------------------- */

static size_t obj_size(const ACPI_OBJECT *o)
{
	size_t s = 0;

	switch (o->Type) {
	case ACPI_TYPE_STRING:
		return o->String.Length + 1;
	case ACPI_TYPE_BUFFER:
		return o->Buffer.Length;
	case ACPI_TYPE_PACKAGE:
		s = o->Package.Count * sizeof(ACPI_OBJECT);
		for (UINT32 i = 0; i < o->Package.Count; i++)
			s += obj_size(&o->Package.Elements[i]) + 8;
		return s;
	default:
		return 0;
	}
}

static void obj_copy(ACPI_OBJECT *d, const ACPI_OBJECT *s, UINT8 **pool)
{
	*d = *s;
	switch (s->Type) {
	case ACPI_TYPE_STRING:
		d->String.Pointer = (char *)*pool;
		kmemcpy(*pool, s->String.Pointer, s->String.Length);
		(*pool)[s->String.Length] = 0;
		*pool += s->String.Length + 1;
		break;
	case ACPI_TYPE_BUFFER:
		d->Buffer.Pointer = *pool;
		kmemcpy(*pool, s->Buffer.Pointer, s->Buffer.Length);
		*pool += s->Buffer.Length;
		break;
	case ACPI_TYPE_PACKAGE:
		*pool = (UINT8 *)(((unsigned long)*pool + 7) & ~7ul);
		d->Package.Elements = (ACPI_OBJECT *)*pool;
		*pool += s->Package.Count * sizeof(ACPI_OBJECT);
		for (UINT32 i = 0; i < s->Package.Count; i++)
			obj_copy(&d->Package.Elements[i],
				 &s->Package.Elements[i], pool);
		break;
	default:
		break;
	}
}

static ACPI_STATUS deliver(const ACPI_OBJECT *o, ACPI_BUFFER *ret)
{
	size_t need = sizeof(ACPI_OBJECT) + obj_size(o) + 8;
	UINT8 *pool;

	if (!ret)
		return AE_OK;
	if (ret->Length == ACPI_ALLOCATE_BUFFER) {
		ret->Pointer = kcalloc(1, need);
		if (!ret->Pointer)
			return AE_NO_MEMORY;
		ret->Length = need;
	} else if (ret->Length < need && o->Type != ACPI_TYPE_INTEGER) {
		return AE_BUFFER_OVERFLOW;
	} else if (ret->Length < sizeof(ACPI_OBJECT)) {
		return AE_BUFFER_OVERFLOW;
	}
	pool = (UINT8 *)ret->Pointer + sizeof(ACPI_OBJECT);
	obj_copy((ACPI_OBJECT *)ret->Pointer, o, &pool);
	return AE_OK;
}

/* ---- ACPICA interfaces ---------------------------------------------------- */

void AcpiOsFree(void *p)
{
	kfree(p);
}

ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, const char *pathname,
			  ACPI_HANDLE *ret)
{
	struct mock_node *n = resolve(parent, pathname);

	if (!n)
		return AE_NOT_FOUND;
	*ret = n;
	return AE_OK;
}

ACPI_STATUS AcpiGetParent(ACPI_HANDLE object, ACPI_HANDLE *out)
{
	struct mock_node *n = object;

	if (!n || !n->parent)
		return AE_NULL_ENTRY;
	*out = n->parent;
	return AE_OK;
}

ACPI_STATUS AcpiGetType(ACPI_HANDLE object, ACPI_OBJECT_TYPE *out)
{
	struct mock_node *n = object;

	if (!n)
		return AE_BAD_PARAMETER;
	*out = n->type;
	return AE_OK;
}

ACPI_STATUS AcpiGetName(ACPI_HANDLE object, UINT32 type, ACPI_BUFFER *buf)
{
	struct mock_node *n = object;
	size_t len;

	(void)type;
	if (!n || !buf)
		return AE_BAD_PARAMETER;
	len = kstrlen(n->path) + 1;
	if (buf->Length == ACPI_ALLOCATE_BUFFER) {
		buf->Pointer = kcalloc(1, len);
		buf->Length = len;
	} else if (buf->Length < len) {
		return AE_BUFFER_OVERFLOW;
	}
	kmemcpy(buf->Pointer, n->path, len);
	return AE_OK;
}

ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE object, ACPI_STRING pathname,
			       ACPI_OBJECT_LIST *args, ACPI_BUFFER *ret)
{
	struct mock_node *n = resolve(object, pathname);
	ACPI_OBJECT out;
	ACPI_STATUS st;

	if (!n)
		return AE_NOT_FOUND;
	n->calls++;
	kmemset(&out, 0, sizeof(out));
	if (n->has_int) {
		mock_obj_int(&out, n->int_value);
		return deliver(&out, ret);
	}
	if (n->type == ACPI_TYPE_POWER || n->type == ACPI_TYPE_DEVICE)
		return AE_TYPE;
	if (!n->eval)
		return ret ? AE_AML_NO_RETURN_VALUE : AE_OK;
	st = n->eval(n, args, ret ? &out : NULL);
	if (ACPI_FAILURE(st))
		return st;
	if (!ret)
		return AE_OK;
	if (out.Type == ACPI_TYPE_ANY)
		return AE_AML_NO_RETURN_VALUE;
	return deliver(&out, ret);
}

ACPI_STATUS AcpiGetObjectInfo(ACPI_HANDLE object, ACPI_DEVICE_INFO **ret)
{
	struct mock_node *n = object;
	ACPI_DEVICE_INFO *info;
	size_t sz = sizeof(*info) + 4 * sizeof(ACPI_PNP_DEVICE_ID) + 256;
	char *pool;

	if (!n)
		return AE_BAD_PARAMETER;
	info = kcalloc(1, sz);
	if (!info)
		return AE_NO_MEMORY;
	pool = (char *)info + sizeof(*info) + 4 * sizeof(ACPI_PNP_DEVICE_ID);
	info->Type = n->type;
	kmemcpy(&info->Name, n->name, 4);
	if (n->hid) {
		info->Valid |= ACPI_VALID_HID;
		info->HardwareId.String = pool;
		info->HardwareId.Length = (UINT32)kstrlen(n->hid) + 1;
		kmemcpy(pool, n->hid, info->HardwareId.Length);
		pool += info->HardwareId.Length;
	}
	if (n->uid) {
		info->Valid |= ACPI_VALID_UID;
		info->UniqueId.String = pool;
		info->UniqueId.Length = (UINT32)kstrlen(n->uid) + 1;
		kmemcpy(pool, n->uid, info->UniqueId.Length);
		pool += info->UniqueId.Length;
	}
	if (n->has_adr) {
		info->Valid |= ACPI_VALID_ADR;
		info->Address = n->adr;
	}
	if (n->ncids) {
		info->Valid |= ACPI_VALID_CID;
		info->CompatibleIdList.Count = (UINT32)n->ncids;
		for (int i = 0; i < n->ncids; i++) {
			ACPI_PNP_DEVICE_ID *id =
				&info->CompatibleIdList.Ids[i];

			id->String = pool;
			id->Length = (UINT32)kstrlen(n->cids[i]) + 1;
			kmemcpy(pool, n->cids[i], id->Length);
			pool += id->Length;
		}
	}
	*ret = info;
	return AE_OK;
}

static ACPI_STATUS walk(struct mock_node *n, ACPI_OBJECT_TYPE type,
			UINT32 depth, UINT32 max, ACPI_WALK_CALLBACK desc,
			ACPI_WALK_CALLBACK asc, void *ctx, void **rv)
{
	for (struct mock_node *c = n->first_child; c; c = c->next_sibling) {
		ACPI_STATUS st = AE_OK;

		if (c->type == type || type == ACPI_TYPE_ANY) {
			if (desc)
				st = desc(c, depth, ctx, rv);
			if (st == AE_CTRL_TERMINATE)
				return st;
			if (st == AE_CTRL_DEPTH)
				continue;
			if (ACPI_FAILURE(st) && st != AE_CTRL_DEPTH)
				return st;
		}
		if (depth < max) {
			st = walk(c, type, depth + 1, max, desc, asc, ctx, rv);
			if (st == AE_CTRL_TERMINATE || ACPI_FAILURE(st))
				return st;
		}
		if (asc && (c->type == type || type == ACPI_TYPE_ANY))
			(void)asc(c, depth, ctx, rv);
	}
	return AE_OK;
}

ACPI_STATUS AcpiWalkNamespace(ACPI_OBJECT_TYPE type, ACPI_HANDLE start,
			      UINT32 max_depth, ACPI_WALK_CALLBACK desc,
			      ACPI_WALK_CALLBACK asc, void *ctx, void **rv)
{
	struct mock_node *s = start == ACPI_ROOT_OBJECT ? g_root : start;
	ACPI_STATUS st;

	if (!s)
		return AE_BAD_PARAMETER;
	st = walk(s, type, 1, max_depth, desc, asc, ctx, rv);
	return st == AE_CTRL_TERMINATE ? AE_OK : st;
}

ACPI_STATUS AcpiGetNextObject(ACPI_OBJECT_TYPE type, ACPI_HANDLE parent,
			      ACPI_HANDLE child, ACPI_HANDLE *out)
{
	struct mock_node *c = child ? ((struct mock_node *)child)->next_sibling :
				      ((struct mock_node *)parent)->first_child;

	for (; c; c = c->next_sibling) {
		if (type == ACPI_TYPE_ANY || c->type == type) {
			*out = c;
			return AE_OK;
		}
	}
	return AE_NOT_FOUND;
}

ACPI_STATUS AcpiWalkResources(ACPI_HANDLE device, char *name,
			      ACPI_WALK_RESOURCE_CALLBACK fn, void *ctx)
{
	struct mock_node *m = resolve(device, name);

	if (!m || !m->res)
		return AE_NOT_FOUND;
	m->calls++;
	for (int i = 0; i < m->nres; i++) {
		ACPI_RESOURCE *r = (ACPI_RESOURCE *)((const UINT8 *)m->res +
						     (long)i * m->res_stride);
		ACPI_STATUS st = fn(r, ctx);

		if (ACPI_FAILURE(st))
			return st == AE_CTRL_TERMINATE ? AE_OK : st;
		if (r->Type == ACPI_RESOURCE_TYPE_END_TAG)
			break;
	}
	return AE_OK;
}

ACPI_STATUS AcpiResourceToAddress64(ACPI_RESOURCE *r,
				    ACPI_RESOURCE_ADDRESS64 *out)
{
	kmemset(out, 0, sizeof(*out));
	switch (r->Type) {
	case ACPI_RESOURCE_TYPE_ADDRESS32: {
		ACPI_RESOURCE_ADDRESS32 *a = &r->Data.Address32;

		out->ResourceType = a->ResourceType;
		out->ProducerConsumer = a->ProducerConsumer;
		out->Info = a->Info;
		out->Address.Minimum = a->Address.Minimum;
		out->Address.Maximum = a->Address.Maximum;
		out->Address.AddressLength = a->Address.AddressLength;
		out->Address.TranslationOffset = a->Address.TranslationOffset;
		return AE_OK;
	}
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		*out = r->Data.Address64;
		return AE_OK;
	default:
		return AE_BAD_PARAMETER;
	}
}

/* Raw AML I2cSerialBus(V2) descriptor -> ACPI_RESOURCE (the I2C
 * GenericSerialBus connections).  Layout: 0x8e, length, revision,
 * resource source index, bus type 1, general flags, type-specific flags
 * (bit 0: 10-bit), type revision, type data length, connection speed
 * (4 bytes), target address (2 bytes), then the source name. */
static ACPI_STATUS mock_i2c_buffer_to_resource(UINT8 *aml, UINT16 len,
					       ACPI_RESOURCE **out)
{
	ACPI_RESOURCE *r;
	size_t namelen, name_off = 18;
	char *name;

	if (len < 19 || aml[5] != ACPI_RESOURCE_SERIAL_TYPE_I2C)
		return AE_AML_BAD_RESOURCE_LENGTH;
	namelen = kstrlen((const char *)aml + name_off) + 1;
	r = kcalloc(1, sizeof(*r) + namelen + 8);
	if (!r)
		return AE_NO_MEMORY;
	name = (char *)(r + 1);
	kmemcpy(name, aml + name_off, namelen);
	r->Type = ACPI_RESOURCE_TYPE_SERIAL_BUS;
	r->Length = sizeof(*r);
	r->Data.I2cSerialBus.Type = ACPI_RESOURCE_SERIAL_TYPE_I2C;
	r->Data.I2cSerialBus.RevisionId = aml[3];
	r->Data.I2cSerialBus.ProducerConsumer = ACPI_CONSUMER;
	r->Data.I2cSerialBus.AccessMode = (aml[7] & 1) ? ACPI_I2C_10BIT_MODE :
							 ACPI_I2C_7BIT_MODE;
	r->Data.I2cSerialBus.ConnectionSpeed =
		(UINT32)(aml[12] | aml[13] << 8 | aml[14] << 16 |
			 (UINT32)aml[15] << 24);
	r->Data.I2cSerialBus.SlaveAddress = (UINT16)(aml[16] | aml[17] << 8);
	r->Data.I2cSerialBus.ResourceSource.StringPtr = name;
	r->Data.I2cSerialBus.ResourceSource.StringLength = (UINT16)namelen;
	*out = r;
	return AE_OK;
}

/* Raw AML GPIO connection descriptor -> ACPI_RESOURCE. */
ACPI_STATUS AcpiBufferToResource(UINT8 *aml, UINT16 len, ACPI_RESOURCE **out)
{
	ACPI_RESOURCE *r;
	UINT16 pin_off, name_off, vendor_off, npins;
	UINT16 *pins;
	char *name;
	size_t namelen;

	if (len >= 1 && aml[0] == 0x8e)
		return mock_i2c_buffer_to_resource(aml, len, out);
	if (len < 23 || aml[0] != 0x8c)
		return AE_AML_BAD_RESOURCE_LENGTH;
	pin_off = (UINT16)(aml[14] | aml[15] << 8);
	name_off = (UINT16)(aml[17] | aml[18] << 8);
	vendor_off = (UINT16)(aml[19] | aml[20] << 8);
	if (pin_off > name_off || name_off >= len)
		return AE_AML_BAD_RESOURCE_LENGTH;
	npins = (UINT16)((name_off - pin_off) / 2);
	(void)vendor_off;
	namelen = kstrlen((const char *)aml + name_off) + 1;
	r = kcalloc(1, sizeof(*r) + npins * 2u + namelen + 8);
	if (!r)
		return AE_NO_MEMORY;
	pins = (UINT16 *)(r + 1);
	name = (char *)(pins + npins);
	r->Type = ACPI_RESOURCE_TYPE_GPIO;
	r->Length = sizeof(*r);
	r->Data.Gpio.RevisionId = aml[3];
	r->Data.Gpio.ConnectionType = aml[4];
	r->Data.Gpio.ProducerConsumer = (aml[5] & 1) ? ACPI_CONSUMER : ACPI_PRODUCER;
	if (aml[4] == ACPI_RESOURCE_GPIO_TYPE_INT) {
		r->Data.Gpio.Triggering = aml[7] & 1;
		r->Data.Gpio.Polarity = (aml[7] >> 1) & 3;
	} else {
		r->Data.Gpio.IoRestriction = aml[7] & 3;
	}
	r->Data.Gpio.Shareable = (aml[7] >> 3) & 1;
	r->Data.Gpio.WakeCapable = (aml[7] >> 4) & 1;
	r->Data.Gpio.PinConfig = aml[9];
	r->Data.Gpio.DriveStrength = (UINT16)(aml[10] | aml[11] << 8);
	r->Data.Gpio.DebounceTimeout = (UINT16)(aml[12] | aml[13] << 8);
	r->Data.Gpio.PinTableLength = npins;
	for (UINT16 i = 0; i < npins; i++)
		pins[i] = (UINT16)(aml[pin_off + 2 * i] |
				   aml[pin_off + 2 * i + 1] << 8);
	r->Data.Gpio.PinTable = pins;
	kmemcpy(name, aml + name_off, namelen);
	r->Data.Gpio.ResourceSource.StringPtr = name;
	r->Data.Gpio.ResourceSource.StringLength = (UINT16)namelen;
	r->Data.Gpio.ResourceSource.Index = aml[16];
	*out = r;
	return AE_OK;
}

ACPI_STATUS AcpiInstallAddressSpaceHandler(ACPI_HANDLE device,
					   ACPI_ADR_SPACE_TYPE space,
					   ACPI_ADR_SPACE_HANDLER handler,
					   ACPI_ADR_SPACE_SETUP setup,
					   void *context)
{
	(void)setup;
	if (g_nregions >= MOCK_MAX_REGIONS)
		return AE_NO_MEMORY;
	g_regions[g_nregions].device = device;
	g_regions[g_nregions].space = space;
	g_regions[g_nregions].handler = handler;
	g_regions[g_nregions].context = context;
	g_nregions++;
	return AE_OK;
}

ACPI_STATUS AcpiRemoveAddressSpaceHandler(ACPI_HANDLE device,
					  ACPI_ADR_SPACE_TYPE space,
					  ACPI_ADR_SPACE_HANDLER handler)
{
	for (int i = 0; i < g_nregions; i++) {
		if (g_regions[i].device == device &&
		    g_regions[i].space == space &&
		    g_regions[i].handler == handler) {
			g_regions[i] = g_regions[--g_nregions];
			return AE_OK;
		}
	}
	return AE_NOT_EXIST;
}

struct mock_region *mock_find_region(ACPI_HANDLE device,
				     ACPI_ADR_SPACE_TYPE space)
{
	for (int i = 0; i < g_nregions; i++)
		if (g_regions[i].device == device &&
		    g_regions[i].space == space)
			return &g_regions[i];
	return NULL;
}

/* ---- resource builders ------------------------------------------------------ */

static void res_source(ACPI_RESOURCE_SOURCE *s, const char *src)
{
	s->Index = 0;
	s->StringPtr = (char *)src;
	s->StringLength = src ? (UINT16)(kstrlen(src) + 1) : 0;
}

void mock_res_end(ACPI_RESOURCE *r)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_END_TAG;
	r->Length = 16;
}

void mock_res_gpio(ACPI_RESOURCE *r, int interrupt, int level, int polarity,
		   int shared, int wake, int pull, int iorestrict,
		   UINT16 debounce, const char *source, UINT16 *pins,
		   int npins)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_GPIO;
	r->Length = sizeof(*r);
	r->Data.Gpio.RevisionId = 1;
	r->Data.Gpio.ConnectionType = interrupt ? ACPI_RESOURCE_GPIO_TYPE_INT :
						  ACPI_RESOURCE_GPIO_TYPE_IO;
	r->Data.Gpio.ProducerConsumer = ACPI_CONSUMER;
	r->Data.Gpio.Triggering = level ? ACPI_LEVEL_SENSITIVE :
					  ACPI_EDGE_SENSITIVE;
	r->Data.Gpio.Polarity = (UINT8)polarity;
	r->Data.Gpio.Shareable = shared ? ACPI_SHARED : ACPI_EXCLUSIVE;
	r->Data.Gpio.WakeCapable = wake ? ACPI_WAKE_CAPABLE : 0;
	r->Data.Gpio.PinConfig = (UINT8)pull;
	r->Data.Gpio.IoRestriction = (UINT8)iorestrict;
	r->Data.Gpio.DebounceTimeout = debounce;
	r->Data.Gpio.PinTableLength = (UINT16)npins;
	r->Data.Gpio.PinTable = pins;
	res_source(&r->Data.Gpio.ResourceSource, source);
}

void mock_res_i2c(ACPI_RESOURCE *r, UINT16 addr, UINT32 speed, int tenbit,
		  const char *source, int v2)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_SERIAL_BUS;
	r->Length = sizeof(*r);
	r->Data.I2cSerialBus.Type = ACPI_RESOURCE_SERIAL_TYPE_I2C;
	r->Data.I2cSerialBus.RevisionId = v2 ? 2 : 1;
	r->Data.I2cSerialBus.ProducerConsumer = ACPI_CONSUMER;
	r->Data.I2cSerialBus.SlaveMode = ACPI_CONTROLLER_INITIATED;
	r->Data.I2cSerialBus.ConnectionSharing = ACPI_EXCLUSIVE;
	r->Data.I2cSerialBus.AccessMode = tenbit ? ACPI_I2C_10BIT_MODE :
						   ACPI_I2C_7BIT_MODE;
	r->Data.I2cSerialBus.SlaveAddress = addr;
	r->Data.I2cSerialBus.ConnectionSpeed = speed;
	res_source(&r->Data.I2cSerialBus.ResourceSource, source);
}

void mock_res_extirq(ACPI_RESOURCE *r, int level, int polarity, int shared,
		     int wake, const UINT32 *irqs, int n)
{
	kmemset(r, 0, sizeof(*r));
	T_ASSERT(n >= 1);
	r->Type = ACPI_RESOURCE_TYPE_EXTENDED_IRQ;
	r->Length = sizeof(*r);
	r->Data.ExtendedIrq.ProducerConsumer = ACPI_CONSUMER;
	r->Data.ExtendedIrq.Triggering = level ? ACPI_LEVEL_SENSITIVE :
						 ACPI_EDGE_SENSITIVE;
	r->Data.ExtendedIrq.Polarity = (UINT8)polarity;
	r->Data.ExtendedIrq.Shareable = shared ? ACPI_SHARED : ACPI_EXCLUSIVE;
	r->Data.ExtendedIrq.WakeCapable = wake ? ACPI_WAKE_CAPABLE : 0;
	r->Data.ExtendedIrq.InterruptCount = (UINT8)n;
	/* room for n entries is the caller's (mock_resources_stride) */
	for (int i = 0; i < n; i++)
		r->Data.ExtendedIrq.Interrupts[i] = irqs[i];
}

void mock_res_irq(ACPI_RESOURCE *r, int level, int polarity, int shared,
		  const UINT8 *irqs, int n)
{
	kmemset(r, 0, sizeof(*r));
	T_ASSERT(n >= 1);
	r->Type = ACPI_RESOURCE_TYPE_IRQ;
	r->Length = sizeof(*r);
	r->Data.Irq.Triggering = level ? ACPI_LEVEL_SENSITIVE :
					 ACPI_EDGE_SENSITIVE;
	r->Data.Irq.Polarity = (UINT8)polarity;
	r->Data.Irq.Shareable = shared ? ACPI_SHARED : ACPI_EXCLUSIVE;
	r->Data.Irq.InterruptCount = (UINT8)n;
	for (int i = 0; i < n; i++)
		r->Data.Irq.Interrupts[i] = irqs[i];
}

void mock_res_mem32fixed(ACPI_RESOURCE *r, UINT32 addr, UINT32 len, int rw)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_FIXED_MEMORY32;
	r->Length = sizeof(*r);
	r->Data.FixedMemory32.Address = addr;
	r->Data.FixedMemory32.AddressLength = len;
	r->Data.FixedMemory32.WriteProtect = rw ? ACPI_READ_WRITE_MEMORY :
						  ACPI_READ_ONLY_MEMORY;
}

void mock_res_mem32(ACPI_RESOURCE *r, UINT32 min, UINT32 len, int rw)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_MEMORY32;
	r->Length = sizeof(*r);
	r->Data.Memory32.Minimum = min;
	r->Data.Memory32.Maximum = min;
	r->Data.Memory32.AddressLength = len;
	r->Data.Memory32.WriteProtect = rw ? ACPI_READ_WRITE_MEMORY :
					     ACPI_READ_ONLY_MEMORY;
}

void mock_res_addr64(ACPI_RESOURCE *r, UINT64 min, UINT64 max, UINT64 len,
		     UINT64 tra, int producer)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_ADDRESS64;
	r->Length = sizeof(*r);
	r->Data.Address64.ResourceType = ACPI_MEMORY_RANGE;
	r->Data.Address64.ProducerConsumer = producer ? ACPI_PRODUCER :
							ACPI_CONSUMER;
	r->Data.Address64.Info.Mem.WriteProtect = ACPI_READ_WRITE_MEMORY;
	r->Data.Address64.Info.Mem.Caching = ACPI_CACHABLE_MEMORY;
	r->Data.Address64.Address.Minimum = min;
	r->Data.Address64.Address.Maximum = max;
	r->Data.Address64.Address.AddressLength = len;
	r->Data.Address64.Address.TranslationOffset = tra;
}

void mock_res_io(ACPI_RESOURCE *r, UINT16 min, UINT8 len)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_IO;
	r->Length = sizeof(*r);
	r->Data.Io.Minimum = min;
	r->Data.Io.Maximum = min;
	r->Data.Io.AddressLength = len;
}

void mock_res_fixeddma(ACPI_RESOURCE *r, UINT16 line, UINT16 chan)
{
	kmemset(r, 0, sizeof(*r));
	r->Type = ACPI_RESOURCE_TYPE_FIXED_DMA;
	r->Length = sizeof(*r);
	r->Data.FixedDma.RequestLines = line;
	r->Data.FixedDma.Channels = chan;
	r->Data.FixedDma.Width = ACPI_DMA_WIDTH32;
}
