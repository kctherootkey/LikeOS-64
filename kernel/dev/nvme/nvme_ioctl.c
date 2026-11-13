// LikeOS - NVMe pass-through ioctls and the block ioctls of namespaces
//
// The NVME_IOCTL_* requests (kernel/uapi/nvme_ioctl.h) on
//   /dev/nvmeX     nvme_dev_ioctl():   admin commands (32- and 64-bit
//                  result), the deprecated I/O command on a controller with
//                  exactly one namespace, controller reset, NVM subsystem
//                  reset, namespace rescan;
//   /dev/nvmeXnY   nvme_ns_ioctl() through the namespace disk's ioctl_mode
//                  (P4): namespace ID, SUBMIT_IO, I/O commands (32/64-bit,
//                  vectored), and the controller's admin commands;
//   /dev/ngXnY     the same, from the generic character node;
//   multipath head nodes: nvme_ns_head_ioctl() picks the current path.
//
// The design is the established one:
//   * PERMISSIONS (nvme_cmd_allowed): CAP_SYS_ADMIN -- capable() in LikeOS
//     -- may send anything.  Without it: no pass-through through a
//     partition node; no vendor-specific or fabrics opcodes; of the admin
//     commands only the Identify CNS values a program needs to build I/O
//     commands (namespace, I/O command set namespace, command set
//     independent namespace, controller, I/O command set controller); I/O
//     commands only when the Commands Supported and Effects log marks them
//     supported with no effects beyond logical block content change, UUID
//     selection and scope, and commands that write data or change block
//     contents only on a descriptor open for writing.  Controller reset,
//     subsystem reset and rescan need the privilege.
//   * The NSID of a pass-through command on a namespace node must be that
//     namespace's.  The flags byte must be 0 (no user-chosen PSDT/fusing).
//   * COMMAND EFFECTS: nvme_passthru_start() before execution (freezes all
//     I/O for commands with command submission and execution effects),
//     nvme_passthru_end() after it (unfreeze, rescan on namespace inventory
//     or capability change, keep-alive update after Set Features KATO) --
//     P3 implements both; this file calls them around every user command.
//   * Return: 0 success, a positive NVMe status, or -errno; the result
//     dword(s) are written back whenever the command was executed.
//
// User buffers (deviation, see DEVIATIONS.md): LikeOS has no API to pin
// user pages for DMA, so data and metadata go through page-aligned,
// physically contiguous, ZEROED kernel bounce buffers.  Data that flows to
// the device is copied in before the command; data that may flow from it
// (every opcode that is not a pure host-to-controller transfer, as the
// established mapping treats them) is copied back after it -- the
// behaviour of the established driver's own bounce path for unaligned user
// buffers.  User pointers are checked up front (validate_user_ptr) and
// every access goes through copy_from_user/copy_to_user (SMAP).
//
// No IOMMU (LikeOS hardening, see DEVIATIONS.md): an UNPRIVILEGED command
// is accepted only when the kernel can bound what the device will
// transfer for it and the buffers given cover that (Read/Write/Compare/
// Zone Append by their block count and the namespace format, the
// reservation, zone and I/O management receive commands by their dword
// counts, DSM/Copy by their range counts, Identify 4 KiB); the device
// would otherwise DMA past the bounce buffer into arbitrary memory.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

/* nvme_submit_user_cmd() flags */
enum {
	NVME_IOCTL_VEC = (1 << 0), /* `addr' is an iovec array */
	NVME_IOCTL_PARTITION = (1 << 1), /* through a partition node */
};

/* The most iovec entries one vectored command may carry (UIO_MAXIOV). */
#define NVME_UIO_MAXIOV 1024u
/* Transfer cap when the queue has no limits yet (the transport's own
 * per-command maximum). */
#define NVME_PASSTHRU_FALLBACK_MAX (8u * 1024u * 1024u)

/* First address above the user half (canonical lower half). */
#define NVME_USER_TOP 0x0000800000000000ULL

/* A whole user range below the top of the user half (validate_user_ptr
 * checks the whole range as well; this bound is kept explicit here). */
static bool nvme_user_range_ok(uint64_t ptr, uint64_t len)
{
	if (!validate_user_ptr(ptr, (size_t)len))
		return false;
	return len <= NVME_USER_TOP && ptr <= NVME_USER_TOP - len;
}

/* SMAP-safe copies of whole, range-checked user spans: 0 / -EFAULT. */
static int nvme_copy_in(void *dst, const void *usrc, size_t n)
{
	if (!n)
		return 0;
	if (!nvme_user_range_ok((uint64_t)usrc, n))
		return -EFAULT;
	return copy_from_user(dst, usrc, n) ? -EFAULT : 0;
}

static int nvme_copy_out(void *udst, const void *src, size_t n)
{
	if (!n)
		return 0;
	if (!nvme_user_range_ok((uint64_t)udst, n))
		return -EFAULT;
	return copy_to_user(udst, src, n) ? -EFAULT : 0;
}

