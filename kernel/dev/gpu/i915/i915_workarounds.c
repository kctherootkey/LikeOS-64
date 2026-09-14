// LikeOS-64 -- the settings this generation needs before it renders.
//
// Silicon ships with behaviour its own documentation calls wrong: a
// cache that must not prefetch, a sampler that must not be powered
// down mid-fetch, an optimisation that corrupts a resolve.  Each one
// has a bit somewhere that turns it off, and the driver is expected to
// set every one of them before the first picture.  Without them the
// part still runs -- simple work comes out right, and anything that
// leans on the samplers or the caches comes out wrong or not at all.
//
// They come in two kinds.  Some belong to the engine and are written
// once as registers.  Others belong to a CONTEXT: the engine saves and
// restores them around every switch, so they have to be in the image a
// context starts from, which is why they are emitted as register loads
// into the batch whose saved state becomes that image.
//
// A "chicken" register carries a write mask in its upper half: a bit is
// only changed where the matching mask bit is set, so two drivers can
// own different bits of one register.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

static void wa_set(struct i915_device *i915, uint32_t reg, uint32_t bits)
{
	i915_write32(i915, reg, i915_read32(i915, reg) | bits);
}

/* Some registers exist once per slice or subslice.  A write reaches
 * every copy; a read returns the copy the selector names, and a copy
 * that is fused off or powered down answers with nothing.  So a read
 * is steered at the first slice and subslice the part has, and the
 * selector put back afterwards. */
static uint32_t mcr_read(struct i915_device *i915, uint32_t reg)
{
	uint32_t sel = i915_read32(i915, GEN8_MCR_SELECTOR);
	uint32_t ss = 0, slice = 0;

	for (; slice < 4 && !(i915->slice_mask & (1u << slice)); slice++)
		;
	if (slice == 4)
		slice = 0;
	for (; ss < 4 && !(i915->subslice_mask[slice] & (1u << ss)); ss++)
		;
	if (ss == 4)
		ss = 0;
	i915_write32(i915, GEN8_MCR_SELECTOR,
		     (sel & ~(GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK)) |
			     GEN8_MCR_SLICE(slice) | GEN8_MCR_SUBSLICE(ss));
	uint32_t v = i915_read32(i915, reg);
	i915_write32(i915, GEN8_MCR_SELECTOR, sel);
	return v;
}

/* set `set', clear `clear', in a register with one copy per slice */
static void wa_mcr_set_clear(struct i915_device *i915, uint32_t reg, uint32_t set,
			     uint32_t clear)
{
	uint32_t v = (mcr_read(i915, reg) | set) & ~clear;
	i915_write32(i915, reg, v);
}

static void wa_masked_set(struct i915_device *i915, uint32_t reg, uint32_t bits)
{
	i915_write32(i915, reg, I915_MASKED_ENABLE(bits));
}

/* The registers a client's batch is allowed to write.  The graphics
 * library sets a few of these itself as it builds the pipeline; a write
 * to one that is not listed is dropped by the engine, silently, and
 * whatever the register controls keeps its reset value. */
static void whitelist_build(struct i915_device *i915, struct i915_engine *e)
{
	static const uint32_t render_whitelist[] = {
		0x2248, /* the preemption register */
		0x2580, /* the command-streamer chicken bits */
		GEN8_HDC_CHICKEN1,
		COMMON_SLICE_CHICKEN2,
		GEN8_L3SQCREG4,
	};
	uint32_t n = 0;

	if (e->class == 0) { /* render */
		for (; n < sizeof(render_whitelist) / sizeof(render_whitelist[0]) &&
		       n < RING_FORCE_TO_NONPRIV_COUNT;
		     n++) {
			i915_write32(i915, RING_FORCE_TO_NONPRIV(e->mmio_base, (int)n),
				     render_whitelist[n]);
		}
	}
	/* the rest of the list must not leave stale entries behind: point
	 * them at the engine's no-op id register, which a write cannot hurt */
	for (; n < RING_FORCE_TO_NONPRIV_COUNT; n++) {
		i915_write32(i915, RING_FORCE_TO_NONPRIV(e->mmio_base, (int)n),
			     e->mmio_base + 0x94);
	}
}

