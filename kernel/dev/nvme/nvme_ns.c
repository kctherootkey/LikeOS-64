// LikeOS - NVMe namespaces: scan, identification, validation, disks,
// block limits, namespace management, format, sanitize
//
// A controller's namespaces are found by its scan work (queued when the
// controller goes live, on a Namespace Attribute Changed event, after a
// pass-through command that changed the namespace inventory, or from the
// rescan attribute):
//
//   * the active namespace list (Identify CNS 02h, 1024 NSIDs per page,
//     paged by the last NSID) -- or, on controllers older than 1.1 or
//     quirked, every NSID from 1 to NN in turn ("sequential scan"), also
//     the fallback when the list fails with Do Not Retry;
//   * per NSID: the identification descriptors (EUI-64, NGUID, UUID, CSI),
//     then the command set independent Identify Namespace where it exists
//     (CAP.CRMS.CRIMS, a non-NVM command set, or version 2.0) or the NVM
//     Identify Namespace; a namespace that is not ready is skipped (an
//     event comes when it is), one whose capacity is 0 is removed;
//   * a namespace seen for the first time is attached to its namespace
//     head (shared between the controllers of a subsystem that report the
//     same NSID and identifiers), gets a disk named nvme<ctrl>n<id>, or the
//     hidden path disk nvme<subsys>c<ctrl>n<id> under native multipath, its
//     block limits from the LBA format, metadata/protection information,
//     atomic write, preferred granularity, MDTS/DMRL/DMRSL and write cache
//     properties, and is registered;
//   * a known namespace is revalidated: identifiers must be unchanged, the
//     limits and capacity are updated (resize, format), an error with Do
//     Not Retry removes it;
//   * NSIDs that are no longer active are removed.
//
// Lifetimes: struct nvme_ns holds a reference on its head, its controller,
// its disk and its queue; the namespace list holds one on the namespace.
// Lists are published and unlinked under their mutex (scan_lock outside,
// subsys->lock for head->list, namespaces_lock for ctrl->namespaces) and
// walked by lock-free readers inside an nvme_srcu section.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

/* Format NVM may run for a long time on large media. */
#define NVME_FORMAT_TIMEOUT_MS (3600u * 1000u)
/* Sanitize status polling interval. */
#define NVME_SANITIZE_POLL_MS 1000u

static struct wait_queue_head nvme_sanitize_wq =
	WAIT_QUEUE_HEAD_INIT("nvme_sanitize");
static volatile uint64_t nvme_sanitize_events;

/* ==========================================================================
 * Sleepable read-side sections
 * ========================================================================== */

void nvme_srcu_init(struct nvme_srcu *s, const char *name)
{
	if (__atomic_load_n(&s->inited, __ATOMIC_ACQUIRE))
		return;
	s->cnt[0] = 0;
	s->cnt[1] = 0;
	s->idx = 0;
	mutex_init(&s->sync_lock, name);
	wq_head_init(&s->wq, name);
	__atomic_store_n(&s->inited, 1, __ATOMIC_RELEASE);
}

int nvme_srcu_read_lock(struct nvme_srcu *s)
{
	int idx;

	WARN_ON_ONCE(!s->inited);
	for (;;) {
		idx = __atomic_load_n(&s->idx, __ATOMIC_ACQUIRE) & 1;
		__atomic_add_fetch(&s->cnt[idx], 1, __ATOMIC_SEQ_CST);
		/* A synchronize that flipped the epoch in between either saw
		 * this count or is seen here: enter on the epoch we count in
		 * only when it is still the current one. */
		if ((__atomic_load_n(&s->idx, __ATOMIC_SEQ_CST) & 1) == idx)
			return idx;
		if (__atomic_sub_fetch(&s->cnt[idx], 1, __ATOMIC_SEQ_CST) == 0 &&
		    __atomic_load_n(&s->wq.first, __ATOMIC_ACQUIRE))
			wake_up(&s->wq);
	}
}

void nvme_srcu_read_unlock(struct nvme_srcu *s, int idx)
{
	long left;

	BUG_ON(idx != 0 && idx != 1);
	left = __atomic_sub_fetch(&s->cnt[idx], 1, __ATOMIC_SEQ_CST);
	BUG_ON(left < 0);
	if (left == 0 && __atomic_load_n(&s->wq.first, __ATOMIC_ACQUIRE))
		wake_up(&s->wq);
}

void nvme_srcu_synchronize(struct nvme_srcu *s)
{
	int old;

	might_sleep();
	if (!__atomic_load_n(&s->inited, __ATOMIC_ACQUIRE))
		return;
	mutex_lock(&s->sync_lock);
	/* Everything unlinked before this call is invisible to readers that
	 * enter the new epoch; wait for the old one to drain. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	old = __atomic_load_n(&s->idx, __ATOMIC_RELAXED) & 1;
	__atomic_store_n(&s->idx, old ^ 1, __ATOMIC_SEQ_CST);
	wait_event(s->wq,
		   __atomic_load_n(&s->cnt[old], __ATOMIC_SEQ_CST) == 0);
	mutex_unlock(&s->sync_lock);
}

void nvme_list_add_tail_srcu(struct list_head *entry, struct list_head *head)
{
	struct list_head *prev = head->prev;

	entry->next = head;
	entry->prev = prev;
	/* The entry is complete before a reader can reach it. */
	__atomic_store_n(&prev->next, entry, __ATOMIC_RELEASE);
	head->prev = entry;
}

void nvme_list_add_srcu(struct list_head *entry, struct list_head *prev)
{
	struct list_head *next = prev->next;

	entry->next = next;
	entry->prev = prev;
	__atomic_store_n(&prev->next, entry, __ATOMIC_RELEASE);
	next->prev = entry;
}

void nvme_list_del_srcu(struct list_head *entry)
{
	struct list_head *prev = entry->prev, *next = entry->next;

	BUG_ON(!prev || !next);
	__atomic_store_n(&prev->next, next, __ATOMIC_RELEASE);
	next->prev = prev;
	/* entry->next stays: a reader standing on it walks on. */
	entry->prev = NULL;
}

/* ==========================================================================
 * Reference counting
 * ========================================================================== */

static void nvme_free_ns_head(struct nvme_ns_head *head)
{
	struct nvme_subsystem *subsys = head->subsys;

	might_sleep();
	nvme_mpath_put_disk(head);
	ida_free(&subsys->ns_ida, (unsigned int)head->instance);
	if (head->plids)
		kfree(head->plids);
	nvme_put_subsystem(subsys);
	kfree(head);
}

bool nvme_tryget_ns_head(struct nvme_ns_head *head)
{
	return refcount_inc_not_zero(&head->ref) != 0;
}

/*
 * A head's last reference may go from one of its own work items (the
 * delayed removal work).  kblockd touches an item after its function
 * returned, so the head -- which embeds the item -- is not freed there:
 * the reaper (a work item of its own) frees it after waiting for the
 * head's items to finish.
 */
static spinlock_t nvme_head_reap_lock = SPINLOCK_INIT("nvme_head_reap");
static LIST_HEAD(nvme_head_reap_list);
static struct blk_work nvme_head_reap_work;
static volatile int nvme_head_reap_inited;

static void nvme_head_reaper(void *arg)
{
	struct nvme_ns_head *head;
	uint64_t flags;

	(void)arg;
	for (;;) {
		spin_lock_irqsave(&nvme_head_reap_lock, &flags);
		head = list_first_entry_or_null(&nvme_head_reap_list,
						struct nvme_ns_head, entry);
		if (head)
			list_del_init(&head->entry);
		spin_unlock_irqrestore(&nvme_head_reap_lock, flags);
		if (!head)
			return;
		nvme_free_ns_head(head);
	}
}

void nvme_put_ns_head(struct nvme_ns_head *head)
{
	uint64_t flags;

	BUG_ON(refcount_read(&head->ref) <= 0);
	if (!refcount_dec_and_test(&head->ref))
		return;
	if (!blk_in_kblockd()) {
		nvme_free_ns_head(head);
		return;
	}
	/* One-time setup: 0 none, 1 being set up, 2 ready. */
	{
		int expected = 0;

		if (__atomic_compare_exchange_n(&nvme_head_reap_inited,
						&expected, 1, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE)) {
			blk_work_init(&nvme_head_reap_work, nvme_head_reaper,
				      NULL);
			__atomic_store_n(&nvme_head_reap_inited, 2,
					 __ATOMIC_RELEASE);
		}
		while (__atomic_load_n(&nvme_head_reap_inited,
				       __ATOMIC_ACQUIRE) != 2)
			__builtin_ia32_pause();
	}
	spin_lock_irqsave(&nvme_head_reap_lock, &flags);
	/* Off the subsystem list by now: `entry' is free to carry it. */
	WARN_ON(!list_empty(&head->entry));
	list_add_tail(&head->entry, &nvme_head_reap_list);
	spin_unlock_irqrestore(&nvme_head_reap_lock, flags);
	nvme_queue_work(&nvme_head_reap_work);
}

static void nvme_free_ns(struct nvme_ns *ns)
{
	struct nvme_ctrl *ctrl = ns->ctrl;

	might_sleep();
	if (ns->queue)
		blk_put_queue(ns->queue);
	if (ns->disk)
		disk_put(ns->disk);
	nvme_put_ns_head(ns->head);
	nvme_put_ctrl(ctrl);
	kfree(ns);
}

bool nvme_get_ns(struct nvme_ns *ns)
{
	return refcount_inc_not_zero(&ns->ref) != 0;
}

void nvme_put_ns(struct nvme_ns *ns)
{
	BUG_ON(refcount_read(&ns->ref) <= 0);
	if (refcount_dec_and_test(&ns->ref)) {
		/* The last reference frees the disk: never from a context
		 * that cannot sleep (every holder is process context). */
		WARN_ON_ONCE(!blk_can_sleep() && blk_kblockd_running());
		nvme_free_ns(ns);
	}
}

/* ==========================================================================
 * Identity strings
 * ========================================================================== */

static const char nvme_hex[] = "0123456789abcdef";

static size_t nvme_put_hex(char *buf, size_t pos, size_t cap,
			   const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n && pos + 2 < cap; i++) {
		buf[pos++] = nvme_hex[p[i] >> 4];
		buf[pos++] = nvme_hex[p[i] & 0xf];
	}
	if (pos < cap)
		buf[pos] = '\0';
	return pos;
}

static size_t nvme_put_str(char *buf, size_t pos, size_t cap, const char *s)
{
	while (*s && pos + 1 < cap)
		buf[pos++] = *s++;
	if (pos < cap)
		buf[pos] = '\0';
	return pos;
}

/* "uuid.%pU", "eui.<nguid>", "eui.<eui64>" or the vendor/serial/model/nsid
 * fallback, as /sys/block/X/wwid prints it (cut at cap). */
void nvme_ns_head_wwid(const struct nvme_ns_head *head, char *buf, size_t cap)
{
	const struct nvme_ns_ids *ids = &head->ids;
	const struct nvme_subsystem *subsys = head->subsys;
	size_t pos = 0;
	char tmp[16];

	if (!cap)
		return;
	buf[0] = '\0';
	if (!nvme_mem_is_zero(ids->uuid, sizeof(ids->uuid))) {
		static const int groups[5] = { 4, 2, 2, 2, 6 };
		const uint8_t *u = ids->uuid;

		pos = nvme_put_str(buf, pos, cap, "uuid.");
		for (int g = 0; g < 5; g++) {
			if (g)
				pos = nvme_put_str(buf, pos, cap, "-");
			pos = nvme_put_hex(buf, pos, cap, u, (size_t)groups[g]);
			u += groups[g];
		}
		return;
	}
	if (!nvme_mem_is_zero(ids->nguid, sizeof(ids->nguid))) {
		pos = nvme_put_str(buf, pos, cap, "eui.");
		nvme_put_hex(buf, pos, cap, ids->nguid, sizeof(ids->nguid));
		return;
	}
	if (!nvme_mem_is_zero(ids->eui64, sizeof(ids->eui64))) {
		pos = nvme_put_str(buf, pos, cap, "eui.");
		nvme_put_hex(buf, pos, cap, ids->eui64, sizeof(ids->eui64));
		return;
	}
	ksnprintf(tmp, sizeof(tmp), "nvme.%04x-", subsys->vendor_id);
	pos = nvme_put_str(buf, pos, cap, tmp);
	pos = nvme_put_hex(buf, pos, cap, (const uint8_t *)subsys->serial,
			   (size_t)nvme_strlen(subsys->serial,
					       sizeof(subsys->serial)));
	pos = nvme_put_str(buf, pos, cap, "-");
	pos = nvme_put_hex(buf, pos, cap, (const uint8_t *)subsys->model,
			   (size_t)nvme_strlen(subsys->model,
					       sizeof(subsys->model)));
	ksnprintf(tmp, sizeof(tmp), "-%08x", head->ns_id);
	nvme_put_str(buf, pos, cap, tmp);
}

