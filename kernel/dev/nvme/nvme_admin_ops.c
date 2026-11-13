// LikeOS - NVMe admin features: firmware, self-test, telemetry, persistent event log, security
//
// Kernel helpers for the admin features programs normally drive through
// pass-through (nvme-cli, smartctl), with the procedures the established
// tooling uses:
//
//   FIRMWARE   nvme_fw_download(): Firmware Image Download in pieces of
//              the update granularity (Identify FWUG, 4 KiB units; 0 = no
//              information -> 4 KiB, 0xff = no restriction), capped by the
//              controller's maximum transfer; dword counts and offsets.
//              nvme_fw_commit(): Firmware Commit with slot, commit action
//              (0..7) and boot partition ID; the statuses that mean "done,
//              but a conventional / subsystem / controller reset activates
//              it" are reported as such (returned to the caller), the
//              multiple-update-detected result bit is logged.
//              Firmware slot information (log 03h) decoded for the debug
//              tree (active slot, next slot, revision of each slot).
//   SELF-TEST  nvme_device_self_test(): short (1h), extended (2h),
//              vendor (Eh) or abort (Fh) on a namespace, all (FFFFFFFFh)
//              or the controller only (0); the self-test log (06h) decoded.
//   TELEMETRY  nvme_get_telemetry_log(): host-initiated (07h, optionally
//              creating new data) or controller-initiated (08h, retaining
//              the asynchronous event): the 512-byte header first, then
//              data areas 1..3, or 1..4 when the controller supports the
//              extended area and the host enabled it (Host Behavior
//              Support ETDAS); the generation number is checked again
//              after the transfer (-EAGAIN when the data changed under
//              the read).
//   EVENTS     nvme_get_persistent_event_log(): establish the reporting
//              context and read (or read within the context, or release
//              it), header first, then the total log length.
//   SECURITY   nvme_security_send_recv(): Security Send/Receive with
//              SECP/SPSP and the transfer length, inserted at the head of
//              the admin queue (the established self-encrypting drive
//              path).
//
// Commands with effects (firmware commit, self-test) run between
// nvme_passthru_start() and nvme_passthru_end() like pass-through, so a
// command the effects log marks as needing exclusive execution freezes
// I/O, and a namespace inventory or capability change triggers a rescan.
//
// All helpers may sleep; they return 0, a positive NVMe status, or -errno.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

/* The largest log these helpers read into memory. */
#define NVME_ADMIN_LOG_MAX (32u * 1024u * 1024u)
/* Telemetry / persistent event log header size. */
#define NVME_LOG_HDR_SIZE 512u

/* ==========================================================================
 * Helpers
 * ========================================================================== */

struct adm_sbuf {
	char *buf;
	long cap;
	long pos;
};

static void asb(struct adm_sbuf *sb, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void asb(struct adm_sbuf *sb, const char *fmt, ...)
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

/* The largest data transfer of one admin command, dword aligned. */
static uint32_t nvme_admin_max_xfer(struct nvme_ctrl *ctrl)
{
	uint64_t max = ctrl->max_hw_sectors ?
			       (uint64_t)ctrl->max_hw_sectors << 9 :
			       NVME_CTRL_PAGE_SIZE;

	if (max > 8u * 1024u * 1024u)
		max = 8u * 1024u * 1024u;
	return (uint32_t)(max & ~3ULL);
}

static bool nvme_admin_usable(struct nvme_ctrl *ctrl)
{
	return ctrl && ctrl->admin_q;
}

/* Run an admin command that may have effects, with the pass-through
 * effects handling around it. */
static int nvme_admin_exec_effects(struct nvme_ctrl *ctrl,
				   struct nvme_command *c, void *buf,
				   unsigned int len, uint32_t *result)
{
	union nvme_result res;
	uint32_t effects;
	int ret;

	res.u64 = 0;
	effects = nvme_passthru_start(ctrl, NULL, c->common.opcode);
	ret = __nvme_submit_sync_cmd(ctrl->admin_q, c, &res, buf, len,
				     NVME_QID_ANY, 0);
	if (effects)
		nvme_passthru_end(ctrl, NULL, effects, c, ret);
	if (result)
		*result = le32_to_cpu(res.u32);
	return ret;
}

int nvme_admin_get_log_rae(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t lid,
			   uint8_t lsp, bool rae, void *buf, size_t len,
			   uint64_t offset)
{
	uint32_t max;
	size_t done = 0;

	might_sleep();
	if (!nvme_admin_usable(ctrl) || !buf || !len)
		return -EINVAL;
	if ((len & 3) || (offset & 3))
		return -EINVAL;
	max = nvme_admin_max_xfer(ctrl);
	if (max < 4)
		return -EINVAL;
	while (done < len) {
		struct nvme_command c;
		size_t chunk = len - done;
		uint32_t numd;
		int ret;

		if (chunk > max)
			chunk = max;
		numd = (uint32_t)(chunk >> 2) - 1;
		mm_memset(&c, 0, sizeof(c));
		c.get_log_page.opcode = nvme_admin_get_log_page;
		c.get_log_page.nsid = cpu_to_le32(nsid);
		c.get_log_page.lid = lid;
		c.get_log_page.lsp = (uint8_t)((lsp & 0x7f) | (rae ? 0x80 : 0));
		c.get_log_page.numdl = cpu_to_le16((uint16_t)(numd & 0xffff));
		c.get_log_page.numdu = cpu_to_le16((uint16_t)(numd >> 16));
		c.get_log_page.lpo = cpu_to_le64(offset + done);
		ret = __nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL,
					     (uint8_t *)buf + done,
					     (unsigned int)chunk, NVME_QID_ANY,
					     0);
		if (ret)
			return ret;
		done += chunk;
	}
	return 0;
}

