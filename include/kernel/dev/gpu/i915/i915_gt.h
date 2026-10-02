// LikeOS -- the GT side of the Intel graphics driver: engines, address
// spaces, contexts, requests.
//
// An engine (render, blitter, video...) executes work from a ring buffer
// that belongs to a context; the execution-list port takes context
// descriptors and the hardware switches between them, reporting through
// the context status buffer.  A request is one submission: a batch buffer
// in some address space, run by one engine under one context, whose
// completion is a sequence number the engine writes and a fence the
// driver signals.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_GT_H
#define KERNEL_DEV_GPU_I915_GT_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/waitq.h>
#include <kernel/dev/gpu/i915/i915_lrc_layout.h>

struct i915_device;
struct i915_gem_context;

/* ---- address spaces (i915_ppgtt.c) --------------------------------------- */

#define I915_VM_BITS 48
#define I915_VM_SIZE (1ULL << I915_VM_BITS)

struct i915_page_list {
	uint64_t *phys; /* pages handed to the tables, freed with the vm */
	uint32_t n, cap;
};

struct i915_vm {
	int refs;
	uint32_t id; /* per-file id (VM_CREATE), 0 = a context's own */
	uint64_t pml4; /* physical */
	/* Xe2 on: the walk has five levels; the top table's first entry
	 * leads to pml4 (the 48 bits a client has), the others to a
	 * scratch fourth level */
	uint64_t pml5, scratch_pml4;
	uint64_t scratch_page, scratch_pt, scratch_pd, scratch_pdp;
	struct i915_page_list tables;
	spinlock_t lock;
	/* where objects that ask for no address go (relocation clients):
	 * next-fit cursors, one for the zone below 4 GB an object without
	 * the 48-bit flag must stay in, one for the zone above it */
	uint64_t alloc_low, alloc_high;
	int tlb_dirty; /* bindings changed since the last submission */
	struct i915_vma *vmas; /* the bindings that own ranges here */
	/* a 32-bit space (Cherryview): the four page directories under
	 * the first entry of the top table, made with the space and kept
	 * for its life -- the context's PDP registers name them */
	uint64_t pd32[4];
};

struct i915_vm *i915_vm_create(struct i915_device *i915);
void i915_vm_get(struct i915_vm *vm);
void i915_vm_put(struct i915_vm *vm);
/* Map `npages' pages at graphics address `addr' (4 KB aligned) with the
 * cache attribute (0 = write-back through the LLC, 1 = uncached). */
int i915_vm_bind(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
		 uint32_t npages, int uncached);
/* The same with the page attribute table entry named outright. */
int i915_vm_bind_pat(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
		     uint32_t npages, uint32_t pat_index);
/* i915_gtt.c: the page attribute table entry the driver's tables give a
 * cached (write-back) or an uncached binding, and the reverse */
uint32_t i915_pat_index(struct i915_device *i915, int uncached);
int i915_pat_is_uncached(struct i915_device *i915, uint32_t pat_index);
uint64_t i915_vm_lookup(struct i915_vm *vm, uint64_t addr);
/* What a context's image names as its page table base: the top table's
 * address (Xe2 on: as a directory entry, with its access and page
 * attribute bits). */
uint64_t i915_vm_root(struct i915_vm *vm);
/* i915_gtt.c: Xe2 on, the page attribute table and its entries */
const uint32_t *i915_xe2_pat_table(const struct i915_device *i915, unsigned *n);
int i915_pat_index_valid(const struct i915_device *i915, uint32_t idx);
int i915_pat_compressed(const struct i915_device *i915, uint32_t idx);
void i915_vm_unbind(struct i915_vm *vm, uint64_t addr, uint32_t npages);

/* ---- engines (i915_engine.c) ------------------------------------------------ */

enum i915_engine_id {
	I915_RCS0 = 0,
	I915_BCS0,
	I915_VCS0,
	I915_VCS1,
	I915_VCS2,
	I915_VCS3,
	I915_VECS0,
	I915_VECS1,
	I915_CCS0,
	I915_CCS1,
	I915_CCS2,
	I915_CCS3,
	I915_NUM_ENGINES
};

#define I915_RING_SIZE (32 * 1024)

