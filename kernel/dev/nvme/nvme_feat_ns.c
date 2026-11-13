// LikeOS - NVMe namespace features: directives (identify, streams), write
// streams, Flexible Data Placement, endurance groups and NVM sets
//
// Streams (nvme_core.streams=1): the streams directive is enabled
// subsystem-wide when the controller supports directives and has at least
// as many streams as there are write-lifetime hints (short, medium, long,
// extreme); a write with a lifetime hint then carries the streams
// directive with stream identifier hint - 1.  Each namespace's stream
// write size (SWS) and granularity (SGS) become its preferred I/O size and
// deallocation granularity.
//
// Flexible Data Placement: when the controller reports FDP support and the
// namespace's endurance group has FDP enabled, the namespace's placement
// handles (Reclaim Unit Handle Status) become its write streams: write
// stream N goes to placement identifier N - 1 with the data placement
// directive.  The reclaim unit size becomes the write stream granularity.
// The FDP logs (configurations, reclaim unit handle usage, statistics,
// events) and the FDP events feature are offered to kernel callers and the
// ioctl layer.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

/* Write lifetime hints (RWH_WRITE_LIFE_*): NOT_SET 0, NONE 1, SHORT 2,
 * MEDIUM 3, LONG 4, EXTREME 5.  Streams are used when the device has one
 * per real hint. */
#define NVME_RWH_WRITE_LIFE_NOT_SET 0
#define NVME_RWH_WRITE_LIFE_NONE 1
#define NVME_MAX_WRITE_HINTS 5

/* ==========================================================================
 * Directives
 * ========================================================================== */

static int nvme_toggle_streams(struct nvme_ctrl *ctrl, bool enable)
{
	struct nvme_command c;

	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_send;
	c.directive.nsid = cpu_to_le32(NVME_NSID_ALL);
	c.directive.doper = NVME_DIR_SND_ID_OP_ENABLE;
	c.directive.dtype = NVME_DIR_IDENTIFY;
	c.directive.tdtype = NVME_DIR_STREAMS;
	c.directive.endir = enable ? NVME_DIR_ENDIR : 0;
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, 0);
}

int nvme_disable_streams(struct nvme_ctrl *ctrl)
{
	might_sleep();
	nvme_set_ctrl_nr_streams(ctrl, 0);
	return nvme_toggle_streams(ctrl, false);
}

int nvme_get_stream_params(struct nvme_ctrl *ctrl,
			   struct streams_directive_params *s, uint32_t nsid)
{
	struct nvme_command c;

	might_sleep();
	kmemset(s, 0, sizeof(*s));
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_recv;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.numd = cpu_to_le32(nvme_bytes_to_numd(sizeof(*s)));
	c.directive.doper = NVME_DIR_RCV_ST_OP_PARAM;
	c.directive.dtype = NVME_DIR_STREAMS;
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, s, sizeof(*s));
}

/*
 * Configure the streams directive (called by the core while identifying
 * the controller): enable it when the controller supports directives and
 * nvme_core.streams asks for it, and use it when enough streams exist.
 * 0 / NVMe status / -errno.
 */
int nvme_enable_streams(struct nvme_ctrl *ctrl)
{
	struct streams_directive_params s;
	int ret;

	might_sleep();
	nvme_set_ctrl_nr_streams(ctrl, 0);
	if (!(ctrl->oacs & NVME_CTRL_OACS_DIRECTIVES))
		return 0;
	if (!nvme_core_param.streams)
		return 0;

	ret = nvme_toggle_streams(ctrl, true);
	if (ret)
		return ret;
	ret = nvme_get_stream_params(ctrl, &s, NVME_NSID_ALL);
	if (ret)
		goto out_disable;

	ctrl->nssa = le16_to_cpu(s.nssa);
	if (ctrl->nssa < NVME_MAX_WRITE_HINTS - 1) {
		nvme_ctrl_info(ctrl, "too few streams (%u) available\n",
			       ctrl->nssa);
		goto out_disable;
	}
	nvme_set_ctrl_nr_streams(ctrl, ctrl->nssa < NVME_MAX_WRITE_HINTS - 1 ?
					       ctrl->nssa :
					       NVME_MAX_WRITE_HINTS - 1);
	nvme_ctrl_info(ctrl, "Using %u streams\n", nvme_ctrl_nr_streams(ctrl));
	return 0;

out_disable:
	nvme_disable_streams(ctrl);
	return ret;
}