static void nvme_copy_trimmed(char *dst, size_t cap, const char *src,
			      int srclen)
{
	int len = nvme_strlen(src, srclen);

	if ((size_t)len >= cap)
		len = (int)cap - 1;
	kmemcpy(dst, src, (size_t)len);
	dst[len] = '\0';
}

void nvme_fill_disk_identity(struct nvme_ctrl *ctrl, struct nvme_ns_head *head,
			     struct gendisk *disk)
{
	struct nvme_subsystem *subsys = head->subsys;

	ksnprintf(disk->vendor, sizeof(disk->vendor), "NVMe    ");
	nvme_copy_trimmed(disk->model, sizeof(disk->model), subsys->model,
			  sizeof(subsys->model));
	nvme_copy_trimmed(disk->serial, sizeof(disk->serial), subsys->serial,
			  sizeof(subsys->serial));
	nvme_copy_trimmed(disk->firmware, sizeof(disk->firmware),
			  subsys->firmware_rev, sizeof(subsys->firmware_rev));
	ksnprintf(disk->bus, sizeof(disk->bus), "nvme");
	nvme_ns_head_wwid(head, disk->wwid, sizeof(disk->wwid));
	/* The first world-wide identifier as the by-id "wwn" style name. */
	if (!nvme_mem_is_zero(head->ids.eui64, sizeof(head->ids.eui64))) {
		size_t p = nvme_put_str(disk->wwn, 0, sizeof(disk->wwn), "0x");

		nvme_put_hex(disk->wwn, p, sizeof(disk->wwn), head->ids.eui64,
			     sizeof(head->ids.eui64));
	} else {
		disk->wwn[0] = '\0';
	}
	ksnprintf(disk->by_path, sizeof(disk->by_path), "nvme-%u",
		  head->ns_id);
	if (head->disk == disk)
		ksnprintf(disk->device_link, sizeof(disk->device_link), "%s",
			  subsys->sysfs_base);
	else
		ksnprintf(disk->device_link, sizeof(disk->device_link), "%s",
			  ctrl->sysfs_base);
	/* The namespace attribute group (nsid, eui, nguid, uuid, csi, nuse,
	 * ana_*, multipath/ on a head) appears in /sys/block/<disk>/ too,
	 * where the established layout keeps it: the namespace directory
	 * under the controller, or the head's under the subsystem. */
	ksnprintf(disk->attr_dir, sizeof(disk->attr_dir), "%s/%s",
		  disk->device_link, disk->disk_name);
	disk->pci = ctrl->pdev;
	disk->host_no = ctrl->instance;
	disk->channel = 0;
	disk->target = 0;
	disk->lun = (int)head->ns_id;
	disk->scsi_type = TYPE_DISK;
}

/* Geometry for HDIO_GETGEO: 64 heads, 32 sectors (the conventional NVMe
 * answer), cylinders from the capacity. */
static void nvme_set_geometry(struct gendisk *disk, uint64_t cap_512)
{
	uint64_t cyl = cap_512 >> 11;

	disk->heads = 1 << 6;
	disk->sectors_per_track = 1 << 5;
	disk->cylinders = cyl > 0xffffffffULL ? 0xffffffffu : (uint32_t)cyl;
}

/* ==========================================================================
 * Namespace identification
 * ========================================================================== */

static int nvme_ns_info_from_identify(struct nvme_ctrl *ctrl,
				      struct nvme_ns_info *info)
{
	struct nvme_ns_ids *ids = &info->ids;
	struct nvme_id_ns *id;
	int ret;

	ret = nvme_identify_ns(ctrl, info->nsid, &id);
	if (ret)
		return ret;

	if (id->ncap == 0) {
		/* Not allocated or not attached. */
		info->is_removed = true;
		ret = -ENODEV;
		goto out;
	}

	info->anagrpid = id->anagrpid;
	info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
	info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
	info->is_ready = true;
	info->endgid = le16_to_cpu(id->endgid);
	if (ctrl->quirks & NVME_QUIRK_BOGUS_NID) {
		nvme_ctrl_info(ctrl,
			       "ignoring bogus namespace identifiers\n");
	} else {
		if (ctrl->vs >= NVME_VS(1, 1, 0) &&
		    nvme_mem_is_zero(ids->eui64, sizeof(ids->eui64)))
			kmemcpy(ids->eui64, id->eui64, sizeof(ids->eui64));
		if (ctrl->vs >= NVME_VS(1, 2, 0) &&
		    nvme_mem_is_zero(ids->nguid, sizeof(ids->nguid)))
			kmemcpy(ids->nguid, id->nguid, sizeof(ids->nguid));
	}
out:
	kfree(id);
	return ret;
}

static int nvme_ns_info_from_id_cs_indep(struct nvme_ctrl *ctrl,
					 struct nvme_ns_info *info)
{
	struct nvme_id_ns_cs_indep *id;
	int ret;

	ret = nvme_identify_ns_cs_indep(ctrl, info->nsid, &id);
	if (!ret) {
		info->anagrpid = id->anagrpid;
		info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
		info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
		/* (The established driver reads NSTAT.NRDY here.) */
		info->is_ready = id->nstat & NVME_NSTAT_NRDY;
		info->is_rotational = id->nsfeat & NVME_NS_ROTATIONAL;
		info->no_vwc = id->nsfeat & NVME_NS_VWC_NOT_PRESENT;
		info->endgid = le16_to_cpu(id->endgid);
		kfree(id);
	}
	return ret;
}

/* One identification descriptor: its identifier length, or -1 for a
 * descriptor with a bogus length (ends the walk). */
static int nvme_process_ns_desc(struct nvme_ctrl *ctrl, struct nvme_ns_ids *ids,
				const struct nvme_ns_id_desc *cur,
				bool *csi_seen)
{
	const uint8_t *data = (const uint8_t *)(cur + 1);

	switch (cur->nidt) {
	case NVME_NIDT_EUI64:
		if (cur->nidl != NVME_NIDT_EUI64_LEN) {
			nvme_ctrl_warn(ctrl,
				       "ctrl returned bogus length: %u for NVME_NIDT_EUI64\n",
				       cur->nidl);
			return -1;
		}
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->eui64, data, NVME_NIDT_EUI64_LEN);
		return NVME_NIDT_EUI64_LEN;
	case NVME_NIDT_NGUID:
		if (cur->nidl != NVME_NIDT_NGUID_LEN) {
			nvme_ctrl_warn(ctrl,
				       "ctrl returned bogus length: %u for NVME_NIDT_NGUID\n",
				       cur->nidl);
			return -1;
		}
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->nguid, data, NVME_NIDT_NGUID_LEN);
		return NVME_NIDT_NGUID_LEN;
	case NVME_NIDT_UUID:
		if (cur->nidl != NVME_NIDT_UUID_LEN) {
			nvme_ctrl_warn(ctrl,
				       "ctrl returned bogus length: %u for NVME_NIDT_UUID\n",
				       cur->nidl);
			return -1;
		}
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->uuid, data, NVME_NIDT_UUID_LEN);
		return NVME_NIDT_UUID_LEN;
	case NVME_NIDT_CSI:
		if (cur->nidl != NVME_NIDT_CSI_LEN) {
			nvme_ctrl_warn(ctrl,
				       "ctrl returned bogus length: %u for NVME_NIDT_CSI\n",
				       cur->nidl);
			return -1;
		}
		ids->csi = data[0];
		*csi_seen = true;
		return NVME_NIDT_CSI_LEN;
	default:
		/* Unknown types are skipped. */
		return cur->nidl;
	}
}

int nvme_ns_descs_parse(struct nvme_ctrl *ctrl, struct nvme_ns_info *info,
			const void *data)
{
	const uint8_t *buf = (const uint8_t *)data;
	bool csi_seen = false;
	int pos, len;

	for (pos = 0; pos + (int)sizeof(struct nvme_ns_id_desc) <=
		      NVME_IDENTIFY_DATA_SIZE;
	     pos += len) {
		const struct nvme_ns_id_desc *cur =
			(const struct nvme_ns_id_desc *)(buf + pos);

		if (cur->nidl == 0)
			break;
		/* The identifier must lie inside the data structure. */
		if (pos + (int)sizeof(*cur) + cur->nidl >
		    NVME_IDENTIFY_DATA_SIZE)
			break;
		len = nvme_process_ns_desc(ctrl, &info->ids, cur, &csi_seen);
		if (len < 0)
			break;
		len += (int)sizeof(*cur);
	}

	if (nvme_multi_css(ctrl) && !csi_seen) {
		nvme_ctrl_warn(ctrl, "Command set not reported for nsid:%u\n",
			       info->nsid);
		return -EINVAL;
	}
	return 0;
}

static bool nvme_ns_ids_equal(const struct nvme_ns_ids *a,
			      const struct nvme_ns_ids *b)
{
	return kmemcmp(a->uuid, b->uuid, sizeof(a->uuid)) == 0 &&
	       kmemcmp(a->nguid, b->nguid, sizeof(a->nguid)) == 0 &&
	       kmemcmp(a->eui64, b->eui64, sizeof(a->eui64)) == 0 &&
	       a->csi == b->csi;
}

static int nvme_identify_ns_nvm(struct nvme_ctrl *ctrl, uint32_t nsid,
				struct nvme_id_ns_nvm **nvmp)
{
	struct nvme_id_ns_nvm *nvm;
	int ret;

	nvm = (struct nvme_id_ns_nvm *)nvme_kzalloc(sizeof(*nvm));
	if (!nvm)
		return -ENOMEM;
	ret = nvme_identify(ctrl, NVME_ID_CNS_CS_NS, NVME_CSI_NVM, nsid, 0,
			    nvm);
	if (ret)
		kfree(nvm);
	else
		*nvmp = nvm;
	return ret;
}

/* ==========================================================================
 * Block limits
 * ==========================================================================
 *
 * The block layer counts limits and capacities in the disk's logical
 * blocks; the controller limits (max_hw_sectors, max_zeroes_sectors,
 * max_zone_append) are kept in 512-byte sectors as the established core
 * does: nvme_ns_sect512_to_disk() converts.
 */

bool nvme_init_integrity(struct nvme_ns_head *head,
				struct blk_limits *lim,
				struct nvme_ns_info *info)
{
	struct blk_integrity *bi = &lim->integrity;

	kmemset(bi, 0, sizeof(*bi));
	if (!head->ms)
		return true;

	/* PI can always be supported: the controller inserts and strips it
	 * (PRACT).  Other metadata needs a separate metadata buffer. */
	if (!(head->features & NVME_NS_METADATA_SUPPORTED))
		return nvme_ns_has_pi(head);

