// LikeOS -- the Intel graphics driver's device state.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_DRV_H
#define KERNEL_DEV_GPU_I915_DRV_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/i915/i915_device_info.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/rwsem.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_guc.h>
#include <kernel/io/console.h>

/* The detailed log -- every register the driver programs, every mode
 * set and flip -- is off unless the kernel is built with I915_DEBUG=1.
 * What the driver found and every error are printed regardless. */
#ifndef I915_DEBUG
#define I915_DEBUG 0
#endif
#define i915_dbg(...)                                                          \
	do {                                                                   \
		if (I915_DEBUG)                                                \
			kprintf(__VA_ARGS__);                                  \
	} while (0)

/* PCH (south display) families, from the ISA bridge's device id. */
enum i915_pch {
	I915_PCH_NONE = 0, /* no PCH (discrete parts, some low-power ones) */
	I915_PCH_UNKNOWN,
	I915_PCH_IBX, /* Ibex Peak (Ironlake) */
	I915_PCH_CPT, /* Cougar Point (Sandy Bridge) */
	I915_PCH_PPT, /* Panther Point (Ivy Bridge) */
	I915_PCH_LPT, /* Lynx Point (Haswell) */
	I915_PCH_WPT, /* Wildcat Point (Broadwell) */
	I915_PCH_SPT, /* Sunrise Point (Skylake/Kaby Lake) */
	I915_PCH_KBP, /* Kaby Point */
	I915_PCH_CNP, /* Cannon Point (Coffee Lake) */
	I915_PCH_CMP, /* Comet Point */
	I915_PCH_ICP, /* Ice Point (Ice Lake) */
	I915_PCH_JSP, /* Jasper Point */
	I915_PCH_TGP, /* Tiger Point */
	I915_PCH_ADP, /* Alder Point */
	I915_PCH_MTP, /* Meteor Point */
	I915_PCH_LNL, /* Lunar Lake's */
	/* Discrete parts: the south display is on the card itself. */
	I915_PCH_DG1,
	I915_PCH_DG2,
};

/* Forcewake domains: which parts of the GT must be kept awake for a
 * register access.  A mask of these accompanies every get/put. */
#define I915_FW_RENDER (1u << 0)
#define I915_FW_GT (1u << 1) /* "blitter" on Gen9 */
#define I915_FW_MEDIA (1u << 2)
#define I915_FW_VDBOX0 (1u << 3)
#define I915_FW_VDBOX1 (1u << 4)
#define I915_FW_VDBOX2 (1u << 5)
#define I915_FW_VDBOX3 (1u << 6)
#define I915_FW_VEBOX0 (1u << 7)
#define I915_FW_VEBOX1 (1u << 8)
/* Meteor Lake's standalone media GT: its own GT-common domain and the
 * security controller's; its video engines use the VDBOX/VEBOX bits
 * above (the primary GT has none there). */
#define I915_FW_MEDIA_GT (1u << 9)
#define I915_FW_GSC (1u << 10)
#define I915_FW_DOMAINS 11
#define I915_FW_ALL ((1u << I915_FW_DOMAINS) - 1)

struct i915_fw_domain {
	uint32_t mask; /* I915_FW_* bit, 0 = absent on this device */
	uint32_t reg_set; /* the request register */
	uint32_t reg_ack; /* the acknowledge register */
	int count; /* outstanding gets */
	uint32_t timeouts; /* acks that never came */
};

struct i915_device {
	struct drm_device drm; /* first: the DRM core's device */
	const pci_device_t *pci;
	const struct i915_pci_id *id;
	const struct intel_device_info *info;
	uint16_t devid;
	uint8_t revid;
	uint8_t pch;
	uint16_t pch_devid;
	/* What the part is beyond its descriptor (i915_step.c): the variant
	 * the device id names, the graphics and media IP versions
	 * (I915_IP: 1200, 1255, 1270...) and their steppings. */
	uint8_t subplatform;
	uint8_t gt_step, media_step;
	uint16_t gt_ip, media_ip;
	/* Meteor Lake on: the video engines are a GT of their own, whose
	 * registers sit I915_MEDIA_GT_BASE above the primary GT's. */
	int has_media_gt;
	/* Xe2 on: the media GT's GMD_ID sub-IP field (Xe3P media whose
	 * engines and GuC are still Xe3's), whether every register read is
	 * to be preceded by dummy writes (Wa_15015404425), and whether the
	 * firmware left the flat compression metadata enabled */
	uint8_t media_subip;
	int mmio_read_flush;
	int has_flat_ccs;
	/* Wa_22019338487: every so many writes of global GTT entries a
	 * dummy write of this GMD_ID register makes the graphics unit
	 * finish the earlier ones (0: no such need) */
	uint32_t ggtt_wa_reg, ggtt_wa_every, ggtt_wa_count;

