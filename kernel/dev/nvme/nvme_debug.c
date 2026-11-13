// LikeOS - NVMe debug dumps and the command trace ring
//
// What somebody debugging an NVMe problem needs to see without a debugger:
//
//   * the COMMAND TRACE: per controller the last NVME_TRACE_SHARDS x
//     NVME_TRACE_PER_SHARD submitted and completed commands -- queue,
//     command ID, opcode with its fields decoded the established way
//     (identify cns/ctrlid, read/write slba/len/control/dsmgmt/reftag,
//     features fid/sel, zone and reservation actions, ...), completion
//     status with its name, result dword, SQ head and the latency between
//     submission and completion.  Recorded from any context by
//     nvme_trace_submit()/nvme_trace_complete() (the transport calls them
//     around the SQ write and for each CQE) into lock-free per-shard rings
//     (shard = queue ID modulo the shard count, so CPUs submitting to
//     their own queues never share a cache line), merged by time when read;
//   * REGISTERS: CAP, VS, CC, CSTS, AQA/ASQ/ACQ, CMB/BP/PMR, CRTO with the
//     fields decoded;
//   * QUEUES (PCIe): depth, SQ tail/last doorbell, CQ head/phase, vector,
//     flags, counters, whether a completion is waiting at the head;
//   * IN-FLIGHT commands: every started request of the admin and I/O tag
//     sets with its CID, opcode, namespace, state and time to its deadline;
//   * admin feature summaries (firmware slots, self-test log, telemetry
//     headers, persistent event log, commands supported and effects).
//
// The tree (dynamic: an entry exists exactly while its object does, and
// every read resolves the object by NAME, taking a reference, so a file
// left open across a removal answers -ENODEV instead of touching freed
// memory):
//   /sys/kernel/debug/nvme/<ctrl>/{info,regs,queues,inflight,cmd_log,
//        trace_enable,fw_slots,self_test,telemetry_host,telemetry_ctrl,
//        persistent_event,effects,fault_inject/}
//   /sys/kernel/debug/nvme/<ns>/fault_inject/
//   /sys/kernel/debug/<ctrl or ns>/fault_inject/   (the established spot)
// fault_inject/ holds probability interval times space verbose status
// dont_retry kind count injected (nvme_fault_inject.c).
//
// Lifetime: the per-controller debug state (struct nvme_debug_ctrl, the
// trace rings) is never freed -- nvme_debug_ctrl_exit() detaches it and
// the next controller reuses it -- so a trace hook racing with a removal
// writes into valid memory at worst.  Registry: g_dbg_lock (leaf spinlock).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/sysfs.h>
#include <kernel/ke/cred.h>
#include <kernel/uapi/dirent.h>

/* ==========================================================================
 * Trace ring layout
 * ========================================================================== */

#define NVME_TRACE_SHARDS 16u
#define NVME_TRACE_PER_SHARD 64u /* power of two */

enum { NVME_TR_SUBMIT = 1, NVME_TR_COMPLETE = 2 };

struct nvme_trace_ent {
	volatile uint64_t seq; /* 0: empty / being written; else index + 1 */
	uint64_t ts_ns;
	uint16_t qid;
	uint16_t cid;
	uint8_t type; /* NVME_TR_* */
	uint8_t opcode; /* submit */
	uint8_t flags; /* submit: command flags byte */
	uint8_t rsvd;
	uint32_t nsid; /* submit */
	union {
		uint8_t cdw10[24]; /* submit: dwords 10..15, wire order */
		struct {
			uint64_t result;
			uint16_t status; /* CQE status >> 1 */
			uint16_t sq_head;
			uint16_t sq_id;
		} cmp;
	};
	uint8_t pad[4];
};

struct nvme_trace_shard {
	volatile uint64_t head; /* next index */
	uint8_t pad[56];
	struct nvme_trace_ent ent[NVME_TRACE_PER_SHARD];
} __attribute__((aligned(64)));

struct nvme_debug_ctrl {
	struct list_head link; /* g_dbg_list */
	unsigned int key; /* stable listing key */
	int in_use; /* attached to a controller */
	struct nvme_ctrl *ctrl; /* NULL while detached */
	char name[NVME_NAME_LEN];
	volatile int trace_on;
	struct nvme_trace_shard shard[NVME_TRACE_SHARDS];
};

volatile int nvme_trace_enabled = 1;

static LIST_HEAD(g_dbg_list);
static spinlock_t g_dbg_lock = SPINLOCK_INIT("nvme_debug");
static unsigned int g_dbg_next_key = 1;

static inline void _nvme_debug_check(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_trace_ent) != 64);
	BUILD_BUG_ON((NVME_TRACE_PER_SHARD & (NVME_TRACE_PER_SHARD - 1)) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct nvme_common_command, cdw10) +
			     24 !=
		     64);
}

/* ==========================================================================
 * Output sink
 * ========================================================================== */

struct nvme_sbuf {
	char *buf;
	long cap;
	long pos; /* may run past cap: the length needed */
};

