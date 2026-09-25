// LikeOS -- GEM objects for Intel graphics: creation, mapping, state.
//
// Objects are the DRM core's (pages, handles, sharing); this file keeps
// what the hardware cares about -- tiling, cache attribute, where the
// object is bound -- and answers the object ioctls of the i915 interface.
// Coherency on parts with a shared last-level cache is by the cache;
// objects the client marks uncached (and every part without one) get
// their lines flushed at the domain transitions.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ---- per-file state ------------------------------------------------------- */

int i915_gem_open(struct drm_device *dev, struct drm_file *fp)
{
	struct i915_device *i915 = to_i915(dev);
	struct i915_file *f = kalloc(sizeof(*f));

	if (!f)
		return -ENOMEM;
	mm_memset(f, 0, sizeof(*f));
	spinlock_init(&f->lock, "i915_file");
	fp->priv = f;
	if (i915->gt_ready) {
		f->ctx[0] = i915_context_create(i915, fp, NULL);
		if (!f->ctx[0]) {
			kfree(f);
			fp->priv = NULL;
			return -ENOMEM;
		}
		f->ctx[0]->id = 0;
	}
	return 0;
}

void i915_gem_postclose(struct drm_device *dev, struct drm_file *fp)
{
	struct i915_file *f = fp->priv;
	(void)dev;
	if (!f)
		return;
	for (int i = 0; i < I915_MAX_CONTEXTS; i++) {
		if (f->ctx[i]) {
			f->ctx[i]->closed = 1;
			i915_context_put(f->ctx[i]);
			f->ctx[i] = NULL;
		}
	}
	for (int i = 0; i < I915_MAX_VMS; i++) {
		if (f->vm[i]) {
			i915_vm_put(f->vm[i]);
			f->vm[i] = NULL;
		}
	}
	kfree(f);
	fp->priv = NULL;
}

struct i915_gem_context *i915_file_context(struct drm_file *fp, uint32_t id)
{
	struct i915_file *f = fp->priv;
	struct i915_gem_context *ctx = NULL;
	uint64_t fl;

	if (!f || id >= I915_MAX_CONTEXTS)
		return NULL;
	spin_lock_irqsave(&f->lock, &fl);
	ctx = f->ctx[id];
	if (ctx)
		i915_context_get(ctx);
	spin_unlock_irqrestore(&f->lock, fl);
	return ctx;
}

/* ---- objects ------------------------------------------------------------------ */

void i915_gem_object_free(struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (!bo)
		return;
	while (bo->vmas) {
		struct i915_vma *v = bo->vmas;
		bo->vmas = v->next;
		/* the translations are cleared only if they are still this
		 * object's: another object may own the range by now */
		if (i915_vm_vma_release(v->vm, v))
			i915_vm_unbind(v->vm, v->addr, v->npages);
		i915_vm_put(v->vm);
		kfree(v);
	}
	i915_fence_object_free(&g_i915, o);
	if (bo->bound)
		i915_ggtt_unbind(&g_i915, bo->ggtt, (uint32_t)(o->npages * 4096));
	{
		/* out from under the lock first: a submission may be reading
		 * them; dropped after, since dropping may free */
		struct drm_fence *olds[1 + I915_ENGINE_CLASSES];
		unsigned no = 0;
		uint64_t fl;
		spin_lock_irqsave(&g_i915.sync_lock, &fl);
		if (bo->write_fence)
			olds[no++] = bo->write_fence;
		bo->write_fence = NULL;
		for (int c = 0; c < I915_ENGINE_CLASSES; c++) {
			if (bo->read_fence[c])
				olds[no++] = bo->read_fence[c];
			bo->read_fence[c] = NULL;
		}
		spin_unlock_irqrestore(&g_i915.sync_lock, fl);
		for (unsigned i = 0; i < no; i++)
			drm_fence_put(olds[i]);
	}
	kfree(bo);
	o->priv = NULL;
}

