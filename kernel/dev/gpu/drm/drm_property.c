// LikeOS -- display-manager core: properties and blobs.
//
// The device's property table (name, type, range or enum values) and the
// blobs -- opaque byte arrays named by an id, which carry EDIDs, modes,
// format lists and lookup tables between the core and clients.  The
// property and blob ioctls: GETPROPERTY, GETPROPBLOB, CREATEPROPBLOB and
// DESTROYPROPBLOB.
//
// Properties are made while the driver sets the device up and live as long
// as the device.  Their type is fixed at creation and decides what a
// client may write (drm_property_change_valid_get): a range or signed range
// bounds the value, an enum names the values allowed, a bitmask the bits,
// an object property the type of object its value names, and a blob
// property takes a blob id.  Enum and bitmask properties have no fixed
// limit on their entries: the first few sit in the table entry, a longer
// list is allocated.
//
// Blobs are reference counted.  The creator holds the first reference: the
// kernel for the ones it makes (EDIDs, modes, format lists), a file for
// the ones a client makes through CREATEPROPBLOB.  Only that file may
// destroy such a blob, and whatever it still holds when it closes goes
// then -- before, a client's blobs outlived it and filled the table.  A
// blob may be as large as the biggest lookup table a crtc takes (a little
// over 2 MiB), so what one file owns at a time is bounded in bytes as well
// (DRM_PROPERTY_BLOB_FILE_MAX).
// Lookups take a reference for as long as they read the blob, so a blob a
// reader is copying out is not freed under it; the slot and its id are
// released with the last reference.  One lock covers the blob tables'
// slots and counts; the data is only read or copied with a reference
// held, never under the lock (a copy to user space may fault).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* The entries an enum or bitmask keeps inside its table entry before it
 * needs an allocated list. */
#define PROP_INLINE_ENUMS ((uint32_t)(sizeof(((struct drm_prop *)0)->enums) / \
				      sizeof(((struct drm_prop *)0)->enums[0])))

/* Every device's blob slots and reference counts.  Blob traffic is a few
 * calls per mode set, so one lock for all devices is plenty; it is never
 * held across anything that can sleep or fault. */
static spinlock_t g_blob_lock = SPINLOCK_INIT("drm_blob");