	/* the register window and the aperture */
	struct pci_bar bar_mmio, bar_aperture, bar_io;
	uint64_t mmio_virt; /* registers, uncached */
	uint64_t mmio_size; /* bytes of registers (half of BAR0 on Gen8-12) */
	uint64_t gtt_virt; /* the global GTT's entries, uncached */
	uint64_t ggtt_scratch_phys; /* what an unbound global entry points at */
	/* one bit per page of the global space the driver hands out (the
	 * part below 4 GB and above the firmware's stolen range): set while
	 * an object is bound there, cleared when it is unbound */
	uint8_t *ggtt_map;
	uint32_t ggtt_map_pages;
	uint32_t ggtt_map_first; /* the first page the map may hand out */
	uint64_t gtt_size; /* bytes of entries */
	uint64_t ggtt_bytes; /* address space the GTT covers */
	uint64_t stolen_base, stolen_size; /* data stolen memory (DSM) */
	uint64_t gsm_base; /* GTT stolen memory (holds the GTT on Gen8-9) */
	uint64_t aperture_virt; /* GMADR, write-combining, 0 until mapped */
	/* Meteor Lake on: BAR2 is no aperture but a window (LMEMBAR) onto
	 * the GTT's and the data stolen memory: the processor reaches the
	 * stolen range at stolen_io */
	struct pci_bar bar_lmem;
	uint64_t stolen_io;

	/* forcewake */
	spinlock_t fw_lock;
	struct i915_fw_domain fw[I915_FW_DOMAINS];
	uint32_t fw_present; /* mask of domains this device has */
	uint32_t fw_held; /* mask currently awake */
	int unclaimed_warned;

	/* topology, from the fuse registers */
	uint32_t slice_mask;
	uint32_t subslice_mask[4]; /* per slice */
	uint32_t eu_total;
	uint32_t eus_per_subslice; /* the most any one has */
	uint16_t eu_mask; /* Gen11 on: the units each subslice has */
	uint8_t gen8_eu_mask[3][4]; /* Gen8/9: each subslice's units, by slice */
	uint32_t l3bank_mask; /* Gen11-12.0, 12.70 on: the L3 banks present */
	uint32_t mslice_mask; /* DG2: the memory slices present */
	uint32_t dss_geometry, dss_compute; /* Xe_HP on: the dual subslices */
	uint8_t vdbox_sfc_access; /* the video engines with a scaler of their own */
	uint8_t sfc_mask; /* Xe_HP media: the scalers present */
	uint32_t cs_timestamp_hz;
	/* Xe2 on: the crystal's period the engines' idle timers count in
	 * (picoseconds, 0 when the crystal is not a known one), the dual
	 * subslices of a steering group, and the compute engines that get
	 * the compute slices (CCS mode: 1, every slice to the first) */
	uint32_t ts_base_ps;
	uint8_t dss_per_group;
	uint8_t ccs_mode, ccs_fused;
	/* Xe2 on: the steering targets of the L3 bank and node ranges and
	 * of the SQIDI/PSMI ranges */
	uint8_t l3bank_group, l3bank_instance, node_group, node_instance;
	uint8_t sqidi_group, sqidi_instance;
	/* the GuC's hardware configuration table (DG2 and Meteor Lake on),
	 * as the query of the same name returns it; NULL where none */
	void *hwconfig;
	uint32_t hwconfig_size;
	/* Where a read of a register with one copy per slice or subslice
	 * is steered when nothing more specific applies: a group (slice)
	 * and an instance (subslice) that exist (i915_mcr.c). */
	uint8_t mcr_group, mcr_instance;

	/* interrupts */
	int irq_vector; /* -1 = none */
	uint64_t irq_count;
	uint64_t irq_unclaimed;
	uint32_t irq_ident_lost; /* engine identities that never showed */
	/* storm watch: when the last 65536 interrupts began, what the last
	 * one's master and display summaries said, and how often a storm
	 * was reported */
	uint64_t irq_storm_mark;
	uint32_t irq_last_master, irq_last_disp;
	int irq_storm_said;

	/* what the firmware left showing (scanout inheritance) */
	struct {
		int pipe; /* -1 = nothing enabled */
		uint32_t plane_ctl, surf, stride, size;
	} boot_scanout;

	/* the display side */
	struct intel_display display;