/* The user's struct iovec (x86-64: pointer + size_t). */
struct nvme_user_iovec {
	uint64_t base;
	uint64_t len;
};

/* ==========================================================================
 * Log rate limit (pass-through misuse must not flood the console)
 * ========================================================================== */

static volatile uint64_t g_ioctl_rl_start;
static volatile uint32_t g_ioctl_rl_count;

static bool nvme_ioctl_ratelimit(void)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t start = __atomic_load_n(&g_ioctl_rl_start, __ATOMIC_RELAXED);

	if (now - start > 5000000000ULL &&
	    __atomic_compare_exchange_n(&g_ioctl_rl_start, &start, now, false,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED))
		__atomic_store_n(&g_ioctl_rl_count, 0, __ATOMIC_RELAXED);
	return __atomic_add_fetch(&g_ioctl_rl_count, 1, __ATOMIC_RELAXED) <= 10;
}

static const char *nvme_cur_comm(struct task *cur)
{
	if (!cur)
		cur = sched_current();
	return cur && cur->comm[0] ? cur->comm : "?";
}

/* ==========================================================================
 * Permission model
 * ========================================================================== */

static bool nvme_open_for_write(int open_flags)
{
	int acc = open_flags & 3;

	return acc == O_WRONLY || acc == O_RDWR;
}

/* May the caller send `c' (to `ns', or as an admin command when ns is
 * NULL)? */
static bool nvme_cmd_allowed(struct nvme_ns *ns, struct nvme_command *c,
			     unsigned int flags, bool open_for_write)
{
	uint32_t effects;

	/* No unprivileged pass-through through a partition: that would
	 * escape the partition's containment. */
	if (flags & NVME_IOCTL_PARTITION)
		goto admin;

	/* No vendor-specific or fabrics opcodes: their effects are not
	 * known. */
	if (c->common.opcode >= nvme_cmd_vendor_start ||
	    c->common.opcode == nvme_fabrics_command)
		goto admin;

	/* Admin commands: only the Identify data a program needs to form
	 * proper I/O commands, nothing that could be sensitive. */
	if (!ns) {
		if (c->common.opcode == nvme_admin_identify) {
			switch (c->identify.cns) {
			case NVME_ID_CNS_NS:
			case NVME_ID_CNS_CS_NS:
			case NVME_ID_CNS_NS_CS_INDEP:
			case NVME_ID_CNS_CS_CTRL:
			case NVME_ID_CNS_CTRL:
				return true;
			default:
				break;
			}
		}
		goto admin;
	}

	/* The controller must report the command supported in its effects
	 * log, ... */
	effects = nvme_command_effects(ns->ctrl, ns, c->common.opcode);
	if (!(effects & NVME_CMD_EFFECTS_CSUPP))
		goto admin;

	/* ... without intrusive (or unknown) effects ... */
	if (effects & ~(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC |
			NVME_CMD_EFFECTS_UUID_SEL |
			NVME_CMD_EFFECTS_SCOPE_MASK))
		goto admin;

	/* ... and data to the device or block content changes only on a
	 * descriptor open for writing. */
	if ((nvme_is_write(c) || (effects & NVME_CMD_EFFECTS_LBCC)) &&
	    !open_for_write)
		goto admin;

	return true;
admin:
	return capable() != 0;
}

/* ---- the DMA bound of unprivileged commands (no IOMMU) ------------------ */

/* Bytes `c' makes the device transfer through the data pointer and the
 * metadata pointer, when the kernel knows them; false when it cannot tell
 * (then only a privileged caller may send it with data). */