/* ==========================================================================
 * Firmware
 * ========================================================================== */

int nvme_fw_download(struct nvme_ctrl *ctrl, const void *image, size_t len,
		     uint32_t xfer_size)
{
	uint32_t max, xfer, gran = 0;
	uint8_t *bounce;
	size_t off = 0;
	int ret = 0;

	might_sleep();
	if (!nvme_admin_usable(ctrl) || !image)
		return -EINVAL;
	if (!(ctrl->oacs & NVME_CTRL_OACS_FW))
		return -EOPNOTSUPP;
	/* Dword granular image (offsets and counts are dwords). */
	if (!len || (len & 3) || len > 0xffffffffULL * 4)
		return -EINVAL;

	{
		struct nvme_id_ctrl *id = NULL;

		if (!nvme_identify_ctrl(ctrl, &id) && id) {
			if (id->fwug && id->fwug != 0xff)
				gran = (uint32_t)id->fwug * 4096u;
			kfree(id);
		}
	}
	max = nvme_admin_max_xfer(ctrl);
	if (xfer_size) {
		if (xfer_size & 3)
			return -EINVAL;
		xfer = xfer_size;
		if (gran && (xfer % gran))
			kprintf(NVME_LOG_PREFIX "%s: firmware transfer size %u "
						"is not a multiple of the "
						"update granularity %u\n",
				ctrl->name, (unsigned)xfer, (unsigned)gran);
	} else {
		xfer = gran ? gran : 4096u;
	}
	if (xfer > max)
		xfer = max;
	if (xfer < 4)
		return -EINVAL;

	bounce = kalloc(xfer);
	if (!bounce)
		return -ENOMEM;
	while (off < len) {
		struct nvme_command c;
		uint32_t chunk = (uint32_t)((len - off) < xfer ? (len - off) :
							       xfer);

		mm_memcpy(bounce, (const uint8_t *)image + off, chunk);
		mm_memset(&c, 0, sizeof(c));
		c.dlfw.opcode = nvme_admin_download_fw;
		c.dlfw.numd = cpu_to_le32((chunk >> 2) - 1);
		c.dlfw.offset = cpu_to_le32((uint32_t)(off >> 2));
		ret = __nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, bounce,
					     chunk, NVME_QID_ANY, 0);
		if (ret) {
			kprintf(NVME_LOG_PREFIX "%s: firmware download failed at "
						"offset %llu: %d\n",
				ctrl->name, (unsigned long long)off, ret);
			break;
		}
		off += chunk;
	}
	kfree(bounce);
	return ret;
}

