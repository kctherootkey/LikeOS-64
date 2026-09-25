// LikeOS -- QUERY: what the device is made of, item by item.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

#define QUERY_MAX_ITEMS 16

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

static long query_topology(struct i915_device *i915, struct drm_i915_query_item *it)
{
	/* header, then: slice mask (1 byte), per-slice subslice masks (1
	 * byte each), per-subslice EU masks (1 byte each) */
	const uint16_t max_slices = 3, max_subslices = 4, max_eus = 8;
	uint8_t buf[sizeof(struct drm_i915_query_topology_info) + 1 + 3 + 3 * 4];
	struct drm_i915_query_topology_info *t = (void *)buf;
	uint8_t *d = buf + sizeof(*t);
	uint32_t len = sizeof(*t) + 1 + max_slices + max_slices * max_subslices;

	mm_memset(buf, 0, sizeof(buf));
	t->flags = 0;
	t->max_slices = max_slices;
	t->max_subslices = max_subslices;
	t->max_eus_per_subslice = max_eus;
	t->subslice_offset = 1;
	t->subslice_stride = 1;
	t->eu_offset = 1 + max_slices;
	t->eu_stride = 1;
	d[0] = (uint8_t)(i915->slice_mask & 0x7);
	for (int s = 0; s < max_slices; s++) {
		d[1 + s] = (uint8_t)(i915->subslice_mask[s] & 0xf);
		for (int ss = 0; ss < max_subslices; ss++) {
			/* per-EU masks are not read from the fuses yet: every
			 * EU of a present subslice counts */
			d[1 + max_slices + s * max_subslices + ss] =
				(i915->subslice_mask[s] & (1u << ss)) ? 0xff : 0;
		}
	}
	return query_reply(it, buf, len);
}

static long query_engines(struct i915_device *i915, struct drm_i915_query_item *it)
{
	uint8_t buf[sizeof(struct drm_i915_query_engine_info) +
		    I915_NUM_ENGINES * sizeof(struct drm_i915_engine_info)];
	struct drm_i915_query_engine_info *q = (void *)buf;
	uint32_t n = 0;

	mm_memset(buf, 0, sizeof(buf));
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		q->engines[n].engine.engine_class = e->class;
		q->engines[n].engine.engine_instance = e->instance;
		q->engines[n].flags = I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE;
		q->engines[n].logical_instance = e->instance;
		/* What a video engine can do beyond the common set: HEVC on
		 * the first video engine from Gen9 and on all of them from
		 * Gen11; the scaler/format converter on every Gen9/10 video
		 * and enhancement engine and on the even-numbered ones from
		 * Gen11. */
		uint64_t caps = 0;
		int gen = i915->info->gen;
		if (e->class == I915_ENGINE_CLASS_VIDEO) {
			if (gen >= 11 || (gen >= 9 && e->instance == 0))
				caps |= I915_VIDEO_CLASS_CAPABILITY_HEVC;
			if (gen >= 9 && (gen < 11 || (e->instance & 1) == 0))
				caps |= I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC;
		} else if (e->class == I915_ENGINE_CLASS_VIDEO_ENHANCE) {
			if (gen >= 9 && (gen < 11 || (e->instance & 1) == 0))
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
		    sizeof(struct drm_i915_memory_region_info)];
	struct drm_i915_query_memory_regions *q = (void *)buf;

	mm_memset(buf, 0, sizeof(buf));
	q->num_regions = 1;
	q->regions[0].region.memory_class = I915_MEMORY_CLASS_SYSTEM;
	q->regions[0].region.memory_instance = 0;
	q->regions[0].probed_size = i915->total_ram_bytes;
	q->regions[0].unallocated_size = (uint64_t)mm_get_free_pages() * 4096;
	q->regions[0].probed_cpu_visible_size = i915->total_ram_bytes;
	q->regions[0].unallocated_cpu_visible_size = q->regions[0].unallocated_size;
	return query_reply(it, buf, sizeof(*q) + sizeof(q->regions[0]));
}

long i915_query_ioctl(struct i915_device *i915, struct drm_file *fp, void *kb)
{
	struct drm_i915_query *a = kb;
	struct drm_i915_query_item items[QUERY_MAX_ITEMS];
	(void)fp;

	if (a->flags)
		return -EINVAL;
	if (a->num_items == 0)
		return 0;
	if (a->num_items > QUERY_MAX_ITEMS)
		return -EINVAL;
	size_t len = a->num_items * sizeof(items[0]);
	if (!a->items_ptr || !validate_user_ptr(a->items_ptr, len))
		return -EFAULT;
	if (copy_from_user(items, (const void *)(uintptr_t)a->items_ptr, len))
		return -EFAULT;
	for (uint32_t i = 0; i < a->num_items; i++) {
		struct drm_i915_query_item *it = &items[i];
		long rc;
		if (it->flags) {
			it->length = -EINVAL;
			continue;
		}
		switch (it->query_id) {
		case DRM_I915_QUERY_TOPOLOGY_INFO:
			rc = query_topology(i915, it);
			break;
		case DRM_I915_QUERY_ENGINE_INFO:
			rc = query_engines(i915, it);
			break;
		case DRM_I915_QUERY_MEMORY_REGIONS:
			rc = query_regions(i915, it);
			break;
		case DRM_I915_QUERY_GUC_SUBMISSION_VERSION:
		case DRM_I915_QUERY_PERF_CONFIG:
			rc = -ENODEV;
			break;
		case DRM_I915_QUERY_HWCONFIG_BLOB:
		case DRM_I915_QUERY_GEOMETRY_SUBSLICES:
		default:
			rc = -EINVAL;
			break;
		}
		if (rc < 0)
			it->length = (int32_t)rc;
	}
	if (copy_to_user((void *)(uintptr_t)a->items_ptr, items, len))
		return -EFAULT;
	return 0;
}