static bool nvme_cmd_xfer_bound(struct nvme_ns *ns,
				const struct nvme_command *c,
				uint64_t *data_bytes, uint64_t *meta_bytes)
{
	uint8_t op = c->common.opcode;

	*data_bytes = 0;
	*meta_bytes = 0;
	if (!ns) {
		/* Admin: the only unprivileged one is Identify (4 KiB). */
		if (op == nvme_admin_identify) {
			*data_bytes = NVME_IDENTIFY_DATA_SIZE;
			return true;
		}
		return NVME_OPCODE_XFER(op) == NVME_OPCODE_XFER_NONE;
	}
	switch (op) {
	case nvme_cmd_read:
	case nvme_cmd_write:
	case nvme_cmd_compare:
	case nvme_cmd_zone_append: {
		struct nvme_ns_head *head = ns->head;
		uint64_t nlb = (uint64_t)le16_to_cpu(c->rw.length) + 1;
		uint16_t control = le16_to_cpu(c->rw.control);
		uint64_t meta = nlb * head->ms;

		*data_bytes = nlb << head->lba_shift;
		/* PRACT with PI-only metadata: the controller inserts or
		 * strips it, nothing is transferred. */
		if ((control & NVME_RW_PRINFO_PRACT) && head->ms == head->pi_size)
			meta = 0;
		if (head->features & NVME_NS_EXT_LBAS)
			*data_bytes += meta;
		else
			*meta_bytes = meta;
		return true;
	}
	case nvme_cmd_resv_report:
		*data_bytes = ((uint64_t)le32_to_cpu(c->common.cdw10) + 1) * 4;
		return true;
	case nvme_cmd_zone_mgmt_recv:
		*data_bytes = ((uint64_t)le32_to_cpu(c->common.cdw12) + 1) * 4;
		return true;
	case nvme_cmd_io_mgmt_recv:
		*data_bytes = ((uint64_t)le32_to_cpu(c->common.cdw11) + 1) * 4;
		return true;
	case nvme_cmd_dsm:
		*data_bytes =
			((uint64_t)(le32_to_cpu(c->common.cdw10) & 0xff) + 1) *
			sizeof(struct nvme_dsm_range);
		return true;
	case nvme_cmd_copy: {
		uint32_t cdw12 = le32_to_cpu(c->common.cdw12);
		uint64_t nr = (uint64_t)(cdw12 & 0xff) + 1;
		uint32_t desc_fmt = (cdw12 >> 8) & 0xf;

		/* Source range descriptor formats 0/1: 32 bytes, 2/3: 40. */
		if (desc_fmt > 3)
			return false;
		*data_bytes = nr * (desc_fmt >= 2 ? 40u : 32u);
		return true;
	}
	case nvme_cmd_resv_register:
	case nvme_cmd_resv_acquire:
		*data_bytes = 16;
		return true;
	case nvme_cmd_resv_release:
		*data_bytes = 8;
		return true;
	case nvme_cmd_zone_mgmt_send:
		/* Only "set zone descriptor extension" (0x10) carries data,
		 * of a size the kernel does not track. */
		return ((le32_to_cpu(c->common.cdw13) & 0xff) != 0x10);
	case nvme_cmd_flush:
	case nvme_cmd_write_zeroes:
	case nvme_cmd_write_uncor:
	case nvme_cmd_verify:
	case nvme_cmd_cancel:
		return true;
	default:
		return NVME_OPCODE_XFER(op) == NVME_OPCODE_XFER_NONE;
	}
}

/* 0 when an unprivileged caller's buffers cover what `c' transfers. */
static int nvme_unpriv_xfer_check(struct nvme_ns *ns,
				  const struct nvme_command *c, bool has_data,
				  uint64_t data_len, bool has_meta,
				  uint64_t meta_len)
{
	uint64_t need_data, need_meta;

	if (!nvme_cmd_xfer_bound(ns, c, &need_data, &need_meta))
		return -EPERM;
	if (need_data && (!has_data || data_len < need_data))
		return -EINVAL;
	if (need_meta && (!has_meta || meta_len < need_meta))
		return -EINVAL;
	return 0;
}

/* ==========================================================================
 * Bounce buffers
 * ========================================================================== */

struct nvme_bounce {
	void *va; /* page aligned, zeroed */
	uint64_t phys; /* contiguous pages, 0: heap */
	void *raw; /* heap allocation behind va */
	size_t pages;
	size_t len;
};

static int nvme_bounce_alloc(struct nvme_bounce *b, size_t len)
{
	mm_memset(b, 0, sizeof(*b));
	if (!len)
		return 0;
	b->len = len;
	b->pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
	b->phys = mm_allocate_contiguous_pages(b->pages);
	if (b->phys) {
		b->va = phys_to_virt(b->phys);
	} else {
		/* Fragmented memory: page-aligned heap memory (the transport
		 * maps it page by page). */
		b->raw = kalloc(b->pages * PAGE_SIZE + PAGE_SIZE);
		if (!b->raw)
			return -ENOMEM;
		b->va = (void *)(((uint64_t)b->raw + PAGE_SIZE - 1) &
				 ~((uint64_t)PAGE_SIZE - 1));
	}
	VM_BUG_ON(((uint64_t)b->va & (PAGE_SIZE - 1)) != 0);
	mm_memset(b->va, 0, b->pages * PAGE_SIZE);
	return 0;
}

static void nvme_bounce_free(struct nvme_bounce *b)
{
	if (b->phys)
		mm_free_contiguous_pages(b->phys, b->pages);
	else if (b->raw)
		kfree(b->raw);
	mm_memset(b, 0, sizeof(*b));
}

/* What nvme_map_user_request() set up, undone by nvme_unmap_user_request. */
struct nvme_umap {
	struct nvme_bounce data;
	struct nvme_bounce meta;
	struct nvme_user_iovec *iov; /* vectored: the user's segments */
	uint32_t nr_iov;
	uint64_t ubuf; /* non-vectored user buffer */
	uint64_t umeta;
	bool copy_out; /* the device may write the data buffer */
};

