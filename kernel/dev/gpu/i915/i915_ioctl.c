// LikeOS -- the i915 ioctl table: device parameters, contexts, spaces.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ---- GETPARAM ---------------------------------------------------------------- */

long i915_getparam(struct i915_device *i915, void *kb)
{
	struct drm_i915_getparam *p = kb;
	const struct intel_device_info *info = i915->info;
	int value;

	switch (p->param) {
	case I915_PARAM_CHIPSET_ID: value = i915->devid; break;
	case I915_PARAM_REVISION: value = i915->revid; break;
	case I915_PARAM_HAS_GEM: value = 1; break;
	case I915_PARAM_NUM_FENCES_AVAIL: value = 32; break;
	case I915_PARAM_HAS_OVERLAY: value = 0; break;
	case I915_PARAM_HAS_PAGEFLIPPING: value = 1; break;
	case I915_PARAM_HAS_EXECBUF2: value = 1; break;
	case I915_PARAM_HAS_BSD: value = !!i915_engine_by_id(i915, I915_VCS0); break;
	case I915_PARAM_HAS_BLT: value = !!i915_engine_by_id(i915, I915_BCS0); break;
	case I915_PARAM_HAS_VEBOX: value = !!i915_engine_by_id(i915, I915_VECS0); break;
	case I915_PARAM_HAS_BSD2: value = !!i915_engine_by_id(i915, I915_VCS1); break;
	case I915_PARAM_HAS_RELAXED_FENCING: value = 1; break;
	case I915_PARAM_HAS_COHERENT_RINGS: value = 1; break;
	case I915_PARAM_HAS_EXEC_CONSTANTS: value = 0; break;
	case I915_PARAM_HAS_RELAXED_DELTA: value = 1; break;
	case I915_PARAM_HAS_GEN7_SOL_RESET: value = 1; break;
	case I915_PARAM_HAS_LLC: value = !!(info->flags & I915_INFO_HAS_LLC); break;
	case I915_PARAM_HAS_ALIASING_PPGTT: value = 3; break; /* full, 48-bit */
	case I915_PARAM_HAS_WAIT_TIMEOUT: value = 1; break;
	case I915_PARAM_HAS_SEMAPHORES: value = 0; break;
	case I915_PARAM_HAS_PRIME_VMAP_FLUSH: value = 1; break;
	case I915_PARAM_HAS_SECURE_BATCHES: value = 0; break;
	case I915_PARAM_HAS_PINNED_BATCHES: value = 1; break;
	case I915_PARAM_HAS_EXEC_NO_RELOC: value = 1; break;
	case I915_PARAM_HAS_EXEC_HANDLE_LUT: value = 1; break;
	case I915_PARAM_HAS_WT: value = !!(info->flags & I915_INFO_HAS_LLC); break;
	case I915_PARAM_CMD_PARSER_VERSION: value = 0; break;
	case I915_PARAM_HAS_COHERENT_PHYS_GTT: value = 1; break;
	case I915_PARAM_MMAP_VERSION: value = 1; break;
	case I915_PARAM_SUBSLICE_TOTAL: {
		unsigned n = 0;
		for (int s = 0; s < 4; s++)
			for (uint32_t m = i915->subslice_mask[s]; m; m >>= 1)
				n += m & 1;
		value = (int)n;
		break;
	}
	case I915_PARAM_EU_TOTAL: value = (int)i915->eu_total; break;
	case I915_PARAM_HAS_GPU_RESET: value = i915->gt_ready ? 1 : 0; break;
	case I915_PARAM_HAS_RESOURCE_STREAMER: value = 0; break;
	case I915_PARAM_HAS_EXEC_SOFTPIN: value = 1; break;
	case I915_PARAM_HAS_POOLED_EU: value = 0; break;
	case I915_PARAM_MIN_EU_IN_POOL: value = 0; break;
	case I915_PARAM_MMAP_GTT_VERSION: value = 4; break;
	case I915_PARAM_HAS_SCHEDULER:
		value = I915_SCHEDULER_CAP_ENABLED | I915_SCHEDULER_CAP_PRIORITY;
		break;
	/* the media firmware: loaded and authenticated, or not (a part on
	 * which the driver does not load it answers 0 like one without) */
	case I915_PARAM_HUC_STATUS: value = i915->guc.huc_authenticated ? 1 : 0; break;
	case I915_PARAM_HAS_EXEC_ASYNC: value = 1; break;
	case I915_PARAM_HAS_EXEC_FENCE: value = 1; break;
	case I915_PARAM_HAS_EXEC_CAPTURE: value = 0; break;
	case I915_PARAM_SLICE_MASK: value = (int)i915->slice_mask; break;
	case I915_PARAM_SUBSLICE_MASK: value = (int)i915->subslice_mask[0]; break;
	case I915_PARAM_HAS_EXEC_BATCH_FIRST: value = 1; break;
	case I915_PARAM_HAS_EXEC_FENCE_ARRAY: value = 1; break;
	case I915_PARAM_HAS_CONTEXT_ISOLATION: value = 1; break; /* render */
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY: value = (int)i915->cs_timestamp_hz; break;
	/* the observation timestamps tick at the command streamer's rate on
	 * every part this driver runs */
	case I915_PARAM_OA_TIMESTAMP_FREQUENCY: value = (int)i915->cs_timestamp_hz; break;
	case I915_PARAM_MMAP_GTT_COHERENT: value = 1; break;
	case I915_PARAM_HAS_EXEC_SUBMIT_FENCE: value = 0; break;
	case I915_PARAM_HAS_EXEC_TIMELINE_FENCES: value = 1; break;
	case I915_PARAM_HAS_USERPTR_PROBE: value = 1; break;
	case I915_PARAM_HAS_CONTEXT_FREQ_HINT: value = 0; break;
	case I915_PARAM_PERF_REVISION:
	case I915_PARAM_PXP_STATUS:
		return -ENODEV;
	default:
		return -EINVAL;
	}
	if (!p->value || !validate_user_ptr((uint64_t)(uintptr_t)p->value, sizeof(int)))
		return -EFAULT;
	if (copy_to_user(p->value, &value, sizeof(value)))
		return -EFAULT;
	return 0;
}