/* Settings that belong to the engine, not to a context. */
void i915_gt_apply_workarounds(struct i915_device *i915, struct i915_engine *e)
{
	if (i915->info->gen != 9)
		return;

	whitelist_build(i915, e);

	if (e->class != 0)
		return; /* the rest are the render engine's */

	/* a context switch must not run while a translation cache is being
	 * invalidated */
	wa_masked_set(i915, GEN9_CSFE_CHICKEN1_RCS(e->mmio_base),
		      GEN9_PREEMPT_GPGPU_SYNC_SWITCH_DISABLE);
	/* the arbiter's retry timer; and an eviction optimisation that is
	 * off on this generation */
	wa_mcr_set_clear(i915, BDW_SCRATCH1, GEN9_LBS_SLA_RETRY_TIMER_DECREMENT_ENABLE,
			 EVICTION_PERF_FIX_ENABLE);
	/* the L3 flushes the lines it keeps coherent; atomics stay out of
	 * the L3, where they have hung the part beyond recovery */
	wa_mcr_set_clear(i915, GEN8_L3SQCREG4, GEN8_LQSC_FLUSH_COHERENT_LINES,
			 GEN8_LQSQ_NONIA_COHERENT_ATOMICS_ENABLE);
	i915_write32(i915, GEN9_SCRATCH_LNCF1,
		     i915_read32(i915, GEN9_SCRATCH_LNCF1) &
			     ~(uint32_t)GEN9_LNCF_NONIA_COHERENT_ATOMICS_ENABLE);
	/* the translation cache and the data cache must not be invalidated
	 * behind the driver's back */
	wa_set(i915, GAM_ECOCHK, ECOCHK_DIS_TLB | BDW_DISABLE_HDC_INVALIDATION);
	/* the hashing the sampler and the display must agree on */
	if (i915->info->flags & I915_INFO_HAS_LLC)
		wa_set(i915, MMCD_MISC_CTRL, MMCD_PCLA | MMCD_HOTSPOT_EN);
	/* a clock gate that stops a unit mid-work */
	wa_set(i915, GEN7_UCGCTL4, GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE);
	/* decompression in place hangs on early steppings */
	wa_set(i915, GEN9_GAMT_ECO_REG_RW_IA, GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);
	/* the credit the memory arbiter gives the sampler */
	wa_set(i915, GEN8_GARBCNTL, GEN9_GAPS_TSV_CREDIT_DISABLE);
}

/* Settings the engine saves and restores with a context.  Emitted as
 * register loads into the batch whose saved state becomes the image
 * every later context starts from. */
struct wa_ctx_entry {
	uint32_t reg;
	uint32_t value; /* already masked where the register wants a mask */
};

/* The Gen9 list, as the part's documentation has it for the parts with
 * a shared last-level cache (Skylake, Kaby Lake, Coffee Lake) and
 * without (the low-power parts). */