	switch (head->pi_type) {
	case NVME_NS_DPS_PI_TYPE3:
		switch (head->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC;
			bi->tag_size = sizeof(uint16_t) + sizeof(uint32_t);
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		case NVME_NVM_NS_64B_GUARD:
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC64;
			bi->tag_size = sizeof(uint16_t) + 6;
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE;
			break;
		default:
			break;
		}
		break;
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		switch (head->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC;
			bi->tag_size = sizeof(uint16_t);
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE |
				     BLK_INTEGRITY_REF_TAG;
			break;
		case NVME_NVM_NS_64B_GUARD:
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC64;
			bi->tag_size = sizeof(uint16_t);
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE |
				     BLK_INTEGRITY_REF_TAG;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	bi->metadata_size = head->ms;
	bi->tuple_size = (uint8_t)(head->ms > 255 ? 255 : head->ms);
	bi->interval_exp = head->lba_shift;
	if (bi->csum_type) {
		bi->pi_tuple_size = (uint8_t)head->pi_size;
		bi->pi_offset = info->pi_offset;
	}
	return true;
}

static void nvme_config_discard(struct nvme_ns *ns,
				const struct nvme_id_ns *id,
				const struct nvme_id_ns_nvm *nvm,
				struct blk_limits *lim)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_ns_head *head = ns->head;
	uint64_t max;

	/* DMRSL is in logical blocks: a range larger than the block layer
	 * can count is no limit.  The shift may still be the raw LBADS of an
	 * unusable format (< 9) here: the clamping conversion, never a
	 * negative shift. */
	if (ctrl->dmrsl && ctrl->dmrsl <= nvme_ns_sect512_to_disk(head, 0xffffffffu))
		max = ctrl->dmrsl;
	else if (ctrl->oncs & NVME_CTRL_ONCS_DSM)
		max = nvme_ns_sect512_to_disk(head, 0xffffffffu);
	else
		max = 0;

	lim->max_hw_discard_sectors = max;
	lim->max_discard_sectors = max;
	lim->discard_granularity = max ? lim->logical_block_size : 0;
	lim->discard_alignment = 0;
	lim->max_discard_segments = ctrl->dmrl ? ctrl->dmrl :
						 NVME_DSM_MAX_RANGES;

	/* NPDG (and the larger NPDGL of 2.1): the preferred deallocation
	 * granularity, when the I/O optimization fields are valid.  NPDA,
	 * its alignment, is a multiple of it (deallocations aligned to a
	 * granule are aligned to it). */
	if (max && (id->nsfeat & NVME_NS_FEAT_IO_OPT)) {
		uint64_t bs = lim->logical_block_size;
		uint64_t npdg = id->npdg ? bs * (1u + le16_to_cpu(id->npdg)) : 0;

		if (nvm && le32_to_cpu(nvm->npdgl) &&
		    bs * le32_to_cpu(nvm->npdgl) > npdg)
			npdg = bs * le32_to_cpu(nvm->npdgl);
		if (npdg && npdg <= 0x80000000ULL && nvme_is_power_of_2(npdg))
			lim->discard_granularity = (uint32_t)npdg;
	}
}

static void nvme_configure_pi_elbas(struct nvme_ns_head *head,
				    const struct nvme_id_ns *id,
				    const struct nvme_id_ns_nvm *nvm)
{
	uint32_t elbaf = le32_to_cpu(nvm->elbaf[nvme_lbaf_index(id->flbas)]);
	uint8_t guard_type;

	/* No storage tag formats. */
	if (nvme_elbaf_sts(elbaf))
		return;

	guard_type = nvme_elbaf_guard_type(elbaf);
	if ((nvm->pic & NVME_ID_NS_NVM_QPIFS) &&
	    guard_type == NVME_NVM_NS_QTYPE_GUARD)
		guard_type = nvme_elbaf_qualified_guard_type(elbaf);

	head->guard_type = guard_type;
	switch (head->guard_type) {
	case NVME_NVM_NS_64B_GUARD:
		head->pi_size = sizeof(struct crc64_pi_tuple);
		break;
	case NVME_NVM_NS_16B_GUARD:
		head->pi_size = sizeof(struct t10_pi_tuple);
		break;
	default:
		break;
	}
}

static void nvme_configure_metadata(struct nvme_ctrl *ctrl,
				    struct nvme_ns_head *head,
				    const struct nvme_id_ns *id,
				    const struct nvme_id_ns_nvm *nvm,
				    struct nvme_ns_info *info)
{
	head->features &= ~(unsigned long)(NVME_NS_METADATA_SUPPORTED |
					   NVME_NS_EXT_LBAS);
	head->pi_type = 0;
	head->pi_size = 0;
	head->ms = le16_to_cpu(id->lbaf[nvme_lbaf_index(id->flbas)].ms);
	if (!head->ms || !(ctrl->ops->flags & NVME_F_METADATA_SUPPORTED))
		return;

	if (nvm && (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_ELBAS)) {
		nvme_configure_pi_elbas(head, id, nvm);
	} else {
		head->pi_size = sizeof(struct t10_pi_tuple);
		head->guard_type = NVME_NVM_NS_16B_GUARD;
	}

	if (head->pi_size && head->ms >= head->pi_size)
		head->pi_type = id->dps & NVME_NS_DPS_PI_MASK;
	if (!(id->dps & NVME_NS_DPS_PI_FIRST))
		info->pi_offset = (uint8_t)(head->ms - head->pi_size);

	/*
	 * PCIe: the separate metadata buffer of the block layer cannot be
	 * remapped into an extended LBA, so block-layer metadata/PI needs
	 * a separate-buffer format; extended LBAs only via pass-through.
	 */
	if (id->flbas & NVME_NS_FLBAS_META_EXT)
		head->features |= NVME_NS_EXT_LBAS;
	else
		head->features |= NVME_NS_METADATA_SUPPORTED;
}

static uint32_t nvme_configure_atomic_write(struct nvme_ns *ns,
					    const struct nvme_id_ns *id,
					    struct blk_limits *lim, uint32_t bs)
{
	uint32_t atomic_bs, boundary = 0;

	/* No support for an offset of the atomic boundaries. */
	if (id->nabo)
		return bs;

	if ((id->nsfeat & NVME_NS_FEAT_ATOMICS) && id->nawupf) {
		/* The namespace's own power-fail atomic write unit. */
		atomic_bs = (1u + le16_to_cpu(id->nawupf)) * bs;
		if (id->nabspf)
			boundary = (le16_to_cpu(id->nabspf) + 1u) * bs;
	} else {
		/* Only the power-fail value of the namespace counts: the
		 * controller-wide AWUPF is not relied upon (and the
		 * non-power-fail NAWUN/AWUN never are -- an atomic write
		 * must survive power loss). */
		static int warned;

		if (ns->ctrl->awupf && nvme_first_time(&warned)) {
			nvme_ctrl_info(ns->ctrl,
				       "AWUPF ignored, only NAWUPF accepted\n");
		}
		atomic_bs = bs;
	}

	lim->atomic_write_hw_max = atomic_bs;
	lim->atomic_write_hw_boundary = boundary;
	lim->atomic_write_hw_unit_min = bs;
	lim->atomic_write_hw_unit_max = nvme_rounddown_pow_of_two(atomic_bs);
	lim->features |= BLK_FEAT_ATOMIC_WRITES;
	return atomic_bs;
}

static uint32_t nvme_max_drv_segments(const struct nvme_ctrl *ctrl)
{
	return ctrl->max_hw_sectors / (NVME_CTRL_PAGE_SIZE >> 9) + 1;
}

static void nvme_ns_set_ctrl_limits(struct nvme_ctrl *ctrl,
				 const struct nvme_ns_head *head,
				 struct blk_limits *lim)
{
	uint32_t segs = nvme_max_drv_segments(ctrl);
	/* No head yet (a new disk): its initial 512-byte logical blocks. */
	uint64_t hw = head ? nvme_ns_sect512_to_disk(head, ctrl->max_hw_sectors) :
			    ctrl->max_hw_sectors;

	if (ctrl->max_segments && ctrl->max_segments < segs)
		segs = ctrl->max_segments;
	if (segs > 0xffff)
		segs = 0xffff;
	if (hw > 0xffffffffULL)
		hw = 0xffffffffULL;
	if (!hw)
		hw = 1;
	lim->max_hw_sectors = (uint32_t)hw;
	lim->max_sectors = (uint32_t)hw;
	lim->max_dev_sectors = (uint32_t)hw;
	lim->max_segments = segs;
	lim->virt_boundary_mask =
		ctrl->ops && ctrl->ops->get_virt_boundary ?
			ctrl->ops->get_virt_boundary(ctrl, false) :
			NVME_CTRL_PAGE_SIZE - 1;
	lim->max_segment_size = 0xffffffffu;
	lim->seg_boundary_mask = ~0ULL;
	lim->dma_alignment = 3;
	lim->queue_depth = (uint32_t)ctrl->sqsize + 1;
}

/* NPWG/NPWA/NOWS/NPDG/NPDA (NSFEAT bit 4), atomic write unit, write
 * zeroes; false when the LBA size is unusable for block I/O. */
static bool nvme_update_disk_info(struct nvme_ns *ns,
				  const struct nvme_id_ns *id,
				  struct blk_limits *lim)
{
	struct nvme_ns_head *head = ns->head;
	uint32_t bs = 1u << head->lba_shift;
	uint32_t atomic_bs, phys_bs, io_opt = 0;
	bool valid = true;

	/* Block sizes below 512 bytes or above 64 KiB cannot be used:
	 * no block I/O to the namespace then (capacity 0). */
	if (head->lba_shift < 9 || head->lba_shift > 16 ||
	    blk_validate_block_size(bs)) {
		bs = 1u << 9;
		valid = false;
	}

	phys_bs = bs;
	atomic_bs = nvme_configure_atomic_write(ns, id, lim, bs);

	if (id->nsfeat & NVME_NS_FEAT_IO_OPT) {
		/* NPWG: preferred write granularity; NPWA: its alignment
		 * (writes aligned to a granule are aligned to it). */
		phys_bs = bs * (1u + le16_to_cpu(id->npwg));
		/* NOWS: optimal write size. */
		if (id->nows)
			io_opt = bs * (1u + le16_to_cpu(id->nows));
		if (le16_to_cpu(id->npwa) != le16_to_cpu(id->npwg) &&
		    id->npwa && (le16_to_cpu(id->npwa) + 1u) %
					(le16_to_cpu(id->npwg) + 1u))
			lim->lim_flags |= BLK_FLAG_MISALIGNED;
	}

	/* Filesystems take a single physical block write as atomic: the
	 * physical block size is at most the power-fail atomic unit. */
	lim->logical_block_size = bs;
	lim->physical_block_size = phys_bs < atomic_bs ? phys_bs : atomic_bs;
	lim->io_min = phys_bs;
	lim->io_opt = io_opt;
	lim->alignment_offset = 0;

	if ((ns->ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES) &&
	    (ns->ctrl->oncs & NVME_CTRL_ONCS_DSM)) {
		lim->max_write_zeroes_sectors =
			nvme_ns_sect512_to_disk(head, 0xffffffffu);
	} else {
		uint64_t wz = nvme_ns_sect512_to_disk(
			head, ns->ctrl->max_zeroes_sectors);

		/* Write Zeroes counts 16 bits of blocks. */
		lim->max_write_zeroes_sectors = wz > 0x10000 ? 0x10000 : wz;
	}
	return valid;
}

static bool nvme_ns_is_readonly(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	return info->is_readonly ||
	       nvme_test_bit(NVME_NS_FORCE_RO, &ns->flags);
}

static bool nvme_first_scan(struct gendisk *disk)
{
	/* The scan revalidates a disk before it is added. */
	return (disk_flags(disk) & (GENHD_FL_UP | GENHD_FL_DEAD)) !=
	       GENHD_FL_UP;
}

static void nvme_set_chunk_sectors(struct nvme_ns *ns,
				   const struct nvme_id_ns *id,
				   struct blk_limits *lim)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	uint64_t iob;

	if ((ctrl->quirks & NVME_QUIRK_STRIPE_SIZE) &&
	    nvme_is_power_of_2(ctrl->max_hw_sectors))
		iob = nvme_ns_sect512_to_disk(ns->head, ctrl->max_hw_sectors);
	else
		iob = le16_to_cpu(id->noiob); /* logical blocks */

	if (!iob)
		return;

	if (!nvme_is_power_of_2(iob)) {
		if (nvme_first_scan(ns->disk))
			kprintf(NVME_LOG_PREFIX
				"%s: ignoring unaligned IO boundary:%llu\n",
				ns->disk->disk_name, (unsigned long long)iob);
		return;
	}

	if (lim->features & BLK_FEAT_ZONED) {
		if (nvme_first_scan(ns->disk))
			kprintf(NVME_LOG_PREFIX
				"%s: ignoring zoned namespace IO boundary\n",
				ns->disk->disk_name);
		return;
	}
	lim->chunk_sectors = (uint32_t)iob;
}