void i915_gem_object_fences(struct drm_gem_object *o, int write, uint64_t skip_context,
			    struct drm_fence **out, unsigned *n)
{
	struct i915_bo *bo = o->priv;
	struct i915_device *i915 = &g_i915;
	uint64_t fl;
	if (!bo)
		return;
	spin_lock_irqsave(&i915->sync_lock, &fl);
	struct drm_fence *f = bo->write_fence;
	if (f && !f->signaled && (!skip_context || f->context != skip_context)) {
		drm_fence_get(f);
		out[(*n)++] = f;
	}
	for (int c = 0; write && c < I915_ENGINE_CLASSES; c++) {
		f = bo->read_fence[c];
		if (f && !f->signaled && (!skip_context || f->context != skip_context)) {
			drm_fence_get(f);
			out[(*n)++] = f;
		}
	}
	spin_unlock_irqrestore(&i915->sync_lock, fl);
}

int i915_gem_object_wait(struct drm_gem_object *o, int write, uint64_t timeout_ns)
{
	struct drm_fence *waits[1 + I915_ENGINE_CLASSES];
	unsigned nw = 0;
	int rc = 0;
	i915_gem_object_fences(o, write, 0, waits, &nw);
	for (unsigned i = 0; i < nw; i++) {
		if (rc == 0)
			rc = drm_fence_wait_flags(waits[i], timeout_ns, 1);
		drm_fence_put(waits[i]);
	}
	if (rc == -ETIMEDOUT || rc == -ETIME) {
		static int said;
		task_t *cur = sched_current();
		if (said < 8) {
			said++;
			kprintf("[drm] i915: process %d (%s) waited %u s for an object the engine never released\n",
				cur ? (int)cur->tgid : -1, cur ? cur->comm : "?",
				(unsigned)(timeout_ns / 1000000000ULL));
		}
	}
	return rc;
}

static int bo_is_coherent(struct i915_device *i915, struct i915_bo *bo)
{
	if (!(i915->info->flags & I915_INFO_HAS_LLC))
		return 0;
	return bo->caching != I915_CACHING_NONE;
}

static void bo_clflush(struct drm_gem_object *o)
{
	if (!o->pages)
		return;
	for (uint32_t p = 0; p < o->npages; p++) {
		uint8_t *va = phys_to_virt(o->pages[p]);
		for (int i = 0; i < 4096; i += 64)
			__asm__ volatile("clflush (%0)" ::"r"(va + i) : "memory");
	}
	__asm__ volatile("mfence" ::: "memory");
}

/* An object about to be scanned out.
 *
 * The display engine of these parts does not look in the processor's
 * last-level cache; it reads memory.  The graphics driver above marks a
 * shared or scanned-out surface "cacheability from the page table", so
 * whether the engine's writes to it land in that cache or in memory is
 * decided HERE.  Left cacheable, the rendering sits in the cache, the
 * display reads stale memory underneath it, and the picture is whatever
 * happened to have been written back -- which changes as the cache
 * fills, so the screen flickers between the real image and older
 * contents.
 *
 * So a surface that goes to the display becomes uncached: its pages are
 * flushed out of the cache once, and every address space that has it
 * mapped is rewritten so later writes go straight to memory. */
void i915_gem_object_set_display(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;

	if (!bo || bo->caching == I915_CACHING_NONE)
		return;
	bo->caching = I915_CACHING_NONE;
	bo_clflush(o);
	for (struct i915_vma *v = bo->vmas; v; v = v->next) {
		if (!v->vm || !v->attached)
			continue;
		if (i915_vm_bind(v->vm, v->addr, o->pages, o->npages, 1) != 0)
			kprintf("[drm] i915: could not remap a scanout buffer uncached at %llx\n",
				(unsigned long long)v->addr);
		v->vm->tlb_dirty = 1;
	}
	(void)i915;
}

/* Before the GPU reads an object the CPU wrote through the cache. */
void i915_gem_object_flush_for_gpu(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (!bo)
		return;
	if (!bo_is_coherent(i915, bo) && (bo->write_domain & I915_GEM_DOMAIN_CPU)) {
		bo_clflush(o);
		bo->write_domain = 0;
	}
}