/* Copy between the bounce and the user's buffer(s); to_user selects the
 * direction.  0 / -EFAULT. */
static int nvme_umap_copy(struct nvme_umap *m, bool to_user)
{
	if (m->iov) {
		uint64_t off = 0;

		for (uint32_t i = 0; i < m->nr_iov; i++) {
			uint64_t l = m->iov[i].len;

			if (!l)
				continue;
			if (off + l > m->data.len)
				return -EFAULT; /* cannot happen: summed */
			if (to_user ?
				    nvme_copy_out((void *)m->iov[i].base,
						 (uint8_t *)m->data.va + off,
						 (size_t)l) :
				    nvme_copy_in((uint8_t *)m->data.va + off,
						   (const void *)m->iov[i].base,
						   (size_t)l))
				return -EFAULT;
			off += l;
		}
		return 0;
	}
	if (!m->data.len)
		return 0;
	if (to_user)
		return nvme_copy_out((void *)m->ubuf, m->data.va, m->data.len) ?
			       -EFAULT :
			       0;
	return nvme_copy_in(m->data.va, (const void *)m->ubuf, m->data.len) ?
		       -EFAULT :
		       0;
}

/* The longest transfer `q' takes in one command.  A namespace queue
 * counts its limits in the disk's logical blocks (4 KiB on many drives),
 * the admin queue in 512-byte sectors: bytes are blocks times the
 * queue's logical block size either way. */
static uint64_t nvme_queue_max_bytes(struct nvme_ctrl *ctrl, struct blk_queue *q)
{
	uint64_t max = 0;

	if (q && q->limits.max_hw_sectors)
		max = (uint64_t)q->limits.max_hw_sectors *
		      (q->limits.logical_block_size ?
			       q->limits.logical_block_size :
			       512u);
	else if (ctrl->max_hw_sectors)
		max = (uint64_t)ctrl->max_hw_sectors << 9;
	if (!max || max > NVME_PASSTHRU_FALLBACK_MAX)
		max = NVME_PASSTHRU_FALLBACK_MAX;
	return max;
}

/* Import the user's iovec array (vectored commands): the segments into
 * m->iov, their total into *total.  0 / -EINVAL / -EFAULT / -ENOMEM. */
static int nvme_import_iovec(struct nvme_umap *m, uint64_t uiov,
			     uint32_t nr_segs, uint64_t max_bytes,
			     uint64_t *total)
{
	uint64_t sum = 0;
	size_t bytes;

	if (!nr_segs || nr_segs > NVME_UIO_MAXIOV)
		return -EINVAL;
	bytes = (size_t)nr_segs * sizeof(struct nvme_user_iovec);
	if (!nvme_user_range_ok(uiov, bytes))
		return -EFAULT;
	m->iov = kalloc(bytes);
	if (!m->iov)
		return -ENOMEM;
	if (nvme_copy_in(m->iov, (const void *)uiov, bytes)) {
		kfree(m->iov);
		m->iov = NULL;
		return -EFAULT;
	}
	m->nr_iov = nr_segs;
	for (uint32_t i = 0; i < nr_segs; i++) {
		uint64_t l = m->iov[i].len;

		if (!l)
			continue;
		if (l > max_bytes || sum + l < sum || sum + l > max_bytes)
			return -EINVAL;
		if (!nvme_user_range_ok(m->iov[i].base, l))
			return -EFAULT;
		sum += l;
	}
	if (!sum)
		return -EINVAL;
	*total = sum;
	return 0;
}

static void nvme_unmap_user_request(struct nvme_umap *m)
{
	nvme_bounce_free(&m->data);
	nvme_bounce_free(&m->meta);
	if (m->iov)
		kfree(m->iov);
	m->iov = NULL;
	m->nr_iov = 0;
}

/* Map the user's data (and metadata) buffer onto `rq' through bounces. */
static int nvme_map_user_request(struct blk_request *rq, struct nvme_ctrl *ctrl,
				 struct nvme_ns *ns, struct nvme_command *c,
				 struct nvme_umap *m, uint64_t ubuffer,
				 uint32_t bufflen, uint64_t meta_buffer,
				 uint32_t meta_len, unsigned int flags,
				 uint64_t total_len)
{
	bool has_metadata = meta_buffer && meta_len;
	bool supports_metadata = ns && ns->disk && blk_get_integrity(ns->disk);
	uint8_t xfer = NVME_OPCODE_XFER(c->common.opcode);
	int ret;

	(void)ctrl;
	(void)bufflen;
	if (has_metadata && !supports_metadata)
		return -EINVAL;

	m->ubuf = (flags & NVME_IOCTL_VEC) ? 0 : ubuffer;
	m->umeta = meta_buffer;
	/* Pinned user pages would see whatever the device writes into them:
	 * everything but a pure host-to-controller transfer is copied back. */
	m->copy_out = xfer != NVME_OPCODE_XFER_H2C;

	ret = nvme_bounce_alloc(&m->data, (size_t)total_len);
	if (ret)
		return ret;
	if (nvme_is_write(c)) {
		ret = nvme_umap_copy(m, false);
		if (ret)
			return ret;
	}
	ret = blk_rq_map_kern(rq, m->data.va, (unsigned int)total_len);
	if (ret)
		return ret;

	if (has_metadata) {
		if (!nvme_user_range_ok(meta_buffer, meta_len))
			return -EFAULT;
		ret = nvme_bounce_alloc(&m->meta, meta_len);
		if (ret)
			return ret;
		if (nvme_is_write(c) &&
		    nvme_copy_in(m->meta.va, (const void *)meta_buffer,
				   meta_len))
			return -EFAULT;
		ret = blk_rq_integrity_map_kern(rq, m->meta.va, meta_len,
						BIP_INTEGRITY_USER);
		if (ret)
			return ret;
	}
	return 0;
}