int nvme_fw_commit(struct nvme_ctrl *ctrl, uint8_t slot, uint8_t action,
		   uint8_t bpid, uint32_t *result)
{
	struct nvme_command c;
	uint32_t res = 0;
	int ret;

	might_sleep();
	if (!nvme_admin_usable(ctrl))
		return -EINVAL;
	if (!(ctrl->oacs & NVME_CTRL_OACS_FW))
		return -EOPNOTSUPP;
	/* slot 0..7 (0: the controller chooses), commit action 0..7 (the
	 * CA field, unshifted), boot partition ID 0/1. */
	if (slot > NVME_FWACT_SLOT_MASK || action > 7 || bpid > 1)
		return -EINVAL;
	mm_memset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_activate_fw;
	c.common.cdw10 = cpu_to_le32((uint32_t)slot |
				     ((uint32_t)action << 3) |
				     (bpid ? NVME_FWACT_BPID : 0));
	ret = nvme_admin_exec_effects(ctrl, &c, NULL, 0, &res);
	if (result)
		*result = res;
	switch (ret > 0 ? (ret & NVME_SCT_SC_MASK) : 0) {
	case NVME_SC_FW_NEEDS_CONV_RESET:
		kprintf(NVME_LOG_PREFIX "%s: firmware committed to slot %u; a "
					"conventional reset activates it\n",
			ctrl->name, (unsigned)slot);
		break;
	case NVME_SC_FW_NEEDS_SUBSYS_RESET:
		kprintf(NVME_LOG_PREFIX "%s: firmware committed to slot %u; an "
					"NVM subsystem reset activates it\n",
			ctrl->name, (unsigned)slot);
		break;
	case NVME_SC_FW_NEEDS_RESET:
		kprintf(NVME_LOG_PREFIX "%s: firmware committed to slot %u; a "
					"controller reset activates it\n",
			ctrl->name, (unsigned)slot);
		break;
	default:
		break;
	}
	if (ret < 0)
		return ret;
	if (!ret && (res & NVME_FWACT_RESULT_MUD))
		kprintf(NVME_LOG_PREFIX "%s: firmware commit: multiple update "
					"detected\n",
			ctrl->name);
	return ret;
}

long nvme_admin_show_fw_slots(struct nvme_ctrl *ctrl, char *buf, long cap)
{
	struct adm_sbuf sb = { buf, cap, 0 };
	struct nvme_fw_slot_info_log *log;
	unsigned int nslots;
	int ret;

	BUILD_BUG_ON(sizeof(struct nvme_fw_slot_info_log) != 512);
	nslots = (ctrl->frmw & NVME_CTRL_FRMW_NR_SLOTS_MASK) >>
		 NVME_CTRL_FRMW_NR_SLOTS_SHIFT;
	asb(&sb, "slots %u%s%s%s\n", nslots,
	    (ctrl->frmw & NVME_CTRL_FRMW_1ST_RO) ? ", slot 1 read-only" : "",
	    (ctrl->frmw & NVME_CTRL_FRMW_FW_ACT_NO_RESET) ?
		    ", activation without reset" :
		    "",
	    (ctrl->frmw & NVME_CTRL_FRMW_MP_UP_DETECTION) ?
		    ", multiple update detection" :
		    "");
	log = kalloc(sizeof(*log));
	if (!log)
		return -ENOMEM;
	mm_memset(log, 0, sizeof(*log));
	ret = nvme_admin_get_log_rae(ctrl, NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0,
				     false, log, sizeof(*log), 0);
	if (ret) {
		asb(&sb, "firmware slot log: error %d\n", ret);
	} else {
		asb(&sb, "active slot %u, next slot %u\n",
		    (unsigned)NVME_FW_AFI_ACTIVE(log->afi),
		    (unsigned)NVME_FW_AFI_NEXT(log->afi));
		for (unsigned int s = 0; s < 7 && s < (nslots ? nslots : 7);
		     s++) {
			char rev[9];
			uint64_t v = le64_to_cpu(log->frs[s]);

			for (int k = 0; k < 8; k++) {
				char ch = (char)((v >> (8 * k)) & 0xff);

				rev[k] = (ch >= 0x20 && ch < 0x7f) ? ch : ' ';
			}
			rev[8] = 0;
			asb(&sb, "slot %u: %s%s\n", s + 1,
			    v ? rev : "(empty)",
			    (unsigned int)NVME_FW_AFI_ACTIVE(log->afi) == s + 1 ?
				    "  [active]" :
								    "");
		}
	}
	kfree(log);
	return sb.pos;
}

/* ==========================================================================
 * Device self-test
 * ========================================================================== */

int nvme_device_self_test(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t stc)
{
	struct nvme_command c;

	might_sleep();
	if (!nvme_admin_usable(ctrl))
		return -EINVAL;
	if (!(ctrl->oacs & NVME_CTRL_OACS_SELF_TEST))
		return -EOPNOTSUPP;
	switch (stc) {
	case 0x1: /* short */
	case 0x2: /* extended */
	case 0xe: /* vendor specific */
	case 0xf: /* abort */
		break;
	default:
		return -EINVAL;
	}
	mm_memset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_dev_self_test;
	c.common.nsid = cpu_to_le32(nsid);
	c.common.cdw10 = cpu_to_le32(stc);
	return nvme_admin_exec_effects(ctrl, &c, NULL, 0, NULL);
}