/* ---- contexts and address spaces ------------------------------------------ */

static int copy_user_bounded(void *dst, uint64_t uptr, size_t len)
{
	if (!uptr || !validate_user_ptr(uptr, len))
		return -EFAULT;
	return copy_from_user(dst, (const void *)(uintptr_t)uptr, len) ? -EFAULT : 0;
}

static long ctx_install(struct drm_file *fp, struct i915_gem_context *ctx, uint32_t *id)
{
	struct i915_file *f = fp->priv;
	uint64_t fl;
	spin_lock_irqsave(&f->lock, &fl);
	for (uint32_t i = 1; i < I915_MAX_CONTEXTS; i++) {
		if (!f->ctx[i]) {
			f->ctx[i] = ctx;
			ctx->id = i;
			spin_unlock_irqrestore(&f->lock, fl);
			*id = i;
			return 0;
		}
	}
	spin_unlock_irqrestore(&f->lock, fl);
	return -ENOSPC;
}

static long vm_install(struct drm_file *fp, struct i915_vm *vm, uint32_t *id)
{
	struct i915_file *f = fp->priv;
	uint64_t fl;
	spin_lock_irqsave(&f->lock, &fl);
	for (uint32_t i = 1; i < I915_MAX_VMS; i++) {
		if (!f->vm[i]) {
			f->vm[i] = vm;
			vm->id = i;
			spin_unlock_irqrestore(&f->lock, fl);
			*id = i;
			return 0;
		}
	}
	spin_unlock_irqrestore(&f->lock, fl);
	return -ENOSPC;
}