static struct drm_gem_object *gem_create(struct i915_device *i915, struct drm_file *fp,
					 uint64_t size, uint32_t *handle)
{
	if (size == 0 || size > (64ULL << 30))
		return NULL;
	uint64_t rounded = (size + 4095) & ~4095ULL;
	struct drm_gem_object *o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, rounded);
	if (!o)
		return NULL;
	if (drm_gem_alloc_pages(o) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	if (i915->drm.drv->gem_init && i915->drm.drv->gem_init(o) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	struct i915_bo *bo = o->priv;
	bo->caching = (i915->info->flags & I915_INFO_HAS_LLC) ? I915_CACHING_CACHED :
								  I915_CACHING_NONE;
	bo->madv = I915_MADV_WILLNEED;
	bo->user_size = size;
	if (drm_gem_handle_create(fp, o, handle) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	return o;
}

/* ---- objects over a client's own pages (USERPTR) --------------------------- */
/*
 * A client hands over a page-aligned range of its own address space and
 * gets an object whose pages are those pages.  Each is faulted in and
 * written to once (which resolves a copy-on-write sharing, so the
 * device and the client see the same page afterwards), then referenced
 * so the memory manager keeps it while the object lives.  What the
 * client does with the mapping later -- unmapping it, forking and
 * writing -- the device does not follow: the object keeps the pages it
 * was given, as the interface has always promised only for a range that
 * stays mapped and unshared.
 */
static void userptr_release(uint64_t *pages, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (pages[i])
			mm_put_page(pages[i]);
}

static long gem_userptr(struct i915_device *i915, struct drm_file *fp, void *kb)
{
	struct drm_i915_gem_userptr *a = kb;
	task_t *cur = sched_current();

	if (a->flags & ~(I915_USERPTR_READ_ONLY | I915_USERPTR_PROBE | I915_USERPTR_UNSYNCHRONIZED))
		return -EINVAL;
	if (a->flags & (I915_USERPTR_UNSYNCHRONIZED | I915_USERPTR_READ_ONLY))
		return -ENODEV; /* no read-only device mappings here */
	if (!a->user_size || (a->user_ptr & 4095) || (a->user_size & 4095))
		return -EINVAL;
	if (a->user_size > (16ULL << 30))
		return -E2BIG;
	if (!cur || !cur->pml4 || !validate_user_ptr(a->user_ptr, a->user_size))
		return -EFAULT;
	uint32_t npages = (uint32_t)(a->user_size / 4096);
	uint64_t *pages = kalloc((size_t)npages * sizeof(uint64_t));
	if (!pages)
		return -ENOMEM;
	mm_memset(pages, 0, (size_t)npages * sizeof(uint64_t));
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t va = a->user_ptr + (uint64_t)i * 4096;
		uint8_t byte;
		/* fault the page in, and make it the client's own */
		if (copy_from_user(&byte, (const void *)(uintptr_t)va, 1) != 0 ||
		    copy_to_user((void *)(uintptr_t)va, &byte, 1) != 0) {
			userptr_release(pages, i);
			kfree(pages);
			return -EFAULT;
		}
		uint64_t phys = mm_get_physical_address_from_pml4(cur->pml4, va);
		if (!phys) {
			userptr_release(pages, i);
			kfree(pages);
			return -EFAULT;
		}
		mm_get_page(phys);
		pages[i] = phys;
	}
	struct drm_gem_object *o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, a->user_size);
	if (!o) {
		userptr_release(pages, npages);
		kfree(pages);
		return -ENOMEM;
	}
	o->pages = pages;
	o->pages_borrowed = 1; /* before anything can release them */
	if (i915->drm.drv->gem_init && i915->drm.drv->gem_init(o) != 0) {
		drm_gem_put(o); /* releases the pages through gem_release_pages */
		return -ENOMEM;
	}
	struct i915_bo *bo = o->priv;
	bo->userptr = 1;
	bo->caching = (i915->info->flags & I915_INFO_HAS_LLC) ? I915_CACHING_CACHED :
								  I915_CACHING_NONE;
	bo->madv = I915_MADV_WILLNEED;
	bo->user_size = a->user_size;
	if (drm_gem_handle_create(fp, o, &a->handle) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	drm_gem_put(o); /* the handle holds it now */
	return 0;
}

/* Called by the core once the driver's per-object state is gone, so
 * whether the pages were borrowed (a client's own, referenced) or
 * allocated is read from the object, never from that state. */
void i915_gem_release_pages(struct drm_gem_object *o)
{
	if (!o->pages)
		return;
	if (o->pages_borrowed) {
		userptr_release(o->pages, o->npages);
		return;
	}
	for (uint32_t i = 0; i < o->npages; i++)
		if (o->pages[i])
			mm_free_physical_page(o->pages[i]);
}

static int copy_user_bounded(void *dst, uint64_t uptr, size_t len)
{
	if (!uptr || !validate_user_ptr(uptr, len))
		return -EFAULT;
	return copy_from_user(dst, (const void *)(uintptr_t)uptr, len) ? -EFAULT : 0;
}

/* ---- the ioctls ------------------------------------------------------------ */

static long gem_create_ext(struct i915_device *i915, struct drm_file *fp,
			   struct drm_i915_gem_create_ext *a)
{
	uint32_t pat = 0;
	int have_pat = 0;
	uint64_t next = a->extensions;

	if (a->flags & ~I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS)
		return -EINVAL;
	for (int n = 0; next && n < 8; n++) {
		struct i915_user_extension ext;
		if (copy_user_bounded(&ext, next, sizeof(ext)))
			return -EFAULT;
		switch (ext.name) {
		case I915_GEM_CREATE_EXT_MEMORY_REGIONS: {
			struct drm_i915_gem_create_ext_memory_regions mr;
			if (copy_user_bounded(&mr, next, sizeof(mr)))
				return -EFAULT;
			if (mr.num_regions == 0 || mr.num_regions > 4)
				return -EINVAL;
			struct drm_i915_gem_memory_class_instance r[4];
			if (copy_user_bounded(r, mr.regions, mr.num_regions * sizeof(r[0])))
				return -EFAULT;
			int sys = 0;
			for (uint32_t i = 0; i < mr.num_regions; i++)
				if (r[i].memory_class == I915_MEMORY_CLASS_SYSTEM &&
				    r[i].memory_instance == 0)
					sys = 1;
			if (!sys)
				return -EINVAL;
			break;
		}
		case I915_GEM_CREATE_EXT_SET_PAT: {
			struct drm_i915_gem_create_ext_set_pat sp;
			if (copy_user_bounded(&sp, next, sizeof(sp)))
				return -EFAULT;
			pat = sp.pat_index;
			have_pat = 1;
			break;
		}
		case I915_GEM_CREATE_EXT_PROTECTED_CONTENT:
			return -ENODEV;
		default:
			return -EINVAL;
		}
		next = ext.next_extension;
	}
	uint32_t handle;
	struct drm_gem_object *o = gem_create(i915, fp, a->size, &handle);
	if (!o)
		return -ENOMEM;
	struct i915_bo *bo = o->priv;
	if (have_pat) {
		bo->pat_index = pat;
		/* index 3 is uncached in the tables the driver programs */
		bo->caching = (pat == 3) ? I915_CACHING_NONE : I915_CACHING_CACHED;
	}
	a->handle = handle;
	a->size = o->size;
	drm_gem_put(o); /* the handle holds it */
	return 0;
}

static long gem_mmap_offset(struct i915_device *i915, struct drm_file *fp,
			    struct drm_i915_gem_mmap_offset *a)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	unsigned kind;
	if (a->extensions) {
		drm_gem_put(o);
		return -EINVAL;
	}
	switch (a->flags) {
	case I915_MMAP_OFFSET_WB:
		kind = 2;
		break;
	case I915_MMAP_OFFSET_UC:
		kind = 3;
		break;
	case I915_MMAP_OFFSET_FIXED:
		kind = (bo && bo->caching != I915_CACHING_NONE &&
			(i915->info->flags & I915_INFO_HAS_LLC)) ? 2 : 1;
		break;
	case I915_MMAP_OFFSET_GTT:
		/* through the aperture, detiled by a fence, where the device
		 * has one; otherwise the pages as they are */
		kind = i915->bar_aperture.size ? I915_MMAP_KIND_GTT : 1;
		break;
	case I915_MMAP_OFFSET_WC:
	default:
		kind = 1; /* write-combining through system memory */
		break;
	}
	a->offset = drm_gem_mmap_offset_kind(o, kind);
	drm_gem_put(o);
	return 0;
}