static const char *st_result_name(unsigned int r)
{
	switch (r) {
	case NVME_ST_RESULT_NO_ERR:
		return "completed without error";
	case NVME_ST_RESULT_ABORTED:
		return "aborted by a self-test command";
	case NVME_ST_RESULT_CLR:
		return "aborted by a controller reset";
	case NVME_ST_RESULT_NS_REMOVED:
		return "aborted: namespace removed";
	case NVME_ST_RESULT_ABORTED_FORMAT:
		return "aborted by Format NVM";
	case NVME_ST_RESULT_FATAL_ERR:
		return "fatal or unknown error";
	case NVME_ST_RESULT_UNKNOWN_SEG_FAIL:
		return "failed, segment unknown";
	case NVME_ST_RESULT_KNOWN_SEG_FAIL:
		return "failed in a known segment";
	case NVME_ST_RESULT_ABORTED_UNKNOWN:
		return "aborted, reason unknown";
	case NVME_ST_RESULT_ABORTED_SANITIZE:
		return "aborted by sanitize";
	default:
		return "reserved";
	}
}

static const char *st_code_name(unsigned int code)
{
	switch (code) {
	case 0x1:
		return "short";
	case 0x2:
		return "extended";
	case 0xe:
		return "vendor";
	default:
		return "?";
	}
}

long nvme_admin_show_self_test(struct nvme_ctrl *ctrl, char *buf, long cap)
{
	struct adm_sbuf sb = { buf, cap, 0 };
	struct nvme_self_test_log *log;
	/* The 564-byte log, read as a dword multiple. */
	size_t sz = (sizeof(*log) + 3) & ~(size_t)3;
	int ret;

	if (!(ctrl->oacs & NVME_CTRL_OACS_SELF_TEST)) {
		asb(&sb, "device self-test not supported\n");
		return sb.pos;
	}
	log = kalloc(sz);
	if (!log)
		return -ENOMEM;
	mm_memset(log, 0, sz);
	ret = nvme_admin_get_log_rae(ctrl, NVME_NSID_ALL,
				     NVME_LOG_DEVICE_SELF_TEST, 0, false, log,
				     sz, 0);
	if (ret) {
		asb(&sb, "self-test log: error %d\n", ret);
		kfree(log);
		return sb.pos;
	}
	if (log->current_operation & 0xf)
		asb(&sb, "running: %s test, %u%% done\n",
		    st_code_name(log->current_operation & 0xf),
		    (unsigned)(log->completion & 0x7f));
	else
		asb(&sb, "no self-test running\n");
	for (int i = 0; i < NVME_LOG_ST_MAX_RESULTS; i++) {
		const struct nvme_st_result *r = &log->result[i];
		unsigned int res = r->dsts & NVME_ST_RESULT_MASK;

		if (res == NVME_ST_RESULT_NOT_USED)
			continue;
		asb(&sb, "#%-2d %-8s %s, power-on hours %llu",
		    i, st_code_name(r->dsts >> NVME_ST_CODE_SHIFT),
		    st_result_name(res), (unsigned long long)le64_to_cpu(r->poh));
		if (res == NVME_ST_RESULT_KNOWN_SEG_FAIL)
			asb(&sb, ", segment %u", (unsigned)r->seg);
		if (r->vdi & NVME_ST_VALID_DIAG_INFO_NSID)
			asb(&sb, ", nsid %u", (unsigned)le32_to_cpu(r->nsid));
		if (r->vdi & NVME_ST_VALID_DIAG_INFO_FLBA)
			asb(&sb, ", failing lba %llu",
			    (unsigned long long)le64_to_cpu(r->flba));
		if (r->vdi & NVME_ST_VALID_DIAG_INFO_SCT)
			asb(&sb, ", sct 0x%x", (unsigned)r->sct);
		if (r->vdi & NVME_ST_VALID_DIAG_INFO_SC)
			asb(&sb, ", sc 0x%x", (unsigned)r->sc);
		asb(&sb, "\n");
	}
	kfree(log);
	return sb.pos;
}

static bool adm_word(const char *s, long n, const char *w, long *used)
{
	long k = 0;

	while (w[k] && k < n && s[k] == w[k])
		k++;
	if (w[k])
		return false;
	if (k < n && s[k] != ' ' && s[k] != '\t' && s[k] != '\n')
		return false;
	*used = k;
	return true;
}

