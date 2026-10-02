// LikeOS -- QUERY: what the device is made of, item by item.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

static unsigned bits_set(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

/* An item: the client either asks the size (length 0) or supplies a
 * buffer of at least that size. */
static long query_reply(struct drm_i915_query_item *it, const void *data, uint32_t len)
{
	if (it->length == 0) {
		it->length = (int32_t)len;
		return 0;
	}
	if (it->length < (int32_t)len)
		return -EINVAL;
	if (!it->data_ptr || !validate_user_ptr(it->data_ptr, len))
		return -EFAULT;
	if (copy_to_user((void *)(uintptr_t)it->data_ptr, data, len))
		return -EFAULT;
	it->length = (int32_t)len;
	return 0;
}

/* Gen11 on: the topology as the hardware is built -- Ice Lake one slice
 * of eight subslices of eight units, Gen12 one slice of six dual
 * subslices of sixteen units, Xe_HP one slice of up to 32 dual
 * subslices -- each mask a byte per eight, the unit mask the same for
 * every subslice present (the fuses give one for all). */
static long query_topology_gen11(struct i915_device *i915, struct drm_i915_query_item *it,
				 uint32_t ss_mask)
{
	uint16_t max_ss = i915->info->gen_x10 >= 125 ? 32 : i915->info->gen_x10 >= 120 ? 6 : 8;
	uint16_t max_eus = i915->info->gen_x10 >= 120 ? 16 : 8;
	uint16_t ss_stride = (uint16_t)((max_ss + 7) / 8), eu_stride = (uint16_t)((max_eus + 7) / 8);
	uint8_t buf[sizeof(struct drm_i915_query_topology_info) + 1 + 4 + 32 * 2];
	struct drm_i915_query_topology_info *t = (void *)buf;
	uint8_t *d = buf + sizeof(*t);
	uint32_t len = sizeof(*t) + 1 + ss_stride + (uint32_t)max_ss * eu_stride;

	mm_memset(buf, 0, sizeof(buf));
	t->max_slices = 1;
	t->max_subslices = max_ss;
	t->max_eus_per_subslice = max_eus;
	t->subslice_offset = 1;
	t->subslice_stride = ss_stride;
	t->eu_offset = (uint16_t)(1 + ss_stride);
	t->eu_stride = eu_stride;
	d[0] = (uint8_t)(i915->slice_mask & 1);
	for (unsigned b = 0; b < ss_stride; b++)
		d[1 + b] = (uint8_t)(ss_mask >> (8 * b));
	for (unsigned ss = 0; ss < max_ss; ss++) {
		if (!(ss_mask & (1u << ss)))
			continue;
		for (unsigned b = 0; b < eu_stride; b++)
			d[1 + ss_stride + ss * eu_stride + b] = (uint8_t)(i915->eu_mask >> (8 * b));
	}
	return query_reply(it, buf, len);
}

static long query_topology(struct i915_device *i915, struct drm_i915_query_item *it)
{
	if (i915->info->gen >= 11)
		return query_topology_gen11(i915, it, i915->subslice_mask[0]);

	/* Before Broadwell only Haswell says: as many slices and subslices
	 * as it has, each with all of its units (the SKU fixes them). */
	if (i915->info->gen < 8) {
		uint16_t ns = (uint16_t)bits_set(i915->slice_mask & 3);
		uint16_t nss = (uint16_t)bits_set(i915->subslice_mask[0] & 3);
		uint16_t ne = (uint16_t)i915->eus_per_subslice, es = (uint16_t)((ne + 7) / 8);
		uint8_t hb[sizeof(struct drm_i915_query_topology_info) + 1 + 2 + 2 * 2 * 2];
		struct drm_i915_query_topology_info *ht = (void *)hb;
		uint8_t *hd = hb + sizeof(*ht);
		if (!ns || !nss || !ne || ne > 16)
			return -ENODEV;
		mm_memset(hb, 0, sizeof(hb));
		ht->max_slices = ns;
		ht->max_subslices = nss;
		ht->max_eus_per_subslice = ne;
		ht->subslice_offset = 1;
		ht->subslice_stride = 1;
		ht->eu_offset = (uint16_t)(1 + ns);
		ht->eu_stride = es;
		hd[0] = (uint8_t)i915->slice_mask;
		for (unsigned sl = 0; sl < ns; sl++) {
			hd[1 + sl] = (uint8_t)i915->subslice_mask[sl];
			for (unsigned ss = 0; ss < nss; ss++)
				for (unsigned b = 0; b < es; b++)
					hd[1 + ns + (sl * nss + ss) * es + b] =
						(uint8_t)((((1u << ne) - 1)) >> (8 * b));
		}
		return query_reply(it, hb, (uint32_t)(sizeof(*ht) + 1 + ns + ns * nss * es));
	}

	/* Gen8/9: the header, then the slice mask (a byte), a byte of
	 * subslices per slice, a byte of units per subslice -- sized by the
	 * most the platform can have: Cherryview one slice of two
	 * subslices, Broadwell three of three, the low-power Gen9 parts one
	 * of three, the others three of four, eight units each. */
	const struct intel_device_info *info = i915->info;
	uint16_t max_slices = 3, max_subslices = 4;
	const uint16_t max_eus = 8;
	if (info->platform == I915_PLATFORM_CHERRYVIEW) {
		max_slices = 1;
		max_subslices = 2;
	} else if (info->gen == 8) {
		max_subslices = 3;
	} else if (info->platform == I915_PLATFORM_BROXTON ||
		   info->platform == I915_PLATFORM_GEMINILAKE) {
		max_slices = 1;
		max_subslices = 3;
	}
	uint8_t buf[sizeof(struct drm_i915_query_topology_info) + 1 + 3 + 3 * 4];
	struct drm_i915_query_topology_info *t = (void *)buf;
	uint8_t *d = buf + sizeof(*t);
	uint32_t len = sizeof(*t) + 1 + max_slices + (uint32_t)max_slices * max_subslices;

	mm_memset(buf, 0, sizeof(buf));
	t->flags = 0;
	t->max_slices = max_slices;
	t->max_subslices = max_subslices;
	t->max_eus_per_subslice = max_eus;
	t->subslice_offset = 1;
	t->subslice_stride = 1;
	t->eu_offset = (uint16_t)(1 + max_slices);
	t->eu_stride = 1;
	d[0] = (uint8_t)(i915->slice_mask & ((1u << max_slices) - 1));
	for (int s = 0; s < max_slices; s++) {
		if (!(d[0] & (1u << s)))
			continue;
		d[1 + s] = (uint8_t)(i915->subslice_mask[s] & ((1u << max_subslices) - 1));
		for (int ss = 0; ss < max_subslices; ss++)
			if (d[1 + s] & (1u << ss))
				d[1 + max_slices + s * max_subslices + ss] = i915->gen8_eu_mask[s][ss];
	}
	return query_reply(it, buf, len);
}

/* The engines as a client names them: class and instance in the user
 * numbering (dense per class), the logical instance, and what a video
 * or enhancement engine can do beyond the common set.  A buffer the
 * client hands in starts with a zeroed header. */
static long query_engines(struct i915_device *i915, struct drm_i915_query_item *it)
{
	uint8_t buf[sizeof(struct drm_i915_query_engine_info) +
		    I915_NUM_ENGINES * sizeof(struct drm_i915_engine_info)];
	struct drm_i915_query_engine_info *q = (void *)buf;
	uint32_t n = 0;

	if (it->flags)
		return -EINVAL;
	mm_memset(buf, 0, sizeof(buf));
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		n += i915->engines[i].present ? 1 : 0;
	if (it->length) {
		struct drm_i915_query_engine_info hdr;
		uint32_t len = (uint32_t)(sizeof(*q) + n * sizeof(q->engines[0]));
		if (it->length < (int32_t)len)
			return -EINVAL;
		if (!it->data_ptr || !validate_user_ptr(it->data_ptr, sizeof(hdr)) ||
		    copy_from_user(&hdr, (const void *)(uintptr_t)it->data_ptr, sizeof(hdr)))
			return -EFAULT;
		if (hdr.num_engines || hdr.rsvd[0] || hdr.rsvd[1] || hdr.rsvd[2])
			return -EINVAL;
	}
	n = 0;
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		q->engines[n].engine.engine_class = e->class;
		q->engines[n].engine.engine_instance = (uint16_t)i915_engine_uabi_instance(e);
		q->engines[n].flags = I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE;
		q->engines[n].logical_instance = (uint16_t)i915_engine_logical_instance(e);
		/* HEVC: on the first video engine from Gen9, on all of them
		 * from Gen11.  The scaler/format converter: on the first video
		 * engine from Gen9 and, from Gen11, on those the media fuses
		 * hook one to; on an enhancement engine from Gen11 where its
		 * scaler is there (from Xe_HP media the fuses say). */
		uint64_t caps = 0;
		int gen = i915->info->gen;
		if (e->class == I915_ENGINE_CLASS_VIDEO) {
			if (gen >= 11 || (gen >= 9 && e->instance == 0))
				caps |= I915_VIDEO_CLASS_CAPABILITY_HEVC;
			if ((gen >= 11 && (i915->vdbox_sfc_access & (1u << e->instance))) ||
			    (gen >= 9 && e->instance == 0))
				caps |= I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC;
		} else if (e->class == I915_ENGINE_CLASS_VIDEO_ENHANCE) {
			if (gen >= 9 && i915->media_ip >= I915_IP(11, 0) &&
			    (i915->sfc_mask & (1u << e->instance)))
				caps |= I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC;
		}
		q->engines[n].capabilities = caps;
		n++;
	}
	q->num_engines = n;
	return query_reply(it, buf, sizeof(*q) + n * sizeof(q->engines[0]));
}

