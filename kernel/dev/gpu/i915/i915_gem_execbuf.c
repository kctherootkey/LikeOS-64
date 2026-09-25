// LikeOS -- EXECBUFFER2: a client's batch, submitted.
//
// The client names the objects its batch touches and where each must be
// in the context's address space (softpin; a client without it gets
// addresses assigned and its relocations patched).  Everything is bound,
// the fences it waits on are waited for, the request is emitted and
// queued, and the fences it promises (an out fence, syncobjs) are
// attached to the request's own.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

#define EXEC_MAX_OBJECTS 65536 /* a batch may name every texture a page has */
#define EXEC_MAX_FENCES 4096
#define EXEC_MAX_RELOCS 65536
#define EXEC_WAIT_NS (10ULL * 1000000000ULL)

void i915_gem_object_flush_for_gpu(struct i915_device *i915, struct drm_gem_object *o);

static int copy_user_bounded(void *dst, uint64_t uptr, size_t len)
{
	if (!uptr || !validate_user_ptr(uptr, len))
		return -EFAULT;
	return copy_from_user(dst, (const void *)(uintptr_t)uptr, len) ? -EFAULT : 0;
}

/* The object's binding in this space: the existing one, or a new one at
 * the given address (pinned) or an assigned one. */
/* Addresses cross this interface in "canonical" form: bit 47 is the
 * sign, repeated through bits 63:48, because that is how the hardware
 * reads a 48-bit graphics address out of a command.  A buffer placed in
 * the upper half of the address space therefore arrives with every top
 * bit set, and taken at face value it is far outside the space.  The
 * address space itself is 48 bits wide and flat, so the sign comes off
 * on the way in and goes back on anything handed out. */
#define I915_ADDR_BITS 48
#define I915_ADDR_MASK ((1ULL << I915_ADDR_BITS) - 1)

static uint64_t addr_from_user(uint64_t a)
{
	return a & I915_ADDR_MASK;
}

static uint64_t addr_to_user(uint64_t a)
{
	return (a & (1ULL << (I915_ADDR_BITS - 1))) ? (a | ~I915_ADDR_MASK) : a;
}

static struct i915_vma *bind_object(struct i915_vm *vm, struct drm_gem_object *o,
				    struct drm_i915_gem_exec_object2 *ex, int *changed)
{
	struct i915_bo *bo = o->priv;
	struct i915_vma *v;
	int pinned = !!(ex->flags & EXEC_OBJECT_PINNED);
	uint64_t want = ex->offset & ~0xfffULL;

	for (v = bo->vmas; v; v = v->next)
		if (v->vm == vm)
			break;
	if (v && v->attached && (!pinned || v->addr == want)) {
		/* bound where it should be, and still owning the range */
		ex->offset = v->addr;
		return v;
	}
	if (v) {
		/* Pinned elsewhere now, or its range was taken by another
		 * object since: bind it again where the client wants it.
		 * The old translations are cleared only if they are still
		 * this binding's. */
		if (pinned) {
			if (i915_vm_vma_release(vm, v))
				i915_vm_unbind(vm, v->addr, v->npages);
		}
	} else {
		v = kalloc(sizeof(*v));
		if (!v)
			return NULL;
		mm_memset(v, 0, sizeof(*v));
		i915_vm_get(vm);
		v->vm = vm;
		v->next = bo->vmas;
		bo->vmas = v;
	}
	if (pinned) {
		v->pinned = 1;
		i915_vm_vma_claim(vm, v, want, o->npages);
	} else if (!v->attached) {
		/* The driver picks the address: below 4 GB unless the client
		 * says the object may live anywhere, since what it writes
		 * into a command may be a 32-bit address. */
		uint64_t align = ex->alignment > 4096 ? ex->alignment : 4096;
		int low = !(ex->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS);
		v->pinned = 0;
		if (i915_vm_vma_alloc(vm, v, o->npages, align, low) != 0)
			return NULL;
	} else {
		want = v->addr;
		i915_vm_vma_claim(vm, v, want, o->npages);
	}
	if (i915_vm_bind(vm, v->addr, o->pages, o->npages,
			 bo->caching == I915_CACHING_NONE) != 0) {
		i915_vm_vma_release(vm, v);
		return NULL;
	}
	ex->offset = v->addr;
	*changed = 1;
	return v;
}