static void sbp(struct nvme_sbuf *sb, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void sbp(struct nvme_sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	long avail = sb->cap - sb->pos;
	int n;

	__builtin_va_start(ap, fmt);
	if (avail > 1) {
		n = kvsnprintf(sb->buf + sb->pos, (size_t)avail, fmt, ap);
	} else {
		char dummy[1];

		n = kvsnprintf(dummy, 1, fmt, ap);
	}
	__builtin_va_end(ap);
	if (n > 0)
		sb->pos += n;
}

static const char *str_or(const char *s, const char *dflt)
{
	return s ? s : dflt;
}

/* ==========================================================================
 * Command decoding (the established trace formats)
 * ========================================================================== */

static const char *const zsa_strs[] = {
	[0x01] = "close zone",	 [0x02] = "finish zone",
	[0x03] = "open zone",	 [0x04] = "reset zone",
	[0x05] = "offline zone", [0x10] = "set zone descriptor extension",
};
static const char *const zrasf_strs[] = {
	[0x00] = "list all zones",
	[0x01] = "list the zones in the ZSE: Empty state",
	[0x02] = "list the zones in the ZSIO: Implicitly Opened state",
	[0x03] = "list the zones in the ZSEO: Explicitly Opened state",
	[0x04] = "list the zones in the ZSC: Closed state",
	[0x05] = "list the zones in the ZSF: Full state",
	[0x06] = "list the zones in the ZSRO: Read Only state",
	[0x07] = "list the zones in the ZSO: Offline state",
	[0x09] = "list the zones that have the zone attribute",
};
static const char *const rrega_strs[] = {
	[0x00] = "register", [0x01] = "unregister", [0x02] = "replace",
};
static const char *const rtype_strs[] = {
	[0x00] = "reserved",
	[0x01] = "write exclusive",
	[0x02] = "exclusive access",
	[0x03] = "write exclusive registrants only",
	[0x04] = "exclusive access registrants only",
	[0x05] = "write exclusive all registrants",
	[0x06] = "exclusive access all registrants",
};
static const char *const racqa_strs[] = {
	[0x00] = "acquire", [0x01] = "preempt", [0x02] = "preempt and abort",
};
static const char *const rrela_strs[] = {
	[0x00] = "release", [0x01] = "clear",
};

#define TBL_STR(tbl, i)                                                     \
	(((unsigned)(i) < sizeof(tbl) / sizeof((tbl)[0]) && (tbl)[(i)]) ?   \
		 (tbl)[(i)] :                                               \
		 "reserved")

/* Decode dwords 10..15 (`c', wire order) of an admin (qid 0) or I/O
 * command into `out'. */
static void nvme_trace_decode(char *out, size_t cap, uint16_t qid,
			      uint8_t opcode, const uint8_t *c)
{
	if (qid == 0) {
		switch (opcode) {
		case nvme_admin_delete_sq:
			ksnprintf(out, cap, "sqid=%u",
				  (unsigned)get_unaligned_le16(c));
			return;
		case nvme_admin_create_sq:
			ksnprintf(out, cap,
				  "sqid=%u, qsize=%u, sq_flags=0x%x, cqid=%u",
				  (unsigned)get_unaligned_le16(c),
				  (unsigned)get_unaligned_le16(c + 2),
				  (unsigned)get_unaligned_le16(c + 4),
				  (unsigned)get_unaligned_le16(c + 6));
			return;
		case nvme_admin_delete_cq:
			ksnprintf(out, cap, "cqid=%u",
				  (unsigned)get_unaligned_le16(c));
			return;
		case nvme_admin_create_cq:
			ksnprintf(out, cap,
				  "cqid=%u, qsize=%u, cq_flags=0x%x, "
				  "irq_vector=%u",
				  (unsigned)get_unaligned_le16(c),
				  (unsigned)get_unaligned_le16(c + 2),
				  (unsigned)get_unaligned_le16(c + 4),
				  (unsigned)get_unaligned_le16(c + 6));
			return;
		case nvme_admin_identify:
			ksnprintf(out, cap, "cns=%u, ctrlid=%u", (unsigned)c[0],
				  (unsigned)get_unaligned_le16(c + 2));
			return;
		case nvme_admin_set_features:
			ksnprintf(out, cap, "fid=0x%x, sv=0x%x, cdw11=0x%x",
				  (unsigned)c[0], (unsigned)(c[3] & 0x8),
				  (unsigned)get_unaligned_le32(c + 4));
			return;
		case nvme_admin_get_features:
			ksnprintf(out, cap, "fid=0x%x, sel=0x%x, cdw11=0x%x",
				  (unsigned)c[0], (unsigned)(c[1] & 0x7),
				  (unsigned)get_unaligned_le32(c + 4));
			return;
		case nvme_admin_get_lba_status:
			ksnprintf(out, cap,
				  "slba=0x%llx, mndw=0x%x, rl=0x%x, atype=%u",
				  (unsigned long long)get_unaligned_le64(c),
				  (unsigned)get_unaligned_le32(c + 8),
				  (unsigned)get_unaligned_le16(c + 12),
				  (unsigned)c[15]);
			return;
		case nvme_admin_format_nvm:
			ksnprintf(out, cap,
				  "lbaf=%u, mset=%u, pi=%u, pil=%u, ses=%u",
				  (unsigned)((c[1] & 0x30) | (c[0] & 0xf)),
				  (unsigned)((c[0] >> 4) & 0x1),
				  (unsigned)((c[0] >> 5) & 0x7),
				  (unsigned)(c[1] & 0x1),
				  (unsigned)((c[1] >> 1) & 0x7));
			return;
		case nvme_admin_get_log_page:
			ksnprintf(out, cap,
				  "lid=0x%x, lsp=0x%x, rae=%u, numd=%u, lsi=%u, "
				  "lpo=%llu",
				  (unsigned)c[0], (unsigned)(c[1] & 0x7f),
				  (unsigned)(c[1] >> 7),
				  (unsigned)(get_unaligned_le16(c + 2) |
					     ((uint32_t)get_unaligned_le16(c +
									   4)
					      << 16)),
				  (unsigned)get_unaligned_le16(c + 6),
				  (unsigned long long)get_unaligned_le64(c + 8));
			return;
		case nvme_admin_abort_cmd:
			ksnprintf(out, cap, "sqid=%u, cid=%u",
				  (unsigned)get_unaligned_le16(c),
				  (unsigned)get_unaligned_le16(c + 2));
			return;
		default:
			break;
		}
	} else {
		switch (opcode) {
		case nvme_cmd_read:
		case nvme_cmd_write:
		case nvme_cmd_write_zeroes:
		case nvme_cmd_zone_append:
		case nvme_cmd_compare:
		case nvme_cmd_verify:
		case nvme_cmd_write_uncor:
			ksnprintf(out, cap,
				  "slba=%llu, len=%u, ctrl=0x%x, dsmgmt=%u, "
				  "reftag=%u",
				  (unsigned long long)get_unaligned_le64(c),
				  (unsigned)get_unaligned_le16(c + 8),
				  (unsigned)get_unaligned_le16(c + 10),
				  (unsigned)get_unaligned_le32(c + 12),
				  (unsigned)get_unaligned_le32(c + 16));
			return;
		case nvme_cmd_dsm:
			ksnprintf(out, cap, "nr=%u, attributes=%u",
				  (unsigned)get_unaligned_le32(c),
				  (unsigned)get_unaligned_le32(c + 4));
			return;
		case nvme_cmd_zone_mgmt_send:
			ksnprintf(out, cap, "slba=%llu, zsa=%u:%s, all=%u",
				  (unsigned long long)get_unaligned_le64(c),
				  (unsigned)c[12], TBL_STR(zsa_strs, c[12]),
				  (unsigned)c[13]);
			return;
		case nvme_cmd_zone_mgmt_recv:
			ksnprintf(out, cap,
				  "slba=%llu, numd=%u, zra=%u, zrasf=%u:%s, "
				  "pr=%u",
				  (unsigned long long)get_unaligned_le64(c),
				  (unsigned)get_unaligned_le32(c + 8),
				  (unsigned)c[12], (unsigned)c[13],
				  TBL_STR(zrasf_strs, c[13]), (unsigned)c[14]);
			return;
		case nvme_cmd_resv_register:
			ksnprintf(out, cap, "rrega=%u:%s, iekey=%u, ptpl=%u",
				  (unsigned)(c[0] & 0x7),
				  TBL_STR(rrega_strs, c[0] & 0x7),
				  (unsigned)((c[0] >> 3) & 0x1),
				  (unsigned)((c[3] >> 6) & 0x3));
			return;
		case nvme_cmd_resv_acquire:
			ksnprintf(out, cap, "racqa=%u:%s, iekey=%u, rtype=%u:%s",
				  (unsigned)(c[0] & 0x7),
				  TBL_STR(racqa_strs, c[0] & 0x7),
				  (unsigned)((c[0] >> 3) & 0x1),
				  (unsigned)c[1], TBL_STR(rtype_strs, c[1]));
			return;
		case nvme_cmd_resv_release:
			ksnprintf(out, cap, "rrela=%u:%s, iekey=%u, rtype=%u:%s",
				  (unsigned)(c[0] & 0x7),
				  TBL_STR(rrela_strs, c[0] & 0x7),
				  (unsigned)((c[0] >> 3) & 0x1),
				  (unsigned)c[1], TBL_STR(rtype_strs, c[1]));
			return;
		case nvme_cmd_resv_report:
			ksnprintf(out, cap, "numd=%u, eds=%u",
				  (unsigned)get_unaligned_le32(c),
				  (unsigned)(c[4] & 0x1));
			return;
		default:
			break;
		}
	}
	/* The common form: the raw bytes of dwords 10..15. */
	{
		size_t pos = 0;
		int n = ksnprintf(out, cap, "cdw10=");

		if (n > 0)
			pos = (size_t)n;
		for (int i = 0; i < 24 && pos + 3 < cap; i++) {
			n = ksnprintf(out + pos, cap - pos, "%s%02x",
				      i ? " " : "", (unsigned)c[i]);
			if (n <= 0)
				break;
			pos += (size_t)n;
		}
	}
}

static const char *nvme_trace_opcode_name(uint16_t qid, uint8_t opcode)
{
	return qid ? str_or(nvme_get_opcode_str(opcode), "unknown") :
		     str_or(nvme_get_admin_opcode_str(opcode), "unknown");
}

/* ==========================================================================
 * Trace hooks (any context)
 * ========================================================================== */

static struct nvme_trace_ent *nvme_trace_slot(struct nvme_debug_ctrl *d,
					      uint16_t qid, uint64_t *idx)
{
	struct nvme_trace_shard *sh = &d->shard[qid % NVME_TRACE_SHARDS];
	struct nvme_trace_ent *e;

	*idx = __atomic_fetch_add(&sh->head, 1, __ATOMIC_RELAXED);
	e = &sh->ent[*idx & (NVME_TRACE_PER_SHARD - 1)];
	__atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	return e;
}

static struct nvme_debug_ctrl *nvme_trace_target(struct nvme_ctrl *ctrl)
{
	struct nvme_debug_ctrl *d;

	if (!__atomic_load_n(&nvme_trace_enabled, __ATOMIC_RELAXED) || !ctrl)
		return NULL;
	d = (struct nvme_debug_ctrl *)__atomic_load_n(&ctrl->debug,
						     __ATOMIC_ACQUIRE);
	if (!d || !__atomic_load_n(&d->trace_on, __ATOMIC_RELAXED))
		return NULL;
	return d;
}

void nvme_trace_submit(struct nvme_ctrl *ctrl, uint16_t qid,
		       const struct nvme_command *cmd)
{
	struct nvme_debug_ctrl *d = nvme_trace_target(ctrl);
	struct nvme_trace_ent *e;
	uint64_t idx;

	if (!d || !cmd)
		return;
	e = nvme_trace_slot(d, qid, &idx);
	e->ts_ns = hrtimer_now_ns();
	e->qid = qid;
	e->cid = cmd->common.command_id;
	e->type = NVME_TR_SUBMIT;
	e->opcode = cmd->common.opcode;
	e->flags = cmd->common.flags;
	e->nsid = le32_to_cpu(cmd->common.nsid);
	mm_memcpy(e->cdw10, (const uint8_t *)cmd +
				    __builtin_offsetof(struct nvme_common_command,
						       cdw10),
		  sizeof(e->cdw10));
	__atomic_store_n(&e->seq, idx + 1, __ATOMIC_RELEASE);
}

void nvme_trace_complete(struct nvme_ctrl *ctrl, uint16_t qid,
			 const struct nvme_completion *cqe)
{
	struct nvme_debug_ctrl *d = nvme_trace_target(ctrl);
	struct nvme_trace_ent *e;
	uint64_t idx;

	if (!d || !cqe)
		return;
	e = nvme_trace_slot(d, qid, &idx);
	e->ts_ns = hrtimer_now_ns();
	e->qid = qid;
	e->cid = cqe->command_id;
	e->type = NVME_TR_COMPLETE;
	e->opcode = 0;
	e->flags = 0;
	e->nsid = 0;
	e->cmp.result = le64_to_cpu(cqe->result.u64);
	e->cmp.status = NVME_CQE_STATUS(le16_to_cpu(cqe->status));
	e->cmp.sq_head = le16_to_cpu(cqe->sq_head);
	e->cmp.sq_id = le16_to_cpu(cqe->sq_id);
	__atomic_store_n(&e->seq, idx + 1, __ATOMIC_RELEASE);
}

/* Copy every complete entry of `d' (consistent snapshot per entry) into
 * `out' merged by time; returns the count. */
static unsigned int nvme_trace_snapshot(struct nvme_debug_ctrl *d,
					struct nvme_trace_ent *out,
					unsigned int max)
{
	unsigned int per[NVME_TRACE_SHARDS], pos[NVME_TRACE_SHARDS];
	struct nvme_trace_ent *lists;
	unsigned int n = 0;

	lists = kalloc(sizeof(struct nvme_trace_ent) * NVME_TRACE_SHARDS *
		       NVME_TRACE_PER_SHARD);
	if (!lists)
		return 0;
	for (unsigned int s = 0; s < NVME_TRACE_SHARDS; s++) {
		struct nvme_trace_shard *sh = &d->shard[s];
		struct nvme_trace_ent *dst = lists + s * NVME_TRACE_PER_SHARD;
		uint64_t head = __atomic_load_n(&sh->head, __ATOMIC_ACQUIRE);
		uint64_t first = head > NVME_TRACE_PER_SHARD ?
					 head - NVME_TRACE_PER_SHARD :
					 0;

		per[s] = 0;
		pos[s] = 0;
		for (uint64_t i = first; i < head; i++) {
			struct nvme_trace_ent *e =
				&sh->ent[i & (NVME_TRACE_PER_SHARD - 1)];
			uint64_t s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
			struct nvme_trace_ent tmp;

			if (s1 != i + 1)
				continue; /* empty, being written or newer */
			mm_memcpy(&tmp, (const void *)e, sizeof(tmp));
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if (__atomic_load_n(&e->seq, __ATOMIC_RELAXED) != s1)
				continue; /* overwritten while copied */
			dst[per[s]++] = tmp;
		}
	}
	/* Merge the per-shard (already chronological) lists. */
	while (n < max) {
		int best = -1;

		for (unsigned int s = 0; s < NVME_TRACE_SHARDS; s++) {
			struct nvme_trace_ent *cand;

			if (pos[s] >= per[s])
				continue;
			cand = lists + s * NVME_TRACE_PER_SHARD + pos[s];
			if (best < 0 ||
			    cand->ts_ns < lists[(unsigned)best *
							NVME_TRACE_PER_SHARD +
						pos[best]]
						  .ts_ns)
				best = (int)s;
		}
		if (best < 0)
			break;
		out[n++] = lists[(unsigned)best * NVME_TRACE_PER_SHARD +
				 pos[best]];
		pos[best]++;
	}
	kfree(lists);
	return n;
}

static void nvme_trace_format(struct nvme_debug_ctrl *d, struct nvme_sbuf *sb)
{
	unsigned int max = NVME_TRACE_SHARDS * NVME_TRACE_PER_SHARD;
	struct nvme_trace_ent *v = kalloc(sizeof(*v) * max);
	unsigned int n;
	uint64_t t0;

	if (!v) {
		sbp(sb, "no memory\n");
		return;
	}
	n = nvme_trace_snapshot(d, v, max);
	sbp(sb, "# %u entries (newest last); time in us relative to the "
		"first\n",
	    n);
	t0 = n ? v[0].ts_ns : 0;
	for (unsigned int i = 0; i < n; i++) {
		struct nvme_trace_ent *e = &v[i];
		uint64_t rel = (e->ts_ns - t0) / 1000;

		if (e->type == NVME_TR_SUBMIT) {
			char dec[160];

			nvme_trace_decode(dec, sizeof(dec), e->qid, e->opcode,
					  e->cdw10);
			sbp(sb,
			    "%10llu %s: qid=%u, cmdid=%u, nsid=%u, flags=0x%x, "
			    "cmd=(%s %s)\n",
			    (unsigned long long)rel, d->name, (unsigned)e->qid,
			    (unsigned)e->cid, (unsigned)e->nsid,
			    (unsigned)e->flags,
			    nvme_trace_opcode_name(e->qid, e->opcode), dec);
		} else {
			long long lat = -1;
			const char *op = "?";

			/* The submission this completion belongs to. */
			for (unsigned int j = i; j-- > 0;) {
				if (v[j].type == NVME_TR_SUBMIT &&
				    v[j].qid == e->qid && v[j].cid == e->cid) {
					lat = (long long)((e->ts_ns -
							   v[j].ts_ns) /
							  1000);
					op = nvme_trace_opcode_name(v[j].qid,
								    v[j].opcode);
					break;
				}
			}
			sbp(sb,
			    "%10llu %s: qid=%u, cmdid=%u, res=%#llx, sqhd=%u, "
			    "status=%#x (%s)",
			    (unsigned long long)rel, d->name, (unsigned)e->qid,
			    (unsigned)e->cid,
			    (unsigned long long)e->cmp.result,
			    (unsigned)e->cmp.sq_head, (unsigned)e->cmp.status,
			    str_or(nvme_get_error_status_str(e->cmp.status),
				   e->cmp.status ? "error" : "success"));
			if (lat >= 0)
				sbp(sb, " [%s, %lld us]\n", op, lat);
			else
				sbp(sb, "\n");
		}
	}
	kfree(v);
}

/* ==========================================================================
 * Attach / detach
 * ========================================================================== */

int nvme_debug_ctrl_init(struct nvme_ctrl *ctrl)
{
	struct nvme_debug_ctrl *d = NULL, *it;
	uint64_t fl;

	if (WARN_ON(!ctrl))
		return -EINVAL;
	if (ctrl->debug)
		return 0;
	/* Reuse a detached one. */
	spin_lock_irqsave(&g_dbg_lock, &fl);
	list_for_each_entry(it, &g_dbg_list, link) {
		if (!it->in_use) {
			it->in_use = 1;
			d = it;
			break;
		}
	}
	spin_unlock_irqrestore(&g_dbg_lock, fl);
	if (!d) {
		d = kalloc(sizeof(*d));
		if (!d)
			return -ENOMEM;
		mm_memset(d, 0, sizeof(*d));
		d->in_use = 1;
		spin_lock_irqsave(&g_dbg_lock, &fl);
		d->key = g_dbg_next_key++;
		list_add_tail(&d->link, &g_dbg_list);
		spin_unlock_irqrestore(&g_dbg_lock, fl);
	}
	for (unsigned int s = 0; s < NVME_TRACE_SHARDS; s++) {
		d->shard[s].head = 0;
		mm_memset((void *)d->shard[s].ent, 0, sizeof(d->shard[s].ent));
	}
	spin_lock_irqsave(&g_dbg_lock, &fl);
	ksnprintf(d->name, sizeof(d->name), "%s", ctrl->name);
	d->trace_on = 1;
	d->ctrl = ctrl;
	spin_unlock_irqrestore(&g_dbg_lock, fl);
	__atomic_store_n(&ctrl->debug, (void *)d, __ATOMIC_RELEASE);
	nvme_debugfs_init_once();
	return 0;
}

void nvme_debug_ctrl_exit(struct nvme_ctrl *ctrl)
{
	struct nvme_debug_ctrl *d;
	uint64_t fl;

	if (!ctrl)
		return;
	d = (struct nvme_debug_ctrl *)ctrl->debug;
	if (!d)
		return;
	__atomic_store_n(&ctrl->debug, NULL, __ATOMIC_RELEASE);
	spin_lock_irqsave(&g_dbg_lock, &fl);
	WARN_ON(d->ctrl != ctrl);
	d->ctrl = NULL;
	d->trace_on = 0;
	d->in_use = 0;
	spin_unlock_irqrestore(&g_dbg_lock, fl);
}

/* The controller called `name' with a reference (nvme_put_ctrl), or NULL. */
static struct nvme_ctrl *nvme_debug_get_ctrl(const char *name,
					     struct nvme_debug_ctrl **dp)
{
	struct nvme_debug_ctrl *d;
	struct nvme_ctrl *ctrl = NULL;
	uint64_t fl;

	spin_lock_irqsave(&g_dbg_lock, &fl);
	list_for_each_entry(d, &g_dbg_list, link) {
		if (d->in_use && d->ctrl && kstrcmp(d->name, name) == 0) {
			/* Attached means not freed yet: the owner detaches
			 * before its last reference goes. */
			if (refcount_inc_not_zero(&d->ctrl->ref)) {
				ctrl = d->ctrl;
				if (dp)
					*dp = d;
			}
			break;
		}
	}
	spin_unlock_irqrestore(&g_dbg_lock, fl);
	return ctrl;
}

static bool nvme_debug_ctrl_exists(const char *name)
{
	struct nvme_debug_ctrl *d;
	uint64_t fl;
	bool r = false;

	spin_lock_irqsave(&g_dbg_lock, &fl);
	list_for_each_entry(d, &g_dbg_list, link) {
		if (d->in_use && d->ctrl && kstrcmp(d->name, name) == 0) {
			r = true;
			break;
		}
	}
	spin_unlock_irqrestore(&g_dbg_lock, fl);
	return r;
}

/* ==========================================================================
 * Dumps
 * ========================================================================== */

static bool nvme_debug_state_dumpable(struct nvme_ctrl *ctrl)
{
	switch (nvme_ctrl_state(ctrl)) {
	case NVME_CTRL_DELETING:
	case NVME_CTRL_DELETING_NOIO:
	case NVME_CTRL_DEAD:
		return false;
	default:
		return true;
	}
}

static void dump_info(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb)
{
	sbp(sb, "name            %s\n", ctrl->name);
	sbp(sb, "state           %s\n",
	    str_or(nvme_ctrl_state_name(nvme_ctrl_state(ctrl)), "?"));
	sbp(sb, "transport       %s\n",
	    ctrl->ops && ctrl->ops->name ? ctrl->ops->name : "?");
	sbp(sb, "flags           0x%lx\n", (unsigned long)ctrl->flags);
	sbp(sb, "events          0x%lx\n", (unsigned long)ctrl->events);
	sbp(sb, "quirks          0x%lx", (unsigned long)ctrl->quirks);
	for (unsigned int b = 0; b < 32; b++) {
		const char *q;

		if (!(ctrl->quirks & (1UL << b)))
			continue;
		q = nvme_quirk_name((enum nvme_quirks)(1UL << b));
		sbp(sb, " %s", q ? q : "?");
	}
	sbp(sb, "\n");
	sbp(sb, "cntlid          %u\n", (unsigned)ctrl->cntlid);
	sbp(sb, "cap             0x%016llx\n", (unsigned long long)ctrl->cap);
	sbp(sb, "vs              %u.%u.%u\n", (unsigned)(ctrl->vs >> 16),
	    (unsigned)((ctrl->vs >> 8) & 0xff), (unsigned)(ctrl->vs & 0xff));
	sbp(sb, "ctrl_config     0x%08x\n", (unsigned)nvme_ctrl_config(ctrl));
	sbp(sb, "queue_count     %u\n", (unsigned)ctrl->queue_count);
	sbp(sb, "sqsize          %u\n", (unsigned)ctrl->sqsize);
	sbp(sb, "max_hw_sectors  %u\n", (unsigned)ctrl->max_hw_sectors);
	sbp(sb, "max_segments    %u\n", (unsigned)ctrl->max_segments);
	sbp(sb, "oacs            0x%04x\n", (unsigned)ctrl->oacs);
	sbp(sb, "oncs            0x%04x\n", (unsigned)ctrl->oncs);
	sbp(sb, "lpa             0x%02x\n", (unsigned)ctrl->lpa);
	sbp(sb, "frmw            0x%02x\n", (unsigned)ctrl->frmw);
	sbp(sb, "sgls            0x%08x\n", (unsigned)ctrl->sgls);
	sbp(sb, "vwc             %u\n", (unsigned)ctrl->vwc);
	sbp(sb, "abort_limit     %d\n", (int)ctrl->abort_limit);
	sbp(sb, "kato            %u s\n", (unsigned)ctrl->kato_s);
	sbp(sb, "shutdown_tmo    %u s\n", (unsigned)ctrl->shutdown_timeout_s);
	sbp(sb, "apst            %s\n", ctrl->apst_enabled ? "on" : "off");
	sbp(sb, "npss            %u\n", (unsigned)ctrl->npss);
	sbp(sb, "wctemp/cctemp   %u/%u K\n", (unsigned)ctrl->wctemp,
	    (unsigned)ctrl->cctemp);
	sbp(sb, "crit_warning    0x%02x\n", (unsigned)ctrl->smart_crit_warning);
	sbp(sb, "error_count     %llu\n", (unsigned long long)ctrl->error_count);
	sbp(sb, "aen_result      0x%08x\n", (unsigned)ctrl->aen_result);
	sbp(sb, "crdt            %u %u %u (100 ms)\n", (unsigned)ctrl->crdt[0],
	    (unsigned)ctrl->crdt[1], (unsigned)ctrl->crdt[2]);
	sbp(sb, "refcount        %d\n", refcount_read(&ctrl->ref));
	sbp(sb, "trace           %s\n",
	    nvme_trace_enabled ? "enabled" : "disabled globally");
}

static int dbg_read32(struct nvme_ctrl *ctrl, uint32_t off, uint32_t *v)
{
	if (!ctrl->ops || !ctrl->ops->reg_read32)
		return -ENODEV;
	return ctrl->ops->reg_read32(ctrl, off, v);
}

static int dbg_read64(struct nvme_ctrl *ctrl, uint32_t off, uint64_t *v)
{
	if (!ctrl->ops || !ctrl->ops->reg_read64)
		return -ENODEV;
	return ctrl->ops->reg_read64(ctrl, off, v);
}

static void dump_reg32(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb,
		       const char *name, uint32_t off)
{
	uint32_t v;

	if (dbg_read32(ctrl, off, &v))
		sbp(sb, "%-8s (0x%04x) unreadable\n", name, (unsigned)off);
	else
		sbp(sb, "%-8s (0x%04x) 0x%08x\n", name, (unsigned)off,
		    (unsigned)v);
}

static void dump_reg64(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb,
		       const char *name, uint32_t off)
{
	uint64_t v;

	if (dbg_read64(ctrl, off, &v))
		sbp(sb, "%-8s (0x%04x) unreadable\n", name, (unsigned)off);
	else
		sbp(sb, "%-8s (0x%04x) 0x%016llx\n", name, (unsigned)off,
		    (unsigned long long)v);
}

static void dump_regs(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb)
{
	uint64_t cap;
	uint32_t vs, cc, csts, aqa;

	if (dbg_read64(ctrl, NVME_REG_CAP, &cap)) {
		sbp(sb, "registers unreadable (device gone or not mapped)\n");
		return;
	}
	sbp(sb, "CAP      (0x0000) 0x%016llx\n", (unsigned long long)cap);
	sbp(sb,
	    "  MQES=%u CQR=%u AMS=0x%x TO=%u ms DSTRD=%u NSSRS=%u CSS=0x%02x "
	    "BPS=%u CPS=%u MPSMIN=%u MPSMAX=%u PMRS=%u CMBS=%u NSSS=%u "
	    "CRMS=%u NSSES=%u\n",
	    (unsigned)NVME_CAP_MQES(cap) + 1, (unsigned)NVME_CAP_CQR(cap),
	    (unsigned)NVME_CAP_AMS(cap), (unsigned)NVME_CAP_TIMEOUT(cap) * 500,
	    (unsigned)NVME_CAP_STRIDE(cap), (unsigned)NVME_CAP_NSSRC(cap),
	    (unsigned)NVME_CAP_CSS(cap), (unsigned)NVME_CAP_BPS(cap),
	    (unsigned)NVME_CAP_CPS(cap), 1u << (12 + NVME_CAP_MPSMIN(cap)),
	    1u << (12 + NVME_CAP_MPSMAX(cap)), (unsigned)NVME_CAP_PMRS(cap),
	    (unsigned)NVME_CAP_CMBS(cap), (unsigned)NVME_CAP_NSSS(cap),
	    (unsigned)NVME_CAP_CRMS(cap), (unsigned)NVME_CAP_NSSES(cap));
	if (!dbg_read32(ctrl, NVME_REG_VS, &vs))
		sbp(sb, "VS       (0x0008) 0x%08x (%u.%u.%u)\n", (unsigned)vs,
		    (unsigned)(vs >> 16), (unsigned)((vs >> 8) & 0xff),
		    (unsigned)(vs & 0xff));
	dump_reg32(ctrl, sb, "INTMS", NVME_REG_INTMS);
	dump_reg32(ctrl, sb, "INTMC", NVME_REG_INTMC);
	if (!dbg_read32(ctrl, NVME_REG_CC, &cc)) {
		sbp(sb, "CC       (0x0014) 0x%08x\n", (unsigned)cc);
		sbp(sb,
		    "  EN=%u CSS=%u MPS=%u AMS=%u SHN=%u IOSQES=%u IOCQES=%u "
		    "CRIME=%u\n",
		    (unsigned)(cc & NVME_CC_ENABLE),
		    (unsigned)((cc & NVME_CC_CSS_MASK) >> NVME_CC_CSS_SHIFT),
		    (unsigned)((cc & NVME_CC_MPS_MASK) >> NVME_CC_MPS_SHIFT),
		    (unsigned)((cc & NVME_CC_AMS_MASK) >> NVME_CC_AMS_SHIFT),
		    (unsigned)((cc & NVME_CC_SHN_MASK) >> NVME_CC_SHN_SHIFT),
		    (unsigned)((cc & NVME_CC_IOSQES_MASK) >>
			       NVME_CC_IOSQES_SHIFT),
		    (unsigned)((cc & NVME_CC_IOCQES_MASK) >>
			       NVME_CC_IOCQES_SHIFT),
		    (unsigned)!!(cc & NVME_CC_CRIME));
	}
	if (!dbg_read32(ctrl, NVME_REG_CSTS, &csts)) {
		static const char *const shst[] = { "normal", "occurring",
						    "complete", "reserved" };

		sbp(sb, "CSTS     (0x001c) 0x%08x\n", (unsigned)csts);
		if (csts == 0xffffffffu)
			sbp(sb, "  all ones: the device does not answer\n");
		else
			sbp(sb, "  RDY=%u CFS=%u SHST=%s NSSRO=%u PP=%u ST=%u\n",
			    (unsigned)!!(csts & NVME_CSTS_RDY),
			    (unsigned)!!(csts & NVME_CSTS_CFS),
			    shst[(csts & NVME_CSTS_SHST_MASK) >>
				 NVME_CSTS_SHST_SHIFT],
			    (unsigned)!!(csts & NVME_CSTS_NSSRO),
			    (unsigned)!!(csts & NVME_CSTS_PP),
			    (unsigned)!!(csts & NVME_CSTS_ST));
	}
	if (!dbg_read32(ctrl, NVME_REG_AQA, &aqa))
		sbp(sb, "AQA      (0x0024) 0x%08x ASQS=%u ACQS=%u\n",
		    (unsigned)aqa, (unsigned)(aqa & 0xfff) + 1,
		    (unsigned)((aqa >> 16) & 0xfff) + 1);
	dump_reg64(ctrl, sb, "ASQ", NVME_REG_ASQ);
	dump_reg64(ctrl, sb, "ACQ", NVME_REG_ACQ);
	dump_reg32(ctrl, sb, "CMBLOC", NVME_REG_CMBLOC);
	dump_reg32(ctrl, sb, "CMBSZ", NVME_REG_CMBSZ);
	if (NVME_CAP_BPS(cap)) {
		dump_reg32(ctrl, sb, "BPINFO", NVME_REG_BPINFO);
		dump_reg32(ctrl, sb, "BPRSEL", NVME_REG_BPRSEL);
		dump_reg64(ctrl, sb, "BPMBL", NVME_REG_BPMBL);
	}
	if (NVME_CAP_CMBS(cap)) {
		dump_reg64(ctrl, sb, "CMBMSC", NVME_REG_CMBMSC);
		dump_reg32(ctrl, sb, "CMBSTS", NVME_REG_CMBSTS);
	}
	if (ctrl->vs >= 0x00010400)
		dump_reg32(ctrl, sb, "CRTO", NVME_REG_CRTO);
	if (NVME_CAP_PMRS(cap)) {
		dump_reg32(ctrl, sb, "PMRCAP", NVME_REG_PMRCAP);
		dump_reg32(ctrl, sb, "PMRCTL", NVME_REG_PMRCTL);
		dump_reg32(ctrl, sb, "PMRSTS", NVME_REG_PMRSTS);
	}
}

static void dump_queues(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb)
{
	struct nvme_dev *dev;
	unsigned int nq;

	if (ctrl->ops != &nvme_pci_ctrl_ops) {
		sbp(sb, "queue dump: transport %s not supported\n",
		    ctrl->ops && ctrl->ops->name ? ctrl->ops->name : "?");
		return;
	}
	dev = to_nvme_dev(ctrl);
	sbp(sb,
	    "online %u allocated %u max_qid %u io_depth %u io_sqes %d "
	    "db_stride %u vectors %u maps default/read/poll %u/%u/%u\n",
	    dev->online_queues, dev->nr_allocated_queues, dev->max_qid,
	    (unsigned)dev->q_depth, dev->io_sqes, (unsigned)dev->db_stride,
	    dev->num_vecs, dev->io_queues[HCTX_TYPE_DEFAULT],
	    dev->io_queues[HCTX_TYPE_READ], dev->io_queues[HCTX_TYPE_POLL]);
	sbp(sb, "cmb %llu bytes%s, pmr %s, hmb %s (%llu bytes, %u descs), "
		"dbbuf %s\n",
	    (unsigned long long)dev->cmb_size,
	    dev->cmb_use_sqes ? " (SQs)" : "",
	    dev->pmr_enabled ? "enabled" : "off", dev->hmb ? "on" : "off",
	    (unsigned long long)dev->host_mem_size, dev->nr_host_mem_descs,
	    dev->dbbuf_dbs ? "on" : "off");
	if (!dev->queues) {
		sbp(sb, "no queues allocated\n");
		return;
	}
	nq = dev->online_queues;
	if (nq > dev->nr_allocated_queues)
		nq = dev->nr_allocated_queues;
	for (unsigned int i = 0; i < nq; i++) {
		struct nvme_queue *q = &dev->queues[i];
		uint16_t head = __atomic_load_n(&q->cq_head, __ATOMIC_RELAXED);
		uint8_t phase = __atomic_load_n(&q->cq_phase, __ATOMIC_RELAXED);
		bool pending = false;
		unsigned long fl = q->flags;

		if (q->cqes && head < q->q_depth)
			pending = (le16_to_cpu(q->cqes[head].status) & 1) ==
				  phase;
		sbp(sb,
		    "q%-3u depth %-5u sqes %u sq_tail %-5u last_db %-5u "
		    "cq_head %-5u phase %u vector %-3u%s%s%s%s%s "
		    "sub %llu comp %llu irq %llu spurious %llu%s%s\n",
		    (unsigned)q->qid, (unsigned)q->q_depth, (unsigned)q->sqes,
		    (unsigned)q->sq_tail, (unsigned)q->last_sq_tail,
		    (unsigned)head, (unsigned)phase, (unsigned)q->cq_vector,
		    nvme_test_bit(NVMEQ_ENABLED, &fl) ? " enabled" :
							" disabled",
		    nvme_test_bit(NVMEQ_SQ_CMB, &fl) ? " sq-in-cmb" : "",
		    nvme_test_bit(NVMEQ_DELETE_ERROR, &fl) ? " delete-error" :
							     "",
		    nvme_test_bit(NVMEQ_POLLED, &fl) ? " polled" : "",
		    q->dbbuf_sq_db ? " dbbuf" : "",
		    (unsigned long long)q->nr_submitted,
		    (unsigned long long)q->nr_completed,
		    (unsigned long long)q->nr_irqs,
		    (unsigned long long)q->nr_spurious,
		    pending ? " CQE-PENDING" : "",
		    q->sq_tail != q->last_sq_tail ? " DOORBELL-DUE" : "");
	}
}

struct inflight_ctx {
	struct nvme_sbuf *sb;
	uint64_t now;
	unsigned int count;
};

static const char *mq_state_name(int s)
{
	switch (s) {
	case MQ_RQ_IDLE:
		return "idle";
	case MQ_RQ_IN_FLIGHT:
		return "in-flight";
	case MQ_RQ_COMPLETE:
		return "complete";
	default:
		return "?";
	}
}

static int dump_inflight_one(struct blk_request *rq, void *data)
{
	struct inflight_ctx *ix = data;
	struct nvme_request *nr = nvme_req(rq);
	struct nvme_command *c = nr->cmd;
	uint16_t qid = nvme_req_qid(rq);
	uint8_t op = c ? c->common.opcode : 0xff;
	long long left_ms = 0;

	if (rq->deadline_ns)
		left_ms = ((long long)rq->deadline_ns - (long long)ix->now) /
			  1000000;
	sbp(ix->sb,
	    "qid %-3u tag %-5d cid 0x%04x opcode 0x%02x %-22s nsid %-4u "
	    "state %s retries %u flags%s%s%s deadline %+lld ms\n",
	    (unsigned)qid, rq->tag, (unsigned)nvme_cid(rq), (unsigned)op,
	    nvme_trace_opcode_name(qid, op),
	    c ? (unsigned)le32_to_cpu(c->common.nsid) : 0u,
	    mq_state_name(blk_mq_rq_state(rq)), (unsigned)nr->retries,
	    (nr->flags & NVME_REQ_CANCELLED) ? " cancelled" : "",
	    (nr->flags & NVME_REQ_USERCMD) ? " user" : "",
	    (rq->flags & BLK_REQ_RESERVED) ? " reserved" : "", left_ms);
	ix->count++;
	return 1; /* keep going */
}

static void dump_inflight(struct nvme_ctrl *ctrl, struct nvme_sbuf *sb)
{
	struct inflight_ctx ix = { sb, hrtimer_now_ns(), 0 };

	if (ctrl->admin_tagset)
		blk_mq_tagset_busy_iter(ctrl->admin_tagset, dump_inflight_one,
					&ix);
	if (ctrl->tagset)
		blk_mq_tagset_busy_iter(ctrl->tagset, dump_inflight_one, &ix);
	sbp(sb, "%u command(s) in flight\n", ix.count);
}

/* ==========================================================================
 * The debug tree
 * ========================================================================== */

/* arg2 of the tree's transient nodes: kind << 8 | index.  The object's
 * name lives in the node's `link' field (unused for directories and
 * files), so no pointer to a removable object is ever stored. */
enum {
	DK_OBJ_DIR = 1, /* /sys/kernel/debug/nvme/<obj> */
	DK_EST_DIR, /* /sys/kernel/debug/<obj> */
	DK_FI_DIR, /* .../<obj>/fault_inject */
	DK_FILE, /* .../<ctrl>/<file> */
	DK_FI_FILE, /* .../fault_inject/<attr> */
};

enum {
	DF_INFO = 0,
	DF_REGS,
	DF_QUEUES,
	DF_INFLIGHT,
	DF_CMD_LOG,
	DF_TRACE_ENABLE,
	DF_FW_SLOTS,
	DF_SELF_TEST,
	DF_TELEMETRY_HOST,
	DF_TELEMETRY_CTRL,
	DF_PEVENT,
	DF_EFFECTS,
	DF_NR
};

static const struct {
	const char *name;
	bool writable;
	bool needs_live; /* issues admin commands */
	bool needs_lock; /* reads transport state (shutdown_lock) */
} g_dbg_files[DF_NR] = {
	[DF_INFO] = { "info", false, false, false },
	[DF_REGS] = { "regs", false, false, true },
	[DF_QUEUES] = { "queues", false, false, true },
	[DF_INFLIGHT] = { "inflight", false, false, true },
	[DF_CMD_LOG] = { "cmd_log", false, false, false },
	[DF_TRACE_ENABLE] = { "trace_enable", true, false, false },
	[DF_FW_SLOTS] = { "fw_slots", false, true, false },
	[DF_SELF_TEST] = { "self_test", true, true, false },
	[DF_TELEMETRY_HOST] = { "telemetry_host", false, true, false },
	[DF_TELEMETRY_CTRL] = { "telemetry_ctrl", false, true, false },
	[DF_PEVENT] = { "persistent_event", false, true, false },
	[DF_EFFECTS] = { "effects", false, true, false },
};

static struct pfs_node *g_dbg_root; /* /sys/kernel/debug/nvme */
static struct pfs_node *g_dbg_est; /* /sys/kernel/debug, when hooked */
static DEFINE_MUTEX(g_dbg_init_mutex);
static volatile int g_dbg_inited;

#define DK_ARG2(kind, idx) (((uint64_t)(kind) << 8) | (uint64_t)(idx))
#define DK_KIND(n) ((int)(((n)->arg2 >> 8) & 0xff))
#define DK_INDEX(n) ((int)((n)->arg2 & 0xff))

static void dk_set_obj(struct pfs_node *n, const char *obj)
{
	size_t k = 0;

	while (obj[k] && k < sizeof(n->link) - 1) {
		n->link[k] = obj[k];
		k++;
	}
	n->link[k] = 0;
}

static struct pfs_node *dk_new(struct pfs_node *dir, const char *name,
			       enum pfs_type type, int kind, int idx,
			       const char *obj)
{
	/* The parent recorded is the static root: a transient parent is
	 * freed right after the lookup that made this node. */
	struct pfs_node *n = pfs_node_new(dir->fs,
					  g_dbg_root ? g_dbg_root : dir, name,
					  type);

	if (!n)
		return NULL;
	n->arg2 = DK_ARG2(kind, idx);
	dk_set_obj(n, obj);
	return n;
}

static long dbg_file_show(struct pfs_node *n, char *buf, long cap);
static long dbg_file_store(struct pfs_node *n, const char *buf, long len);
static long dbg_fi_show(struct pfs_node *n, char *buf, long cap);
static long dbg_fi_store(struct pfs_node *n, const char *buf, long len);
static int dbg_obj_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type);
static struct pfs_node *dbg_obj_lookup(struct pfs_node *dir, const char *name);
static int dbg_fi_list(struct pfs_node *dir, unsigned *cursor, char *name,
		       long cap, int *type);
