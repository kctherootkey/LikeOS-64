// LikeOS -- the GT of the Intel graphics generations before Broadwell.
//
// Generations 2 to 7.5 (i830 to Haswell, and Valleyview) run their
// engines from one ring buffer each instead of execution lists, address
// memory through the global GTT (and on Gen6/7 one PPGTT that aliases
// it), take forcewake differently or not at all, and raise interrupts
// through registers the Gen8 handler does not know.  The driver's common
// files (probe, registers, the GTT, engines, requests, interrupts,
// fences, submission) call these functions where the generation is below
// 8; everything they call works on the same structures the Gen8 code
// does -- struct i915_engine, i915_request, i915_vm, i915_bo -- so
// execbuffer, waits, busy queries and fences behave the same.
//
// Unless a comment says otherwise, a function here is only called for a
// part whose descriptor says generation 2 to 7.  The interrupt entry
// points also serve Cherryview (a Gen8 GT under Valleyview's display
// interrupts).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_LEGACY_H
#define KERNEL_DEV_GPU_I915_LEGACY_H

#include <kernel/dev/gpu/i915/i915_drv.h>

/* Whether the common code hands a device to these functions. */
static inline int i915_is_legacy(const struct i915_device *i915)
{
	return i915->info->gen < 8;
}

/* Interrupts: every legacy part, and Cherryview. */
static inline int i915_legacy_irqs(const struct i915_device *i915)
{
	return i915->info->gen < 8 || i915->info->platform == I915_PLATFORM_CHERRYVIEW;
}

/* ---- registers (i915_legacy_uncore.c) ------------------------------------ */

/* The BARs as each generation lays them out: gen2's registers are BAR1
 * (BAR0 is the aperture), gen3's are BAR0 with the aperture in BAR2 and
 * the GTT in BAR3, gen4 on share BAR0 between registers and GTT.  Fills
 * bar_mmio, bar_aperture, bar_io, mmio_size (512 KB to gen4, 2 MB from
 * Ironlake) and maps mmio_virt.  Replaces the probe's own BAR code. */
int i915_legacy_mmio_map(struct i915_device *i915);
/* Forcewake for Gen6/7 (one render domain; Valleyview render and media;
 * Ivy Bridge picks the multi-threaded register when the firmware enabled
 * it), the GT FIFO, the unclaimed-access detectors.  Gen2-5 have no
 * forcewake: fw_present stays 0.  Replaces i915_uncore_init. */
int i915_legacy_uncore_init(struct i915_device *i915);
void i915_legacy_uncore_fini(struct i915_device *i915);
/* The domains an access to `reg' needs: Gen6/7 reads take the render
 * domain below 0x40000 (Valleyview: its range table); writes need none
 * (they go through the GT FIFO instead). */
uint32_t i915_legacy_fw_domains_for(struct i915_device *i915, uint32_t reg, int write);
/* One domain woken / let sleep, the whole sequence the part wants (the
 * FIFO error check, the old acknowledge gone, the request, the new
 * acknowledge, the GT thread out of C6).  Called with fw_lock held in
 * place of the set-and-wait pair. */
void i915_legacy_fw_wake(struct i915_device *i915, struct i915_fw_domain *d);
void i915_legacy_fw_sleep(struct i915_device *i915, struct i915_fw_domain *d);
/* Before every register access through i915_read32/i915_write32 (with
 * fw_lock held, or from the lock-free path): Ironlake's dummy write that
 * wakes it from render standby, Gen6/7's wait for a free GT FIFO entry
 * ahead of a write. */
void i915_legacy_mmio_read_prepare(struct i915_device *i915, uint32_t reg);
void i915_legacy_mmio_write_prepare(struct i915_device *i915, uint32_t reg);
/* The unclaimed-access detectors of Haswell (FPGA_DBG) and Valleyview
 * (CLAIM_ER): one line, then silence. */