/* Relocations of a client without softpin: each names a target object
 * and where in this object the target's address goes. */
static int apply_relocs(struct drm_file *fp, struct drm_gem_object *o,
			struct drm_i915_gem_exec_object2 *ex,
			struct drm_gem_object **objs,
			struct drm_i915_gem_exec_object2 *exs, uint32_t nobjs,
			int lut)
{
	uint32_t n = ex->relocation_count;
	if (n == 0)
		return 0;
	if (n > EXEC_MAX_RELOCS)
		return -EINVAL;
	/* The processor writes into the object; the caller has made sure
	 * no engine is still using it. */
	struct drm_i915_gem_relocation_entry *r = kalloc(n * sizeof(*r));
	if (!r)
		return -ENOMEM;
	if (copy_user_bounded(r, ex->relocs_ptr, n * sizeof(*r))) {
		kfree(r);
		return -EFAULT;
	}
	int rc = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t target_off = 0;
		uint32_t ti = nobjs;
		if (lut) {
			if (r[i].target_handle >= nobjs) {
				rc = -EINVAL;
				break;
			}
			ti = r[i].target_handle;
		} else {
			for (uint32_t j = 0; j < nobjs; j++) {
				if (exs[j].handle == r[i].target_handle) {
					ti = j;
					break;
				}
			}
		}
		if (ti >= nobjs) {
			rc = -ENOENT;
			break;
		}
		target_off = exs[ti].offset;
		/* A relocation with a write domain is how a client of the
		 * older buffer-manager library says the batch writes the
		 * target (a decoder's output surface): the object is written
		 * by this submission as surely as one flagged so. */
		if (r[i].write_domain)
			exs[ti].flags |= EXEC_OBJECT_WRITE;
		/* what goes INTO a buffer is read back by the hardware out of
		 * a command, so it is written the way the hardware reads it */
		uint64_t value = addr_to_user(target_off + r[i].delta);
		if (r[i].offset + 8 > o->size || (r[i].offset & 3)) {
			rc = -EINVAL;
			break;
		}
		uint32_t page = (uint32_t)(r[i].offset / 4096);
		uint32_t in = (uint32_t)(r[i].offset & 4095);
		uint8_t *va = (uint8_t *)phys_to_virt(o->pages[page]) + in;
		if (in + 8 <= 4096) {
			*(uint32_t *)va = (uint32_t)value;
			*(uint32_t *)(va + 4) = (uint32_t)(value >> 32);
		} else {
			*(uint32_t *)va = (uint32_t)value;
			*(uint32_t *)phys_to_virt(o->pages[page + 1]) = (uint32_t)(value >> 32);
		}
		r[i].presumed_offset = addr_to_user(target_off);
	}
	if (rc == 0)
		copy_to_user((void *)(uintptr_t)ex->relocs_ptr, r, n * sizeof(*r));
	kfree(r);
	(void)fp;
	(void)objs;
	return rc;
}

/* The engine a submission is aimed at, and the slot of the context's
 * logical rings it runs in: the map entry with an explicit map (two
 * entries that name one engine are two logical rings), the engine
 * itself with the legacy map.
 *
 * The video ring flags: on a part with two video engines a BSD
 * submission names one of them, or neither and takes turns; on a part
 * with one the flags mean nothing.  Naming a video ring on any other
 * engine is an error. */