static uint32_t prop_strlen(const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* A name into a DRM_PROP_NAME_LEN field, zero padded. */
static void prop_name_copy(char *dst, const char *src)
{
	uint32_t i = 0;

	for (; i < DRM_PROP_NAME_LEN - 1 && src[i]; i++)
		dst[i] = src[i];
	for (; i < DRM_PROP_NAME_LEN; i++)
		dst[i] = 0;
}

/* ---- properties --------------------------------------------------------- */

static bool drm_property_flags_valid(uint32_t flags)
{
	uint32_t legacy_type = flags & DRM_MODE_PROP_LEGACY_TYPE;
	uint32_t ext_type = flags & DRM_MODE_PROP_EXTENDED_TYPE;

	/* Reject undefined/deprecated flags */
	if (flags & ~(DRM_MODE_PROP_LEGACY_TYPE |
		      DRM_MODE_PROP_EXTENDED_TYPE |
		      DRM_MODE_PROP_IMMUTABLE |
		      DRM_MODE_PROP_ATOMIC))
		return false;

	/* Either a legacy type or an extended type, not both */
	if (!legacy_type == !ext_type)
		return false;

	/* Only one legacy type at a time */
	if (legacy_type && (legacy_type & (legacy_type - 1)))
		return false;

	return true;
}

static bool prop_is_enum_like(const struct drm_prop *p)
{
	return drm_property_type_is(p, DRM_MODE_PROP_ENUM) ||
	       drm_property_type_is(p, DRM_MODE_PROP_BITMASK);
}

/* A fresh table entry with the next id; NULL when the table is full. */
static struct drm_prop *prop_slot_alloc(struct drm_device *dev)
{
	if (dev->nprops >= DRM_MAX_PROPS)
		return NULL;
	struct drm_prop *p = &dev->props[dev->nprops++];
	mm_memset(p, 0, sizeof(*p));
	p->id = drm_mode_id_alloc(dev);
	p->dev = dev;
	return p;
}

/* The older entry point: no checks on the flags, a name longer than the
 * field is cut, and an enum takes as many entries as it is given. */
struct drm_prop *drm_prop_add(struct drm_device *dev, const char *name,
			      uint32_t flags)
{
	struct drm_prop *p = prop_slot_alloc(dev);

	if (!p)
		return NULL;
	p->flags = flags;
	prop_name_copy(p->name, name);
	if (prop_is_enum_like(p))
		p->num_values = -1;
	else if (drm_property_type_is(p, DRM_MODE_PROP_RANGE) ||
		 drm_property_type_is(p, DRM_MODE_PROP_SIGNED_RANGE))
		p->num_values = 2;
	else if (drm_property_type_is(p, DRM_MODE_PROP_OBJECT))
		p->num_values = 1;
	else
		p->num_values = 0;
	return p;
}

void drm_prop_enum(struct drm_prop *p, uint64_t value, const char *name)
{
	char n[DRM_PROP_NAME_LEN];

	if (!p)
		return;
	prop_name_copy(n, name);
	(void)drm_property_add_enum(p, value, n);
}

struct drm_prop *drm_prop_find(struct drm_device *dev, uint32_t id)
{
	if (!id)
		return NULL;
	for (uint32_t i = 0; i < dev->nprops; i++)
		if (dev->props[i].id == id)
			return &dev->props[i];
	return NULL;
}

struct drm_prop *drm_property_create(struct drm_device *dev, uint32_t flags,
				     const char *name, int num_values)
{
	struct drm_mode_property_enum *list = NULL;
	struct drm_prop *p;
	bool enum_like = (flags & (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BITMASK)) &&
			 !(flags & DRM_MODE_PROP_EXTENDED_TYPE);

	if (WARN_ON(!drm_property_flags_valid(flags)))
		return NULL;
	if (WARN_ON(prop_strlen(name) >= DRM_PROP_NAME_LEN))
		return NULL;
	if (WARN_ON(num_values < 0))
		return NULL;
	/* Ranges keep min and max, an object property its object type: the
	 * table entry has room for two values and no more. */
	if (WARN_ON(!enum_like && num_values > 2))
		return NULL;
	/* An enum longer than the entry's own room gets its list now, so
	 * adding the entries cannot fail for want of memory. */
	if (enum_like && (uint32_t)num_values > PROP_INLINE_ENUMS) {
		list = kcalloc((size_t)num_values, sizeof(*list));
		if (!list)
			return NULL;
	}
	p = prop_slot_alloc(dev);
	if (!p) {
		kfree(list);
		return NULL;
	}
	p->flags = flags;
	p->num_values = num_values;
	prop_name_copy(p->name, name);
	if (list) {
		p->enum_list = list;
		p->enum_list_cap = (uint32_t)num_values;
	}
	return p;
}

struct drm_prop *drm_property_create_enum(struct drm_device *dev,
					  uint32_t flags, const char *name,
					  const struct drm_prop_enum_list *props,
					  int num_values)
{
	struct drm_prop *property;
	int i, ret;

	flags |= DRM_MODE_PROP_ENUM;

	property = drm_property_create(dev, flags, name, num_values);
	if (!property)
		return NULL;

	for (i = 0; i < num_values; i++) {
		ret = drm_property_add_enum(property, (uint64_t)props[i].type,
					    props[i].name);
		if (ret) {
			drm_property_destroy(dev, property);
			return NULL;
		}
	}

	return property;
}

struct drm_prop *drm_property_create_bitmask(struct drm_device *dev,
					     uint32_t flags, const char *name,
					     const struct drm_prop_enum_list *props,
					     int num_props,
					     uint64_t supported_bits)
{
	struct drm_prop *property;
	int i, ret;
	int num_values = 0;

	for (uint64_t b = supported_bits; b; b &= b - 1)
		num_values++;

	flags |= DRM_MODE_PROP_BITMASK;

	property = drm_property_create(dev, flags, name, num_values);
	if (!property)
		return NULL;
	for (i = 0; i < num_props; i++) {
		if (props[i].type < 0 || props[i].type > 63 ||
		    !(supported_bits & (1ULL << props[i].type)))
			continue;

		ret = drm_property_add_enum(property, (uint64_t)props[i].type,
					    props[i].name);
		if (ret) {
			drm_property_destroy(dev, property);
			return NULL;
		}
	}

	return property;
}

static struct drm_prop *property_create_range(struct drm_device *dev,
					      uint32_t flags, const char *name,
					      uint64_t min, uint64_t max)
{
	struct drm_prop *property;

	property = drm_property_create(dev, flags, name, 2);
	if (!property)
		return NULL;

	property->values[0] = min;
	property->values[1] = max;

	return property;
}

struct drm_prop *drm_property_create_range(struct drm_device *dev,
					   uint32_t flags, const char *name,
					   uint64_t min, uint64_t max)
{
	return property_create_range(dev, DRM_MODE_PROP_RANGE | flags,
				     name, min, max);
}

struct drm_prop *drm_property_create_signed_range(struct drm_device *dev,
						  uint32_t flags,
						  const char *name,
						  int64_t min, int64_t max)
{
	return property_create_range(dev, DRM_MODE_PROP_SIGNED_RANGE | flags,
				     name, (uint64_t)min, (uint64_t)max);
}

struct drm_prop *drm_property_create_object(struct drm_device *dev,
					    uint32_t flags, const char *name,
					    uint32_t type)
{
	struct drm_prop *property;

	flags |= DRM_MODE_PROP_OBJECT;

	/* Only atomic clients set object properties. */
	if (WARN_ON(!(flags & DRM_MODE_PROP_ATOMIC)))
		return NULL;

	property = drm_property_create(dev, flags, name, 1);
	if (!property)
		return NULL;

	property->values[0] = type;

	return property;
}

struct drm_prop *drm_property_create_bool(struct drm_device *dev,
					  uint32_t flags, const char *name)
{
	return drm_property_create_range(dev, flags, name, 0, 1);
}

int drm_property_add_enum(struct drm_prop *property, uint64_t value,
			  const char *name)
{
	struct drm_mode_property_enum *e;
	const struct drm_mode_property_enum *list;
	uint32_t index;

	if (WARN_ON(prop_strlen(name) >= DRM_PROP_NAME_LEN))
		return -EINVAL;

	if (WARN_ON(!prop_is_enum_like(property)))
		return -EINVAL;

	/* A bitmask's values are bit numbers, 0 to 63. */
	if (WARN_ON(drm_property_type_is(property, DRM_MODE_PROP_BITMASK) &&
		    value > 63))
		return -EINVAL;

	list = drm_property_enums(property);
	for (index = 0; index < property->nenums; index++)
		if (WARN_ON(list[index].value == value))
			return -EINVAL;

	if (property->num_values >= 0 &&
	    WARN_ON(index >= (uint32_t)property->num_values))
		return -EINVAL;

	if (!property->enum_list && index < PROP_INLINE_ENUMS) {
		e = &property->enums[index];
	} else {
		/* Past the entry's own room: move to (or grow) the
		 * allocated list, which then holds every entry. */
		if (!property->enum_list || index >= property->enum_list_cap) {
			uint32_t cap = property->enum_list_cap ?
					       property->enum_list_cap * 2 :
					       PROP_INLINE_ENUMS * 2;
			struct drm_mode_property_enum *n;

			while (cap <= index)
				cap *= 2;
			n = kcalloc(cap, sizeof(*n));
			if (!n)
				return -ENOMEM;
			mm_memcpy(n, drm_property_enums(property),
				  property->nenums * sizeof(*n));
			kfree(property->enum_list);
			property->enum_list = n;
			property->enum_list_cap = cap;
		}
		e = &property->enum_list[index];
	}

	prop_name_copy(e->name, name);
	e->value = value;
	property->nenums = index + 1;
	return 0;
}

void drm_property_destroy(struct drm_device *dev, struct drm_prop *property)
{
	if (!property)
		return;
	kfree(property->enum_list);
	/* The newest entry -- the only kind a failed creation leaves -- goes
	 * from the table; one further down stays as a hole nothing finds
	 * (drm_prop_find never matches id 0).  The id is not reused. */
	if (dev->nprops && property == &dev->props[dev->nprops - 1])
		dev->nprops--;
	mm_memset(property, 0, sizeof(*property));
}

/* GETPROPERTY: the name, the flags, the values (a range's min and max, an
 * object property's object type, an enum's values or a bitmask's bit
 * numbers) and, for an enum or bitmask, the named entries.  Each array is
 * filled as far as the caller made room and its count says how much room
 * it takes; a client asks once to size them and again to fill them. */
int drm_mode_getproperty_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_get_property *out_resp = kb;
	struct drm_prop *property;
	const struct drm_mode_property_enum *list;
	uint32_t value_count, enum_count, copied, i;
	bool enum_like;

	property = drm_prop_find(dev, out_resp->prop_id);
	if (!property)
		return -ENOENT;

	mm_memcpy(out_resp->name, property->name, DRM_PROP_NAME_LEN);
	out_resp->flags = property->flags;

	enum_like = prop_is_enum_like(property);
	list = drm_property_enums(property);
	if (enum_like)
		value_count = property->nenums;
	else
		value_count = property->num_values > 2 ? 2 :
			      property->num_values < 0 ? 0 :
			      (uint32_t)property->num_values;

	/* A null array is room for nothing (not a fault): clients that only
	 * size a property pass counts with no arrays. */
	if (out_resp->values_ptr) {
		for (i = 0; i < value_count && i < out_resp->count_values; i++) {
			uint64_t v = enum_like ? list[i].value : property->values[i];
			uint64_t dst = out_resp->values_ptr + (uint64_t)i * sizeof(v);

			if (!validate_user_ptr(dst, sizeof(v)) ||
			    copy_to_user((void *)(uintptr_t)dst, &v, sizeof(v)) != 0)
				return -EFAULT;
		}
	}
	out_resp->count_values = value_count;

	if (enum_like) {
		copied = 0;
		enum_count = 0;
		for (i = 0; i < property->nenums; i++) {
			enum_count++;
			if (out_resp->count_enum_blobs < enum_count ||
			    !out_resp->enum_blob_ptr)
				continue;
			uint64_t dst = out_resp->enum_blob_ptr +
				       (uint64_t)copied * sizeof(list[i]);
			if (!validate_user_ptr(dst, sizeof(list[i])) ||
			    copy_to_user((void *)(uintptr_t)dst, &list[i],
					 sizeof(list[i])) != 0)
				return -EFAULT;
			copied++;
		}
		out_resp->count_enum_blobs = enum_count;
	} else {
		/* A blob property's value is read with GETPROPBLOB, never
		 * listed here; nor does any other type have entries. */
		out_resp->count_enum_blobs = 0;
	}
	return 0;
}