static struct i915_vm *vm_lookup(struct drm_file *fp, uint32_t id)
{
	struct i915_file *f = fp->priv;
	struct i915_vm *vm = NULL;
	uint64_t fl;
	if (!f || id == 0 || id >= I915_MAX_VMS)
		return NULL;
	spin_lock_irqsave(&f->lock, &fl);
	vm = f->vm[id];
	if (vm)
		i915_vm_get(vm);
	spin_unlock_irqrestore(&f->lock, fl);
	return vm;
}

/* The engine map: a list of (class, instance) the client's indices refer
 * to; an (invalid, none) entry is a hole. */
static long ctx_set_engines(struct i915_device *i915, struct i915_gem_context *ctx,
			    struct drm_i915_gem_context_param *p)
{
	if (p->size == 0) {
		/* back to the default map */
		ctx->explicit_engines = 0;
		ctx->engines[0] = ctx->engines[1] = i915_engine_by_id(i915, I915_RCS0);
		ctx->engines[2] = i915_engine_by_id(i915, I915_VCS0);
		ctx->engines[3] = i915_engine_by_id(i915, I915_BCS0);
		ctx->engines[4] = i915_engine_by_id(i915, I915_VECS0);
		ctx->nengines = 5;
		return 0;
	}
	if (p->size < 8 || (p->size - 8) % 4)
		return -EINVAL;
	uint32_t n = (p->size - 8) / 4;
	if (n > 16)
		return -EINVAL;
	uint8_t buf[8 + 16 * 4];
	if (copy_user_bounded(buf, p->value, p->size))
		return -EFAULT;
	struct i915_engine *map[16];
	for (uint32_t i = 0; i < n; i++) {
		int16_t cls = (int16_t)(buf[8 + i * 4] | (buf[9 + i * 4] << 8));
		int16_t inst = (int16_t)(buf[10 + i * 4] | (buf[11 + i * 4] << 8));
		if (cls == I915_ENGINE_CLASS_INVALID) {
			map[i] = NULL;
			continue;
		}
		map[i] = i915_engine_by_class(i915, cls, inst);
		if (!map[i])
			return -ENOENT;
	}
	/* The extensions.  A balanced engine stands in an empty slot for a
	 * set of siblings of one class; the reference spreads its work over
	 * them, this driver runs it on the first sibling, which on a part
	 * with one video engine is the same thing.  Bonds only say which
	 * sibling a parallel submission pairs with, and parallel submission
	 * is not offered, so a bond is accepted and means nothing. */
	uint64_t ext = *(uint64_t *)buf;
	for (int depth = 0; ext; depth++) {
		if (depth >= 16)
			return -EINVAL;
		struct i915_user_extension ue;
		if (copy_user_bounded(&ue, ext, sizeof(ue)))
			return -EFAULT;
		if (ue.flags)
			return -EINVAL;
		switch (ue.name) {
		case I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE: {
			struct i915_context_engines_load_balance lb;
			struct i915_engine_class_instance sib[16];
			if (copy_user_bounded(&lb, ext, sizeof(lb)))
				return -EFAULT;
			if (lb.flags || lb.mbz64)
				return -EINVAL;
			if (lb.engine_index >= n || map[lb.engine_index])
				return -EINVAL;
			if (lb.num_siblings == 0 || lb.num_siblings > 16)
				return -EINVAL;
			if (copy_user_bounded(sib, ext + sizeof(lb),
					      (uint64_t)lb.num_siblings * sizeof(sib[0])))
				return -EFAULT;
			struct i915_engine *first = NULL;
			for (uint32_t k = 0; k < lb.num_siblings; k++) {
				struct i915_engine *se = i915_engine_by_class(
					i915, sib[k].engine_class, sib[k].engine_instance);
				if (!se)
					return -ENOENT;
				if (first && se->class != first->class)
					return -EINVAL;
				if (!first)
					first = se;
			}
			map[lb.engine_index] = first;
			break;
		}
		case I915_CONTEXT_ENGINES_EXT_BOND:
			break;
		default:
			return -EINVAL;
		}
		ext = ue.next_extension;
	}
	for (uint32_t i = 0; i < n; i++)
		ctx->engines[i] = map[i];
	ctx->nengines = (int)n;
	ctx->explicit_engines = 1;
	return 0;
}