static long gem_set_domain(struct i915_device *i915, struct drm_file *fp,
			   struct drm_i915_gem_set_domain *a)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	int write = !!a->write_domain;
	/* the GPU must be done with it */
	int rc = i915_gem_object_wait(o, write, 10ULL * 1000000000ULL);
	if (rc == 0 && bo) {
		if (!bo_is_coherent(i915, bo) && (a->read_domains & I915_GEM_DOMAIN_CPU))
			bo_clflush(o); /* drop stale lines before the CPU reads */
		/* the aperture reads memory: what the processor cached must
		 * be there first */
		if (!bo_is_coherent(i915, bo) && (a->read_domains & I915_GEM_DOMAIN_GTT))
			bo_clflush(o);
		bo->read_domains = a->read_domains;
		bo->write_domain = a->write_domain;
	}
	drm_gem_put(o);
	return rc == -ERESTARTSYS ? -ERESTARTSYS : (rc ? -EIO : 0);
}

static long gem_tiling(struct i915_device *i915, struct drm_file *fp, void *kb, int set)
{
	(void)i915;
	if (set) {
		struct drm_i915_gem_set_tiling *a = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
		if (!o)
			return -ENOENT;
		struct i915_bo *bo = o->priv;
		if (a->tiling_mode > I915_TILING_Y) {
			drm_gem_put(o);
			return -EINVAL;
		}
		/* a tiled stride is whole tiles: X tiles are 512 bytes wide,
		 * Y tiles 128 */
		uint32_t tw = a->tiling_mode == I915_TILING_X ? 512 :
			      a->tiling_mode == I915_TILING_Y ? 128 : 1;
		if (a->tiling_mode != I915_TILING_NONE &&
		    (a->stride == 0 || (a->stride % tw))) {
			drm_gem_put(o);
			return -EINVAL;
		}
		bo->tiling = a->tiling_mode;
		bo->stride = a->tiling_mode == I915_TILING_NONE ? 0 : a->stride;
		a->stride = bo->stride;
		a->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		drm_gem_put(o);
		return 0;
	}
	struct drm_i915_gem_get_tiling *a = kb;
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	a->tiling_mode = bo->tiling;
	a->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
	a->phys_swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
	drm_gem_put(o);
	return 0;
}

