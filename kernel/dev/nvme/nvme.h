// LikeOS - NVMe host driver: core objects and the contract between its parts
//
// The NVMe driver is split the established way:
//
//   CORE (transport independent)
//     nvme_core.c        controller lifetime, state machine, subsystems,
//                        quirks by model/firmware, init/start/stop/remove,
//                        keep-alive, AEN handling, firmware activation,
//                        power states/APST, host behavior, timestamp,
//                        work items, boot command line knobs     (P3)
//     nvme_admin.c       admin tag set and queue, sync/async command
//                        submission, retries (CRD/ACRE), identify, get/set
//                        features, get log page (paged, LPO)       (P3)
//     nvme_constants.c   names of opcodes, status codes, status types (P3)
//     nvme_hwmon.c       SMART/health watch, temperatures, error log (P3)
//     nvme_sysfs.c       /sys/class/nvme/nvmeX/*, subsystem attributes (P3)
//     nvme_ns.c          namespace scan/validate/revalidate/remove,
//                        gendisk registration, block limits, NS
//                        management, format, sanitize                (P4)
//     nvme_io.c          nvme_setup_cmd (every I/O opcode), PI/reftag,
//                        nvme_complete_rq / disposition / errno      (P4)
//     nvme_mpath.c       native multipath: ns heads, path selection,
//                        ANA log and states, ANATT, failover         (P4)
//     nvme_zns.c         zoned namespaces                            (P4)
//     nvme_pr.c          persistent reservations (pr_ops)            (P4)
//     nvme_feat_ns.c     directives/streams, FDP, endurance groups and
//                        NVM sets, write streams                     (P4)
//     nvme_ioctl.c       NVME_IOCTL_* pass-through and block ioctls  (P6)
//     nvme_chrdev.c      /dev/nvmeX and /dev/ngXnY character devices (P6)
//     nvme_admin_ops.c   firmware download/commit, self-test, telemetry,
//                        persistent event log, security send/recv    (P6)
//     nvme_debug.c       debugfs-style dumps, command trace ring     (P6)
//     nvme_fault_inject.c  status/timeout/abort/reset injection      (P6)
//   PCIe TRANSPORT (nvme_pci.h)
//     nvme_pci.c, nvme_pci_queue.c, nvme_pci_dma.c, nvme_pci_irq.c,
//     nvme_pci_pm.c, nvme_pci_quirks.c                               (P5)
//
// ---- Lock order (MANDATORY, outermost first) -------------------------------
//   1 ctrl->shutdown_lock (mutex)      controller disable/enable: reset,
//                                      shutdown, suspend, removal
//   2 ctrl->scan_lock (mutex)          namespace scan/registration/removal
//   2a nvme_subsystems_lock (mutex)    the subsystem registry; a
//                                      subsystem's ctrls list changes under
//                                      it AND subsys->lock (either one
//                                      suffices to read it)
//   3 subsys->lock (mutex)             subsystem ctrl list + ns_head list
//   4 ctrl->namespaces_lock (mutex)    the namespace list
//   5 g_disk_registry_mutex            block layer (never held while calling
//                                      a driver op)
//   6 ctrl->lock (spinlock, irqsave)   state machine, AEN/shared fields
//   7 nvmeq->sq_lock (spinlock, irqsave) SQ tail, shadow doorbell, doorbell
//                                      write, command copy-in/CID
//   8 nvmeq->cq_poll_lock (spin, irqsave) CQ processing of poll queues and
//                                      the timeout-poll path
//   9 block-layer leaf locks (hctx, tags, timer)
// Further rules:
//   - ctrl->state changes ONLY through nvme_change_ctrl_state(), under
//     ctrl->lock, following nvme_ctrl_state_transitions[] (below).
//   - Exactly one reset_work per controller; the RESETTING state is the
//     gate: nvme_reset_ctrl() only queues it after a successful transition
//     to RESETTING, so two recoveries never run at once.
//   - Synchronous admin/pass-through commands wait on a struct completion
//     (kernel/ke/completion.h) through blk_execute_rq(); asynchronous I/O
//     completes through end_io.  NO spinlock is ever held while sleeping,
//     waiting for I/O or calling anything that may sleep.
//   - An interrupt handler touches only its own queue (one CQ per vector).
//   - Hot-removal, shutdown and removal take shutdown_lock, then scan_lock.
//   - namespaces_lock is held only around list changes and walks, never
//     across a command or a wait (recovery walks the list to freeze /
//     quiesce while scan may be blocked behind the recovery).
//
// ---- Lifetimes (refcount_t) --------------------------------------------------
//   struct nvme_ctrl       ctrl->ref: the transport's (probe..remove), each
//                          open /dev/nvmeX, each namespace, each in-flight
//                          work item that may outlive removal.  The last
//                          nvme_put_ctrl() calls ops->free_ctrl().
//   struct nvme_subsystem  subsys->ref: each controller in it, each ns head.
//   struct nvme_ns_head    head->ref: each path (ns), each open of the head
//                          node, the multipath disk.
//   struct nvme_ns         ns->ref: the namespace list, each open of its
//                          disk / generic node, each in-flight request
//                          that looked it up.
//
// ---- Context conventions -------------------------------------------------------
//   "may sleep"   process context (a kblockd work item, the boot thread
//                 after nvme_late_init, an ioctl); might_sleep() asserted.
//   "any context" interrupts may be off / an interrupt handler.
//   Work items are struct blk_work run on kblockd (process context, may
//   sleep; one item never runs twice at once).
//   Return values: 0 / -errno; functions returning an NVMe status say so
//   (> 0 is an NVMe status as nvme_req()->status stores it, see
//   nvme_spec.h, < 0 a -errno for a failure around the command).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_NVME_NVME_H
#define KERNEL_DEV_NVME_NVME_H

#include <kernel/dev/nvme/nvme_spec.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_mq.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/completion.h>
#include <kernel/ke/ida.h>
#include <kernel/ke/list.h>
#include <kernel/ke/io.h>
#include <kernel/fs/device.h>
#include <kernel/uapi/bug.h>
#include <kernel/uapi/nvme_ioctl.h>
#include <kernel/uapi/pr.h>

struct nvme_ctrl;
struct nvme_ns;
struct nvme_ns_head;
struct nvme_subsystem;
struct pfs_node;

#define NVME_LOG_PREFIX "nvme: "

/* Error numbers the NVMe paths return that kernel/ke/syscall.h does not
 * define (the userland values, user/lib/libc/include/errno.h). */
#ifndef ENETRESET
#define ENETRESET 102 /* reset did not bring the controller back */
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

/* ==========================================================================
 * Driver-wide constants
 * ========================================================================== */

/* The memory page size the driver programs into CC.MPS and builds PRPs
 * for: 4 KiB on every controller (CAP.MPSMIN must allow it). */
#define NVME_CTRL_PAGE_SHIFT 12
#define NVME_CTRL_PAGE_SIZE (1u << NVME_CTRL_PAGE_SHIFT)

/* Admin queue: 32 entries, one tag reserved per outstanding asynchronous
 * event request (NVME_NR_AEN_COMMANDS), the rest for the block layer. */
#define NVME_AQ_DEPTH 32
#define NVME_NR_AEN_COMMANDS 1
#define NVME_AQ_BLK_MQ_DEPTH (NVME_AQ_DEPTH - NVME_NR_AEN_COMMANDS)
/* Admin tag set depth: one less than the SQ (a full SQ is tail == head-1),
 * the AEN command IDs above it. */
#define NVME_AQ_MQ_TAG_DEPTH (NVME_AQ_BLK_MQ_DEPTH - 1)

/* Command ID = generation counter (4 bits) << 12 | tag (12 bits): a
 * completion carrying a stale generation is rejected (nvme_find_rq). */
#define nvme_genctr_mask(gen) ((gen) & 0xf)
#define nvme_cid_install_genctr(gen) (nvme_genctr_mask(gen) << 12)
#define nvme_genctr_from_cid(cid) (((cid) & 0xf000) >> 12)
#define nvme_tag_from_cid(cid) ((cid) & 0xfff)
#define NVME_MAX_TAGS_PER_QUEUE 4096 /* what 12 tag bits address */

/* Defaults of the boot command line knobs (struct nvme_core_params). */
#define NVME_DEFAULT_ADMIN_TIMEOUT_S 60
#define NVME_DEFAULT_IO_TIMEOUT_S 30
#define NVME_DEFAULT_SHUTDOWN_TIMEOUT_S 5
#define NVME_DEFAULT_MAX_RETRIES 5
#define NVME_DEFAULT_KATO_S 5 /* keep-alive timeout (when required) */
#define NVME_DEFAULT_PS_MAX_LATENCY_US 100000
#define NVME_DEFAULT_APST_PRIMARY_TIMEOUT_MS 100
#define NVME_DEFAULT_APST_SECONDARY_TIMEOUT_MS 2000
#define NVME_DEFAULT_APST_PRIMARY_LATENCY_TOL_US 15000
#define NVME_DEFAULT_APST_SECONDARY_LATENCY_TOL_US 100000

/* NVME_QUIRK_DELAY_BEFORE_CHK_RDY: wait this long (ms) after toggling
 * CC.EN before looking at CSTS.RDY. */
#define NVME_QUIRK_DELAY_AMOUNT 2300

/* Retry delays: CRD index -> ctrl->crdt[] in 100 ms units. */
#define NVME_CRDT_UNIT_MS 100

/* The longest identity strings of struct nvme_subsystem (spec sizes). */
#define NVME_SERIAL_LEN 20
#define NVME_MODEL_LEN 40
#define NVME_FW_REV_LEN 8

/* Names: "nvme%d" (controller), "nvme%dn%d" (namespace, or subsystem
 * instance + head instance under multipath), "nvme%dc%dn%d" (hidden path
 * disks), "ng%dn%d" (generic char node), "nvme-subsys%d". */
#define NVME_NAME_LEN 24

/* ==========================================================================
 * Boot command line knobs (module-parameter equivalents)
 * ==========================================================================
 *
 * Parsed once by nvme_core_parse_cmdline() (P3) from the kernel command
 * line ("nvme_core.<name>=<value>", the established parameter names) at
 * nvme_init(); read-only afterwards.  The transport's own knobs are in
 * nvme_pci.h ("nvme.<name>="). */
struct nvme_core_params {
	unsigned int admin_timeout_s; /* nvme_core.admin_timeout */
	unsigned int io_timeout_s; /* nvme_core.io_timeout */
	unsigned int shutdown_timeout_s; /* nvme_core.shutdown_timeout */
	unsigned int max_retries; /* nvme_core.max_retries */
	uint64_t default_ps_max_latency_us; /* .default_ps_max_latency_us */
	int force_apst; /* nvme_core.force_apst */
	unsigned int apst_primary_timeout_ms;
	unsigned int apst_secondary_timeout_ms;
	unsigned int apst_primary_latency_tol_us;
	unsigned int apst_secondary_latency_tol_us;
	int multipath; /* nvme_core.multipath (default 1) */
	int multipath_always_on; /* nvme_core.multipath_always_on */
	int iopolicy; /* enum nvme_iopolicy, nvme_core.iopolicy */
	int streams; /* nvme_core.streams: enable write streams */
	int passthru_err_log; /* log pass-through command errors */
	unsigned long quirks_add; /* nvme_core.quirks=VID:DID:hexmask added */
	uint16_t quirks_vid, quirks_did;
};
extern struct nvme_core_params nvme_core_param;

/* ==========================================================================
 * Quirks
 * ========================================================================== */

/* struct nvme_ctrl.quirks bits.  The semantics are the established ones;
 * the per-PCI-ID table is nvme_pci_quirks.c (P5), the model/firmware
 * table nvme_core.c (P3). */
