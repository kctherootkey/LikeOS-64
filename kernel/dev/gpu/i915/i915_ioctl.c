// LikeOS -- the i915 ioctl table: device parameters, contexts, spaces.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ---- GETPARAM ---------------------------------------------------------------- */

long i915_getparam(struct i915_device *i915, void *kb)
{
	struct drm_i915_getparam *p = kb;
	const struct intel_device_info *info = i915->info;
	int value;

	/* before Broadwell some answers differ (fences, the kind of PPGTT) */
	if (i915_is_legacy(i915) && i915_legacy_getparam(i915, (int)p->param, &value))
		goto answer;
	switch (p->param) {
	case I915_PARAM_CHIPSET_ID: value = i915->devid; break;
	/* From Xe2 on the graphics IP's own revision (GMD_ID), which is
	 * what the stepping tables of the graphics library are keyed on;
	 * before, the PCI revision. */
	case I915_PARAM_REVISION:
		value = i915->gt_ip >= I915_IP(20, 0) && i915->gt_step >= I915_STEP_A0 &&
				i915->gt_step < I915_STEP_FUTURE ?
				i915->gt_step - I915_STEP_A0 :
				i915->revid;
		break;
	case I915_PARAM_HAS_GEM: value = 1; break;
	case I915_PARAM_NUM_FENCES_AVAIL: value = i915->num_fences; break;
	case I915_PARAM_HAS_OVERLAY: value = 0; break;
	case I915_PARAM_HAS_PAGEFLIPPING: value = 1; break;
	case I915_PARAM_HAS_EXECBUF2: value = 1; break;
	/* the engines as a client numbers them: the second video engine is
	 * the second one present, whichever the fuses left (Meteor Lake's
	 * vcs2 with vcs1 fused off) */
	case I915_PARAM_HAS_BSD:
		value = !!i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 0);
		break;
	case I915_PARAM_HAS_BLT:
		value = !!i915_engine_lookup_user(i915, I915_ENGINE_CLASS_COPY, 0);
		break;
	case I915_PARAM_HAS_VEBOX:
		value = !!i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO_ENHANCE, 0);
		break;
	case I915_PARAM_HAS_BSD2:
		value = !!i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 1);
		break;
	case I915_PARAM_HAS_RELAXED_FENCING: value = 1; break;
	case I915_PARAM_HAS_COHERENT_RINGS: value = 1; break;
	case I915_PARAM_HAS_EXEC_CONSTANTS: return -ENODEV; /* an old one, rejected */
	case I915_PARAM_HAS_RELAXED_DELTA: value = 1; break;
	case I915_PARAM_HAS_GEN7_SOL_RESET: value = 1; break;
	case I915_PARAM_HAS_LLC: value = !!(info->flags & I915_INFO_HAS_LLC); break;
	case I915_PARAM_HAS_ALIASING_PPGTT: value = I915_GEM_PPGTT_FULL; break;
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
		if (!value)
			return -ENODEV;
		break;
	}
	case I915_PARAM_EU_TOTAL:
		value = (int)i915->eu_total;
		if (!value)
			return -ENODEV;
		break;
	case I915_PARAM_HAS_GPU_RESET: value = i915->gt_ready ? 1 : 0; break;
	case I915_PARAM_HAS_RESOURCE_STREAMER: value = 0; break;
	case I915_PARAM_HAS_EXEC_SOFTPIN: value = 1; break;
	case I915_PARAM_HAS_POOLED_EU: value = 0; break;
	case I915_PARAM_MIN_EU_IN_POOL: value = 0; break;
	case I915_PARAM_MMAP_GTT_VERSION: value = 4; break;
	case I915_PARAM_HAS_SCHEDULER:
		value = I915_SCHEDULER_CAP_ENABLED | I915_SCHEDULER_CAP_PRIORITY;
		break;
	/* the media firmware: loaded and authenticated, or not; a part
	 * without one has no such thing to report */
	case I915_PARAM_HUC_STATUS:
		if (!(info->flags & I915_INFO_HAS_GUC) || !info->huc_fw)
			return -ENODEV;
		value = i915->guc.huc_authenticated ? 1 : 0;
		break;
	case I915_PARAM_HAS_EXEC_ASYNC: value = 1; break;
	case I915_PARAM_HAS_EXEC_FENCE: value = 1; break;
	case I915_PARAM_HAS_EXEC_CAPTURE: value = 1; break; /* accepted, nothing captured */
	/* from Xe_HP on the topology query alone describes the slices */
	case I915_PARAM_SLICE_MASK:
		if (i915->gt_ip >= I915_IP(12, 55))
			return -EINVAL;
		value = (int)i915->slice_mask;
		if (!value)
			return -ENODEV;
		break;
	case I915_PARAM_SUBSLICE_MASK:
		if (i915->gt_ip >= I915_IP(12, 55))
			return -EINVAL;
		value = (int)i915->subslice_mask[0];
		if (!value)
			return -ENODEV;
		break;
	case I915_PARAM_HAS_EXEC_BATCH_FIRST: value = 1; break;
	case I915_PARAM_HAS_EXEC_FENCE_ARRAY: value = 1; break;
	/* the engine classes whose contexts start from a clean image: all
	 * of them once the GT runs, none before */
	case I915_PARAM_HAS_CONTEXT_ISOLATION:
		value = 0;
		if (i915->gt_ready)
			for (int i = 0; i < I915_NUM_ENGINES; i++)
				if (i915->engines[i].present)
					value |= 1 << i915->engines[i].class;
		break;
	case I915_PARAM_CS_TIMESTAMP_FREQUENCY:
		value = (int)i915->cs_timestamp_hz;
		if (!value)
			return -ENODEV;
		break;
	/* the observation timestamps tick at the command streamer's rate on
	 * every part this driver runs */
	case I915_PARAM_OA_TIMESTAMP_FREQUENCY: value = (int)i915->cs_timestamp_hz; break;
	/* the aperture is coherent with the processor's view, the Atom
	 * parts aside; Xe_HP on has none to speak of */
	case I915_PARAM_MMAP_GTT_COHERENT:
		value = i915->gt_ip < I915_IP(12, 55) &&
			info->platform != I915_PLATFORM_VALLEYVIEW &&
			info->platform != I915_PLATFORM_CHERRYVIEW;
		break;
	case I915_PARAM_HAS_EXEC_SUBMIT_FENCE: value = 1; break;
	case I915_PARAM_HAS_EXEC_TIMELINE_FENCES: value = 1; break;
	case I915_PARAM_HAS_USERPTR_PROBE: value = 1; break;
	case I915_PARAM_HAS_CONTEXT_FREQ_HINT: value = 0; break;
	case I915_PARAM_PERF_REVISION:
	case I915_PARAM_PXP_STATUS:
		return -ENODEV;
	default:
		return -EINVAL;
	}