static struct pfs_node *dbg_fi_lookup(struct pfs_node *dir, const char *name);

/* Object names in one key space: controllers 0x00000000 + key, fault
 * objects 0x40000000 + key (a namespace name appears once; a controller
 * name, which is also a fault object, once). */
#define DBG_FI_KEYBASE 0x40000000u

static int dbg_top_list_common(unsigned *cursor, char *name, long cap,
			       int *type, bool with_ctrls)
{
	if (with_ctrls && *cursor < DBG_FI_KEYBASE) {
		struct nvme_debug_ctrl *d;
		uint64_t fl;
		int found = 0;

		spin_lock_irqsave(&g_dbg_lock, &fl);
		list_for_each_entry(d, &g_dbg_list, link) {
			if (d->key >= *cursor && d->in_use && d->ctrl) {
				ksnprintf(name, (size_t)cap, "%s", d->name);
				*cursor = d->key + 1;
				found = 1;
				break;
			}
		}
		spin_unlock_irqrestore(&g_dbg_lock, fl);
		if (found) {
			*type = DT_DIR;
			return 1;
		}
		*cursor = DBG_FI_KEYBASE;
	}
	for (;;) {
		unsigned int c = *cursor >= DBG_FI_KEYBASE ?
					 *cursor - DBG_FI_KEYBASE :
					 0;

		if (!nvme_fault_list_names(&c, name, cap))
			return 0;
		*cursor = c + DBG_FI_KEYBASE;
		/* A controller is listed above already. */
		if (with_ctrls && nvme_debug_ctrl_exists(name))
			continue;
		*type = DT_DIR;
		return 1;
	}
}

