// LikeOS - the extended block layer: requests, queues, disks, partitions
//
// <kernel/dev/block/block.h> is the original, synchronous block device
// interface every filesystem uses.  This header is everything built on top
// of it so that real disk controllers (AHCI/SATA, later others) and the USB
// mass storage driver can present disks the way a production system does:
//
//   * asynchronous REQUESTS (struct blk_request) carrying a scatter/gather
//     list, completed by the driver from any context through an end_io
//     callback; the old synchronous read/write become thin wrappers that
//     submit a request and wait for it;
//   * a REQUEST QUEUE per disk (struct blk_queue, blk_queue.c) with a
//     deadline-style elevator: requests sorted by sector per direction,
//     front/back merging, FIFO expiry so nothing starves, a dispatch depth
//     taken from the device's command queue depth (NCQ), plugging, and the
//     per-disk statistics /proc/diskstats reports;
//   * DISKS (struct gendisk, genhd.c): a registry with the conventional
//     names (sda, sdb ... sdz, sdaa ...; sr0 for optical drives), device
//     numbers (major 8 / 65-71 for disks, 16 minors each; major 11 for
//     optical drives), identity strings for /dev/disk/by-id;
//   * PARTITIONS (struct blk_partition, partition.c): GPT (both headers,
//     CRC32 checked, backup used when the primary is damaged) and MBR
//     (including the extended/logical chain), each partition a block
//     device of its own (sda1 ...) with offset translation;
//   * DEVICE NODES (blkdev_node.c): S_IFBLK nodes in /dev, read()/write()
//     on them, and the established BLK*/HDIO_*/SG_IO/SCSI_IOCTL_* ioctls;
//   * SYSFS/PROCFS (blk_sysfs.c): /sys/block/<disk>/..., /sys/class/block,
//     /proc/partitions, /proc/diskstats;
//   * whole-system SHUTDOWN and SUSPEND/RESUME of every disk.
//
// ---- Return conventions ------------------------------------------------------
// Every function declared in this header returns 0 (or a non-negative count
// where documented) on success and a NEGATIVE ERRNO (-EIO, -ENODEV, -EINVAL,
// -ETIMEDOUT, -ENOMEM, -EBUSY, -EROFS, -ENOMEDIUM, -EOPNOTSUPP ...) on
// failure, using the kernel's errno values (<kernel/ke/syscall.h>).  The
// legacy block.h API keeps its ST_* codes; blk_errno_to_status() below is
// the one conversion between the two worlds.
//
// ---- Context rules -----------------------------------------------------------
//   "any context"   callable with interrupts disabled, from an interrupt
//                   handler or hrtimer callback, with spinlocks held.
//   "may sleep"     process context with interrupts enabled; may block on
//                   a mutex or a completion.  Every such function starts
//                   with might_sleep().  BEFORE the scheduler runs (boot,
//                   interrupts off) these functions still work, by polling:
//                   the disk drivers start in polled mode and blk_wait()
//                   drives completion through ops->poll() instead of
//                   sleeping (see blk_can_sleep()).
//   "registry mutex held" / "queue lock held" as stated per function.
//
// ---- Locking (the order is mandatory, outermost first) -----------------------
//   driver error-handling mutexes (for an ATA disk: port->eh_mutex)
//                           drivers call disk_alloc/disk_add/disk_del and
//                           disk_set_capacity/disk_media_changed while
//                           holding these (probe, hot-plug, revalidation)
//     -> g_disk_registry_mutex (sleeping, struct blk_mutex, genhd.c)
//                           disk/partition registration and removal, name
//                           and minor allocation, the registration step of
//                           partition (re)scans, BLKPG, open counts and
//                           holders.  It is an INNER lock: the block layer
//                           NEVER calls into a driver while holding it --
//                           no ops->submit (no I/O at all: partition tables
//                           and filesystem identities are read before or
//                           after it is taken), no ops->ioctl/open/release/
//                           shutdown/suspend/resume/revalidate/poll, no
//                           end_io.  Walkers (shutdown, suspend/resume,
//                           BLKRRPART) take it only to snapshot the disk
//                           list with a reference on each disk, drop it,
//                           and then call the driver.
//     -> driver spinlocks   (for an ATA disk: port->lock, hba->lock)
//   blk_queue.lock          (spinlock, irqsave) -- a LEAF lock: the
//                           elevator (sorted and FIFO lists, merging, the
//                           merge pool, the batch state) and the dispatch
//                           list.  Taken by submitters and dispatchers only:
//                           the completion of a request never takes it.
//                           NEVER held while calling into a driver
//                           (ops->submit) or an end_io callback, and never
//                           taken while a driver lock is held.  A driver
//                           completes a request from its interrupt handler
//                           with blk_complete_request(), which only queues
//                           it for the block softirq (driver locks may be
//                           held); the end of the request -- end_io, the
//                           waiter's wake-up, then accounting, the tag and
//                           the restart -- runs there with no driver lock
//                           held and no queue lock at all (per-CPU
//                           statistics, an atomic tag table and restart
//                           bit), and the next dispatch is made by kblockd,
//                           never from the completion.
//   blk_queue.flush_lock,   (spinlocks, irqsave) -- leaves of their own: the
//   .requeue_lock,          flush machinery's state, the list of requests
//   .timer_lock,            given back to the queue, the timeout timer's
//   .wait_lock              arming, the submitter/drainer wait-up hand-off.
//                           Completions may take these (never the elevator
//                           lock); none is held across a call out.
//   g_block_lock            (block.c registry spinlock) -- leaf.
//
// ---- Ownership -----------------------------------------------------------------
//   A6 (block layer agent): block.c (extended), blk_queue.c, genhd.c,
//   partition.c, blkdev_node.c, blk_sysfs.c; plus the S_IFBLK support in
//   kernel/fs/device.h + devfs.c, the /proc registrations in procfs.c and
//   the blk_shutdown_all() calls in ke/system.c and the ACPI power-off path.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_BLKDEV_H
#define LIKEOS_BLKDEV_H

#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blk_softirq.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/hal/pci.h>
#include <kernel/uapi/bug.h>
#include <kernel/uapi/types.h>
#include <kernel/uapi/status.h>
#include <kernel/uapi/blkdev.h>
#include <kernel/uapi/hdreg.h>
#include <kernel/uapi/sg.h>
#include <kernel/uapi/scsi.h>

struct task;
struct pfs_node;
struct devfs_node;

/* ==========================================================================
 * Constants
 * ========================================================================== */

/* Device numbers.  The dev_t encoding of this system is major << 8 | minor
 * with 8 bits each (kernel/fs/devfs.c, libc <sys/sysmacros.h>), so every
 * number below fits in 8 bits. */
#define BLK_SD_MAJOR SCSI_DISK0_MAJOR /* 8: sda..sdp */
#define BLK_SD_MAJOR_EXT_FIRST SCSI_DISK1_MAJOR /* 65: sdq.. */
#define BLK_SD_MAJOR_EXT_LAST SCSI_DISK7_MAJOR /* 71 */
#define BLK_SR_MAJOR SCSI_CDROM_MAJOR /* 11: sr0, sr1 ... */
/* Partitions numbered 16 and above have no minor in their disk's range of
 * 16; they get a minor from this extended major, allocated in order of
 * appearance (256 of them system-wide).  The conventional extended block
 * major (259) does not fit in the 8-bit major field, so LikeOS uses 254. */
#define BLK_EXT_MAJOR 254
#define BLK_EXT_MINORS 256

#define DISK_MINORS 16 /* per sd disk: the disk + partitions 1..15 */
#define DISK_MAX_DISKS 128 /* sda .. (16 on major 8, 112 on 65..71) */
#define DISK_MAX_SR 32 /* sr0 .. sr31 */
/* Partition table entries scanned per disk.  GPT allows 128 entries (the
 * usual array size); MBR has 4 primary + up to (128-4) logical. */
#define DISK_MAX_PARTS 128
#define DISK_NAME_LEN 16 /* "sdaaa15" + NUL fits */

/* The longest scatter/gather list one request can carry inline without an
 * allocation; longer lists are allocated by the submitter. */
#define BLK_SG_INLINE 4

/* Default request timeout when the submitter gives none (ms). */
#define BLK_DEFAULT_TIMEOUT_MS 30000
/* The queue's timeout timer: a request's deadline is rounded up by this
 * much so neighbouring deadlines share one firing, the timer is never set
 * further ahead than BLK_MAX_TIMER_NS (a scan then looks again), and an
 * armed timer is moved earlier only when the new expiry is earlier by at
 * least half the slack. */
#define BLK_TIMER_SLACK_NS (1024ULL * 1000ULL * 1000ULL)
#define BLK_MAX_TIMER_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)
/* A driver that ran out of a resource with nothing of the queue's in
 * flight (no completion will come to restart it) is asked again after
 * this long, by kblockd. */
#define BLK_RESOURCE_DELAY_NS (3ULL * 1000ULL * 1000ULL)
/* What a driver's ->timeout wants done with a request that timed out. */
enum blk_eh_timer_return {
	BLK_EH_DONE = 0, /* the driver's error handling completes it */
	BLK_EH_RESET_TIMER = 1, /* not yet: another timeout period */
};
/* Default queue sizes.  nr_requests bounds the requests a queue holds,
 * from their insertion to their completion (dispatched ones included):
 * twice the dispatch depth, the depth counted up to
 * BLK_DEFAULT_NR_REQUESTS.  The dispatch depth is the driver's (NCQ
 * depth) and set at disk_add. */
#define BLK_DEFAULT_NR_REQUESTS 128
#define BLK_DEFAULT_READ_EXPIRE_MS 500
#define BLK_DEFAULT_WRITE_EXPIRE_MS 5000
#define BLK_DEFAULT_FIFO_BATCH 16
#define BLK_DEFAULT_WRITES_STARVED 2
/* A task's plug is flushed early once it holds this many requests (twice
 * that across several queues) or a request of this many bytes. */
#define BLK_MAX_REQUEST_COUNT 32
#define BLK_PLUG_FLUSH_SIZE (128u * 1024u)

/* blk_wait()/blk_mutex polling granularity before the scheduler runs. */
#define BLK_POLL_STEP_US 10

/* ==========================================================================
 * GUIDs
 * ========================================================================== */

/* A GUID in its ON-DISK byte order (the mixed-endian form GPT stores: the
 * first three fields little-endian, the last two big-endian).  Compared as
 * 16 bytes.  blk_guid_to_str() gives the canonical lowercase text form
 * "4f68bce3-e8cd-4db1-96e7-fbcaf984b709". */
struct blk_guid {
	uint8_t b[16];
};

#define BLK_GUID_STR_LEN 37 /* 36 characters + NUL */

/* Build a struct blk_guid from the canonical text fields
 * aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee: a, b and c are stored
 * little-endian, d and e big-endian -- exactly what GPT writes. */
#define BLK_GUID_INIT(a, b, c, d, e)                                          \
	{                                                                     \
		{                                                             \
			(uint8_t)((a) >> 0), (uint8_t)((a) >> 8),             \
				(uint8_t)((a) >> 16), (uint8_t)((a) >> 24),   \
				(uint8_t)((b) >> 0), (uint8_t)((b) >> 8),     \
				(uint8_t)((c) >> 0), (uint8_t)((c) >> 8),     \
				(uint8_t)((d) >> 8), (uint8_t)((d) >> 0),     \
				(uint8_t)((e) >> 40), (uint8_t)((e) >> 32),   \
				(uint8_t)((e) >> 24), (uint8_t)((e) >> 16),   \
				(uint8_t)((e) >> 8), (uint8_t)((e) >> 0)      \
		}                                                             \
	}

