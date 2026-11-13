// LikeOS - NVMe over PCI Express: the transport's objects and the contract
// between its files (Phase 1 agent P5)
//
//   nvme_pci.c         probe/remove/shutdown, the PCI driver and its error
//                      handlers, nvme_reset_work, nvme_dev_disable,
//                      nvme_shutdown_all, controller ops (nvme_pci_ctrl_ops)
//   nvme_pci_queue.c   queue memory, admin queue setup, Create/Delete
//                      SQ/CQ, queue counts and maps, the ring (submit,
//                      doorbell, CQ processing with phase tags), shadow
//                      doorbells (DBBUF), the blk_mq_ops (queue_rq,
//                      commit_rqs, queue_rqs, poll, complete, timeout)
//   nvme_pci_dma.c     DMA pools, PRP lists (chained), SGLs (segments),
//                      metadata pointers/SGLs, the SGL-vs-PRP threshold,
//                      unmapping; CMB (SQs in CMB, P2P), PMR, HMB
//   nvme_pci_irq.c     vector allocation with affinity (one pre-vector for
//                      the admin queue), the per-queue interrupt handler,
//                      INTx fallback, interrupt coalescing knob
//   nvme_pci_pm.c      suspend/resume (simple or host-managed via APST/PS),
//                      PCI PM, ASPM decisions
//   nvme_pci_quirks.c  the PCI ID table with quirk flags (QEMU 1b36:0010,
//                      VMware 15ad:07f0, VirtualBox 80ee:4e56, Intel,
//                      Samsung, Toshiba, Apple, ...)
//
// The lock order of nvme.h applies: dev->ctrl.shutdown_lock (1) around
// every enable/disable; nvmeq->sq_lock (7) for the SQ tail, the shadow
// doorbell and the doorbell write; nvmeq->cq_poll_lock (8) only for poll
// queues and the timeout's CQ poll.  An interrupt handler processes only
// its own queue's CQ and takes no lock for it (one CQ <-> one vector,
// the handler cannot run twice at once for one vector).  The transport's
// own registry lock nvme_pci_devs_lock is OUTSIDE all of them (level 0):
// it is held only to walk/change the device list, and nvme_shutdown_all
// drops it before shutting the controllers down.
//
// DMA: no IOMMU, bus address == physical address.  Every buffer the
// controller may access stays allocated until the controller can no
// longer access it: queue memory until the queue is deleted or the
// controller disabled; PRP/SGL lists and bounce data until the command's
// completion was consumed, or the controller disabled after a timeout
// (nvme_dev_disable before nvme_cancel_request frees them).  A DMA pool
// with blocks still out at destruction keeps its pages (WARN, leak)
// rather than hand memory the device may still write back to the
// allocator.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_NVME_NVME_PCI_H
#define KERNEL_DEV_NVME_NVME_PCI_H

#include <kernel/dev/nvme/nvme.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/io/console.h>

struct nvme_dev;
struct nvme_queue;

/* ==========================================================================
 * Sizes and limits
 * ========================================================================== */

#define NVME_PCI_MIN_QUEUE_SIZE 2
#define NVME_PCI_MAX_QUEUE_SIZE 4095
#define NVME_PCI_DEFAULT_IO_QUEUE_DEPTH 1024

/* Largest transfer the transport maps in one command (further capped by
 * MDTS): 8 MiB, the established limit. */
#define NVME_MAX_BYTES (8u * 1024u * 1024u)
/* PRP lists / SGL segments per command. */
#define NVME_MAX_NR_DESCRIPTORS 5
/* The small descriptor pool (short PRP lists, metadata SGLs). */
#define NVME_SMALL_POOL_SIZE 256
/* SGL descriptors in one segment page / metadata segments. */
#define NVME_MAX_SEGS (NVME_CTRL_PAGE_SIZE / sizeof(struct nvme_sgl_desc))
#define NVME_MAX_META_SEGS \
	((NVME_SMALL_POOL_SIZE / sizeof(struct nvme_sgl_desc)) - 1)
/* PRP entries per list page (the last entry chains to the next page). */
#define PRPS_PER_PAGE ((NVME_CTRL_PAGE_SIZE / sizeof(__le64)) - 1)
#define MAX_PRP_RANGE (NVME_MAX_BYTES + 2 * (NVME_CTRL_PAGE_SIZE - 1))

#define SQ_SIZE(q) ((uint64_t)(q)->q_depth << (q)->sqes)
#define CQ_SIZE(q) ((uint64_t)(q)->q_depth * sizeof(struct nvme_completion))