static int dbg_top_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type)
{
	(void)dir;
	return dbg_top_list_common(cursor, name, cap, type, true);
}

static struct pfs_node *dbg_top_lookup(struct pfs_node *dir, const char *name)
{
	struct pfs_node *n;

	if (!nvme_debug_ctrl_exists(name) && !nvme_fault_name_exists(name))
		return NULL;
	n = dk_new(dir, name, PFS_DIR, DK_OBJ_DIR, 0, name);
	if (n)
		pfs_set_dynamic(n, dbg_obj_list, dbg_obj_lookup);
	return n;
}

/* /sys/kernel/debug: only the fault injection objects (the established
 * per-device directories). */
static int dbg_est_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type)
{
	(void)dir;
	if (*cursor < DBG_FI_KEYBASE)
		*cursor = DBG_FI_KEYBASE;
	return dbg_top_list_common(cursor, name, cap, type, false);
}

static struct pfs_node *dbg_est_lookup(struct pfs_node *dir, const char *name)
{
	struct pfs_node *n;

	if (kstrncmp(name, "nvme", 4) != 0 || !nvme_fault_name_exists(name))
		return NULL;
	n = dk_new(dir, name, PFS_DIR, DK_EST_DIR, 0, name);
	if (n)
		pfs_set_dynamic(n, dbg_obj_list, dbg_obj_lookup);
	return n;
}