/* A ring: a GEM object in the GGTT the CPU writes commands into. */
struct i915_ring {
	struct drm_gem_object *obj;
	uint32_t ggtt;
	uint32_t size;
	uint32_t tail; /* the CPU's write position */
	uint32_t head; /* last known hardware read position */
	uint8_t *vaddr; /* kernel mapping (the pages are contiguous) */
};

struct i915_request {
	struct i915_request *next; /* engine queue */
	/* Two owners at least: the thread that built it, and the engine it
	 * was handed to.  The engine's completion runs from an interrupt
	 * and lets go the moment the batch is done, which for a short
	 * batch is BEFORE the submitting thread has finished recording
	 * the request's fence on the objects it touched. */
	int refs;
	struct i915_gem_context *ctx;
	struct i915_engine *engine;
	/* the logical ring context it runs in: one of the context's slots
	 * (i915_request_alloc picks the engine's own; the submission ioctl
	 * picks the slot of the engine map it was aimed at) */
	int lrc_idx;
	struct i915_lrc *lrc;
	uint32_t seqno;
	uint32_t ring_tail; /* the ring position after this request */
	struct drm_fence *fence;
	uint64_t submitted_ns;
	int submitted; /* handed to the hardware */
	int guilty;
	/* objects it touches: released on retire */
	struct drm_gem_object **objs;
	uint32_t nobjs;
	/* before Broadwell: where the request starts in the engine's ring
	 * (a reset replays the ring from the first unfinished request) */
	uint32_t ring_head;
};

struct i915_engine {
	struct i915_device *i915;
	enum i915_engine_id id;
	const char *name;
	uint8_t class; /* I915_ENGINE_CLASS_* of the uAPI */
	uint8_t instance;
	uint8_t present;
	uint32_t mmio_base;
	/* the register offset of the GT the engine belongs to: 0 on the
	 * primary GT, I915_MEDIA_GT_BASE on a standalone media GT.  It
	 * applies to that GT's registers below 0x40000 (forcewake, reset,
	 * the GuC, the tables); the engines' own ring registers sit at
	 * their usual offsets on either GT. */
	uint32_t gsi_offset;
	uint32_t irq_shift; /* Gen8-10: shift in the GT IIR pair */
	uint32_t gt_iir; /* which GT IIR (0..3) */
	uint32_t fw_domain;
	uint32_t context_size; /* bytes of the logical ring context image */

	/* the hardware status page, in the GGTT */
	struct drm_gem_object *hwsp_obj;
	uint32_t hwsp_ggtt;
	volatile uint32_t *hwsp;

	/* execution lists */
	int csb_entries;
	int csb_head;
	uint32_t active_ctx_id; /* what the hardware is running, 0 none */
	struct i915_gem_context *active_ctx;
	struct i915_lrc *active_lrc;
	/* The engine keeps the context it last ran loaded after the
	 * context is done, and writes its image once more when the next
	 * one is loaded.  A context is therefore held (referenced) until
	 * two others have been loaded after it; the ones let go of are
	 * parked here and released outside the engine's lock. */
	struct i915_gem_context *resident[2];
	struct i915_gem_context *stale[16];
	int nstale;
	int port_busy;

	/* requests */
	spinlock_t lock;
	struct i915_request *queue, *queue_tail; /* not yet submitted */
	struct i915_request *inflight, *inflight_tail; /* submitted, not retired */
	uint32_t next_seqno;
	uint32_t last_retired;
	uint64_t fence_context;
	struct wait_queue_head wq;

	/* the kernel context (golden state, engine init) */
	struct i915_gem_context *kctx;
	int golden_ready;
	/* the batch the engine runs at every context restore (render,
	 * Gen9): a page in the GGTT, its first wa_bb_bytes the batch, the
	 * scratch it uses further in */
	struct drm_gem_object *wa_bb_obj;
	uint32_t wa_bb_ggtt, wa_bb_bytes;

	/* hang detection */
	uint32_t hang_seqno, hang_acthd;
	int hang_strikes;
	/* interrupt watch: user interrupts taken, and what they and the
	 * retired sequence stood at when the GT worker last looked (a
	 * request that completes with no interrupt is found by polling,
	 * and said) */
	volatile uint32_t user_irqs;
	uint32_t watch_irqs, watch_retired;
	int noirq_said;
	int ring_full_said; /* a full ring is reported once */
};

/* The GT reset and what the GT needs before any engine (or the GuC)
 * runs: page attribute table, GT-wide settings, cache-control tables. */