answer:
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

/* An extension's header: the flags and the reserved words are zero. */
static int user_ext_mbz(const struct i915_user_extension *ue)
{
	if (ue->flags)
		return -EINVAL;
	for (int i = 0; i < 4; i++)
		if (ue->rsvd[i])
			return -EINVAL;
	return 0;
}

/* How long a chain of extensions may be. */
#define USER_EXT_DEPTH 512

/* Bonding a submission to another engine's: Gen11, and those Gen12
 * parts that kept the execlists interface (Tiger Lake, Rocket Lake,
 * Alder Lake-S).  The others submit in parallel instead. */
static int has_bonding(const struct i915_device *i915)
{
	int p = i915->info->platform;

	return i915->info->gen < 12 || p == I915_PLATFORM_TIGERLAKE ||
	       p == I915_PLATFORM_ROCKETLAKE || p == I915_PLATFORM_ALDERLAKE_S;
}

enum { SLOT_INVALID, SLOT_PHYSICAL, SLOT_BALANCED };

/* A load balancer: an empty slot becomes a set of siblings of one class.
 * One sibling is that engine itself; more are a virtual engine, which
 * this driver runs on the first of them. */
static long engines_ext_balance(struct i915_device *i915, uint64_t ext, uint32_t n,
				struct i915_engine **map, uint8_t *type)
{
	struct i915_context_engines_load_balance lb;
	struct i915_engine_class_instance ci;
	struct i915_engine *first = NULL;
	uint32_t seen = 0;