long nvme_admin_store_self_test(struct nvme_ctrl *ctrl, const char *buf,
				long len)
{
	uint8_t stc;
	uint32_t nsid = NVME_NSID_ALL;
	long i = 0, used = 0;
	int ret;

	while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
		i++;
	if (adm_word(buf + i, len - i, "short", &used))
		stc = 0x1;
	else if (adm_word(buf + i, len - i, "extended", &used))
		stc = 0x2;
	else if (adm_word(buf + i, len - i, "abort", &used))
		stc = 0xf;
	else
		return -EINVAL;
	i += used;
	while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
		i++;
	if (i < len && buf[i] >= '0' && buf[i] <= '9') {
		uint64_t v = 0;

		while (i < len && buf[i] >= '0' && buf[i] <= '9') {
			v = v * 10 + (uint64_t)(buf[i] - '0');
			if (v > 0xffffffffULL)
				return -EINVAL;
			i++;
		}
		nsid = (uint32_t)v;
	}
	while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n'))
		i++;
	if (i != len)
		return -EINVAL;
	ret = nvme_device_self_test(ctrl, nsid, stc);
	if (ret > 0)
		return -EIO; /* the controller refused (status logged) */
	return ret < 0 ? ret : len;
}

/* ==========================================================================
 * Telemetry
 * ========================================================================== */

/* Is the extended telemetry data area 4 enabled (Host Behavior ETDAS)? */
static bool nvme_telemetry_da4(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_host_behavior *hb;
	uint32_t res = 0;
	bool on = false;

	if (!(ctrl->lpa & NVME_CTRL_LPA_TELEMETRY_DA4))
		return false;
	hb = kalloc(sizeof(*hb));
	if (!hb)
		return false;
	mm_memset(hb, 0, sizeof(*hb));
	if (!nvme_get_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0, hb,
			       sizeof(*hb), &res))
		on = hb->etdas != 0;
	kfree(hb);
	return on;
}

static uint8_t telemetry_gen(const struct nvme_telemetry_log *h, bool host)
{
	return host ? h->hostdgn : h->ctrldgn;
}

int nvme_get_telemetry_log(struct nvme_ctrl *ctrl, bool host_initiated,
			   bool create, void **buf, size_t *len)
{
	uint8_t lid = host_initiated ? NVME_LOG_TELEMETRY_HOST :
				       NVME_LOG_TELEMETRY_CTRL;
	/* Controller-initiated data stays reported until read (RAE). */
	bool rae = !host_initiated;
	struct nvme_telemetry_log *hdr, *again;
	uint64_t last, total;
	uint8_t *log;
	int ret;

	might_sleep();
	if (!buf || !len)
		return -EINVAL;
	*buf = NULL;
	*len = 0;
	if (!nvme_admin_usable(ctrl))
		return -EINVAL;
	if (!(ctrl->lpa & NVME_CTRL_LPA_TELEMETRY))
		return -EOPNOTSUPP;
	if (create && !host_initiated)
		return -EINVAL;

	BUILD_BUG_ON(sizeof(struct nvme_telemetry_log) != NVME_LOG_HDR_SIZE);
	hdr = kalloc(NVME_LOG_HDR_SIZE);
	if (!hdr)
		return -ENOMEM;
	mm_memset(hdr, 0, NVME_LOG_HDR_SIZE);
	ret = nvme_admin_get_log_rae(ctrl, 0, lid,
				     create ? NVME_LOG_TELEM_LSP_CREATE : 0, rae,
				     hdr, NVME_LOG_HDR_SIZE, 0);
	if (ret)
		goto out_hdr;
	if (!host_initiated && !hdr->ctrlavail) {
		/* No controller-initiated data: the header is all. */
		last = 0;
	} else if (nvme_telemetry_da4(ctrl)) {
		last = le32_to_cpu(hdr->dalb4);
	} else {
		last = le16_to_cpu(hdr->dalb3);
	}
	total = (last + 1) * NVME_LOG_HDR_SIZE;
	if (total > NVME_ADMIN_LOG_MAX) {
		kprintf(NVME_LOG_PREFIX "%s: telemetry log of %llu bytes "
					"truncated to %u\n",
			ctrl->name, (unsigned long long)total,
			(unsigned)NVME_ADMIN_LOG_MAX);
		total = NVME_ADMIN_LOG_MAX;
	}
	log = kalloc((size_t)total);
	if (!log) {
		ret = -ENOMEM;
		goto out_hdr;
	}
	mm_memcpy(log, hdr, NVME_LOG_HDR_SIZE);
	if (total > NVME_LOG_HDR_SIZE) {
		/* The data areas; never create again while reading. */
		ret = nvme_admin_get_log_rae(ctrl, 0, lid, 0, rae,
					     log + NVME_LOG_HDR_SIZE,
					     (size_t)total - NVME_LOG_HDR_SIZE,
					     NVME_LOG_HDR_SIZE);
		if (ret)
			goto out_log;
		/* The data must belong to the header's generation. */
		again = kalloc(NVME_LOG_HDR_SIZE);
		if (!again) {
			ret = -ENOMEM;
			goto out_log;
		}
		ret = nvme_admin_get_log_rae(ctrl, 0, lid, 0, rae, again,
					     NVME_LOG_HDR_SIZE, 0);
		if (!ret && telemetry_gen(again, host_initiated) !=
				    telemetry_gen(hdr, host_initiated))
			ret = -EAGAIN;
		kfree(again);
		if (ret)
			goto out_log;
	}
	kfree(hdr);
	*buf = log;
	*len = (size_t)total;
	return 0;

out_log:
	kfree(log);
out_hdr:
	kfree(hdr);
	return ret;
}