void i915_gt_init_hw(struct i915_device *i915);
/* Xe2 on: the compute engines the fuses leave and the CCS mode
 * (i915->ccs_fused, i915->ccs_mode), and the CCS mode register's value */
void i915_xe2_compute_fuses(struct i915_device *i915);
uint32_t i915_xe2_ccs_mode_value(const struct i915_device *i915);
/* Wa_16023105232, Wa_14025941587: the engine's idle delay is adjusted
 * (and is one of the registers the GuC restores) */
int i915_engine_wa_idledly(const struct i915_engine *e);
/* Xe2/Xe3 discrete: the L3's transient (display) lines out to memory
 * before a surface the GT wrote is scanned out; nothing elsewhere */
void i915_gt_td_flush(struct i915_device *i915);
int i915_engines_init(struct i915_device *i915);
void i915_engines_fini(struct i915_device *i915);
/* The engines' registers programmed again on the rings and status pages
 * they already have (after a resume); nothing is allocated. */
int i915_engines_resume(struct i915_device *i915);
/* Release the contexts the engine no longer keeps loaded.  Called
 * without the engine's lock, after any submission. */
void i915_engine_drop_stale(struct i915_engine *e);
struct i915_engine *i915_engine_by_id(struct i915_device *i915, int id);
struct i915_engine *i915_engine_by_class(struct i915_device *i915, int class, int instance);
/* Interrupt handling: user interrupt (seqno advanced) / context switch
 * (context status buffer has entries). */
void i915_engine_irq(struct i915_engine *e, uint32_t bits);
/* Retire completed requests (signal fences, drop object refs). */
void i915_engine_retire(struct i915_engine *e);
/* A word of the engine's status page, as the device last wrote it.  On a
 * part with a last-level cache the page is bound cached and the device's
 * writes are seen through it; elsewhere the processor's own copy of the
 * line is dropped first. */
uint32_t i915_hwsp_read(struct i915_engine *e, unsigned index);
/* Reset one engine after a hang. */
int i915_engine_reset(struct i915_engine *e, const char *why);
int i915_gt_reset_all(struct i915_device *i915);
/* Say where the engine behind fence stream `context' is with the
 * request numbered `seqno' (a wait for it has lasted seconds). */
void i915_engine_report_stuck(struct i915_device *i915, uint64_t context, uint64_t seqno);
/* Wait until the engine has nothing in flight (bounded). */
int i915_engine_idle(struct i915_engine *e, uint32_t timeout_ms);

/* ---- contexts (i915_gem_context.c) ------------------------------------------ */

struct i915_lrc {
	struct drm_gem_object *obj; /* the context image, in the GGTT */
	uint32_t ggtt;
	uint32_t hw_id; /* what goes into the descriptor; the status buffer reports it */
	struct i915_engine *engine;
	uint32_t *regs; /* the register-state page (page 1) */
	struct i915_ring ring;
	int initialised; /* the hardware has saved state into it once */
	int settings_done; /* the context settings have gone in ahead of a batch */
	/* GuC submission: the id the context is registered under, and
	 * whether its scheduling has been switched on */
	uint32_t guc_id;
	int guc_registered;
	int guc_enabled;
};

#define I915_CTX_LRC_SLOTS 16 /* engine map entries; >= I915_NUM_ENGINES */

struct i915_gem_context {
	int refs;
	uint32_t id; /* per-file id; hardware context id derived */
	uint32_t hw_id; /* the first slot's; each logical ring has its own */
	struct i915_device *i915;
	struct i915_vm *vm;
	struct drm_file *fp;
	/* the engine map: slot -> engine, NULL = unused */
	struct i915_engine *engines[16];
	int nengines;
	int explicit_engines;
	/* One logical ring context (image and ring) per SLOT, lazily: with
	 * the legacy map the slot is the physical engine, with an explicit
	 * map it is the map entry -- two entries that name the same engine
	 * are two separate hardware contexts, each with its own pipeline
	 * state, as the client that drew up the map expects. */
	struct i915_lrc lrc[I915_CTX_LRC_SLOTS];
	int priority;
	int recoverable;
	int bannable;
	int persistent;
	int banned;
	int closed;
	/* reset statistics */
	uint32_t reset_count, batch_active, batch_pending;
	uint32_t hang_count;
	spinlock_t lock;
};