void nvme_ns_streams_update(struct nvme_ns *ns)
{
	struct streams_directive_params s;
	struct nvme_ns_head *head = ns->head;

	head->sws = 0;
	head->sgs = 0;
	if (!nvme_ctrl_nr_streams(ns->ctrl))
		return;
	if (nvme_get_stream_params(ns->ctrl, &s, head->ns_id))
		return;
	head->sws = le32_to_cpu(s.sws);
	head->sgs = le16_to_cpu(s.sgs);
}

void nvme_assign_write_stream(struct nvme_ctrl *ctrl, struct blk_request *rq,
			      uint16_t *control, uint32_t *dsmgmt)
{
	uint32_t streamid = rq->write_hint;

	if (streamid == NVME_RWH_WRITE_LIFE_NOT_SET ||
	    streamid == NVME_RWH_WRITE_LIFE_NONE)
		return;
	streamid--;
	if (WARN_ON_ONCE(streamid > nvme_ctrl_nr_streams(ctrl)))
		return;
	*control |= NVME_RW_DTYPE_STREAMS;
	*dsmgmt |= streamid << NVME_RW_DSPEC_SHIFT;
}

blk_status_t nvme_write_stream_dspec(struct nvme_ns *ns, uint16_t stream,
				     uint16_t *control, uint32_t *dsmgmt)
{
	struct nvme_ns_head *head = ns->head;

	if (!stream)
		return BLK_STS_OK;
	if (WARN_ON_ONCE(stream > head->nr_plids || !head->plids))
		return BLK_STS_INVAL;
	*dsmgmt |= (uint32_t)head->plids[stream - 1] << NVME_RW_DSPEC_SHIFT;
	*control |= NVME_RW_DTYPE_DPLCMT;
	return BLK_STS_OK;
}

int nvme_identify_directives(struct nvme_ctrl *ctrl, uint32_t nsid,
			     struct nvme_id_directives *id)
{
	struct nvme_command c;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_recv;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.numd = cpu_to_le32(nvme_bytes_to_numd(sizeof(*id)));
	c.directive.doper = NVME_DIR_RCV_ID_OP_PARAM;
	c.directive.dtype = NVME_DIR_IDENTIFY;
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));
}

int nvme_get_streams_status(struct nvme_ctrl *ctrl, uint32_t nsid, void *buf,
			    size_t len)
{
	struct nvme_command c;

	might_sleep();
	if (len < sizeof(struct nvme_streams_directive_status) || (len & 3) ||
	    len > 0x20000)
		return -EINVAL;
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_recv;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.numd = cpu_to_le32(nvme_bytes_to_numd(len));
	c.directive.doper = NVME_DIR_RCV_ST_OP_STATUS;
	c.directive.dtype = NVME_DIR_STREAMS;
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, buf, (unsigned int)len);
}

int nvme_alloc_streams(struct nvme_ctrl *ctrl, uint32_t nsid, uint16_t nsr,
		       uint16_t *granted)
{
	union nvme_result res;
	struct nvme_command c;
	int ret;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_recv;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.doper = NVME_DIR_RCV_ST_OP_RESOURCE;
	c.directive.dtype = NVME_DIR_STREAMS;
	/* cdw12 bits 15:0: number of streams requested. */
	c.common.cdw12 = cpu_to_le32(nsr);
	kmemset(&res, 0, sizeof(res));
	ret = __nvme_submit_sync_cmd(ctrl->admin_q, &c, &res, NULL, 0,
				     NVME_QID_ANY, 0);
	if (!ret && granted)
		*granted = (uint16_t)(le32_to_cpu(res.u32) & 0xffff);
	return ret;
}