long nvme_admin_show_telemetry(struct nvme_ctrl *ctrl, bool host, char *buf,
			       long cap)
{
	struct adm_sbuf sb = { buf, cap, 0 };
	struct nvme_telemetry_log *h;
	int ret;

	if (!(ctrl->lpa & NVME_CTRL_LPA_TELEMETRY)) {
		asb(&sb, "telemetry not supported\n");
		return sb.pos;
	}
	h = kalloc(NVME_LOG_HDR_SIZE);
	if (!h)
		return -ENOMEM;
	mm_memset(h, 0, NVME_LOG_HDR_SIZE);
	/* The header only: never creates host data, retains the
	 * controller-initiated event. */
	ret = nvme_admin_get_log_rae(ctrl, 0,
				     host ? NVME_LOG_TELEMETRY_HOST :
					    NVME_LOG_TELEMETRY_CTRL,
				     0, !host, h, NVME_LOG_HDR_SIZE, 0);
	if (ret) {
		asb(&sb, "telemetry header: error %d\n", ret);
	} else {
		asb(&sb, "log id 0x%02x, IEEE OUI %02x%02x%02x\n",
		    (unsigned)h->lpi, (unsigned)h->ieee[2],
		    (unsigned)h->ieee[1], (unsigned)h->ieee[0]);
		asb(&sb, "data area last blocks: 1=%u 2=%u 3=%u 4=%u%s\n",
		    (unsigned)le16_to_cpu(h->dalb1),
		    (unsigned)le16_to_cpu(h->dalb2),
		    (unsigned)le16_to_cpu(h->dalb3),
		    (unsigned)le32_to_cpu(h->dalb4),
		    (ctrl->lpa & NVME_CTRL_LPA_TELEMETRY_DA4) ?
			    " (area 4 supported)" :
			    "");
		asb(&sb, "host generation %u, controller data %s, controller "
			 "generation %u\n",
		    (unsigned)h->hostdgn,
		    h->ctrlavail ? "available" : "not available",
		    (unsigned)h->ctrldgn);
		asb(&sb, "reason identifier:");
		for (int i = 0; i < 32; i++)
			asb(&sb, " %02x", (unsigned)h->rsnident[i]);
		asb(&sb, " ...\n");
	}
	kfree(h);
	return sb.pos;
}

/* ==========================================================================
 * Persistent event log
 * ========================================================================== */

int nvme_get_persistent_event_log(struct nvme_ctrl *ctrl, uint8_t action,
				  void **buf, size_t *len)
{
	struct nvme_persistent_event_log *hdr;
	uint64_t tll;
	uint8_t *log;
	int ret;

	might_sleep();
	if (!buf || !len)
		return -EINVAL;
	*buf = NULL;
	*len = 0;
	if (!nvme_admin_usable(ctrl))
		return -EINVAL;
	if (!(ctrl->lpa & NVME_CTRL_LPA_PERSISTENT_EVENT))
		return -EOPNOTSUPP;
	if (action > NVME_PEVENT_LOG_RELEASE_CTX)
		return -EINVAL;

	BUILD_BUG_ON(sizeof(struct nvme_persistent_event_log) !=
		     NVME_LOG_HDR_SIZE);
	hdr = kalloc(NVME_LOG_HDR_SIZE);
	if (!hdr)
		return -ENOMEM;
	mm_memset(hdr, 0, NVME_LOG_HDR_SIZE);
	ret = nvme_admin_get_log_rae(ctrl, NVME_NSID_ALL,
				     NVME_LOG_PERSISTENT_EVENT, action, false,
				     hdr, NVME_LOG_HDR_SIZE, 0);
	if (ret || action == NVME_PEVENT_LOG_RELEASE_CTX) {
		kfree(hdr);
		return ret;
	}
	tll = le64_to_cpu(hdr->tll);
	if (tll < NVME_LOG_HDR_SIZE) {
		kfree(hdr);
		return -EPROTO;
	}
	tll &= ~3ULL;
	if (tll > NVME_ADMIN_LOG_MAX) {
		kprintf(NVME_LOG_PREFIX "%s: persistent event log of %llu bytes "
					"truncated to %u\n",
			ctrl->name, (unsigned long long)tll,
			(unsigned)NVME_ADMIN_LOG_MAX);
		tll = NVME_ADMIN_LOG_MAX;
	}
	log = kalloc((size_t)tll);
	if (!log) {
		kfree(hdr);
		return -ENOMEM;
	}
	/* The whole log within the context just established. */
	ret = nvme_admin_get_log_rae(ctrl, NVME_NSID_ALL,
				     NVME_LOG_PERSISTENT_EVENT,
				     NVME_PEVENT_LOG_READ, false, log,
				     (size_t)tll, 0);
	if (!ret) {
		const struct nvme_persistent_event_log *h2 = (const void *)log;

		if (h2->gen_number != hdr->gen_number)
			ret = -EAGAIN; /* the context changed under us */
	}
	kfree(hdr);
	if (ret) {
		kfree(log);
		return ret;
	}
	*buf = log;
	*len = (size_t)tll;
	return 0;
}