static struct i915_engine *engine_for(struct i915_gem_context *ctx, struct drm_file *fp,
				      uint64_t flags, int *slot)
{
	unsigned ring = (unsigned)(flags & I915_EXEC_RING_MASK);
	unsigned bsd = (unsigned)(flags & I915_EXEC_BSD_MASK);
	struct i915_engine *e;
	if (ctx->explicit_engines) {
		if (ring >= (unsigned)ctx->nengines || ring >= I915_CTX_LRC_SLOTS)
			return NULL;
		e = ctx->engines[ring];
		if (e)
			*slot = (int)ring;
		return e;
	}
	if (ring != I915_EXEC_BSD && bsd)
		return NULL;
	switch (ring) {
	case I915_EXEC_DEFAULT:
	case I915_EXEC_RENDER:
		e = ctx->engines[0];
		break;
	case I915_EXEC_BSD: {
		struct i915_engine *v0 = ctx->engines[2];
		struct i915_engine *v1 = i915_engine_by_id(ctx->i915, I915_VCS1);
		if (!v0 || !v1) {
			e = v0;
			break;
		}
		if (bsd == I915_EXEC_BSD_DEFAULT) {
			struct i915_file *f = fp->priv;
			uint32_t turn = f ? __atomic_fetch_add(&f->bsd_next, 1, __ATOMIC_RELAXED) : 0;
			e = (turn & 1) ? v1 : v0;
		} else if (bsd == I915_EXEC_BSD_RING1) {
			e = v0;
		} else if (bsd == I915_EXEC_BSD_RING2) {
			e = v1;
		} else {
			return NULL;
		}
		break;
	}
	case I915_EXEC_BLT:
		e = ctx->engines[3];
		break;
	case I915_EXEC_VEBOX:
		e = ctx->engines[4];
		break;
	default:
		return NULL;
	}
	if (e)
		*slot = (int)e->id;
	return e;
}

static int wait_fence(struct drm_fence *f)
{
	if (!f || f->signaled)
		return 0;
	int rc = drm_fence_wait_flags(f, EXEC_WAIT_NS, 1);
	if (rc == -ETIMEDOUT || rc == -ETIME) {
		/* Ten seconds is not a wait, it is a fence that will never
		 * signal; the submission fails and the client that made it
		 * rarely says why. */
		static int said;
		task_t *cur = sched_current();
		if (said < 8) {
			said++;
			kprintf("[drm] i915: a submission by process %d (%s) waited %u s for a fence that never signalled\n",
				cur ? (int)cur->tgid : -1, cur ? cur->comm : "?",
				(unsigned)(EXEC_WAIT_NS / 1000000000ULL));
		}
	}
	return rc;
}

