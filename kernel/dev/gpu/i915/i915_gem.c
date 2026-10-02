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
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
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
	if (ctx || id != 0 || !g_i915.gt_ready)
		return ctx;
	/* A file opened before the GT was up has no default context yet:
	 * it is made on first use, as if it had been there from the open. */
	struct i915_gem_context *made = i915_context_create(&g_i915, fp, NULL);
	if (!made)
		return NULL;
	made->id = 0;
	spin_lock_irqsave(&f->lock, &fl);
	if (!f->ctx[0]) {
		f->ctx[0] = made;
		made = NULL;
	}
	ctx = f->ctx[0];
	i915_context_get(ctx);
	spin_unlock_irqrestore(&f->lock, fl);
	if (made)
		i915_context_put(made);
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
		i915_ggtt_unbind(&g_i915, bo->ggtt,
				 bo->ggtt_size ? bo->ggtt_size : (uint32_t)(o->npages * 4096));
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
	i915_lmem_object_free(&g_i915, o);
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

/* What a user of the object must wait for, as one fence (merged), or
 * NULL when the object is idle: every engine's use for a writer, the
 * writer's for a reader.  The dma-buf's poll, sync and fence export. */
struct drm_fence *i915_gem_busy_fence(struct drm_gem_object *o, int write)
{
	struct drm_fence *fs[1 + I915_ENGINE_CLASSES];
	unsigned n = 0;

	i915_gem_object_fences(o, write, 0, fs, &n);
	if (!n)
		return NULL;
	struct drm_fence *m = fs[0];
	for (unsigned i = 1; i < n; i++) {
		struct drm_fence *both = drm_fence_merge(m, fs[i]);
		if (both) {
			drm_fence_put(m);
			drm_fence_put(fs[i]);
			m = both;
		} else {
			/* no memory: the later of the two is the better half */
			drm_fence_put(m);
			m = fs[i];
		}
	}
	return m;
}

/* A fence from outside (a sync file the client attached to the shared
 * buffer): the next submissions that touch the object wait for it -- as
 * they wait for a writer's when `write', else only those that write.
 * It joins what the slot already holds rather than replacing it. */
int i915_gem_attach_fence(struct drm_gem_object *o, struct drm_fence *f, int write)
{
	struct i915_bo *bo = o->priv;
	struct i915_device *i915 = &g_i915;
	unsigned cls = I915_ENGINE_CLASS_RENDER;
	uint64_t fl;

	if (!bo)
		return -EINVAL;
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present && i915->engines[i].fence_context == f->context &&
		    i915->engines[i].class < I915_ENGINE_CLASSES)
			cls = i915->engines[i].class;
	for (int attempt = 0; attempt < 8; attempt++) {
		struct drm_fence **slot, *old, *merged;
		spin_lock_irqsave(&i915->sync_lock, &fl);
		slot = write ? &bo->write_fence : &bo->read_fence[cls];
		old = *slot;
		if (old)
			drm_fence_get(old);
		spin_unlock_irqrestore(&i915->sync_lock, fl);
		if (old && !old->signaled) {
			merged = drm_fence_merge(old, f);
			if (!merged) {
				drm_fence_put(old);
				return -ENOMEM;
			}
		} else {
			drm_fence_get(f);
			merged = f;
		}
		int done = 0;
		spin_lock_irqsave(&i915->sync_lock, &fl);
		if (*slot == old) {
			*slot = merged;
			if (write)
				bo->write_class = (uint8_t)cls;
			done = 1;
		}
		spin_unlock_irqrestore(&i915->sync_lock, fl);
		if (done) {
			/* the slot's reference to the old one goes with it */
			if (old)
				drm_fence_put(old);
			if (old)
				drm_fence_put(old);
			return 0;
		}
		drm_fence_put(merged);
		if (old)
			drm_fence_put(old);
	}
	return -EAGAIN;
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
	/* a discrete card snoops system memory, and local memory is
	 * mapped write-combining: nothing to flush either way */
	if (i915->info->flags & I915_INFO_IS_DGFX)
		return 1;
	if (!(i915->info->flags & I915_INFO_HAS_LLC))
		return 0;
	return bo->caching != I915_CACHING_NONE;
}