/* ==========================================================================
 * Command submission
 * ========================================================================== */

static int nvme_user_req_op(const struct nvme_command *c)
{
	return nvme_is_write(c) ? BLK_OP_DRV_OUT : BLK_OP_DRV_IN;
}

static struct blk_request *nvme_alloc_user_request(struct blk_queue *q,
						   struct nvme_command *cmd,
						   int *err)
{
	struct blk_request *rq;

	*err = 0;
	rq = blk_mq_alloc_request(q, nvme_user_req_op(cmd), 0, err);
	if (!rq) {
		if (!*err)
			*err = -ENOMEM;
		return NULL;
	}
	nvme_init_request(rq, cmd);
	nvme_req(rq)->flags |= NVME_REQ_USERCMD;
	return rq;
}

/* Run one user command on `q' (ns->queue, or the admin queue when ns is
 * NULL).  `bufflen' is the byte count, or the iovec count with
 * NVME_IOCTL_VEC.  Returns 0 / NVMe status > 0 / -errno; *result gets the
 * completion's dwords 0-1 once the command ran. */
static int nvme_submit_user_cmd(struct nvme_ctrl *ctrl, struct blk_queue *q,
				struct nvme_ns *ns, struct nvme_command *cmd,
				uint64_t ubuffer, uint32_t bufflen,
				uint64_t meta_buffer, uint32_t meta_len,
				uint64_t *result, uint32_t timeout_ms,
				unsigned int flags, bool privileged)
{
	struct nvme_umap map;
	struct blk_request *rq;
	uint64_t total = 0, max_bytes;
	bool has_data = ubuffer && bufflen;
	uint32_t effects;
	int ret, err, copy_ret = 0;

	might_sleep();
	if (!q)
		return -ENODEV;
	mm_memset(&map, 0, sizeof(map));
	max_bytes = nvme_queue_max_bytes(ctrl, q);

	/* Validate and size the user's buffers before anything happens. */
	if (has_data) {
		if (flags & NVME_IOCTL_VEC) {
			ret = nvme_import_iovec(&map, ubuffer, bufflen,
						max_bytes, &total);
			if (ret)
				goto out_unmap;
		} else {
			total = bufflen;
			if (total > max_bytes) {
				ret = -EINVAL;
				goto out_unmap;
			}
			if (!nvme_user_range_ok(ubuffer, total)) {
				ret = -EFAULT;
				goto out_unmap;
			}
		}
	}
	if (meta_buffer && meta_len && meta_len > max_bytes) {
		ret = -EINVAL;
		goto out_unmap;
	}
	if (!privileged) {
		ret = nvme_unpriv_xfer_check(ns, cmd, has_data, total,
					     meta_buffer && meta_len, meta_len);
		if (ret) {
			if (nvme_ioctl_ratelimit())
				kprintf(NVME_LOG_PREFIX
					"%s: unprivileged pass-through opcode "
					"0x%02x refused: %s\n",
					ctrl->name,
					(unsigned)cmd->common.opcode,
					ret == -EPERM ?
						"transfer size unknown" :
						"buffer smaller than the transfer");
			if (ret == -EPERM)
				ret = -EACCES;
			goto out_unmap;
		}
	}

	rq = nvme_alloc_user_request(q, cmd, &err);
	if (!rq) {
		ret = err;
		goto out_unmap;
	}
	/* 0: the queue's default (the admin or I/O timeout). */
	if (timeout_ms)
		rq->timeout_ms = timeout_ms;
	if (has_data) {
		ret = nvme_map_user_request(rq, ctrl, ns, cmd, &map, ubuffer,
					    bufflen, meta_buffer, meta_len,
					    flags, total);
		if (ret)
			goto out_free_req;
	}

	effects = nvme_passthru_start(ctrl, ns, cmd->common.opcode);
	ret = nvme_execute_user_rq(rq);
	if (result)
		*result = le64_to_cpu(nvme_req(rq)->result.u64);
	/* The command is over: the controller owns none of the bounce
	 * memory any more (a timed-out command is only cancelled after the
	 * controller was disabled). */
	if (has_data && map.copy_out)
		copy_ret = nvme_umap_copy(&map, true);
	if (has_data && map.meta.len && map.copy_out &&
	    nvme_copy_out((void *)map.umeta, map.meta.va, map.meta.len))
		copy_ret = -EFAULT;
	blk_mq_free_request(rq);
	nvme_unmap_user_request(&map);

	if (effects)
		nvme_passthru_end(ctrl, ns, effects, cmd, ret);
	/* A buffer that went away while the command ran: the data is lost,
	 * say so (pinned pages could not have gone away). */
	if (copy_ret && ret >= 0)
		ret = copy_ret;
	return ret;

out_free_req:
	blk_mq_free_request(rq);
out_unmap:
	nvme_unmap_user_request(&map);
	return ret;
}