long i915_gem_execbuffer2(struct i915_device *i915, struct drm_file *fp,
			  void *kb, int wr)
{
	struct drm_i915_gem_execbuffer2 *a = kb;
	struct drm_i915_gem_exec_object2 *exs = NULL;
	struct drm_gem_object **objs = NULL;
	struct drm_i915_gem_exec_fence *fences = NULL;
	struct i915_gem_context *ctx = NULL;
	struct i915_request *rq = NULL;
	struct drm_fence *in_fence = NULL;
	struct drm_fence **waits = NULL;
	unsigned nwait = 0;
	int locked = 0;
	uint32_t n = a->buffer_count;
	int lut = !!(a->flags & I915_EXEC_HANDLE_LUT);
	int changed = 0;
	long rc;
	/* the fence array (syncobjs to wait for / signal) -- declared here,
	 * ahead of every early exit, since `out' frees them */
	uint32_t nfences = 0;
	uint64_t *tl_handles = NULL, *tl_values = NULL;
	uint32_t ntl = 0;
	const char *why = "";

	if (!i915->gt_ready)
		return -ENODEV;
	if (a->flags & __I915_EXEC_UNKNOWN_FLAGS) {
		why = "unknown flags";
		rc = -EINVAL;
		goto out;
	}
	if (n == 0 || n > EXEC_MAX_OBJECTS) {
		why = "object count";
		rc = -EINVAL;
		goto out;
	}
	if (a->flags & (I915_EXEC_SECURE | I915_EXEC_RESOURCE_STREAMER)) {
		why = "secure or resource streamer";
		rc = -EPERM;
		goto out;
	}
	if (a->flags & I915_EXEC_FENCE_SUBMIT) {
		why = "submit fence";
		rc = -EINVAL;
		goto out;
	}
	if ((a->flags & I915_EXEC_FENCE_OUT) && !wr) {
		why = "out fence on the read-only form";
		rc = -EINVAL;
		goto out;
	}

	ctx = i915_file_context(fp, (uint32_t)(a->rsvd1 & I915_EXEC_CONTEXT_ID_MASK));
	if (!ctx) {
		why = "no such context";
		rc = -ENOENT;
		goto out;
	}
	if (ctx->banned) {
		rc = -EIO;
		goto out;
	}
	int slot = 0;
	struct i915_engine *e = engine_for(ctx, fp, a->flags, &slot);
	if (!e) {
		why = "no such engine";
		rc = -EINVAL;
		goto out;
	}

	exs = kalloc(n * sizeof(*exs));
	objs = kalloc(n * sizeof(*objs));
	if (!exs || !objs) {
		i915_enomem_once(i915, "the object array of a submission");
		why = "object array memory";
		rc = -ENOMEM;
		goto out;
	}
	mm_memset(objs, 0, n * sizeof(*objs));
	if ((rc = copy_user_bounded(exs, a->buffers_ptr, n * sizeof(*exs)))) {
		why = "object array copy";
		goto out;
	}
	/* every address in the array, from here on, is a plain one */
	for (uint32_t i = 0; i < n; i++)
		exs[i].offset = addr_from_user(exs[i].offset);

	if (a->flags & I915_EXEC_FENCE_ARRAY) {
		nfences = a->num_cliprects;
		if (nfences > EXEC_MAX_FENCES) {
			why = "fence count";
			rc = -EINVAL;
			goto out;
		}
		if (nfences) {
			fences = kalloc(nfences * sizeof(*fences));
			if (!fences) {
				rc = -ENOMEM;
				goto out;
			}
			if ((rc = copy_user_bounded(fences, a->cliprects_ptr,
						    nfences * sizeof(*fences)))) {
				why = "fence array copy";
				goto out;
			}
		}
	} else if (a->flags & I915_EXEC_USE_EXTENSIONS) {
		uint64_t next = a->cliprects_ptr;
		for (int k = 0; next && k < 8; k++) {
			struct i915_user_extension ext;
			if ((rc = copy_user_bounded(&ext, next, sizeof(ext))))
				goto out;
			if (ext.name == DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES) {
				struct drm_i915_gem_execbuffer_ext_timeline_fences tf;
				if ((rc = copy_user_bounded(&tf, next, sizeof(tf))))
					goto out;
				if (tf.fence_count > EXEC_MAX_FENCES) {
					rc = -EINVAL;
					goto out;
				}
				nfences = (uint32_t)tf.fence_count;
				if (nfences) {
					fences = kalloc(nfences * sizeof(*fences));
					tl_values = kalloc(nfences * sizeof(uint64_t));
					if (!fences || !tl_values) {
						rc = -ENOMEM;
						goto out;
					}
					if ((rc = copy_user_bounded(fences, tf.handles_ptr,
								    nfences * sizeof(*fences))))
						goto out;
					if ((rc = copy_user_bounded(tl_values, tf.values_ptr,
								    nfences * sizeof(uint64_t))))
						goto out;
					ntl = nfences;
				}
			} else {
				rc = -EINVAL;
				goto out;
			}
			next = ext.next_extension;
		}
	} else if (a->num_cliprects) {
		why = "clip rectangles";
		rc = -EINVAL;
		goto out;
	}

	/* the objects */
	for (uint32_t i = 0; i < n; i++) {
		if (exs[i].flags & __EXEC_OBJECT_UNKNOWN_FLAGS) {
			why = "unknown object flags";
			rc = -EINVAL;
			goto out;
		}
		objs[i] = drm_gem_lookup(fp, exs[i].handle);
		if (!objs[i]) {
			why = "no such object handle";
			rc = -ENOENT;
			goto out;
		}
	}
	waits = kalloc((size_t)n * (1 + I915_ENGINE_CLASSES) * sizeof(*waits));
	if (!waits) {
		why = "memory for the waits";
		rc = -ENOMEM;
		goto out;
	}

	/* waits: the in fence, the fence array, other engines' writes */
	if (a->flags & I915_EXEC_FENCE_IN) {
		in_fence = drm_fence_from_fd((int)(a->rsvd2 & 0xffffffff));
		if (!in_fence) {
			why = "the in fence is not a sync file";
			rc = -EINVAL;
			goto out;
		}
		if ((rc = wait_fence(in_fence))) {
			why = "waiting for the in fence";
			goto out;
		}
	}
	for (uint32_t i = 0; i < nfences; i++) {
		if (fences[i].flags & __I915_EXEC_FENCE_UNKNOWN_FLAGS) {
			why = "unknown fence flags";
			rc = -EINVAL;
			goto out;
		}
		if (!(fences[i].flags & I915_EXEC_FENCE_WAIT))
			continue;
		struct drm_syncobj *so = drm_syncobj_lookup(fp, fences[i].handle);
		if (!so) {
			why = "no such syncobj in the fence array";
			rc = -ENOENT;
			goto out;
		}
		struct drm_fence *f = NULL;
		int frc = drm_syncobj_find_fence(so, ntl ? tl_values[i] : 0, &f);
		drm_syncobj_put(so);
		if (frc == 0) {
			rc = wait_fence(f);
			drm_fence_put(f);
			if (rc) {
				why = "waiting for a fence of the array";
				goto out;
			}
		}
	}
	/* Bindings, relocations and the dependencies, under the submission
	 * lock; the waits themselves outside it, and everything looked at
	 * again afterwards, since another submission may have run meanwhile.
	 * Out of the loop the lock is held and nothing is outstanding. */
	for (;;) {
		mm_write_lock(&i915->submit_lock);
		locked = 1;
		for (uint32_t i = 0; i < n; i++) {
			if (!bind_object(ctx->vm, objs[i], &exs[i], &changed)) {
				i915_bind_failed_once(i915, exs[i].offset,
						      (uint64_t)objs[i]->npages * 4096,
						      !!(exs[i].flags & EXEC_OBJECT_PINNED));
				why = "binding an object";
				rc = -ENOMEM;
				goto out;
			}
		}
		nwait = 0;
		if (!(a->flags & I915_EXEC_NO_RELOC)) {
			/* an object the processor patches must be idle first:
			 * an engine may still be reading the last batch in it */
			for (uint32_t i = 0; i < n; i++)
				if (exs[i].relocation_count)
					i915_gem_object_fences(objs[i], 1, 0, waits, &nwait);
			if (nwait == 0) {
				for (uint32_t i = 0; i < n; i++) {
					if ((rc = apply_relocs(fp, objs[i], &exs[i], objs, exs, n, lut))) {
						why = "relocations";
						goto out;
					}
				}
			}
		}
		if (nwait == 0) {
			/* what the CPU wrote must be visible to the device */
			for (uint32_t i = 0; i < n; i++)
				i915_gem_object_flush_for_gpu(i915, objs[i]);
			/* One engine's work is ordered behind its own; another's
			 * writes must be done before this engine touches the
			 * object, and its reads before this engine writes it. */
			for (uint32_t i = 0; i < n; i++)
				i915_gem_object_fences(objs[i], !!(exs[i].flags & EXEC_OBJECT_WRITE),
						       e->fence_context, waits, &nwait);
		}
		if (nwait == 0)
			break;
		mm_write_unlock(&i915->submit_lock);
		locked = 0;
		rc = 0;
		for (unsigned k = 0; k < nwait; k++) {
			if (rc == 0)
				rc = wait_fence(waits[k]);
			drm_fence_put(waits[k]);
		}
		nwait = 0;
		if (rc) {
			why = "waiting for another engine";
			goto out;
		}
	}

	/* the batch */
	uint32_t bi = (a->flags & I915_EXEC_BATCH_FIRST) ? 0 : n - 1;
	uint64_t batch_addr = exs[bi].offset + a->batch_start_offset;
	if (a->batch_start_offset & 7 || a->batch_start_offset >= objs[bi]->size) {
		why = "batch start offset";
		rc = -EINVAL;
		goto out;
	}
	rq = i915_request_alloc(ctx, e);
	if (!rq) {
		i915_enomem_once(i915, "allocating a request");
		why = "request memory";
		rc = -ENOMEM;
		goto out;
	}
	rq->lrc_idx = slot;
	rq->objs = objs;
	rq->nobjs = n;
	objs = NULL; /* the request owns the references now */
	rc = i915_request_submit(rq, batch_addr, a->batch_len);
	if (rc) {
		if (rc == -ENOMEM)
			i915_enomem_once(i915, "the context image or its ring");
		why = rc == -EBUSY ? "the ring stayed full" :
		      rc == -ENOMEM ? "the context image or its ring" : "submitting";
		goto out;
	}
	ctx->vm->tlb_dirty = 0;

	/* The request's fence on the objects, before any other submission
	 * can look at them; the fences displaced are dropped afterwards,
	 * since dropping may free.  Then the out fence and the syncobjs. */
	{
		unsigned cls = e->class < I915_ENGINE_CLASSES ? e->class : 0;
		unsigned nold = 0;
		uint64_t fl;
		spin_lock_irqsave(&i915->sync_lock, &fl);
		for (uint32_t i = 0; i < n; i++) {
			struct drm_gem_object *o = rq->objs[i];
			struct i915_bo *bo = o->priv;
			if (exs[i].flags & EXEC_OBJECT_WRITE) {
				drm_fence_get(rq->fence);
				if (bo->write_fence)
					waits[nold++] = bo->write_fence;
				bo->write_fence = rq->fence;
				bo->write_class = (uint8_t)cls;
			}
			/* a writer reads too, as far as the busy call is concerned */
			drm_fence_get(rq->fence);
			if (bo->read_fence[cls])
				waits[nold++] = bo->read_fence[cls];
			bo->read_fence[cls] = rq->fence;
			/* the core's teardown wait */
			drm_fence_get(rq->fence);
			if (o->fence)
				waits[nold++] = o->fence;
			o->fence = rq->fence;
		}
		spin_unlock_irqrestore(&i915->sync_lock, fl);
		for (unsigned k = 0; k < nold; k++)
			drm_fence_put(waits[k]);
	}
	mm_write_unlock(&i915->submit_lock);
	locked = 0;
	if (a->flags & I915_EXEC_FENCE_OUT) {
		int fd = drm_fence_export_fd(rq->fence, 1);
		if (fd < 0) {
			rc = fd;
			goto out;
		}
		a->rsvd2 = (a->rsvd2 & 0xffffffffULL) | ((uint64_t)(uint32_t)fd << 32);
	}
	for (uint32_t i = 0; i < nfences; i++) {
		if (!(fences[i].flags & I915_EXEC_FENCE_SIGNAL))
			continue;
		struct drm_syncobj *so = drm_syncobj_lookup(fp, fences[i].handle);
		if (!so)
			continue;
		drm_syncobj_add_point(so, rq->fence, ntl ? tl_values[i] : 0);
		drm_syncobj_put(so);
	}
	/* offsets back to the client (presumed offsets for next time) */
	copy_to_user((void *)(uintptr_t)a->buffers_ptr, exs, n * sizeof(*exs));
	rc = 0;
	/* rq keeps the caller's reference until `out', where it is dropped:
	 * the engine holds its own, so the request stays whole through the
	 * bookkeeping above even if the batch has already completed. */
out:
	/* A refused submission is fatal to the client: its graphics library
	 * aborts on any error but EIO, and says nothing.  So say what was
	 * refused, a few times per boot. */
	if (rc < 0 && rc != -EIO && rc != -ERESTARTSYS && rc != -EINTR) {
		static int said;
		task_t *cur = sched_current();
		if (said < 16) {
			said++;
			kprintf("[drm] i915: submission by process %d (%s) refused (%d): %s; %u objects, flags %llx, batch %u+%u\n",
				cur ? (int)cur->tgid : -1, cur ? cur->comm : "?", (int)rc, why,
				n, (unsigned long long)a->flags, a->batch_start_offset, a->batch_len);
		}
	}
	if (locked)
		mm_write_unlock(&i915->submit_lock);
	if (waits) {
		for (unsigned k = 0; k < nwait; k++)
			drm_fence_put(waits[k]);
		kfree(waits);
	}
	if (rq)
		i915_request_put(rq);
	if (objs) {
		for (uint32_t i = 0; i < n; i++)
			if (objs[i])
				drm_gem_put(objs[i]);
		kfree(objs);
	}
	if (in_fence)
		drm_fence_put(in_fence);
	if (fences)
		kfree(fences);
	if (tl_values)
		kfree(tl_values);
	if (tl_handles)
		kfree(tl_handles);
	if (exs)
		kfree(exs);
	if (ctx)
		i915_context_put(ctx);
	return rc;
}
