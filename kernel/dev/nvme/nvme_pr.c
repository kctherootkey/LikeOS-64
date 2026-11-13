// LikeOS - NVMe persistent reservations (the IOC_PR_* operations of a
// namespace or multipath disk) and reservation notifications
//
// Register / acquire / release / preempt / clear map onto the Reservation
// Register, Acquire, Release commands with the established encodings
// (CPTPL "persist through power loss" on register, IEKEY for "ignore the
// existing key"); reading keys and the holder uses Reservation Report with
// the extended data structure (128-bit host identifiers), falling back to
// the 64-bit one when the controller answers "host identifier
// inconsistent".  Status codes become the block layer's pr_status values;
// a multipath disk sends through the current path.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

static uint8_t nvme_pr_type_from_blk(uint32_t type)
{
	switch (type) {
	case PR_WRITE_EXCLUSIVE:
		return NVME_PR_WRITE_EXCLUSIVE;
	case PR_EXCLUSIVE_ACCESS:
		return NVME_PR_EXCLUSIVE_ACCESS;
	case PR_WRITE_EXCLUSIVE_REG_ONLY:
		return NVME_PR_WRITE_EXCLUSIVE_REG_ONLY;
	case PR_EXCLUSIVE_ACCESS_REG_ONLY:
		return NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY;
	case PR_WRITE_EXCLUSIVE_ALL_REGS:
		return NVME_PR_WRITE_EXCLUSIVE_ALL_REGS;
	case PR_EXCLUSIVE_ACCESS_ALL_REGS:
		return NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS;
	}
	return 0;
}

static uint32_t nvme_block_pr_type_from_nvme(uint8_t type)
{
	switch (type) {
	case NVME_PR_WRITE_EXCLUSIVE:
		return PR_WRITE_EXCLUSIVE;
	case NVME_PR_EXCLUSIVE_ACCESS:
		return PR_EXCLUSIVE_ACCESS;
	case NVME_PR_WRITE_EXCLUSIVE_REG_ONLY:
		return PR_WRITE_EXCLUSIVE_REG_ONLY;
	case NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY:
		return PR_EXCLUSIVE_ACCESS_REG_ONLY;
	case NVME_PR_WRITE_EXCLUSIVE_ALL_REGS:
		return PR_WRITE_EXCLUSIVE_ALL_REGS;
	case NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS:
		return PR_EXCLUSIVE_ACCESS_ALL_REGS;
	}
	return 0;
}

/* Through the current path of a multipath disk. */
static int nvme_send_ns_head_pr_command(struct gendisk *disk,
					struct nvme_command *c, void *data,
					unsigned int data_len)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)disk->private_data;
	struct nvme_ns *ns;
	int idx, ret = -EWOULDBLOCK;

	idx = nvme_srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (ns) {
		c->common.nsid = cpu_to_le32(ns->head->ns_id);
		ret = nvme_submit_sync_cmd(ns->queue, c, data, data_len);
	}
	nvme_srcu_read_unlock(&head->srcu, idx);
	return ret;
}

static int nvme_send_ns_pr_command(struct nvme_ns *ns, struct nvme_command *c,
				   void *data, unsigned int data_len)
{
	c->common.nsid = cpu_to_le32(ns->head->ns_id);
	return nvme_submit_sync_cmd(ns->queue, c, data, data_len);
}

/* NVMe status -> pr_status (or -errno).  Pure (host-tested). */
int nvme_status_to_pr_err(int status);
int nvme_status_to_pr_err(int status)
{
	if (nvme_is_path_error((uint16_t)status))
		return PR_STS_PATH_FAILED;

	switch (status & NVME_SCT_SC_MASK) {
	case NVME_SC_SUCCESS:
		return PR_STS_SUCCESS;
	case NVME_SC_RESERVATION_CONFLICT:
		return PR_STS_RESERVATION_CONFLICT;
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:
		return -EINVAL;
	default:
		return PR_STS_IOERR;
	}
}