enum nvme_quirks {
	/* Prefers I/O aligned to a vendor-specific stripe size. */
	NVME_QUIRK_STRIPE_SIZE = (1 << 0),
	/* Identify CNS 01h (controller) is not supported: no namespace
	 * list, scan sequentially. */
	NVME_QUIRK_IDENTIFY_CNS = (1 << 1),
	/* Deallocated blocks read back zero (discard implies zeroes). */
	NVME_QUIRK_DEALLOCATE_ZEROES = (1 << 2),
	/* Wait NVME_QUIRK_DELAY_AMOUNT ms before checking CSTS.RDY. */
	NVME_QUIRK_DELAY_BEFORE_CHK_RDY = (1 << 3),
	/* APST is broken: never enable it. */
	NVME_QUIRK_NO_APST = (1 << 4),
	/* Never enter the deepest power state. */
	NVME_QUIRK_NO_DEEPEST_PS = (1 << 5),
	/* Only one outstanding I/O at a time. */
	NVME_QUIRK_QDEPTH_ONE = (1 << 6),
	/* Create I/O SQs with medium priority. */
	NVME_QUIRK_MEDIUM_PRIO_SQ = (1 << 7),
	/* The subsystem NQN the device reports is not unique. */
	NVME_QUIRK_IGNORE_DEV_SUBNQN = (1 << 8),
	/* Write Zeroes is broken. */
	NVME_QUIRK_DISABLE_WRITE_ZEROES = (1 << 9),
	/* Suspend with a full shutdown, not APST/host-managed PS. */
	NVME_QUIRK_SIMPLE_SUSPEND = (1 << 10),
	/* Use one interrupt vector for everything. */
	NVME_QUIRK_SINGLE_VECTOR = (1 << 11),
	/* SQ entries are 128 bytes (some Apple controllers). */
	NVME_QUIRK_128_BYTES_SQES = (1 << 12),
	/* Admin and I/O queues share one tag space (Apple). */
	NVME_QUIRK_SHARED_TAGS = (1 << 13),
	/* Do not change the temperature thresholds. */
	NVME_QUIRK_NO_TEMP_THRESH_CHANGE = (1 << 14),
	/* Identify Namespace Descriptor List is broken. */
	NVME_QUIRK_NO_NS_DESC_LIST = (1 << 15),
	/* DMA addresses are limited to 48 bits. */
	NVME_QUIRK_DMA_ADDRESS_BITS_48 = (1 << 16),
	/* Do not put a generation counter into command IDs. */
	NVME_QUIRK_SKIP_CID_GEN = (1 << 17),
	/* Namespace identifiers (EUI64/NGUID/UUID) are not unique. */
	NVME_QUIRK_BOGUS_NID = (1 << 18),
	/* No secondary temperature threshold support. */
	NVME_QUIRK_NO_SECONDARY_TEMP_THRESH = (1 << 19),
	/* Never use simple suspend even where the platform asks for it. */
	NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND = (1 << 20),
	/* MSI is broken: MSI-X or INTx only. */
	NVME_QUIRK_BROKEN_MSI = (1 << 21),
	/* PRP list pools must be 512-byte aligned. */
	NVME_QUIRK_DMAPOOL_ALIGN_512 = (1 << 22),
};

/* "stripe_size", "no_apst" ... (nvme_constants.c, P3). */
const char *nvme_quirk_name(enum nvme_quirks q);

/* ==========================================================================
 * Controller state machine
 * ========================================================================== */

enum nvme_ctrl_state {
	NVME_CTRL_NEW = 0,
	NVME_CTRL_LIVE,
	NVME_CTRL_RESETTING,
	NVME_CTRL_CONNECTING,
	NVME_CTRL_DELETING,
	NVME_CTRL_DELETING_NOIO,
	NVME_CTRL_DEAD,
	NVME_CTRL_NR_STATES
};

/* nvme_ctrl_state_transitions[new][old] != 0: new may follow old.  The
 * established table (defined in nvme_core.c, P3):
 *   LIVE           <- CONNECTING
 *   RESETTING      <- NEW, LIVE
 *   CONNECTING     <- NEW, RESETTING
 *   DELETING       <- LIVE, RESETTING, CONNECTING
 *   DELETING_NOIO  <- DELETING, DEAD
 *   DEAD           <- DELETING
 * (NEW is never entered.)  Any other request fails and changes nothing.
 * So a transport brings a controller up NEW -> CONNECTING -> LIVE (or
 * NEW -> RESETTING -> CONNECTING -> LIVE when probe queues a reset) and
 * recovers it LIVE -> RESETTING -> CONNECTING -> LIVE. */
extern const uint8_t nvme_ctrl_state_transitions[NVME_CTRL_NR_STATES]
						[NVME_CTRL_NR_STATES];
/* "new", "live", "resetting" ... (sysfs state attribute). */
const char *nvme_ctrl_state_name(enum nvme_ctrl_state state);

/* struct nvme_ctrl.flags bits (atomic bit operations). */
enum nvme_ctrl_flags {
	NVME_CTRL_FAILFAST_EXPIRED = 0,
	NVME_CTRL_ADMIN_Q_STOPPED = 1,
	NVME_CTRL_STARTED_ONCE = 2,
	NVME_CTRL_STOPPED = 3,
	NVME_CTRL_SKIP_ID_CNS_CS = 4,
	NVME_CTRL_DIRTY_CAPABILITY = 5,
	NVME_CTRL_FROZEN = 6,
	/* nvme_remove_ctrl() ran (P3): never tear the core down twice. */
	NVME_CTRL_REMOVED = 7,
};

/* struct nvme_ctrl.events bits (pending AEN work). */
#define NVME_AER_NOTICE_NS_CHANGED_EVENT 0
#define NVME_AER_NOTICE_FW_ACT_STARTING_EVENT 1

static inline int nvme_test_bit(unsigned int nr, const volatile unsigned long *w)
{
	return (int)((__atomic_load_n(w, __ATOMIC_ACQUIRE) >> nr) & 1UL);
}
static inline void nvme_set_bit(unsigned int nr, volatile unsigned long *w)
{
	__atomic_fetch_or(w, 1UL << nr, __ATOMIC_ACQ_REL);
}
static inline void nvme_clear_bit(unsigned int nr, volatile unsigned long *w)
{
	__atomic_fetch_and(w, ~(1UL << nr), __ATOMIC_ACQ_REL);
}
/* Returns the old value of the bit. */
static inline int nvme_test_and_set_bit(unsigned int nr,
					volatile unsigned long *w)
{
	return (int)((__atomic_fetch_or(w, 1UL << nr, __ATOMIC_ACQ_REL) >>
		      nr) &
		     1UL);
}
static inline int nvme_test_and_clear_bit(unsigned int nr,
					  volatile unsigned long *w)
{
	return (int)((__atomic_fetch_and(w, ~(1UL << nr), __ATOMIC_ACQ_REL) >>
		      nr) &
		     1UL);
}

/* ==========================================================================
 * Per-command state (the start of every transport's block-layer PDU)
 * ========================================================================== */

/* struct nvme_request.flags */
enum {
	NVME_REQ_CANCELLED = (1 << 0), /* completed by cancel/abort path */
	NVME_REQ_USERCMD = (1 << 1), /* a user pass-through command */
	NVME_MPATH_IO_STATS = (1 << 2), /* counted on the head's disk */
	NVME_MPATH_CNT_ACTIVE = (1 << 3), /* counted in ctrl->nr_active */
	/* (P4) the command carries a payload that is not the request's
	 * data (the Dataset Management range list): the transport maps
	 * special_vec/special_len as the data buffer, nvme_cleanup_cmd()
	 * frees it. */
	NVME_REQ_SPECIAL_PAYLOAD = (1 << 4),
	/* The host asked the device to cancel the command (Cancel, or an
	 * Abort in its place) for a submitter that gave up on it: whatever
	 * status it ends with is final, never retried or failed over. */
	NVME_REQ_HOST_CANCEL = (1 << 5),
};

/* The driver data of every block-layer request on an NVMe queue: the
 * FIRST member of the transport's PDU (struct nvme_iod for PCIe), so
 * nvme_req(rq) is blk_mq_rq_to_pdu(rq). */
struct nvme_request {
	struct nvme_command *cmd; /* the SQE being built / sent */
	union nvme_result result; /* completion dword 0/1 */
	uint8_t genctr; /* command ID generation (4 bits used) */
	uint8_t retries; /* times retried so far */
	uint8_t flags; /* NVME_REQ_* */
	uint8_t pad;
	uint16_t status; /* NVMe status (CQE status >> 1) or
			  * NVME_SC_HOST_* when the host failed it */
	uint16_t pad2;
	uint64_t start_time_ns; /* multipath statistics / latency */
	struct nvme_ctrl *ctrl;
	/* (P4) NVME_REQ_SPECIAL_PAYLOAD: kernel virtual buffer and length
	 * (physically contiguous within each 4 KiB page; the transport
	 * translates per page). */
	void *special_vec;
	uint32_t special_len;
	uint32_t pad3;
};

static inline struct nvme_request *nvme_req(struct blk_request *rq)
{
	return (struct nvme_request *)blk_mq_rq_to_pdu(rq);
}

/* The generation counter of a request: advanced when it completes, read by
 * the CQ handler of another processor checking a (possibly stale) CQE. */
static inline uint8_t nvme_req_genctr(struct nvme_request *nr)
{
	return __atomic_load_n(&nr->genctr, __ATOMIC_RELAXED);
}

/* The command ID of `rq': its tag with the generation counter on top. */
static inline uint16_t nvme_cid(struct blk_request *rq)
{
	return (uint16_t)(nvme_cid_install_genctr(nvme_req_genctr(nvme_req(rq))) |
			  (uint16_t)rq->tag);
}

/* Queue ID of a request: 0 for the admin queue (no queuedata), hctx
 * index + 1 for an I/O queue. */
static inline uint16_t nvme_req_qid(struct blk_request *rq)
{
	if (!rq->queue || !rq->queue->queuedata || !rq->mq_hctx)
		return 0;
	return (uint16_t)(rq->mq_hctx->queue_num + 1);
}

/* What to do with a completed request (nvme_decide_disposition, P4). */
enum nvme_disposition {
	COMPLETE, /* end it with its status */
	RETRY, /* requeue (after the CRD delay) */
	FAILOVER, /* multipath: try another path */
	AUTHENTICATE, /* (fabrics only: never returned here) */
};

/* ==========================================================================
 * Transport operations
 * ========================================================================== */

/* struct nvme_ctrl_ops.flags */
#define NVME_F_FABRICS (1u << 0) /* never set: no fabrics in LikeOS */
#define NVME_F_METADATA_SUPPORTED (1u << 1) /* separate metadata buffers */
#define NVME_F_BLOCKING (1u << 2) /* queue_rq may sleep */

struct nvme_ctrl_ops {
	const char *name; /* "pcie" */
	unsigned int flags; /* NVME_F_* */
	/* Register access (CAP/VS/CC/CSTS/...).  0 or -errno (-ENODEV when
	 * the device is gone: reads of all-ones on PCIe).  Any context. */
	int (*reg_read32)(struct nvme_ctrl *ctrl, uint32_t off, uint32_t *val);
	int (*reg_write32)(struct nvme_ctrl *ctrl, uint32_t off, uint32_t val);
	int (*reg_read64)(struct nvme_ctrl *ctrl, uint32_t off, uint64_t *val);
	/* The last reference is gone: free the transport's controller
	 * (struct nvme_dev) and everything it owns.  May sleep. */
	void (*free_ctrl)(struct nvme_ctrl *ctrl);
	/* Post one Asynchronous Event Request on the admin queue, with a
	 * command ID above the block layer's tags (NVME_AQ_BLK_MQ_DEPTH).
	 * Any context (it takes the admin queue's sq_lock). */
	void (*submit_async_event)(struct nvme_ctrl *ctrl);
	/* NVM subsystem reset (NSSR): -ENOTTY when not supported.  May
	 * sleep. */
	int (*subsystem_reset)(struct nvme_ctrl *ctrl);
	/* Delete the controller (transport teardown after
	 * nvme_remove_namespaces).  May sleep. */
	void (*delete_ctrl)(struct nvme_ctrl *ctrl);
	/* Stop transport-specific work (nvme_stop_ctrl).  May sleep. */
	void (*stop_ctrl)(struct nvme_ctrl *ctrl);
	/* "0000:01:00.0" for the sysfs address attribute; bytes written. */
	int (*get_address)(struct nvme_ctrl *ctrl, char *buf, int size);
	/* Log the controller identity on errors (optional). */
	void (*print_device_info)(struct nvme_ctrl *ctrl);
	/* Peer-to-peer DMA possible (CMB data buffers)?  Optional. */
	bool (*supports_pci_p2pdma)(struct nvme_ctrl *ctrl);
	/* virt_boundary_mask for the queues (PRP: page - 1, SGL-only
	 * controllers: 0).  Optional (default NVME_CTRL_PAGE_SIZE - 1). */
	unsigned long (*get_virt_boundary)(struct nvme_ctrl *ctrl,
					   bool is_admin);
};

/* ==========================================================================
 * Sleepable read-side sections (P4, nvme_ns.c)
 * ==========================================================================
 *
 * The namespace list of a controller and the path list of a namespace
 * head are walked from places that cannot take their mutexes: the
 * multipath submission path (any context), path selection, and
 * recovery code that must not wait behind a scan.  Writers change the
 * lists under their mutex with the publish/unlink helpers of nvme_ns.c
 * and then nvme_srcu_synchronize() before freeing what they unlinked;
 * readers bracket the walk with nvme_srcu_read_lock()/unlock() (any
 * context, may sleep inside).  Two counters by epoch: a synchronize
 * flips the epoch and waits for the old one to drain, twice, so every
 * reader that could have seen the old list is gone. */