/* Well-known GPT partition type GUIDs (partition.c defines the objects). */
extern const struct blk_guid BLK_GUID_ZERO; /* unused entry */
/* c12a7328-f81f-11d2-ba4b-00a0c93ec93b: EFI System Partition */
extern const struct blk_guid BLK_GUID_EFI_SYSTEM;
/* 4f68bce3-e8cd-4db1-96e7-fbcaf984b709: root filesystem for x86-64 (the
 * discoverable-partitions type LikeOS selects its root by) */
extern const struct blk_guid BLK_GUID_ROOT_X86_64;
/* 0fc63daf-8483-4772-8e79-3d69d8477de4: generic filesystem data (what
 * partitioning tools call type 8300) */
extern const struct blk_guid BLK_GUID_GENERIC_DATA;
/* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7: basic data (FAT/NTFS/exFAT) */
extern const struct blk_guid BLK_GUID_BASIC_DATA;
/* e3c9e316-0b5c-4db8-817d-f92df00215ae: reserved (Windows MSR) */
extern const struct blk_guid BLK_GUID_MS_RESERVED;
/* 0657fd6d-a4ab-43c4-84e5-0933c84b4f4f: swap */
extern const struct blk_guid BLK_GUID_SWAP;
/* 933ac7e1-2eb4-4f13-b844-0e14e2aef915: /home */
extern const struct blk_guid BLK_GUID_HOME;
/* 3b8f8425-20e0-4f3b-907f-1a25a76f98e8: /srv */
extern const struct blk_guid BLK_GUID_SRV;
/* 4d21b016-b534-45c2-a9fb-5c16e091fd2d: /var */
extern const struct blk_guid BLK_GUID_VAR;
/* 21686148-6449-6e6f-744e-656564454649: BIOS boot partition */
extern const struct blk_guid BLK_GUID_BIOS_BOOT;
/* e6d6d379-f507-44c2-a23c-238f2a3df928: logical volume manager */
extern const struct blk_guid BLK_GUID_LVM;
/* a19d880f-05fc-4d3b-a006-743f0f84911e: software RAID member */
extern const struct blk_guid BLK_GUID_RAID;

/* GPT partition entry attribute bits (struct blk_partition.gpt_attrs) */
#define BLK_GPT_ATTR_REQUIRED (1ULL << 0) /* platform required */
#define BLK_GPT_ATTR_NO_BLOCK_IO (1ULL << 1) /* firmware: no block I/O */
#define BLK_GPT_ATTR_LEGACY_BOOT (1ULL << 2) /* legacy BIOS bootable */
#define BLK_GPT_ATTR_READ_ONLY (1ULL << 60) /* discoverable: mount ro */
#define BLK_GPT_ATTR_NO_AUTO (1ULL << 63) /* discoverable: do not auto-use */

/* ==========================================================================
 * Driver-local sleeping mutex (block layer)
 * ========================================================================== */

/* A sleeping mutex for the block layer's own serialisation (the disk
 * registry, partition rescans, a request queue's drain).  Built the same
 * way as the USB mass storage driver's per-device I/O lock: a flag and an
 * owner protected by a small spinlock, and sleepers parked on the flag's
 * address as their wait channel (sched_wake_channel() on release).
 *
 * Semantics (implemented in block.c):
 *   - not recursive: locking a mutex the caller already holds is a BUG;
 *   - lock may sleep; with interrupts off or before the scheduler exists
 *     (sched_current() == NULL) it takes a free mutex at once and spins
 *     (with a pause, bounded by nothing -- only another CPU can hold it
 *     then) on a held one;
 *   - unlock from the owner only (WARN otherwise); any context except an
 *     interrupt handler;
 *   - holding it counts as a sleeping lock for the tracer
 *     (task_sleep_lock_taken/dropped);
 *   - release_if_owner(): orphan recovery for a task killed while holding
 *     it, called from the filesystem layer's release_locks_for_task hook. */
struct blk_mutex {
	volatile int locked; /* 0 free, 1 held */
	volatile uint64_t owner; /* task id of the holder, (uint64_t)-1 none */
	const char *name;
	spinlock_t wait_lock; /* protects locked/owner against the sleep race */
	uint32_t contended; /* times a locker had to wait (diagnostics) */
};

#define BLK_MUTEX_INIT(n)                                                \
	{                                                                \
		.locked = 0, .owner = (uint64_t)-1, .name = (n),         \
		.wait_lock = SPINLOCK_INIT(n), .contended = 0            \
	}

void blk_mutex_init(struct blk_mutex *m, const char *name); /* any context */
void blk_mutex_lock(struct blk_mutex *m); /* may sleep (see above) */
int blk_mutex_trylock(struct blk_mutex *m); /* 1 taken, 0 busy; any ctx */
void blk_mutex_unlock(struct blk_mutex *m);
/* 1 when the calling task holds it (for lockdep-style assertions). */
int blk_mutex_held(const struct blk_mutex *m);
int blk_mutex_release_if_owner(struct blk_mutex *m, uint64_t task_id);

/* The disk registry mutex (genhd.c).  Outermost lock of the storage stack. */
extern struct blk_mutex g_disk_registry_mutex;

/* True when the caller may block: interrupts on, a scheduler task to
 * block, and not inside a hard interrupt handler or the block softirq
 * (both run with interrupts enabled at times, on the stack of whatever
 * task they interrupted).  Before the scheduler (boot, polled drivers)
 * every wait polls.  The context queries are only asked once a task
 * exists: before that the softirq framework is not set up. */
static inline int blk_can_sleep(void)
{
	return irqs_enabled() && sched_current() != NULL &&
	       !blk_in_hardirq() && !blk_in_softirq();
}

/* The calling CPU for completion routing and kblockd, -1 before the block
 * softirq framework runs (early boot: per-CPU data may not be set up). */
static inline int blk_this_cpu(void)
{
	return blk_softirq_running() ? (int)this_cpu_id() : -1;
}

/* ==========================================================================
 * kblockd: deferred work of the storage stack (blk_work.c)
 * ========================================================================== */

/* A piece of work that must run in process context with interrupts
 * enabled -- a request queue run (which rings the controller's doorbell),
 * the timeout scan, a requeue -- handed to kblockd by a completion, an
 * interrupt handler or a timer callback.  kblockd is a pool of worker
 * threads per CPU (kblockd/N:M, at the highest priority), each pool with
 * its own list; an item is queued on the current CPU
 * (kblockd_schedule_work, blk_work_queue) or on a chosen one, optionally
 * after a delay (kblockd_mod_delayed_work_on; blk_softirq.h).  One worker
 * of a pool runs its items; when that worker blocks inside an item the
 * scheduler tells the pool, and an idle worker takes the next one.
 *
 * The item is embedded in its owner and initialised once (blk_work_init).
 * Rules:
 *   - PENDING (in `flags') is owned by whoever queued the item: set when
 *     it is queued or its delay timer armed, cleared by the worker when it
 *     takes the item off its list, just before the function runs.  So an
 *     item is never queued twice, a burst of requests costs one run, and a
 *     request that arrives while the item runs queues it again and is not
 *     lost;
 *   - an item never runs on two CPUs at once: queued while it runs, it
 *     goes to the list of the CPU that runs it;
 *   - modifying the delay of a pending item takes it back from its timer
 *     or its list first, then aims it anew;
 *   - the function runs in a kblockd worker's process context with
 *     interrupts enabled; it may sleep (another worker carries on).  Before kblockd exists (boot) an item queued from
 *     process context runs inline in the caller; one queued from an
 *     interrupt waits on its list for the thread (or a poller).
 *
 * The fields belong to blk_work.c (under its per-CPU list locks). */
struct blk_work {
	void (*fn)(void *arg);
	void *arg;
	struct blk_work *next; /* the list it waits on */
	struct blk_work *prev;
	void *list; /* that list (a pool's or a worker's), NULL: none */
	volatile uint32_t flags; /* BLK_WORK_PENDING */
	volatile int pool; /* CPU whose list it waits on, -1: none */
	volatile int last_pool; /* CPU it was last queued on, -1: never */
	volatile int run_cpu; /* CPU whose worker (or poller) runs it, -1 */
	volatile int queued; /* on a list, not yet started */
	volatile int running; /* runs in progress (kblockd or a poller) */
	uint32_t delay_cpu; /* target of a delayed queueing */
	hrtimer_t timer; /* the delay */
};

#define BLK_WORK_PENDING 0x1u

/* Any context.  Zero-initialised storage is NOT enough: the function is
 * needed. */
void blk_work_init(struct blk_work *w, void (*fn)(void *arg), void *arg);
/* kblockd_schedule_work() for the existing callers.  Any context, any
 * lock held (the internal locks are leaves).  Returns 1 when queued now,
 * 0 when it was pending already, -1 when it ran inline because kblockd is
 * not running yet. */
int blk_work_queue(struct blk_work *w);
/* Take `w' back from its timer or its list if it is pending and wait
 * (bounded by `timeout_ms', spinning) until no run of it is in progress,
 * so its owner may free it.  0; -ETIMEDOUT when a run never finished,
 * -EBUSY when called by kblockd from inside the item's own run -- the
 * owner must then leak rather than free.  Any context. */
int blk_work_cancel_sync(struct blk_work *w, uint32_t timeout_ms);
/* Run whatever waits on kblockd's lists in the calling context, this
 * CPU's first.  For pollers that wait with interrupts off once the
 * scheduler runs (kblockd may then never get the processor).  Items
 * another runner is running are left alone.  Any context. */
void blk_work_run_pending(void);
/* 1 once kblockd runs (from then on blk_work_queue() defers). */
int blk_kblockd_running(void);
/* 1 when the caller is one of the kblockd workers. */
int blk_in_kblockd(void);
/* The per-CPU lists; from blk_softirq_init(), boot, interrupts off. */
void kblockd_init(void);

/* ==========================================================================
 * Completion sections (genhd.c)
 * ========================================================================== */

/* A driver completing a command calls back into the block layer, and the
 * last reference to a request, a partition, a disk or the driver's device
 * may go there -- while the driver's completion code still uses objects
 * that release would free (an optical drive's command lives in the drive).
 * So a completion runs as a section: blk_cpl_begin() before,
 * blk_cpl_end() after.  Inside it the "any context" puts park what they
 * would release on the section (struct blk_cpl_ctx, on the completer's
 * stack), and the section's end releases it -- in the same task, once the
 * completion has returned, so no other task can release it early.
 * Sections nest: an inner end hands its parked releases to the outer one.
 * In the block softirq a section belongs to the CPU (not to the task it
 * interrupted); its end cannot release (the softirq never sleeps) and
 * hands everything parked to kblockd instead.  Where the caller cannot
 * sleep otherwise (hard interrupt, interrupts off) a section is inert and
 * the puts use the global deferred lists, which kblockd reaps. */

/* A release a driver parks on a section (blk_cpl_park): `fn' runs at the
 * section's end. */
struct blk_release {
	void (*fn)(struct blk_release *r);
	struct blk_release *next;
};

#define BLK_CPL_PARTS 8