int nvme_release_stream_id(struct nvme_ctrl *ctrl, uint32_t nsid, uint16_t sid)
{
	struct nvme_command c;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_send;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.doper = NVME_DIR_SND_ST_OP_REL_ID;
	c.directive.dtype = NVME_DIR_STREAMS;
	c.directive.dspec = cpu_to_le16(sid);
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, 0);
}

int nvme_release_stream_resources(struct nvme_ctrl *ctrl, uint32_t nsid)
{
	struct nvme_command c;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	c.directive.opcode = nvme_admin_directive_send;
	c.directive.nsid = cpu_to_le32(nsid);
	c.directive.doper = NVME_DIR_SND_ST_OP_REL_RSC;
	c.directive.dtype = NVME_DIR_STREAMS;
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, 0);
}

/* ==========================================================================
 * Flexible Data Placement
 * ========================================================================== */

/* The largest FDP log the driver reads (configurations can be large but
 * not unbounded). */
#define NVME_FDP_LOG_MAX (1u << 20)

/* Read a log whose first dword-aligned header says its size: header,
 * then the whole log.  *log kalloc'd. */
static int nvme_get_sized_log(struct nvme_ctrl *ctrl, uint8_t lid, uint8_t lsp,
			      uint16_t lsi, size_t hdr_len,
			      size_t (*size_of)(const void *hdr), void **log,
			      size_t *len)
{
	uint8_t hdr[64];
	size_t size;
	void *buf;
	int ret;

	BUG_ON(hdr_len > sizeof(hdr));
	kmemset(hdr, 0, sizeof(hdr));
	ret = nvme_get_log_lsi(ctrl, 0, lid, lsp, lsi, NVME_CSI_NVM, hdr,
			       hdr_len, 0);
	if (ret)
		return ret;
	size = size_of(hdr);
	if (size < hdr_len)
		size = hdr_len;
	if (size > NVME_FDP_LOG_MAX) {
		nvme_ctrl_warn(ctrl, "log %#x too large: %llu bytes\n", lid,
			       (unsigned long long)size);
		return -E2BIG;
	}
	size = (size + 3) & ~(size_t)3;
	buf = nvme_kzalloc(size);
	if (!buf)
		return -ENOMEM;
	ret = nvme_get_log_lsi(ctrl, 0, lid, lsp, lsi, NVME_CSI_NVM, buf, size,
			       0);
	if (ret) {
		kfree(buf);
		return ret;
	}
	*log = buf;
	*len = size;
	return 0;
}

static size_t nvme_fdp_config_size(const void *hdr)
{
	return le32_to_cpu(((const struct nvme_fdp_config_log *)hdr)->sze);
}

static size_t nvme_fdp_ruhu_size(const void *hdr)
{
	return sizeof(struct nvme_fdp_ruhu_log) +
	       (size_t)le16_to_cpu(((const struct nvme_fdp_ruhu_log *)hdr)->nruh) *
		       sizeof(struct nvme_fdp_ruhu_desc);
}

static size_t nvme_fdp_events_size(const void *hdr)
{
	return sizeof(struct nvme_fdp_events_log) +
	       (size_t)le32_to_cpu(((const struct nvme_fdp_events_log *)hdr)->n) *
		       sizeof(struct nvme_fdp_event);
}

int nvme_fdp_config_log(struct nvme_ctrl *ctrl, uint16_t endgid, void **log,
			size_t *len)
{
	might_sleep();
	return nvme_get_sized_log(ctrl, NVME_LOG_FDP_CONFIGS, 0, endgid,
				  sizeof(struct nvme_fdp_config_log),
				  nvme_fdp_config_size, log, len);
}