struct nvme_srcu {
	volatile long cnt[2]; /* readers inside, per epoch */
	volatile int idx; /* the epoch new readers enter */
	struct mutex sync_lock; /* one synchronize at a time */
	struct wait_queue_head wq; /* synchronize waits here */
	volatile int inited;
};

/* ==========================================================================
 * Fault injection (P6, nvme_fault_inject.c)
 * ========================================================================== */

struct nvme_fault_inject {
	/* Probability (percent 0..100) / interval (every n-th) / times
	 * (remaining, -1 unlimited) of the injection, as the conventional
	 * fault attributes define them. */
	volatile uint32_t probability;
	volatile uint32_t interval;
	volatile int32_t times;
	volatile uint32_t space; /* skip this many first */
	volatile uint32_t count; /* commands seen */
	volatile uint16_t status; /* NVMe status to inject */
	volatile uint8_t dont_retry; /* set DNR */
	volatile uint8_t kind; /* NVME_FAULT_* */
	volatile uint32_t injected; /* injections done */
	uint32_t verbose;
};
#define NVME_FAULT_STATUS 0 /* complete with `status' */
#define NVME_FAULT_TIMEOUT 1 /* drop the completion: timeout path */
#define NVME_FAULT_ABORT 2 /* force the abort path on the next timeout */
#define NVME_FAULT_RESET 3 /* schedule a controller reset */

/* ==========================================================================
 * The controller
 * ========================================================================== */

enum nvme_iopolicy {
	NVME_IOPOLICY_NUMA = 0,
	NVME_IOPOLICY_RR = 1,
	NVME_IOPOLICY_QD = 2,
};

enum nvme_ctrl_type_host {
	NVME_CTRL_TYPE_IO = 1, /* NVME_CTRL_IO in CNTRLTYPE */
	NVME_CTRL_TYPE_DISC = 2,
	NVME_CTRL_TYPE_ADMIN = 3,
};

/* Command set indexes cached in struct nvme_ctrl.cels (NVM, KV, ZNS,
 * SLM, CP and spares). */
#define NVME_NR_EFFECTS_CSI 8

struct nvme_ctrl {
	/* --- lifetime and identity (P3) ----------------------------------- */
	refcount_t ref; /* see "Lifetimes" */
	int instance; /* nvme%d, from nvme_instance_ida */
	char name[NVME_NAME_LEN]; /* "nvme0" */
	const struct nvme_ctrl_ops *ops;
	pci_device_t *pdev; /* the PCI function (P5 sets it) */
	unsigned long quirks; /* NVME_QUIRK_* */
	volatile unsigned long flags; /* enum nvme_ctrl_flags bits */
	volatile unsigned long events; /* NVME_AER_NOTICE_*_EVENT bits */
	bool comp_seen; /* a completion was seen (timeout heuristics) */
	bool identified; /* nvme_init_ctrl_finish ran once */
	bool passthru_err_log_enabled;
	bool subsystem; /* CAP.NSSRS: NSSR supported */

	/* --- state machine (P3) -------------------------------------------- */
	volatile enum nvme_ctrl_state state; /* under `lock' */
	spinlock_t lock; /* lock order 6 */
	struct wait_queue_head state_wq; /* woken on every state change */

	/* --- locks (see the lock order) ------------------------------------- */
	struct mutex shutdown_lock; /* order 1: enable/disable (P5 uses) */
	struct mutex scan_lock; /* order 2 */
	struct mutex namespaces_lock; /* order 4 */

	/* --- queues and tag sets -------------------------------------------- */
	struct blk_queue *admin_q; /* no disk */
	struct blk_mq_tag_set *admin_tagset; /* the transport's */
	struct blk_mq_tag_set *tagset; /* I/O, shared by all namespaces */
	struct blk_queue *connect_q; /* unused (fabrics) */
	uint32_t queue_count; /* admin + I/O queues allocated */
	uint16_t sqsize; /* I/O queue size - 1 (0's based) */

	/* --- namespaces (P4) ------------------------------------------------- */
	struct list_head namespaces; /* struct nvme_ns.list, by nsid */
	uint32_t max_namespaces; /* MNAN or NN */
	/* Readers of `namespaces' that cannot take namespaces_lock (ANA
	 * updates, requeue kicks, path clearing); nvme_mpath_init_ctrl()
	 * initialises it. */
	struct nvme_srcu ns_srcu;
	/* Fallback DSM range page when the per-command allocation fails
	 * (one user at a time: bit 0 of discard_page_busy). */
	void *discard_page;
	volatile unsigned long discard_page_busy;
	/* Streams directive (nvme_enable_streams): subsystem streams
	 * available and the ones the write-lifetime hints map to. */
	uint16_t nssa;
	uint16_t nr_streams;
	/* Identify Controller fields the I/O helpers need, captured by
	 * nvme_mpath_init_identify() on every (re)identify: FUSES, OCFS,
	 * FNA, SANICAP (full dword). */
	uint16_t fuses;
	uint16_t ocfs;
	uint8_t fna;
	uint32_t sanicap;

	/* --- subsystem ------------------------------------------------------- */
	struct nvme_subsystem *subsys;
	struct list_head subsys_entry; /* subsys->ctrls, subsys->lock */

	/* --- character device /dev/nvmeX (P6) ----------------------------- */
	struct devfs_node cdev_node;
	uint32_t cdev_minor;
	int cdev_registered;

	/* --- work items (kblockd; P3 unless noted) ------------------------- */
	struct blk_work reset_work; /* P5's nvme_reset_work */
	struct blk_work delete_work;
	struct blk_work scan_work; /* P4's nvme_scan_work */
	struct blk_work async_event_work;
	struct blk_work ka_work; /* delayed */
	struct blk_work failfast_work; /* delayed */
	struct blk_work fw_act_work;
	struct nvme_command ka_cmd; /* the keep-alive command */
	uint64_t ka_last_check_ns;

	/* --- identify-derived limits and capabilities (P3) ------------------ */
	uint16_t cntlid;
	uint16_t mtfa; /* max time for firmware activation, 100 ms */
	uint32_t ctrl_config; /* the CC value written */
	uint64_t cap; /* CAP */
	uint32_t vs; /* VS */
	uint32_t max_hw_sectors; /* from MDTS (512-byte sectors) */
	uint32_t max_segments;
	uint32_t max_integrity_segments;
	uint32_t max_zeroes_sectors;
	uint32_t max_zone_append; /* ZNS: ZASL in sectors */
	uint16_t crdt[3]; /* command retry delay times, 100 ms units */
	uint16_t oncs;
	uint8_t dmrl;
	uint32_t dmrsl;
	uint16_t oacs;
	volatile int abort_limit; /* outstanding Aborts left (ACL + 1) */
	uint8_t vwc;
	uint32_t sgls;
	uint16_t kas; /* keep-alive support, 100 ms units */
	uint8_t npss;
	uint8_t apsta;
	uint16_t wctemp, cctemp; /* Kelvin */
	uint32_t oaes;
	uint32_t aen_result; /* last AEN result (sysfs) */
	uint32_t ctratt;
	unsigned int shutdown_timeout_s; /* RTD3E-derived, >= the knob */
	unsigned int kato_s; /* keep-alive timeout in use, 0 off */
	uint8_t cmic;
	uint8_t frmw;
	uint8_t lpa;
	uint8_t elpe;
	uint8_t hctma;
	uint8_t sanicap_bits;
	uint16_t mntmt, mxtmt; /* thermal management temps (Kelvin) */
	struct nvme_id_power_state psd[32];
	struct nvme_effects_log *effects; /* commands supported & effects */
	uint64_t ps_max_latency_us; /* APST latency tolerance */
	bool apst_enabled;
	uint16_t hmmaxd;
	uint32_t hmpre; /* HMB preferred size, 4 KiB units */
	uint32_t hmmin; /* HMB minimum size, 4 KiB units */
	uint32_t hmminds; /* HMB minimum descriptor entry size */
	uint32_t ioccsz, iorcsz; /* (fabrics; kept for sysfs) */
	uint16_t icdoff;
	uint16_t maxcmd;
	uint16_t awupf; /* 0's based */
	uint16_t acwu;
	uint32_t rtd3e, rtd3r; /* resume/entry latency, us */
	enum nvme_ctrl_type_host cntrltype;
	uint8_t dctype;
	char subsys_serial_cache[NVME_SERIAL_LEN + 1];

	/* --- ANA (multipath; P4) -------------------------------------------- */
	uint8_t anacap;
	uint8_t anatt; /* ANA transition time, seconds */
	uint32_t anagrpmax;
	uint32_t nanagrpid;
	struct mutex ana_lock;
	struct nvme_ana_rsp_hdr *ana_log_buf;
	size_t ana_log_size;
	hrtimer_t anatt_timer;
	struct blk_work ana_work;
	volatile int nr_active; /* in-flight multipath I/O (QD policy) */
	/* ANATT timer arming: anatt_fires counts armings whose callback is
	 * still to run (or running), so nvme_mpath_stop() can wait for the
	 * callback before the controller goes away (hrtimer_cancel does not
	 * wait).  Arming and cancelling under anatt_lock. */
	spinlock_t anatt_lock;
	volatile int anatt_fires;
	volatile int mpath_inited; /* nvme_mpath_init_ctrl ran */

	/* --- monitoring (P3: nvme_hwmon.c) ---------------------------------- */
	uint8_t smart_crit_warning; /* last critical warning byte */
	uint16_t smart_composite_temp; /* Kelvin */
	uint64_t error_count; /* error log entries seen */
	uint64_t smart_last_ns;

	/* --- debug (P6) ------------------------------------------------------ */
	struct nvme_fault_inject fault_inject;
	void *debug; /* struct nvme_debug_ctrl (nvme_debug.c) */

	/* --- sysfs (P3) ------------------------------------------------------- */
	char sysfs_base[64]; /* "class/nvme/nvme0" */
	int nr_reconnects;
	unsigned int numa_node;

	/* --- core private (P3: nvme_core.c / nvme_admin.c / nvme_hwmon.c) ---
	 * Commands Supported and Effects logs per command set (index CSI);
	 * effects == cels[NVME_CSI_NVM] once identified.  Filled once with a
	 * compare-and-swap, freed with the controller. */
	struct nvme_effects_log *cels[NVME_NR_EFFECTS_CSI];
	/* struct nvme_hwmon_data (nvme_hwmon.c), NULL before hwmon init. */
	void *hwmon;
	/* Asynchronous events: the AEC value programmed, the result handed
	 * from the CQ to async_event_work (exchanged to 0 there), the last
	 * one seen and how many arrived (sysfs). */
	uint32_t aen_cfg;
	volatile uint32_t aen_pending;
	uint32_t aen_last_result;
	volatile uint64_t aen_count;
	uint32_t nn; /* Identify Controller NN */
	uint8_t aerl; /* AER limit, 0's based */
	uint8_t tmpthha; /* temperature threshold hysteresis attributes */
	uint16_t pad_p3;
	/* Last Number of Queues granted (Set Features 07h): NSQA/NCQA + 1. */
	uint32_t nr_io_queues_granted;
	/* Event storm guard of async_event_work: events in the current
	 * one-second window. */
	uint64_t aen_window_ns;
	uint32_t aen_window_count;
	/* Deferred free list link (nvme_put_ctrl -> nvme_free_work). */
	struct nvme_ctrl *free_next;
};


static inline enum nvme_ctrl_state nvme_ctrl_state(const struct nvme_ctrl *ctrl)
{
	return __atomic_load_n(&ctrl->state, __ATOMIC_ACQUIRE);
}

/* ==========================================================================
 * Subsystem, namespace heads, namespaces
 * ========================================================================== */

enum nvme_subsys_type {
	NVME_NQN_DISC = 1, /* discovery subsystem (unused) */
	NVME_NQN_NVME = 2, /* NVMe I/O subsystem */
	NVME_NQN_CURR = 3,
};

struct nvme_subsystem {
	int instance; /* nvme-subsys%d */
	refcount_t ref;
	struct list_head entry; /* nvme_subsystems, nvme_subsystems_lock */
	struct mutex lock; /* lock order 3: ctrls and nsheads */
	struct list_head ctrls; /* struct nvme_ctrl.subsys_entry */
	struct list_head nsheads; /* struct nvme_ns_head.entry */
	char subnqn[NVMF_NQN_SIZE + 1];
	char serial[NVME_SERIAL_LEN];
	char model[NVME_MODEL_LEN];
	char firmware_rev[NVME_FW_REV_LEN];
	uint8_t cmic;
	enum nvme_subsys_type subtype;
	uint16_t vendor_id;
	struct ida ns_ida; /* head instances (nvme%dn<id>) */
	enum nvme_iopolicy iopolicy;
	char sysfs_base[64]; /* "class/nvme-subsystem/nvme-subsys0" */
};