/*
 * Everything the identify data says about the limits of a block namespace
 * (no I/O, no locks): fills `lim', the head's format fields and returns the
 * capacity in logical blocks (0: no block I/O possible).  `zoned' says
 * whether nvme_query_zone_info() accepted the namespace.
 */
uint64_t nvme_ns_compute_limits(struct nvme_ns *ns, const struct nvme_id_ns *id,
				const struct nvme_id_ns_nvm *nvm,
				struct nvme_ns_info *info,
				struct blk_limits *lim, bool zoned);
uint64_t nvme_ns_compute_limits(struct nvme_ns *ns, const struct nvme_id_ns *id,
				const struct nvme_id_ns_nvm *nvm,
				struct nvme_ns_info *info,
				struct blk_limits *lim, bool zoned)
{
	struct nvme_ns_head *head = ns->head;
	struct nvme_ctrl *ctrl = ns->ctrl;
	unsigned int lbaf = nvme_lbaf_index(id->flbas);
	uint64_t capacity;

	head->lba_shift = id->lbaf[lbaf].ds;
	head->nuse = le64_to_cpu(id->nuse);
	head->mssrl = le16_to_cpu(id->mssrl);
	head->mcl = le32_to_cpu(id->mcl);
	head->msrc = id->msrc;
	head->nacwu = (id->nsfeat & NVME_NS_FEAT_ATOMICS) ?
			      le16_to_cpu(id->nacwu) :
			      0;
	/* An unusable LBA size still needs a sane shift for the unit
	 * conversions below; the capacity becomes 0. */
	if (head->lba_shift < 9 || head->lba_shift > 16)
		capacity = 0;
	else
		capacity = le64_to_cpu(id->nsze);
	lim->features &= ~(uint32_t)(BLK_FEAT_ATOMIC_WRITES | BLK_FEAT_ZONED |
				     BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA |
				     BLK_FEAT_ROTATIONAL);
	lim->lim_flags &= ~(uint32_t)BLK_FLAG_MISALIGNED;
	lim->chunk_sectors = 0;

	{
		/* The conversions want a valid shift. */
		uint8_t saved = head->lba_shift;

		if (saved < 9 || saved > 16)
			head->lba_shift = 9;
		nvme_ns_set_ctrl_limits(ctrl, head, lim);
		head->lba_shift = saved;
	}
	nvme_configure_metadata(ctrl, head, id, nvm, info);
	if (zoned)
		nvme_update_zone_info(ns, lim);
	nvme_set_chunk_sectors(ns, id, lim);
	if (!nvme_update_disk_info(ns, id, lim))
		capacity = 0;
	nvme_config_discard(ns, id, nvm, lim);
	/* The streams directive: the stream write size is the preferred
	 * write size, SWS * SGS the optimal I/O and the deallocation
	 * granularity. */
	if (nvme_ctrl_nr_streams(ctrl) && head->sws &&
	    (uint64_t)head->sws * lim->logical_block_size <= 0x40000000ULL) {
		uint32_t phys = head->sws * lim->logical_block_size;
		uint32_t atomic = lim->atomic_write_hw_max ?
					  lim->atomic_write_hw_max :
					  lim->logical_block_size;

		lim->physical_block_size = phys < atomic ? phys : atomic;
		lim->io_min = phys;
		if (head->sgs && (uint64_t)phys * head->sgs <= 0x40000000ULL) {
			lim->io_opt = phys * head->sgs;
			if (lim->max_discard_sectors)
				lim->discard_granularity = lim->io_opt;
		}
	}

	if ((ctrl->vwc & NVME_CTRL_VWC_PRESENT) && !info->no_vwc) {
		lim->features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA;
		lim->write_cache = 1;
		lim->fua = 1;
	} else {
		lim->write_cache = 0;
		lim->fua = 0;
	}
	if (info->is_rotational) {
		lim->features |= BLK_FEAT_ROTATIONAL;
		lim->rotational = 1;
	} else {
		lim->rotational = 0;
	}
	lim->features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT;

	/*
	 * PI, or plain metadata masquerading as "type 0" when a separate
	 * buffer can carry it; otherwise no block I/O on a namespace with
	 * metadata except PI (which the controller strips/inserts).
	 */
	if (!nvme_init_integrity(head, lim, info))
		capacity = 0;
	if (head->ms)
		lim->features |= BLK_FEAT_STABLE_WRITES;

	lim->max_write_streams = head->nr_plids;
	lim->write_stream_granularity =
		lim->max_write_streams ?
			(info->runs > 0xffffffffULL ? 0xffffffffu :
						      (uint32_t)info->runs) :
			0;

	/* DEAC only when deallocated blocks read back zeroes. */
	if ((id->dlfeat & NVME_NS_DLFEAT_READ_MASK) ==
		    NVME_NS_DLFEAT_READ_ZEROES &&
	    (id->dlfeat & NVME_NS_DLFEAT_WRITE_ZEROES)) {
		head->features |= NVME_NS_DEAC;
		lim->discard_zeroes_data = 1;
	} else {
		head->features &= ~(unsigned long)NVME_NS_DEAC;
		lim->discard_zeroes_data =
			(ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES) ? 1 : 0;
	}
	lim->secure_erase = 0;
	return capacity;
}

/* Mirror the limits into the whole-disk block device as the legacy
 * readers see them. */
void nvme_ns_fill_bdev(struct gendisk *disk, const struct blk_limits *lim,
		       bool ro)
{
	block_device_t *b = &disk->bdev;

	b->physical_sector_size = lim->physical_block_size;
	b->alignment_offset = lim->alignment_offset;
	b->io_min = lim->io_min;
	b->io_opt = lim->io_opt;
	b->max_sectors = lim->max_sectors;
	b->queue_depth = lim->queue_depth;
	b->discard_granularity = lim->discard_granularity;
	b->discard_max_sectors = lim->max_discard_sectors;
	b->write_zeroes_max_sectors = lim->max_write_zeroes_sectors;
	b->rotational = lim->rotational;
	b->flush_supported = lim->write_cache;
	b->fua_supported = lim->fua;
	b->discard_zeroes_data = lim->discard_zeroes_data;
	b->removable = 0;
	/* b->ro is the disk's write protection: disk_set_ro(), which every
	 * caller has just made, sets it together with GENHD_FL_RO -- writing
	 * it here as well could undo a racing change. */
	(void)ro;
}

/* Apply `lim' to the disk: the live queue's limits when it has a queue,
 * the registration limits always.  0 / -errno. */
int nvme_ns_commit_limits(struct gendisk *disk, struct blk_limits *lim)
{
	int ret = blk_validate_limits(lim);

	if (ret)
		return ret;
	if (disk->queue) {
		ret = queue_limits_commit_update(disk->queue, lim);
		if (ret)
			return ret;
	}
	disk->limits = *lim;
	return 0;
}

void nvme_ns_set_capacity(struct gendisk *disk, uint64_t capacity,
			  uint32_t lbs)
{
	if ((disk_flags(disk) & (GENHD_FL_UP | GENHD_FL_DEAD)) == GENHD_FL_UP)
		set_capacity_and_notify(disk, capacity);
	else
		disk->capacity = capacity;
	nvme_set_geometry(disk, capacity * (lbs / 512));
}

static int nvme_update_ns_info_generic(struct nvme_ns *ns,
				       struct nvme_ns_info *info)
{
	struct blk_limits lim;
	int ret;

	if (ns->disk->queue)
		queue_limits_start_update(ns->disk->queue, &lim);
	else
		lim = ns->disk->limits;
	if (!lim.logical_block_size)
		lim.logical_block_size = 512;
	if (!lim.physical_block_size)
		lim.physical_block_size = lim.logical_block_size;
	nvme_ns_set_ctrl_limits(ns->ctrl, ns->head, &lim);

	if (ns->disk->queue)
		blk_mq_freeze_queue(ns->disk->queue);
	ret = nvme_ns_commit_limits(ns->disk, &lim);
	disk_set_ro(ns->disk, nvme_ns_is_readonly(ns, info));
	if (ns->disk->queue)
		blk_mq_unfreeze_queue(ns->disk->queue);

	/* No block interface for these command sets. */
	if (!ret)
		ret = -ENODEV;
	return ret;
}

static int nvme_update_ns_info_block(struct nvme_ns *ns,
				     struct nvme_ns_info *info)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct blk_queue *q = ns->disk->queue;
	struct nvme_id_ns_nvm *nvm = NULL;
	struct nvme_id_ns *id;
	struct blk_limits lim;
	uint64_t capacity;
	unsigned int lbaf;
	bool zoned = false, ro;
	int ret;

	might_sleep();
	ret = nvme_identify_ns(ctrl, info->nsid, &id);
	if (ret)
		return ret;

	if (id->ncap == 0) {
		/* Not allocated or attached. */
		info->is_removed = true;
		ret = -ENXIO;
		goto out;
	}
	lbaf = nvme_lbaf_index(id->flbas);

	if (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_ELBAS) {
		ret = nvme_identify_ns_nvm(ctrl, info->nsid, &nvm);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			/* The NVM-specific data is optional information. */
			nvm = NULL;
			ret = 0;
		}
	}

	if (q)
		queue_limits_start_update(q, &lim);
	else
		lim = ns->disk->limits;

	if (ns->head->ids.csi == NVME_CSI_ZNS) {
		ret = nvme_query_zone_info(ns, lbaf, &lim);
		if (ret < 0)
			goto out;
		if (ret > 0)
			goto out; /* NVMe status: give up for now */
		zoned = true;
	}

	if (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_FDPS) {
		ret = nvme_query_fdp_info(ns, info);
		if (ret < 0)
			goto out;
		ret = 0;
	}

	nvme_ns_streams_update(ns);

	/* The format may change under I/O (Format NVM, resize): no request
	 * may be built with half old, half new values. */
	if (q)
		blk_mq_freeze_queue(q);
	capacity = nvme_ns_compute_limits(ns, id, nvm, info, &lim, zoned);
	ret = nvme_ns_commit_limits(ns->disk, &lim);
	if (ret) {
		if (q)
			blk_mq_unfreeze_queue(q);
		goto out;
	}
	ro = nvme_ns_is_readonly(ns, info);
	nvme_ns_set_capacity(ns->disk, capacity, lim.logical_block_size);
	disk_set_ro(ns->disk, ro);
	nvme_ns_fill_bdev(ns->disk, &lim, ro);
	nvme_set_bit(NVME_NS_READY, &ns->flags);
	if (q)
		blk_mq_unfreeze_queue(q);

	/* Before the disk is added too: the zone write plugs must exist
	 * before the first write can reach the disk. */
	if (zoned) {
		ret = blk_revalidate_disk_zones(ns->disk);
		if (ret && !nvme_first_scan(ns->disk))
			goto out;
	}
	ret = 0;
out:
	if (nvm)
		kfree(nvm);
	kfree(id);
	return ret;
}

static int nvme_update_ns_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	bool unsupported = false;
	int ret;

	might_sleep();
	/* Another controller's scan may update the same head right now
	 * (the head's format fields, the head disk's stacked limits). */
	mutex_lock(&ns->head->update_lock);
	switch (info->ids.csi) {
	case NVME_CSI_ZNS:
	case NVME_CSI_NVM:
		ret = nvme_update_ns_info_block(ns, info);
		break;
	default:
		nvme_ctrl_info(ns->ctrl,
			       "block device for nsid %u not supported (csi %u)\n",
			       info->nsid, info->ids.csi);
		ret = nvme_update_ns_info_generic(ns, info);
		break;
	}

	/* An unsupported feature hides the block interface; the generic
	 * character node still works. */
	if (ret == -ENODEV) {
		disk_set_flags(ns->disk,
			       GENHD_FL_HIDDEN | GENHD_FL_NO_PART_SCAN);
		nvme_set_bit(NVME_NS_READY, &ns->flags);
		unsupported = true;
		ret = 0;
	}

	if (!ret && nvme_ns_head_multipath(ns->head))
		nvme_mpath_stack_limits(ns, unsupported, ns->disk->capacity,
					nvme_ns_is_readonly(ns, info));
	mutex_unlock(&ns->head->update_lock);
	return ret;
}