/* ---- blobs -------------------------------------------------------------- */

/* Put `data' (taken over: freed with the blob, or here on failure) into a
 * free slot with one reference held by `owner' (NULL: the kernel).  NULL
 * when the table is full. */
static struct drm_blob *blob_install(struct drm_device *dev, void *data,
				     uint32_t length, struct drm_file *owner)
{
	struct drm_blob *b = NULL;
	uint64_t fl;

	spin_lock_irqsave(&g_blob_lock, &fl);
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		struct drm_blob *s = &dev->blobs[i];

		/* A slot is free once its last reference went; refs is
		 * checked as well so a slot in the middle of being freed
		 * is never handed out. */
		if (s->in_use || s->refs)
			continue;
		s->in_use = 1;
		s->id = drm_mode_id_alloc(dev);
		s->data = data;
		s->length = length;
		s->dev = dev;
		s->refs = 1;
		s->owner = owner;
		b = s;
		break;
	}
	spin_unlock_irqrestore(&g_blob_lock, fl);
	if (!b)
		kfree(data);
	return b;
}

/* A kernel blob holding a copy of `data' (zeroed when NULL).  A length of
 * 0 is a blob with no bytes (the older entry point allows it). */
static struct drm_blob *blob_new(struct drm_device *dev, const void *data,
				 uint32_t length)
{
	void *d = kalloc(length ? length : 1);

	if (!d)
		return NULL;
	if (data)
		mm_memcpy(d, data, length);
	else
		mm_memset(d, 0, length ? length : 1);
	return blob_install(dev, d, length, NULL);
}