void i915_legacy_check_unclaimed(struct i915_device *i915, uint32_t reg);
/* The command streamer's timestamp rate. */
uint32_t i915_legacy_timestamp_hz(struct i915_device *i915);
/* Slices, subslices and EUs where the part says (Haswell's GT level). */
void i915_legacy_read_topology(struct i915_device *i915);
/* Valleyview's and Cherryview's IOSF sideband: one message to any unit
 * (devfn, port, opcode as the hardware takes them; *val written for a
 * write opcode, filled in for a read).  The one implementation, with one
 * lock, for every part of the driver -- the display's clocks and PHYs
 * included -- and usable on Cherryview, whose GT is not this backend's.
 * 0, -EAGAIN (the doorbell stayed busy) or -ETIMEDOUT. */
int i915_legacy_vlv_sideband(struct i915_device *i915, uint32_t devfn, uint32_t port,
			     uint32_t opcode, uint32_t addr, uint32_t *val);
/* The P-unit and the north cluster through it. */
int i915_legacy_vlv_punit_read(struct i915_device *i915, uint32_t addr, uint32_t *val);
int i915_legacy_vlv_punit_write(struct i915_device *i915, uint32_t addr, uint32_t val);
int i915_legacy_vlv_nc_read(struct i915_device *i915, uint32_t addr, uint32_t *val);

/* ---- the global GTT and the shared address space (i915_legacy_gtt.c) ---- */

/* Stolen memory (per chipset: the host bridge's GMCH_CTRL and memory
 * size registers on gen2, BSM and GMCH_CTRL on gen3-5, BSM and
 * SNB_GMCH_CTRL on Gen6/7), the GTT's size and location, its entry
 * format, the scratch page, what the firmware is scanning out.  Sets up
 * i915->ggtt_map itself.  Replaces i915_gtt_probe / i915_gtt_fini. */
int i915_legacy_gtt_probe(struct i915_device *i915);
void i915_legacy_gtt_fini(struct i915_device *i915);
/* The global GTT bindings of i915_gtt.c, for these parts (4-byte
 * entries, their cache bits, the chipset flush).  Same contracts as
 * i915_ggtt_bind_obj / _mappable / i915_ggtt_unbind / rewrite_all. */
int i915_legacy_ggtt_bind_obj(struct i915_device *i915, struct drm_gem_object *o,
			      int uncached, uint32_t *ggtt_offset);
int i915_legacy_ggtt_bind_obj_mappable(struct i915_device *i915, struct drm_gem_object *o,
				       uint32_t *ggtt_offset);
void i915_legacy_ggtt_unbind(struct i915_device *i915, uint32_t ggtt_offset, uint32_t size);
void i915_legacy_ggtt_rewrite_all(struct i915_device *i915);
/* What GET_APERTURE should report as available. */
uint64_t i915_legacy_aperture_available(struct i915_device *i915);
/* Write buffers of the chipset drained to memory (gen2-5): after the
 * processor flushed what the device is about to read. */
void i915_legacy_chipset_flush(struct i915_device *i915);

/* The address space contexts run in.  There is one, shared: on gen2-5 it
 * is the global GTT itself, on Gen6/7 the aliasing PPGTT (bindings go to
 * the PPGTT only; their addresses are reserved in the global space so
 * they never collide with the driver's own global bindings).  The
 * i915_vm functions of i915_ppgtt.c dispatch here: create returns the
 * shared space (one more user of it), bind/unbind/lookup write and read its
 * entries, vma_alloc finds an address the global space has free. */
struct i915_vm *i915_legacy_vm_create(struct i915_device *i915);
int i915_legacy_vm_bind(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
			uint32_t npages, int uncached);
void i915_legacy_vm_unbind(struct i915_vm *vm, uint64_t addr, uint32_t npages);
uint64_t i915_legacy_vm_lookup(struct i915_vm *vm, uint64_t addr);
int i915_legacy_vm_vma_alloc(struct i915_vm *vm, struct i915_vma *v, uint32_t npages,
			     uint64_t align, int low);