/* Namespace identifiers. */
struct nvme_ns_ids {
	uint8_t eui64[8];
	uint8_t nguid[16];
	uint8_t uuid[16];
	uint8_t csi; /* NVME_CSI_* */
};

/* What identification found about a namespace (scan). */
struct nvme_ns_info {
	struct nvme_ns_ids ids;
	uint32_t nsid;
	__le32 anagrpid;
	uint8_t pi_offset;
	uint16_t endgid;
	uint64_t runs; /* reservation status / generation */
	bool is_shared;
	bool is_readonly;
	bool is_ready;
	bool is_removed;
	bool is_rotational;
	bool no_vwc;
};

/* struct nvme_ns_head.features */
enum nvme_ns_features {
	NVME_NS_EXT_LBAS = 1 << 0, /* extended LBA format (inline md) */
	NVME_NS_METADATA_SUPPORTED = 1 << 1, /* separate metadata usable */
	NVME_NS_DEAC = 1 << 2, /* Write Zeroes DEAC supported */
};

/* struct nvme_ns_head.flags bits */
#define NVME_NSHEAD_DISK_LIVE 0
#define NVME_NSHEAD_QUEUE_IF_NO_PATH 1

/* One namespace as the subsystem sees it: shared by every controller
 * (path) that attaches it.  Owns the multipath disk when native multipath
 * is on (disk != NULL). */
struct nvme_ns_head {
	struct list_head list; /* paths: struct nvme_ns.siblings */
	struct nvme_subsystem *subsys;
	struct nvme_ns_ids ids;
	uint8_t lba_shift; /* log2 of the logical block size */
	uint16_t ms; /* metadata bytes per block */
	uint16_t pi_size; /* PI tuple size, 0 none */
	uint8_t pi_type; /* NVME_NS_DPS_PI_TYPE1..3, 0 none */
	uint8_t guard_type; /* NVME_NVM_NS_16B/32B/64B_GUARD */
	struct list_head entry; /* subsys->nsheads */
	refcount_t ref;
	bool shared;
	bool rotational;
	bool passthru_err_log_enabled;
	struct nvme_effects_log *effects;
	uint64_t nuse; /* namespace utilisation (blocks) */
	uint32_t ns_id;
	int instance; /* subsys->ns_ida */
	uint64_t zsze; /* ZNS zone size (blocks) */
	unsigned long features; /* enum nvme_ns_features */
	uint64_t nuse_last_ns; /* rate limit of the nuse refresh */
	/* Generic char node /dev/ngXnY of the head (multipath). */
	struct devfs_node cdev_node;
	uint32_t cdev_minor;
	int cdev_registered;
	struct gendisk *disk; /* multipath head disk, NULL without */
	uint16_t nr_plids; /* FDP placement IDs */
	uint16_t *plids;
	/* --- multipath (P4) --------------------------------------------------- */
	spinlock_t requeue_lock;
	struct blk_request *requeue_head, *requeue_tail; /* through rq_next */
	struct blk_work requeue_work;
	struct blk_work partition_scan_work;
	struct mutex lock;
	/* One format/limits update of the head at a time: the paths of a
	 * shared namespace are scanned by their controllers concurrently,
	 * and each writes the head's format fields and stacks its limits
	 * onto the head disk (read-modify-write).  Inside the controller's
	 * scan_lock; outside any freeze, registry or spin lock. */
	struct mutex update_lock;
	volatile unsigned long flags; /* NVME_NSHEAD_* */
	struct blk_work remove_work; /* delayed removal */
	unsigned int delayed_removal_secs;
	/* Current path per CPU (NUMA policy) / last used (RR); read without
	 * locks with atomic loads, written under subsys->lock. */
	struct nvme_ns *volatile current_path[MAX_CPUS];
	/* Readers of `list' (path selection in any context); writers hold
	 * subsys->lock and synchronize before a path goes away. */
	struct nvme_srcu srcu;
	/* Deferred removal of the multipath disk (delayed_removal_secs). */
	volatile int remove_work_pending;
	uint32_t nr_requeued; /* statistics: requests sent to another path */
	/* Streams: stream write size (blocks) and granularity (SWS units). */
	uint32_t sws;
	uint16_t sgs;
	/* Namespace limits used by the Copy command (Identify Namespace). */
	uint16_t mssrl;
	uint32_t mcl;
	uint8_t msrc;
	uint16_t nacwu; /* atomic compare & write unit, 0's based */
	/* ZNS: zone size (blocks) found by nvme_query_zone_info(), applied
	 * to zsze by nvme_update_zone_info() with the queue frozen. */
	uint64_t zsze_pending;
};

static inline bool nvme_ns_head_multipath(const struct nvme_ns_head *head)
{
	return head->disk != NULL;
}

/* struct nvme_ns.flags bits */
#define NVME_NS_REMOVING 0
#define NVME_NS_ANA_PENDING 2
#define NVME_NS_FORCE_RO 3
#define NVME_NS_READY 4
#define NVME_NS_SYSFS_ATTR_LINK 5

/* One namespace on one controller (a path). */
struct nvme_ns {
	struct list_head list; /* ctrl->namespaces */
	struct nvme_ctrl *ctrl;
	struct blk_queue *queue; /* disk->queue */
	struct gendisk *disk; /* nvme0n1, or hidden nvme0c0n1 */
	enum nvme_ana_state ana_state;
	uint32_t ana_grpid;
	struct list_head siblings; /* head->list */
	refcount_t ref;
	struct nvme_ns_head *head;
	volatile unsigned long flags; /* NVME_NS_* bits */
	/* Generic char node /dev/ngXnY (P6). */
	struct devfs_node cdev_node;
	uint32_t cdev_minor;
	int cdev_registered;
	struct nvme_fault_inject fault_inject;
	char sysfs_base[96];
};

static inline bool nvme_ns_has_pi(const struct nvme_ns_head *head)
{
	return head->pi_type && head->ms == head->pi_size;
}

/* LBA <-> 512-byte sector. */
static inline uint64_t nvme_sect_to_lba(const struct nvme_ns_head *head,
					uint64_t sector)
{
	return sector >> (head->lba_shift - 9);
}
static inline uint64_t nvme_lba_to_sect(const struct nvme_ns_head *head,
					uint64_t lba)
{
	return lba << (head->lba_shift - 9);
}
/* Dword count for NUMD/NUMDL fields (0's based). */
static inline uint32_t nvme_bytes_to_numd(size_t len)
{
	return (uint32_t)((len >> 2) - 1);
}

/* Is `status' an ANA error / a path error (multipath may fail over)? */
static inline bool nvme_is_ana_error(uint16_t status)
{
	switch (status & NVME_SCT_SC_MASK) {
	case NVME_SC_ANA_TRANSITION:
	case NVME_SC_ANA_INACCESSIBLE:
	case NVME_SC_ANA_PERSISTENT_LOSS:
		return true;
	default:
		return false;
	}
}
static inline bool nvme_is_path_error(uint16_t status)
{
	/* Path related status codes (type 3) and the host's own. */
	return (status & NVME_SCT_MASK) == NVME_SCT_PATH;
}

/* Registry of all subsystems (P3). */
extern struct list_head nvme_subsystems;
extern struct mutex nvme_subsystems_lock;
/* Controller instances (nvme%d) and character majors (P3 allocates the
 * majors with chrdev_register(0, ...) at nvme_init). */
extern struct ida nvme_instance_ida;
extern struct ida nvme_ns_chr_minor_ida;
extern uint32_t nvme_char_major; /* "nvme" */
extern uint32_t nvme_ns_chr_major; /* "nvme-generic" */

/* ==========================================================================
 * Entry points from kernel/ke (init.c, system.c)
 * ========================================================================== */

/* Early init (interrupts off, no scheduler, after ahci_init): core state,
 * character majors, then nvme_pci_init() registers the PCI driver, whose
 * probe sets each controller up WITHOUT bringing it up (no interrupts
 * yet).  Returns 0 or -errno (logged, boot continues).  Owner: P3
 * (nvme_core.c). */
int nvme_init(void);
/* Late init (interrupts on, scheduler and kblockd running, after
 * ahci_late_init): nvme_pci_late_init() queues every probed controller's
 * reset_work, which enables it, creates the I/O queues and scans the
 * namespaces asynchronously (the storage bootstrap accepts a root disk
 * that registers late).  Does not wait.  Owner: P3. */
int nvme_late_init(void);
/* System shutdown (after blk_shutdown_all flushed every disk): shut every
 * controller down with CC.SHN (normal, waiting for CSTS.SHST complete up
 * to the shutdown timeout).  Never fails; may sleep (polls when it
 * cannot).  Owner: P5 (nvme_pci.c). */
void nvme_shutdown_all(void);
/* Parse "nvme_core.*" knobs into nvme_core_param (P3). */
void nvme_core_parse_cmdline(void);
/* The value of "key=" on the kernel command line into buf (last one
 * wins): 1 found, 0 not.  Shared by P3 and P5.  Owner: P3. */
int nvme_cmdline_get(const char *key, char *buf, size_t cap);

/* ==========================================================================
 * Owned by P3: nvme_core.c -- controller lifetime and state
 * ========================================================================== */

/* Initialise a controller the transport allocated (embedded in its
 * struct nvme_dev): refcount 1, locks, work items, instance + name, state
 * NEW, the /dev/nvmeX node is NOT created yet.  quirks: the transport's
 * (plus nvme_core.quirks= for a matching PCI ID).  reset_work is the
 * transport's to initialise.  Never blocks: callable from the early boot
 * probe (interrupts off). */
int nvme_init_ctrl(struct nvme_ctrl *ctrl, pci_device_t *pdev,
		   const struct nvme_ctrl_ops *ops, unsigned long quirks);
/* Make the controller visible: /dev/nvmeX (P6), sysfs class directory,
 * fault injection and debug state; takes a reference that
 * nvme_uninit_ctrl() drops.  Blocks only as far as P6's node
 * registration does (early boot probe allowed). */
int nvme_add_ctrl(struct nvme_ctrl *ctrl);
/* Undo nvme_add_ctrl / nvme_init_ctrl (removal).  May sleep. */
void nvme_uninit_ctrl(struct nvme_ctrl *ctrl);
/* After the admin queue works: identify the controller, subsystem
 * lookup/creation by SUBNQN, quirks by model/firmware, limits (MDTS,
 * DMRL, ...), effects log, APST, host behavior, temperature thresholds,
 * AEN configuration, keep-alive need, HMB sizes.  `was_suspended':
 * resuming (skip what must not change).  0 / -errno / NVMe status > 0.
 * May sleep. */
int nvme_init_ctrl_finish(struct nvme_ctrl *ctrl, bool was_suspended);
/* After the transition to LIVE: AEN configuration (Set Features 0Bh)
 * and the first Asynchronous Event Request (through async_event_work),
 * namespace scan queued, I/O queues unquiesced, multipath updated.
 * (Keep-alive was started by nvme_init_ctrl_finish.)  May sleep. */
void nvme_start_ctrl(struct nvme_ctrl *ctrl);
/* Stop the core's activity before a disable: keep-alive, AEN work,
 * fw_act work, failfast, ANA timer; transport stop_ctrl.  May sleep. */
void nvme_stop_ctrl(struct nvme_ctrl *ctrl);
/* Removal: DELETING (unless already deleting/dead), flush reset_work,
 * nvme_stop_ctrl, nvme_remove_namespaces, transport delete_ctrl (when
 * set), nvme_uninit_ctrl.  Once per controller (NVME_CTRL_REMOVED).
 * Never from reset_work itself (it flushes it).  May sleep. */
void nvme_remove_ctrl(struct nvme_ctrl *ctrl);
/* Asynchronous delete through delete_work (-EBUSY when already
 * deleting) / the same, waited for. */
int nvme_delete_ctrl(struct nvme_ctrl *ctrl);
void nvme_delete_ctrl_sync(struct nvme_ctrl *ctrl);

void nvme_get_ctrl(struct nvme_ctrl *ctrl); /* any context */
/* Any context: the last reference queues the free on kblockd (it may be
 * dropped inside one of the controller's own work items), which cancels
 * the controller's work items, releases the core's parts, calls
 * ops->free_ctrl and drops the subsystem. */
void nvme_put_ctrl(struct nvme_ctrl *ctrl);

/* State change under ctrl->lock: true when the transition is allowed
 * (and done), false otherwise.  Wakes state_wq.  Any context. */
bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,
			    enum nvme_ctrl_state new_state);
/* The table lookup itself (pure; host tests use it). */
static inline bool nvme_ctrl_state_transition_ok(enum nvme_ctrl_state old,
						 enum nvme_ctrl_state new_state)
{
	if ((unsigned int)old >= NVME_CTRL_NR_STATES ||
	    (unsigned int)new_state >= NVME_CTRL_NR_STATES)
		return false;
	return nvme_ctrl_state_transitions[new_state][old] != 0;
}
/* True when the controller is going away (DELETING, DELETING_NOIO,
 * DEAD): nothing may (re)start it. */