static long ctx_get_engines(struct i915_gem_context *ctx,
			    struct drm_i915_gem_context_param *p)
{
	if (!ctx->explicit_engines) {
		p->size = 0;
		return 0;
	}
	uint32_t need = 8 + (uint32_t)ctx->nengines * 4;
	if (p->size == 0) {
		p->size = need;
		return 0;
	}
	if (p->size < need)
		return -EINVAL;
	uint8_t buf[8 + 16 * 4];
	mm_memset(buf, 0, sizeof(buf));
	for (int i = 0; i < ctx->nengines; i++) {
		int16_t cls = ctx->engines[i] ? ctx->engines[i]->class : I915_ENGINE_CLASS_INVALID;
		int16_t inst = ctx->engines[i] ? ctx->engines[i]->instance : I915_ENGINE_CLASS_INVALID_NONE;
		buf[8 + i * 4] = (uint8_t)cls;
		buf[9 + i * 4] = (uint8_t)(cls >> 8);
		buf[10 + i * 4] = (uint8_t)inst;
		buf[11 + i * 4] = (uint8_t)(inst >> 8);
	}
	if (!p->value || !validate_user_ptr(p->value, need))
		return -EFAULT;
	if (copy_to_user((void *)(uintptr_t)p->value, buf, need))
		return -EFAULT;
	p->size = need;
	return 0;
}

static long ctx_setparam(struct i915_device *i915, struct drm_file *fp,
			 struct i915_gem_context *ctx, struct drm_i915_gem_context_param *p)
{
	switch (p->param) {
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
	case I915_CONTEXT_PARAM_BAN_PERIOD:
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
	case I915_CONTEXT_PARAM_RINGSIZE:
		return 0;
	case I915_CONTEXT_PARAM_BANNABLE:
		ctx->bannable = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_RECOVERABLE:
		ctx->recoverable = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_PERSISTENCE:
		ctx->persistent = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_PRIORITY:
		if ((int64_t)p->value < I915_CONTEXT_MIN_USER_PRIORITY ||
		    (int64_t)p->value > I915_CONTEXT_MAX_USER_PRIORITY)
			return -EINVAL;
		ctx->priority = (int)(int64_t)p->value;
		return 0;
	case I915_CONTEXT_PARAM_VM: {
		struct i915_vm *vm = vm_lookup(fp, (uint32_t)p->value);
		if (!vm)
			return -ENOENT;
		/* only before the context has run anywhere */
		for (int i = 0; i < I915_CTX_LRC_SLOTS; i++)
			if (ctx->lrc[i].obj) {
				i915_vm_put(vm);
				return -EBUSY;
			}
		i915_vm_put(ctx->vm);
		ctx->vm = vm; /* the lookup's reference */
		return 0;
	}
	case I915_CONTEXT_PARAM_ENGINES:
		for (int i = 0; i < I915_CTX_LRC_SLOTS; i++)
			if (ctx->lrc[i].obj)
				return -EBUSY;
		return ctx_set_engines(i915, ctx, p);
	case I915_CONTEXT_PARAM_SSEU:
		/* accepted as-is: the fused configuration is what runs */
		return 0;
	case I915_CONTEXT_PARAM_GTT_SIZE:
		return -EINVAL;
	case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
	case I915_CONTEXT_PARAM_LOW_LATENCY:
	case I915_CONTEXT_PARAM_CONTEXT_IMAGE:
		return -ENODEV;
	default:
		return -EINVAL;
	}
}