struct drm_blob *drm_property_create_blob(struct drm_device *dev,
					  size_t length, const void *data)
{
	if (!length || length > 0x7fffffffu)
		return NULL;
	return blob_new(dev, data, (uint32_t)length);
}

struct drm_blob *drm_property_blob_get(struct drm_blob *blob)
{
	uint64_t fl;

	spin_lock_irqsave(&g_blob_lock, &fl);
	WARN_ON(blob->refs <= 0);
	blob->refs++;
	spin_unlock_irqrestore(&g_blob_lock, fl);
	return blob;
}

void drm_property_blob_put(struct drm_blob *blob)
{
	void *data = NULL;
	uint64_t fl;

	if (!blob)
		return;
	spin_lock_irqsave(&g_blob_lock, &fl);
	if (WARN_ON(blob->refs <= 0)) {
		spin_unlock_irqrestore(&g_blob_lock, fl);
		return;
	}
	if (--blob->refs == 0) {
		/* The last one: the id stops naming anything and the slot
		 * is free; the bytes are freed outside the lock. */
		data = blob->data;
		blob->data = NULL;
		blob->id = 0;
		blob->length = 0;
		blob->owner = NULL;
		blob->in_use = 0;
	}
	spin_unlock_irqrestore(&g_blob_lock, fl);
	kfree(data);
}

