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
#define I915_FW_DOMAINS 9
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
	uint32_t cs_timestamp_hz;

	/* interrupts */
	int irq_vector; /* -1 = none */
	uint64_t irq_count;
	uint64_t irq_unclaimed;

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
	int hangcheck_pending;
	struct wait_queue_head gt_wq; /* the GT worker sleeps here */
	task_t *gt_worker;
	volatile int gt_worker_ready;
	uint32_t gpu_reset_count;
	uint32_t gt_faults;
	int enomem_warned;
	/* render P-states (i915_rps.c): the part's range and the request */
	uint32_t rps_rp0, rps_rp1, rps_rpn, rps_cur;
	/* the GuC (i915_guc.c) */
	struct i915_guc guc;
	/* Submissions, one at a time: from binding an object through
	 * recording the request on it, so that the order of sequence
	 * numbers is the order in the rings and in the queues, and no
	 * submission sees an object between another's submission and its
	 * record of it.  Waits for other engines happen outside it. */
	mm_rwsem_t submit_lock;
	/* the fences recorded on objects (write_fence, read_fence[]) */
	spinlock_t sync_lock;
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
};

/* Per-file driver state (drm_file.priv): the contexts and address
 * spaces this client made. */
#define I915_MAX_CONTEXTS 4096 /* per open file; a page with many canvases makes many */
#define I915_MAX_VMS 256
struct i915_file {
	struct i915_gem_context *ctx[I915_MAX_CONTEXTS]; /* 0 = the default */
	struct i915_vm *vm[I915_MAX_VMS]; /* ids 1.. */
	spinlock_t lock;
	/* which video engine the next I915_EXEC_BSD submission that names
	 * neither ring goes to, on a part with two */
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