int nvme_fdp_ruhu_log(struct nvme_ctrl *ctrl, uint16_t endgid, void **log,
		      size_t *len)
{
	might_sleep();
	return nvme_get_sized_log(ctrl, NVME_LOG_RUHU, 0, endgid,
				  sizeof(struct nvme_fdp_ruhu_log),
				  nvme_fdp_ruhu_size, log, len);
}

int nvme_fdp_events_log(struct nvme_ctrl *ctrl, uint16_t endgid, bool host,
			void **log, size_t *len)
{
	might_sleep();
	return nvme_get_sized_log(ctrl, NVME_LOG_FDP_EVENTS,
				  host ? NVME_FDP_EVENTS_LSP_HOST : 0, endgid,
				  sizeof(struct nvme_fdp_events_log),
				  nvme_fdp_events_size, log, len);
}

int nvme_fdp_stats_log(struct nvme_ctrl *ctrl, uint16_t endgid,
		       struct nvme_fdp_stats_log *log)
{
	might_sleep();
	return nvme_get_log_lsi(ctrl, 0, NVME_LOG_FDP_STATS, 0, endgid,
				NVME_CSI_NVM, log, sizeof(*log), 0);
}

/*
 * The reclaim unit size of FDP configuration `fdp_idx' of a configurations
 * log (`size' bytes, header included).  0 with *runs set; 1 when the index
 * or the descriptor list is invalid or the configuration has more than one
 * reclaim group (not supported).  Pure (host-tested).
 */
int nvme_fdp_config_runs(const void *log, size_t size, unsigned int fdp_idx,
			 uint64_t *runs);
int nvme_fdp_config_runs(const void *log, size_t size, unsigned int fdp_idx,
			 uint64_t *runs)
{
	const struct nvme_fdp_config_log *h =
		(const struct nvme_fdp_config_log *)log;
	const uint8_t *p, *end;
	const struct nvme_fdp_config_desc *desc;
	unsigned int n;

	if (size < sizeof(*h) + sizeof(*desc))
		return 1;
	n = le16_to_cpu(h->numfdpc) + 1u;
	if (fdp_idx >= n)
		return 1;
	p = (const uint8_t *)(h + 1);
	end = (const uint8_t *)log + size;
	for (unsigned int i = 0; i < fdp_idx; i++) {
		uint16_t dsze;

		desc = (const struct nvme_fdp_config_desc *)p;
		if ((size_t)(end - p) < sizeof(*desc))
			return 1;
		dsze = le16_to_cpu(desc->dsze);
		if (dsze < sizeof(*desc))
			return 1;
		p += dsze;
		if (p >= end)
			return 1;
	}
	desc = (const struct nvme_fdp_config_desc *)p;
	if ((size_t)(end - p) < sizeof(*desc))
		return 1;
	if (le32_to_cpu(desc->nrg) > 1)
		return 1;
	*runs = le64_to_cpu(desc->runs);
	return 0;
}

static int nvme_query_fdp_granularity(struct nvme_ctrl *ctrl,
				      struct nvme_ns_info *info,
				      uint8_t fdp_idx)
{
	void *log = NULL;
	size_t size = 0;
	uint64_t runs = 0;
	int ret;

	ret = nvme_fdp_config_log(ctrl, info->endgid, &log, &size);
	if (ret) {
		nvme_ctrl_warn(ctrl,
			       "FDP configs log status:0x%x endgid:%u\n",
			       (unsigned int)ret, info->endgid);
		/* Too large: proceed without FDP streams. */
		return ret == -E2BIG ? 0 : ret;
	}
	if (nvme_fdp_config_runs(log, size, fdp_idx, &runs)) {
		nvme_ctrl_warn(ctrl,
			       "FDP config %u unusable (index, descriptors or reclaim groups)\n",
			       fdp_idx);
	} else {
		info->runs = runs;
	}
	kfree(log);
	return 0;
}