	if (copy_user_bounded(&lb, ext, sizeof(lb)))
		return -EFAULT;
	if (lb.engine_index >= n)
		return -EINVAL;
	if (type[lb.engine_index] != SLOT_INVALID)
		return -EEXIST;
	if (lb.flags || lb.mbz64)
		return -EINVAL;
	if (lb.num_siblings == 0)
		return 0;
	if (lb.num_siblings > I915_NUM_ENGINES)
		return -EINVAL;
	for (uint32_t k = 0; k < lb.num_siblings; k++) {
		if (copy_user_bounded(&ci, ext + sizeof(lb) + k * sizeof(ci), sizeof(ci)))
			return -EFAULT;
		struct i915_engine *se = i915_engine_lookup_user(i915, ci.engine_class,
								 ci.engine_instance);
		if (!se)
			return -EINVAL;
		if (first && se->class != first->class)
			return -EINVAL;
		/* a sibling named twice */
		if (seen & (1u << se->id))
			return -EINVAL;
		seen |= 1u << se->id;
		if (!first)
			first = se;
	}
	map[lb.engine_index] = first;
	type[lb.engine_index] = lb.num_siblings == 1 ? SLOT_PHYSICAL : SLOT_BALANCED;
	return 0;
}

/* A bond: which engine a submission on a physical slot pairs with when
 * another one's starts.  Checked, and remembered by nothing -- the
 * submission order alone pairs them here. */
static long engines_ext_bond(struct i915_device *i915, uint64_t ext, uint32_t n,
			     const uint8_t *type)
{
	struct i915_context_engines_bond b;
	struct i915_engine_class_instance ci;

	if (!has_bonding(i915))
		return -ENODEV;
	if (copy_user_bounded(&b, ext, sizeof(b)))
		return -EFAULT;
	if (b.virtual_index >= n || type[b.virtual_index] != SLOT_PHYSICAL)
		return -EINVAL;
	if (b.flags || b.mbz64[0] || b.mbz64[1] || b.mbz64[2] || b.mbz64[3])
		return -EINVAL;
	if (!i915_engine_lookup_user(i915, b.master.engine_class, b.master.engine_instance))
		return -EINVAL;
	for (uint32_t k = 0; k < b.num_bonds; k++) {
		if (copy_user_bounded(&ci, ext + sizeof(b) + k * sizeof(ci), sizeof(ci)))
			return -EFAULT;
		if (!i915_engine_lookup_user(i915, ci.engine_class, ci.engine_instance))
			return -EINVAL;
	}
	return 0;
}

/* The engine map: a list of (class, instance), in the client's
 * numbering, the indices of a submission refer to; an (invalid, none)
 * entry is a hole an extension may fill.  Parallel submission -- one
 * submission running a batch on each of several engines at once -- is
 * not offered: the extension asking for it is refused as a part without
 * the firmware interface for it refuses it. */