/* NVME_IOCTL_SUBMIT_IO: a read, write or compare from struct
 * nvme_user_io. */
static int nvme_submit_io(struct nvme_ns *ns, struct nvme_user_io *uio)
{
	struct nvme_user_io io;
	struct nvme_command c;
	uint64_t length, meta_len;
	uint64_t metadata;

	if (nvme_copy_in(&io, uio, sizeof(io)))
		return -EFAULT;
	if (io.flags)
		return -EINVAL;

	switch (io.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
	case nvme_cmd_compare:
		break;
	default:
		return -EINVAL;
	}

	length = ((uint64_t)io.nblocks + 1) << ns->head->lba_shift;

	if ((io.control & NVME_RW_PRINFO_PRACT) &&
	    ns->head->ms == ns->head->pi_size) {
		/* Protection information is stripped/inserted by the
		 * controller. */
		if (io.metadata)
			return -EINVAL;
		meta_len = 0;
		metadata = 0;
	} else {
		meta_len = ((uint64_t)io.nblocks + 1) * ns->head->ms;
		metadata = io.metadata;
	}

	if (ns->head->features & NVME_NS_EXT_LBAS) {
		length += meta_len;
		meta_len = 0;
	} else if (meta_len) {
		if ((io.metadata & 3) || !io.metadata)
			return -EINVAL;
	}
	if (length > 0xffffffffULL || meta_len > 0xffffffffULL)
		return -EINVAL;

	mm_memset(&c, 0, sizeof(c));
	c.rw.opcode = io.opcode;
	c.rw.flags = io.flags;
	c.rw.nsid = cpu_to_le32(ns->head->ns_id);
	c.rw.slba = cpu_to_le64(io.slba);
	c.rw.length = cpu_to_le16(io.nblocks);
	c.rw.control = cpu_to_le16(io.control);
	c.rw.dsmgmt = cpu_to_le32(io.dsmgmt);
	c.rw.reftag = cpu_to_le32(io.reftag);
	c.rw.lbat = cpu_to_le16(io.apptag);
	c.rw.lbatm = cpu_to_le16(io.appmask);

	/* The buffers are sized from the block count; an unprivileged
	 * caller still has to provide them (no device DMA to address 0). */
	return nvme_submit_user_cmd(ns->ctrl, ns->queue, ns, &c, io.addr,
				    (uint32_t)length, metadata,
				    (uint32_t)meta_len, NULL, 0, 0,
				    capable() != 0);
}

static bool nvme_validate_passthru_nsid(struct nvme_ctrl *ctrl,
					struct nvme_ns *ns, uint32_t nsid,
					struct task *cur)
{
	if (ns && nsid != ns->head->ns_id) {
		if (nvme_ioctl_ratelimit())
			kprintf(NVME_LOG_PREFIX
				"%s: %s: nsid (%u) in cmd does not match nsid "
				"(%u) of namespace\n",
				ctrl->name, nvme_cur_comm(cur), (unsigned)nsid,
				(unsigned)ns->head->ns_id);
		return false;
	}
	return true;
}

/* Build the SQE of a pass-through command from the user's fields. */
static void nvme_passthru_to_cmd(struct nvme_command *c, uint8_t opcode,
				 uint8_t flags, uint32_t nsid, uint32_t cdw2,
				 uint32_t cdw3, const uint32_t cdw10_15[6])
{
	mm_memset(c, 0, sizeof(*c));
	c->common.opcode = opcode;
	c->common.flags = flags;
	c->common.nsid = cpu_to_le32(nsid);
	c->common.cdw2[0] = cpu_to_le32(cdw2);
	c->common.cdw2[1] = cpu_to_le32(cdw3);
	c->common.cdw10 = cpu_to_le32(cdw10_15[0]);
	c->common.cdw11 = cpu_to_le32(cdw10_15[1]);
	c->common.cdw12 = cpu_to_le32(cdw10_15[2]);
	c->common.cdw13 = cpu_to_le32(cdw10_15[3]);
	c->common.cdw14 = cpu_to_le32(cdw10_15[4]);
	c->common.cdw15 = cpu_to_le32(cdw10_15[5]);
}