/* Doorbell of SQ `qid' (tail) / CQ `qid' (head): NVME_SQ_DB_IDX /
 * NVME_CQ_DB_IDX of nvme_spec.h, in dwords from dev->dbs. */

/* The shortest CMB-resident SQ worth having: below this depth the queues
 * go to host memory with the full depth instead. */
#define NVME_CMB_MIN_QUEUE_DEPTH 64

/* Largest chunk the HMB allocator starts with: the page allocator's
 * largest naturally aligned block (4 MiB, 1024 pages). */
#define NVME_HMB_MAX_CHUNK (4u * 1024u * 1024u)

/* What a DMA translation reports for an address it cannot map. */
#define NVME_DMA_BAD_ADDR (~0ULL)

/* ==========================================================================
 * Boot command line knobs ("nvme.<name>=", the established parameters)
 * ========================================================================== */

struct nvme_pci_params {
	int use_threaded_interrupts; /* nvme.use_threaded_interrupts */
	int use_cmb_sqes; /* nvme.use_cmb_sqes (default 1) */
	unsigned int max_host_mem_size_mb; /* nvme.max_host_mem_size_mb, 128 */
	unsigned int sgl_threshold; /* nvme.sgl_threshold, 32768; 0 off */
	unsigned int io_queue_depth; /* nvme.io_queue_depth, 1024 */
	unsigned int write_queues; /* nvme.write_queues */
	unsigned int poll_queues; /* nvme.poll_queues */
	int noacpi; /* nvme.noacpi */
	/* nvme.quirks=VID:DID:name+name-...: parsed list (P5). */
	char quirks[256];
	/* Interrupt coalescing (Set Features 08h) applied after every
	 * bring-up when nvme.irq_coalesce_time= (100 us units) is set;
	 * nvme.irq_coalesce_thr= is the aggregation threshold (0's based).
	 * Default off: the controller's own setting stays. */
	unsigned int irq_coalesce_time;
	unsigned int irq_coalesce_thr;
	int irq_coalesce_set; /* either knob given */
	/* nvme.pmr=0 keeps the Persistent Memory Region disabled. */
	int use_pmr;
};
extern struct nvme_pci_params nvme_pci_param;
void nvme_pci_parse_cmdline(void);

/* nvme.quirks= entries (parsed by nvme_pci_cmdline_parse_quirks). */
#define NVME_PCI_MAX_DYN_QUIRKS 16
struct nvme_pci_quirk_entry {
	uint16_t vendor_id;
	uint16_t dev_id;
	uint32_t enabled_quirks;
	uint32_t disabled_quirks;
};
/* Parse "VID:DID:name,^name-VID:DID:name" into entries (0 / -EINVAL; on
 * error nothing is stored).  Exposed for the host tests. */
int nvme_pci_cmdline_parse_quirks(const char *value,
				  struct nvme_pci_quirk_entry *out,
				  unsigned int max, unsigned int *count);

/* ==========================================================================
 * DMA pools (nvme_pci_dma.c)
 * ========================================================================== */

/* Fixed-size, aligned, physically contiguous blocks carved out of pages
 * from mm_allocate_contiguous_pages (below 4 GiB when the DMA mask
 * demands it), for PRP lists and SGL segments.  alloc/free are any
 * context (a spinlock inside).  Pages are pre-grown in process context;
 * an alloc that finds the pool empty grows it by one page from the page
 * allocator (whose lock is interrupt safe -- the established pool grows
 * atomically the same way) and fails with NULL only when that fails ->
 * the command gets BLK_STS_RESOURCE. */
struct nvme_dma_pool_page;
struct nvme_dma_pool {
	const char *name;
	uint32_t size; /* block size */
	uint32_t align;
	uint64_t dma_mask;
	spinlock_t lock;
	void *free_list; /* free blocks, linked through themselves */
	uint32_t nr_free, nr_total;
	struct nvme_dma_pool_page *pages;
	uint32_t nr_pages;
	uint32_t blocks_per_page;
	uint64_t nr_alloc_fail; /* growth failures (statistics) */
};
int nvme_dma_pool_init(struct nvme_dma_pool *pool, const char *name,
		       uint32_t size, uint32_t align, uint64_t dma_mask,
		       uint32_t prealloc);
void nvme_dma_pool_destroy(struct nvme_dma_pool *pool);
/* Any context.  *phys gets the bus address.  NULL when empty and the
 * pool could not grow. */
void *nvme_dma_pool_alloc(struct nvme_dma_pool *pool, uint64_t *phys);
void nvme_dma_pool_free(struct nvme_dma_pool *pool, void *vaddr,
			uint64_t phys);