bool nvme_state_terminal(const struct nvme_ctrl *ctrl);
/* Wait until this caller could move the controller to RESETTING (it now
 * owns the recovery: true), or the controller is going away (false).
 * May sleep. */
bool nvme_wait_reset(struct nvme_ctrl *ctrl);

/* Reset: change to RESETTING (-EBUSY if not allowed) and queue
 * reset_work.  Any context. */
int nvme_reset_ctrl(struct nvme_ctrl *ctrl);
/* The same, waiting for the reset to finish: 0 when LIVE, -ENETRESET
 * otherwise.  May sleep. */
int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl);
/* Queue reset_work if the state is already RESETTING (the timeout path):
 * 0 / -EBUSY.  Any context. */
int nvme_try_sched_reset(struct nvme_ctrl *ctrl);
/* NSSR: subsystem_reset op (-ENOTTY without CAP.NSSRS). */
static inline int nvme_reset_subsystem(struct nvme_ctrl *ctrl)
{
	if (!ctrl->subsystem || !ctrl->ops->subsystem_reset)
		return -ENOTTY;
	return ctrl->ops->subsystem_reset(ctrl);
}

/* Controller enable/disable sequence helpers (CC/CSTS), used by the
 * transport's reset and shutdown: nvme_disable_ctrl(ctrl, shutdown) --
 * shutdown: CC.SHN normal and wait for SHST complete (shutdown_timeout);
 * else CC.EN = 0 and wait for !RDY (CAP.TO, the delay quirk).
 * nvme_enable_ctrl: CC (CSS, MPS, AMS, IOSQES/IOCQES, CRIME when
 * supported), EN = 1, wait for RDY (CAP.TO or CRTO).  May sleep. */
int nvme_disable_ctrl(struct nvme_ctrl *ctrl, bool shutdown);
int nvme_enable_ctrl(struct nvme_ctrl *ctrl);
/* Wait until CSTS & mask == value (ready / shutdown status), up to
 * timeout_s; -ENODEV when CSTS reads all-ones, -ETIMEDOUT.  May sleep. */
int nvme_wait_ready(struct nvme_ctrl *ctrl, uint32_t mask, uint32_t val,
		    uint32_t timeout_s, const char *op);

/* Asynchronous events: the transport hands every AEN completion here
 * (any context: CQ processing).  `status'/`res' from the CQE. */
void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,
			       volatile union nvme_result *res);
/* Is the CQE's command ID one of the AEN command IDs? */
static inline bool nvme_is_aen_req(uint16_t qid, uint16_t command_id)
{
	return !qid &&
	       nvme_tag_from_cid(command_id) >= NVME_AQ_BLK_MQ_DEPTH;
}

/* Keep-alive (only when KAS requires it). */
void nvme_start_keep_alive(struct nvme_ctrl *ctrl);
void nvme_stop_keep_alive(struct nvme_ctrl *ctrl);

/* Power states and APST. */
int nvme_configure_apst(struct nvme_ctrl *ctrl);
int nvme_set_power_state(struct nvme_ctrl *ctrl, uint32_t ps);
int nvme_get_power_state(struct nvme_ctrl *ctrl, uint32_t *ps);
/* Volatile write cache. */
int nvme_set_vwc(struct nvme_ctrl *ctrl, bool enable);
/* Timestamp feature (host time in ms since the epoch). */
int nvme_configure_timestamp(struct nvme_ctrl *ctrl);
/* Host behavior support (ACRE, LBAFEE). */
int nvme_configure_host_options(struct nvme_ctrl *ctrl);
/* Number of I/O queues (Set Features 07h): *count in, granted out. */
int nvme_set_queue_count(struct nvme_ctrl *ctrl, int *count);

/* Quiesce / freeze helpers over all namespaces' queues (recovery). */
void nvme_quiesce_io_queues(struct nvme_ctrl *ctrl);
void nvme_unquiesce_io_queues(struct nvme_ctrl *ctrl);
void nvme_quiesce_admin_queue(struct nvme_ctrl *ctrl);
void nvme_unquiesce_admin_queue(struct nvme_ctrl *ctrl);
void nvme_mark_namespaces_dead(struct nvme_ctrl *ctrl);
void nvme_sync_queues(struct nvme_ctrl *ctrl);
void nvme_sync_io_queues(struct nvme_ctrl *ctrl);
void nvme_start_freeze(struct nvme_ctrl *ctrl);
void nvme_wait_freeze(struct nvme_ctrl *ctrl);
int nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout_ms);
void nvme_unfreeze(struct nvme_ctrl *ctrl);

/* Cancel every started request of the tag sets with
 * NVME_SC_HOST_ABORTED_CMD (after a controller disable, when the device
 * no longer owns any DMA).  busy_tag_iter_fn form for P5. */
int nvme_cancel_request(struct blk_request *rq, void *data);
void nvme_cancel_tagset(struct nvme_ctrl *ctrl);
void nvme_cancel_admin_tagset(struct nvme_ctrl *ctrl);

/* Work helpers: queue on kblockd / after a delay (any context): 1 when
 * queued now, 0 when it was pending already (nothing changes then);
 * cancel (take it back, wait for a running instance): 0 / -ETIMEDOUT /
 * -EBUSY (called from inside the item); flush (wait for a queued or
 * running instance to finish): 0 / -EBUSY.  May sleep (cancel/flush). */
int nvme_queue_work(struct blk_work *w);
int nvme_queue_delayed_work(struct blk_work *w, uint64_t delay_ms);
int nvme_cancel_work_sync(struct blk_work *w);
int nvme_flush_work(struct blk_work *w);
/* Sleep `ms' milliseconds (busy-waits when the caller may not sleep). */
void nvme_msleep(unsigned int ms);

/* Controller and subsystem instance numbers (nvme%d, nvme-subsys%d) stay
 * below this (the sysfs registry's size). */
#define NVME_MAX_INSTANCES 1024

/* Drop a subsystem reference (the last one unregisters and frees it).
 * May sleep.  Taking one: refcount_inc_not_zero(&subsys->ref) under
 * nvme_subsystems_lock, or refcount_inc while holding one already. */
void nvme_put_subsystem(struct nvme_subsystem *subsys);

/* The quirks of the core table (model/firmware) for this controller,
 * matched against Identify's VID (as the established table is keyed);
 * `vid' (the PCI vendor) is not used for the match. */
unsigned long nvme_core_quirks_lookup(const struct nvme_id_ctrl *id,
				      uint16_t vid);

/* Commands Supported and Effects log of command set `csi', read once and
 * cached in ctrl->cels (P4: the effects of a namespace's command set).
 * 0 / NVMe status / -errno.  May sleep. */
int nvme_get_effects_log(struct nvme_ctrl *ctrl, uint8_t csi,
			 struct nvme_effects_log **log);

/* Identify I/O Command Set specific Controller (CNS 06h, NVM): DMRL,
 * DMRSL and the Write Zeroes limit (max_zeroes_sectors).  Called by the
 * namespace scan (P4) like the established scan work does.  0 / NVMe
 * status / -errno.  May sleep. */
int nvme_init_non_mdts_limits(struct nvme_ctrl *ctrl);
/* A Set Features KATO pass-through succeeded (nvme_passthru_end): follow
 * the new keep-alive timeout (milliseconds). */
void nvme_core_kato_changed(struct nvme_ctrl *ctrl, uint32_t kato_ms);

/* Can Identify with this CNS be sent to this controller version? */
bool nvme_id_cns_ok(struct nvme_ctrl *ctrl, uint8_t cns);

/* The CC value last written.  Written whole (one store) by the enable /
 * disable paths, which the controller's shutdown lock serialises; read
 * without that lock by the scan work and sysfs, so both sides use single
 * atomic accesses. */
static inline uint32_t nvme_ctrl_config(const struct nvme_ctrl *ctrl)
{
	return __atomic_load_n(&ctrl->ctrl_config, __ATOMIC_RELAXED);
}

static inline void nvme_set_ctrl_config(struct nvme_ctrl *ctrl, uint32_t cc)
{
	__atomic_store_n(&ctrl->ctrl_config, cc, __ATOMIC_RELAXED);
}

/* Identify-derived controller fields a reset rewrites while the scan work
 * (and I/O setup) reads them: single atomic accesses. */
static inline uint32_t nvme_ctrl_ctratt(const struct nvme_ctrl *ctrl)
{
	return __atomic_load_n(&ctrl->ctratt, __ATOMIC_RELAXED);
}

static inline uint16_t nvme_ctrl_nr_streams(const struct nvme_ctrl *ctrl)
{
	return __atomic_load_n(&ctrl->nr_streams, __ATOMIC_RELAXED);
}

static inline void nvme_set_ctrl_nr_streams(struct nvme_ctrl *ctrl, uint16_t n)
{
	__atomic_store_n(&ctrl->nr_streams, n, __ATOMIC_RELAXED);
}

/* A path's ANA state (and group id): written by the ANA log update under
 * ctrl->ana_lock, read lock-free by path selection and sysfs. */
_Static_assert(sizeof(enum nvme_ana_state) == sizeof(int),
	       "ana_state is accessed as an int");

static inline enum nvme_ana_state nvme_ns_ana_state(const struct nvme_ns *ns)
{
	return (enum nvme_ana_state)__atomic_load_n((const int *)&ns->ana_state,
						    __ATOMIC_RELAXED);
}

static inline void nvme_ns_set_ana_state(struct nvme_ns *ns,
					 enum nvme_ana_state state)
{
	__atomic_store_n((int *)&ns->ana_state, (int)state, __ATOMIC_RELAXED);
}

/* True for exactly one caller per `flag' (a one-time message): the first
 * one to get here, even when several processors arrive together. */
static inline bool nvme_first_time(int *flag)
{
	return __atomic_load_n(flag, __ATOMIC_RELAXED) == 0 &&
	       __atomic_exchange_n(flag, 1, __ATOMIC_RELAXED) == 0;
}

/* Several I/O command sets enabled (CC.CSS = CSI)? */
static inline bool nvme_multi_css(const struct nvme_ctrl *ctrl)
{
	return (nvme_ctrl_config(ctrl) & NVME_CC_CSS_MASK) == NVME_CC_CSS_CSI;
}
static inline bool nvme_ctrl_sgl_supported(const struct nvme_ctrl *ctrl)
{
	return (ctrl->sgls & (NVME_CTRL_SGLS_BYTE_ALIGNED |
			      NVME_CTRL_SGLS_DWORD_ALIGNED)) != 0;
}
static inline bool nvme_ctrl_meta_sgl_supported(const struct nvme_ctrl *ctrl)
{
	return (ctrl->sgls & NVME_CTRL_SGLS_MSDS) != 0;
}
/* Length of a space-padded identity string without the padding. */
static inline int nvme_strlen(const char *s, int len)
{
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\0'))
		len--;
	return len;
}
/* Log the controller identity (transport op or VID/model/firmware). */
void nvme_print_device_info(struct nvme_ctrl *ctrl);

/* Command Retry Delay of a completion status: the CRD field selects
 * CRDT1..3 (100 ms units) of Identify Controller; 0 without CRD. */
static inline unsigned long nvme_crd_delay_ms(const struct nvme_ctrl *ctrl,
					      uint16_t status)
{
	unsigned int crd = (status & NVME_STATUS_CRD) >> NVME_STATUS_CRD_SHIFT;

	if (!crd)
		return 0;
	return (unsigned long)ctrl->crdt[crd - 1] * NVME_CRDT_UNIT_MS;
}

/* APST table builder (pure; nvme_configure_apst and the host tests):
 * fills entries[32] for the power states psd[0..npss] under the latency
 * limit, with the established policy (primary/secondary latency
 * tolerances and timeouts; or the 2 % heuristic when a primary knob is
 * 0); NO_DEEPEST_PS skips the deepest state.  Returns the deepest state
 * used (-1: none) and the largest total latency in *max_lat_us. */
int nvme_apst_build_table(const struct nvme_id_power_state *psd,
			  unsigned int npss, unsigned long quirks,
			  uint64_t ps_max_latency_us,
			  uint64_t primary_timeout_ms,
			  uint64_t secondary_timeout_ms,
			  uint64_t primary_latency_tol_us,
			  uint64_t secondary_latency_tol_us,
			  __le64 entries[32], uint64_t *max_lat_us);

/* ==========================================================================
 * Owned by P3: nvme_admin.c -- admin tag set and command submission
 * ========================================================================== */

/* Admin tag set + admin queue (no disk) for the transport's ops and PDU
 * size; reserved tags for internal commands (keep-alive, connect).
 * May sleep. */
int nvme_alloc_admin_tag_set(struct nvme_ctrl *ctrl,
			     struct blk_mq_tag_set *set,
			     const struct blk_mq_ops *ops,
			     unsigned int cmd_size);