/* NVME_IOCTL_ADMIN_CMD / NVME_IOCTL_IO_CMD (32-bit result). */
static int nvme_user_cmd(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			 struct nvme_passthru_cmd *ucmd, unsigned int flags,
			 bool open_for_write, struct task *cur)
{
	struct nvme_passthru_cmd cmd;
	struct nvme_command c;
	uint64_t result = 0;
	uint32_t cdws[6];
	int status;

	if (nvme_copy_in(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid, cur))
		return -EINVAL;

	cdws[0] = cmd.cdw10;
	cdws[1] = cmd.cdw11;
	cdws[2] = cmd.cdw12;
	cdws[3] = cmd.cdw13;
	cdws[4] = cmd.cdw14;
	cdws[5] = cmd.cdw15;
	nvme_passthru_to_cmd(&c, cmd.opcode, cmd.flags, cmd.nsid, cmd.cdw2,
			     cmd.cdw3, cdws);

	if (!nvme_cmd_allowed(ns, &c, flags, open_for_write))
		return -EACCES;

	status = nvme_submit_user_cmd(ctrl, ns ? ns->queue : ctrl->admin_q, ns,
				      &c, cmd.addr, cmd.data_len, cmd.metadata,
				      cmd.metadata_len, &result, cmd.timeout_ms,
				      flags & ~NVME_IOCTL_VEC, capable() != 0);

	if (status >= 0) {
		uint32_t r32 = (uint32_t)result;

		if (nvme_copy_out(&ucmd->result, &r32, sizeof(r32)))
			return -EFAULT;
	}
	return status;
}

/* NVME_IOCTL_ADMIN64_CMD / IO64_CMD / IO64_CMD_VEC (64-bit result). */
static int nvme_user_cmd64(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			   struct nvme_passthru_cmd64 *ucmd, unsigned int flags,
			   bool open_for_write, struct task *cur)
{
	struct nvme_passthru_cmd64 cmd;
	struct nvme_command c;
	uint32_t cdws[6];
	int status;

	if (nvme_copy_in(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid, cur))
		return -EINVAL;

	cdws[0] = cmd.cdw10;
	cdws[1] = cmd.cdw11;
	cdws[2] = cmd.cdw12;
	cdws[3] = cmd.cdw13;
	cdws[4] = cmd.cdw14;
	cdws[5] = cmd.cdw15;
	nvme_passthru_to_cmd(&c, cmd.opcode, cmd.flags, cmd.nsid, cmd.cdw2,
			     cmd.cdw3, cdws);

	if (!nvme_cmd_allowed(ns, &c, flags, open_for_write))
		return -EACCES;

	/* data_len is the iovec count of the vectored form (union). */
	status = nvme_submit_user_cmd(ctrl, ns ? ns->queue : ctrl->admin_q, ns,
				      &c, cmd.addr, cmd.data_len, cmd.metadata,
				      cmd.metadata_len, &cmd.result,
				      cmd.timeout_ms, flags, capable() != 0);

	if (status >= 0) {
		if (nvme_copy_out(&ucmd->result, &cmd.result,
				 sizeof(cmd.result)))
			return -EFAULT;
	}
	return status;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static bool is_ctrl_ioctl(unsigned long cmd)
{
	/* (No self-encrypting drive ioctls in LikeOS.) */
	return cmd == NVME_IOCTL_ADMIN_CMD || cmd == NVME_IOCTL_ADMIN64_CMD;
}

static long nvme_ctrl_ioctl(struct nvme_ctrl *ctrl, unsigned long cmd,
			    void *argp, bool open_for_write, struct task *cur)
{
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp, 0, open_for_write, cur);
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, 0, open_for_write, cur);
	default:
		return -ENOTTY;
	}
}

static long nvme_ns_ioctl_flags(struct nvme_ns *ns, unsigned long cmd,
				void *argp, unsigned int flags,
				bool open_for_write, struct task *cur)
{
	switch (cmd) {
	case NVME_IOCTL_ID:
		return (long)ns->head->ns_id;
	case NVME_IOCTL_IO_CMD:
		return nvme_user_cmd(ns->ctrl, ns, argp, flags, open_for_write,
				     cur);
	case NVME_IOCTL_SUBMIT_IO:
		return nvme_submit_io(ns, argp);
	case NVME_IOCTL_IO64_CMD_VEC:
		flags |= NVME_IOCTL_VEC;
		/* fall through */
	case NVME_IOCTL_IO64_CMD:
		return nvme_user_cmd64(ns->ctrl, ns, argp, flags,
				       open_for_write, cur);
	default:
		return -ENOTTY;
	}
}