int nvme_fdp_ruh_status(struct nvme_ns *ns, void **buf, size_t *len)
{
	struct nvme_fdp_ruh_status *ruhs;
	struct nvme_command c;
	/* Room for the most handles a namespace can have (126). */
	size_t size = sizeof(*ruhs) + 126 * sizeof(struct nvme_fdp_ruh_status_desc);
	int ret;

	might_sleep();
	ruhs = (struct nvme_fdp_ruh_status *)nvme_kzalloc(size);
	if (!ruhs)
		return -ENOMEM;
	kmemset(&c, 0, sizeof(c));
	c.imr.opcode = nvme_cmd_io_mgmt_recv;
	c.imr.nsid = cpu_to_le32(ns->head->ns_id);
	c.imr.mo = NVME_IO_MGMT_RECV_MO_RUHS;
	c.imr.numd = cpu_to_le32(nvme_bytes_to_numd(size));
	ret = nvme_submit_sync_cmd(ns->queue, &c, ruhs, (unsigned int)size);
	if (ret) {
		kfree(ruhs);
		return ret;
	}
	*buf = ruhs;
	*len = size;
	return 0;
}

int nvme_query_fdp_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ns_head *head = ns->head;
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_fdp_ruh_status *ruhs;
	struct nvme_fdp_config fdp;
	uint32_t result = 0;
	size_t size = 0;
	uint16_t n;
	int ret;

	might_sleep();
	/* The FDP configuration is static for the namespace's lifetime. */
	if (head->nr_plids)
		return 0;

	ret = nvme_get_features(ctrl, NVME_FEAT_FDP, info->endgid, NULL, 0,
				&result);
	if (ret) {
		nvme_ctrl_warn(ctrl, "FDP get feature status:0x%x\n",
			       (unsigned int)ret);
		return ret;
	}
	kmemcpy(&fdp, &result, sizeof(fdp));
	if (!(fdp.flags & FDPCFG_FDPE))
		return 0;

	ret = nvme_query_fdp_granularity(ctrl, info, fdp.fdpcidx);
	if (!info->runs)
		return ret;

	ret = nvme_fdp_ruh_status(ns, (void **)&ruhs, &size);
	if (ret) {
		nvme_ctrl_warn(ctrl, "FDP io-mgmt status:0x%x\n",
			       (unsigned int)ret);
		return ret;
	}

	n = le16_to_cpu(ruhs->nruhsd);
	if (n > (size - sizeof(*ruhs)) / sizeof(ruhs->ruhsd[0]))
		n = (uint16_t)((size - sizeof(*ruhs)) / sizeof(ruhs->ruhsd[0]));
	if (!n)
		goto free;

	head->plids = (uint16_t *)nvme_kzalloc((size_t)n * sizeof(uint16_t));
	if (!head->plids) {
		nvme_ctrl_warn(ctrl,
			       "failed to allocate %u FDP placement IDs\n", n);
		ret = -ENOMEM;
		goto free;
	}
	for (uint16_t i = 0; i < n; i++)
		head->plids[i] = le16_to_cpu(ruhs->ruhsd[i].pid);
	/* Published last: nr_plids != 0 means plids is valid. */
	__atomic_store_n(&head->nr_plids, n, __ATOMIC_RELEASE);
free:
	kfree(ruhs);
	return ret;
}

int nvme_fdp_ruh_update(struct nvme_ns *ns, const uint16_t *pids,
			unsigned int nr)
{
	struct nvme_command c;
	__le16 *list;
	int ret;

	might_sleep();
	if (!nr || nr > 128 || !pids)
		return -EINVAL;
	list = (__le16 *)nvme_kzalloc((size_t)nr * sizeof(__le16));
	if (!list)
		return -ENOMEM;
	for (unsigned int i = 0; i < nr; i++)
		list[i] = cpu_to_le16(pids[i]);
	kmemset(&c, 0, sizeof(c));
	c.ims.opcode = nvme_cmd_io_mgmt_send;
	c.ims.nsid = cpu_to_le32(ns->head->ns_id);
	c.ims.mo = NVME_IO_MGMT_SEND_MO_RUHU;
	c.ims.mos = cpu_to_le16((uint16_t)(nr - 1));
	ret = nvme_submit_sync_cmd(ns->queue, &c, list,
				   (unsigned int)(nr * sizeof(__le16)));
	kfree(list);
	return ret;
}