	/* the GT: engines and everything that runs on them */
	struct i915_engine engines[I915_NUM_ENGINES];
	int nengines;
	int gt_ready; /* engines initialised; submissions accepted */
	uint32_t next_ctx_hw_id;
	uint64_t total_ram_bytes;
	hrtimer_t hangcheck_timer;
	hrtimer_t gt_poll_timer; /* the worker's next look while engines are busy */
	int hangcheck_pending; /* work for the GT worker: retire, the GuCs' messages */
	uint64_t hangcheck_last_ns; /* when the engines were last judged for progress */
	struct wait_queue_head gt_wq; /* the GT worker sleeps here */
	task_t *gt_worker;
	volatile int gt_worker_ready;
	uint32_t gpu_reset_count;
	uint32_t gt_faults;
	int enomem_warned;
	/* render P-states (i915_rps.c): the part's range and the request */
	uint32_t rps_rp0, rps_rp1, rps_rpn, rps_cur;
	/* the GuC (i915_guc.c): the primary GT's, and the standalone media
	 * GT's (Meteor Lake), which has a GuC of its own */
	struct i915_guc guc;
	struct i915_guc media_guc;
	/* Submissions, one at a time: from binding an object through
	 * recording the request on it, so that the order of sequence
	 * numbers is the order in the rings and in the queues, and no
	 * submission sees an object between another's submission and its
	 * record of it.  Waits for other engines happen outside it. */
	mm_rwsem_t submit_lock;
	mm_rwsem_t clear_lock; /* the copy engine's clearing range (i915_gem_context.c) */
	/* the fences recorded on objects (write_fence, read_fence[]) */
	spinlock_t sync_lock;
	/* the global address space's allocation map (i915_gtt.c): bindings
	 * are made and dropped from every thread */
	spinlock_t ggtt_lock;
	/* the fence registers (i915_fence.c): which object holds each */
	spinlock_t fence_lock;
	int num_fences;
	struct drm_gem_object *fence_obj[32];
};

/* The driver's own mapping kind: the object through the graphics aperture
 * (drm_gem_mmap_offset_kind), translated by the global address space and,
 * for a tiled object, a fence register. */
#define I915_MMAP_KIND_GTT 4

/* i915_fence.c */
int i915_fences_init(struct i915_device *i915);
void i915_fences_restore(struct i915_device *i915);
int i915_gem_mmap_kind(struct drm_gem_object *o, unsigned kind, uint64_t first_page,
		       struct device_mmap *m);
void i915_fence_object_free(struct i915_device *i915, struct drm_gem_object *o);
/* i915_gtt.c: a binding low in the global space, where the aperture reaches */
int i915_ggtt_bind_obj_mappable(struct i915_device *i915, struct drm_gem_object *o,
				uint32_t *ggtt_offset);

/* Name the allocation a failed submission could not make, once. */
void i915_enomem_once(struct i915_device *i915, const char *what);
/* Say where a binding was asked to go when it could not be made, once. */
void i915_bind_failed_once(struct i915_device *i915, uint64_t addr, uint64_t size,
			   int pinned);

/* i915_gem.c: an object the display will read must not be left in the
 * processor's cache, nor written into it afterwards */
void i915_gem_object_set_display(struct i915_device *i915,
				 struct drm_gem_object *o);
/* i915_gem.c: the object's pages go back to where they came from */
void i915_gem_release_pages(struct drm_gem_object *o);
/* i915_firmware.c: look for the GT's firmware images and say what is there */
void i915_uc_firmware_describe(struct i915_device *i915);

/* Where an object is bound in one address space. */
struct i915_vma {
	struct i915_vma *next; /* the object's list */
	struct i915_vma *vm_next; /* the address space's list (i915_vma.c) */
	struct i915_vm *vm;
	uint64_t addr;
	uint32_t npages;
	int pinned; /* at the client's address */
	int attached; /* owns its range in the address space */
	int allocated; /* the driver chose the address (a relocation client) */
};

/* i915_vma.c: the ranges an address space has given out */
void i915_vma_list_attach(struct i915_vma **head, struct i915_vma *v);
int i915_vma_list_detach(struct i915_vma **head, struct i915_vma *v);
unsigned i915_vma_list_supersede(struct i915_vma **head, uint64_t addr,
				 uint32_t npages, const struct i915_vma *except);
unsigned i915_vm_vma_claim(struct i915_vm *vm, struct i915_vma *v,
			   uint64_t addr, uint32_t npages);
int i915_vm_vma_release(struct i915_vm *vm, struct i915_vma *v);
/* Pure: the first gap of `npages' pages aligned to `align', at or after
 * `from' and wholly below `limit', that no binding on the list overlaps;
 * 0 when there is none. */