/* Grow until at least min_free blocks are free (process context). */
int nvme_dma_pool_refill(struct nvme_dma_pool *pool, uint32_t min_free);

/* Coherent DMA memory (queues, DBBUF pages, HMB descriptor list):
 * zeroed, physically contiguous, aligned to `align', below the mask.
 * May sleep. */
void *nvme_dma_alloc_coherent(struct nvme_dev *dev, size_t size,
			      size_t align, uint64_t *phys);
void nvme_dma_free_coherent(struct nvme_dev *dev, size_t size, void *vaddr,
			    uint64_t phys);

struct nvme_descriptor_pools {
	struct nvme_dma_pool large; /* NVME_CTRL_PAGE_SIZE blocks */
	struct nvme_dma_pool small; /* NVME_SMALL_POOL_SIZE blocks */
	int initialized;
};

/* ==========================================================================
 * Queues (nvme_pci_queue.c)
 * ========================================================================== */

/* struct nvme_queue.flags bits */
#define NVMEQ_ENABLED 0
#define NVMEQ_SQ_CMB 1 /* SQ lives in the CMB */
#define NVMEQ_DELETE_ERROR 2
#define NVMEQ_POLLED 3 /* no interrupt: polled */

struct nvme_queue {
	struct nvme_dev *dev;
	struct nvme_descriptor_pools *descriptor_pools; /* per NUMA node */
	/* Submission side (lock order 7). */
	spinlock_t sq_lock;
	void *sq_cmds; /* q_depth entries of 1 << sqes bytes */
	uint64_t sq_dma_addr;
	volatile uint32_t *q_db; /* SQ tail doorbell; CQ head at
				  * q_db + dev->db_stride */
	uint32_t q_depth;
	uint16_t sq_tail;
	uint16_t last_sq_tail; /* last tail written to the doorbell */
	uint8_t sqes; /* log2 SQ entry size (6, or 7 for the quirk) */
	/* Completion side: the interrupt handler or (poll queues, timeout
	 * poll) cq_poll_lock (lock order 8), on its own cache line. */
	spinlock_t cq_poll_lock __attribute__((aligned(64)));
	volatile struct nvme_completion *cqes;
	uint64_t cq_dma_addr;
	uint16_t cq_head;
	uint8_t cq_phase;
	uint16_t cq_vector; /* index of the PCI vector (pci_irq_vector) */
	uint16_t qid; /* 0 = admin */
	volatile unsigned long flags; /* NVMEQ_* bits */
	/* Shadow doorbells (DBBUF): the slots of this queue in the
	 * doorbell / event-index pages, NULL without DBBUF. */
	__le32 *dbbuf_sq_db;
	__le32 *dbbuf_cq_db;
	__le32 *dbbuf_sq_ei;
	__le32 *dbbuf_cq_ei;
	/* Delete SQ/CQ completion (nvme_delete_io_queues). */
	struct completion delete_done;
	/* Statistics (debug dumps). */
	uint64_t nr_submitted, nr_completed, nr_irqs, nr_spurious;
	/* SQ in the CMB: its offset in the CMB (nvme_free_queue gives it
	 * back to the CMB's stack allocator). */
	uint64_t cmb_offset;
	/* The interrupt's name ("nvme0q3"); request_irq keeps the pointer. */
	char irq_name[20];
	/* Interrupt number the handler is registered on, -1 none. */
	int irq;
};

/* struct nvme_iod.flags */
enum nvme_iod_flags {
	IOD_ABORTED = 1u << 0, /* the timeout handler aborted it */
	IOD_SMALL_DESCRIPTOR = 1u << 1, /* descriptors from the small pool */
	IOD_SINGLE_SEGMENT = 1u << 2, /* one contiguous data segment */
	IOD_DATA_P2P = 1u << 3, /* data in peer memory (CMB) */
	IOD_META_P2P = 1u << 4,
	IOD_DATA_MMIO = 1u << 5,
	IOD_META_MMIO = 1u << 6,
	IOD_BOUNCED = 1u << 7, /* data went through a bounce buffer
				* (32-bit DMA mask, misaligned) */
};

/* One data segment as the controller sees it. */
struct nvme_dma_vec {
	uint64_t addr;
	uint32_t len;
};

/* The PCIe transport's per-command data: the block-layer PDU of every
 * tag (tag set cmd_size = sizeof(struct nvme_iod)).  `req' MUST stay the
 * first member (nvme_req()). */