static long query_regions(struct i915_device *i915, struct drm_i915_query_item *it)
{
	uint8_t buf[sizeof(struct drm_i915_query_memory_regions) +
		    2 * sizeof(struct drm_i915_memory_region_info)];
	struct drm_i915_query_memory_regions *q = (void *)buf;

	if (it->flags)
		return -EINVAL;
	mm_memset(buf, 0, sizeof(buf));
	q->num_regions = (uint32_t)i915_lmem_query_regions(i915, q->regions, 2);
	uint32_t len = (uint32_t)(sizeof(*q) + q->num_regions * sizeof(q->regions[0]));
	/* a buffer the client hands in starts with a zeroed header */
	if (it->length && it->length >= (int32_t)len) {
		struct drm_i915_query_memory_regions hdr;
		if (!it->data_ptr || !validate_user_ptr(it->data_ptr, sizeof(hdr)) ||
		    copy_from_user(&hdr, (const void *)(uintptr_t)it->data_ptr, sizeof(hdr)))
			return -EFAULT;
		if (hdr.num_regions || hdr.rsvd[0] || hdr.rsvd[1] || hdr.rsvd[2])
			return -EINVAL;
	}
	return query_reply(it, buf, len);
}

/* The GuC's hardware configuration table, as the firmware gave it: only
 * where the GuC has one and it was read. */