struct blk_cpl_ctx {
	struct blk_cpl_ctx *outer;
	struct blk_request *rqs; /* heap requests, through disp_next */
	struct gendisk *disks; /* through next */
	struct blk_release *items;
	struct blk_partition *parts[BLK_CPL_PARTS];
	int nparts;
	int active; /* 0: inert (the caller could not sleep) */
	int softirq; /* a block softirq section: its end hands to kblockd */
	/* blk_cpl_collect_done(): requests this task completes are kept
	 * here (through softirq_entry.next) instead of going to the block
	 * softirq, and finished by blk_cpl_flush_done() / the end. */
	int collect_done;
	struct blk_softirq_entry *done_head, *done_tail;
};

/* Open / close a completion section.  Any context (inert where the
 * caller cannot sleep); blk_cpl_end() may sleep when it releases. */
void blk_cpl_begin(struct blk_cpl_ctx *c);
void blk_cpl_end(struct blk_cpl_ctx *c);
/* The caller's innermost active section, or NULL (also wherever the
 * caller cannot sleep). */
struct blk_cpl_ctx *blk_cpl_current(void);
/* Park `r' on the caller's section: 1 when parked, 0 when there is none
 * (the caller then releases or defers it itself). */
int blk_cpl_park(struct blk_release *r);
/* An error handler's done queue.  After blk_cpl_collect_done(c) the block
 * requests this task completes (blk_complete_request, from commands its
 * recovery ended) are kept on the section instead of being handed to the
 * block softirq; blk_cpl_flush_done(c) -- called once the handler let go
 * of its own locks and mutexes -- finishes each of them here, in this
 * task (the driver's ->complete: the request ends, or goes back to its
 * queue for a retry).  blk_cpl_end() flushes what is left.  Process
 * context. */
void blk_cpl_collect_done(struct blk_cpl_ctx *c);
void blk_cpl_flush_done(struct blk_cpl_ctx *c);
/* The innermost section of this task that collects completions, or NULL
 * (blk_complete_request). */
struct blk_cpl_ctx *blk_cpl_collector(void);

/* May a last reference dropped by an "any context" put be released right
 * here?  Only where sleeping is allowed and outside a completion section
 * (see above): there it is parked instead. */
static inline int blk_can_release_now(void)
{
	return blk_can_sleep() && !blk_cpl_current();
}
/* Start kblockd.  Boot thread, scheduler running; once. */
void blk_kblockd_start(void);

/* ==========================================================================
 * Requests
 * ========================================================================== */

enum blk_op {
	BLK_OP_READ = 0, /* read `count' sectors into the sg list */
	BLK_OP_WRITE = 1, /* write `count' sectors from the sg list */
	BLK_OP_FLUSH = 2, /* empty the volatile write cache (no data) */
	BLK_OP_DISCARD = 3, /* TRIM/UNMAP `count' sectors (no data) */
	BLK_OP_SECURE_ERASE = 4, /* discard that must destroy the data */
	BLK_OP_WRITE_ZEROES = 5, /* make `count' sectors read back zero */
	BLK_OP_DRV_IN = 6, /* driver pass-through, data from device */
	BLK_OP_DRV_OUT = 7, /* driver pass-through, data to device / none */
	BLK_OP_NR
};

static inline int blk_op_is_write(enum blk_op op)
{
	return op == BLK_OP_WRITE || op == BLK_OP_DISCARD ||
	       op == BLK_OP_SECURE_ERASE || op == BLK_OP_WRITE_ZEROES ||
	       op == BLK_OP_DRV_OUT;
}

/* Does the op move data through the sg list? */
static inline int blk_op_has_data(enum blk_op op)
{
	return op == BLK_OP_READ || op == BLK_OP_WRITE ||
	       op == BLK_OP_DRV_IN || op == BLK_OP_DRV_OUT;
}

/* struct blk_request.flags -- set by the submitter */
#define BLK_REQ_FUA (1u << 0) /* write through to media (no cache) */
#define BLK_REQ_PREFLUSH (1u << 1) /* flush the cache before this write */
#define BLK_REQ_SYNC (1u << 2) /* someone waits: prefer over async */
#define BLK_REQ_META (1u << 3) /* filesystem metadata (accounting) */
#define BLK_REQ_NOMERGE (1u << 4) /* never merge with neighbours */
#define BLK_REQ_FAILFAST (1u << 5) /* no driver retries (probes) */
#define BLK_REQ_NOUNMAP (1u << 6) /* WRITE_ZEROES must not deallocate */
#define BLK_REQ_RAHEAD (1u << 7) /* read-ahead: may be dropped/failed */
#define BLK_REQ_QUIET (1u << 8) /* do not log errors (probing reads) */
#define BLK_REQ_HEAD (1u << 9) /* insert at the queue head (EH, SG_IO) */
#define BLK_REQ_POLLED (1u << 10) /* complete by polling, not interrupt */
#define BLK_REQ_PM (1u << 11) /* power management: allowed while the
				* queue is suspended */
#define BLK_REQ_NO_ACCOUNT (1u << 12) /* not counted in diskstats */

/* struct blk_request.state -- owned by the block layer */
enum blk_req_state {
	BLK_RQ_IDLE = 0, /* initialised, not submitted */
	BLK_RQ_QUEUED, /* in the elevator */
	BLK_RQ_DISPATCHED, /* handed to the driver (ops->submit accepted) */
	BLK_RQ_COMPLETE, /* blk_request_complete() ran */
};

/* One scatter/gather element.  `virt' is a kernel virtual address (the
 * direct map, the heap or a vmalloc-style range -- the driver translates
 * page by page with mm_get_physical_address() and never assumes physical
 * contiguity across a page boundary).  When the submitter already knows
 * the physical address (a page-cache page), it sets `phys' as well and
 * `virt' to phys_to_virt(phys); `phys' != 0 promises the element is
 * physically contiguous.  Lengths are bytes; the sum over the list equals
 * count * logical sector size for READ/WRITE.  Elements may start at any
 * byte offset the device's dma_alignment allows (AHCI: 2-byte). */
struct blk_sg_entry {
	void *virt;
	uint64_t phys; /* 0 = translate virt */
	uint32_t len;
	uint32_t flags; /* reserved, 0 */
};

struct blk_request;
typedef void (*blk_end_io_fn)(struct blk_request *req, int status);

struct blk_request {
	uint32_t magic; /* BLK_REQUEST_MAGIC while initialised */
	enum blk_op op;
	uint32_t flags; /* BLK_REQ_* */
	volatile enum blk_req_state state;

	/* --- what the submitter fills in -------------------------------- */
	block_device_t *bdev; /* as submitted (whole disk or partition) */
	uint64_t lba; /* relative to bdev, in bdev->sector_size units */
	uint32_t count; /* sectors; 0 only for FLUSH and DRV_* */
	struct blk_sg_entry *sg; /* NULL for no data */
	uint32_t nsg;
	uint32_t sg_cap; /* entries available at sg */
	struct blk_sg_entry sg_inline[BLK_SG_INLINE];
	uint32_t timeout_ms; /* 0 = BLK_DEFAULT_TIMEOUT_MS */
	/* Pass-through (BLK_OP_DRV_IN/OUT): a command object the driver
	 * understands -- for an ATA disk a prepared struct ata_queued_cmd
	 * (ata_sat.c, HDIO paths) -- queued in order with ordinary I/O. */
	void *drv_cmd;

	/* Completion callback, called exactly once from blk_request_complete()
	 * -- possibly in INTERRUPT CONTEXT with interrupts disabled: it must
	 * not sleep, must not take a sleeping lock and must not wait for
	 * another request.  NULL: nobody but blk_wait() is interested. */
	blk_end_io_fn end_io;
	void *private_data; /* the submitter's context for end_io */

	/* --- filled in by blk_submit() --------------------------------------- */
	struct gendisk *disk; /* the whole disk */
	struct blk_queue *queue;
	uint64_t sector; /* absolute LBA on the disk (lba + start_lba) */
	uint64_t submit_ns; /* hrtimer_now_ns() at blk_submit */
	uint64_t dispatch_ns; /* when ops->submit accepted it */
	uint64_t complete_ns;
	uint64_t fifo_deadline_ns; /* elevator FIFO expiry */

	/* --- result ------------------------------------------------------- */
	int status; /* 0 or -errno, valid once state == COMPLETE */
	uint32_t resid_bytes; /* bytes not transferred */
	uint32_t retries; /* how often the driver retried it */

	/* --- the driver's while it owns the request (DISPATCHED) ------------ */
	void *driver_data; /* e.g. the ata_queued_cmd carrying it */
	uint32_t driver_tag; /* e.g. the NCQ tag, for traces */

	/* --- completion hand-off (blk_complete_request) -------------------- */
	/* The CPU that inserted the request into its queue.  The driver's
	 * completion is finished in the block softirq of that CPU, or of the
	 * interrupted one when both share the last-level cache.  -1 while the
	 * softirq framework does not run yet (the completion stays local). */
	int submit_cpu;
	/* The driver's result, recorded by blk_complete_request() for the
	 * block softirq (the driver's ->complete, or blk_request_complete()
	 * with these values when it has none). */
	int drv_status;
	uint32_t drv_resid;
	struct blk_softirq_entry softirq_entry;

	/* --- queue linkage (blk_queue.c; under blk_queue.lock) -------------- */
	/* While the request is at the driver its deadline and timed-out
	 * state live in the queue's tag table (struct blk_tag_slot, bl.tag),
	 * never in the request: the timeout scan must not touch a request
	 * that may have completed and been freed meanwhile. */
	struct blk_request *sort_next, *sort_prev; /* per-direction, by sector */
	struct blk_request *fifo_next, *fifo_prev; /* per-direction FIFO */
	struct blk_request *disp_next; /* dispatch list / requeue */
	/* Requests merged INTO this one.  When this one completes, each of
	 * them is completed with the same status (end_io fan-out); their sg
	 * elements were appended to this request's list at merge time. */
	struct blk_request *merged_next;
	uint32_t nr_merged;

	/* --- waiting --------------------------------------------------------- */
	spinlock_t done_lock; /* protects done against the sleep race */
	volatile int done;
	struct wait_queue_head wq;
	refcount_t ref; /* submitter + block layer while in flight */
	/* sg list allocated by blk_request_add_buf() beyond sg_inline, freed
	 * by blk_request_release() */
	struct blk_sg_entry *sg_alloc;

	/* --- block layer private (block.c, blk_queue.c); drivers and
	 * submitters never read or write any of this ------------------------ */
	struct {
		/* Flush sequencing (BLK_RQI_FLUSH_SEQ, blk_queue.c): the
		 * steps (BLK_SEQ_*) already done -- the ones the request does
		 * not need count as done from the start.  While a PREFLUSH or
		 * POSTFLUSH step is due the request waits on the queue's flush
		 * list (flush_next); the queue's own flush request serves every
		 * request waiting there at once. */
		uint32_t seq;
		struct blk_request *flush_next;
		/* Split at submission (block.c): requests of this one still
		 * in flight, and the first byte not known to be done. */
		uint32_t split_pending;
		uint64_t split_done_bytes;
		/* A merge carrier's own list and range, put back when it
		 * completes (merging redirects sg to a queue pool array and
		 * widens lba/sector/count). */
		struct blk_sg_entry *own_sg;
		uint32_t own_nsg, own_sg_cap, own_count;
		uint64_t own_lba, own_sector;
		int merge_slot; /* queue merge-pool array in use, -1 none */
		uint32_t internal; /* BLK_RQI_* */
		/* The queue's tag while the request is at the driver (its
		 * slot in struct blk_queue.tags), -1 while it holds none. */
		int tag;
	} bl;
};