struct nvme_iod {
	struct nvme_request req;
	struct nvme_command cmd; /* built here, copied into the SQ */
	uint8_t flags; /* IOD_* */
	uint8_t nr_descriptors; /* PRP list pages / SGL segments used */
	/* The device completed the command successfully: only then does a
	 * bounced read copy the bounce buffer back (never stale, unwritten
	 * pages into the caller's buffer).  Set by the completion path. */
	uint8_t data_valid;
	uint8_t pad;
	uint32_t total_len; /* data bytes mapped */
	void *descriptors[NVME_MAX_NR_DESCRIPTORS]; /* list blocks */
	uint64_t descriptor_dma[NVME_MAX_NR_DESCRIPTORS];
	struct nvme_dma_vec *dma_vecs; /* the segments (for unmap) */
	unsigned int nr_dma_vecs;
	uint64_t first_dma; /* PRP1/SGL first segment address */
	void *bounce; /* IOD_BOUNCED: the bounce buffer */
	uint32_t bounce_len;
	uint64_t meta_dma; /* MPTR (metadata, contiguous) */
	uint32_t meta_total_len;
	struct nvme_sgl_desc *meta_descriptor; /* metadata SGL, small pool */
	uint64_t meta_descriptor_dma;
	void *meta_bounce;
	/* Bounce bookkeeping: physical base and page counts. */
	uint64_t bounce_phys;
	uint32_t bounce_pages;
	uint64_t meta_bounce_phys;
	uint32_t meta_bounce_len;
	uint32_t meta_bounce_pages;
	uint8_t meta_flags; /* NVME_IOD_META_* */
};

/* struct nvme_iod.meta_flags */
#define NVME_IOD_META_MAPPED 0x01 /* nvme_map_metadata succeeded */
#define NVME_IOD_META_SINGLE 0x02 /* MPTR to one contiguous segment */
#define NVME_IOD_META_SGL 0x04 /* MPTR to an SGL segment (METASEG) */
#define NVME_IOD_META_BOUNCED 0x08

/* ==========================================================================
 * The PCIe controller
 * ========================================================================== */

struct nvme_dev {
	struct nvme_queue *queues; /* [nr_allocated_queues], [0] admin */
	struct blk_mq_tag_set tagset; /* I/O */
	struct blk_mq_tag_set admin_tagset;
	volatile uint32_t *dbs; /* doorbells: bar + NVME_REG_DBS */
	pci_device_t *pdev;
	unsigned int online_queues; /* queues live (admin included) */
	unsigned int max_qid; /* highest I/O qid */
	unsigned int io_queues[HCTX_MAX_TYPES]; /* per map type */
	unsigned int num_vecs; /* interrupt vectors allocated */
	uint32_t q_depth; /* I/O queue depth (min(CAP.MQES+1, knob)) */
	int io_sqes; /* log2 of the I/O SQ entry size */
	uint32_t db_stride; /* doorbell stride in dwords (1 << CAP.DSTRD) */
	volatile void *bar; /* BAR0 mapping */
	unsigned long bar_mapped_size;
	bool subsystem; /* NSSR supported (CAP.NSSRS) */
	/* Controller Memory Buffer. */
	uint64_t cmb_size;
	bool cmb_use_sqes;
	uint32_t cmbsz, cmbloc;
	uint64_t cmb_bus_addr; /* physical address of the CMB */
	volatile void *cmb; /* WC mapping */
	uint64_t cmb_alloc_offset; /* simple bump allocator for SQs */
	/* Persistent Memory Region. */
	uint32_t pmrcap;
	volatile void *pmr;
	uint64_t pmr_size;
	bool pmr_enabled;
	/* The core controller (embedded). */
	struct nvme_ctrl ctrl;
	uint32_t last_ps; /* power state before host-managed suspend */
	/* Host Memory Buffer. */
	bool hmb; /* enabled */
	uint64_t host_mem_size; /* bytes given */
	uint32_t nr_host_mem_descs;
	uint32_t host_mem_descs_size;
	uint64_t host_mem_descs_dma;
	struct nvme_host_mem_buf_desc *host_mem_descs;
	void **host_mem_desc_bufs; /* chunk virtual addresses */
	uint64_t *host_mem_desc_phys;
	/* Shadow doorbells. */
	__le32 *dbbuf_dbs;
	uint64_t dbbuf_dbs_dma_addr;
	__le32 *dbbuf_eis;
	uint64_t dbbuf_eis_dma_addr;
	/* Queue counts. */
	unsigned int nr_allocated_queues;
	unsigned int nr_write_queues;
	unsigned int nr_poll_queues;
	/* DMA. */
	uint64_t dma_mask;
	struct nvme_descriptor_pools descriptor_pools; /* one node */
	/* Probe/reset bookkeeping. */
	struct list_head dev_list; /* nvme_pci_devs (shutdown_all) */
	volatile int probed; /* probe() finished, reset not yet queued */
	volatile int removing;
	uint64_t last_reset_ns;
	int reset_failures; /* consecutive failed resets */
	char irq_name[8][16]; /* names of the first vectors (mirror) */