uint64_t i915_vma_list_find_gap(struct i915_vma *head, uint64_t from,
				uint64_t limit, uint32_t npages, uint64_t align);
/* Cherryview's contexts address 4 GB through three levels: four page
 * directories the context's PDP registers name, not one fourth-level
 * table (the address spaces of i915_ppgtt.c make them up front). */
static inline int i915_vm_3lvl(const struct i915_device *i915)
{
	return i915->info->gen >= 8 && i915->info->ppgtt_bits && i915->info->ppgtt_bits <= 32;
}

/* What a client may use of an address space: all of it, less the top
 * 64 KB that the copy engine's per-context batch scribbles into
 * (Wa_16018031267, Xe_HP to Meteor Lake). */
static inline uint64_t i915_vm_total(const struct i915_device *i915)
{
	uint64_t t = i915_vm_3lvl(i915) ? 1ULL << 32 : I915_VM_SIZE;
	if (i915->gt_ip >= I915_IP(12, 55) && i915->gt_ip <= I915_IP(12, 71))
		t -= 0x10000;
	return t;
}

/* i915_ppgtt.c: an address for an object the client did not place, taken
 * and attached under the space's lock.  `low' keeps it below 4 GB (an
 * object without the 48-bit flag). */
int i915_vm_vma_alloc(struct i915_vm *vm, struct i915_vma *v, uint32_t npages,
		      uint64_t align, int low);

/* The engine classes of the interface: render, copy, video, video
 * enhancement, compute. */
#define I915_ENGINE_CLASSES 5

/* Per-object driver state (drm_gem_object.priv): where the object sits in
 * the GGTT (scanout, rings, context images) and in the address spaces
 * that have it bound; what the client said about it. */
struct i915_bo {
	uint32_t ggtt;
	int bound;
	int ggtt_uncached; /* how the binding was made, for a resume */
	/* bytes of global space the binding reserves when that is more than
	 * the object (gen2/3 tiled scanout: the fence's region); 0 = the
	 * object's size */
	uint32_t ggtt_size;
	struct i915_vma *vmas;
	uint32_t tiling; /* I915_TILING_* */
	uint32_t stride;
	uint32_t caching; /* I915_CACHING_* */
	uint32_t pat_index;
	uint32_t madv; /* I915_MADV_* */
	int purged;
	uint32_t read_domains, write_domain;
	/* last submission writing the object, and the class of the engine
	 * it ran on; the last reader on each engine class (a writer counts
	 * as a reader of its own class, as the busy call reports it) */
	struct drm_fence *write_fence;
	uint8_t write_class;
	struct drm_fence *read_fence[I915_ENGINE_CLASSES];
	uint64_t user_size; /* what CREATE asked for, before rounding */
	/* The pages are a client's own (USERPTR): referenced, not owned;
	 * released with mm_put_page() rather than freed. */
	int userptr;
	/* Mapped through the aperture (i915_fence.c): how many mappings, the
	 * binding made for them (or the scanout one reused), the fence */
	int gtt_map_refs;
	int gtt_map_bound;
	int gtt_map_own;
	uint32_t gtt_map_ggtt;
	int fence_id; /* -1 = none */
	int pat_set; /* the client named a page attribute table entry (pat_index) */
};

/* Per-file driver state (drm_file.priv): the contexts and address
 * spaces this client made. */
#define I915_MAX_CONTEXTS 4096 /* per open file; a page with many canvases makes many */
#define I915_MAX_VMS 256
struct i915_file {
	struct i915_gem_context *ctx[I915_MAX_CONTEXTS]; /* 0 = the default */
	struct i915_vm *vm[I915_MAX_VMS]; /* ids 1.. */
	spinlock_t lock;
	/* the video engine an I915_EXEC_BSD submission naming neither ring
	 * goes to: its user instance + 1, 0 until the file's first such
	 * submission */
	uint32_t bsd_next;
};

/* i915_gem.c */
long i915_gem_ioctl(struct i915_device *i915, struct drm_file *fp, unsigned nr,
		    void *kb, unsigned size, int *handled);
int i915_gem_open(struct drm_device *dev, struct drm_file *fp);
void i915_gem_postclose(struct drm_device *dev, struct drm_file *fp);
void i915_gem_object_free(struct drm_gem_object *o);
struct i915_gem_context *i915_file_context(struct drm_file *fp, uint32_t id);
/* Wait for whatever last wrote (and, if `write', read) the object. */
int i915_gem_object_wait(struct drm_gem_object *o, int write, uint64_t timeout_ns);
/* The dma-buf's view of the object's fences (drm_driver hooks). */
struct drm_fence *i915_gem_busy_fence(struct drm_gem_object *o, int write);
int i915_gem_attach_fence(struct drm_gem_object *o, struct drm_fence *f, int write);
/* The unsignalled fences recorded on an object, referenced, appended to
 * `out': the writer's, and every reader's when `write' (the caller means
 * to write it).  Fences of `skip_context' are left out (an engine's own
 * work is ordered behind its own); 0 skips none. */