int nvme_ns_get_unique_id(struct nvme_ns *ns, uint8_t id[16], int type)
{
	struct nvme_ns_ids *ids = &ns->head->ids;

	switch (type) {
	case 1: /* EUI-64 style: NGUID preferred, then EUI-64 */
		if (!nvme_mem_is_zero(ids->nguid, sizeof(ids->nguid))) {
			kmemcpy(id, ids->nguid, sizeof(ids->nguid));
			return sizeof(ids->nguid);
		}
		if (!nvme_mem_is_zero(ids->eui64, sizeof(ids->eui64))) {
			kmemcpy(id, ids->eui64, sizeof(ids->eui64));
			return sizeof(ids->eui64);
		}
		return -EINVAL;
	case 3: /* UUID / NGUID */
		if (!nvme_mem_is_zero(ids->uuid, sizeof(ids->uuid))) {
			kmemcpy(id, ids->uuid, sizeof(ids->uuid));
			return sizeof(ids->uuid);
		}
		if (!nvme_mem_is_zero(ids->nguid, sizeof(ids->nguid))) {
			kmemcpy(id, ids->nguid, sizeof(ids->nguid));
			return sizeof(ids->nguid);
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

/* ==========================================================================
 * Namespace heads
 * ========================================================================== */

bool nvme_is_unique_nsid(struct nvme_ctrl *ctrl, struct nvme_ns_head *head)
{
	return head->shared ||
	       (ctrl->oacs & NVME_CTRL_OACS_NS_MNGT_SUPP) ||
	       (ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA) ||
	       (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_CTRATT_NVM_SETS);
}

static struct nvme_ns_head *nvme_find_ns_head(struct nvme_ctrl *ctrl,
					      uint32_t nsid)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&ctrl->subsys->lock);
	list_for_each_entry(h, &ctrl->subsys->nsheads, entry) {
		/* Private namespaces may reuse NSIDs under some conditions:
		 * no shared head for them then. */
		if (h->ns_id != nsid || !nvme_is_unique_nsid(ctrl, h))
			continue;
		if (nvme_tryget_ns_head(h))
			return h;
	}
	return NULL;
}

static int nvme_subsys_check_duplicate_ids(struct nvme_subsystem *subsys,
					   struct nvme_ns_ids *ids)
{
	bool has_uuid = !nvme_mem_is_zero(ids->uuid, sizeof(ids->uuid));
	bool has_nguid = !nvme_mem_is_zero(ids->nguid, sizeof(ids->nguid));
	bool has_eui64 = !nvme_mem_is_zero(ids->eui64, sizeof(ids->eui64));
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (has_uuid &&
		    !kmemcmp(ids->uuid, h->ids.uuid, sizeof(ids->uuid)))
			return -EINVAL;
		if (has_nguid &&
		    !kmemcmp(ids->nguid, h->ids.nguid, sizeof(ids->nguid)))
			return -EINVAL;
		if (has_eui64 &&
		    !kmemcmp(ids->eui64, h->ids.eui64, sizeof(ids->eui64)))
			return -EINVAL;
	}
	return 0;
}

static int nvme_global_check_duplicate_ids(struct nvme_subsystem *this,
					   struct nvme_ns_ids *ids)
{
	struct nvme_subsystem *s;
	int ret = 0;

	/* Racy by design (no global lock over head creation): a sanity
	 * check only. */
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(s, &nvme_subsystems, entry) {
		if (s == this)
			continue;
		mutex_lock(&s->lock);
		ret = nvme_subsys_check_duplicate_ids(s, ids);
		mutex_unlock(&s->lock);
		if (ret)
			break;
	}
	mutex_unlock(&nvme_subsystems_lock);
	return ret;
}

static struct nvme_ns_head *nvme_alloc_ns_head(struct nvme_ctrl *ctrl,
					       struct nvme_ns_info *info,
					       int *err)
{
	struct nvme_ns_head *head;
	int ret = -ENOMEM;

	lockdep_assert_held(&ctrl->subsys->lock);
	head = (struct nvme_ns_head *)nvme_kzalloc(sizeof(*head));
	if (!head)
		goto out;
	ret = ida_alloc_min(&ctrl->subsys->ns_ida, 1);
	if (ret < 0)
		goto out_free_head;
	head->instance = ret;
	INIT_LIST_HEAD(&head->list);
	INIT_LIST_HEAD(&head->entry);
	mutex_init(&head->update_lock, "nvme_head_update");
	nvme_srcu_init(&head->srcu, "nvme_head_srcu");
	head->subsys = ctrl->subsys;
	head->ns_id = info->nsid;
	head->ids = info->ids;
	head->shared = info->is_shared;
	head->rotational = info->is_rotational;
	head->passthru_err_log_enabled = ctrl->passthru_err_log_enabled;
	refcount_set(&head->ref, 1);

	if (head->ids.csi) {
		ret = nvme_get_effects_log(ctrl, head->ids.csi, &head->effects);
		if (ret)
			goto out_ida;
	} else {
		head->effects = ctrl->effects;
	}

	ret = nvme_mpath_alloc_disk(ctrl, head);
	if (ret)
		goto out_ida;

	list_add_tail(&head->entry, &ctrl->subsys->nsheads);
	refcount_inc(&ctrl->subsys->ref);
	return head;

out_ida:
	ida_free(&ctrl->subsys->ns_ida, (unsigned int)head->instance);
out_free_head:
	kfree(head);
out:
	if (ret > 0)
		ret = blk_status_to_errno(nvme_error_status((uint16_t)ret));
	*err = ret;
	return NULL;
}

static int nvme_init_ns_head(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_ns_head *head = NULL;
	int ret;

	ret = nvme_global_check_duplicate_ids(ctrl->subsys, &info->ids);
	if (ret) {
		/*
		 * Two namespaces of two subsystems report the same ID.  For
		 * a multi-path capable shared namespace that is a recipe for
		 * data corruption: refuse it.  Otherwise (a cheap consumer
		 * device, most likely) clear the IDs and warn: by-id links
		 * may then point at either.
		 */
		if (ctrl->ops->print_device_info)
			ctrl->ops->print_device_info(ctrl);
		if ((ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) &&
		    info->is_shared) {
			nvme_ctrl_err(ctrl,
				      "ignoring nsid %u because of duplicate IDs\n",
				      info->nsid);
			return ret;
		}
		nvme_ctrl_err(ctrl, "clearing duplicate IDs for nsid %u\n",
			      info->nsid);
		nvme_ctrl_err(ctrl,
			      "use of /dev/disk/by-id/ may cause data corruption\n");
		kmemset(info->ids.nguid, 0, sizeof(info->ids.nguid));
		kmemset(info->ids.uuid, 0, sizeof(info->ids.uuid));
		kmemset(info->ids.eui64, 0, sizeof(info->ids.eui64));
		ctrl->quirks |= NVME_QUIRK_BOGUS_NID;
	}

	mutex_lock(&ctrl->subsys->lock);
	head = nvme_find_ns_head(ctrl, info->nsid);
	if (!head) {
		ret = nvme_subsys_check_duplicate_ids(ctrl->subsys, &info->ids);
		if (ret) {
			nvme_ctrl_err(ctrl,
				      "duplicate IDs in subsystem for nsid %u\n",
				      info->nsid);
			goto out_unlock;
		}
		head = nvme_alloc_ns_head(ctrl, info, &ret);
		if (!head)
			goto out_unlock;
	} else {
		ret = -EINVAL;
		if ((!info->is_shared || !head->shared) &&
		    !list_empty(&head->list)) {
			nvme_ctrl_err(ctrl, "duplicate unshared namespace %u\n",
				      info->nsid);
			goto out_put_ns_head;
		}
		if (!nvme_ns_ids_equal(&head->ids, &info->ids)) {
			nvme_ctrl_err(ctrl,
				      "IDs don't match for shared namespace %u\n",
				      info->nsid);
			goto out_put_ns_head;
		}
		if (!nvme_mpath_multipath_param()) {
			static int warned;

			nvme_ctrl_warn(ctrl,
				       "found shared namespace %u, but multipathing not supported\n",
				       info->nsid);
			if (nvme_first_time(&warned)) {
				nvme_ctrl_warn(ctrl,
					       "shared namespace support requires nvme_core.multipath=Y\n");
			}
		}
	}

	nvme_list_add_tail_srcu(&ns->siblings, &head->list);
	ns->head = head;
	mutex_unlock(&ctrl->subsys->lock);

	/* A pending delayed removal of the head's disk is moot now. */
	if (head->disk && __atomic_load_n(&head->remove_work_pending,
					  __ATOMIC_ACQUIRE))
		nvme_cancel_work_sync(&head->remove_work);
	return 0;

out_put_ns_head:
	nvme_put_ns_head(head);
out_unlock:
	mutex_unlock(&ctrl->subsys->lock);
	return ret;
}

/* ==========================================================================
 * Namespaces: lookup, add, remove
 * ========================================================================== */

struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, uint32_t nsid)
{
	struct nvme_ns *ns, *ret = NULL;
	int idx;

	idx = nvme_srcu_read_lock(&ctrl->ns_srcu);
	nvme_list_for_each_entry_srcu(ns, &ctrl->namespaces, list) {
		if (ns->head->ns_id == nsid) {
			if (!nvme_get_ns(ns))
				continue;
			ret = ns;
			break;
		}
		if (ns->head->ns_id > nsid)
			break;
	}
	nvme_srcu_read_unlock(&ctrl->ns_srcu, idx);
	return ret;
}

/* Insert sorted by NSID. */
static void nvme_ns_add_to_ctrl_list(struct nvme_ns *ns)
{
	struct nvme_ns *tmp;

	lockdep_assert_held(&ns->ctrl->namespaces_lock);
	list_for_each_entry_reverse(tmp, &ns->ctrl->namespaces, list) {
		if (tmp->head->ns_id < ns->head->ns_id) {
			nvme_list_add_srcu(&ns->list, &tmp->list);
			return;
		}
	}
	nvme_list_add_srcu(&ns->list, &ns->ctrl->namespaces);
}

/* Unlink `ns' from its head's path list (and the head from the subsystem
 * when it was the last path and nothing keeps it queueing).  true when it
 * was the last path. */
static bool nvme_ns_unlink_head(struct nvme_ns *ns, bool keep_if_queueing)
{
	struct nvme_ns_head *head = ns->head;
	bool last_path = false;

	mutex_lock(&ns->ctrl->subsys->lock);
	nvme_list_del_srcu(&ns->siblings);
	if (list_empty(&head->list)) {
		if (!keep_if_queueing || !nvme_mpath_queue_if_no_path(head))
			list_del_init(&head->entry);
		last_path = true;
	}
	mutex_unlock(&ns->ctrl->subsys->lock);
	return last_path;
}

/* Provisional names of disks allocated before their head is known (the
 * final name derives from the head instance). */
static uint32_t nvme_ns_alloc_seq;

