// LikeOS - NVMe Zoned Namespaces: zone geometry and limits, zone report,
// zone management send/receive, zone append support
//
// A ZNS namespace is a block device of equally sized sequential-write-
// required zones (the size a power of two, per LBA format).  The driver
// takes it only when Zone Append is supported (else the namespace is
// forced read-only, as zoned writers need append), no zone operation
// characteristic is required, and the zone size is a power of two.  The
// controller's append limit (ZASL) is read once, lazily, with the first
// zoned namespace.  Reports are read in buffers as large as one transfer
// may be, partial reports, walking from the requested zone.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

static int nvme_set_max_append(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl_zns *id;
	int status;

	id = (struct nvme_id_ctrl_zns *)nvme_kzalloc(sizeof(*id));
	if (!id)
		return -ENOMEM;
	status = nvme_identify(ctrl, NVME_ID_CNS_CS_CTRL, NVME_CSI_ZNS, 0, 0,
			       id);
	if (status) {
		kfree(id);
		return status;
	}
	/* ZASL: 2^n units of CAP.MPSMIN, 0 = MDTS. */
	if (id->zasl) {
		uint32_t shift = id->zasl + 12u +
				 (uint32_t)NVME_CAP_MPSMIN(ctrl->cap) - 9u;

		ctrl->max_zone_append = shift >= 32 ? 0xffffffffu : 1u << shift;
	} else {
		ctrl->max_zone_append = ctrl->max_hw_sectors;
	}
	kfree(id);
	return 0;
}

bool nvme_zns_append_supported(struct nvme_ns *ns)
{
	const struct nvme_effects_log *log = ns->head->effects;

	return log && (le32_to_cpu(log->iocs[nvme_cmd_zone_append]) &
		       NVME_CMD_EFFECTS_CSUPP);
}

int nvme_query_zone_info(struct nvme_ns *ns, unsigned int lbaf,
			 struct blk_limits *lim)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_id_ns_zns *id;
	uint64_t zone_size;
	int status;

	might_sleep();
	BUG_ON(lbaf >= 64);
	/* Zoned writers need Zone Append. */
	if (nvme_zns_append_supported(ns)) {
		if (nvme_test_and_clear_bit(NVME_NS_FORCE_RO, &ns->flags))
			nvme_ctrl_warn(ctrl,
				       "Zone Append supported for zoned namespace:%u. Remove read-only mode\n",
				       ns->head->ns_id);
	} else {
		nvme_set_bit(NVME_NS_FORCE_RO, &ns->flags);
		nvme_ctrl_warn(ctrl,
			       "Zone Append not supported for zoned namespace:%u. Forcing to read-only mode\n",
			       ns->head->ns_id);
	}

	/* The controller's append limit, read with the first zoned
	 * namespace. */
	if (!ctrl->max_zone_append) {
		status = nvme_set_max_append(ctrl);
		if (status)
			return status;
	}

	id = (struct nvme_id_ns_zns *)nvme_kzalloc(sizeof(*id));
	if (!id)
		return -ENOMEM;
	status = nvme_identify(ctrl, NVME_ID_CNS_CS_NS, NVME_CSI_ZNS,
			       ns->head->ns_id, 0, id);
	if (status)
		goto free_data;

	/* No zone operation characteristics (variable capacity, active
	 * excursions) are handled. */
	if (id->zoc) {
		nvme_ctrl_warn(ctrl,
			       "zone operations:%x not supported for namespace:%u\n",
			       le16_to_cpu(id->zoc), ns->head->ns_id);
		status = -ENODEV;
		goto free_data;
	}

	zone_size = le64_to_cpu(id->lbafe[lbaf].zsze);
	if (!nvme_is_power_of_2(zone_size)) {
		nvme_ctrl_warn(ctrl,
			       "invalid zone size: %llu for namespace: %u\n",
			       (unsigned long long)zone_size, ns->head->ns_id);
		status = -ENODEV;
		goto free_data;
	}
	ns->head->zsze_pending = zone_size;
	/* MOR/MAR are 0's based; all ones (no limit) wraps to 0 = none. */
	lim->max_open_zones = le32_to_cpu(id->mor) + 1u;
	lim->max_active_zones = le32_to_cpu(id->mar) + 1u;
free_data:
	kfree(id);
	return status;
}