static long query_hwconfig(struct i915_device *i915, struct drm_i915_query_item *it)
{
	if (!i915->hwconfig || !i915->hwconfig_size)
		return -ENODEV;
	return query_reply(it, i915->hwconfig, i915->hwconfig_size);
}

/* The GuC submission interface's version: only with GuC submission; the
 * item must come in zeroed, and the answer leaves the length at 0. */
static long query_guc_version(struct i915_device *i915, struct drm_i915_query_item *it)
{
	struct drm_i915_query_guc_submission_version v;

	if (!i915->guc.submission)
		return -ENODEV;
	if (it->length == 0) {
		it->length = (int32_t)sizeof(v);
		return 0;
	}
	if (it->length < (int32_t)sizeof(v))
		return -EINVAL;
	if (!it->data_ptr || !validate_user_ptr(it->data_ptr, sizeof(v)))
		return -EFAULT;
	if (copy_from_user(&v, (const void *)(uintptr_t)it->data_ptr, sizeof(v)))
		return -EFAULT;
	if (v.branch || v.major || v.minor || v.patch)
		return -EINVAL;
	v.major = (i915->guc.submit_version >> 16) & 0xff;
	v.minor = (i915->guc.submit_version >> 8) & 0xff;
	v.patch = i915->guc.submit_version & 0xff;
	if (copy_to_user((void *)(uintptr_t)it->data_ptr, &v, sizeof(v)))
		return -EFAULT;
	it->length = 0;
	return 0;
}