#define BLK_REQUEST_MAGIC 0x52514B42u /* "BKQR" */

/* Initialise a request (any context).  Zeroes it, sets op/bdev/lba/count,
 * points sg at sg_inline (sg_cap = BLK_SG_INLINE), state IDLE, refcount 1,
 * initialises the wait queue and done_lock. */
void blk_request_init(struct blk_request *req, block_device_t *bdev,
		      enum blk_op op, uint64_t lba, uint32_t count);
/* Append a data buffer to the request's sg list, splitting nothing (the
 * driver splits at page boundaries).  Grows the list beyond sg_inline with
 * kalloc when needed.  0, -ENOMEM, -EINVAL (zero length).  Any context
 * that may allocate (not an interrupt handler). */
int blk_request_add_buf(struct blk_request *req, void *buf, uint32_t len);
/* Same for a physically contiguous range known by its physical address. */
int blk_request_add_phys(struct blk_request *req, uint64_t phys, uint32_t len);
/* Drop the submitter's reference; frees a list made by add_buf.  The
 * request memory itself belongs to the caller (stack or its own kalloc). */
void blk_request_release(struct blk_request *req);
/* Allocate + init / free a heap request (for asynchronous submitters whose
 * request outlives the submitting function).  NULL on -ENOMEM. */
struct blk_request *blk_request_alloc(block_device_t *bdev, enum blk_op op,
				      uint64_t lba, uint32_t count);
void blk_request_free(struct blk_request *req);

/* ==========================================================================
 * Queue limits and statistics
 * ========================================================================== */

struct blk_limits {
	uint32_t logical_block_size; /* bytes: 512, 2048 (optical), 4096 */
	uint32_t physical_block_size; /* bytes */
	uint32_t io_min; /* bytes: preferred minimum I/O */
	uint32_t io_opt; /* bytes: optimal I/O, 0 unknown */
	uint32_t alignment_offset; /* bytes, see BLKALIGNOFF */
	uint32_t max_hw_sectors; /* hardware limit per request */
	uint32_t max_sectors; /* soft limit (<= max_hw_sectors) */
	uint32_t max_segments; /* sg elements per request (PRDT size) */
	uint32_t max_segment_size; /* bytes per element (AHCI: 4 MiB) */
	uint64_t seg_boundary_mask; /* an element may not cross this */
	uint32_t dma_alignment; /* mask: buffer address/length alignment */
	uint32_t discard_granularity; /* bytes, 0 = no discard */
	uint32_t discard_alignment; /* bytes */
	uint64_t max_discard_sectors; /* per request */
	uint32_t max_discard_segments; /* ranges per request */
	uint64_t max_write_zeroes_sectors; /* 0 = emulate with writes */
	uint32_t queue_depth; /* commands outstanding at once (NCQ) */
	uint8_t rotational;
	uint8_t write_cache; /* volatile write cache enabled */
	uint8_t fua; /* FUA writes supported */
	uint8_t discard_zeroes_data;
	uint8_t secure_erase; /* BLK_OP_SECURE_ERASE supported */
	uint8_t pad[3];
};

/* Indices of the four /proc/diskstats groups */
enum blk_stat_group {
	BLK_STAT_READ = 0,
	BLK_STAT_WRITE = 1,
	BLK_STAT_DISCARD = 2,
	BLK_STAT_FLUSH = 3,
	BLK_STAT_NR
};

/* The statistics of one device (the disk and each partition keep their
 * own), kept per CPU: a CPU adds to its own counters with interrupts off
 * and nothing else -- no lock is shared between the CPUs that start and
 * complete requests -- and a reader sums every CPU's counters.  in_flight
 * is incremented where a request starts and decremented where it
 * completes, so one CPU's count may be negative; only the sum means
 * something (clamped at 0).  nsecs are the summed request times, io_ticks
 * the milliseconds with something in flight. */
#define BLK_STAT_CPUS MAX_CPUS
struct blk_stat_cpu {
	uint64_t ios[BLK_STAT_NR];
	uint64_t merges[BLK_STAT_NR];
	uint64_t sectors[BLK_STAT_NR]; /* 512-byte sectors */
	uint64_t nsecs[BLK_STAT_NR];
	uint64_t io_ticks_ms;
	int64_t in_flight[2]; /* [0] reads, [1] writes */
};
struct blk_part_stats {
	struct blk_stat_cpu cpu[BLK_STAT_CPUS];
	/* The busy-time stamp (ms since boot): advanced with a
	 * compare-and-exchange, so exactly one CPU adds each interval. */
	volatile uint64_t stamp_ms;
};

/* What /proc/diskstats and /sys/block/<dev>/stat print, per device: the
 * sum of a struct blk_part_stats (blk_queue_stats_snapshot).  Times are
 * milliseconds. */
struct blk_disk_stats {
	uint64_t ios[BLK_STAT_NR]; /* completed requests */
	uint64_t merges[BLK_STAT_NR]; /* requests merged into others */
	uint64_t sectors[BLK_STAT_NR]; /* 512-byte sectors transferred */
	uint64_t ticks_ms[BLK_STAT_NR]; /* time spent on completed requests */
	uint64_t in_flight[2]; /* [0] reads, [1] writes outstanding */
	uint64_t io_ticks_ms; /* time with at least one request in flight */
	uint64_t time_in_queue_ms; /* weighted: in_flight * elapsed */
	uint64_t stamp_ns; /* last io_ticks/time_in_queue update */
};

/* ==========================================================================
 * The request queue (blk_queue.c)
 * ========================================================================== */

enum blk_sched {
	BLK_SCHED_DEADLINE = 0, /* sorted + FIFO expiry (default) */
	BLK_SCHED_NONE = 1, /* FIFO only, still merging (fast SSDs) */
};

#define BLK_QUEUE_FL_STOPPED (1u << 0) /* driver stopped dispatch (EH) */
#define BLK_QUEUE_FL_DYING (1u << 1) /* disk removed: fail everything */
#define BLK_QUEUE_FL_SUSPENDED (1u << 2) /* only BLK_REQ_PM requests run */
#define BLK_QUEUE_FL_NOMERGES (1u << 3) /* sysfs queue/nomerges */
#define BLK_QUEUE_FL_POLLED (1u << 5) /* driver has no interrupts yet */

/* struct blk_queue.state -- bits changed without the queue lock */
/* Dispatch stopped with requests left for want of a tag (or the driver
 * refused one taken from the dispatch list): the completion that frees a
 * tag clears the bit and runs the queue through kblockd. */
#define BLK_QS_SCHED_RESTART (1u << 0)

/* The most commands one queue can have at its driver at once: the tag
 * table's size (the busy bitmap is one 64-bit word).  NCQ allows 32. */
#define BLK_QUEUE_MAX_TAGS 64

/* One tag of the queue: what the timeout scan needs to know about the
 * request at the driver under it -- without ever touching the request,
 * which may complete (and its memory go back to its submitter) at any
 * moment.  `word' packs the state, a generation that changes with every
 * use of the tag, and the deadline in milliseconds; every change of it is
 * a single atomic operation, so a scan that compared it with what it read
 * knows the other fields belong to that same use:
 *   bits 0-1   BLK_TAG_IDLE / BLK_TAG_IN_FLIGHT / BLK_TAG_TIMED_OUT
 *   bits 2-17  generation
 *   bits 18-63 deadline, ms since boot */
struct blk_tag_slot {
	volatile uint64_t word;
	struct blk_request *volatile rq; /* compared, never dereferenced */
	void *volatile driver_data; /* driver_data when it was handed over */
	volatile uint64_t dispatch_ns;
	volatile uint32_t rq_flags; /* BLK_REQ_* (QUIET for the log) */
	volatile uint32_t timeout_ms; /* another period: BLK_EH_RESET_TIMER */
};
#define BLK_TAG_IDLE 0u
#define BLK_TAG_IN_FLIGHT 1u
#define BLK_TAG_TIMED_OUT 2u

struct blk_queue {
	struct gendisk *disk;
	/* The elevator and dispatch list lock (see the locking rules above):
	 * submitters and dispatchers only, never a completion. */
	spinlock_t lock;
	volatile uint32_t flags; /* BLK_QUEUE_FL_*, atomic updates */
	enum blk_sched sched;
	struct blk_limits limits;

	/* elevator, per direction (0 read, 1 write) */
	struct blk_request *sort_head[2]; /* ascending sector */
	struct blk_request *fifo_head[2], *fifo_tail[2]; /* arrival order */
	uint64_t next_sector[2]; /* where the last dispatch ended */
	int batch_dir; /* direction of the running batch */
	uint32_t batching; /* requests dispatched in this batch */
	uint32_t starved; /* read batches since the last write batch */
	uint32_t read_expire_ms, write_expire_ms;
	uint32_t fifo_batch, writes_starved;
	/* The dispatch list: requests that go before anything in the
	 * elevator, in order -- flushes, pass-through, BLK_REQ_HEAD, given-
	 * back requests, and requests a dispatcher took but could not hand
	 * over (no tag, the driver out of a resource). */
	struct blk_request *disp_head, *disp_tail;

	/* counts (written under `lock', read without it) */
	volatile uint32_t nr_queued[2]; /* in the elevator */
	/* The queue's request slots: every request holds one from its
	 * insertion to the end of its completion (BLK_RQI_SCHED_TAG); a
	 * submitter that may sleep waits for one.  At most nr_requests
	 * (sysfs nr_requests), and at most async_depth for anything but a
	 * read, so writes cannot take every slot from the readers.
	 * nr_sched is atomic. */
	volatile uint32_t nr_sched;
	uint32_t nr_requests;
	volatile uint32_t async_depth;
	/* Where a completion finishes (sysfs rq_affinity): 0 where the
	 * interrupt arrived, 1 in the submitting CPU's cache domain
	 * (default), 2 on the submitting CPU itself. */
	volatile uint8_t rq_affinity;
	volatile uint32_t depth; /* dispatch limit (driver queue depth) */
	uint64_t merges_front, merges_back;
	volatile uint64_t dispatched, requeued; /* atomic */

	/* waiters: submitters blocked on nr_requests, drainers.  A waiter
	 * counts itself in nr_waiters before it looks at its condition; a
	 * change that may satisfy one looks at nr_waiters after making the
	 * change and only then takes wait_lock to wake.  Readers wait for a
	 * slot under nr_requests on wait_free, everything else for one under
	 * async_depth on wait_free_async: each is woken only when its own
	 * limit has room. */
	struct wait_queue_head wait_free;
	struct wait_queue_head wait_free_async;
	struct wait_queue_head wait_drain;
	spinlock_t wait_lock;
	volatile uint32_t nr_waiters;