static long ctx_set_engines(struct i915_device *i915, struct i915_gem_context *ctx,
			    struct drm_i915_gem_context_param *p)
{
	if (p->size == 0) {
		/* back to the default map */
		i915_context_default_engines(ctx);
		return 0;
	}
	if (p->size < 8 || (p->size - 8) % 4)
		return -EINVAL;
	uint32_t n = (p->size - 8) / 4;
	if (n > I915_EXEC_RING_MASK + 1 || n > I915_CTX_LRC_SLOTS)
		return -EINVAL;
	uint8_t buf[8 + I915_CTX_LRC_SLOTS * 4];
	if (copy_user_bounded(buf, p->value, p->size))
		return -EFAULT;
	struct i915_engine *map[I915_CTX_LRC_SLOTS];
	uint8_t type[I915_CTX_LRC_SLOTS];
	for (uint32_t i = 0; i < n; i++) {
		uint16_t cls = (uint16_t)(buf[8 + i * 4] | (buf[9 + i * 4] << 8));
		uint16_t inst = (uint16_t)(buf[10 + i * 4] | (buf[11 + i * 4] << 8));
		if (cls == (uint16_t)I915_ENGINE_CLASS_INVALID &&
		    inst == (uint16_t)I915_ENGINE_CLASS_INVALID_NONE) {
			map[i] = NULL;
			type[i] = SLOT_INVALID;
			continue;
		}
		map[i] = i915_engine_lookup_user(i915, cls, inst);
		if (!map[i])
			return -ENOENT;
		type[i] = SLOT_PHYSICAL;
	}
	uint64_t ext;
	mm_memcpy(&ext, buf, sizeof(ext));
	for (int depth = 0; ext; depth++) {
		struct i915_user_extension ue;
		long rc;
		if (depth >= USER_EXT_DEPTH)
			return -E2BIG;
		if (copy_user_bounded(&ue, ext, sizeof(ue)))
			return -EFAULT;
		if ((rc = user_ext_mbz(&ue)))
			return rc;
		switch (ue.name) {
		case I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE:
			rc = engines_ext_balance(i915, ext, n, map, type);
			break;
		case I915_CONTEXT_ENGINES_EXT_BOND:
			rc = engines_ext_bond(i915, ext, n, type);
			break;
		case I915_CONTEXT_ENGINES_EXT_PARALLEL_SUBMIT:
			rc = -ENODEV;
			break;
		default:
			rc = -EINVAL;
			break;
		}
		if (rc)
			return rc;
		ext = ue.next_extension;
	}
	for (uint32_t i = 0; i < n; i++)
		ctx->engines[i] = map[i];
	ctx->nengines = (int)n;
	ctx->explicit_engines = 1;
	return 0;
}

/* The engine an SSEU request names: by its index in an explicit map, or
 * by (class, instance) with the default one -- the one way only. */
static struct i915_engine *sseu_engine(struct i915_gem_context *ctx,
				       const struct drm_i915_gem_context_param_sseu *s)
{
	int by_index = !!(s->flags & I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX);

	if (by_index != !!ctx->explicit_engines)
		return NULL;
	if (!by_index)
		return i915_engine_lookup_user(ctx->i915, s->engine.engine_class,
					       s->engine.engine_instance);
	if (s->engine.engine_instance >= (unsigned)ctx->nengines)
		return NULL;
	return ctx->engines[s->engine.engine_instance];
}

static unsigned hweight8(uint64_t v)
{
	unsigned n = 0;
	for (v &= 0xff; v; v &= v - 1)
		n++;
	return n;
}

/* The device's own configuration, what every engine runs with: the
 * slices, the first slice's subslices, the units of a subslice. */
static void sseu_device(const struct i915_device *i915, uint8_t *slices, uint8_t *subslices,
			uint16_t *eus)
{
	*slices = (uint8_t)i915->slice_mask;
	*subslices = (uint8_t)i915->subslice_mask[0];
	*eus = i915->eus_per_subslice ? (uint16_t)i915->eus_per_subslice : 8;
}

/* Only Gen11 lets a context power down part of the render engine, and
 * then only as far as the media codecs' kernels want: all slices or
 * one, half the subslices or all, the units untouched.  A request that
 * passes is accepted; the configuration that runs stays the fused one. */
static long ctx_set_sseu(struct i915_device *i915, struct i915_gem_context *ctx,
			 struct drm_i915_gem_context_param *p)
{
	struct drm_i915_gem_context_param_sseu s;
	uint8_t dev_s, dev_ss;
	uint16_t dev_eus;