void i915_gem_object_fences(struct drm_gem_object *o, int write, uint64_t skip_context,
			    struct drm_fence **out, unsigned *n);
/* i915_gem_execbuf.c */
long i915_gem_execbuffer2(struct i915_device *i915, struct drm_file *fp,
			  void *kb, int wr);
/* i915_query.c */
long i915_query_ioctl(struct i915_device *i915, struct drm_file *fp, void *kb);
/* i915_ioctl.c */
long i915_getparam(struct i915_device *i915, void *kb);
long i915_context_ioctl(struct i915_device *i915, struct drm_file *fp, unsigned nr,
			void *kb, unsigned size, int *handled);

/* The one device (there is one integrated graphics function per system). */
extern struct i915_device g_i915;

static inline struct i915_device *to_i915(struct drm_device *dev)
{
	return (struct i915_device *)dev;
}

/* i915_drv.c */
int i915_init(void);

/* i915_uncore.c */
uint32_t i915_read32(struct i915_device *i915, uint32_t reg);
void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t val);
uint64_t i915_read64(struct i915_device *i915, uint32_t reg);
void i915_write64(struct i915_device *i915, uint32_t reg, uint64_t val);
/* Read that ignores forcewake (for registers that need none). */
uint32_t i915_read32_fw(struct i915_device *i915, uint32_t reg);
void i915_write32_fw(struct i915_device *i915, uint32_t reg, uint32_t val);
/* Wait for (reg & mask) == value; 0 or -ETIMEDOUT. */
int i915_wait_reg(struct i915_device *i915, uint32_t reg, uint32_t mask,
		  uint32_t value, uint32_t timeout_us);
int i915_uncore_init(struct i915_device *i915);
void i915_uncore_fini(struct i915_device *i915);
void i915_fw_get(struct i915_device *i915, uint32_t mask);
void i915_fw_put(struct i915_device *i915, uint32_t mask);
uint32_t i915_fw_domains_for(struct i915_device *i915, uint32_t reg);

/* i915_gtt.c */
int i915_gtt_probe(struct i915_device *i915);
/* After a resume: every object bound in the global space gets its
 * entries written again (the table itself may have been lost). */
void i915_ggtt_rewrite_all(struct i915_device *i915);
void i915_ggtt_program_pat(struct i915_device *i915);
void i915_gtt_fini(struct i915_device *i915);
void i915_read_topology(struct i915_device *i915);
uint32_t i915_timestamp_hz(struct i915_device *i915);

/* i915_step.c: the variant, IP versions and steppings, from the device
 * id, the revision and (Meteor Lake on) the GMD_ID registers */
void i915_step_init(struct i915_device *i915);
const char *i915_step_name(uint8_t step);

/* i915_mcr.c: registers with one copy per slice, subslice or bank.  A
 * read is steered at a copy that exists; a write reaches every copy. */
void i915_mcr_init(struct i915_device *i915);
uint32_t i915_mcr_read(struct i915_device *i915, uint32_t reg);
void i915_mcr_write(struct i915_device *i915, uint32_t reg, uint32_t val);
/* DG2: the selectors' defaults, after every GT reset */
void i915_mcr_program_defaults(struct i915_device *i915);
/* The group and instance a read of `reg' (absolute) is steered at; 0 when
 * the register is in none of the part's steering ranges (0/0 then). */
int i915_mcr_steering(struct i915_device *i915, uint32_t reg, uint8_t *group,
		      uint8_t *instance);
/* Xe2 on: the dual subslices of a steering group, from the GuC's
 * hardware configuration table */
void i915_mcr_set_dss_per_group(struct i915_device *i915, unsigned n);

/* i915_rps.c */
int i915_rps_init(struct i915_device *i915);
uint32_t i915_rps_current_mhz(struct i915_device *i915);

/* i915_irq.c */
int i915_irq_init(struct i915_device *i915);
void i915_irq_fini(struct i915_device *i915);
/* Everything masked (suspend) / the masters back on (resume), the
 * vector kept. */
void i915_irq_suspend(struct i915_device *i915);
void i915_irq_resume(struct i915_device *i915);

#endif