struct i915_gem_context *i915_context_create(struct i915_device *i915,
					     struct drm_file *fp, struct i915_vm *vm);
/* A driver-owned object of `size' bytes, bound in the GGTT (uncached or
 * not; physically contiguous when `linear'), or NULL. */
struct drm_gem_object *i915_gem_alloc_bound(struct i915_device *i915, uint64_t size,
					    int uncached, int linear, uint32_t *ggtt);
void i915_context_get(struct i915_gem_context *ctx);
void i915_context_put(struct i915_gem_context *ctx);
/* The context's logical ring in slot `idx' for an engine (created on
 * first use). */
struct i915_lrc *i915_context_lrc(struct i915_gem_context *ctx, struct i915_engine *e, int idx);
uint64_t i915_lrc_descriptor(struct i915_device *i915, struct i915_lrc *lrc, struct i915_engine *e);
void i915_context_capture_golden(struct i915_gem_context *ctx, struct i915_engine *e);
/* Run one empty batch per engine under the driver's context: captures the
 * golden state and proves the engines execute. */
int i915_gt_golden_init(struct i915_device *i915);
/* Discrete parts, once the engines run: the copy engine clears local
 * memory beyond the processor's window from then on. */
void i915_gt_lmem_ready(struct i915_device *i915);
/* Integrated Xe2 on with flat compression: an object a client will
 * reach through a compressing entry starts with its compression state
 * cleared (by the copy engine; waited for).  0 when there is nothing to
 * do or the GT is not up. */
int i915_gt_clear_ccs(struct i915_device *i915, struct drm_gem_object *o);

/* the logical ring context image layout: i915_lrc_layout.h (pure) */

/* ---- requests / submission (i915_request.c, i915_execlists.c) ---------- */

void i915_request_get(struct i915_request *rq);
struct i915_request *i915_request_alloc(struct i915_gem_context *ctx,
					struct i915_engine *e);
/* Emit the request's commands into the context's ring: the cache and
 * translation invalidation every batch starts with, the batch start,
 * the breadcrumb.  Then queue it. */
int i915_request_submit(struct i915_request *rq, uint64_t batch_addr,
			uint32_t batch_len);
void i915_request_put(struct i915_request *rq);
/* Push queued requests to the hardware (called with e->lock held or from
 * the tasklet path). */
void i915_execlists_submit(struct i915_engine *e);
void i915_execlists_process_csb(struct i915_engine *e);
/* The same from the interrupt: the port is freed and the next context
 * submitted, but nothing is retired or released (that may free). */
void i915_execlists_process_csb_irq(struct i915_engine *e);
int i915_execlists_init_engine(struct i915_engine *e);
void i915_execlists_reset_engine(struct i915_engine *e);
/* i915_workarounds.c: what this part needs set before it renders.  The
 * engine's own settings are written at engine init; the ones a context
 * carries are emitted into the batch whose saved state becomes the
 * image every later context starts from. */
void i915_gt_apply_workarounds(struct i915_device *i915, struct i915_engine *e);
/* Gen12 on: the GT-wide settings, after every GT reset. */
void i915_gt_apply_gt_workarounds(struct i915_device *i915);
/* The batch a render engine runs at every context restore: built once
 * at engine init (nothing on parts that need none), freed at fini. */
int i915_wa_bb_init(struct i915_device *i915, struct i915_engine *e);
void i915_wa_bb_fini(struct i915_engine *e);
uint32_t i915_ctx_workarounds_emit(struct i915_device *i915, int engine_class,
				   uint32_t *ring, uint32_t max_dwords);
/* Everything a render context must carry, emitted into its ring ahead of
 * every batch: the chicken bits, the L3 cache-control table and the
 * power/clock request are context state, and a context the driver never
 * tells runs with the part's own defaults. */
uint32_t i915_ctx_settings_emit(struct i915_device *i915, struct i915_engine *e,
				uint32_t *ring, uint32_t max_dwords);
/* Gen12: the engine's AUX table invalidation register (absolute), 0 for
 * none. */