	if (p->size < sizeof(s))
		return -EINVAL;
	if (i915->info->gen != 11)
		return -ENODEV;
	if (copy_user_bounded(&s, p->value, sizeof(s)))
		return -EFAULT;
	if (s.rsvd || (s.flags & ~I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX))
		return -EINVAL;
	struct i915_engine *e = sseu_engine(ctx, &s);
	if (!e)
		return -EINVAL;
	if (e->class != I915_ENGINE_CLASS_RENDER)
		return -ENODEV;
	sseu_device(i915, &dev_s, &dev_ss, &dev_eus);
	if (!s.slice_mask || !s.subslice_mask || !s.min_eus_per_subslice ||
	    !s.max_eus_per_subslice || s.max_eus_per_subslice < s.min_eus_per_subslice)
		return -EINVAL;
	if (s.slice_mask > 0xff || s.subslice_mask > 0xff ||
	    s.min_eus_per_subslice > 0xff || s.max_eus_per_subslice > 0xff)
		return -EINVAL;
	if ((s.slice_mask & ~(uint64_t)dev_s) || (s.subslice_mask & ~(uint64_t)dev_ss) ||
	    s.max_eus_per_subslice > dev_eus)
		return -EINVAL;
	unsigned hw_s = hweight8(dev_s), hw_ss = hweight8(dev_ss);
	unsigned req_s = hweight8(s.slice_mask), req_ss = hweight8(s.subslice_mask);
	if (req_s > 1 && req_ss != hw_ss)
		return -EINVAL;
	if (req_ss > 4 && (req_ss & 1))
		return -EINVAL;
	if (req_s == 1 && req_ss < hw_ss && req_ss > hw_ss / 2)
		return -EINVAL;
	if (req_s != 1 && req_s != hw_s)
		return -EINVAL;
	if (req_s == 1 && req_ss != hw_ss && req_ss != hw_ss / 2)
		return -EINVAL;
	if (s.min_eus_per_subslice != dev_eus || s.max_eus_per_subslice != dev_eus)
		return -EINVAL;
	return 0;
}

static long ctx_get_sseu(struct i915_gem_context *ctx, struct drm_i915_gem_context_param *p)
{
	struct drm_i915_gem_context_param_sseu s;
	uint8_t dev_s, dev_ss;
	uint16_t dev_eus;

	if (p->size == 0) {
		p->size = sizeof(s);
		return 0;
	}
	if (p->size < sizeof(s))
		return -EINVAL;
	if (copy_user_bounded(&s, p->value, sizeof(s)))
		return -EFAULT;
	if (s.rsvd || (s.flags & ~I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX))
		return -EINVAL;
	if (!sseu_engine(ctx, &s))
		return -EINVAL;
	sseu_device(ctx->i915, &dev_s, &dev_ss, &dev_eus);
	s.slice_mask = dev_s;
	s.subslice_mask = dev_ss;
	s.min_eus_per_subslice = dev_eus;
	s.max_eus_per_subslice = dev_eus;
	if (copy_to_user((void *)(uintptr_t)p->value, &s, sizeof(s)))
		return -EFAULT;
	p->size = sizeof(s);
	return 0;
}

/* The space and the engine map change only before the context has run
 * anywhere; once it has, they are fixed. */
static int ctx_has_run(const struct i915_gem_context *ctx)
{
	for (int i = 0; i < I915_CTX_LRC_SLOTS; i++)
		if (ctx->lrc[i].obj)
			return 1;
	return 0;
}