static long gem_caching(struct i915_device *i915, struct drm_file *fp,
			struct drm_i915_gem_caching *a, int set)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	if (set) {
		if (a->caching > I915_CACHING_DISPLAY) {
			drm_gem_put(o);
			return -EINVAL;
		}
		if (!(i915->info->flags & I915_INFO_HAS_LLC) && a->caching == I915_CACHING_CACHED) {
			drm_gem_put(o);
			return -ENODEV;
		}
		if (bo->caching != a->caching) {
			/* the bindings change attribute: rebind each */
			bo->caching = a->caching;
			int unc = (a->caching == I915_CACHING_NONE);
			for (struct i915_vma *v = bo->vmas; v; v = v->next)
				if (v->attached)
					i915_vm_bind(v->vm, v->addr, o->pages, v->npages, unc);
			bo_clflush(o);
		}
	} else {
		a->caching = bo->caching;
	}
	drm_gem_put(o);
	return 0;
}

static long gem_madvise(struct drm_file *fp, struct drm_i915_gem_madvise *a)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	if (a->madv != I915_MADV_WILLNEED && a->madv != I915_MADV_DONTNEED) {
		drm_gem_put(o);
		return -EINVAL;
	}
	/* Purging under pressure is not done yet: the contents are always
	 * retained, which is a valid answer. */
	bo->madv = a->madv;
	a->retained = !bo->purged;
	drm_gem_put(o);
	return 0;
}