void nvme_update_zone_info(struct nvme_ns *ns, struct blk_limits *lim)
{
	struct nvme_ns_head *head = ns->head;
	uint64_t zs = head->zsze_pending;

	BUG_ON(!nvme_is_power_of_2(zs));
	lim->features |= BLK_FEAT_ZONED;
	lim->max_hw_zone_append_sectors = (uint32_t)nvme_ns_sect512_to_disk(
		head, ns->ctrl->max_zone_append);
	lim->max_zone_append_sectors = lim->max_hw_zone_append_sectors;
	/* Zone size in logical blocks (the disk's units). */
	head->zsze = zs;
	lim->chunk_sectors = zs > 0xffffffffULL ? 0xffffffffu : (uint32_t)zs;
	lim->zone_write_granularity = lim->logical_block_size;
}

/*
 * One zone descriptor into the block layer's zone (512-byte sectors).
 * Only sequential-write-required zones exist in ZNS.  Pure (host-tested).
 */
int nvme_zone_parse_desc(const struct nvme_ns_head *head,
			 const struct nvme_zone_descriptor *entry,
			 struct blk_zone *zone);
int nvme_zone_parse_desc(const struct nvme_ns_head *head,
			 const struct nvme_zone_descriptor *entry,
			 struct blk_zone *zone)
{
	if ((entry->zt & NVME_ZONE_TYPE_MASK) != NVME_ZONE_TYPE_SEQWRITE_REQ)
		return -EINVAL;
	kmemset(zone, 0, sizeof(*zone));
	zone->type = BLK_ZONE_TYPE_SEQWRITE_REQ;
	zone->cond = (uint8_t)(entry->zs >> NVME_ZONE_STATE_SHIFT);
	zone->len = nvme_lba_to_sect(head, head->zsze);
	zone->capacity = nvme_lba_to_sect(head, le64_to_cpu(entry->zcap));
	zone->start = nvme_lba_to_sect(head, le64_to_cpu(entry->zslba));
	if (zone->cond == BLK_ZONE_COND_FULL)
		zone->wp = zone->start + zone->len;
	else
		zone->wp = nvme_lba_to_sect(head, le64_to_cpu(entry->wp));
	return 0;
}

static void *nvme_zns_alloc_report_buffer(struct nvme_ns *ns,
					  unsigned int nr_zones, size_t *buflen)
{
	const struct blk_limits *lim = &ns->disk->limits;
	const size_t min_bufsize = sizeof(struct nvme_zone_report) +
				   sizeof(struct nvme_zone_descriptor);
	uint64_t zones_on_disk = ns->head->zsze ?
					 ns->disk->capacity / ns->head->zsze :
					 0;
	size_t bufsize, max_xfer;
	void *buf;

	if (zones_on_disk < nr_zones)
		nr_zones = (unsigned int)zones_on_disk;
	bufsize = sizeof(struct nvme_zone_report) +
		  (size_t)nr_zones * sizeof(struct nvme_zone_descriptor);
	/* At most one transfer and what the segments can map. */
	max_xfer = (size_t)lim->max_hw_sectors * lim->logical_block_size;
	if (max_xfer && bufsize > max_xfer)
		bufsize = max_xfer;
	if (lim->max_segments &&
	    bufsize > (size_t)lim->max_segments * NVME_CTRL_PAGE_SIZE)
		bufsize = (size_t)lim->max_segments * NVME_CTRL_PAGE_SIZE;

	while (bufsize >= min_bufsize) {
		buf = kalloc(bufsize);
		if (buf) {
			*buflen = bufsize;
			return buf;
		}
		bufsize >>= 1;
	}
	return NULL;
}

int nvme_zns_report(struct nvme_ns *ns, uint64_t slba, uint8_t zrasf,
		    bool partial, void *buf, size_t buflen)
{
	struct nvme_command c;

	might_sleep();
	if (buflen < sizeof(struct nvme_zone_report) || (buflen & 3))
		return -EINVAL;
	kmemset(&c, 0, sizeof(c));
	c.zmr.opcode = nvme_cmd_zone_mgmt_recv;
	c.zmr.nsid = cpu_to_le32(ns->head->ns_id);
	c.zmr.slba = cpu_to_le64(slba);
	c.zmr.numd = cpu_to_le32(nvme_bytes_to_numd(buflen));
	c.zmr.zra = NVME_ZRA_ZONE_REPORT;
	c.zmr.zrasf = zrasf;
	c.zmr.pr = partial ? NVME_REPORT_ZONE_PARTIAL : 0;
	kmemset(buf, 0, buflen);
	return nvme_submit_sync_cmd(ns->queue, &c, buf, (unsigned int)buflen);
}