static void nvme_alloc_ns(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
	struct blk_limits lim;
	struct nvme_ns *ns;
	struct gendisk *disk;
	char name[DISK_NAME_LEN];
	bool last_path;
	int ret;

	might_sleep();
	lockdep_assert_held(&ctrl->scan_lock);
	ns = (struct nvme_ns *)nvme_kzalloc(sizeof(*ns));
	if (!ns)
		return;

	kmemset(&lim, 0, sizeof(lim));
	lim.logical_block_size = 512;
	lim.physical_block_size = 512;
	if (ctrl->ops->supports_pci_p2pdma &&
	    ctrl->ops->supports_pci_p2pdma(ctrl))
		lim.features |= BLK_FEAT_PCI_P2PDMA;
	/* 512-byte blocks until nvme_update_ns_info() knows the format. */
	nvme_ns_set_ctrl_limits(ctrl, NULL, &lim);

	/*
	 * The disk and its queue exist before the namespace is published as
	 * a path of its head: a revalidation running for another controller
	 * of the subsystem may walk head->list at any time and must find
	 * ns->disk set.  The final name needs the head instance, so the disk
	 * starts under a provisional one.
	 */
	ksnprintf(name, sizeof(name), "nvnew%u",
		  __atomic_add_fetch(&nvme_ns_alloc_seq, 1, __ATOMIC_RELAXED));
	disk = blk_mq_alloc_disk(ctrl->tagset, &lim, &nvme_bdev_ops, name, ns);
	if (!disk) {
		nvme_ctrl_err(ctrl, "no disk for namespace %u\n", info->nsid);
		goto out_free_ns;
	}
	disk->private_data = ns;
	disk->queuedata = ns;
	disk->flags |= GENHD_FL_MEDIA_PRESENT;
	if (blk_kblockd_running())
		disk->flags |= GENHD_FL_HOTPLUGGED;
	/* The namespace's own references on its disk and queue: both stay
	 * valid until nvme_free_ns(), whatever disk_del() frees. */
	disk_get(disk);
	ns->disk = disk;
	ns->queue = disk->queue;
	if (ns->queue)
		blk_get_queue(ns->queue);
	ns->ctrl = ctrl;
	refcount_set(&ns->ref, 1);
	INIT_LIST_HEAD(&ns->list);
	INIT_LIST_HEAD(&ns->siblings);
	nvme_ns_set_ana_state(ns, NVME_ANA_OPTIMIZED);

	if (nvme_init_ns_head(ns, info))
		goto out_cleanup_disk;

	/*
	 * Under multipath every disk name derives from the subsystem
	 * instance (shared and private namespaces would collide otherwise);
	 * without it, from the controller instance (a shared namespace then
	 * shows up once per controller).
	 */
	if (nvme_ns_head_multipath(ns->head))
		ret = ksnprintf(name, sizeof(name), "nvme%dc%dn%d",
				ctrl->subsys->instance, ctrl->instance,
				ns->head->instance);
	else if (nvme_mpath_multipath_param())
		ret = ksnprintf(name, sizeof(name), "nvme%dn%d",
				ctrl->subsys->instance, ns->head->instance);
	else
		ret = ksnprintf(name, sizeof(name), "nvme%dn%d",
				ctrl->instance, ns->head->instance);
	if (ret < 0 || ret >= (int)sizeof(name)) {
		/* A cut name could collide with another disk's. */
		nvme_ctrl_err(ctrl, "disk name for namespace %u too long\n",
			      info->nsid);
		goto out_unlink_ns;
	}
	ret = disk_set_name(disk, name);
	if (ret) {
		nvme_ctrl_err(ctrl, "%s: naming the disk of namespace %u failed (%d)\n",
			      name, info->nsid, ret);
		goto out_unlink_ns;
	}

	/* A path disk of a multipath head is internal: no node, no
	 * partitions of its own (the head has them). */
	if (nvme_ns_head_multipath(ns->head))
		disk->flags |= GENHD_FL_HIDDEN | GENHD_FL_NO_PART_SCAN;
	nvme_fill_disk_identity(ctrl, ns->head, disk);

	if (nvme_update_ns_info(ns, info))
		goto out_unlink_ns;
	disk->bdev.ready = 1;

	mutex_lock(&ctrl->namespaces_lock);
	/* No namespace joins the list once the queues are frozen: a scan
	 * racing a reset would deadlock on the freeze otherwise. */
	if (nvme_test_bit(NVME_CTRL_FROZEN, &ctrl->flags)) {
		mutex_unlock(&ctrl->namespaces_lock);
		goto out_unlink_ns;
	}
	nvme_ns_add_to_ctrl_list(ns);
	mutex_unlock(&ctrl->namespaces_lock);
	nvme_srcu_synchronize(&ctrl->ns_srcu);
	nvme_get_ctrl(ctrl);

	/* The namespace directory (and the disk's device link) before the
	 * disk becomes visible. */
	nvme_sysfs_add_ns(ns);
	if (disk_add(disk)) {
		nvme_ctrl_err(ctrl, "%s: registering the disk failed\n", name);
		nvme_sysfs_del_ns(ns);
		goto out_cleanup_ns_from_list;
	}

	if (!nvme_ns_head_multipath(ns->head))
		nvme_cdev_add_ns(ns);
	nvme_mpath_add_disk(ns, info->anagrpid);
	nvme_fault_inject_init(&ns->fault_inject, disk->disk_name);
	return;

out_cleanup_ns_from_list:
	nvme_put_ctrl(ctrl);
	mutex_lock(&ctrl->namespaces_lock);
	nvme_list_del_srcu(&ns->list);
	mutex_unlock(&ctrl->namespaces_lock);
	nvme_srcu_synchronize(&ctrl->ns_srcu);
out_unlink_ns:
	/* No path selection may keep (or pick) this namespace. */
	nvme_clear_bit(NVME_NS_READY, &ns->flags);
	if (nvme_mpath_clear_current_path(ns))
		nvme_srcu_synchronize(&ns->head->srcu);
	last_path = nvme_ns_unlink_head(ns, true);
	nvme_srcu_synchronize(&ns->head->srcu);
	/* The last path gone: the multipath disk goes too (removed when it
	 * was live, its reference on the head dropped -- or the delayed
	 * removal armed); then the namespace's own head reference. */
	if (last_path)
		nvme_mpath_remove_disk(ns->head);
	nvme_put_ns_head(ns->head);
out_cleanup_disk:
	if (ns->queue)
		blk_put_queue(ns->queue);
	disk_put(disk); /* the namespace's reference */
	disk_put(disk); /* never added: the allocation's */
out_free_ns:
	kfree(ns);
}

void nvme_ns_remove(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;
	struct nvme_ctrl *ctrl = ns->ctrl;
	bool last_path;

	might_sleep();
	if (nvme_test_and_set_bit(NVME_NS_REMOVING, &ns->flags))
		return;

	nvme_clear_bit(NVME_NS_READY, &ns->flags);
	/* I/O that still gets in is failed by the block layer's range
	 * check, not sent to a namespace that is going away. */
	disk_set_capacity(ns->disk, 0);
	nvme_fault_inject_fini(&ns->fault_inject);

	/* !READY is visible to every path selection from here on ... */
	nvme_srcu_synchronize(&head->srcu);
	/* ... and no submitter still uses a cached current path to it. */
	if (nvme_mpath_clear_current_path(ns))
		nvme_srcu_synchronize(&head->srcu);

	last_path = nvme_ns_unlink_head(ns, true);
	/* Not reachable through head->list any more. */
	nvme_srcu_synchronize(&head->srcu);

	if (!nvme_ns_head_multipath(head))
		nvme_cdev_del_ns(ns);
	nvme_mpath_remove_sysfs_link(ns);
	nvme_sysfs_del_ns(ns);

	/* Added (a disk marked dead by a surprise removal is still
	 * registered and must go). */
	if (disk_flags(ns->disk) & GENHD_FL_UP)
		disk_del(ns->disk);

	mutex_lock(&ctrl->namespaces_lock);
	nvme_list_del_srcu(&ns->list);
	mutex_unlock(&ctrl->namespaces_lock);
	nvme_srcu_synchronize(&ctrl->ns_srcu);

	if (last_path)
		nvme_mpath_remove_disk(head);
	nvme_put_ns(ns);
}

static void nvme_ns_remove_by_nsid(struct nvme_ctrl *ctrl, uint32_t nsid)
{
	struct nvme_ns *ns = nvme_find_get_ns(ctrl, nsid);

	if (ns) {
		nvme_ns_remove(ns);
		nvme_put_ns(ns);
	}
}

static void nvme_validate_ns(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	int ret = NVME_SC_INVALID_NS | NVME_STATUS_DNR;

	if (!nvme_ns_ids_equal(&ns->head->ids, &info->ids)) {
		nvme_ctrl_err(ns->ctrl, "identifiers changed for nsid %u\n",
			      ns->head->ns_id);
		goto out;
	}
	ret = nvme_update_ns_info(ns, info);
out:
	/* Only a fatal answer of the device removes the namespace; other
	 * errors leave it as it was until the next scan. */
	if (ret > 0 && (ret & NVME_STATUS_DNR))
		nvme_ns_remove(ns);
}

/* Fill `info' for `nsid': descriptors, then the command set independent
 * or NVM identify.  0 when the namespace is usable now; > 0 / -errno. */
static int nvme_ns_info_get(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
	int ret = 1;

	if (nvme_identify_ns_descs(ctrl, info))
		return -EIO;

	if (info->ids.csi != NVME_CSI_NVM && !nvme_multi_css(ctrl)) {
		nvme_ctrl_warn(ctrl, "command set not reported for nsid: %u\n",
			       info->nsid);
		return -EINVAL;
	}

	/* The command set independent data where it exists, else the
	 * NVM identify data. */
	if ((ctrl->cap & NVME_CAP_CRMS_CRIMS) ||
	    (info->ids.csi != NVME_CSI_NVM && info->ids.csi != NVME_CSI_ZNS) ||
	    ctrl->vs >= NVME_VS(2, 0, 0))
		ret = nvme_ns_info_from_id_cs_indep(ctrl, info);
	if (ret > 0)
		ret = nvme_ns_info_from_identify(ctrl, info);
	return ret;
}

static void nvme_scan_ns(struct nvme_ctrl *ctrl, uint32_t nsid)
{
	struct nvme_ns_info info;
	struct nvme_ns *ns;
	int ret;

	kmemset(&info, 0, sizeof(info));
	info.nsid = nsid;
	ret = nvme_ns_info_get(ctrl, &info);
	if (ret == -EIO || ret == -EINVAL)
		return;

	if (info.is_removed)
		nvme_ns_remove_by_nsid(ctrl, nsid);

	/* Not ready: an event comes when it is, and the scan reruns. */
	if (ret || !info.is_ready)
		return;

	ns = nvme_find_get_ns(ctrl, nsid);
	if (ns) {
		nvme_validate_ns(ns, &info);
		nvme_put_ns(ns);
	} else {
		nvme_alloc_ns(ctrl, &info);
	}
}

int nvme_revalidate_ns(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ns_info local;
	int ret;

	might_sleep();
	if (nvme_test_bit(NVME_NS_REMOVING, &ns->flags))
		return -ENODEV;
	if (!info) {
		kmemset(&local, 0, sizeof(local));
		local.nsid = ns->head->ns_id;
		ret = nvme_ns_info_get(ns->ctrl, &local);
		if (local.is_removed) {
			nvme_ns_remove(ns);
			return -ENODEV;
		}
		if (ret)
			return ret;
		if (!local.is_ready)
			return -EAGAIN;
		info = &local;
	}
	mutex_lock(&ns->ctrl->scan_lock);
	if (!nvme_ns_ids_equal(&ns->head->ids, &info->ids)) {
		ret = NVME_SC_INVALID_NS | NVME_STATUS_DNR;
		nvme_ctrl_err(ns->ctrl, "identifiers changed for nsid %u\n",
			      ns->head->ns_id);
	} else {
		ret = nvme_update_ns_info(ns, info);
	}
	if (ret > 0 && (ret & NVME_STATUS_DNR))
		nvme_ns_remove(ns);
	mutex_unlock(&ns->ctrl->scan_lock);
	return ret;
}

/* Take a reference on the first namespace (by NSID order) above `nsid'
 * that is not being removed, or NULL. */
static struct nvme_ns *nvme_get_ns_above(struct nvme_ctrl *ctrl, uint32_t nsid,
					 bool any)
{
	struct nvme_ns *ns, *found = NULL;

	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (!any && ns->head->ns_id <= nsid)
			continue;
		if (nvme_test_bit(NVME_NS_REMOVING, &ns->flags))
			continue;
		if (nvme_get_ns(ns)) {
			found = ns;
			break;
		}
	}
	mutex_unlock(&ctrl->namespaces_lock);
	return found;
}

/* Remove every namespace above `nsid' (no longer reported active).
 * nvme_ns_remove() unlinks each from the list itself. */
static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,
					   uint32_t nsid)
{
	struct nvme_ns *ns;

	lockdep_assert_held(&ctrl->scan_lock);
	while ((ns = nvme_get_ns_above(ctrl, nsid, false)) != NULL) {
		nvme_ns_remove(ns);
		nvme_put_ns(ns);
	}
}

/* Remove the namespaces with lo < NSID < hi: the gaps of the active
 * list.  One walk over the existing namespaces, not one lookup per
 * missing NSID. */