static const char *pel_type_name(uint8_t t)
{
	switch (t) {
	case NVME_PEL_SMART_HEALTH_EVENT:
		return "SMART/health snapshot";
	case NVME_PEL_FW_COMMIT_EVENT:
		return "firmware commit";
	case NVME_PEL_TIMESTAMP_EVENT:
		return "timestamp change";
	case NVME_PEL_POWER_ON_RESET_EVENT:
		return "power-on or reset";
	case NVME_PEL_NSS_HW_ERROR_EVENT:
		return "NVM subsystem hardware error";
	case NVME_PEL_CHANGE_NS_EVENT:
		return "change namespace";
	case NVME_PEL_FORMAT_START_EVENT:
		return "format NVM start";
	case NVME_PEL_FORMAT_COMPLETION_EVENT:
		return "format NVM completion";
	case 0x09:
		return "sanitize start";
	case 0x0a:
		return "sanitize completion";
	case 0x0b:
		return "set feature";
	case 0x0c:
		return "telemetry log create";
	case 0x0d:
		return "thermal excursion";
	case 0xde:
		return "vendor specific";
	case 0xdf:
		return "TCG defined";
	default:
		return "reserved";
	}
}

long nvme_admin_show_pevent(struct nvme_ctrl *ctrl, char *buf, long cap)
{
	struct adm_sbuf sb = { buf, cap, 0 };
	const struct nvme_persistent_event_log *h;
	void *log = NULL;
	size_t len = 0, off;
	uint32_t tnev;
	int ret;
	char sn[21], mn[41];

	if (!(ctrl->lpa & NVME_CTRL_LPA_PERSISTENT_EVENT)) {
		asb(&sb, "persistent event log not supported\n");
		return sb.pos;
	}
	ret = nvme_get_persistent_event_log(ctrl,
					    NVME_PEVENT_LOG_EST_CTX_AND_READ,
					    &log, &len);
	if (ret) {
		asb(&sb, "persistent event log: error %d\n", ret);
		return sb.pos;
	}
	h = log;
	tnev = le32_to_cpu(h->tnev);
	mm_memcpy(sn, h->sn, 20);
	sn[20] = 0;
	mm_memcpy(mn, h->mn, 40);
	mn[40] = 0;
	asb(&sb, "events %u, log length %llu, revision %u, header length %u, "
		 "generation %u\n",
	    (unsigned)tnev, (unsigned long long)le64_to_cpu(h->tll),
	    (unsigned)h->rv, (unsigned)le16_to_cpu(h->lhl),
	    (unsigned)le16_to_cpu(h->gen_number));
	asb(&sb, "timestamp %llu, power cycles %llu, vid 0x%04x ssvid 0x%04x\n",
	    (unsigned long long)(le64_to_cpu(h->ts) & 0xffffffffffffULL),
	    (unsigned long long)le64_to_cpu(h->pcc),
	    (unsigned)le16_to_cpu(h->vid), (unsigned)le16_to_cpu(h->ssvid));
	asb(&sb, "serial '%s' model '%s'\n", sn, mn);

	/* Events: header (ehl + 3 bytes), then el bytes (vendor info and
	 * event data). */
	off = NVME_LOG_HDR_SIZE;
	for (uint32_t i = 0; i < tnev; i++) {
		const struct nvme_persistent_event_entry *e;
		size_t step;

		if (off + sizeof(*e) > len)
			break;
		e = (const void *)((const uint8_t *)log + off);
		step = (size_t)e->ehl + 3 + le16_to_cpu(e->el);
		if (step < sizeof(*e) || off + step > len)
			break;
		asb(&sb, "#%-4u type 0x%02x %-28s rev %u cntlid %u ts %llu "
			 "length %u\n",
		    (unsigned)i, (unsigned)e->etype, pel_type_name(e->etype),
		    (unsigned)e->etype_rev, (unsigned)le16_to_cpu(e->cntlid),
		    (unsigned long long)(le64_to_cpu(e->ets) &
					 0xffffffffffffULL),
		    (unsigned)le16_to_cpu(e->el));
		off += step;
		if (i >= 255) {
			asb(&sb, "... (%u more)\n", (unsigned)(tnev - i - 1));
			break;
		}
	}
	kfree(log);
	/* Leave no reporting context behind. */
	{
		void *dummy = NULL;
		size_t dlen = 0;

		(void)nvme_get_persistent_event_log(ctrl,
						    NVME_PEVENT_LOG_RELEASE_CTX,
						    &dummy, &dlen);
	}
	return sb.pos;
}