/* What a submission must reserve for an object in the shared space
 * (ex_flags: the object's EXEC_OBJECT_* flags): gen2/3 bind a tiled
 * object the engine reaches through a fence at the fence's power-of-two
 * size and alignment, behind the aperture (*mappable: pass low = 2 to
 * i915_vm_vma_alloc); everything else is its own size, anywhere.
 * *align is only ever raised. */
void i915_legacy_exec_layout(struct i915_device *i915, struct drm_gem_object *o,
			     uint64_t ex_flags, uint32_t *npages, uint64_t *align, int *mappable);
/* An existing binding that no longer suits the layout above (the object
 * was tiled after it was bound): the submission releases and rebinds. */
int i915_legacy_exec_misplaced(struct i915_device *i915, struct drm_gem_object *o,
			       const struct i915_vma *v, uint64_t ex_flags);
/* Gen2/3: the object (bound at `addr' in the shared space) gets a fence
 * register for the engine's tiled access (EXEC_OBJECT_NEEDS_FENCE; an
 * untiled object loses a stale one).  May take back an idle object's
 * fence; -EAGAIN when every fence is busy, -EBUSY when the object's
 * fence is pinned at another address. */
int i915_legacy_exec_fence(struct i915_device *i915, struct drm_gem_object *o, uint64_t addr);
/* Gen2/3: a tiled object that needs its fence (EXEC_OBJECT_NEEDS_FENCE)
 * and already has a fenced global binding -- a scanout surface, or the
 * binding its aperture mapping uses -- is reached there by the engine
 * too, so that one fence register serves the plane, the mapping and the
 * submission.  1 and *addr when so; 0 otherwise. */
int i915_legacy_exec_shared_binding(struct i915_device *i915, struct drm_gem_object *o,
				    uint64_t ex_flags, uint64_t *addr);

/* ---- fences and tiling (i915_legacy_fence.c) ----------------------------- */

/* The fence registers of gen2 (8, 512 KB granular), gen3 (8 or 16,
 * power-of-two regions from 1 MB), gen4/5 (16, page granular, 64 bit)
 * and Gen6/7 (16 or 32).  i915_fence.c's init/restore dispatch here; its
 * mapping path calls map_in/map_out (with fence_lock held) in place of
 * its own binding and fence code. */
int i915_legacy_fences_init(struct i915_device *i915);
void i915_legacy_fences_restore(struct i915_device *i915);
int i915_legacy_gtt_map_in(struct i915_device *i915, struct drm_gem_object *o, uint32_t *base);
void i915_legacy_gtt_map_out(struct i915_device *i915, struct drm_gem_object *o);
/* A tiled scanout surface on gen2/3 is read through a fence: the display
 * pins one over its binding while it scans out, and lets go after. */
int i915_legacy_fence_pin(struct i915_device *i915, struct drm_gem_object *o, uint32_t ggtt);
void i915_legacy_fence_unpin(struct i915_device *i915, struct drm_gem_object *o);
/* The binding such a surface needs: fence-aligned and fence-sized,
 * behind the aperture; unbound with i915_ggtt_unbind(ggtt,
 * i915_legacy_fence_size(o)). */
int i915_legacy_ggtt_bind_fenced(struct i915_device *i915, struct drm_gem_object *o, uint32_t *ggtt);
uint32_t i915_legacy_fence_size(struct i915_device *i915, struct drm_gem_object *o);
/* SET_TILING / GET_TILING: whether the stride is legal, and the bit-6
 * swizzle the client must apply (the second is what the hardware does;
 * the first hides bit-17 swizzling, which this driver's pinned pages
 * never need to undo). */
int i915_legacy_tiling_ok(struct i915_device *i915, uint32_t tiling, uint32_t stride);
void i915_legacy_swizzle(struct i915_device *i915, uint32_t tiling, uint32_t *swizzle,
			 uint32_t *phys_swizzle);
int i915_legacy_num_fences(struct i915_device *i915);