	/* --- added by P5 in Phase 1 ------------------------------------------ */
	uint32_t dbbuf_size; /* bytes of each DBBUF page set */
	int cmb_bar; /* BAR of the CMB, -1 none */
	uint64_t cmb_map_len; /* bytes mapped at `cmb' */
	int pmr_bar; /* BAR of the PMR, -1 none */
	uint64_t pmr_bus_addr;
	uint64_t pmr_map_len;
	bool aspm_enabled; /* the link has ASPM on (suspend policy) */
	volatile int shutdown_done; /* nvme_shutdown_all / ->shutdown ran */
	volatile int in_list; /* on nvme_pci_devs */
	uint32_t hmb_chunk_size; /* chunk size the HMB was built with */
	uint64_t hmb_alloc_fail; /* HMB allocation failures */
	/* Serialises every enable, disable, allocation and free of the host
	 * memory buffer (sysfs `hmb', reset, suspend/resume, removal): the
	 * controller must never be handed a buffer another path is freeing.
	 * Taken outside shutdown_lock's users only (never inside it). */
	struct mutex hmb_lock;
	/* Counts the controller disables (nvme_dev_disable): a Set Features
	 * (HMB) that completed successfully only sets `hmb' when no disable
	 * came in between -- a disabled controller let go of the buffer,
	 * whatever its last command answered.  ctrl.lock. */
	uint32_t hmb_disable_seq;
	unsigned long platform_quirks; /* nvme_pci_platform_quirks() */
	uint64_t nr_bounces; /* data/metadata bounces (statistics) */
};

static inline struct nvme_dev *to_nvme_dev(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_dev, ctrl);
}

/* All probed PCIe controllers (nvme_shutdown_all, late init). */
extern struct list_head nvme_pci_devs;
extern struct mutex nvme_pci_devs_lock;

/* SGL data blocks must be dword aligned in address and length (SGLS
 * support field 10b).  The SGL/metadata-SGL support tests are the core's
 * nvme_ctrl_sgl_supported / nvme_ctrl_meta_sgl_supported. */
static inline bool nvme_pci_ctrl_sgl_dword_only(const struct nvme_ctrl *ctrl)
{
	return (ctrl->sgls & NVME_CTRL_SGLS_SUPP_MASK) ==
	       NVME_CTRL_SGLS_DWORD_ALIGNED;
}

/* Bytes of BAR0 that hold the registers and the doorbells of
 * nr_io_queues I/O queues plus the admin queue. */
static inline unsigned long nvme_db_bar_size(const struct nvme_dev *dev,
					     unsigned int nr_io_queues)
{
	return NVME_REG_DBS +
	       ((unsigned long)(nr_io_queues + 1) * 8u * dev->db_stride);
}

/* Time-window rate limit of log messages, the established default: at
 * most NVME_PCI_RL_BURST messages per NVME_PCI_RL_INTERVAL_NS per call
 * site.  Races between CPUs only blur the count. */
#define NVME_PCI_RL_INTERVAL_NS (5ull * 1000000000ull)
#define NVME_PCI_RL_BURST 10u
struct nvme_pci_ratelimit {
	uint64_t begin_ns;
	uint32_t printed;
	uint32_t missed;
};
/* Called from every queue's interrupt at once: the state is shared,
 * so it is read and written with relaxed atomics (a lost update only
 * shifts the budget by one message). */