int nvme_fdp_set_events(struct nvme_ctrl *ctrl, uint32_t nsid, uint16_t phndl,
			const uint8_t *types, unsigned int nr, bool enable)
{
	struct nvme_command c;
	uint8_t *buf;
	int ret;

	might_sleep();
	if (!nr || nr > 255 || !types)
		return -EINVAL;
	buf = (uint8_t *)nvme_kzalloc(nr);
	if (!buf)
		return -ENOMEM;
	kmemcpy(buf, types, nr);
	kmemset(&c, 0, sizeof(c));
	c.features.opcode = nvme_admin_set_features;
	c.features.nsid = cpu_to_le32(nsid);
	c.features.fid = cpu_to_le32(NVME_FEAT_FDP_EVENTS);
	c.features.dword11 = cpu_to_le32(
		(uint32_t)phndl | (nr << NVME_FEAT_FDP_EVENTS_NOET_SHIFT));
	c.features.dword12 =
		cpu_to_le32(enable ? NVME_FEAT_FDP_EVENTS_ENABLE : 0);
	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, buf, nr);
	kfree(buf);
	return ret;
}

/* ==========================================================================
 * Endurance groups and NVM sets
 * ========================================================================== */

int nvme_endurance_group_log(struct nvme_ctrl *ctrl, uint16_t endgid,
			     struct nvme_endurance_group_log *log)
{
	might_sleep();
	if (!(nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_ENDURANCE_GROUPS))
		return -EOPNOTSUPP;
	return nvme_get_log_lsi(ctrl, 0, NVME_LOG_ENDURANCE_GROUP, 0, endgid,
				NVME_CSI_NVM, log, sizeof(*log), 0);
}

static int nvme_identify_cnssid(struct nvme_ctrl *ctrl, uint8_t cns,
				uint16_t cnssid, void *buf)
{
	struct nvme_command c;

	kmemset(&c, 0, sizeof(c));
	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = cns;
	c.identify.cnssid = cpu_to_le16(cnssid);
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, buf,
				    NVME_IDENTIFY_DATA_SIZE);
}

int nvme_identify_endgrp_list(struct nvme_ctrl *ctrl, uint16_t start,
			      struct nvme_id_endurance_group_list *list)
{
	might_sleep();
	if (!(nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_ENDURANCE_GROUPS))
		return -EOPNOTSUPP;
	if (!nvme_id_cns_ok(ctrl, NVME_ID_CNS_ENDGRP_LIST))
		return -EOPNOTSUPP;
	return nvme_identify_cnssid(ctrl, NVME_ID_CNS_ENDGRP_LIST, start, list);
}

int nvme_identify_nvmset_list(struct nvme_ctrl *ctrl, uint16_t start,
			      struct nvme_id_nvmset_list *list)
{
	might_sleep();
	if (!(nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_NVM_SETS))
		return -EOPNOTSUPP;
	if (!nvme_id_cns_ok(ctrl, NVME_ID_CNS_NVMSET_LIST))
		return -EOPNOTSUPP;
	return nvme_identify_cnssid(ctrl, NVME_ID_CNS_NVMSET_LIST, start, list);
}

int nvme_nvmset_predictable_lat_log(struct nvme_ctrl *ctrl, uint16_t setid,
				    struct nvme_nvmset_predictable_lat_log *log)
{
	might_sleep();
	if (!(nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_PLM))
		return -EOPNOTSUPP;
	return nvme_get_log_lsi(ctrl, 0, NVME_LOG_PLM, 0, setid, NVME_CSI_NVM,
				log, sizeof(*log), 0);
}