struct drm_blob *drm_property_lookup_blob(struct drm_device *dev, uint32_t id)
{
	struct drm_blob *b = NULL;
	uint64_t fl;

	if (!id)
		return NULL;
	spin_lock_irqsave(&g_blob_lock, &fl);
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		struct drm_blob *s = &dev->blobs[i];

		if (s->in_use && s->refs > 0 && s->id == id) {
			s->refs++;
			b = s;
			break;
		}
	}
	spin_unlock_irqrestore(&g_blob_lock, fl);
	return b;
}

/* The older entry points, by id.  drm_blob_create makes a kernel blob
 * (one reference, the creator's) and returns its id, 0 when there is no
 * room.  drm_blob_find returns the blob WITHOUT a reference: the caller
 * reads it at once and keeps nothing.  drm_blob_destroy drops the kernel's
 * creator reference -- the blob itself goes once nobody else holds one. */
uint32_t drm_blob_create(struct drm_device *dev, const void *data,
			 uint32_t length)
{
	struct drm_blob *b = blob_new(dev, data, length);

	return b ? b->id : 0;
}

struct drm_blob *drm_blob_find(struct drm_device *dev, uint32_t id)
{
	struct drm_blob *b = NULL;
	uint64_t fl;

	if (!id)
		return NULL;
	spin_lock_irqsave(&g_blob_lock, &fl);
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (dev->blobs[i].in_use && dev->blobs[i].refs > 0 &&
		    dev->blobs[i].id == id) {
			b = &dev->blobs[i];
			break;
		}
	}
	spin_unlock_irqrestore(&g_blob_lock, fl);
	return b;
}

void drm_blob_destroy(struct drm_device *dev, uint32_t id)
{
	struct drm_blob *b = drm_blob_find(dev, id);

	if (!b)
		return;
	/* A client's blob is that client's to destroy; the kernel only
	 * ever drops the ones it made. */
	if (WARN_ON_ONCE(b->owner != NULL))
		return;
	drm_property_blob_put(b);
}

/* The owning file's count of blob bytes, as its reference on a blob goes
 * (under g_blob_lock). */
static void blob_owner_uncharge(struct drm_file *fp, const struct drm_blob *b)
{
#if DRM_PROPERTY_BLOB_LARGE
	if (fp)
		fp->blob_bytes = fp->blob_bytes >= b->length ? fp->blob_bytes - b->length : 0;
#else
	(void)fp;
	(void)b;
#endif
}

bool drm_property_replace_blob(struct drm_blob **blob,
			       struct drm_blob *new_blob)
{
	struct drm_blob *old_blob = *blob;

	if (old_blob == new_blob)
		return false;

	drm_property_blob_put(old_blob);
	if (new_blob)
		drm_property_blob_get(new_blob);
	*blob = new_blob;
	return true;
}

int drm_property_replace_blob_from_id(struct drm_device *dev,
				      struct drm_blob **blob,
				      uint64_t blob_id,
				      int64_t max_size,
				      int64_t expected_size,
				      int64_t expected_elem_size,
				      bool *replaced)
{
	struct drm_blob *new_blob = NULL;

	if (blob_id != 0) {
		if (blob_id > 0xffffffffULL)
			return -EINVAL;
		new_blob = drm_property_lookup_blob(dev, (uint32_t)blob_id);
		if (new_blob == NULL)
			return -EINVAL;

		if (max_size > 0 &&
		    (int64_t)new_blob->length > max_size) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}

		if (expected_size > 0 &&
		    (int64_t)new_blob->length != expected_size) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
		if (expected_elem_size > 0 &&
		    (int64_t)new_blob->length % expected_elem_size != 0) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
	}

	*replaced |= drm_property_replace_blob(blob, new_blob);
	drm_property_blob_put(new_blob);

	return 0;
}