/* <obj>/: the controller files (DK_OBJ_DIR of a controller) and
 * fault_inject. */
static int dbg_obj_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type)
{
	bool files = DK_KIND(dir) == DK_OBJ_DIR &&
		     nvme_debug_ctrl_exists(dir->link);

	while (*cursor <= DF_NR) {
		unsigned int i = *cursor;

		*cursor = i + 1;
		if (i < DF_NR) {
			if (!files)
				continue;
			ksnprintf(name, (size_t)cap, "%s", g_dbg_files[i].name);
			*type = DT_REG;
			return 1;
		}
		if (nvme_fault_name_exists(dir->link)) {
			ksnprintf(name, (size_t)cap, "fault_inject");
			*type = DT_DIR;
			return 1;
		}
	}
	return 0;
}

static struct pfs_node *dbg_obj_lookup(struct pfs_node *dir, const char *name)
{
	struct pfs_node *n;

	if (kstrcmp(name, "fault_inject") == 0) {
		if (!nvme_fault_name_exists(dir->link))
			return NULL;
		n = dk_new(dir, name, PFS_DIR, DK_FI_DIR, 0, dir->link);
		if (n)
			pfs_set_dynamic(n, dbg_fi_list, dbg_fi_lookup);
		return n;
	}
	if (DK_KIND(dir) != DK_OBJ_DIR || !nvme_debug_ctrl_exists(dir->link))
		return NULL;
	for (int i = 0; i < DF_NR; i++) {
		if (kstrcmp(name, g_dbg_files[i].name) != 0)
			continue;
		n = dk_new(dir, name, PFS_FILE, DK_FILE, i, dir->link);
		if (!n)
			return NULL;
		n->show = dbg_file_show;
		if (g_dbg_files[i].writable) {
			n->store = dbg_file_store;
			n->mode = 0600;
		} else {
			n->mode = 0400;
		}
		return n;
	}
	return NULL;
}