static long ctx_setparam(struct i915_device *i915, struct drm_file *fp,
			 struct i915_gem_context *ctx, struct drm_i915_gem_context_param *p)
{
	switch (p->param) {
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
		/* nothing is captured anyway */
		return p->size ? -EINVAL : 0;
	case I915_CONTEXT_PARAM_BANNABLE:
		if (p->size)
			return -EINVAL;
		if (!p->value && !capable())
			return -EPERM;
		ctx->bannable = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_RECOVERABLE:
		if (p->size)
			return -EINVAL;
		ctx->recoverable = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_PERSISTENCE:
		if (p->size)
			return -EINVAL;
		ctx->persistent = !!p->value;
		return 0;
	case I915_CONTEXT_PARAM_PRIORITY: {
		int64_t prio = (int64_t)p->value;
		if (p->size)
			return -EINVAL;
		if (prio < I915_CONTEXT_MIN_USER_PRIORITY || prio > I915_CONTEXT_MAX_USER_PRIORITY)
			return -EINVAL;
		/* above the default only for the privileged */
		if (prio > I915_CONTEXT_DEFAULT_PRIORITY && !capable())
			return -EPERM;
		ctx->priority = (int)prio;
		return 0;
	}
	case I915_CONTEXT_PARAM_VM: {
		/* before Broadwell every context shares one space */
		if (i915_is_legacy(i915))
			return -ENODEV;
		if (p->size)
			return -EINVAL;
		if (p->value >> 32)
			return -ENOENT;
		struct i915_vm *vm = vm_lookup(fp, (uint32_t)p->value);
		if (!vm)
			return -ENOENT;
		if (ctx_has_run(ctx)) {
			i915_vm_put(vm);
			return -EBUSY;
		}
		i915_vm_put(ctx->vm);
		ctx->vm = vm; /* the lookup's reference */
		return 0;
	}
	case I915_CONTEXT_PARAM_ENGINES:
		if (ctx_has_run(ctx))
			return -EBUSY;
		return ctx_set_engines(i915, ctx, p);
	case I915_CONTEXT_PARAM_SSEU:
		return ctx_set_sseu(i915, ctx, p);
	case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
		/* no protected content here: asking for none is fine */
		return p->value ? -ENODEV : 0;
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
	case I915_CONTEXT_PARAM_BAN_PERIOD:
	case I915_CONTEXT_PARAM_RINGSIZE:
	case I915_CONTEXT_PARAM_GTT_SIZE:
	case I915_CONTEXT_PARAM_LOW_LATENCY:
	case I915_CONTEXT_PARAM_CONTEXT_IMAGE:
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
		p->value = i915_vm_total(&g_i915);
		return 0;
	case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
	case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
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
		/* the context's space becomes a client-visible one; the one
		 * space every context shares before Broadwell cannot */
		if (i915_is_legacy(ctx->i915))
			return -ENODEV;
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
	case I915_CONTEXT_PARAM_SSEU:
		return ctx_get_sseu(ctx, p);
	/* the engine map is the client's own: it is not read back */
	case I915_CONTEXT_PARAM_ENGINES:
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
	case I915_CONTEXT_PARAM_BAN_PERIOD:
	case I915_CONTEXT_PARAM_RINGSIZE:
	case I915_CONTEXT_PARAM_CONTEXT_IMAGE:
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
				/* each a parameter set on the new context (which
				 * has no id yet to name) */
				uint64_t next = a->extensions;
				for (int n = 0; next; n++) {
					struct drm_i915_gem_context_create_ext_setparam sp;
					long rc = 0;
					if (n >= USER_EXT_DEPTH)
						rc = -E2BIG;
					else if (copy_user_bounded(&sp, next, sizeof(sp)))
						rc = -EFAULT;
					else if ((rc = user_ext_mbz(&sp.base)) == 0 &&
						 (sp.base.name != I915_CONTEXT_CREATE_EXT_SETPARAM ||
						  sp.param.ctx_id))
						rc = -EINVAL;
					if (!rc)
						rc = ctx_setparam(i915, fp, ctx, &sp.param);
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
		if (i915_is_legacy(i915))
			return -ENODEV;
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
		if (a->flags || a->pad)
			return -EINVAL;
		struct i915_gem_context *ctx = i915_file_context(fp, a->ctx_id);
		if (!ctx)
			return -ENOENT;
		/* the device-wide count is the privileged's to see */
		a->reset_count = capable() ? i915->gpu_reset_count : 0;
		a->batch_active = ctx->batch_active;
		a->batch_pending = ctx->batch_pending;
		i915_context_put(ctx);
		return 0;
	}
	case DRM_I915_REG_READ: {
		struct drm_i915_reg_read *a = kb;
		uint64_t off = a->offset & ~0x7ULL;
		uint64_t flags = a->offset & 0x7;
		/* The render engine's timestamp only, a 64-bit register from
		 * Gen4 on: read whole, or (the workaround flag) as two halves,
		 * the upper read again until it held still across the lower. */
		if (off != RING_TIMESTAMP(RENDER_RING_BASE) || i915->info->gen < 4)
			return -EINVAL;
		if (flags == I915_REG_READ_8B_WA) {
			uint32_t hi = i915_read32(i915, RING_TIMESTAMP_UDW(RENDER_RING_BASE)), lo, old;
			int loop = 0;
			do {
				old = hi;
				lo = i915_read32(i915, (uint32_t)off);
				hi = i915_read32(i915, RING_TIMESTAMP_UDW(RENDER_RING_BASE));
			} while (hi != old && loop++ < 2);
			a->val = ((uint64_t)hi << 32) | lo;
		} else if (flags == 0) {
			a->val = i915_read64(i915, (uint32_t)off);
		} else {
			return -EINVAL;
		}
		return 0;
	}
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