uint32_t i915_engine_aux_inv_reg(struct i915_device *i915, const struct i915_engine *e);
/* What the render engine should have powered, from the fuses. */
uint32_t i915_rpcs_for(struct i915_device *i915);
/* Report a memory access the engine could not make, once per fault. */
void i915_gt_check_faults(struct i915_device *i915);
/* i915_mocs.c: the cache-control tables a batch's state refers to. */
unsigned i915_mocs_entries(int gen);
uint32_t i915_mocs_control_value(int gen, unsigned index);
uint32_t i915_mocs_l3cc_value(int gen, unsigned reg);
void i915_mocs_init_engine(struct i915_device *i915, struct i915_engine *e);
/* Gen12 on: the GT's own tables, after every GT reset. */
void i915_mocs_init(struct i915_device *i915);
uint32_t i915_mocs_l3cc_emit(struct i915_device *i915, int engine_class,
			     uint32_t *ring, uint32_t max_dwords);

/* ---- workaround lists (i915_workarounds.c builds and applies them; the
 * Gen12-and-later contents are in i915_wa_lists.c) ------------------------ */

#define I915_WA_MASKED (1u << 0) /* the upper half is a write mask */
#define I915_WA_MCR (1u << 1) /* one copy per slice/subslice/bank: steered read, multicast write */

/* One register: clear `clr', set `set'; `read' the bits that must read
 * back as `set' has them.  A masked register is written without a read
 * (clr is 0).  A whitelist entry carries the access flags in `reg'. */
struct i915_wa {
	uint32_t reg;
	uint32_t clr;
	uint32_t set;
	uint32_t read;
	uint32_t flags;
};

struct i915_wa_list {
	const char *name;
	struct i915_wa *list; /* kept sorted by register */
	unsigned count, cap;
	unsigned dropped; /* entries that did not fit */
};

void i915_wa_list_init(struct i915_wa_list *wal, const char *name,
		       struct i915_wa *storage, unsigned cap);
/* Add an entry; a second entry for the same register merges into the
 * first (its clear bits win over earlier set bits). */
void i915_wa_add(struct i915_wa_list *wal, uint32_t reg, uint32_t clr, uint32_t set,
		 uint32_t read, uint32_t flags);
/* i915_wa_lists.c, graphics IP 12.0 and later (nothing on older parts):
 * the GT-wide list of the primary GT or (media_gt) of the standalone
 * media GT -- registers below 0x40000 relative to that GT, i915_gt_reg()
 * makes them absolute -- the list of one engine,
 * the list a render or compute context carries, and the registers a
 * client's batch may write on an engine (each with its access flags). */
void i915_wa_gt_list(struct i915_device *i915, int media_gt, struct i915_wa_list *wal);
void i915_wa_engine_list(struct i915_device *i915, const struct i915_engine *e,
			 struct i915_wa_list *wal);
void i915_wa_ctx_list(struct i915_device *i915, const struct i915_engine *e,
		      struct i915_wa_list *wal);
void i915_wa_whitelist(struct i915_device *i915, const struct i915_engine *e,
		       struct i915_wa_list *wal);
/* A register of the GT at `gsi_offset' (0 or I915_MEDIA_GT_BASE): the
 * GT's own registers below 0x40000 move with it, the rest do not. */
static inline uint32_t i915_gt_reg(uint32_t gsi_offset, uint32_t reg)
{
	return reg < 0x40000 ? reg + gsi_offset : reg;
}
/* i915_mocs.c: the table entries the driver itself names */
unsigned i915_mocs_uc_index(struct i915_device *i915);
unsigned i915_mocs_wb_index(struct i915_device *i915);

/* The hangcheck timer and the GT worker thread. */
int i915_gt_workers_start(struct i915_device *i915);

/* i915_gem_context.c: the engines as clients name them -- the instances
 * of a class numbered densely in hardware order (a fused-off engine
 * leaves no hole), and the logical instance; the lookup takes a
 * client's (class, instance), NULL for none. */
int i915_engine_uabi_instance(const struct i915_engine *e);
struct i915_engine *i915_engine_lookup_user(struct i915_device *i915, int class, int instance);
int i915_engine_logical_instance(const struct i915_engine *e);
/* The default engine map (the legacy ring numbering) of a context. */
void i915_context_default_engines(struct i915_gem_context *ctx);
/* The video engine a legacy I915_EXEC_BSD submission goes to, from its
 * ring selector (flags & I915_EXEC_BSD_MASK) and the file's standing
 * choice (`file_pick', 0 until made); NULL for an invalid selector. */
struct i915_engine *i915_engine_legacy_bsd(struct i915_device *i915, uint32_t *file_pick,
					   unsigned bsd_flags);

#endif