static long gem_busy(struct drm_file *fp, struct drm_i915_gem_busy *a)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	uint32_t busy = 0;
	uint64_t fl;
	/* The interface's encoding: the low half names the class of the
	 * engine writing the object, plus one (0 = no writer); the high half
	 * has one bit per engine class with a reader, the writer among them. */
	spin_lock_irqsave(&g_i915.sync_lock, &fl);
	if (bo && bo->write_fence && !bo->write_fence->signaled)
		busy |= ((uint32_t)bo->write_class + 1) | (0x10000u << bo->write_class);
	for (int c = 0; bo && c < I915_ENGINE_CLASSES; c++)
		if (bo->read_fence[c] && !bo->read_fence[c]->signaled)
			busy |= 0x10000u << c;
	spin_unlock_irqrestore(&g_i915.sync_lock, fl);
	a->busy = busy;
	drm_gem_put(o);
	return 0;
}

/* The legacy mapping call: the kernel maps the object into the caller
 * and answers with the address, write-back or write-combining.  The
 * mapping is the same one mmap(2) of the node's offset would make; only
 * the way it is asked for differs.  A client of the older buffer-manager
 * library maps every buffer this way. */
static long gem_mmap_legacy(struct drm_file *fp, struct drm_i915_gem_mmap *a,
			    unsigned size)
{
	uint64_t flags = size >= sizeof(*a) ? a->flags : 0;
	if (flags & ~(uint64_t)I915_MMAP_WC)
		return -EINVAL;
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	if (a->size == 0 || (a->offset & 0xfff) || a->offset + a->size < a->offset ||
	    a->offset + a->size > o->size || a->offset >= (1ULL << 32)) {
		drm_gem_put(o);
		return -EINVAL;
	}
	/* an object over a client's own pages is mapped by the client already */
	if (bo && bo->userptr) {
		drm_gem_put(o);
		return -EINVAL;
	}
	unsigned kind = (flags & I915_MMAP_WC) ? 1 : 2;
	uint64_t off = drm_gem_mmap_offset_kind(o, kind) + a->offset;
	drm_gem_put(o);
	if (!fp->vfs)
		return -ENODEV;
	int64_t va = mm_mmap_file(fp->vfs, a->size, PROT_READ | PROT_WRITE, MAP_SHARED, off);
	if (va < 0)
		return (long)va;
	a->addr_ptr = (uint64_t)va;
	return 0;
}

static long gem_wait(struct drm_file *fp, struct drm_i915_gem_wait *a)
{
	struct drm_gem_object *o = drm_gem_lookup(fp, a->bo_handle);
	if (!o)
		return -ENOENT;
	if (a->flags) {
		drm_gem_put(o);
		return -EINVAL;
	}
	uint64_t t = a->timeout_ns < 0 ? 3600ULL * 1000000000ULL : (uint64_t)a->timeout_ns;
	int rc = i915_gem_object_wait(o, 1, t ? t : 1);
	drm_gem_put(o);
	if (rc == -ETIMEDOUT)
		return -ETIME;
	return rc;
}

static long gem_pread_pwrite(struct i915_device *i915, struct drm_file *fp, void *kb,
			     int write)
{
	struct drm_i915_gem_pwrite *a = kb; /* pread has the same layout */
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	if (a->offset > o->size || a->size > o->size - a->offset) {
		drm_gem_put(o);
		return -EINVAL;
	}
	if (a->size && (!a->data_ptr || !validate_user_ptr(a->data_ptr, a->size))) {
		drm_gem_put(o);
		return -EFAULT;
	}
	int rc = i915_gem_object_wait(o, write, 10ULL * 1000000000ULL);
	if (rc) {
		drm_gem_put(o);
		return -EIO;
	}
	/* An object the engines write past the shared cache may still be
	 * in the processor's caches from the last time it was read: drop
	 * those lines first, or the read returns what the engine
	 * overwrote. */
	if (!write && o->priv && !bo_is_coherent(i915, o->priv))
		bo_clflush(o);
	uint64_t done = 0;
	while (done < a->size) {
		uint64_t off = a->offset + done;
		uint32_t page = (uint32_t)(off / 4096);
		uint32_t in = (uint32_t)(off & 4095);
		uint32_t chunk = 4096 - in;
		if (chunk > a->size - done)
			chunk = (uint32_t)(a->size - done);
		uint8_t *va = (uint8_t *)phys_to_virt(o->pages[page]) + in;
		if (write) {
			if (copy_from_user(va, (const void *)(uintptr_t)(a->data_ptr + done), chunk)) {
				rc = -EFAULT;
				break;
			}
		} else {
			if (copy_to_user((void *)(uintptr_t)(a->data_ptr + done), va, chunk)) {
				rc = -EFAULT;
				break;
			}
		}
		done += chunk;
	}
	if (write)
		bo_clflush(o);
	drm_gem_put(o);
	return rc;
}