/* ---- engines and requests (i915_legacy_ring.c) ---------------------------- */

/* Rings, status pages, the render context size, interrupts at the
 * engine, the GT's settings; replaces i915_engines_init/_fini/_resume. */
int i915_legacy_engines_init(struct i915_device *i915);
void i915_legacy_engines_fini(struct i915_device *i915);
int i915_legacy_engines_resume(struct i915_device *i915);
/* One batch per engine; the render engine runs the null render state
 * and its saved image becomes every context's starting point.
 * Replaces i915_gt_golden_init. */
int i915_legacy_gt_golden_init(struct i915_device *i915);
/* Dispatch flags of a batch: secure (privileged, global GTT) and pinned
 * (i830/845 run it in place instead of from a copy). */
#define I915_LEGACY_DISPATCH_SECURE (1u << 0)
#define I915_LEGACY_DISPATCH_PINNED (1u << 1)
/* The request into the engine's ring -- caches invalidated, the
 * context's address space and image switched to, the batch, the
 * breadcrumb -- and the tail moved.  Replaces i915_request_submit (the
 * request goes straight to the in-flight list). */
int i915_legacy_request_submit(struct i915_request *rq, uint64_t batch_addr,
			       uint32_t batch_len, unsigned dispatch_flags);
/* Hang recovery: the engine (Gen7) or the whole GPU (everything older)
 * reset, the guilty request skipped, the innocent ones run again.  Gen2
 * has no reset: the guilty request fails and the ring restarts after
 * it.  Replaces i915_engine_reset / i915_gt_reset_all. */
int i915_legacy_engine_reset(struct i915_engine *e, const char *why);
int i915_legacy_gt_reset_all(struct i915_device *i915);
/* The engine's active head, for the hang check (gen2/3 keep it
 * elsewhere). */
uint32_t i915_legacy_engine_acthd(struct i915_engine *e);
/* Retirement for these engines: i915_engine_retire, then the contexts the
 * engine has since saved are let go of.  In place of the execution-list
 * status processing (there is none) in the fence poll and the GT
 * worker. */
void i915_legacy_engine_retire(struct i915_engine *e);

/* ---- interrupts (i915_legacy_irq.c) ---------------------------------------- */

/* gen2/3 (IIR), gen4 (IIR with the BSD bit), Ironlake to Haswell (the
 * display master over GT and PM), Valleyview and Cherryview.  MSI from
 * Ironlake on, the legacy line before (MSI is broken on those chipsets).
 * Replaces i915_irq_init/_fini/_suspend/_resume for i915_legacy_irqs(). */
int i915_legacy_irq_init(struct i915_device *i915);
void i915_legacy_irq_fini(struct i915_device *i915);
void i915_legacy_irq_suspend(struct i915_device *i915);
void i915_legacy_irq_resume(struct i915_device *i915);
int i915_legacy_irq_handler(struct i915_device *i915);

/* ---- power (i915_legacy_rps.c) --------------------------------------------- */

/* Render P-states (Gen6/7 by the up/down interrupts, Valleyview at its
 * highest frequency through the P-unit) and render standby (RC6) where
 * the part enables it by default.  Replaces i915_rps_init. */
int i915_legacy_rps_init(struct i915_device *i915);
void i915_legacy_rps_fini(struct i915_device *i915);
uint32_t i915_legacy_rps_current_mhz(struct i915_device *i915);

/* ---- the interface ------------------------------------------------------- */

/* GETPARAM values these parts answer differently (fences, PPGTT kind,
 * LLC, write-through, the timestamp).  Returns 1 and sets *value when it
 * has an answer, 0 to leave the parameter to the common table. */
int i915_legacy_getparam(struct i915_device *i915, int param, int *value);

/* ---- the null render state (i915_legacy_renderstate.c) ------------------- */

uint32_t i915_legacy_renderstate_bytes(int gen);
int i915_legacy_renderstate_build(int gen, uint32_t *page, uint32_t page_bytes, uint32_t base);

#endif