int drm_property_replace_global_blob(struct drm_device *dev,
				     struct drm_blob **replace,
				     size_t length, const void *data,
				     struct drm_object_properties *obj_holds_id,
				     struct drm_prop *prop_holds_id)
{
	struct drm_blob *new_blob = NULL;
	struct drm_blob *old_blob;
	int ret;

	if (WARN_ON(replace == NULL))
		return -EINVAL;

	old_blob = *replace;

	if (length && data) {
		new_blob = drm_property_create_blob(dev, length, data);
		if (!new_blob)
			return -ENOMEM;
	}

	if (obj_holds_id) {
		ret = drm_object_property_set_value(obj_holds_id, prop_holds_id,
						    new_blob ? new_blob->id : 0);
		if (ret != 0)
			goto err_created;
	}

	drm_property_blob_put(old_blob);
	*replace = new_blob;

	return 0;

err_created:
	drm_property_blob_put(new_blob);
	return ret;
}

void drm_property_destroy_user_blobs(struct drm_device *dev,
				     struct drm_file *fp)
{
	/* One at a time: the file's reference is taken off under the lock
	 * (so a DESTROYPROPBLOB racing with the close cannot drop it a
	 * second time) and dropped outside it. */
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		struct drm_blob *b = &dev->blobs[i];
		bool mine = false;
		uint64_t fl;

		spin_lock_irqsave(&g_blob_lock, &fl);
		if (b->in_use && b->refs > 0 && b->owner == fp) {
			blob_owner_uncharge(fp, b);
			b->owner = NULL;
			mine = true;
		}
		spin_unlock_irqrestore(&g_blob_lock, fl);
		if (mine)
			drm_property_blob_put(b);
	}
}

/* GETPROPBLOB: the blob's bytes and length.  The copy is made with a
 * reference held, so a blob destroyed meanwhile goes only afterwards.
 * Bytes are copied whenever the caller gave a buffer, as many as fit;
 * the length is always the blob's. */
int drm_mode_getblob_ioctl(struct drm_device *dev, void *kb,
			   struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_get_blob *out_resp = kb;
	struct drm_blob *blob;
	int ret = 0;

	blob = drm_property_lookup_blob(dev, out_resp->blob_id);
	if (!blob)
		return -ENOENT;

	if (out_resp->data && out_resp->length) {
		uint32_t n = out_resp->length < blob->length ? out_resp->length :
							       blob->length;
		if (!validate_user_ptr(out_resp->data, n) ||
		    copy_to_user((void *)(uintptr_t)out_resp->data, blob->data,
				 n) != 0) {
			ret = -EFAULT;
			goto unref;
		}
	}
	out_resp->length = blob->length;
unref:
	drm_property_blob_put(blob);
	return ret;
}

/* A charge made for a blob that was not created after all. */
static void blob_charge_undo(struct drm_file *fp, uint32_t length)
{
#if DRM_PROPERTY_BLOB_LARGE
	uint64_t fl;

	spin_lock_irqsave(&g_blob_lock, &fl);
	fp->blob_bytes = fp->blob_bytes >= length ? fp->blob_bytes - length : 0;
	spin_unlock_irqrestore(&g_blob_lock, fl);
#else
	(void)fp;
	(void)length;
#endif
}

/* CREATEPROPBLOB: a blob of the caller's bytes, owned by the caller's
 * file.  The bytes are copied in before the blob gets an id, so nobody
 * can read a half-filled blob. */
int drm_mode_createblob_ioctl(struct drm_device *dev, void *kb,
			      struct drm_file *fp)
{
	struct drm_mode_create_blob *out_resp = kb;
	struct drm_blob *blob;
	void *data;