long i915_gem_ioctl(struct i915_device *i915, struct drm_file *fp, unsigned nr,
		    void *kb, unsigned size, int *handled)
{
	*handled = 1;
	switch (nr) {
	case DRM_I915_GEM_CREATE: {
		struct drm_i915_gem_create *a = kb;
		uint32_t handle;
		if (size < sizeof(*a))
			return -EINVAL;
		struct drm_gem_object *o = gem_create(i915, fp, a->size, &handle);
		if (!o)
			return -ENOMEM;
		a->handle = handle;
		a->size = o->size;
		drm_gem_put(o);
		return 0;
	}
	case DRM_I915_GEM_CREATE_EXT:
		if (size < sizeof(struct drm_i915_gem_create_ext))
			return -EINVAL;
		return gem_create_ext(i915, fp, kb);
	case DRM_I915_GEM_MMAP:
		/* the older form, without the flags word, is 32 bytes */
		if (size < 32)
			return -EINVAL;
		return gem_mmap_legacy(fp, kb, size);
	case DRM_I915_GEM_MMAP_GTT:
		if (size >= sizeof(struct drm_i915_gem_mmap_offset))
			return gem_mmap_offset(i915, fp, kb);
		{
			struct drm_i915_gem_mmap_gtt *a = kb;
			struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
			if (!o)
				return -ENOENT;
			a->offset = drm_gem_mmap_offset_kind(o, i915->bar_aperture.size ? I915_MMAP_KIND_GTT : 1);
			drm_gem_put(o);
			return 0;
		}
	case DRM_I915_GEM_SET_DOMAIN:
		return gem_set_domain(i915, fp, kb);
	case DRM_I915_GEM_SW_FINISH: {
		struct drm_i915_gem_sw_finish *a = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
		if (!o)
			return -ENOENT;
		struct i915_bo *bo = o->priv;
		if (!bo_is_coherent(i915, bo))
			bo_clflush(o);
		drm_gem_put(o);
		return 0;
	}
	case DRM_I915_GEM_SET_TILING:
		return gem_tiling(i915, fp, kb, 1);
	case DRM_I915_GEM_GET_TILING:
		return gem_tiling(i915, fp, kb, 0);
	case DRM_I915_GEM_SET_CACHING:
		return gem_caching(i915, fp, kb, 1);
	case DRM_I915_GEM_GET_CACHING:
		return gem_caching(i915, fp, kb, 0);
	case DRM_I915_GEM_MADVISE:
		return gem_madvise(fp, kb);
	case DRM_I915_GEM_BUSY:
		return gem_busy(fp, kb);
	case DRM_I915_GEM_WAIT:
		return gem_wait(fp, kb);
	case DRM_I915_GEM_THROTTLE:
		return 0;
	case DRM_I915_GEM_GET_APERTURE: {
		struct drm_i915_gem_get_aperture *a = kb;
		a->aper_size = i915->ggtt_bytes;
		a->aper_available_size = i915->ggtt_bytes - (256u << 20);
		return 0;
	}
	case DRM_I915_GEM_PREAD:
		return gem_pread_pwrite(i915, fp, kb, 0);
	case DRM_I915_GEM_PWRITE:
		return gem_pread_pwrite(i915, fp, kb, 1);
	case DRM_I915_GEM_USERPTR:
		if (size < sizeof(struct drm_i915_gem_userptr))
			return -EINVAL;
		return gem_userptr(i915, fp, kb);
	case DRM_I915_GEM_PIN:
	case DRM_I915_GEM_UNPIN:
	case DRM_I915_GEM_INIT:
	case DRM_I915_GEM_ENTERVT:
	case DRM_I915_GEM_LEAVEVT:
	case DRM_I915_GEM_EXECBUFFER:
		return -ENODEV;
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
