// LikeOS - NVMe host driver: helpers private to the namespace/I/O part
// (nvme_ns.c, nvme_io.c, nvme_mpath.c, nvme_zns.c, nvme_pr.c,
// nvme_feat_ns.c).  Nothing outside these files includes it.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_NVME_NVME_NS_INTERNAL_H
#define KERNEL_DEV_NVME_NVME_NS_INTERNAL_H

#include <kernel/dev/nvme/nvme.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- rate limited messages ------------------------------------------------
 * The conventional default of the established driver's *_ratelimited
 * messages: at most NVME_RL_BURST messages per NVME_RL_INTERVAL_NS per
 * call site, the suppressed count reported when the window reopens.
 * Lock free (a racing pair may print one message more); any context. */
#define NVME_RL_INTERVAL_NS (5ULL * 1000000000ULL)
#define NVME_RL_BURST 10

struct nvme_rl {
	volatile uint64_t begin;
	volatile int printed;
	volatile int missed;
};

static inline bool nvme_rl_ok(struct nvme_rl *rl, const char *who)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t begin = __atomic_load_n(&rl->begin, __ATOMIC_RELAXED);

	if (!begin || now - begin >= NVME_RL_INTERVAL_NS) {
		int missed = __atomic_exchange_n(&rl->missed, 0,
						 __ATOMIC_RELAXED);

		__atomic_store_n(&rl->begin, now ? now : 1, __ATOMIC_RELAXED);
		__atomic_store_n(&rl->printed, 0, __ATOMIC_RELAXED);
		if (missed)
			kprintf(NVME_LOG_PREFIX "%s: %d callbacks suppressed\n",
				who, missed);
	}
	if (__atomic_add_fetch(&rl->printed, 1, __ATOMIC_RELAXED) <=
	    NVME_RL_BURST)
		return true;
	__atomic_add_fetch(&rl->missed, 1, __ATOMIC_RELAXED);
	return false;
}

#define nvme_pr_ratelimited(who, fmt, ...)                              \
	do {                                                            \
		static struct nvme_rl __rl;                             \
		if (nvme_rl_ok(&__rl, (who)))                           \
			kprintf(NVME_LOG_PREFIX "%s: " fmt, (who),      \
				##__VA_ARGS__);                         \
	} while (0)

#define nvme_ctrl_info(ctrl, fmt, ...)                                       \
	kprintf(NVME_LOG_PREFIX "%s: " fmt, (ctrl)->name, ##__VA_ARGS__)
#define nvme_ctrl_warn(ctrl, fmt, ...)                                       \
	kprintf(NVME_LOG_PREFIX "%s: warning: " fmt, (ctrl)->name,           \
		##__VA_ARGS__)
#define nvme_ctrl_err(ctrl, fmt, ...)                                        \
	kprintf(NVME_LOG_PREFIX "%s: error: " fmt, (ctrl)->name,             \
		##__VA_ARGS__)

/* ---- small helpers ---------------------------------------------------------- */

static inline bool nvme_mem_is_zero(const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;

	for (size_t i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

static inline bool nvme_is_power_of_2(uint64_t v)
{
	return v && !(v & (v - 1));
}

static inline uint32_t nvme_rounddown_pow_of_two(uint32_t v)
{
	uint32_t r = 1;

	if (!v)
		return 0;
	while (r <= v / 2)
		r <<= 1;
	return r;
}

static inline unsigned int nvme_ilog2_64(uint64_t v)
{
	unsigned int r = 0;

	while (v >>= 1)
		r++;
	return r;
}

/* Is the controller an I/O controller (not discovery/administrative)? */
static inline bool nvme_is_io_ctrl(const struct nvme_ctrl *ctrl)
{
	return !ctrl->cntrltype || ctrl->cntrltype == NVME_CTRL_TYPE_IO;
}

/* A kalloc'd, zeroed buffer (kalloc does not zero). */
static inline void *nvme_kzalloc(size_t size)
{
	void *p = kalloc(size);

	if (p)
		kmemset(p, 0, size);
	return p;
}

/* ---- cross-file (P4 internal) ------------------------------------------------ */

/* nvme_ns.c */
/* Translate the units of nvme_ns_info and limits; the NVMe status of a
 * fatal namespace error. */
void nvme_ns_remove(struct nvme_ns *ns);
/* The wwid string of a head (/sys/block/X/wwid). */
void nvme_ns_head_wwid(const struct nvme_ns_head *head, char *buf, size_t cap);
/* Fill identity fields (model, serial, firmware, bus, wwid, by-path,
 * device link) of a namespace or multipath disk. */
void nvme_fill_disk_identity(struct nvme_ctrl *ctrl, struct nvme_ns_head *head,
			     struct gendisk *disk);

void nvme_ns_fill_bdev(struct gendisk *disk, const struct blk_limits *lim,
		       bool ro);
void nvme_ns_set_capacity(struct gendisk *disk, uint64_t capacity,
			  uint32_t lbs);
int nvme_ns_commit_limits(struct gendisk *disk, struct blk_limits *lim);
bool nvme_init_integrity(struct nvme_ns_head *head, struct blk_limits *lim,
			 struct nvme_ns_info *info);

/* nvme_mpath.c */
/* struct nvme_ns_head.flags: the multipath disk went through disk_add()
 * (its registry reference is then dropped by disk_del()). */
#define NVME_NSHEAD_DISK_ADDED 2
bool nvme_mpath_multipath_param(void);
bool nvme_mpath_always_on_param(void);
/* Stack the path limits into the head's (after a path revalidated):
 * device configuration from `ns', splitting limits the least common
 * denominator of all paths. */
void nvme_mpath_stack_limits(struct nvme_ns *ns, bool unsupported,
			     uint64_t capacity, bool ro);
void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns);

/* nvme_feat_ns.c */
/* Per-namespace streams parameters (sws/sgs) into the head. */
void nvme_ns_streams_update(struct nvme_ns *ns);
/* Map a write lifetime hint to a streams directive (control/dsmgmt). */
void nvme_assign_write_stream(struct nvme_ctrl *ctrl, struct blk_request *rq,
			      uint16_t *control, uint32_t *dsmgmt);

/* nvme_zns.c */
bool nvme_zns_append_supported(struct nvme_ns *ns);

#endif /* KERNEL_DEV_NVME_NVME_NS_INTERNAL_H */