static void nvme_remove_ns_range(struct nvme_ctrl *ctrl, uint32_t lo,
				 uint32_t hi)
{
	struct nvme_ns *ns, *found;

	lockdep_assert_held(&ctrl->scan_lock);
	if (hi <= lo + 1)
		return;
	for (;;) {
		found = NULL;
		mutex_lock(&ctrl->namespaces_lock);
		list_for_each_entry(ns, &ctrl->namespaces, list) {
			uint32_t id = ns->head->ns_id;

			if (id <= lo)
				continue;
			if (id >= hi)
				break;
			if (nvme_test_bit(NVME_NS_REMOVING, &ns->flags))
				continue;
			if (nvme_get_ns(ns)) {
				found = ns;
				break;
			}
		}
		mutex_unlock(&ctrl->namespaces_lock);
		if (!found)
			return;
		nvme_ns_remove(found);
		nvme_put_ns(found);
	}
}

/* ==========================================================================
 * The scan
 * ========================================================================== */

static int nvme_scan_ns_list(struct nvme_ctrl *ctrl)
{
	const unsigned int nr_entries = NVME_IDENTIFY_DATA_SIZE / sizeof(__le32);
	__le32 *ns_list;
	uint32_t prev = 0;
	int ret = 0;

	lockdep_assert_held(&ctrl->scan_lock);
	ns_list = (__le32 *)nvme_kzalloc(NVME_IDENTIFY_DATA_SIZE);
	if (!ns_list)
		return -ENOMEM;

	for (;;) {
		ret = nvme_identify_active_ns_list(ctrl, prev, ns_list);
		if (ret) {
			nvme_ctrl_warn(ctrl,
				       "Identify NS List failed (status=0x%x)\n",
				       (unsigned int)ret);
			goto free;
		}
		for (unsigned int i = 0; i < nr_entries; i++) {
			uint32_t nsid = le32_to_cpu(ns_list[i]);

			if (!nsid) /* end of the list */
				goto out;
			if (WARN_ON_ONCE(nsid <= prev ||
					 nsid == NVME_NSID_ALL)) {
				/* Not ascending: a broken list.  Keep what
				 * was found, remove nothing more. */
				goto free;
			}
			nvme_scan_ns(ctrl, nsid);
			nvme_remove_ns_range(ctrl, prev, nsid);
			prev = nsid;
		}
		/* A full page: the next starts after its last NSID. */
	}
out:
	nvme_remove_invalid_namespaces(ctrl, prev);
free:
	kfree(ns_list);
	return ret;
}

static void nvme_scan_ns_sequential(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl *id;
	uint32_t nn, i;

	lockdep_assert_held(&ctrl->scan_lock);
	if (nvme_identify_ctrl(ctrl, &id))
		return;
	nn = le32_to_cpu(id->nn);
	kfree(id);

	for (i = 1; i <= nn && i != NVME_NSID_ALL; i++)
		nvme_scan_ns(ctrl, i);
	nvme_remove_invalid_namespaces(ctrl, nn);
}

static void nvme_clear_changed_ns_log(struct nvme_ctrl *ctrl)
{
	size_t log_size = NVME_MAX_CHANGED_NAMESPACES * sizeof(__le32);
	__le32 *log;
	int error;

	log = (__le32 *)nvme_kzalloc(log_size);
	if (!log)
		return;
	/*
	 * Reading the log clears the event; its contents are not trusted
	 * for what changed (userland may have read the page first): the
	 * whole list is rescanned.
	 */
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0,
			     NVME_CSI_NVM, log, log_size, 0);
	if (error)
		nvme_ctrl_warn(ctrl, "reading changed ns log failed: %d\n",
			       error);
	kfree(log);
}

void nvme_queue_scan(struct nvme_ctrl *ctrl)
{
	/* Only with the admin AND the I/O queues alive. */
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE && ctrl->tagset)
		nvme_queue_work(&ctrl->scan_work);
}

void nvme_scan_work(void *arg)
{
	struct nvme_ctrl *ctrl = (struct nvme_ctrl *)arg;
	int ret;

	might_sleep();
	BUG_ON(!ctrl);
	/* A live controller without a tag set could not create I/O
	 * queues: nothing to scan. */
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE || !ctrl->tagset)
		return;

	ret = nvme_init_non_mdts_limits(ctrl);
	if (ret < 0) {
		nvme_ctrl_warn(ctrl, "reading non-mdts-limits failed: %d\n",
			       ret);
		return;
	}

	/* The fallback DSM range page (one per controller). */
	if (!ctrl->discard_page)
		ctrl->discard_page = nvme_kzalloc(NVME_CTRL_PAGE_SIZE);

	if (nvme_test_and_clear_bit(NVME_AER_NOTICE_NS_CHANGED_EVENT,
				    &ctrl->events)) {
		nvme_ctrl_info(ctrl, "rescanning namespaces.\n");
		nvme_clear_changed_ns_log(ctrl);
	}

	mutex_lock(&ctrl->scan_lock);
	if (!nvme_id_cns_ok(ctrl, NVME_ID_CNS_NS_ACTIVE_LIST)) {
		nvme_scan_ns_sequential(ctrl);
	} else {
		/* Devices whose version promises the list but who fail it
		 * with Do Not Retry get the sequential scan. */
		ret = nvme_scan_ns_list(ctrl);
		if (ret > 0 && (ret & NVME_STATUS_DNR))
			nvme_scan_ns_sequential(ctrl);
	}
	mutex_unlock(&ctrl->scan_lock);

	/* An event that arrived meanwhile: scan again.  Otherwise re-read
	 * the ANA log so no state change was missed. */
	if (nvme_test_bit(NVME_AER_NOTICE_NS_CHANGED_EVENT, &ctrl->events))
		nvme_queue_scan(ctrl);
	else if (ctrl->ana_log_buf)
		nvme_queue_work(&ctrl->ana_work);
}

/*
 * Remove every namespace (controller removal, dead controller).  The
 * list is spliced off and walked without any lock held across the
 * removals, so a failed controller can be torn down; the state
 * (DELETING / DELETING_NOIO by now) keeps the scan from adding
 * namespaces.
 */
void nvme_remove_namespaces(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns, *next;
	LIST_HEAD(ns_list);

	might_sleep();
	/* I/O the scan itself issued must complete for the scan to end:
	 * requeue multipath I/O, unquiesce the queues. */
	nvme_mpath_clear_ctrl_paths(ctrl);
	nvme_unquiesce_io_queues(ctrl);

	/* No scan running from here on. */
	nvme_flush_work(&ctrl->scan_work);

	/* Not gracefully disconnected: nothing can be flushed while the
	 * disks go away, fail every queue now. */
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_DEAD)
		nvme_mark_namespaces_dead(ctrl);

	/* No-op when called from the reset handler. */
	nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING_NOIO);

	/*
	 * Take the whole list private, then remove without scan_lock held:
	 * a removal may wait long for I/O to drain, and nothing that takes
	 * scan_lock (revalidation, passthrough with namespace effects) may
	 * queue behind it.  The state keeps the scan from adding namespaces.
	 * Readers still on the old list walk off its last entry, which keeps
	 * pointing at the controller's (now empty) list head until they are
	 * gone; only then does the private head take its place.
	 */
	mutex_lock(&ctrl->namespaces_lock);
	if (!list_empty(&ctrl->namespaces)) {
		struct list_head *first = ctrl->namespaces.next;
		struct list_head *last = ctrl->namespaces.prev;

		ctrl->namespaces.prev = &ctrl->namespaces;
		__atomic_store_n(&ctrl->namespaces.next, &ctrl->namespaces,
				 __ATOMIC_RELEASE);
		nvme_srcu_synchronize(&ctrl->ns_srcu);
		first->prev = &ns_list;
		last->next = &ns_list;
		ns_list.next = first;
		ns_list.prev = last;
	}
	mutex_unlock(&ctrl->namespaces_lock);
	nvme_srcu_synchronize(&ctrl->ns_srcu);

	list_for_each_entry_safe(ns, next, &ns_list, list)
		nvme_ns_remove(ns);

	if (ctrl->discard_page) {
		/* No namespace, no DSM in flight. */
		WARN_ON(nvme_test_bit(0, &ctrl->discard_page_busy));
		kfree(ctrl->discard_page);
		ctrl->discard_page = NULL;
	}
}

/* ==========================================================================
 * Block device operations of a namespace disk
 * ========================================================================== */

static struct nvme_ns *nvme_bdev_ns(block_device_t *bdev)
{
	struct gendisk *disk = bdev ? bdev->disk : NULL;

	if (!disk)
		return NULL;
	return nvme_disk_to_ns(disk);
}

static int nvme_open(block_device_t *bdev, int flags)
{
	struct nvme_ns *ns = nvme_bdev_ns(bdev);

	(void)flags;
	if (!ns)
		return -ENXIO;
	/* Hidden path disks of a multipath head have no node. */
	if (WARN_ON_ONCE(nvme_ns_head_multipath(ns->head)))
		return -ENXIO;
	if (!nvme_get_ns(ns))
		return -ENXIO;
	return 0;
}

static void nvme_release(block_device_t *bdev)
{
	struct nvme_ns *ns = nvme_bdev_ns(bdev);

	if (ns)
		nvme_put_ns(ns);
}

static long nvme_ioctl_mode(block_device_t *bdev, unsigned long cmd,
			    void *argp, struct task *cur, int open_flags)
{
	struct nvme_ns *ns = nvme_bdev_ns(bdev);

	if (!ns)
		return -ENXIO;
	/* Through a partition node: no unprivileged pass-through. */
	return nvme_ns_ioctl(ns, cmd, argp, cur,
			     nvme_ioctl_open_flags(bdev, open_flags));
}

static int nvme_report_zones(struct gendisk *disk, uint64_t sector,
			     unsigned int nr_zones, report_zones_cb cb,
			     void *data)
{
	return nvme_ns_report_zones(nvme_disk_to_ns(disk), sector, nr_zones,
				    cb, data);
}

static int nvme_get_unique_id(struct gendisk *disk, uint8_t id[16], int type)
{
	return nvme_ns_get_unique_id(nvme_disk_to_ns(disk), id, type);
}

static int nvme_bdev_revalidate(struct gendisk *disk)
{
	struct nvme_ns *ns = nvme_disk_to_ns(disk);
	int ret;

	if (!ns || !nvme_get_ns(ns))
		return -ENXIO;
	ret = nvme_revalidate_ns(ns, NULL);
	nvme_put_ns(ns);
	if (ret > 0)
		ret = blk_status_to_errno(nvme_error_status((uint16_t)ret));
	return ret;
}

const struct blk_ops nvme_bdev_ops = {
	.name = "nvme",
	.ioctl_mode = nvme_ioctl_mode,
	.open = nvme_open,
	.release = nvme_release,
	.revalidate = nvme_bdev_revalidate,
	.report_zones = nvme_report_zones,
	.pr_ops = &nvme_pr_ops,
	.get_unique_id = nvme_get_unique_id,
};

/* ==========================================================================
 * Namespace management, attachment, Format NVM, Sanitize
 * ========================================================================== */

/* An admin command with its own timeout (0: the admin default) and the
 * command's dword 0 in *result.  NVMe status / -errno.  May sleep. */
static int nvme_ns_admin_exec(struct nvme_ctrl *ctrl, struct nvme_command *c,
			      void *buf, unsigned int len, uint32_t timeout_ms,
			      uint32_t *result)
{
	struct blk_request *rq;
	int err = 0, ret;

	might_sleep();
	rq = blk_mq_alloc_request(ctrl->admin_q,
				  nvme_is_write(c) ? BLK_OP_DRV_OUT :
						     BLK_OP_DRV_IN,
				  0, &err);
	if (!rq)
		return err ? err : -ENOMEM;
	nvme_init_request(rq, c);
	if (timeout_ms)
		rq->timeout_ms = timeout_ms;
	if (buf && len) {
		ret = blk_rq_map_kern(rq, buf, len);
		if (ret) {
			blk_mq_free_request(rq);
			return ret;
		}
	}
	ret = nvme_execute_rq(rq, false);
	if (result && ret >= 0)
		*result = le32_to_cpu(nvme_req(rq)->result.u32);
	blk_mq_free_request(rq);
	return ret;
}