	/* --- blk_queue.c private ------------------------------------------- */
	/* Contexts executing queue code outside every lock that the drain
	 * and teardown would otherwise not see (a submitter between
	 * blk_queue_enter and blk_queue_leave, a dispatcher, the timeout
	 * scan, a queue run or requeue waiting on kblockd).  Atomic. */
	volatile uint32_t active;
	/* A run of the queue is waiting on kblockd (run_work); it holds one
	 * count of `active' until it has run.  Atomic. */
	volatile uint32_t run_pending;
	/* Before kblockd runs a queued run executes inline in the caller;
	 * one asked for while a run of this queue is already going on in
	 * the same (boot) context is left as a note instead (the outer run
	 * repeats), so the inline runs never nest. */
	volatile uint32_t inline_runs, inline_rerun;
	volatile uint32_t nr_bypass; /* requests on the dispatch list */
	volatile uint32_t nr_accounted; /* started for the statistics */
	volatile uint32_t state; /* BLK_QS_* */
	/* How often dispatch recently found the driver busy (a decaying
	 * average): a busy queue dispatches one request per round. */
	volatile uint32_t dispatch_busy;
	/* Firings of timeout_timer still to come or still running: one per
	 * arming that found the timer idle, dropped at the very end of the
	 * callback or by a cancel that caught the arming.  blk_queue_free()
	 * waits for 0 (hrtimer_cancel() does not wait for a callback already
	 * running on another processor).  Atomic. */
	volatile uint32_t timer_fires;
	/* Merge scatter/gather arrays: a request that absorbs neighbours
	 * borrows one of these (merge_pool_seg entries each) so merging never
	 * allocates under the queue lock.  A completion gives its array back
	 * with an atomic bit clear. */
	struct blk_sg_entry *merge_pool;
	volatile uint64_t merge_pool_used; /* bitmap, merge_pool_n <= 64 */
	uint32_t merge_pool_n, merge_pool_seg;
	uint32_t read_ahead_kb; /* BLKRAGET/BLKRASET, queue/read_ahead_kb */
	volatile uint32_t timeouts; /* requests completed with -ETIMEDOUT */
	volatile uint64_t errors; /* requests completed with an error */
	volatile uint64_t completed; /* every completion, merged included */
	/* The elevator's own count of the requests it handed out that
	 * completed (an atomic, the only thing a completion tells it). */
	uint64_t elv_inserted;
	volatile uint64_t elv_completed;
	/* Queue runs that may not be made by the caller (the block softirq
	 * restarting the queue after a completion, a submission with the
	 * elevator in use, a driver out of resources) are made by kblockd:
	 * the dispatch they lead to writes the controller's doorbell. */
	struct blk_work run_work;

	/* --- the tag table: requests at the driver ------------------------ */
	volatile uint64_t tag_busy; /* bit per tag in use */
	struct blk_tag_slot tags[BLK_QUEUE_MAX_TAGS];

	/* --- requests given back to the queue ----------------------------- */
	/* A completion that gives a request back (a retry, a further step, a
	 * flush sequence's data step) and the flush machinery's own flush
	 * request do not insert into the elevator from the softirq: they go
	 * on these lists under their own small lock, and requeue_work on
	 * kblockd inserts them and runs the queue. */
	spinlock_t requeue_lock;
	struct blk_request *requeue_head, *requeue_tail; /* through disp_next */
	struct blk_request *rq_flush_head, *rq_flush_tail;
	volatile uint32_t nr_requeue; /* on either list */
	struct blk_work requeue_work;

	/* --- request timeouts --------------------------------------------- */
	/* ONE coarse timer for the whole queue: armed for the earliest
	 * deadline (rounded up, at most BLK_MAX_TIMER_NS ahead), moved only
	 * when a new deadline is earlier by more than BLK_TIMER_SLACK_NS / 2,
	 * never cancelled when a request completes.  Its callback only queues
	 * timeout_work on kblockd, which scans the busy tags and hands an
	 * expired request to the driver's ->timeout.  Arming under
	 * timer_lock. */
	spinlock_t timer_lock;
	hrtimer_t timeout_timer;
	volatile uint64_t timeout_expires; /* while armed: when it fires */
	struct blk_work timeout_work;

	/* --- the flush machinery (blk_queue.c, under flush_lock) ---------- */
	/* Requests waiting for a flush step: flush_list[flush_pending_idx]
	 * collects, flush_list[flush_running_idx] is served by the flush
	 * request in flight; the two indexes differ while one runs. */
	spinlock_t flush_lock;
	struct blk_request *flush_list[2], *flush_tail[2];
	uint8_t flush_pending_idx, flush_running_idx;
	uint64_t flush_pending_since; /* the first one began waiting */
	uint32_t flush_data_in_flight; /* data steps of flush sequences */
	struct blk_request flush_rq; /* the queue's own FLUSH */
};

/* Set bits of a word (the kernel links no compiler support library, so
 * no __builtin_popcount). */
static inline uint32_t blk_popcount64(uint64_t v)
{
	v = v - ((v >> 1) & 0x5555555555555555ULL);
	v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
	v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
	return (uint32_t)((v * 0x0101010101010101ULL) >> 56);
}

/* Requests the queue's driver has now (tags in use). */
static inline uint32_t blk_queue_nr_in_flight(const struct blk_queue *q)
{
	return blk_popcount64(__atomic_load_n(&q->tag_busy, __ATOMIC_ACQUIRE));
}

/* Has the queue's timeout scan handed this dispatch of `rq' to the
 * driver's ->timeout?  The scan sets the tag's state to BLK_TAG_TIMED_OUT
 * before it calls ->timeout, and every new use of a tag starts
 * BLK_TAG_IN_FLIGHT, so a request that completed, or was dispatched
 * again, since does not match.  Any context; read atomically. */
static inline int blk_rq_timed_out(const struct blk_request *rq)
{
	const struct blk_queue *q = rq->queue;
	int tag = __atomic_load_n(&rq->bl.tag, __ATOMIC_ACQUIRE);

	if (!q || tag < 0 || tag >= BLK_QUEUE_MAX_TAGS)
		return 0;
	if (__atomic_load_n(&q->tags[tag].rq, __ATOMIC_ACQUIRE) != rq)
		return 0;
	return (__atomic_load_n(&q->tags[tag].word, __ATOMIC_ACQUIRE) & 3u) ==
	       BLK_TAG_TIMED_OUT;
}

/* Create the queue of `disk' with `lim' (copied).  Any context that may
 * allocate.  NULL on -ENOMEM. */
struct blk_queue *blk_queue_create(struct gendisk *disk,
				   const struct blk_limits *lim);
/* Tear down: marks DYING, fails every queued request with -ENODEV, waits
 * for in-flight ones (may sleep).  The memory itself goes with the disk's
 * last reference (a holder of an open handle may still look at the queue
 * and must find it dying, not freed).  Registry mutex NOT held (failing
 * the queued requests runs their end_io). */
void blk_queue_destroy(struct blk_queue *q);
/* Insert a validated request (blk_submit's tail) and have the queue run:
 * a request for the elevator goes in and the run is made by kblockd on
 * this CPU (asynchronously); pass-through, HEAD/PM requests and the
 * cache flush go in and the queue is run right here (synchronously, where
 * the caller may run it); with the "none" scheduler the request is handed
 * to the driver directly when a tag is free and nothing waits before it.
 * May be called from any context; never sleeps (a full queue is the
 * SUBMITTER's wait, done in blk_submit before calling this). */
void blk_queue_insert(struct blk_queue *q, struct blk_request *req);
/* Run the queue: when it has work and is not stopped, dispatch -- take
 * requests from the dispatch list, then from the elevator, each with a
 * free tag, and hand them to disk->ops->submit() WITHOUT the queue lock.
 * Several CPUs may run the queue at once: the choice of the next request
 * is serialised by the queue lock, the tags by their atomic bitmap.  No
 * tag free: dispatch stops and the next completion restarts it.  -EBUSY
 * from submit puts the request on the dispatch list and the queue is run
 * again by kblockd at once or after BLK_RESOURCE_DELAY_NS (see
 * blk_queue_dispatch_list).  Runs in the caller when it may sleep
 * (process context: the submitter, kblockd), when it polls with
 * interrupts off, or before kblockd exists; from a hard interrupt or the
 * block softirq the run is handed to kblockd.  Any context. */
void blk_queue_run(struct blk_queue *q);
/* The same, always through kblockd (on this CPU), and only when the queue
 * has work and is not stopped.  Any context. */
void blk_queue_run_async(struct blk_queue *q);
/* The queue's side of a completion, for a request that is no longer used
 * by anybody but the block layer's accounting: statistics, its tag back
 * (the timeout scan forgets it), the elevator's completion count, the
 * restart of a queue whose dispatch stopped for want of a tag (through
 * kblockd), the wake-up of submitters and drainers.  Takes no queue lock;
 * never dispatches.  Any context.  (block.c calls the snapshot form,
 * blk_queue_done_snap, after the request's end_io and waiter.) */
void blk_queue_done(struct blk_queue *q, struct blk_request *req);
/* Put a dispatched request back (driver could not finish it -- EH reset
 * requeue).  Its tag goes back, driver_data cleared, retries counted; it
 * goes on the queue's requeue list, from which kblockd's requeue work
 * puts it at the head of the dispatch list and runs the queue.  Any
 * context (the block softirq included); takes no elevator lock. */
void blk_queue_requeue(struct blk_queue *q, struct blk_request *req);
/* The same for a request the driver has not finished with: its next step
 * (a further chunk, a flush after the data, the same command once more
 * after recovery cancelled it).  driver_data is kept -- ->submit finds
 * its own context there and issues the next step.  Any context. */
void blk_queue_continue(struct blk_queue *q, struct blk_request *req);
/* Stop / restart dispatch (EH freeze and thaw).  Any context. */
void blk_queue_stop(struct blk_queue *q);
void blk_queue_start(struct blk_queue *q);
/* The same with the run left to kblockd (the end of error handling). */
void blk_queue_start_async(struct blk_queue *q);
/* Wait until nothing is queued or in flight.  May sleep (polls before the
 * scheduler).  0 or -ETIMEDOUT. */
int blk_queue_drain(struct blk_queue *q, uint32_t timeout_ms);
/* Per-task plugging.  The plug lives on the submitting task's stack and
 * in task_t.plug between start and finish; requests submitted meanwhile
 * (data, discard, write zeroes) collect on it and go to their queues
 * together -- at blk_finish_plug(), when the plug is full (see
 * BLK_MAX_REQUEST_COUNT), or when the task is about to sleep
 * (blk_flush_plug(plug, 1) from the scheduler: the queues are then run by
 * kblockd).  A nested start is a no-op.  Process context. */
struct blk_plug {
	struct blk_request *head, *tail; /* through disp_next */
	uint32_t rq_count;
	uint8_t multiple_queues;
};
void blk_start_plug(struct blk_plug *plug);
void blk_finish_plug(struct blk_plug *plug);
/* Any context that may take the queue locks; `from_schedule': run the
 * queues through kblockd instead of here, and end a request whose queue
 * is dying through the block softirq rather than inside the scheduler's
 * hook. */
void blk_flush_plug(struct blk_plug *plug, int from_schedule);
/* The driver's queue depth changed (NCQ turned on/off, EH lowered it). */
void blk_queue_set_depth(struct blk_queue *q, uint32_t depth);
/* sysfs queue/scheduler: "mq-deadline"/"deadline" or "none".  -EINVAL. */
int blk_queue_set_scheduler(struct blk_queue *q, const char *name);
/* Suspend: only BLK_REQ_PM requests are dispatched until resume. */
void blk_queue_suspend(struct blk_queue *q);
void blk_queue_resume(struct blk_queue *q);

/* Merging, exposed for the host tests (pure functions of the two requests
 * and the limits): may `b' be appended to `a' (back merge) / prepended
 * (front merge)?  Same op and bdev queue, adjacent sectors, compatible
 * flags (no FUA/PREFLUSH/NOMERGE mix), sum within max_sectors and
 * max_segments, data ops only (discards merge too when both are
 * discards and max_discard_segments allows). */
int blk_rq_can_back_merge(const struct blk_request *a,
			  const struct blk_request *b,
			  const struct blk_limits *lim);