static long ctx_getparam(struct drm_file *fp, struct i915_gem_context *ctx,
			 struct drm_i915_gem_context_param *p)
{
	switch (p->param) {
	case I915_CONTEXT_PARAM_GTT_SIZE:
		p->size = 0;
		p->value = I915_VM_SIZE;
		return 0;
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
	case I915_CONTEXT_PARAM_BAN_PERIOD:
		p->size = 0;
		p->value = 0;
		return 0;
	case I915_CONTEXT_PARAM_BANNABLE:
		p->size = 0;
		p->value = ctx->bannable;
		return 0;
	case I915_CONTEXT_PARAM_RECOVERABLE:
		p->size = 0;
		p->value = ctx->recoverable;
		return 0;
	case I915_CONTEXT_PARAM_PERSISTENCE:
		p->size = 0;
		p->value = ctx->persistent;
		return 0;
	case I915_CONTEXT_PARAM_PRIORITY:
		p->size = 0;
		p->value = (uint64_t)(int64_t)ctx->priority;
		return 0;
	case I915_CONTEXT_PARAM_VM: {
		/* the context's space becomes a client-visible one */
		uint32_t id = ctx->vm->id;
		if (!id) {
			i915_vm_get(ctx->vm);
			if (vm_install(fp, ctx->vm, &id) != 0) {
				i915_vm_put(ctx->vm);
				return -ENOSPC;
			}
		}
		p->size = 0;
		p->value = id;
		return 0;
	}
	case I915_CONTEXT_PARAM_ENGINES:
		return ctx_get_engines(ctx, p);
	case I915_CONTEXT_PARAM_SSEU: {
		struct drm_i915_gem_context_param_sseu s;
		struct i915_device *i915 = ctx->i915;
		if (p->size == 0) {
			p->size = sizeof(s);
			return 0;
		}
		if (p->size < sizeof(s))
			return -EINVAL;
		if (copy_user_bounded(&s, p->value, sizeof(s)))
			return -EFAULT;
		s.slice_mask = i915->slice_mask;
		s.subslice_mask = i915->subslice_mask[0];
		s.min_eus_per_subslice = 8;
		s.max_eus_per_subslice = 8;
		if (copy_to_user((void *)(uintptr_t)p->value, &s, sizeof(s)))
			return -EFAULT;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

long i915_context_ioctl(struct i915_device *i915, struct drm_file *fp, unsigned nr,
			void *kb, unsigned size, int *handled)
{
	*handled = 1;
	if (!i915->gt_ready)
		return -ENODEV;
	switch (nr) {
	case DRM_I915_GEM_CONTEXT_CREATE: {
		uint32_t id;
		struct i915_gem_context *ctx = i915_context_create(i915, fp, NULL);
		if (!ctx)
			return -ENOMEM;
		if (size >= sizeof(struct drm_i915_gem_context_create_ext)) {
			struct drm_i915_gem_context_create_ext *a = kb;
			if (a->flags & I915_CONTEXT_CREATE_FLAGS_UNKNOWN) {
				i915_context_put(ctx);
				return -EINVAL;
			}
			if (a->flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS) {
				uint64_t next = a->extensions;
				for (int n = 0; next && n < 8; n++) {
					struct drm_i915_gem_context_create_ext_setparam sp;
					if (copy_user_bounded(&sp, next, sizeof(sp))) {
						i915_context_put(ctx);
						return -EFAULT;
					}
					if (sp.base.name != I915_CONTEXT_CREATE_EXT_SETPARAM) {
						i915_context_put(ctx);
						return -EINVAL;
					}
					long rc = ctx_setparam(i915, fp, ctx, &sp.param);
					if (rc) {
						i915_context_put(ctx);
						return rc;
					}
					next = sp.base.next_extension;
				}
			}
			long rc = ctx_install(fp, ctx, &id);
			if (rc) {
				i915_context_put(ctx);
				return rc;
			}
			a->ctx_id = id;
			return 0;
		}
		struct drm_i915_gem_context_create *a = kb;
		long rc = ctx_install(fp, ctx, &id);
		if (rc) {
			i915_context_put(ctx);
			return rc;
		}
		a->ctx_id = id;
		return 0;
	}
	case DRM_I915_GEM_CONTEXT_DESTROY: {
		struct drm_i915_gem_context_destroy *a = kb;
		struct i915_file *f = fp->priv;
		uint64_t fl;
		if (a->ctx_id == 0 || a->ctx_id >= I915_MAX_CONTEXTS || !f)
			return -ENOENT;
		spin_lock_irqsave(&f->lock, &fl);
		struct i915_gem_context *ctx = f->ctx[a->ctx_id];
		f->ctx[a->ctx_id] = NULL;
		spin_unlock_irqrestore(&f->lock, fl);
		if (!ctx)
			return -ENOENT;
		ctx->closed = 1;
		i915_context_put(ctx);
		return 0;
	}
	case DRM_I915_GEM_CONTEXT_GETPARAM:
	case DRM_I915_GEM_CONTEXT_SETPARAM: {
		struct drm_i915_gem_context_param *p = kb;
		struct i915_gem_context *ctx = i915_file_context(fp, p->ctx_id);
		if (!ctx)
			return -ENOENT;
		long rc = (nr == DRM_I915_GEM_CONTEXT_SETPARAM) ? ctx_setparam(i915, fp, ctx, p) :
								   ctx_getparam(fp, ctx, p);
		i915_context_put(ctx);
		return rc;
	}
	case DRM_I915_GEM_VM_CREATE: {
		struct drm_i915_gem_vm_control *a = kb;
		if (a->flags || a->extensions)
			return -EINVAL;
		struct i915_vm *vm = i915_vm_create(i915);
		if (!vm)
			return -ENOMEM;
		uint32_t id;
		long rc = vm_install(fp, vm, &id);
		if (rc) {
			i915_vm_put(vm);
			return rc;
		}
		a->vm_id = id;
		return 0;
	}
	case DRM_I915_GEM_VM_DESTROY: {
		struct drm_i915_gem_vm_control *a = kb;
		struct i915_file *f = fp->priv;
		uint64_t fl;
		if (a->flags || a->extensions)
			return -EINVAL;
		if (!f || a->vm_id == 0 || a->vm_id >= I915_MAX_VMS)
			return -ENOENT;
		spin_lock_irqsave(&f->lock, &fl);
		struct i915_vm *vm = f->vm[a->vm_id];
		f->vm[a->vm_id] = NULL;
		spin_unlock_irqrestore(&f->lock, fl);
		if (!vm)
			return -ENOENT;
		i915_vm_put(vm);
		return 0;
	}
	case DRM_I915_GET_RESET_STATS: {
		struct drm_i915_reset_stats *a = kb;
		if (a->flags)
			return -EINVAL;
		struct i915_gem_context *ctx = i915_file_context(fp, a->ctx_id);
		if (!ctx)
			return -ENOENT;
		a->reset_count = i915->gpu_reset_count;
		a->batch_active = ctx->batch_active;
		a->batch_pending = ctx->batch_pending;
		i915_context_put(ctx);
		return 0;
	}
	case DRM_I915_REG_READ: {
		struct drm_i915_reg_read *a = kb;
		uint64_t off = a->offset & ~0x7ULL;
		int wide = !!(a->offset & I915_REG_READ_8B_WA);
		/* the timestamps only: what a profiler reads */
		if (off != RING_TIMESTAMP(RENDER_RING_BASE))
			return -EINVAL;
		if (wide)
			a->val = i915_read64(i915, (uint32_t)off);
		else
			a->val = i915_read32(i915, (uint32_t)off);
		return 0;
	}
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