/* ==========================================================================
 * Commands supported and effects
 * ========================================================================== */

static void effects_bits(struct adm_sbuf *sb, uint32_t e)
{
	static const char *const cse[] = { "", " cse=ns", " cse=all", " cse=?3",
					   " cse=?4", " cse=?5", " cse=?6",
					   " cse=?7" };

	asb(sb, "0x%08x%s%s%s%s%s%s%s\n", (unsigned)e,
	    (e & NVME_CMD_EFFECTS_LBCC) ? " lbcc" : "",
	    (e & NVME_CMD_EFFECTS_NCC) ? " ncc" : "",
	    (e & NVME_CMD_EFFECTS_NIC) ? " nic" : "",
	    (e & NVME_CMD_EFFECTS_CCC) ? " ccc" : "",
	    cse[(e & NVME_CMD_EFFECTS_CSE_MASK) >> 16],
	    (e & NVME_CMD_EFFECTS_UUID_SEL) ? " uuid" : "",
	    (e & NVME_CMD_EFFECTS_CSER_MASK) ? " cser" : "");
}

long nvme_admin_show_effects(struct nvme_ctrl *ctrl, char *buf, long cap)
{
	struct adm_sbuf sb = { buf, cap, 0 };
	const struct nvme_effects_log *log = ctrl->effects;

	if (!log) {
		asb(&sb, "no commands supported and effects log\n");
		return sb.pos;
	}
	asb(&sb, "admin commands:\n");
	for (int op = 0; op < 256; op++) {
		uint32_t e = le32_to_cpu(log->acs[op]);
		const char *name;

		if (!(e & NVME_CMD_EFFECTS_CSUPP))
			continue;
		name = nvme_get_admin_opcode_str((uint8_t)op);
		asb(&sb, "  0x%02x %-28s ", (unsigned)op, name ? name : "?");
		effects_bits(&sb, e);
	}
	asb(&sb, "I/O commands:\n");
	for (int op = 0; op < 256; op++) {
		uint32_t e = le32_to_cpu(log->iocs[op]);
		const char *name;

		if (!(e & NVME_CMD_EFFECTS_CSUPP))
			continue;
		name = nvme_get_opcode_str((uint8_t)op);
		asb(&sb, "  0x%02x %-28s ", (unsigned)op, name ? name : "?");
		effects_bits(&sb, e);
	}
	return sb.pos;
}

/* ==========================================================================
 * Security send / receive
 * ========================================================================== */

int nvme_security_send_recv(struct nvme_ctrl *ctrl, bool send, uint8_t secp,
			    uint16_t spsp, void *buf, size_t len)
{
	struct nvme_command c;

	might_sleep();
	if (!nvme_admin_usable(ctrl))
		return -EINVAL;
	if (!(ctrl->oacs & NVME_CTRL_OACS_SEC_SUPP))
		return -EOPNOTSUPP;
	if ((len && !buf) || len > nvme_admin_max_xfer(ctrl))
		return -EINVAL;
	mm_memset(&c, 0, sizeof(c));
	c.common.opcode = send ? nvme_admin_security_send :
				 nvme_admin_security_recv;
	c.common.nsid = 0;
	c.common.cdw10 = cpu_to_le32(((uint32_t)secp) << 24 |
				     ((uint32_t)spsp) << 8);
	c.common.cdw11 = cpu_to_le32((uint32_t)len);
	return __nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, buf,
				      (unsigned int)len, NVME_QID_ANY,
				      NVME_SUBMIT_AT_HEAD);
}