void nvme_remove_admin_tag_set(struct nvme_ctrl *ctrl);
/* I/O tag set (nr_maps 1-3: default/read/poll), shared by every
 * namespace queue. */
int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
			  const struct blk_mq_ops *ops, unsigned int nr_maps,
			  unsigned int cmd_size);
void nvme_remove_io_tag_set(struct nvme_ctrl *ctrl);

/* The transport's queue_rq calls these: */
/* Initialise nvme_req(rq) for a pass-through command (cmd copied in,
 * flags, genctr kept). */
void nvme_init_request(struct blk_request *rq, struct nvme_command *cmd);
/* Can this command run now given the controller state?  (Admin commands
 * during RESETTING/CONNECTING: only the internal ones.) */
bool __nvme_check_ready(struct nvme_ctrl *ctrl, struct blk_request *rq,
			bool queue_live, enum nvme_ctrl_state state);
static inline bool nvme_check_ready(struct nvme_ctrl *ctrl,
				    struct blk_request *rq, bool queue_live)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

	if (likely(state == NVME_CTRL_LIVE))
		return true;
	return __nvme_check_ready(ctrl, rq, queue_live, state);
}

/* The block-layer op of a pass-through command: data to the device
 * (and no data) is DRV_OUT, data from it DRV_IN. */
static inline int nvme_req_op(const struct nvme_command *cmd)
{
	return nvme_is_write(cmd) ? BLK_OP_DRV_OUT : BLK_OP_DRV_IN;
}

/* Completion side of the command ID: the request a CQE's command_id
 * names, or NULL when the tag is unknown or the generation counter does
 * not match (a stale or bogus completion: logged, ignored).  Any context
 * (the CQ handler).  nvme_cid_to_rq: without the generation check
 * (NVME_QUIRK_SKIP_CID_GEN). */
static inline struct blk_request *nvme_find_rq(struct blk_mq_tags *tags,
					       uint16_t command_id)
{
	uint8_t genctr = (uint8_t)nvme_genctr_from_cid(command_id);
	uint16_t tag = (uint16_t)nvme_tag_from_cid(command_id);
	struct blk_request *rq = blk_mq_tag_to_rq(tags, tag);

	if (unlikely(!rq)) {
		WARN_RATELIMIT(1, "nvme: no request for tag %#x", tag);
		return NULL;
	}
	uint8_t cur = nvme_req_genctr(nvme_req(rq));

	if (unlikely(nvme_genctr_mask(cur) != genctr)) {
		WARN_RATELIMIT(1,
			       "nvme: request %#x genctr mismatch (got %#x "
			       "expected %#x)",
			       tag, genctr, nvme_genctr_mask(cur));
		return NULL;
	}
	return rq;
}
static inline struct blk_request *nvme_cid_to_rq(struct blk_mq_tags *tags,
						 uint16_t command_id)
{
	return blk_mq_tag_to_rq(tags, command_id);
}
/* Submission side: install the command ID (tag + current generation)
 * into the SQE (nvme_setup_cmd, P4, and every transport-built command of
 * a tagged request).  The generation advances once per command, when it
 * completes (nvme_try_complete_req), so the tag's next command carries a
 * new one. */
static inline void nvme_assign_cid(struct nvme_ctrl *ctrl,
				   struct blk_request *rq,
				   struct nvme_command *cmd)
{
	(void)ctrl;
	BUG_ON(rq->tag < 0 || rq->tag >= NVME_MAX_TAGS_PER_QUEUE);
	cmd->common.command_id = nvme_cid(rq);
}

/* Limits of a controller's queues (admin queue here; P4 for namespace
 * queues): MDTS, segments, virt boundary, dword alignment. */
void nvme_set_ctrl_limits(struct nvme_ctrl *ctrl, struct blk_limits *lim,
			  bool is_admin);
/* Fail (or requeue for multipath) a request that may not run now. */
blk_status_t nvme_fail_nonready_command(struct nvme_ctrl *ctrl,
					struct blk_request *rq);
/* Mark the request started (timeout clock) and record multipath
 * statistics. */
void nvme_start_request(struct blk_request *rq);

/* Synchronous commands (may sleep).  Return 0, an NVMe status > 0, or
 * -errno.  `result' gets completion dword 0/1 when non-NULL.  qid
 * NVME_QID_ANY: any I/O queue; at_head: before queued I/O; flags
 * BLK_MQ_REQ_*.  The admin queue is q == ctrl->admin_q. */
#define NVME_QID_ANY (-1)
enum nvme_submit_flags {
	NVME_SUBMIT_AT_HEAD = 1 << 0, /* insert at the head */
	NVME_SUBMIT_NOWAIT = 1 << 1, /* no tag wait */
	NVME_SUBMIT_RESERVED = 1 << 2, /* reserved tag */
	NVME_SUBMIT_RETRY = 1 << 3, /* may be retried */
};
int __nvme_submit_sync_cmd(struct blk_queue *q, struct nvme_command *cmd,
			   union nvme_result *result, void *buffer,
			   unsigned int bufflen, int qid,
			   unsigned int submit_flags);
int nvme_submit_sync_cmd(struct blk_queue *q, struct nvme_command *cmd,
			 void *buffer, unsigned int bufflen);
/* Execute an already prepared request synchronously: NVMe status / -errno. */
int nvme_execute_rq(struct blk_request *rq, bool at_head);
/* The same for a user pass-through command: when the caller is killed
 * while it waits, the request is cancelled (blk_mq_cancel_request: taken
 * back if the device does not have it yet, else the transport's
 * blk_mq_ops.cancel), and the wait goes on until it completed -- its
 * buffers stay the device's until then.  May sleep. */
int nvme_execute_user_rq(struct blk_request *rq);

int nvme_identify_ctrl(struct nvme_ctrl *ctrl, struct nvme_id_ctrl **id);
int nvme_identify_ns(struct nvme_ctrl *ctrl, uint32_t nsid,
		     struct nvme_id_ns **id);
int nvme_identify_ns_cs_indep(struct nvme_ctrl *ctrl, uint32_t nsid,
			      struct nvme_id_ns_cs_indep **id);
int nvme_identify_ns_descs(struct nvme_ctrl *ctrl, struct nvme_ns_info *info);
int nvme_identify_active_ns_list(struct nvme_ctrl *ctrl, uint32_t prev,
				 __le32 *list /* 1024 entries */);
/* Generic Identify for any CNS/CSI into a kalloc'd 4 KiB buffer. */
int nvme_identify(struct nvme_ctrl *ctrl, uint8_t cns, uint8_t csi,
		  uint32_t nsid, uint16_t cntid, void *buf);
int nvme_set_features(struct nvme_ctrl *ctrl, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      uint32_t *result);
int nvme_get_features(struct nvme_ctrl *ctrl, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      uint32_t *result);
/* Get Log Page, split into transfers of at most MDTS with the log page
 * offset (LPO) advanced; lsp/lsi/csi as the log needs. */
int nvme_get_log(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t log_page,
		 uint8_t lsp, uint8_t csi, void *log, size_t size,
		 uint64_t offset);
int nvme_get_log_lsi(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t log_page,
		     uint8_t lsp, uint16_t lsi, uint8_t csi, void *log,
		     size_t size, uint64_t offset);
/* Abort command `cid' on `sqid' (admin): NVMe status / -errno; -EBUSY
 * when the Abort Command Limit (ACL + 1 outstanding) is used up.
 * Synchronous (may sleep). */
int nvme_abort_cmd(struct nvme_ctrl *ctrl, uint16_t sqid, uint16_t cid);

/* Command effects of an opcode on this controller/namespace (effects log
 * or the conservative defaults). */
uint32_t nvme_command_effects(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			      uint8_t opcode);
/* Around a pass-through command with side effects: freeze I/O when it
 * changes logical block content or capabilities (LBCC/NCC/NIC/CSE) ... */
uint32_t nvme_passthru_start(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			     uint8_t opcode);
/* ... and unfreeze / rescan / re-identify afterwards. */
void nvme_passthru_end(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		       uint32_t effects, struct nvme_command *cmd, int status);

/* ==========================================================================
 * Owned by P3: nvme_constants.c
 * ========================================================================== */

const char *nvme_get_admin_opcode_str(uint8_t opcode);
const char *nvme_get_opcode_str(uint8_t opcode); /* NVM I/O */
const char *nvme_get_fabrics_opcode_str(uint8_t opcode);
const char *nvme_get_error_status_str(uint16_t status);
const char *nvme_get_sct_str(uint16_t status);
const char *nvme_feature_name(uint8_t fid);
const char *nvme_log_page_name(uint8_t lid);
/* Log a failed command, rate-limited as the established driver does. */
void nvme_log_error(struct blk_request *rq);
void nvme_log_err_passthru(struct blk_request *rq);

/* ==========================================================================
 * Owned by P3: nvme_hwmon.c -- health and temperatures
 * ========================================================================== */

int nvme_hwmon_init(struct nvme_ctrl *ctrl);
void nvme_hwmon_exit(struct nvme_ctrl *ctrl);
/* The monitor's hwmon class number (its directory is hwmon<N> under the
 * controller's), -1 when there is none. */
int nvme_hwmon_id(struct nvme_ctrl *ctrl);
/* Read the SMART/health log (cached for the sysfs readers). */
int nvme_get_smart_log(struct nvme_ctrl *ctrl, struct nvme_smart_log *log);
/* A SMART AEN arrived: refresh, log critical warnings. */
void nvme_hwmon_smart_event(struct nvme_ctrl *ctrl, uint32_t aen_result);
/* Error log entries since the last look (error_count). */
void nvme_hwmon_error_event(struct nvme_ctrl *ctrl);
/* Temperature threshold get/set (sensor 0 = composite), Kelvin. */
int nvme_get_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
			 uint16_t *kelvin);
int nvme_set_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
			 uint16_t kelvin);
/* Free what nvme_hwmon_init allocated (from nvme_free_ctrl, when no
 * reader can be left); nvme_hwmon_exit only shuts the readers out. */
void nvme_hwmon_free(struct nvme_ctrl *ctrl);
/* The hwmon-style temperature attributes (sysfs): channel 0 =
 * composite, 1..8 = temperature sensors.  Values in millidegrees
 * Celsius (alarm: 0/1).  read/write: 0 / -errno; is_visible: the file
 * mode (0 = absent). */
enum nvme_hwmon_attr {
	NVME_HWMON_TEMP_INPUT,
	NVME_HWMON_TEMP_MAX,
	NVME_HWMON_TEMP_MIN,
	NVME_HWMON_TEMP_CRIT,
	NVME_HWMON_TEMP_ALARM,
	NVME_HWMON_TEMP_LABEL,
};
#define NVME_HWMON_CHANNELS 9
int nvme_hwmon_read(struct nvme_ctrl *ctrl, enum nvme_hwmon_attr attr,
		    int channel, long *val);
int nvme_hwmon_write(struct nvme_ctrl *ctrl, enum nvme_hwmon_attr attr,
		     int channel, long val);
unsigned int nvme_hwmon_is_visible(struct nvme_ctrl *ctrl,
				   enum nvme_hwmon_attr attr, int channel);
const char *nvme_hwmon_label(int channel);
/* Kelvin <-> millidegrees Celsius. */
static inline long nvme_kelvin_to_millicelsius(long k)
{
	return k * 1000 - 273150;
}
static inline long nvme_millicelsius_to_kelvin(long mc)
{
	/* rounded to the nearest Kelvin */
	long v = mc + 273150;

	return v >= 0 ? (v + 500) / 1000 : -((-v + 500) / 1000);
}

/* ==========================================================================
 * Owned by P3: nvme_sysfs.c
 * ========================================================================== */

/* /sys/class/nvme/nvmeX/{model,serial,firmware_rev,cntlid,state,
 * subsysnqn,address,transport,numa_node,queue_count,sqsize,kato,
 * cntrltype,dctype,hmb,hwmon temps,fault_inject/...,reset_controller,
 * rescan_controller,delete_controller,dev} and the namespace and
 * subsystem directories. */
/* The dynamic /sys/class/nvme and /sys/class/nvme-subsystem trees (from
 * nvme_init). */
void nvme_sysfs_init(void);
int nvme_sysfs_add_ctrl(struct nvme_ctrl *ctrl);
void nvme_sysfs_del_ctrl(struct nvme_ctrl *ctrl);
int nvme_sysfs_add_subsys(struct nvme_subsystem *subsys);
void nvme_sysfs_del_subsys(struct nvme_subsystem *subsys);
int nvme_sysfs_add_ns(struct nvme_ns *ns);
void nvme_sysfs_del_ns(struct nvme_ns *ns);

/* ==========================================================================
 * Owned by P4: nvme_ns.c -- namespaces
 * ========================================================================== */