long i915_query_ioctl(struct i915_device *i915, struct drm_file *fp, void *kb)
{
	struct drm_i915_query *a = kb;
	(void)fp;

	if (a->flags)
		return -EINVAL;
	/* Item by item, each answered (its length, or what went wrong) in
	 * place before the next is read; an item naming no query at all
	 * ends the call. */
	for (uint32_t i = 0; i < a->num_items; i++) {
		struct drm_i915_query_item item, *it = &item;
		uint64_t uptr = a->items_ptr + (uint64_t)i * sizeof(item);
		long rc;
		if (!a->items_ptr || !validate_user_ptr(uptr, sizeof(item)))
			return -EFAULT;
		if (copy_from_user(&item, (const void *)(uintptr_t)uptr, sizeof(item)))
			return -EFAULT;
		if (item.query_id == 0)
			return -EINVAL;
		/* The flags: the engine for the geometry query, the operation
		 * for the observation configurations, nothing for the
		 * topology, the engines and the memory regions; the
		 * configuration table and the submission version look at
		 * none. */
		switch (it->query_id) {
		case DRM_I915_QUERY_GEOMETRY_SUBSLICES: {
			/* Xe_HP on: the dual subslices the render engine's
			 * geometry pipeline has */
			if (i915->gt_ip < I915_IP(12, 55)) {
				rc = -ENODEV;
				break;
			}
			struct i915_engine *e = i915_engine_lookup_user(
				i915, (int)(it->flags & 0xffff), (int)((it->flags >> 16) & 0xffff));
			if (!e || e->class != I915_ENGINE_CLASS_RENDER) {
				rc = -EINVAL;
				break;
			}
			rc = query_topology_gen11(i915, it, i915->dss_geometry);
			break;
		}
		case DRM_I915_QUERY_TOPOLOGY_INFO:
			rc = it->flags ? -EINVAL : query_topology(i915, it);
			break;
		case DRM_I915_QUERY_ENGINE_INFO:
			rc = query_engines(i915, it);
			break;
		case DRM_I915_QUERY_MEMORY_REGIONS:
			rc = query_regions(i915, it);
			break;
		case DRM_I915_QUERY_GUC_SUBMISSION_VERSION:
			rc = query_guc_version(i915, it);
			break;
		case DRM_I915_QUERY_HWCONFIG_BLOB:
			rc = query_hwconfig(i915, it);
			break;
		case DRM_I915_QUERY_PERF_CONFIG:
			/* the three operations exist; there are no
			 * configurations to list or read */
			rc = (it->flags >= DRM_I915_QUERY_PERF_CONFIG_LIST &&
			      it->flags <= DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_ID) ? -ENODEV : -EINVAL;
			break;
		default:
			rc = -EINVAL;
			break;
		}
		if (rc < 0)
			it->length = (int32_t)rc;
		if (copy_to_user((void *)(uintptr_t)(uptr + __builtin_offsetof(struct drm_i915_query_item, length)),
				 &it->length, sizeof(it->length)))
			return -EFAULT;
	}
	return 0;
}