	if (out_resp->length == 0 ||
	    out_resp->length > DRM_PROPERTY_BLOB_MAX_LENGTH)
		return -EINVAL;
#if DRM_PROPERTY_BLOB_LARGE
	/* The bytes are charged to the file before they are allocated, so
	 * that two calls racing cannot both pass the bound. */
	{
		uint64_t fl;
		int over;

		spin_lock_irqsave(&g_blob_lock, &fl);
		over = fp->blob_bytes + out_resp->length > DRM_PROPERTY_BLOB_FILE_MAX;
		if (!over)
			fp->blob_bytes += out_resp->length;
		spin_unlock_irqrestore(&g_blob_lock, fl);
		if (over)
			return -ENOSPC;
	}
#endif
	/* A large blob is a run of whole pages (see kalloc); a lookup table
	 * is read in place by the drivers, so it has to be one piece. */
	data = kalloc(out_resp->length);
	if (!data) {
		blob_charge_undo(fp, out_resp->length);
		return -ENOMEM;
	}
	if (!validate_user_ptr(out_resp->data, out_resp->length) ||
	    copy_from_user(data, (void *)(uintptr_t)out_resp->data,
			   out_resp->length) != 0) {
		kfree(data);
		blob_charge_undo(fp, out_resp->length);
		return -EFAULT;
	}
	blob = blob_install(dev, data, out_resp->length, fp);
	if (!blob) {
		blob_charge_undo(fp, out_resp->length);
		return -ENOSPC;
	}
	out_resp->blob_id = blob->id;
	return 0;
}

/* DESTROYPROPBLOB: only the file that created the blob may destroy it.
 * Its reference goes; a state or object still using the blob keeps it. */
int drm_mode_destroyblob_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp)
{
	struct drm_mode_destroy_blob *out_resp = kb;
	struct drm_blob *blob;
	bool found = false;
	uint64_t fl;

	blob = drm_property_lookup_blob(dev, out_resp->blob_id);
	if (!blob)
		return -ENOENT;

	spin_lock_irqsave(&g_blob_lock, &fl);
	/* Ensure the blob was actually created by this file. */
	if (blob->owner == fp) {
		blob_owner_uncharge(fp, blob);
		blob->owner = NULL;
		found = true;
	}
	spin_unlock_irqrestore(&g_blob_lock, fl);

	if (!found) {
		drm_property_blob_put(blob);
		return -EPERM;
	}

	/* One reference from the lookup, and one from the file. */
	drm_property_blob_put(blob);
	drm_property_blob_put(blob);
	return 0;
}

/* ---- value checks -------------------------------------------------------- */

bool drm_property_change_valid_get(struct drm_prop *property, uint64_t value,
				   void **ref)
{
	*ref = NULL;

	if (property->flags & DRM_MODE_PROP_IMMUTABLE)
		return false;

	if (drm_property_type_is(property, DRM_MODE_PROP_RANGE)) {
		if (value < property->values[0] || value > property->values[1])
			return false;
		return true;
	} else if (drm_property_type_is(property, DRM_MODE_PROP_SIGNED_RANGE)) {
		int64_t svalue = (int64_t)value;

		if (svalue < (int64_t)property->values[0] ||
		    svalue > (int64_t)property->values[1])
			return false;
		return true;
	} else if (drm_property_type_is(property, DRM_MODE_PROP_BITMASK)) {
		const struct drm_mode_property_enum *list = drm_property_enums(property);
		uint64_t valid_mask = 0;

		for (uint32_t i = 0; i < property->nenums; i++)
			valid_mask |= (1ULL << list[i].value);
		return !(value & ~valid_mask);
	} else if (drm_property_type_is(property, DRM_MODE_PROP_BLOB)) {
		struct drm_blob *blob;

		if (value == 0)
			return true;
		if (value > 0xffffffffULL || !property->dev)
			return false;

		blob = drm_property_lookup_blob(property->dev, (uint32_t)value);
		if (blob) {
			*ref = blob;
			return true;
		}
		return false;
	} else if (drm_property_type_is(property, DRM_MODE_PROP_OBJECT)) {
		/* a zero value for an object property translates to null: */
		if (value == 0)
			return true;
		if (value > 0xffffffffULL || !property->dev)
			return false;

		/* Objects other than blobs are not reference counted here:
		 * they stay until the file that made them goes, and the
		 * write resolves the id again itself. */
		return drm_mode_object_find(property->dev, (uint32_t)value,
					    (uint32_t)property->values[0],
					    NULL) != NULL;
	}

	/* an enum */
	const struct drm_mode_property_enum *list = drm_property_enums(property);
	for (uint32_t i = 0; i < property->nenums; i++)
		if (list[i].value == value)
			return true;
	return false;
}

void drm_property_change_valid_put(struct drm_prop *property, void *ref)
{
	if (!ref)
		return;

	if (drm_property_type_is(property, DRM_MODE_PROP_BLOB))
		drm_property_blob_put(ref);
}