static void bo_clflush(struct drm_gem_object *o)
{
	if (!o->pages)
		return;
	if (g_i915.info->flags & I915_INFO_IS_DGFX) {
		__asm__ volatile("sfence" ::: "memory");
		return;
	}
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
	bo->pat_set = 0;
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
	if (i915_lmem_object(o)) {
		i915_lmem_free_pages(&g_i915, o);
		return;
	}
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
	struct i915_lmem_placement pl = { 0 };

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
			int rc = i915_lmem_parse_regions(i915, &mr, &pl);
			if (rc)
				return rc;
			break;
		}
		case I915_GEM_CREATE_EXT_SET_PAT: {
			struct drm_i915_gem_create_ext_set_pat sp;
			/* Meteor Lake on: the table is the client's to choose
			 * from, within the entries it may use */
			if (i915->gt_ip < I915_IP(12, 70))
				return -ENODEV;
			if (copy_user_bounded(&sp, next, sizeof(sp)))
				return -EFAULT;
			if (!i915_pat_index_valid(i915, sp.pat_index))
				return -EINVAL;
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
	int err;
	struct drm_gem_object *o =
		i915_lmem_object_create(i915, a->size, &pl,
					!!(a->flags & I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS), &err);
	if (!o)
		return err;
	struct i915_bo *bo = o->priv;
	bo->caching = (i915->info->flags & I915_INFO_HAS_LLC) ? I915_CACHING_CACHED :
								  I915_CACHING_NONE;
	bo->madv = I915_MADV_WILLNEED;
	bo->user_size = a->size;
	/* compressed from the first access: the state the pages' last user
	 * left goes before the client sees them */
	if (have_pat && i915_pat_compressed(i915, pat)) {
		int crc = i915_gt_clear_ccs(i915, o);
		if (crc) {
			drm_gem_put(o);
			return crc;
		}
	}
	if (drm_gem_handle_create(fp, o, &handle) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	if (have_pat) {
		/* the client's choice of page attribute table entry stands for
		 * its bindings; whether that entry is the uncached one decides
		 * how the processor's side is kept in step */
		bo->pat_index = pat;
		bo->pat_set = 1;
		bo->caching = i915_pat_is_uncached(i915, pat) ? I915_CACHING_NONE :
								 I915_CACHING_CACHED;
	}
	a->handle = handle;
	a->size = o->size;
	drm_gem_put(o); /* the handle holds it */
	return 0;
}

static long gem_mmap_offset(struct i915_device *i915, struct drm_file *fp,
			    struct drm_i915_gem_mmap_offset *a)
{
	unsigned kind;

	/* The older MMAP_GTT call is this one with the flags word zero:
	 * a mapping through the aperture.  What is asked is checked before
	 * the object is looked at: an unknown mode is invalid, the aperture
	 * on a part without one (Meteor Lake on, the discrete parts) is
	 * not there. */
	if (a->extensions)
		return -EINVAL;
	switch (a->flags) {
	case I915_MMAP_OFFSET_GTT:
		if (!i915->bar_aperture.size)
			return -ENODEV;
		kind = I915_MMAP_KIND_GTT;
		break;
	case I915_MMAP_OFFSET_WC:
		kind = 1;
		break;
	case I915_MMAP_OFFSET_WB:
		kind = 2;
		break;
	case I915_MMAP_OFFSET_UC:
		kind = 3;
		break;
	case I915_MMAP_OFFSET_FIXED:
		kind = 0; /* below: only where local memory decides */
		break;
	default:
		return -EINVAL;
	}
	struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
	if (!o)
		return -ENOENT;
	struct i915_bo *bo = o->priv;
	long rc = 0;
	if (bo && bo->userptr) {
		/* a client's own pages are mapped by the client already */
		rc = -ENODEV;
	} else if (i915_lmem_present(i915)) {
		/* a part with local memory has one kind of mapping: the one
		 * the object's memory wants, and only that */
		if (a->flags != I915_MMAP_OFFSET_FIXED)
			rc = -ENODEV;
		else
			kind = i915_lmem_mmap_kind(o);
	} else if (a->flags == I915_MMAP_OFFSET_FIXED) {
		/* without local memory the caller chooses the mode */
		rc = -ENODEV;
	}
	if (rc == 0)
		a->offset = drm_gem_mmap_offset_kind(o, kind);
	drm_gem_put(o);
	return rc;
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
	int legacy = i915_is_legacy(i915);

	/* tiling is a property of a fence: without fence registers there
	 * is nothing to set or report */
	if (!i915->num_fences)
		return -EOPNOTSUPP;
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
		 * Y tiles 128; before Broadwell the generation's own limits
		 * (tile sizes, fence pitch) */
		uint32_t tw = a->tiling_mode == I915_TILING_X ? 512 :
			      a->tiling_mode == I915_TILING_Y ? 128 : 1;
		if (legacy ? !i915_legacy_tiling_ok(i915, a->tiling_mode, a->stride) :
			     (a->tiling_mode != I915_TILING_NONE &&
			      (a->stride == 0 || (a->stride % tw)))) {
			drm_gem_put(o);
			return -EINVAL;
		}
		bo->tiling = a->tiling_mode;
		bo->stride = a->tiling_mode == I915_TILING_NONE ? 0 : a->stride;
		a->stride = bo->stride;
		a->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		if (legacy)
			i915_legacy_swizzle(i915, bo->tiling, &a->swizzle_mode, NULL);
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
	if (legacy)
		i915_legacy_swizzle(i915, bo->tiling, &a->swizzle_mode, &a->phys_swizzle_mode);
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
		/* cached without a shared cache is the snooped kind */
		if (!(i915->info->flags & (I915_INFO_HAS_LLC | I915_INFO_HAS_SNOOP)) &&
		    a->caching == I915_CACHING_CACHED) {
			drm_gem_put(o);
			return -ENODEV;
		}
		if (bo->caching != a->caching) {
			/* the bindings change attribute: rebind each (a
			 * binding may reserve more than the object has, the
			 * pages are the object's) */
			bo->caching = a->caching;
			bo->pat_set = 0;
			int unc = (a->caching == I915_CACHING_NONE);
			for (struct i915_vma *v = bo->vmas; v; v = v->next)
				if (v->attached)
					i915_vm_bind(v->vm, v->addr, o->pages, o->npages, unc);
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
	int open_fence = 0;
	/* Anything still open is looked at against the engines first: a
	 * client that polls BUSY (a display server waiting for a client's
	 * shared buffer) must not see "busy" for as long as it takes the
	 * next interrupt or the worker to notice a completion. */
	spin_lock_irqsave(&g_i915.sync_lock, &fl);
	if (bo && bo->write_fence && !bo->write_fence->signaled)
		open_fence = 1;
	for (int c = 0; bo && c < I915_ENGINE_CLASSES; c++)
		if (bo->read_fence[c] && !bo->read_fence[c]->signaled)
			open_fence = 1;
	spin_unlock_irqrestore(&g_i915.sync_lock, fl);
	if (open_fence && g_i915.drm.drv && g_i915.drm.drv->fence_poll)
		g_i915.drm.drv->fence_poll(&g_i915.drm);
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
	/* not on the discrete parts, nor past Gen12's first version:
	 * those have MMAP_OFFSET only */
	if ((g_i915.info->flags & I915_INFO_IS_DGFX) || g_i915.gt_ip > I915_IP(12, 0))
		return -EOPNOTSUPP;
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
		/* MMAP_OFFSET shares the number: the shorter MMAP_GTT arrives
		 * zero-extended, so with the flags of a GTT mapping */
		if (size < sizeof(struct drm_i915_gem_mmap_gtt))
			return -EINVAL;
		return gem_mmap_offset(i915, fp, kb);
	case DRM_I915_GEM_SET_DOMAIN:
		if (i915->info->flags & I915_INFO_IS_DGFX)
			return -ENODEV;
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
	case DRM_I915_GEM_GET_CACHING:
		if (i915->info->flags & I915_INFO_IS_DGFX)
			return -ENODEV;
		return gem_caching(i915, fp, kb, nr == DRM_I915_GEM_SET_CACHING);
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
		if (i915_is_legacy(i915))
			a->aper_available_size = i915_legacy_aperture_available(i915);
		else
			a->aper_available_size = i915->ggtt_bytes > (256u << 20) ?
							 i915->ggtt_bytes - (256u << 20) :
							 0;
		return 0;
	}
	case DRM_I915_GEM_PREAD:
	case DRM_I915_GEM_PWRITE:
		if (i915->info->flags & I915_INFO_IS_DGFX)
			return -EOPNOTSUPP;
		return gem_pread_pwrite(i915, fp, kb, nr == DRM_I915_GEM_PWRITE);
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