/* Queue / run the namespace scan (scan_work, under scan_lock). */
void nvme_queue_scan(struct nvme_ctrl *ctrl);
void nvme_scan_work(void *arg); /* blk_work fn, arg = ctrl */
/* Remove every namespace (removal, dead controller).  May sleep. */
void nvme_remove_namespaces(struct nvme_ctrl *ctrl);
/* Lookup with a reference (nvme_put_ns drops it). */
struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, uint32_t nsid);
bool nvme_get_ns(struct nvme_ns *ns);
void nvme_put_ns(struct nvme_ns *ns);
bool nvme_tryget_ns_head(struct nvme_ns_head *head);
void nvme_put_ns_head(struct nvme_ns_head *head);
/* Revalidate a namespace (capacity, format, ANA, ...). */
int nvme_revalidate_ns(struct nvme_ns *ns, struct nvme_ns_info *info);
/* Is nsid unique in the subsystem (not shared)? */
bool nvme_is_unique_nsid(struct nvme_ctrl *ctrl, struct nvme_ns_head *head);
/* Namespace management / attachment, Format NVM, Sanitize (with the
 * rescans they need): NVMe status / -errno.  May sleep. */
int nvme_ns_create(struct nvme_ctrl *ctrl, const struct nvme_id_ns *id,
		   uint8_t csi, uint32_t *nsid_out);
int nvme_ns_delete(struct nvme_ctrl *ctrl, uint32_t nsid);
int nvme_ns_attach(struct nvme_ctrl *ctrl, uint32_t nsid, bool attach,
		   const uint16_t *ctrl_ids, unsigned int nr);
int nvme_format_ns(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t lbaf,
		   uint8_t ses, uint8_t pi, uint8_t pil, uint8_t mset);
int nvme_sanitize(struct nvme_ctrl *ctrl, uint32_t cdw10, uint32_t ovrpat);
int nvme_sanitize_progress(struct nvme_ctrl *ctrl, uint32_t *percent,
			   uint16_t *sstat);
/* Block ops of a namespace disk (open/release/ioctl/report_zones/pr_ops/
 * get_unique_id/getgeo), filled by P4, ioctls dispatched to P6. */
extern const struct blk_ops nvme_bdev_ops;
/* Unique ID of a namespace for blk_ops->get_unique_id. */
int nvme_ns_get_unique_id(struct nvme_ns *ns, uint8_t id[16], int type);
/* Parse a Namespace Identification Descriptor list (Identify CNS 03h,
 * 4096 bytes) into info->ids: EUI-64, NGUID, UUID (ignored with
 * NVME_QUIRK_BOGUS_NID), CSI; bogus lengths end the walk.  -EINVAL when
 * several command sets are enabled and no CSI was reported.  For P3's
 * nvme_identify_ns_descs().  Any context. */
int nvme_ns_descs_parse(struct nvme_ctrl *ctrl, struct nvme_ns_info *info,
			const void *data);

/* Sleepable read-side sections (struct nvme_srcu above).  init: any
 * context, idempotent; read_lock/unlock: any context, return/take the
 * epoch; synchronize: may sleep, waits for every reader that entered
 * before it. */
void nvme_srcu_init(struct nvme_srcu *s, const char *name);
int nvme_srcu_read_lock(struct nvme_srcu *s);
void nvme_srcu_read_unlock(struct nvme_srcu *s, int idx);
void nvme_srcu_synchronize(struct nvme_srcu *s);
/* Publish / unlink an entry of a list walked under an nvme_srcu: the
 * writer holds the list's mutex; an unlinked entry keeps its `next' so a
 * reader standing on it finishes its walk; free it only after
 * nvme_srcu_synchronize(). */
void nvme_list_add_tail_srcu(struct list_head *entry, struct list_head *head);
void nvme_list_add_srcu(struct list_head *entry, struct list_head *prev);
void nvme_list_del_srcu(struct list_head *entry);
/* The next entry of a list walked under an nvme_srcu (acquire load). */
static inline struct list_head *nvme_list_next_srcu(const struct list_head *p)
{
	return __atomic_load_n(&p->next, __ATOMIC_ACQUIRE);
}
#define nvme_list_for_each_entry_srcu(pos, head, member)                     \
	for (pos = list_entry(nvme_list_next_srcu(head), __typeof__(*pos),   \
			      member);                                       \
	     &pos->member != (head);                                         \
	     pos = list_entry(nvme_list_next_srcu(&pos->member),            \
			      __typeof__(*pos), member))

/* The namespace that a disk of nvme_bdev_ops belongs to; whether a disk
 * is a multipath head (nvme_ns_head_ops, nvme_mpath.c). */
static inline struct nvme_ns *nvme_disk_to_ns(const struct gendisk *disk)
{
	return (struct nvme_ns *)disk->private_data;
}
extern const struct blk_ops nvme_ns_head_ops;
static inline bool nvme_disk_is_ns_head(const struct gendisk *disk)
{
	return disk->ops == &nvme_ns_head_ops;
}
/* NVM command set I/O helpers for kernel users and the ioctl layer: build
 * the command (nsid, LBA range in logical blocks, 0's based counts and
 * control bits filled in) and run it synchronously on the namespace's I/O
 * queue.  NVMe status > 0 / -errno.  May sleep. */
void nvme_init_compare_cmd(struct nvme_ns *ns, struct nvme_command *c,
			   uint64_t slba, uint32_t nlb, uint16_t control);
void nvme_init_verify_cmd(struct nvme_ns *ns, struct nvme_command *c,
			  uint64_t slba, uint32_t nlb, uint16_t control);
void nvme_init_write_uncor_cmd(struct nvme_ns *ns, struct nvme_command *c,
			       uint64_t slba, uint32_t nlb);
/* Copy: `ranges' source ranges (format 0 descriptors written into `buf',
 * at least nr * 32 bytes) to sdlba.  -EINVAL when the namespace limits
 * (MSRC, MSSRL, MCL) or ONCS forbid it. */
struct nvme_copy_src {
	uint64_t slba;
	uint32_t nlb; /* blocks, not 0's based */
};
int nvme_init_copy_cmd(struct nvme_ns *ns, struct nvme_command *c, void *buf,
		       size_t buflen, uint64_t sdlba,
		       const struct nvme_copy_src *ranges, unsigned int nr);
/* Fused compare-and-write: the two halves (FUSE_FIRST compare, FUSE_SECOND
 * write) of one atomic operation; -EOPNOTSUPP without FUSES bit 0 or when
 * nlb exceeds ACWU/NACWU. */
int nvme_init_fused_cmp_write(struct nvme_ns *ns, struct nvme_command *cmp,
			      struct nvme_command *wr, uint64_t slba,
			      uint32_t nlb);
int nvme_ns_submit_io_sync(struct nvme_ns *ns, struct nvme_command *c,
			   void *buf, unsigned int len);
/* Asynchronous event hooks the core (P3) calls from its AEN work: a
 * sanitize finished (NVME_AER_CSS sanitize results) / a reservation
 * notification log page is available (reads and logs the pages, which
 * clears the event).  May sleep. */
void nvme_sanitize_event(struct nvme_ctrl *ctrl, uint32_t aen_result);
void nvme_pr_resv_event(struct nvme_ctrl *ctrl);
/* Logical blocks of the namespace <-> the units of the block layer's
 * limits and capacities (the disk's logical sectors). */
static inline uint64_t nvme_ns_sect512_to_disk(const struct nvme_ns_head *h,
					       uint64_t sect512)
{
	/* Before the format is known (lba_shift 0) the disk's initial
	 * logical block is 512 bytes. */
	unsigned int shift = h->lba_shift > 9 ? h->lba_shift - 9u : 0;

	return sect512 >> shift;
}

/* ==========================================================================
 * Owned by P4: nvme_io.c -- command setup and completion
 * ========================================================================== */

/* Build the SQE for a block request (every op: read/write/flush/DSM/
 * write zeroes/zone ops/append/pass-through copy, PI prinfo + reftag,
 * FUA/LR/DTYPE).  BLK_STS_OK or an error status.  Any context. */
blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct blk_request *rq);
/* Release what setup_cmd allocated (DSM range page). */
void nvme_cleanup_cmd(struct blk_request *rq);
/* The transport's ->complete: decide and end (retry with CRD delay,
 * multipath failover, complete with the errno mapping). */
void nvme_complete_rq(struct blk_request *rq);
void nvme_complete_batch_req(struct blk_request *rq);
/* Batch end for polled/irq batches (the transport wraps it). */
void nvme_complete_batch(struct io_comp_batch *iob,
			 void (*fn)(struct blk_request *rq));
/* The transport's CQE handling: record status/result and route to
 * blk_mq_complete_request (or batch).  true when completed here. */
bool nvme_try_complete_req(struct blk_request *rq, __le16 status,
			   union nvme_result result);
enum nvme_disposition nvme_decide_disposition(struct blk_request *rq);
/* NVMe status -> block status (the established mapping). */
blk_status_t nvme_error_status(uint16_t status);
/* Fail with a host path error (queue dead, controller gone). */
blk_status_t nvme_host_path_error(struct blk_request *rq);
/* End a request with its NVMe status mapped. */
void nvme_end_req(struct blk_request *rq);
/* Ops of the request for the command (helpers for P5/P6). */
static inline bool nvme_is_write_rq(struct blk_request *rq)
{
	return rq_data_dir(rq) != 0;
}

/* ==========================================================================
 * Owned by P4: nvme_mpath.c -- native multipath and ANA
 * ========================================================================== */

bool nvme_mpath_enabled(void);
int nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl);
int nvme_mpath_init_identify(struct nvme_ctrl *ctrl,
			     const struct nvme_id_ctrl *id);
void nvme_mpath_uninit(struct nvme_ctrl *ctrl);
void nvme_mpath_stop(struct nvme_ctrl *ctrl);
void nvme_mpath_update(struct nvme_ctrl *ctrl);
int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl, struct nvme_ns_head *head);
void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid);
void nvme_mpath_remove_disk(struct nvme_ns_head *head);
void nvme_mpath_put_disk(struct nvme_ns_head *head);
bool nvme_mpath_clear_current_path(struct nvme_ns *ns);
void nvme_mpath_revalidate_paths(struct nvme_ns *ns);
void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl);
void nvme_mpath_start_request(struct blk_request *rq);
void nvme_mpath_end_request(struct blk_request *rq);
void nvme_mpath_start_freeze(struct nvme_subsystem *subsys);
void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys);
void nvme_mpath_unfreeze(struct nvme_subsystem *subsys);
bool nvme_ctrl_use_ana(struct nvme_ctrl *ctrl);
/* Requeue a failed multipath request onto the head for another path. */
void nvme_failover_req(struct blk_request *rq);
void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl);
struct nvme_ns *nvme_find_path(struct nvme_ns_head *head);
/* ANA log processing (ana_work), AEN ANA change. */
void nvme_ana_work(void *arg);
const char *nvme_ana_state_name(enum nvme_ana_state state);
/* "numa", "round-robin", "queue-depth" (the subsystem iopolicy attribute)
 * and the update the sysfs store calls: clears every path cache. */
const char *nvme_iopolicy_name(int iopolicy);
int nvme_iopolicy_from_name(const char *name);
void nvme_subsys_iopolicy_update(struct nvme_subsystem *subsys, int iopolicy);
void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys);
/* delayed_removal_secs attribute of a multipath head disk. */
void nvme_mpath_set_delayed_removal(struct nvme_ns_head *head,
				    unsigned int secs);
static inline bool nvme_mpath_queue_if_no_path(struct nvme_ns_head *head)
{
	return nvme_test_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);
}

/* ==========================================================================
 * Owned by P4: nvme_zns.c, nvme_pr.c, nvme_feat_ns.c
 * ========================================================================== */

int nvme_query_zone_info(struct nvme_ns *ns, unsigned int lbaf,
			 struct blk_limits *lim);
void nvme_update_zone_info(struct nvme_ns *ns, struct blk_limits *lim);
int nvme_ns_report_zones(struct nvme_ns *ns, uint64_t sector,
			 unsigned int nr_zones, report_zones_cb cb,
			 void *data);
blk_status_t nvme_setup_zone_mgmt_send(struct nvme_ns *ns,
				       struct blk_request *rq,
				       struct nvme_command *cmnd,
				       uint8_t action);
extern const struct pr_ops nvme_pr_ops;
/* Directives: enable streams, read stream params; FDP: config log,
 * placement IDs (head->plids); endurance groups / NVM sets. */
int nvme_enable_streams(struct nvme_ctrl *ctrl);
int nvme_get_stream_params(struct nvme_ctrl *ctrl,
			   struct streams_directive_params *s, uint32_t nsid);
int nvme_query_fdp_info(struct nvme_ns *ns, struct nvme_ns_info *info);
int nvme_endurance_group_log(struct nvme_ctrl *ctrl, uint16_t endgid,
			     struct nvme_endurance_group_log *log);