int blk_rq_can_front_merge(const struct blk_request *a,
			   const struct blk_request *b,
			   const struct blk_limits *lim);

/* ==========================================================================
 * Disk operations (what a driver provides)
 * ========================================================================== */

struct gendisk;

/* Shutdown modes for ops->shutdown and blk_shutdown_all() */
enum blk_shutdown_mode {
	BLK_SHUTDOWN_REBOOT = 0, /* flush caches, leave the drive spinning */
	BLK_SHUTDOWN_POWEROFF = 1, /* flush, then STANDBY IMMEDIATE (park) */
	BLK_SHUTDOWN_HALT = 2, /* as poweroff */
};

struct blk_ops {
	const char *name; /* "ahci", "atapi", "usb-storage" */

	/* Start a request.  0: accepted -- the driver will call
	 * blk_request_complete() exactly once, from any context.  -EBUSY:
	 * no command slot/tag free right now (the queue puts it back on its
	 * dispatch list and runs again -- at once, after
	 * BLK_RESOURCE_DELAY_NS, or on the next completion; end_io is NOT
	 * called).  Several CPUs may be in ->submit of one disk at once.  Any
	 * other -errno: rejected, end_io NOT called (the queue completes it
	 * with that status).  Called from any context WITHOUT the queue lock;
	 * must not sleep. */
	int (*submit)(struct gendisk *disk, struct blk_request *req);

	/* Drive completion without interrupts (polled boot mode, blk_wait
	 * before the scheduler).  Returns the number of requests completed.
	 * Any context.  Optional for interrupt-only drivers. */
	int (*poll)(struct gendisk *disk);

	/* Device-specific ioctls (HDIO_*, SG_IO, SCSI_IOCTL_*, eject ...),
	 * tried before the generic BLK* handling.  -ENOTTY: not mine, the
	 * generic code continues.  May sleep.  `argp' is the raw user
	 * pointer; the driver validates and copies. */
	long (*ioctl)(block_device_t *bdev, unsigned long cmd, void *argp,
		      struct task *cur);

	/* A /dev node of the disk (or one of its partitions) is opened /
	 * released.  Optional.  May sleep.  -ENOMEDIUM for an empty
	 * optical drive opened for writing, etc. */
	int (*open)(block_device_t *bdev, int flags);
	void (*release)(block_device_t *bdev);

	/* Media state (removable media): 1 changed since last asked, 0 not,
	 * -errno.  Called by the poller and on open.  May sleep. */
	int (*check_events)(struct gendisk *disk);
	/* Re-read capacity / sector size after a media change or resize.
	 * May sleep. */
	int (*revalidate)(struct gendisk *disk);

	/* System shutdown: flush the write cache, and for POWEROFF/HALT put
	 * the device into standby.  Called once per disk from
	 * blk_shutdown_all() with the queue drained.  May sleep. */
	int (*shutdown)(struct gendisk *disk, enum blk_shutdown_mode mode);
	/* Suspend: flush, then the device's low-power state (STANDBY
	 * IMMEDIATE for ATA); resume: wake it and revalidate.  Queue is
	 * suspended around them.  May sleep. */
	int (*suspend)(struct gendisk *disk);
	int (*resume)(struct gendisk *disk);

	/* The last reference to the disk is gone (after disk_del): free the
	 * driver's per-disk state.  May sleep. */
	void (*free_disk)(struct gendisk *disk);

	/* Optional, preferred over ->ioctl when set: the same call with the
	 * open flags of the node the request came through (O_RDONLY/
	 * O_WRONLY/O_RDWR in the low bits), so SG_IO and friends can allow
	 * writing commands to a caller that opened the node for writing.
	 * Same return convention as ->ioctl. */
	long (*ioctl_mode)(block_device_t *bdev, unsigned long cmd, void *argp,
			   struct task *cur, int open_flags);

	/* The driver's half of a completion, in the block softirq of the
	 * request's CPU (or in the error handler's process context): it
	 * reads what the hardware reported (the driver recorded it before
	 * calling blk_complete_request()), translates it, and either ends
	 * the request with blk_request_complete() or gives it back with
	 * blk_queue_continue()/blk_queue_requeue() for a further step or a
	 * retry.  Never sleeps, never dispatches.  Optional: without it the
	 * request ends with the status and residue blk_complete_request()
	 * recorded. */
	void (*complete)(struct blk_request *req);
	/* A dispatched request passed its deadline.  Called by the queue's
	 * timeout work on kblockd (process context, no lock held), at most
	 * once per dispatch.  BLK_EH_DONE: the driver's error handling owns
	 * the request now and completes it (also the answer when it finds the
	 * request already completing); BLK_EH_RESET_TIMER: give it another
	 * timeout period.  The request may have completed -- and its memory
	 * been reused -- since it was picked: the driver only COMPARES `req'
	 * with what its commands carry, under its own lock, and never
	 * dereferences it.  `driver_data' is the request's driver_data as it
	 * was when the queue handed the request over (a continued request's
	 * context; what ->submit stores afterwards is not seen: the queue
	 * keeps its copy in the request's tag slot and never reads the
	 * request after the hand-over).  Optional: without it the timer is
	 * reset. */
	enum blk_eh_timer_return (*timeout)(struct gendisk *disk,
					    struct blk_request *req,
					    void *driver_data);
	/* A request the driver gave back with its context attached
	 * (blk_queue_continue) is ended by the queue instead of being
	 * dispatched again (the disk is going away): release what
	 * driver_data holds.  Any context, no lock held.  Optional. */
	void (*cleanup_rq)(struct blk_request *req);
};

/* ==========================================================================
 * Disks (genhd.c)
 * ========================================================================== */

enum blk_disk_kind {
	BLK_DISK_SD = 0, /* a disk: sdX, partitionable */
	BLK_DISK_SR = 1, /* an optical drive: srN, not partitioned */
};

/* struct gendisk.flags */
#define GENHD_FL_REMOVABLE (1u << 0) /* removable media */
#define GENHD_FL_MEDIA_PRESENT (1u << 1) /* media in the drive */
#define GENHD_FL_NO_PART_SCAN (1u << 2) /* never look for partitions */
#define GENHD_FL_UP (1u << 3) /* disk_add() completed */
#define GENHD_FL_DEAD (1u << 4) /* disk_del() started */
#define GENHD_FL_SUSPENDED (1u << 5)
#define GENHD_FL_RO (1u << 6) /* write protected */
#define GENHD_FL_HIDDEN (1u << 7) /* no /dev node (internal) */
#define GENHD_FL_HOTPLUGGED (1u << 8) /* appeared after boot */

/* Filesystem identity found on a device -- what /dev/disk/by-uuid and
 * by-label and the root=UUID=/LABEL= boot option are made of.  Probed by
 * blk_probe_fs_identity() (ext2/3/4, FAT12/16/32, exFAT, NTFS, swap,
 * ISO 9660). */
struct blk_fs_identity {
	char type[16]; /* "ext4", "vfat", "swap" ... "" unknown */
	char uuid[40]; /* canonical text form, lowercase; FAT "abcd-ef01" */
	char label[64]; /* UTF-8 */
	uint8_t valid; /* probed and found something */
	uint8_t pad[7];
};

/* Partition table type of a disk */
enum blk_pt_type {
	BLK_PT_NONE = 0,
	BLK_PT_MBR = 1,
	BLK_PT_GPT = 2,
};

struct blk_partition;

struct gendisk {
	uint32_t magic; /* GENDISK_MAGIC */
	enum blk_disk_kind kind;
	uint32_t flags; /* GENHD_FL_* */

	/* The whole-disk block device ("sda").  Embedded: its address is
	 * what the registry, the /dev node and the filesystems see. */
	block_device_t bdev;

	char disk_name[DISK_NAME_LEN]; /* "sda", "sr0" */
	int index; /* 0 = sda (or sr0) */
	uint32_t major, first_minor, minors; /* minors: 16 sd, 1 sr */

	const struct blk_ops *ops;
	void *private_data; /* driver: struct ata_device * etc. */
	struct blk_queue *queue;
	struct blk_limits limits; /* as the driver set them before disk_add */
	uint64_t capacity; /* logical sectors */

	/* identity, for /dev/disk/by-id, /sys/block/X/device, HDIO, SG */
	char vendor[9]; /* SCSI-style vendor ("ATA     " on SATA) */
	char model[41];
	char serial[21];
	char firmware[9];
	char wwn[24]; /* "0x5000039..." or "" */
	char bus[8]; /* "ata", "usb", "scsi" -- by-id prefix */
	const pci_device_t *pci; /* the controller, for /sys links */
	/* SCSI addressing reported by SCSI_IOCTL_GET_IDLUN/SG_GET_SCSI_ID */
	int host_no, channel, target, lun;
	int scsi_type; /* TYPE_DISK / TYPE_ROM */

	/* CHS for HDIO_GETGEO (0 = derive 255/63 from the capacity) */
	uint32_t cylinders;
	uint16_t heads, sectors_per_track;

	/* partitions (index = number; [0] unused) -- registry mutex */
	struct blk_partition *part[DISK_MAX_PARTS + 1];
	int nr_parts; /* highest number in use */
	enum blk_pt_type pt_type;
	struct blk_guid disk_guid; /* GPT disk GUID */
	uint32_t mbr_signature; /* MBR disk signature (by-partuuid) */

	struct blk_fs_identity fs_id; /* when the whole disk holds a fs */
	struct blk_part_stats stats; /* the whole disk (part0), per CPU */

	/* lifetime and registry */
	refcount_t refcount; /* registry + every bdev reference */
	struct gendisk *next; /* registry list (registry mutex) */
	uint64_t diskseq;
	uint64_t add_ns; /* when disk_add() completed */

	/* sysfs: "block/sda" relative to /sys, set by blk_sysfs_add_disk */
	char sysfs_base[96];
	struct devfs_node *node; /* the /dev node (blkdev_node.c) */
};

#define GENDISK_MAGIC 0x4B534944u /* "DISK" */

/* Allocate a disk and assign its name, index and device numbers.  The
 * driver then fills limits/capacity/identity/private_data before
 * disk_add().  `kind' chooses sdX or srN naming.  May sleep (takes the
 * registry mutex briefly).  NULL on -ENOMEM or when the names are
 * exhausted. */
struct gendisk *disk_alloc(enum blk_disk_kind kind, const struct blk_ops *ops,
			   void *private_data);
/* Make the disk live: create its queue from disk->limits, fill the
 * whole-disk block_device_t (extended fields + legacy read/write/sync
 * wrappers), register it, create /dev and /sys entries and the by-id
 * links, scan partitions (unless NO_PART_SCAN or no media) and register
 * each.  Logs "sda: 976773168 512-byte logical blocks (500 GB) ...".
 * May sleep (polls at boot).  Takes the registry mutex for the
 * registration steps only (the partition table is read without it), so a
 * driver may call it with its own error-handling mutex held. */
int disk_add(struct gendisk *disk);
/* Hot-removal or driver detach: marks DEAD, fails new requests with
 * -ENODEV, removes partitions, nodes and sysfs entries, unregisters,
 * destroys the queue, drops the registry reference (the disk is freed --
 * ops->free_disk -- when the last reference goes).  May sleep. */
void disk_del(struct gendisk *disk);
void disk_get(struct gendisk *disk); /* any context */
void disk_put(struct gendisk *disk); /* may sleep when it is the last */
/* Capacity changed (media change, HPA unlocked, revalidate): updates
 * the block devices and bumps diskseq.  May sleep. */