static inline bool nvme_pci_ratelimit_ok(struct nvme_pci_ratelimit *rl)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t begin = __atomic_load_n(&rl->begin_ns, __ATOMIC_RELAXED);

	if (!begin || now - begin >= NVME_PCI_RL_INTERVAL_NS) {
		uint32_t missed = __atomic_exchange_n(&rl->missed, 0,
						      __ATOMIC_RELAXED);

		if (missed)
			kprintf(NVME_LOG_PREFIX "%u messages suppressed\n",
				missed);
		__atomic_store_n(&rl->begin_ns, now ? now : 1,
				 __ATOMIC_RELAXED);
		__atomic_store_n(&rl->printed, 0, __ATOMIC_RELAXED);
	}
	if (__atomic_add_fetch(&rl->printed, 1, __ATOMIC_RELAXED) <=
	    NVME_PCI_RL_BURST)
		return true;
	__atomic_add_fetch(&rl->missed, 1, __ATOMIC_RELAXED);
	return false;
}
#define nvme_pci_warn_ratelimited(fmt, ...)                               \
	do {                                                              \
		static struct nvme_pci_ratelimit __nvme_rl;               \
		if (nvme_pci_ratelimit_ok(&__nvme_rl))                    \
			kprintf(fmt, ##__VA_ARGS__);                      \
	} while (0)

/* Milliseconds of the core's admin / I/O timeouts. */
static inline unsigned long nvme_pci_admin_timeout_ms(void)
{
	unsigned int s = nvme_core_param.admin_timeout_s;

	return (unsigned long)(s ? s : NVME_DEFAULT_ADMIN_TIMEOUT_S) * 1000ul;
}
static inline unsigned long nvme_pci_io_timeout_ms(void)
{
	unsigned int s = nvme_core_param.io_timeout_s;

	return (unsigned long)(s ? s : NVME_DEFAULT_IO_TIMEOUT_S) * 1000ul;
}

/* ==========================================================================
 * Cross-file prototypes of the transport
 * ========================================================================== */

/* --- nvme_pci.c ------------------------------------------------------------ */
extern const struct nvme_ctrl_ops nvme_pci_ctrl_ops;
extern struct pci_driver nvme_pci_driver;
/* Disable the controller: freeze/quiesce, delete I/O queues (or not when
 * dead), CC.EN=0 or CC.SHN when `shutdown', free interrupts, cancel what
 * is left, keep the admin queue's memory.  Takes shutdown_lock.  May
 * sleep. */
void nvme_dev_disable(struct nvme_dev *dev, bool shutdown);
/* Wait for a running reset to finish, then nvme_dev_disable: -EBUSY when
 * the controller did not come out of the reset LIVE.  May sleep. */
int nvme_disable_prepare_reset(struct nvme_dev *dev, bool shutdown);
void nvme_reset_work(void *arg); /* blk_work fn, arg = dev */
/* The tail of a failed bring-up: DELETING, disable with shutdown, mark
 * the namespaces dead (their I/O fails at once), DEAD.  The controller
 * object stays until the PCI function is removed.  May sleep. */
void nvme_remove_dead_ctrl(struct nvme_dev *dev);
/* Map / unmap BAR0 (at least `size' bytes). */
int nvme_remap_bar(struct nvme_dev *dev, unsigned long size);
/* Is the controller in a state where it cannot be recovered (CFS, gone)? */
bool nvme_should_reset(struct nvme_dev *dev, uint32_t csts);
/* Log why a controller is being reset (CSTS, PCI status). */
void nvme_warn_reset(struct nvme_dev *dev, uint32_t csts);
/* Enable the function, read CAP, size the queues, map the CMB/PMR,
 * configure the admin queue.  shutdown_lock held.  May sleep. */
int nvme_pci_enable(struct nvme_dev *dev);
/* PCI driver callbacks (also the targets of nvme_pci_driver). */
int nvme_pci_probe(pci_device_t *pdev, const struct pci_device_id *id);
void nvme_pci_remove(pci_device_t *pdev);
void nvme_pci_shutdown(pci_device_t *pdev);

/* --- nvme_pci_queue.c ------------------------------------------------------ */
extern const struct blk_mq_ops nvme_mq_admin_ops;
extern const struct blk_mq_ops nvme_mq_ops;
int nvme_pci_alloc_queue(struct nvme_dev *dev, int qid, int depth);
void nvme_free_queues(struct nvme_dev *dev, int lowest);
int nvme_pci_configure_admin_queue(struct nvme_dev *dev);
int nvme_create_io_queues(struct nvme_dev *dev);
void nvme_delete_io_queues(struct nvme_dev *dev);
int nvme_setup_io_queues(struct nvme_dev *dev);
void nvme_suspend_io_queues(struct nvme_dev *dev);
void nvme_suspend_queue(struct nvme_dev *dev, unsigned int qid);
void nvme_reap_pending_cqes(struct nvme_dev *dev);
/* Copy `cmd' into the SQ at the tail and advance it.  sq_lock held.  Any
 * context. */
void nvme_sq_copy_cmd(struct nvme_queue *nvmeq, struct nvme_command *cmd);
/* Ring the SQ doorbell when `write_sq' or the next command would reach
 * the last tail written (a full ring).  sq_lock held.  Any context. */
void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq);
/* Process the CQ: completed requests to iob (NULL: complete them).
 * Returns the number of CQEs consumed.  Interrupt handler: own queue
 * only, no lock; poll / timeout: cq_poll_lock held. */