static unsigned gen9_ctx_workarounds(struct i915_device *i915, struct wa_ctx_entry *wa,
				     unsigned max)
{
	int llc = !!(i915->info->flags & I915_INFO_HAS_LLC);
	int lp = !!(i915->info->flags & I915_INFO_IS_LP);
	unsigned n = 0;

	if (max < 12)
		return 0;
	if (llc) {
		/* compressed surfaces are hashed the way the display
		 * engine hashes them, by the pixel back end and the
		 * sampler both */
		wa[n].reg = COMMON_SLICE_CHICKEN2;
		wa[n++].value = I915_MASKED_ENABLE(GEN9_PBE_COMPRESSED_HASH_SELECTION);
	}
	wa[n].reg = GEN9_HALF_SLICE_CHICKEN7;
	wa[n++].value = I915_MASKED_ENABLE((llc ? GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR : 0) |
					   GEN9_ENABLE_YV12_BUGFIX |
					   GEN9_ENABLE_GPGPU_PREEMPTION);
	/* flow control state is cleared when a compute context is saved;
	 * an instruction shootdown that leaves a thread half-run is off */
	wa[n].reg = GEN8_ROW_CHICKEN;
	wa[n++].value = I915_MASKED_ENABLE(FLOW_CONTROL_ENABLE |
					   PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE);
	/* a resolve optimisation that corrupts the result */
	wa[n].reg = CACHE_MODE_1;
	wa[n++].value = I915_MASKED_ENABLE(GEN8_4x4_STC_OPTIMIZATION_DISABLE |
					   GEN9_PARTIAL_RESOLVE_IN_VC_DISABLE);
	/* the compression cache must not prefetch translations */
	wa[n].reg = GEN9_HALF_SLICE_CHICKEN5;
	wa[n++].value = I915_MASKED_DISABLE(GEN9_CCS_TLB_PREFETCH_ENABLE);
	/* The data port treats every access as non-coherent, the context
	 * save and restore included.  Documented as guarding against a
	 * hang when a translation cache invalidation lands while the
	 * pixel shader dispatcher is being flushed. */
	wa[n].reg = HDC_CHICKEN0;
	wa[n++].value = I915_MASKED_ENABLE(HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT |
					   HDC_FORCE_CSR_NON_COHERENT_OVR_DISABLE |
					   HDC_FORCE_NON_COHERENT);
	if (!lp) {
		/* the sampler must not power down between the two halves
		 * of a stream-output ping-pong */
		wa[n].reg = HALF_SLICE_CHICKEN3;
		wa[n++].value = I915_MASKED_ENABLE(GEN8_SAMPLER_POWER_BYPASS_DIS);
	}
	/* a unit that powers down while the pipeline still needs it */
	wa[n].reg = HALF_SLICE_CHICKEN2;
	wa[n++].value = I915_MASKED_ENABLE(GEN8_ST_PO_DISABLE);
	/* a batch may be stopped short only between commands: not in the
	 * middle of a primitive, nor of a compute thread group (a client
	 * that knows better may allow more; the register is on its list) */
	wa[n].reg = GEN8_CS_CHICKEN1(RENDER_RING_BASE);
	wa[n++].value = I915_MASKED_DISABLE(GEN9_PREEMPT_3D_OBJECT_LEVEL) |
			(GEN9_PREEMPT_GPGPU_LEVEL_MASK << 16) | GEN9_PREEMPT_GPGPU_COMMAND_LEVEL;
	return n;
}

/* Write the context settings into `ring' as register loads.  Returns the
 * number of dwords emitted. */
uint32_t i915_ctx_workarounds_emit(struct i915_device *i915, int engine_class,
				   uint32_t *ring, uint32_t max_dwords)
{
	struct wa_ctx_entry wa[12];
	unsigned count;
	uint32_t n = 0;

	if (i915->info->gen != 9 || engine_class != 0)
		return 0;
	count = gen9_ctx_workarounds(i915, wa, 12);
	if (!count || max_dwords < 2 + count * 2)
		return 0;
	ring[n++] = MI_LOAD_REGISTER_IMM((uint32_t)count);
	for (unsigned i = 0; i < count; i++) {
		ring[n++] = wa[i].reg;
		ring[n++] = wa[i].value;
	}
	if (n & 1)
		ring[n++] = MI_NOOP;
	return n;
}

/* Everything a render context must carry, as register loads for its ring.
 *
 * These registers are CONTEXT state: the engine saves them when a
 * context leaves and restores them when it comes back, so a value
 * written once through the register window lasts only until the next
 * context switch, and each context runs with whatever its own image
 * holds -- the part's defaults, for a context the driver never told.
 * The defaults are wrong for this driver: the cache-control table says
 * entry 2, which every surface a client draws into or samples from
 * names, skips the shared L3 on some accesses and not others, so one
 * unit reads what another has not written yet.  A picture drawn that
 * way is right at first and wrong once the cache has filled.
 *
 * So they go in ahead of every batch.  A ring is privileged where a
 * batch is not, which is what lets them be written at all, and the cost
 * is some eighty words of ring per submission. */