static int __nvme_send_pr_command(struct gendisk *disk, uint32_t cdw10,
				  uint32_t cdw11, uint8_t op, void *data,
				  unsigned int data_len)
{
	struct nvme_command c;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	c.common.opcode = op;
	c.common.cdw10 = cpu_to_le32(cdw10);
	c.common.cdw11 = cpu_to_le32(cdw11);

	if (nvme_disk_is_ns_head(disk))
		return nvme_send_ns_head_pr_command(disk, &c, data, data_len);
	return nvme_send_ns_pr_command(nvme_disk_to_ns(disk), &c, data,
				       data_len);
}

static int nvme_send_pr_command(struct gendisk *disk, uint32_t cdw10,
				uint32_t cdw11, uint8_t op, void *data,
				unsigned int data_len)
{
	int ret = __nvme_send_pr_command(disk, cdw10, cdw11, op, data,
					 data_len);

	return ret < 0 ? ret : nvme_status_to_pr_err(ret);
}

static int nvme_pr_register(struct gendisk *disk, uint64_t old_key,
			    uint64_t new_key, uint32_t flags)
{
	struct nvmet_pr_register_data data;
	uint32_t cdw10;

	if (flags & ~(uint32_t)PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	kmemset(&data, 0, sizeof(data));
	data.crkey = cpu_to_le64(old_key);
	data.nrkey = cpu_to_le64(new_key);

	cdw10 = old_key ? NVME_PR_REGISTER_ACT_REPLACE :
			  NVME_PR_REGISTER_ACT_REG;
	cdw10 |= (flags & PR_FL_IGNORE_KEY) ? NVME_PR_IGNORE_KEY : 0;
	cdw10 |= NVME_PR_CPTPL_PERSIST;
	return nvme_send_pr_command(disk, cdw10, 0, nvme_cmd_resv_register,
				    &data, sizeof(data));
}

static int nvme_pr_reserve(struct gendisk *disk, uint64_t key, uint32_t type,
			   uint32_t flags)
{
	struct nvmet_pr_acquire_data data;
	uint32_t cdw10;

	if (flags & ~(uint32_t)PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	kmemset(&data, 0, sizeof(data));
	data.crkey = cpu_to_le64(key);

	cdw10 = NVME_PR_ACQUIRE_ACT_ACQUIRE;
	cdw10 |= (uint32_t)nvme_pr_type_from_blk(type) << NVME_PR_RTYPE_SHIFT;
	cdw10 |= (flags & PR_FL_IGNORE_KEY) ? NVME_PR_IGNORE_KEY : 0;
	return nvme_send_pr_command(disk, cdw10, 0, nvme_cmd_resv_acquire,
				    &data, sizeof(data));
}

static int nvme_pr_preempt(struct gendisk *disk, uint64_t old_key,
			   uint64_t new_key, uint32_t type, int abort)
{
	struct nvmet_pr_acquire_data data;
	uint32_t cdw10;

	kmemset(&data, 0, sizeof(data));
	data.crkey = cpu_to_le64(old_key);
	data.prkey = cpu_to_le64(new_key);

	cdw10 = abort ? NVME_PR_ACQUIRE_ACT_PREEMPT_AND_ABORT :
			NVME_PR_ACQUIRE_ACT_PREEMPT;
	cdw10 |= (uint32_t)nvme_pr_type_from_blk(type) << NVME_PR_RTYPE_SHIFT;
	return nvme_send_pr_command(disk, cdw10, 0, nvme_cmd_resv_acquire,
				    &data, sizeof(data));
}

static int nvme_pr_clear(struct gendisk *disk, uint64_t key)
{
	struct nvmet_pr_release_data data;
	uint32_t cdw10;

	kmemset(&data, 0, sizeof(data));
	data.crkey = cpu_to_le64(key);

	cdw10 = NVME_PR_RELEASE_ACT_CLEAR;
	cdw10 |= key ? 0 : NVME_PR_IGNORE_KEY;
	return nvme_send_pr_command(disk, cdw10, 0, nvme_cmd_resv_release,
				    &data, sizeof(data));
}

static int nvme_pr_release(struct gendisk *disk, uint64_t key, uint32_t type)
{
	struct nvmet_pr_release_data data;
	uint32_t cdw10;

	kmemset(&data, 0, sizeof(data));
	data.crkey = cpu_to_le64(key);

	cdw10 = NVME_PR_RELEASE_ACT_RELEASE;
	cdw10 |= (uint32_t)nvme_pr_type_from_blk(type) << NVME_PR_RTYPE_SHIFT;
	cdw10 |= key ? 0 : NVME_PR_IGNORE_KEY;
	return nvme_send_pr_command(disk, cdw10, 0, nvme_cmd_resv_release,
				    &data, sizeof(data));
}

/* Reservation Report, extended data structure first. */
static int nvme_pr_resv_report(struct gendisk *disk, void *data,
			       uint32_t data_len, bool *eds)
{
	uint32_t cdw10, cdw11;
	int ret;

	cdw10 = nvme_bytes_to_numd(data_len);
	cdw11 = NVME_EXTENDED_DATA_STRUCT;
	*eds = true;
retry:
	ret = __nvme_send_pr_command(disk, cdw10, cdw11, nvme_cmd_resv_report,
				     data, data_len);
	if (ret == NVME_SC_HOST_ID_INCONSIST &&
	    cdw11 == NVME_EXTENDED_DATA_STRUCT) {
		cdw11 = 0;
		*eds = false;
		goto retry;
	}
	return ret < 0 ? ret : nvme_status_to_pr_err(ret);
}

/*
 * The keys of a report buffer (extended or not) into keys_info, at most
 * `room'.  Pure (host-tested).
 */
void nvme_pr_parse_keys(const void *buf, bool eds, uint32_t room,
			struct pr_keys *keys_info);
void nvme_pr_parse_keys(const void *buf, bool eds, uint32_t room,
			struct pr_keys *keys_info)
{
	const struct nvme_reservation_status_ext *rse =
		(const struct nvme_reservation_status_ext *)buf;
	const struct nvme_reservation_status *rs =
		(const struct nvme_reservation_status *)buf;
	uint32_t n;

	keys_info->generation = le32_to_cpu(rse->gen);
	keys_info->num_keys = get_unaligned_le16(rse->regctl);
	n = keys_info->num_keys < room ? keys_info->num_keys : room;
	for (uint32_t i = 0; i < n; i++) {
		if (eds)
			keys_info->keys[i] = le64_to_cpu(rse->regctl_eds[i].rkey);
		else
			keys_info->keys[i] = le64_to_cpu(rs->regctl_ds[i].rkey);
	}
}

static int nvme_pr_read_keys(struct gendisk *disk, struct pr_keys *keys_info)
{
	uint32_t num_keys = keys_info->num_keys;
	struct nvme_reservation_status_ext *rse;
	size_t rse_len;
	bool eds;
	int ret;

	/* Room for num_keys 128-bit host ID entries (the larger form). */
	rse_len = sizeof(*rse) +
		  (size_t)num_keys * sizeof(struct nvme_registered_ctrl_ext);
	if (rse_len > 0xffffffffULL || num_keys > PR_KEYS_MAX)
		return -EINVAL;
	rse = (struct nvme_reservation_status_ext *)nvme_kzalloc(rse_len);
	if (!rse)
		return -ENOMEM;

	ret = nvme_pr_resv_report(disk, rse, (uint32_t)rse_len, &eds);
	if (!ret)
		nvme_pr_parse_keys(rse, eds, num_keys, keys_info);
	kfree(rse);
	return ret;
}

static int nvme_pr_read_reservation(struct gendisk *disk,
				    struct pr_held_reservation *resv)
{
	struct nvme_reservation_status_ext tmp_rse, *rse;
	unsigned int num_regs, i, tries = 0;
	uint32_t rse_len;
	bool eds;
	int ret;

get_num_regs:
	/* How many registrations, to size the buffer. */
	kmemset(&tmp_rse, 0, sizeof(tmp_rse));
	ret = nvme_pr_resv_report(disk, &tmp_rse, sizeof(tmp_rse), &eds);
	if (ret)
		return ret;

	num_regs = get_unaligned_le16(tmp_rse.regctl);
	if (!num_regs) {
		resv->generation = le32_to_cpu(tmp_rse.gen);
		return 0;
	}

	rse_len = (uint32_t)(sizeof(*rse) +
			     (size_t)num_regs *
				     sizeof(struct nvme_registered_ctrl_ext));
	rse = (struct nvme_reservation_status_ext *)nvme_kzalloc(rse_len);
	if (!rse)
		return -ENOMEM;

	ret = nvme_pr_resv_report(disk, rse, rse_len, &eds);
	if (ret)
		goto free_rse;

	if (num_regs != get_unaligned_le16(rse->regctl)) {
		/* Changed in between: again (bounded). */
		kfree(rse);
		if (++tries > 16)
			return -EAGAIN;
		goto get_num_regs;
	}

	resv->generation = le32_to_cpu(rse->gen);
	resv->type = nvme_block_pr_type_from_nvme(rse->rtype);
	for (i = 0; i < num_regs; i++) {
		if (eds) {
			if (rse->regctl_eds[i].rcsts) {
				resv->key = le64_to_cpu(rse->regctl_eds[i].rkey);
				break;
			}
		} else {
			const struct nvme_reservation_status *rs =
				(const struct nvme_reservation_status *)rse;

			if (rs->regctl_ds[i].rcsts) {
				resv->key = le64_to_cpu(rs->regctl_ds[i].rkey);
				break;
			}
		}
	}
free_rse:
	kfree(rse);
	return ret;
}

const struct pr_ops nvme_pr_ops = {
	.pr_register = nvme_pr_register,
	.pr_reserve = nvme_pr_reserve,
	.pr_release = nvme_pr_release,
	.pr_preempt = nvme_pr_preempt,
	.pr_clear = nvme_pr_clear,
	.pr_read_keys = nvme_pr_read_keys,
	.pr_read_reservation = nvme_pr_read_reservation,
};

/* ==========================================================================
 * Reservation notifications
 * ========================================================================== */

static const char *nvme_pr_log_type_name(uint8_t type)
{
	switch (type) {
	case NVME_PR_LOG_EMPTY_LOG_PAGE:
		return "empty";
	case NVME_PR_LOG_REGISTRATION_PREEMPTED:
		return "registration preempted";
	case NVME_PR_LOG_RESERVATION_RELEASED:
		return "reservation released";
	case NVME_PR_LOG_RESERVATION_PREEMPTED:
		return "reservation preempted";
	default:
		return "unknown";
	}
}

/* A reservation notification event: read (and so consume) the pages
 * until the log is empty, logging each. */
void nvme_pr_resv_event(struct nvme_ctrl *ctrl)
{
	struct nvme_pr_log *log;
	unsigned int pages = 0;
	int ret;

	might_sleep();
	log = (struct nvme_pr_log *)nvme_kzalloc(sizeof(*log));
	if (!log)
		return;
	/* At most 255 pages are available at once (NR_PAGES is a byte). */
	do {
		ret = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_RESERVATION, 0,
				   NVME_CSI_NVM, log, sizeof(*log), 0);
		if (ret) {
			nvme_ctrl_warn(ctrl,
				       "reading the reservation log failed: %d\n",
				       ret);
			break;
		}
		if (log->type == NVME_PR_LOG_EMPTY_LOG_PAGE)
			break;
		nvme_ctrl_info(ctrl,
			       "reservation notification %llu: %s, nsid %u\n",
			       (unsigned long long)le64_to_cpu(log->count),
			       nvme_pr_log_type_name(log->type),
			       le32_to_cpu(log->nsid));
	} while (log->nr_pages && ++pages < 256);
	kfree(log);
}