static int dbg_fi_list(struct pfs_node *dir, unsigned *cursor, char *name,
		       long cap, int *type)
{
	(void)dir;
	if (*cursor >= NVME_FI_ATTR_NR)
		return 0;
	ksnprintf(name, (size_t)cap, "%s", nvme_fault_attr_name((int)*cursor));
	*type = DT_REG;
	*cursor = *cursor + 1;
	return 1;
}

static struct pfs_node *dbg_fi_lookup(struct pfs_node *dir, const char *name)
{
	for (int a = 0; a < NVME_FI_ATTR_NR; a++) {
		struct pfs_node *n;

		if (kstrcmp(name, nvme_fault_attr_name(a)) != 0)
			continue;
		if (!nvme_fault_name_exists(dir->link))
			return NULL;
		n = dk_new(dir, name, PFS_FILE, DK_FI_FILE, a, dir->link);
		if (!n)
			return NULL;
		n->show = dbg_fi_show;
		if (nvme_fault_attr_writable(a)) {
			n->store = dbg_fi_store;
			n->mode = 0600;
		} else {
			n->mode = 0400;
		}
		return n;
	}
	return NULL;
}

static long dbg_fi_show(struct pfs_node *n, char *buf, long cap)
{
	return nvme_fault_attr_show(n->link, DK_INDEX(n), buf, cap);
}