int disk_set_capacity(struct gendisk *disk, uint64_t sectors);
/* Write-protect state (BLKROSET on the disk, media write-protect). */
void disk_set_ro(struct gendisk *disk, int ro);
/* Media appeared/disappeared (optical): updates flags, diskseq, and the
 * ready bit; on arrival revalidates.  May sleep. */
void disk_media_changed(struct gendisk *disk, int present);

/* Pure naming helpers (host-testable): index 0 -> "sda", 25 -> "sdz",
 * 26 -> "sdaa", 701 -> "sdzz", 702 -> "sdaaa".  Partitions append the
 * number, with a 'p' when the disk name ends in a digit ("sr0" never has
 * partitions; kept general).  0 or -EINVAL / -ENAMETOOLONG. */
int disk_name_for_index(int index, char *out, size_t cap);
int disk_part_name(const char *disk_name, int partno, char *out, size_t cap);
/* Device numbers for sd disk `index', partition `partno' (0 = disk):
 * major 8 for index < 16 else 65 + (index-16)/16; minor 16*(index%16) +
 * partno for partno 1..15.  partno >= 16: -ERANGE (use the extended
 * pool, genhd.c allocates).  0 or -errno. */
int disk_devt_for_index(int index, int partno, uint32_t *major,
			uint32_t *minor);

/* Registry lookups.  The returned device carries a reference the caller
 * drops with blk_bdev_put().  May sleep (registry mutex). */
block_device_t *blk_lookup_name(const char *name); /* "sda", "sda1" */
block_device_t *blk_lookup_devt(uint32_t major, uint32_t minor);
/* Partition whose GPT unique GUID (or MBR "xxxxxxxx-NN" form) matches,
 * case-insensitive text. */
block_device_t *blk_lookup_partuuid(const char *partuuid);
/* Partition `nth' (0 = first) of `disk' with GPT type `type'. */
block_device_t *blk_lookup_parttype(struct gendisk *disk,
				    const struct blk_guid *type, int nth);
/* Device holding a filesystem with that UUID / label (probed identity). */
block_device_t *blk_lookup_fs_uuid(const char *uuid);
block_device_t *blk_lookup_fs_label(const char *label);
/* Walk every registered disk in index order with the registry mutex
 * held; stop early when fn returns non-zero (that value is returned). */
int disk_for_each(int (*fn)(struct gendisk *disk, void *arg), void *arg);

/* ==========================================================================
 * Partitions (partition.c)
 * ========================================================================== */

#define PART_FL_LOGICAL (1u << 0) /* MBR logical (in the extended chain) */
#define PART_FL_BOOTABLE (1u << 1) /* MBR active flag / GPT legacy boot */
#define PART_FL_EXTENDED (1u << 2) /* the MBR extended container itself */
#define PART_FL_FROM_BLKPG (1u << 3) /* added by BLKPG, not the table */
#define PART_FL_BACKUP_GPT (1u << 4) /* from the backup GPT header */

struct blk_partition {
	/* The partition's block device ("sda1").  Embedded first; its
	 * part/parent/disk members point back here and to the disk. */
	block_device_t bdev;
	struct gendisk *disk;
	int partno; /* 1-based */
	uint32_t flags; /* PART_FL_* */
	uint64_t start_lba; /* disk logical sectors */
	uint64_t nr_sectors;
	/* GPT */
	struct blk_guid type_guid;
	struct blk_guid part_guid;
	uint64_t gpt_attrs; /* BLK_GPT_ATTR_* */
	char name[72]; /* GPT name (36 UTF-16 units) as UTF-8 */
	/* MBR */
	uint8_t mbr_type; /* 0x83, 0x0c, 0xee ... */
	uint8_t mbr_active;
	uint8_t pad[6];
	/* by-partuuid text: GPT part_guid lowercase, or MBR "%08x-%02x"
	 * (disk signature, partition number) */
	char partuuid[BLK_GUID_STR_LEN];
	char dev_name[DISK_NAME_LEN + 4]; /* "sda1" */
	struct blk_fs_identity fs_id;
	struct blk_part_stats stats; /* per CPU */
	char sysfs_base[112]; /* "block/sda/sda1" */
};

/* One entry found by a table parser (pure, host-testable). */
struct blk_pt_entry {
	int partno;
	uint32_t flags; /* PART_FL_* */
	uint64_t start_lba, nr_sectors;
	struct blk_guid type_guid, part_guid;
	uint64_t gpt_attrs;
	char name[72];
	uint8_t mbr_type, mbr_active;
};

/* The table a parser found. */
struct blk_pt_result {
	enum blk_pt_type type;
	struct blk_guid disk_guid;
	uint32_t mbr_signature;
	int used_backup_gpt; /* the primary GPT was damaged */
	int nr; /* entries in e[] */
	struct blk_pt_entry e[DISK_MAX_PARTS];
	char warn[128]; /* first problem noticed ("" none) */
};

/* Sector reader the parsers use, so they run on the host against an image
 * buffer as well as on a disk.  Returns 0 or -errno; reads `count'
 * logical sectors at `lba' into buf. */
struct blk_pt_reader {
	int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
	void *ctx;
	uint32_t sector_size; /* 512 or 4096 */
	uint64_t nr_sectors;
};

/* Parse a GPT (protective MBR check, primary header + CRC32 of header and
 * entry array, backup header at the last LBA when the primary fails,
 * entries inside the usable range and not overlapping).  0 with
 * out->type == BLK_PT_GPT, -ENOENT no GPT, -EIO/-EINVAL damaged beyond
 * use.  Pure (no allocation beyond the reader's buffer use, no locks). */
int blk_parse_gpt(const struct blk_pt_reader *rd, struct blk_pt_result *out);
/* Parse an MBR (signature 55AA, four primary entries, the extended
 * partition's EBR chain with loop/overlap protection; logical partitions
 * numbered from 5).  A protective 0xEE entry makes it return -ENOENT so
 * the caller tries GPT.  Pure. */
int blk_parse_mbr(const struct blk_pt_reader *rd, struct blk_pt_result *out);
/* Both, GPT first.  Pure. */
int blk_parse_partitions(const struct blk_pt_reader *rd,
			 struct blk_pt_result *out);

/* Scan `disk' and register a partition device for each entry.  Called by
 * disk_add() and rescans with the registry mutex NOT held: the table is
 * read first, the mutex is taken only to register the partitions, and
 * their filesystem identities are probed after it is dropped again.  0
 * (also when no table) or -errno for I/O failure. */
int blk_partition_scan(struct gendisk *disk);
/* BLKRRPART: -EBUSY when a partition is open or mounted, else remove all
 * partitions and scan again.  Takes the registry mutex.  May sleep. */
int blk_partition_rescan(struct gendisk *disk);
/* BLKPG: add/delete/resize one partition (byte offsets as the ioctl
 * gives them).  Takes the registry mutex.  May sleep. */
int blk_partition_add(struct gendisk *disk, int partno, uint64_t start_lba,
		      uint64_t nr_sectors);
int blk_partition_del(struct gendisk *disk, int partno);
int blk_partition_resize(struct gendisk *disk, int partno,
			 uint64_t nr_sectors);
/* Remove every partition of the disk (disk_del, rescan).  Registry mutex
 * held.  -EBUSY when one is open and `force' is 0. */
int blk_partition_remove_all_locked(struct gendisk *disk, int force);

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) as GPT uses it: start
 * with crc = 0, the function pre- and post-inverts. */
uint32_t blk_crc32(uint32_t crc, const void *buf, size_t len);

/* GUID text conversions.  to_str: lowercase canonical form into a
 * BLK_GUID_STR_LEN buffer.  from_str: accepts either case, with or
 * without surrounding braces; 0 or -EINVAL. */
void blk_guid_to_str(const struct blk_guid *g, char *out);
int blk_guid_from_str(const char *s, struct blk_guid *out);
static inline int blk_guid_equal(const struct blk_guid *a,
				 const struct blk_guid *b)
{
	for (int i = 0; i < 16; i++)
		if (a->b[i] != b->b[i])
			return 0;
	return 1;
}
static inline int blk_guid_is_zero(const struct blk_guid *g)
{
	for (int i = 0; i < 16; i++)
		if (g->b[i])
			return 0;
	return 1;
}
/* A short name for well-known types ("EFI System", "root (x86-64)",
 * "generic data", "swap" ...), NULL for unknown. */
const char *blk_guid_type_name(const struct blk_guid *type);

/* Probe the first sectors of `bdev' for a filesystem signature and fill
 * `out' (type, UUID, label).  May sleep (reads).  0, -ENOENT nothing
 * recognised, -EIO. */
int blk_probe_fs_identity(block_device_t *bdev, struct blk_fs_identity *out);

/* ==========================================================================
 * Block device core (block.c, extended)
 * ========================================================================== */

/* Submit a request.  Validates (bdev registered and not DEAD -> -ENODEV;
 * range lba+count within the device -> -EINVAL; writes to a read-only
 * device -> -EROFS; op unsupported by the device (no discard, no flush
 * cache) -> -EOPNOTSUPP, or for FLUSH without a write cache: completes
 * at once with 0), remaps a partition request to the disk (sector =
 * lba + start_lba), stamps submit_ns, takes the in-flight reference and
 * hands it to the disk's queue -- split into pieces that each fit one
 * command of the disk when it is larger (the request ends with the last
 * piece), held on the submitting task's plug while one is open, and
 * through the flush machinery when it asks for PREFLUSH or a FUA the
 * disk cannot do.  When the queue is full (nr_requests)
 * and the caller may sleep it waits for room; otherwise it is queued
 * anyway (boot, interrupt context).
 *
 * Legacy devices (no queue, magic unset): executed synchronously through
 * dev->read/dev->write/dev->sync and completed before returning.
 *
 * Returns 0 when accepted -- end_io will be (or already was) called
 * exactly once -- or -errno when rejected, in which case end_io is NOT
 * called and the request is untouched.  Any context (does not sleep when
 * !blk_can_sleep()). */
int blk_submit(struct blk_request *req);
/* Wait for a submitted request and return its status.  Sleeps on req->wq
 * when blk_can_sleep(), otherwise polls disk->ops->poll() every
 * BLK_POLL_STEP_US.  The request's own timeout is the queue's timeout
 * work and the driver's ->timeout (its error handling completes the
 * request with -ETIMEDOUT); this wait is unbounded.  Uninterruptible: a
 * signal does not end it. */
int blk_wait(struct blk_request *req);
/* blk_submit + blk_wait; returns the status (or the submit error). */
int blk_execute(struct blk_request *req);
/* The hardware finished a dispatched request: record the driver's result
 * and finish the completion in the block softirq -- on the request's
 * submit_cpu, or here when that CPU shares this one's last-level cache.
 * There the driver's ->complete runs (or blk_request_complete() with
 * `status' and `resid_bytes' when it has none).  Any context, driver
 * spinlocks may be held: from a hard interrupt it only queues the
 * request.  A caller that polls with interrupts off (boot, a polled
 * driver) has the softirq part run inline -- with no driver lock held
 * then. */
void blk_complete_request(struct blk_request *req, int status,
			  uint32_t resid_bytes);
/* blk_complete_request() for a driver completing from its own thread
 * (process context, no driver lock held): the softirq part runs right
 * here, inline.  May sleep only as far as the end does not. */
void blk_complete_request_direct(struct blk_request *req, int status,
				 uint32_t resid_bytes);