/* Run an admin command that changes the namespace inventory or contents
 * with the effects handling of pass-through (freeze while it runs,
 * rescan afterwards), then wait for the rescan. */
static int nvme_ns_admin_effects_exec(struct nvme_ctrl *ctrl,
				      struct nvme_command *c, void *buf,
				      unsigned int len, uint32_t timeout_ms,
				      uint32_t *result)
{
	uint32_t effects;
	int ret;

	might_sleep();
	effects = nvme_passthru_start(ctrl, NULL, c->common.opcode);
	ret = nvme_ns_admin_exec(ctrl, c, buf, len, timeout_ms, result);
	nvme_passthru_end(ctrl, NULL, effects, c, ret);
	if (!ret) {
		nvme_queue_scan(ctrl);
		nvme_flush_work(&ctrl->scan_work);
	}
	return ret;
}

int nvme_ns_create(struct nvme_ctrl *ctrl, const struct nvme_id_ns *id,
		   uint8_t csi, uint32_t *nsid_out)
{
	struct nvme_ns_mgmt_host_sw_specified *d;
	struct nvme_command c;
	uint32_t result = 0;
	int ret;

	might_sleep();
	if (!(ctrl->oacs & NVME_CTRL_OACS_NS_MNGT_SUPP))
		return -EOPNOTSUPP;
	if (!id || !id->nsze || le64_to_cpu(id->ncap) > le64_to_cpu(id->nsze))
		return -EINVAL;
	if (csi != NVME_CSI_NVM && !nvme_multi_css(ctrl))
		return -EOPNOTSUPP;

	d = (struct nvme_ns_mgmt_host_sw_specified *)nvme_kzalloc(sizeof(*d));
	if (!d)
		return -ENOMEM;
	d->nsze = id->nsze;
	d->ncap = id->ncap;
	d->flbas = id->flbas;
	d->dps = id->dps;
	d->nmic = id->nmic;
	d->anagrpid = id->anagrpid;
	d->nvmsetid = id->nvmsetid;
	d->endgid = id->endgid;

	kmemset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_ns_mgmt;
	c.common.cdw10 = cpu_to_le32(NVME_NS_MGMT_SEL_CREATE);
	c.common.cdw11 = cpu_to_le32((uint32_t)csi << NVME_NS_MGMT_CSI_SHIFT);
	ret = nvme_ns_admin_effects_exec(ctrl, &c, d, sizeof(*d), 0, &result);
	kfree(d);
	if (!ret) {
		if (nsid_out)
			*nsid_out = result;
		nvme_ctrl_info(ctrl, "created namespace %u\n", result);
	}
	return ret;
}

int nvme_ns_delete(struct nvme_ctrl *ctrl, uint32_t nsid)
{
	struct nvme_command c;
	int ret;

	might_sleep();
	if (!(ctrl->oacs & NVME_CTRL_OACS_NS_MNGT_SUPP))
		return -EOPNOTSUPP;
	if (!nsid)
		return -EINVAL;
	kmemset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_ns_mgmt;
	c.common.nsid = cpu_to_le32(nsid);
	c.common.cdw10 = cpu_to_le32(NVME_NS_MGMT_SEL_DELETE);
	ret = nvme_ns_admin_effects_exec(ctrl, &c, NULL, 0, 0, NULL);
	if (!ret)
		nvme_ctrl_info(ctrl, "deleted namespace %u\n", nsid);
	return ret;
}

int nvme_ns_attach(struct nvme_ctrl *ctrl, uint32_t nsid, bool attach,
		   const uint16_t *ctrl_ids, unsigned int nr)
{
	struct nvme_ctrl_list *list;
	struct nvme_command c;
	int ret;

	might_sleep();
	if (!(ctrl->oacs & NVME_CTRL_OACS_NS_MNGT_SUPP))
		return -EOPNOTSUPP;
	if (!nsid || nsid == NVME_NSID_ALL || nr > 2047 || (nr && !ctrl_ids))
		return -EINVAL;
	list = (struct nvme_ctrl_list *)nvme_kzalloc(sizeof(*list));
	if (!list)
		return -ENOMEM;
	if (nr) {
		list->num = cpu_to_le16((uint16_t)nr);
		for (unsigned int i = 0; i < nr; i++)
			list->identifier[i] = cpu_to_le16(ctrl_ids[i]);
	} else {
		/* This controller. */
		list->num = cpu_to_le16(1);
		list->identifier[0] = cpu_to_le16(ctrl->cntlid);
	}

	kmemset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_ns_attach;
	c.common.nsid = cpu_to_le32(nsid);
	c.common.cdw10 = cpu_to_le32(attach ? NVME_NS_ATTACH_SEL_ATTACH :
					      NVME_NS_ATTACH_SEL_DETACH);
	ret = nvme_ns_admin_effects_exec(ctrl, &c, list, sizeof(*list), 0,
					 NULL);
	kfree(list);
	return ret;
}

int nvme_format_ns(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t lbaf,
		   uint8_t ses, uint8_t pi, uint8_t pil, uint8_t mset)
{
	struct nvme_command c;
	uint32_t cdw10;

	might_sleep();
	if (!(ctrl->oacs & NVME_CTRL_OACS_FORMAT))
		return -EOPNOTSUPP;
	if (lbaf > 63 || ses > 2 || pi > 3)
		return -EINVAL;
	if (ses == 2 && !(ctrl->fna & NVME_CTRL_FNA_CRYPTO_ERASE))
		return -EOPNOTSUPP;
	if (nsid == NVME_NSID_ALL &&
	    (ctrl->fna & NVME_CTRL_FNA_NSID_FFFFFFFF))
		return -EOPNOTSUPP;

	cdw10 = NVME_FORMAT_LBAF(lbaf) | (mset ? NVME_FORMAT_MSET : 0) |
		((uint32_t)pi << NVME_FORMAT_PI_SHIFT) |
		(pil ? NVME_FORMAT_PIL : 0) |
		((uint32_t)ses << NVME_FORMAT_SES_SHIFT);
	kmemset(&c, 0, sizeof(c));
	c.format.opcode = nvme_admin_format_nvm;
	c.format.nsid = cpu_to_le32(nsid);
	c.format.cdw10 = cpu_to_le32(cdw10);
	return nvme_ns_admin_effects_exec(ctrl, &c, NULL, 0,
					  NVME_FORMAT_TIMEOUT_MS, NULL);
}

int nvme_sanitize_progress(struct nvme_ctrl *ctrl, uint32_t *percent,
			   uint16_t *sstat)
{
	struct nvme_sanitize_log_page *log;
	uint16_t sprog;
	int ret;

	might_sleep();
	log = (struct nvme_sanitize_log_page *)nvme_kzalloc(sizeof(*log));
	if (!log)
		return -ENOMEM;
	ret = nvme_get_log(ctrl, 0, NVME_LOG_SANITIZE, 0, NVME_CSI_NVM, log,
			   sizeof(*log), 0);
	if (!ret) {
		sprog = le16_to_cpu(log->sprog);
		if (percent)
			*percent = sprog == NVME_SANITIZE_SPROG_DONE ?
					   100 :
					   (uint32_t)sprog * 100u / 65536u;
		if (sstat)
			*sstat = le16_to_cpu(log->sstat);
	}
	kfree(log);
	return ret;
}

/* Is the sanitize action (and its no-deallocate option) supported? */
static int nvme_sanitize_check(struct nvme_ctrl *ctrl, uint32_t cdw10)
{
	uint32_t cap = ctrl->sanicap;

	switch (cdw10 & NVME_SANACT_MASK) {
	case NVME_SANACT_EXIT_FAILURE:
		break;
	case NVME_SANACT_BLOCK_ERASE:
		if (!(cap & NVME_CTRL_SANICAP_BES))
			return -EOPNOTSUPP;
		break;
	case NVME_SANACT_OVERWRITE:
		if (!(cap & NVME_CTRL_SANICAP_OWS))
			return -EOPNOTSUPP;
		break;
	case NVME_SANACT_CRYPTO_ERASE:
		if (!(cap & NVME_CTRL_SANICAP_CES))
			return -EOPNOTSUPP;
		break;
	case NVME_SANACT_EXIT_MEDIA_VERIF:
		if (!(cap & NVME_CTRL_SANICAP_VERS))
			return -EOPNOTSUPP;
		break;
	default:
		return -EINVAL;
	}
	/* No-deallocate after sanitize may be inhibited. */
	if ((cdw10 & NVME_SANITIZE_NODAS) && (cap & NVME_CTRL_SANICAP_NDI))
		return -EOPNOTSUPP;
	return 0;
}

int nvme_sanitize(struct nvme_ctrl *ctrl, uint32_t cdw10, uint32_t ovrpat)
{
	struct nvme_command c;
	uint32_t act = cdw10 & NVME_SANACT_MASK, pct = 0;
	uint16_t sstat = 0;
	uint64_t seen;
	int ret;

	might_sleep();
	if (!ctrl->sanicap)
		return -EOPNOTSUPP;
	ret = nvme_sanitize_check(ctrl, cdw10);
	if (ret)
		return ret;

	kmemset(&c, 0, sizeof(c));
	c.sanitize.opcode = nvme_admin_sanitize_nvm;
	c.sanitize.cdw10 = cpu_to_le32(cdw10);
	c.sanitize.ovrpat = cpu_to_le32(ovrpat);
	seen = __atomic_load_n(&nvme_sanitize_events, __ATOMIC_ACQUIRE);
	ret = nvme_ns_admin_effects_exec(ctrl, &c, NULL, 0, 0, NULL);
	if (ret)
		return ret;
	if (act == NVME_SANACT_EXIT_FAILURE ||
	    act == NVME_SANACT_EXIT_MEDIA_VERIF)
		return 0;

	nvme_ctrl_info(ctrl, "sanitize (action %u) started\n", act);
	/* The operation runs in the background: follow the sanitize status
	 * log (and its completion event) until it is no longer in
	 * progress.  A signal ends the wait, not the sanitize. */
	for (;;) {
		long r;

		ret = nvme_sanitize_progress(ctrl, &pct, &sstat);
		if (ret)
			break;
		if ((sstat & NVME_SANITIZE_SSTAT_STATUS_MASK) !=
		    NVME_SANITIZE_SSTAT_STATUS_IN_PROGESS) {
			if ((sstat & NVME_SANITIZE_SSTAT_STATUS_MASK) ==
			    NVME_SANITIZE_SSTAT_STATUS_COMPLETED_FAILED) {
				nvme_ctrl_err(ctrl, "sanitize failed\n");
				ret = -EIO;
			} else {
				nvme_ctrl_info(ctrl, "sanitize completed\n");
			}
			break;
		}
		if (nvme_state_terminal(ctrl) ||
		    nvme_ctrl_state(ctrl) == NVME_CTRL_DELETING) {
			ret = -ENODEV;
			break;
		}
		r = wait_event_interruptible_timeout(
			nvme_sanitize_wq,
			__atomic_load_n(&nvme_sanitize_events,
					__ATOMIC_ACQUIRE) != seen,
			NVME_SANITIZE_POLL_MS);
		if (r < 0)
			return (int)r;
		seen = __atomic_load_n(&nvme_sanitize_events, __ATOMIC_ACQUIRE);
	}
	/* Every namespace's contents changed. */
	nvme_queue_scan(ctrl);
	return ret;
}

void nvme_sanitize_event(struct nvme_ctrl *ctrl, uint32_t aen_result)
{
	uint32_t pct = 0;
	uint16_t sstat = 0;

	might_sleep();
	/* Reading the sanitize log clears the event. */
	if (!nvme_sanitize_progress(ctrl, &pct, &sstat))
		nvme_ctrl_info(ctrl,
			       "sanitize event %08x: status %u, %u%% done\n",
			       aen_result,
			       sstat & NVME_SANITIZE_SSTAT_STATUS_MASK, pct);
	__atomic_add_fetch(&nvme_sanitize_events, 1, __ATOMIC_RELEASE);
	wake_up(&nvme_sanitize_wq);
	nvme_queue_scan(ctrl);
}