/* Zoned: one zone management send/receive on a namespace (synchronous;
 * NVMe status / -errno).  May sleep. */
int nvme_zns_mgmt_send(struct nvme_ns *ns, uint64_t slba, uint8_t action,
		       bool select_all);
int nvme_zns_report(struct nvme_ns *ns, uint64_t slba, uint8_t zrasf,
		    bool partial, void *buf, size_t buflen);
/* Directives: identify (supported/enabled bitmaps), disable streams,
 * streams status, allocate resources (granted count out), release an
 * identifier / all resources.  NVMe status / -errno.  May sleep. */
int nvme_identify_directives(struct nvme_ctrl *ctrl, uint32_t nsid,
			     struct nvme_id_directives *id);
int nvme_disable_streams(struct nvme_ctrl *ctrl);
int nvme_get_streams_status(struct nvme_ctrl *ctrl, uint32_t nsid, void *buf,
			    size_t len);
int nvme_alloc_streams(struct nvme_ctrl *ctrl, uint32_t nsid, uint16_t nsr,
		       uint16_t *granted);
int nvme_release_stream_id(struct nvme_ctrl *ctrl, uint32_t nsid,
			   uint16_t sid);
int nvme_release_stream_resources(struct nvme_ctrl *ctrl, uint32_t nsid);
/* FDP: configuration log (kalloc'd, *len its size), reclaim unit handle
 * usage, statistics, events (host or controller), reclaim unit handle
 * status / update on a namespace, FDP events feature (enable/disable
 * event types for a placement handle). */
int nvme_fdp_config_log(struct nvme_ctrl *ctrl, uint16_t endgid, void **log,
			size_t *len);
int nvme_fdp_ruhu_log(struct nvme_ctrl *ctrl, uint16_t endgid, void **log,
		      size_t *len);
int nvme_fdp_stats_log(struct nvme_ctrl *ctrl, uint16_t endgid,
		       struct nvme_fdp_stats_log *log);
int nvme_fdp_events_log(struct nvme_ctrl *ctrl, uint16_t endgid, bool host,
			void **log, size_t *len);
int nvme_fdp_ruh_status(struct nvme_ns *ns, void **buf, size_t *len);
int nvme_fdp_ruh_update(struct nvme_ns *ns, const uint16_t *pids,
			unsigned int nr);
int nvme_fdp_set_events(struct nvme_ctrl *ctrl, uint32_t nsid, uint16_t phndl,
			const uint8_t *types, unsigned int nr, bool enable);
/* The DSPEC/DTYPE of a write for its write stream (FDP placement handle
 * or a streams identifier): control |= DTYPE, dsmgmt |= DSPEC << 16.
 * BLK_STS_INVAL for a stream the namespace does not have. */
blk_status_t nvme_write_stream_dspec(struct nvme_ns *ns, uint16_t stream,
				     uint16_t *control, uint32_t *dsmgmt);
/* Endurance groups / NVM sets: the identify lists (4096 bytes). */
int nvme_identify_endgrp_list(struct nvme_ctrl *ctrl, uint16_t start,
			      struct nvme_id_endurance_group_list *list);
int nvme_identify_nvmset_list(struct nvme_ctrl *ctrl, uint16_t start,
			      struct nvme_id_nvmset_list *list);
int nvme_nvmset_predictable_lat_log(struct nvme_ctrl *ctrl, uint16_t setid,
				    struct nvme_nvmset_predictable_lat_log *log);

/* ==========================================================================
 * Owned by P6: nvme_chrdev.c, nvme_ioctl.c, nvme_admin_ops.c,
 * nvme_debug.c, nvme_fault_inject.c
 * ========================================================================== */

/* /dev/nvmeX (minor = instance on nvme_char_major) and /dev/ngXnY
 * (nvme_ns_chr_major, minor from nvme_ns_chr_minor_ida). */
int nvme_cdev_add_ctrl(struct nvme_ctrl *ctrl);
void nvme_cdev_del_ctrl(struct nvme_ctrl *ctrl);
int nvme_cdev_add_ns(struct nvme_ns *ns);
void nvme_cdev_del_ns(struct nvme_ns *ns);
int nvme_cdev_add_head(struct nvme_ns_head *head);
void nvme_cdev_del_head(struct nvme_ns_head *head);
/* ioctls: controller node, namespace block node (blk_ops->ioctl_mode),
 * generic node, multipath head.  May sleep. */
long nvme_dev_ioctl(struct nvme_ctrl *ctrl, unsigned long cmd, void *argp,
		    struct task *cur, int open_flags);
long nvme_ns_ioctl(struct nvme_ns *ns, unsigned long cmd, void *argp,
		   struct task *cur, int open_flags);
long nvme_ns_head_ioctl(struct nvme_ns_head *head, unsigned long cmd,
			void *argp, struct task *cur, int open_flags);
/* Kernel helpers for admin features. */
int nvme_fw_download(struct nvme_ctrl *ctrl, const void *image, size_t len,
		     uint32_t xfer_size);
int nvme_fw_commit(struct nvme_ctrl *ctrl, uint8_t slot, uint8_t action,
		   uint8_t bpid, uint32_t *result);
int nvme_device_self_test(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t stc);
int nvme_get_telemetry_log(struct nvme_ctrl *ctrl, bool host_initiated,
			   bool create, void **buf, size_t *len);
int nvme_get_persistent_event_log(struct nvme_ctrl *ctrl, uint8_t action,
				  void **buf, size_t *len);
int nvme_security_send_recv(struct nvme_ctrl *ctrl, bool send, uint8_t secp,
			    uint16_t spsp, void *buf, size_t len);
/* Debug: dumps and the command trace ring. */
int nvme_debug_ctrl_init(struct nvme_ctrl *ctrl);
void nvme_debug_ctrl_exit(struct nvme_ctrl *ctrl);
/* Record a submitted / completed command in the trace ring (any
 * context, cheap; compiled to nothing when tracing is off). */
void nvme_trace_submit(struct nvme_ctrl *ctrl, uint16_t qid,
		       const struct nvme_command *cmd);
void nvme_trace_complete(struct nvme_ctrl *ctrl, uint16_t qid,
			 const struct nvme_completion *cqe);
/* Fault injection: attributes and the per-command hook (called by
 * nvme_complete_rq / the transport before completing). */
void nvme_fault_inject_init(struct nvme_fault_inject *fi, const char *name);
void nvme_fault_inject_fini(struct nvme_fault_inject *fi);
void nvme_should_fail(struct blk_request *rq);
/* Should the transport drop this completion (timeout injection)? */
bool nvme_fault_drop_completion(struct nvme_ctrl *ctrl, struct blk_request *rq);

/* --- P6 additions (11-11, see CHANGES.md) ------------------------------- */

/* open_flags bit for nvme_ns_ioctl()/nvme_ns_head_ioctl(): the request
 * came through a PARTITION node of the namespace disk.  Unprivileged
 * pass-through is then refused (a partition must not reach the blocks of
 * the whole namespace).  The disk's ioctl_mode passes
 * nvme_ioctl_open_flags(bdev, open_flags).  Bit 30 is no O_* flag. */
#define NVME_IOCTL_OPEN_PARTITION (1 << 30)
static inline int nvme_ioctl_open_flags(const block_device_t *bdev,
					int open_flags)
{
	return blk_bdev_is_partition(bdev) ?
		       (open_flags | NVME_IOCTL_OPEN_PARTITION) :
		       open_flags;
}

/* The one-file view of a fault injection object found by name ("nvme0",
 * "nvme0n1"), for a sysfs attribute such as /sys/class/nvme/nvmeX/
 * fault_inject (P3).  Grammar of the store side: whitespace or comma
 * separated "key=value" words, keys kind (status|timeout|abort|reset),
 * probability, interval, times, space, status, dont_retry, verbose, and the
 * bare words "off" (probability 0) and "clear" (defaults back).  The show
 * side prints the same keys plus count and injected.  Bytes / -ENODEV when
 * no such object exists any more / -EINVAL.  Process context. */
long nvme_fault_inject_show_name(const char *name, char *buf, long cap);
long nvme_fault_inject_store_name(const char *name, const char *buf,
				  long len);

/* Command trace switch: nvme_core.trace=0 turns the trace hooks off for
 * every controller (default on); /sys/kernel/debug/nvme/nvmeX/trace_enable
 * per controller. */
extern volatile int nvme_trace_enabled;

/* --- P6 internal: between the P6 files only -------------------------------- */

/* Admin feature summaries for the debug tree (nvme_admin_ops.c).  Each
 * returns the length written (may exceed cap: retried bigger) or -errno.
 * May sleep. */
long nvme_admin_show_fw_slots(struct nvme_ctrl *ctrl, char *buf, long cap);
long nvme_admin_show_self_test(struct nvme_ctrl *ctrl, char *buf, long cap);
long nvme_admin_show_telemetry(struct nvme_ctrl *ctrl, bool host,
			       char *buf, long cap);
long nvme_admin_show_pevent(struct nvme_ctrl *ctrl, char *buf, long cap);
long nvme_admin_show_effects(struct nvme_ctrl *ctrl, char *buf, long cap);
/* "short [nsid]" / "extended [nsid]" / "abort" -> nvme_device_self_test. */
long nvme_admin_store_self_test(struct nvme_ctrl *ctrl, const char *buf,
				long len);
/* Get Log Page with retained asynchronous events (RAE) and the offset
 * advanced in transfers of at most the controller's MDTS.  NVMe status >
 * 0 / -errno / 0.  May sleep. */
int nvme_admin_get_log_rae(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t lid,
			   uint8_t lsp, bool rae, void *buf, size_t len,
			   uint64_t offset);

/* Fault attributes by object name for the debug tree (nvme_fault_inject.c):
 * `attr' is one of NVME_FI_ATTR_*. */
enum {
	NVME_FI_ATTR_PROBABILITY = 0,
	NVME_FI_ATTR_INTERVAL,
	NVME_FI_ATTR_TIMES,
	NVME_FI_ATTR_SPACE,
	NVME_FI_ATTR_VERBOSE,
	NVME_FI_ATTR_STATUS,
	NVME_FI_ATTR_DONT_RETRY,
	NVME_FI_ATTR_KIND,
	NVME_FI_ATTR_COUNT, /* read-only */
	NVME_FI_ATTR_INJECTED, /* read-only */
	NVME_FI_ATTR_NR
};
const char *nvme_fault_attr_name(int attr);
bool nvme_fault_attr_writable(int attr);
long nvme_fault_attr_show(const char *name, int attr, char *buf, long cap);
long nvme_fault_attr_store(const char *name, int attr, const char *buf,
			   long len);
/* The names of the live fault injection objects in key order: the one with
 * the smallest key >= *cursor into name (1) / none left (0). */
int nvme_fault_list_names(unsigned int *cursor, char *name, long cap);
bool nvme_fault_name_exists(const char *name);
/* Create /sys/kernel/debug/nvme (and hook the established per-device
 * directories under /sys/kernel/debug) once (nvme_debug.c).  Any context
 * that may take a mutex (polls before the scheduler runs). */
void nvme_debugfs_init_once(void);

/* ==========================================================================
 * Owned by P5: the PCIe transport (entry points the core calls)
 * ========================================================================== */

/* Register the PCI driver (from nvme_init). */
int nvme_pci_init(void);
/* Queue the reset work of every probed controller (from
 * nvme_late_init). */
void nvme_pci_late_init(void);

/* ==========================================================================
 * Compile-time checks
 * ========================================================================== */

static inline void _nvme_core_check(void)
{
	BUILD_BUG_ON(NVME_AQ_MQ_TAG_DEPTH <= 0);
	BUILD_BUG_ON(NVME_CTRL_NR_STATES != 7);
	BUILD_BUG_ON(sizeof(struct nvme_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_completion) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_user_io) != 48);
	BUILD_BUG_ON(sizeof(struct nvme_passthru_cmd) != 72);
	BUILD_BUG_ON(sizeof(struct nvme_passthru_cmd64) != 80);
	BUILD_BUG_ON(sizeof(struct nvme_uring_cmd) != 72);
	BUILD_BUG_ON(NVME_IOCTL_ADMIN_CMD != 0xc0484e41UL);
	BUILD_BUG_ON(NVME_IOCTL_ADMIN64_CMD != 0xc0504e47UL);
	BUILD_BUG_ON(NVME_IOCTL_ID != 0x4e40UL);
	BUILD_BUG_ON(sizeof(struct pr_registration) != 24);
	BUILD_BUG_ON(sizeof(struct pr_read_keys) != 16);
	BUILD_BUG_ON(IOC_PR_REGISTER != 0x401870c8UL);
}

#endif /* KERNEL_DEV_NVME_NVME_H */