/* The end of a request: sets status/resid, accounts statistics (disk and
 * partition), completes merged requests, calls end_io, wakes blk_wait,
 * drops the in-flight reference, frees the queue slot and -- when
 * dispatch had stopped for want of one -- restarts the queue through
 * kblockd.  Called by the driver's ->complete (block softirq), by error
 * handling (process context) or by the queue itself (rejection, a dying
 * queue).  Never dispatches; NO driver spinlock may be held. */
void blk_request_complete(struct blk_request *req, int status,
			  uint32_t resid_bytes);

/* Synchronous helpers on top (may sleep; poll at boot).  lba/count in the
 * device's logical sectors; buffers kernel virtual, any alignment the
 * device's dma_alignment allows (callers with odd buffers get a bounce).
 * Return 0 or -errno. */
int blk_read_sync(block_device_t *bdev, uint64_t lba, uint32_t count,
		  void *buf);
int blk_write_sync(block_device_t *bdev, uint64_t lba, uint32_t count,
		   const void *buf, uint32_t flags /* BLK_REQ_FUA ... */);
int blk_flush_sync(block_device_t *bdev);
int blk_discard_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     int secure);
int blk_zeroout_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     uint32_t flags /* BLK_REQ_NOUNMAP */);

/* The legacy callbacks installed into an extended device's read/write/
 * sync members by disk_add() and the partition code, so every existing
 * caller of bdev->read()/write()/sync() goes through the queue.  They
 * return ST_* (blk_errno_to_status of the request status). */
int blk_legacy_read(block_device_t *dev, unsigned long lba,
		    unsigned long count, void *buf);
int blk_legacy_write(block_device_t *dev, unsigned long lba,
		     unsigned long count, const void *buf);
int blk_legacy_sync(block_device_t *dev);

/* Reference counting of block devices (a partition reference also pins
 * its disk).  get: any context; put: may sleep when it is the last. */
void blk_bdev_get(block_device_t *bdev);
void blk_bdev_put(block_device_t *bdev);
/* Open/close accounting and exclusive claim (mount).  Registry mutex
 * taken inside.  claim: -EBUSY when held by another holder. */
int blk_bdev_open(block_device_t *bdev, int flags);
void blk_bdev_close(block_device_t *bdev);
int blk_bdev_claim(block_device_t *bdev, void *holder);
void blk_bdev_unclaim(block_device_t *bdev, void *holder);

/* Whole-system power management of every registered disk (in reverse
 * registration order: partitions are not visited, their disks are).
 *   blk_shutdown_all: drain each queue, ops->shutdown(mode).  Called from
 *     sys_reboot() after sys_sync() and from the ACPI power-off path.
 *     Never fails; logs per-disk errors.  May sleep (polls when not).
 *   blk_suspend_all / blk_resume_all: queue suspend + ops->suspend, and
 *     the reverse.  Triggered by the sysfs power/state attribute (there is
 *     no system sleep yet).  0 or the first error (suspend rolls back the
 *     disks it already suspended on failure). */
void blk_shutdown_all(enum blk_shutdown_mode mode);
int blk_suspend_all(void);
int blk_resume_all(void);

/* The one conversion from this header's -errno to block.h's ST_* codes. */
static inline int blk_errno_to_status(int err)
{
	switch (err) {
	case 0:
		return ST_OK;
	case -EIO:
		return ST_IO;
	case -ETIMEDOUT:
	case -ETIME:
		return ST_TIMEOUT;
	case -ENODEV:
	case -ENXIO:
	case -ENOMEDIUM:
		return ST_NO_DEVICE;
	case -EINVAL:
	case -ERANGE:
		return ST_INVALID;
	case -ENOMEM:
		return ST_NOMEM;
	case -EROFS:
		return ST_ROFS;
	case -EBUSY:
		return ST_BUSY;
	case -EAGAIN:
		return ST_AGAIN;
	case -EOPNOTSUPP:
		return ST_UNSUPPORTED;
	case -ENOSPC:
		return ST_NOSPC;
	case -EACCES:
		return ST_ACCESS;
	case -EPERM:
		return ST_PERM;
	default:
		return err < 0 ? ST_ERR : ST_OK;
	}
}

/* ---- Accessors for the extended block_device_t members ---------------- */

static inline int blk_bdev_is_extended(const block_device_t *bdev)
{
	return bdev && bdev->magic == BLOCK_DEVICE_MAGIC;
}
static inline int blk_bdev_is_partition(const block_device_t *bdev)
{
	return blk_bdev_is_extended(bdev) && bdev->part != NULL;
}
/* The partition record of a partition device, NULL otherwise. */
static inline struct blk_partition *blk_bdev_part(const block_device_t *bdev)
{
	return blk_bdev_is_extended(bdev) ? bdev->part : NULL;
}
/* "" when not a partition */
static inline const char *blk_bdev_partuuid(const block_device_t *bdev)
{
	struct blk_partition *p = blk_bdev_part(bdev);
	return p ? p->partuuid : "";
}
static inline const char *blk_bdev_partname(const block_device_t *bdev)
{
	struct blk_partition *p = blk_bdev_part(bdev);
	return p ? p->name : "";
}
static inline const struct blk_guid *
blk_bdev_parttype(const block_device_t *bdev)
{
	struct blk_partition *p = blk_bdev_part(bdev);
	return p ? &p->type_guid : &BLK_GUID_ZERO;
}
/* The disk's by-id stem ("ata-<model>_<serial>"), built on demand. */
int blk_bdev_disk_id(const block_device_t *bdev, char *out, size_t cap);
/* Device size in bytes. */
static inline uint64_t blk_bdev_bytes(const block_device_t *bdev)
{
	return (uint64_t)bdev->total_sectors * bdev->sector_size;
}

/* ==========================================================================
 * Device nodes and ioctls (blkdev_node.c)
 * ========================================================================== */

/* Create / remove the S_IFBLK node of a registered device (/dev/sda,
 * /dev/sda1, /dev/sr0; mode 0660 root:disk) and the /dev/disk/by-id,
 * by-path, by-partuuid, by-partlabel, by-uuid, by-label links.  May
 * sleep.  Registry mutex held by the caller. */
int blkdev_node_add_locked(block_device_t *bdev);
void blkdev_node_remove_locked(block_device_t *bdev);
/* Re-create the identity links of a device after a rescan/identity probe. */
int blkdev_links_update_locked(block_device_t *bdev);

/* The generic ioctl handler: ops->ioctl first (-ENOTTY falls through),
 * then BLK*, HDIO_GETGEO, the informational SG_* (SG_GET_VERSION_NUM,
 * SG_EMULATED_HOST, SG_GET_SCSI_ID, SG_GET_RESERVED_SIZE ...) and
 * SCSI_IOCTL_GET_IDLUN / GET_BUS_NUMBER.  Permission checks (root for
 * BLKROSET, BLKRRPART, BLKPG, BLKDISCARD, BLKSECDISCARD, BLKZEROOUT on a
 * node not opened for writing...) happen here.  May sleep. */
long blkdev_ioctl(block_device_t *bdev, unsigned long cmd, void *argp,
		  struct task *cur, int open_flags);

/* The devfs groups of disk and optical drive nodes.  The numbers must
 * match res/etc/group ("disk:x:6:", "cdrom:x:24:"). */
#define DEVFS_GID_DISK 6
#define DEVFS_GID_CDROM 24

/* ==========================================================================
 * sysfs / procfs (blk_sysfs.c)
 * ========================================================================== */

/* /sys/block/<disk>/{dev,size,ro,removable,range,ext_range,capability,
 *   stat,inflight,alignment_offset,discard_alignment,diskseq,uevent,
 *   queue/{logical_block_size,physical_block_size,minimum_io_size,
 *   optimal_io_size,max_sectors_kb,max_hw_sectors_kb,max_segments,
 *   rotational,scheduler,nr_requests,discard_granularity,
 *   discard_max_bytes,discard_zeroes_data,write_cache,fua,nomerges},
 *   device/{model,vendor,rev,serial,wwn,queue_depth,state,timeout},
 *   power/state, <part>/{dev,partition,start,size,ro,stat,inflight,
 *   alignment_offset,uevent}}, /sys/class/block/<dev> links and
 * /sys/dev/block/MAJ:MIN.  Registry mutex held. */
int blk_sysfs_add_disk_locked(struct gendisk *disk);
void blk_sysfs_remove_disk_locked(struct gendisk *disk);
int blk_sysfs_add_partition_locked(struct blk_partition *part);
void blk_sysfs_remove_partition_locked(struct blk_partition *part);
/* /proc/partitions and /proc/diskstats show callbacks, registered by
 * procfs_init() (procfs.c gets the two pfs_add_file lines). */
long blk_proc_partitions_show(struct pfs_node *n, char *buf, long cap);
long blk_proc_diskstats_show(struct pfs_node *n, char *buf, long cap);
/* /sys/power/disk_state-style trigger: "suspend" / "resume" written to
 * /sys/block/power_state calls blk_suspend_all/blk_resume_all. */
void blk_sysfs_init(void);

/* ==========================================================================
 * Compile-time layout checks of the UAPI structures
 * ========================================================================== */

/* Called from nowhere at run time; exists so the BUILD_BUG_ONs are
 * compiled in every unit that includes this header. */
static inline void blkdev_uapi_layout_checks(void)
{
	BUILD_BUG_ON(sizeof(struct hd_geometry) != 16);
	BUILD_BUG_ON(sizeof(struct hd_driveid) != 512);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, serial_no) != 20);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, model) != 54);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, lba_capacity) !=
		     120);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, command_set_1) !=
		     164);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, lba_capacity_2) !=
		     200);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, dlf) != 256);
	BUILD_BUG_ON(__builtin_offsetof(struct hd_driveid, integrity_word) !=
		     510);
	BUILD_BUG_ON(sizeof(ide_task_request_t) != 48);
	BUILD_BUG_ON(sizeof(struct sg_io_hdr) != 88);
	BUILD_BUG_ON(__builtin_offsetof(struct sg_io_hdr, dxferp) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct sg_io_hdr, timeout) != 40);
	BUILD_BUG_ON(__builtin_offsetof(struct sg_io_hdr, status) != 64);
	BUILD_BUG_ON(__builtin_offsetof(struct sg_io_hdr, resid) != 72);
	BUILD_BUG_ON(sizeof(struct sg_scsi_id) != 32);
	BUILD_BUG_ON(sizeof(struct scsi_idlun) != 8);
	BUILD_BUG_ON(sizeof(struct blkpg_partition) != 152);
	BUILD_BUG_ON(sizeof(struct blkpg_ioctl_arg) != 24);
	BUILD_BUG_ON(BLKGETSIZE64 != 0x80081272UL);
	BUILD_BUG_ON(BLKBSZGET != 0x80081270UL);
	BUILD_BUG_ON(BLKBSZSET != 0x40081271UL);
	BUILD_BUG_ON(BLKGETDISKSEQ != 0x80081280UL);
	BUILD_BUG_ON(BLKSSZGET != 0x1268UL);
	BUILD_BUG_ON(BLKRRPART != 0x125fUL);
	BUILD_BUG_ON(sizeof(struct blk_guid) != 16);
	BUILD_BUG_ON(DISK_MAX_DISKS > 16 * (1 + BLK_SD_MAJOR_EXT_LAST -
					    BLK_SD_MAJOR_EXT_FIRST + 1));
}

#endif // LIKEOS_BLKDEV_H