int nvme_poll_cq(struct nvme_queue *nvmeq, struct io_comp_batch *iob);
/* Is a new CQE waiting at the head (phase tag matches)?  Any context. */
bool nvme_cqe_pending(struct nvme_queue *nvmeq);
/* Poll an interrupt driven queue with its interrupt masked (timeout,
 * disable).  May sleep (disable_irq waits for a running handler). */
void nvme_poll_irqdisable(struct nvme_queue *nvmeq);
/* Shadow doorbell: update the doorbell value in the DBBUF page and tell
 * whether the MMIO doorbell must be written (event index crossed). */
bool nvme_dbbuf_update_and_check_event(uint16_t value, __le32 *dbbuf_db,
				       volatile __le32 *dbbuf_ei);
int nvme_dbbuf_dma_alloc(struct nvme_dev *dev);
void nvme_dbbuf_dma_free(struct nvme_dev *dev);
void nvme_dbbuf_set(struct nvme_dev *dev);
void nvme_dbbuf_init(struct nvme_dev *dev, struct nvme_queue *nvmeq, int qid);
void nvme_dbbuf_free(struct nvme_queue *nvmeq);
/* blk_mq timeout handler: CSTS.CFS check, CQ poll, abort (abort_limit),
 * reset. */
enum blk_eh_timer_return nvme_timeout(struct blk_request *rq);
unsigned int nvme_max_io_queues(struct nvme_dev *dev);
/* CMB-resident SQ depth for nr_io_queues queues of entry_size bytes:
 * the depth, or -ENOMEM when the CMB is too small to be worth it. */
int nvme_cmb_qdepth(struct nvme_dev *dev, int nr_io_queues, int entry_size);
/* After a reset: drop the queues beyond the ones that came back and
 * resize the tag set (or allocate it the first time).  false when racing
 * a disable.  May sleep. */
bool nvme_pci_update_nr_queues(struct nvme_dev *dev);
unsigned int nvme_pci_nr_maps(struct nvme_dev *dev);
/* The admin queue's tag set goes (removal / failed probe). */
void nvme_dev_remove_admin(struct nvme_dev *dev);
void nvme_free_tagset(struct nvme_dev *dev);
/* Batch completion of the transport (unmap + the core's batch end). */
void nvme_pci_complete_batch(struct io_comp_batch *iob);
void nvme_pci_complete_rq(struct blk_request *rq);
/* AEN submission (ctrl ops). */
void nvme_pci_submit_async_event(struct nvme_ctrl *ctrl);

/* --- nvme_pci_dma.c --------------------------------------------------------- */
/* Map the request's data (PRP or SGL by the threshold) and metadata into
 * iod / the SQE.  BLK_STS_OK, BLK_STS_RESOURCE (pool empty),
 * BLK_STS_IOERR / BLK_STS_TARGET (not mappable).  Any context. */
blk_status_t nvme_map_data(struct nvme_dev *dev, struct blk_request *rq,
			   struct nvme_command *cmnd);
blk_status_t nvme_map_metadata(struct nvme_dev *dev, struct blk_request *rq,
			       struct nvme_command *cmnd);
void nvme_unmap_data(struct nvme_dev *dev, struct blk_request *rq);
/* blk_mq_ops.cancel of the I/O queues (nvme_pci_queue.c): NVMe Cancel on
 * the command's own SQ, or an Abort.  Never ends the request itself. */
int nvme_pci_cancel_request(struct blk_request *rq);
void nvme_unmap_metadata(struct nvme_dev *dev, struct blk_request *rq);
/* Should this request's data go out as an SGL rather than PRPs?  nseg:
 * the physical segments it has (forced for gaps PRPs cannot describe,
 * user commands and several integrity segments; else by the average
 * segment size against nvme.sgl_threshold). */
bool nvme_pci_use_sgls(struct nvme_dev *dev, struct blk_request *rq,
		       int nseg);
int nvme_setup_descriptor_pools(struct nvme_dev *dev);
void nvme_release_descriptor_pools(struct nvme_dev *dev);
/* CMB / PMR / HMB. */
void nvme_map_cmb(struct nvme_dev *dev);
void nvme_release_cmb(struct nvme_dev *dev);
/* Carve a CMB-resident SQ of `size' bytes (page aligned): its kernel
 * address and bus address, NULL when the CMB has no room.  Freed in
 * reverse order of allocation (nvme_free_queues frees top down). */
void *nvme_cmb_alloc_sq(struct nvme_dev *dev, uint64_t size, uint64_t *bus,
			uint64_t *offset);