static long dbg_fi_store(struct pfs_node *n, const char *buf, long len)
{
	return nvme_fault_attr_store(n->link, DK_INDEX(n), buf, len);
}

static long dbg_file_show(struct pfs_node *n, char *buf, long cap)
{
	struct nvme_debug_ctrl *d = NULL;
	struct nvme_ctrl *ctrl;
	struct nvme_sbuf sb = { buf, cap, 0 };
	int idx = DK_INDEX(n);
	long ret;

	if (idx < 0 || idx >= DF_NR)
		return -EINVAL;
	ctrl = nvme_debug_get_ctrl(n->link, &d);
	if (!ctrl)
		return -ENODEV;

	if (g_dbg_files[idx].needs_live &&
	    nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE) {
		sbp(&sb, "controller %s is %s: no admin commands now\n",
		    ctrl->name,
		    str_or(nvme_ctrl_state_name(nvme_ctrl_state(ctrl)), "?"));
		ret = sb.pos;
		goto out;
	}
	if (g_dbg_files[idx].needs_lock) {
		/* Transport state (BAR mapping, queue arrays, tag sets)
		 * changes only under shutdown_lock: dump under it, but never
		 * wait for a reset that may be stuck. */
		if (!mutex_trylock(&ctrl->shutdown_lock)) {
			sbp(&sb, "controller %s busy (enable/disable in "
				 "progress), try again\n",
			    ctrl->name);
			ret = sb.pos;
			goto out;
		}
		if (!nvme_debug_state_dumpable(ctrl)) {
			mutex_unlock(&ctrl->shutdown_lock);
			sbp(&sb, "controller %s is going away\n", ctrl->name);
			ret = sb.pos;
			goto out;
		}
	}

	switch (idx) {
	case DF_INFO:
		dump_info(ctrl, &sb);
		ret = sb.pos;
		break;
	case DF_REGS:
		dump_regs(ctrl, &sb);
		ret = sb.pos;
		break;
	case DF_QUEUES:
		dump_queues(ctrl, &sb);
		ret = sb.pos;
		break;
	case DF_INFLIGHT:
		dump_inflight(ctrl, &sb);
		ret = sb.pos;
		break;
	case DF_CMD_LOG:
		nvme_trace_format(d, &sb);
		ret = sb.pos;
		break;
	case DF_TRACE_ENABLE:
		sbp(&sb, "%d\n", d->trace_on ? 1 : 0);
		ret = sb.pos;
		break;
	case DF_FW_SLOTS:
		ret = nvme_admin_show_fw_slots(ctrl, buf, cap);
		break;
	case DF_SELF_TEST:
		ret = nvme_admin_show_self_test(ctrl, buf, cap);
		break;
	case DF_TELEMETRY_HOST:
		ret = nvme_admin_show_telemetry(ctrl, true, buf, cap);
		break;
	case DF_TELEMETRY_CTRL:
		ret = nvme_admin_show_telemetry(ctrl, false, buf, cap);
		break;
	case DF_PEVENT:
		ret = nvme_admin_show_pevent(ctrl, buf, cap);
		break;
	case DF_EFFECTS:
		ret = nvme_admin_show_effects(ctrl, buf, cap);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (g_dbg_files[idx].needs_lock)
		mutex_unlock(&ctrl->shutdown_lock);
out:
	nvme_put_ctrl(ctrl);
	return ret;
}

static long dbg_file_store(struct pfs_node *n, const char *buf, long len)
{
	struct nvme_debug_ctrl *d = NULL;
	struct nvme_ctrl *ctrl;
	int idx = DK_INDEX(n);
	long ret;

	if (idx < 0 || idx >= DF_NR || !g_dbg_files[idx].writable)
		return -EPERM;
	if (!capable())
		return -EACCES;
	ctrl = nvme_debug_get_ctrl(n->link, &d);
	if (!ctrl)
		return -ENODEV;
	switch (idx) {
	case DF_TRACE_ENABLE: {
		long i = 0;

		while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
			i++;
		if (i < len && (buf[i] == '0' || buf[i] == '1')) {
			__atomic_store_n(&d->trace_on, buf[i] == '1',
					 __ATOMIC_RELAXED);
			ret = len;
		} else {
			ret = -EINVAL;
		}
		break;
	}
	case DF_SELF_TEST:
		if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
			ret = -EBUSY;
		else
			ret = nvme_admin_store_self_test(ctrl, buf, len);
		break;
	default:
		ret = -EPERM;
		break;
	}
	nvme_put_ctrl(ctrl);
	return ret;
}

void nvme_debugfs_init_once(void)
{
	struct pfs_node *dbg;
	char v[8];

	if (__atomic_load_n(&g_dbg_inited, __ATOMIC_ACQUIRE))
		return;
	mutex_lock(&g_dbg_init_mutex);
	if (g_dbg_inited) {
		mutex_unlock(&g_dbg_init_mutex);
		return;
	}
	if (nvme_cmdline_get("nvme_core.trace", v, sizeof(v)) == 1)
		nvme_trace_enabled = !(v[0] == '0' || v[0] == 'n' ||
				       v[0] == 'N');
	/* Directories are dynamic from the moment they are visible
	 * (pfs_add_node publishes them complete). */
	dbg = sysfs_mkdir("kernel");
	g_dbg_root = NULL;
	if (dbg) {
		struct pfs_node t;
		struct pfs_node *cur;

		kmemset(&t, 0, sizeof(t));
		t.list = dbg_top_list;
		t.lookup = dbg_top_lookup;
		g_dbg_root = pfs_add_node(dbg->fs, "kernel/debug/nvme",
					  PFS_DIR, &t);
		/* The established per-device fault_inject directories live
		 * right under /sys/kernel/debug: hooked only when nobody else
		 * made that directory dynamic (adding the nvme directory above
		 * created it as a plain one if it was missing). */
		cur = pfs_lookup(dbg->fs, "kernel/debug");
		if (cur && !cur->transient &&
		    !__atomic_load_n(&cur->list, __ATOMIC_ACQUIRE) &&
		    !__atomic_load_n(&cur->lookup, __ATOMIC_ACQUIRE)) {
			kmemset(&t, 0, sizeof(t));
			t.list = dbg_est_list;
			t.lookup = dbg_est_lookup;
			g_dbg_est = pfs_add_node(dbg->fs, "kernel/debug",
						 PFS_DIR, &t);
		}
		if (cur)
			pfs_node_put(cur);
	}
	if (!g_dbg_root)
		kprintf(NVME_LOG_PREFIX "cannot create the debug tree\n");
	__atomic_store_n(&g_dbg_inited, 1, __ATOMIC_RELEASE);
	mutex_unlock(&g_dbg_init_mutex);
}