long nvme_ns_ioctl(struct nvme_ns *ns, unsigned long cmd, void *argp,
		   struct task *cur, int open_flags)
{
	bool open_for_write = nvme_open_for_write(open_flags);
	unsigned int flags = 0;

	if (WARN_ON_ONCE(!ns || !ns->ctrl || !ns->head))
		return -ENODEV;
	might_sleep();
	if (open_flags & NVME_IOCTL_OPEN_PARTITION)
		flags |= NVME_IOCTL_PARTITION;
	if (is_ctrl_ioctl(cmd))
		return nvme_ctrl_ioctl(ns->ctrl, cmd, argp, open_for_write, cur);
	return nvme_ns_ioctl_flags(ns, cmd, argp, flags, open_for_write, cur);
}

long nvme_ns_head_ioctl(struct nvme_ns_head *head, unsigned long cmd,
			void *argp, struct task *cur, int open_flags)
{
	bool open_for_write = nvme_open_for_write(open_flags);
	struct nvme_ns *ns;
	unsigned int flags = 0;
	long ret = -EWOULDBLOCK;
	int srcu_idx;

	if (WARN_ON_ONCE(!head))
		return -ENODEV;
	might_sleep();
	if (open_flags & NVME_IOCTL_OPEN_PARTITION)
		flags |= NVME_IOCTL_PARTITION;

	/* The current path stays valid inside the head's read section. */
	srcu_idx = nvme_srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (!ns)
		goto out_unlock;

	/* Controller ioctls hold the controller and leave the read section
	 * first: deleting a namespace through pass-through waits for it. */
	if (is_ctrl_ioctl(cmd)) {
		struct nvme_ctrl *ctrl = ns->ctrl;

		nvme_get_ctrl(ctrl);
		nvme_srcu_read_unlock(&head->srcu, srcu_idx);
		ret = nvme_ctrl_ioctl(ctrl, cmd, argp, open_for_write, cur);
		nvme_put_ctrl(ctrl);
		return ret;
	}
	ret = nvme_ns_ioctl_flags(ns, cmd, argp, flags, open_for_write, cur);
out_unlock:
	nvme_srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

/* NVME_IOCTL_IO_CMD on the controller node: only when it has exactly one
 * namespace (deprecated). */
static long nvme_dev_user_cmd(struct nvme_ctrl *ctrl, void *argp,
			      bool open_for_write, struct task *cur)
{
	struct nvme_ns *ns;
	long ret;

	mutex_lock(&ctrl->namespaces_lock);
	if (list_empty(&ctrl->namespaces)) {
		mutex_unlock(&ctrl->namespaces_lock);
		return -ENOTTY;
	}
	ns = list_first_entry(&ctrl->namespaces, struct nvme_ns, list);
	if (ns != list_last_entry(&ctrl->namespaces, struct nvme_ns, list)) {
		mutex_unlock(&ctrl->namespaces_lock);
		if (nvme_ioctl_ratelimit())
			kprintf(NVME_LOG_PREFIX
				"%s: NVME_IOCTL_IO_CMD not supported when "
				"multiple namespaces present!\n",
				ctrl->name);
		return -EINVAL;
	}
	if (nvme_ioctl_ratelimit())
		kprintf(NVME_LOG_PREFIX "%s: using deprecated NVME_IOCTL_IO_CMD "
					"ioctl on the char device!\n",
			ctrl->name);
	if (!nvme_get_ns(ns)) {
		mutex_unlock(&ctrl->namespaces_lock);
		return -ENXIO;
	}
	mutex_unlock(&ctrl->namespaces_lock);

	ret = nvme_user_cmd(ctrl, ns, argp, 0, open_for_write, cur);
	nvme_put_ns(ns);
	return ret;
}

long nvme_dev_ioctl(struct nvme_ctrl *ctrl, unsigned long cmd, void *argp,
		    struct task *cur, int open_flags)
{
	bool open_for_write = nvme_open_for_write(open_flags);

	if (WARN_ON_ONCE(!ctrl))
		return -ENODEV;
	might_sleep();
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp, 0, open_for_write, cur);
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, 0, open_for_write, cur);
	case NVME_IOCTL_IO_CMD:
		return nvme_dev_user_cmd(ctrl, argp, open_for_write, cur);
	case NVME_IOCTL_RESET:
		if (!capable())
			return -EACCES;
		kprintf(NVME_LOG_PREFIX "%s: resetting controller\n",
			ctrl->name);
		return nvme_reset_ctrl_sync(ctrl);
	case NVME_IOCTL_SUBSYS_RESET:
		if (!capable())
			return -EACCES;
		return nvme_reset_subsystem(ctrl);
	case NVME_IOCTL_RESCAN:
		if (!capable())
			return -EACCES;
		nvme_queue_scan(ctrl);
		return 0;
	default:
		return -ENOTTY;
	}
}