void nvme_cmb_free_sq(struct nvme_dev *dev, uint64_t offset, uint64_t size);
int nvme_pmr_enable(struct nvme_dev *dev);
void nvme_pmr_disable(struct nvme_dev *dev);
int nvme_setup_host_mem(struct nvme_dev *dev);
int nvme_set_host_mem(struct nvme_dev *dev, uint32_t bits);
void nvme_free_host_mem(struct nvme_dev *dev);

/* --- nvme_pci_irq.c ---------------------------------------------------------- */
/* Allocate vectors for nr_io_queues (+1 pre-vector for the admin queue)
 * with the default/read set split; returns vectors or -errno. */
int nvme_setup_irqs(struct nvme_dev *dev, unsigned int nr_io_queues);
int nvme_pci_request_queue_irq(struct nvme_queue *nvmeq);
void nvme_pci_free_queue_irq(struct nvme_queue *nvmeq);
int nvme_irq(void *data); /* irq_handler_t: IRQ_HANDLED/IRQ_NONE */
int nvme_irq_check(void *data); /* threaded primary */
void nvme_calc_irq_sets(struct irq_affinity *affd, unsigned int nrirqs);
/* Interrupt coalescing (Set Features 08h): time in 100 us, threshold. */
int nvme_set_irq_coalescing(struct nvme_dev *dev, uint8_t time_100us,
			    uint8_t threshold);
/* The admin queue's vector offset of the I/O queues' interrupts: 1 when
 * the admin queue has a vector of its own, else 0. */
int nvme_queue_irq_offset(struct nvme_dev *dev);
/* blk_mq_ops->map_queues of the I/O tag set. */
void nvme_pci_map_queues(struct blk_mq_tag_set *set);

/* --- nvme_pci_pm.c ------------------------------------------------------------ */
int nvme_pci_suspend(pci_device_t *pdev);
int nvme_pci_resume(pci_device_t *pdev);
bool nvme_pci_simple_suspend(struct nvme_dev *dev);
void nvme_pci_configure_aspm(struct nvme_dev *dev);
/* Freeze/poweroff-style suspend and its resume (full shutdown, reset on
 * resume). */
int nvme_pci_simple_suspend_dev(struct nvme_dev *dev);
int nvme_pci_simple_resume_dev(struct nvme_dev *dev);

/* --- nvme_pci_quirks.c ------------------------------------------------------ */
/* The match table of nvme_pci_driver: every NVMe class device plus the
 * vendor/device entries carrying quirk flags in driver_data. */
extern const struct pci_device_id nvme_id_table[];
/* Quirks from nvme.quirks= for this device (enabled, disabled masks). */
void nvme_pci_cmdline_quirks(const pci_device_t *pdev, unsigned long *quirks);
/* Parse nvme.quirks= (nvme_pci_param.quirks) into the dynamic quirk
 * list; logs and keeps nothing on a malformed string. */
void nvme_pci_quirks_init(void);
/* Platform (ACPI/DMI-style) decisions: simple suspend, no APST, ... */
unsigned long nvme_pci_platform_quirks(const pci_device_t *pdev);
/* Does the platform ask for D3 of this storage device during suspend
 * (ACPI _DSD "StorageD3Enable")? */
bool nvme_pci_acpi_storage_d3(const pci_device_t *pdev);

/* ==========================================================================
 * Compile-time checks
 * ========================================================================== */

static inline void _nvme_pci_check(void)
{
	BUILD_BUG_ON(__builtin_offsetof(struct nvme_iod, req) != 0);
	BUILD_BUG_ON(MAX_PRP_RANGE / NVME_CTRL_PAGE_SIZE >
		     1 + NVME_MAX_NR_DESCRIPTORS * PRPS_PER_PAGE);
	BUILD_BUG_ON(NVME_PCI_MAX_QUEUE_SIZE >= NVME_MAX_TAGS_PER_QUEUE);
	BUILD_BUG_ON(sizeof(struct nvme_sgl_desc) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_create_cq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_sq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_delete_queue) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_host_mem_buf_desc) != 16);
	BUILD_BUG_ON(IRQ_AFFINITY_MAX_SETS < 2);
	BUILD_BUG_ON(NVME_SMALL_POOL_SIZE % sizeof(struct nvme_sgl_desc));
	BUILD_BUG_ON(__builtin_offsetof(union nvme_data_ptr, prp1) !=
		     __builtin_offsetof(union nvme_data_ptr, sgl.addr));
	BUILD_BUG_ON(NVME_MAX_META_SEGS != 15);
	BUILD_BUG_ON(NVME_MAX_SEGS != 256);
}

#endif /* KERNEL_DEV_NVME_NVME_PCI_H */