uint32_t i915_ctx_settings_emit(struct i915_device *i915, struct i915_engine *e,
				uint32_t *ring, uint32_t max_dwords)
{
	uint32_t n;

	if (i915->info->gen != 9 || e->class != 0)
		return 0;
	n = i915_ctx_workarounds_emit(i915, e->class, ring, max_dwords);
	n += i915_mocs_l3cc_emit(i915, e->class, ring + n, max_dwords - n);
	/* how much of the part the context asks to have powered */
	if (max_dwords - n >= 4) {
		ring[n++] = MI_LOAD_REGISTER_IMM(1);
		ring[n++] = GEN8_R_PWR_CLK_STATE;
		ring[n++] = i915_rpcs_for(i915);
		ring[n++] = MI_NOOP;
	}
	return n;
}

/* ---- what the engine could not reach ------------------------------------- */

/* A memory access the engine made and the translation tables did not
 * answer.  The engine drops the work that asked for it and carries on,
 * so the only sign is a picture missing whatever that work drew.  Said
 * once per fault; the register keeps the first until it is cleared. */
void i915_gt_check_faults(struct i915_device *i915)
{
	uint32_t fault;

	if (i915->info->gen < 8 || i915->info->gen >= 11)
		return;
	fault = i915_read32(i915, GEN8_RING_FAULT_REG);
	if (!(fault & RING_FAULT_VALID))
		return;
	uint32_t d0 = i915_read32(i915, GEN8_FAULT_TLB_DATA0);
	uint32_t d1 = i915_read32(i915, GEN8_FAULT_TLB_DATA1);
	uint64_t addr = ((uint64_t)(d1 & FAULT_VA_HIGH_BITS) << 44) | ((uint64_t)d0 << 12);
	i915->gt_faults++;
	kprintf("[drm] i915: the engine could not reach %llx in the %s (type %u, source %u, engine %u); fault %u\n",
		(unsigned long long)addr, (d1 & FAULT_GTT_SEL) ? "global address space" : "a client's address space",
		RING_FAULT_TYPE(fault), RING_FAULT_SRCID(fault),
		GEN8_RING_FAULT_ENGINE_ID(fault), (unsigned)i915->gt_faults);
	i915_write32(i915, GEN8_RING_FAULT_REG, fault & ~RING_FAULT_VALID);
}

/* ---- the batch every context restore runs ---------------------------------- */

/* One page in the GGTT per render engine: the batch at its start, the
 * scratch it keeps a register in at its end.  The context images name
 * it (i915_lrc_init_regs); the engine runs it, uninterruptible, in the
 * middle of restoring each context.  Gen9 only; other generations run
 * none and the image says so. */
int i915_wa_bb_init(struct i915_device *i915, struct i915_engine *e)
{
	if (e->wa_bb_obj)
		return 0;
	if (i915->info->gen != 9 || e->class != 0)
		return 0;
	uint32_t ggtt;
	struct drm_gem_object *o = i915_gem_alloc_bound(i915, 4096, 0, 1, &ggtt);
	if (!o) {
		kprintf("[drm] i915: %s: no memory for the restore batch\n", e->name);
		return -ENOMEM;
	}
	uint32_t *b = phys_to_virt(o->pages[0]);
	mm_memset(b, 0, 4096);
	uint32_t words = i915_lrc_wa_bb_gen9(b, ggtt + 2048);
	for (int i = 0; i < 4096; i += 64)
		__asm__ volatile("clflush (%0)" ::"r"((uint8_t *)b + i) : "memory");
	__asm__ volatile("mfence" ::: "memory");
	e->wa_bb_obj = o;
	e->wa_bb_ggtt = ggtt;
	e->wa_bb_bytes = words * 4;
	i915_dbg("[drm] i915: %s: restore batch of %u words at %08x\n", e->name, words, ggtt);
	return 0;
}

void i915_wa_bb_fini(struct i915_engine *e)
{
	if (e->wa_bb_obj) {
		drm_gem_put(e->wa_bb_obj);
		e->wa_bb_obj = NULL;
	}
	e->wa_bb_ggtt = e->wa_bb_bytes = 0;
}