int nvme_ns_report_zones(struct nvme_ns *ns, uint64_t sector,
			 unsigned int nr_zones, report_zones_cb cb, void *data)
{
	struct nvme_zone_report *report;
	struct nvme_ns_head *head;
	unsigned int zone_idx = 0, nz, fit;
	uint64_t lba, cap;
	size_t buflen;
	int ret;

	might_sleep();
	if (!ns || !cb)
		return -EINVAL;
	head = ns->head;
	if (head->ids.csi != NVME_CSI_ZNS || !head->zsze)
		return -EINVAL;

	report = (struct nvme_zone_report *)nvme_zns_alloc_report_buffer(
		ns, nr_zones, &buflen);
	if (!report)
		return -ENOMEM;
	fit = (unsigned int)((buflen - sizeof(struct nvme_zone_report)) /
			     sizeof(struct nvme_zone_descriptor));

	cap = ns->disk->capacity; /* logical blocks */
	lba = nvme_sect_to_lba(head, sector) & ~(head->zsze - 1);
	ret = 0;
	while (zone_idx < nr_zones && lba < cap) {
		ret = nvme_zns_report(ns, lba, NVME_ZRASF_ZONE_REPORT_ALL, true,
				      report, buflen);
		if (ret) {
			if (ret > 0)
				ret = -EIO;
			goto out_free;
		}

		nz = (unsigned int)(le64_to_cpu(report->nr_zones) < nr_zones ?
					    le64_to_cpu(report->nr_zones) :
					    nr_zones);
		if (nz > fit)
			nz = fit;
		if (!nz)
			break;

		for (unsigned int i = 0; i < nz && zone_idx < nr_zones; i++) {
			struct blk_zone zone;

			ret = nvme_zone_parse_desc(head, &report->entries[i],
						   &zone);
			if (ret) {
				nvme_ctrl_err(ns->ctrl, "invalid zone type %#x\n",
					      report->entries[i].zt);
				goto out_free;
			}
			ret = cb(&zone, zone_idx, data);
			if (ret)
				goto out_free;
			zone_idx++;
		}
		lba += head->zsze * nz;
	}

	ret = zone_idx > 0 ? (int)zone_idx : -EINVAL;
out_free:
	kfree(report);
	return ret;
}

blk_status_t nvme_setup_zone_mgmt_send(struct nvme_ns *ns,
				       struct blk_request *rq,
				       struct nvme_command *c, uint8_t action)
{
	kmemset(c, 0, sizeof(*c));
	c->zms.opcode = nvme_cmd_zone_mgmt_send;
	c->zms.nsid = cpu_to_le32(ns->head->ns_id);
	c->zms.slba = cpu_to_le64(nvme_sect_to_lba(ns->head, blk_rq_pos(rq)));
	c->zms.zsa = action;
	if (req_op(rq) == BLK_OP_ZONE_RESET_ALL)
		c->zms.select_all = NVME_ZMS_SELECT_ALL;
	return BLK_STS_OK;
}

int nvme_zns_mgmt_send(struct nvme_ns *ns, uint64_t slba, uint8_t action,
		       bool select_all)
{
	struct nvme_command c;

	might_sleep();
	if (ns->head->ids.csi != NVME_CSI_ZNS)
		return -EINVAL;
	switch (action) {
	case NVME_ZONE_CLOSE:
	case NVME_ZONE_FINISH:
	case NVME_ZONE_OPEN:
	case NVME_ZONE_RESET:
	case NVME_ZONE_OFFLINE:
		break;
	default:
		return -EINVAL;
	}
	kmemset(&c, 0, sizeof(c));
	c.zms.opcode = nvme_cmd_zone_mgmt_send;
	c.zms.nsid = cpu_to_le32(ns->head->ns_id);
	c.zms.slba = cpu_to_le64(slba);
	c.zms.zsa = action;
	if (select_all)
		c.zms.select_all = NVME_ZMS_SELECT_ALL;
	return nvme_submit_sync_cmd(ns->queue, &c, NULL, 0);
}
