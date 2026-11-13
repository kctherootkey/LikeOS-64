// LikeOS - NVMe over PCI Express: probe, reset, removal, shutdown, error handling
//
// Bring-up happens in two stages, the way the established driver probes
// asynchronously:
//
//   1. PROBE (pci_register_driver from nvme_init: the boot thread, before
//      interrupts and the scheduler): the controller object (struct
//      nvme_dev with the core's nvme_ctrl embedded), quirks from the PCI
//      ID table, the platform and nvme.quirks=, the DMA mask, the
//      /dev/nvmeX node, BAR0, the descriptor pools; CAP and CSTS are read
//      once to check the controller answers and can use 4 KiB pages.  The
//      controller is NOT touched otherwise, and bus mastering stays off.
//
//   2. RESET WORK (queued by nvme_late_init, or by probe for a hot-plugged
//      controller): enable the function, disable the controller (handing
//      over one the firmware left enabled; a Function Level Reset when
//      CSTS.RDY does not follow), the admin queue, CC.EN, identify (the
//      core), shadow doorbells, the host memory buffer, Number of Queues,
//      the interrupt vectors, the I/O queues, the tag sets -- then LIVE
//      and the namespace scan.  The same work recovers a controller later
//      (timeout, error recovery, resume); a failure leaves it DEAD.
//
// nvme_dev_disable is the one way down: freeze (and drain for a
// shutdown), quiesce, delete the I/O queues, CC.EN=0 or CC.SHN, free the
// interrupts, turn the function off, reap what completed meanwhile and
// cancel the rest.  Reset, shutdown, suspend, removal and the PCI error
// handlers all go through it under ctrl->shutdown_lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>

#define NVME_PCI_LOG NVME_LOG_PREFIX

struct nvme_pci_params nvme_pci_param = {
	.use_threaded_interrupts = 0,
	.use_cmb_sqes = 1,
	.max_host_mem_size_mb = 128,
	.sgl_threshold = 32768,
	.io_queue_depth = NVME_PCI_DEFAULT_IO_QUEUE_DEPTH,
	.write_queues = 0,
	.poll_queues = 0,
	.noacpi = 0,
	.quirks = { 0 },
	.irq_coalesce_time = 0,
	.irq_coalesce_thr = 0,
	.irq_coalesce_set = 0,
	.use_pmr = 1,
};

LIST_HEAD(nvme_pci_devs);
DEFINE_MUTEX(nvme_pci_devs_lock);

/* nvme_late_init ran: a controller probed from now on (hot-plug) starts
 * its reset work from the probe. */
static volatile int nvme_pci_late_done;

/* ==========================================================================
 * Boot command line ("nvme.<name>=")
 * ========================================================================== */

static int nvme_pci_parse_uint(const char *s, unsigned int *out)
{
	unsigned int v = 0, base = 10;

	if (!s || !*s)
		return -EINVAL;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
		if (!*s)
			return -EINVAL;
	}
	for (; *s; s++) {
		unsigned int d;

		if (*s >= '0' && *s <= '9')
			d = (unsigned int)(*s - '0');
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = (unsigned int)(*s - 'a' + 10);
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = (unsigned int)(*s - 'A' + 10);
		else
			return -EINVAL;
		if (v > (0xffffffffu - d) / base)
			return -ERANGE;
		v = v * base + d;
	}
	*out = v;
	return 0;
}

static int nvme_pci_parse_bool(const char *s, int *out)
{
	if (!s || !*s)
		return -EINVAL;
	switch (s[0]) {
	case '1':
	case 'y':
	case 'Y':
	case 't':
	case 'T':
		*out = 1;
		return 0;
	case '0':
	case 'n':
	case 'N':
	case 'f':
	case 'F':
		*out = 0;
		return 0;
	case 'o':
	case 'O':
		if (s[1] == 'n' || s[1] == 'N') {
			*out = 1;
			return 0;
		}
		if (s[1] == 'f' || s[1] == 'F') {
			*out = 0;
			return 0;
		}
		break;
	default:
		break;
	}
	return -EINVAL;
}

static void nvme_pci_knob_uint(const char *key, unsigned int *dst,
			       unsigned int min, unsigned int max)
{
	char buf[32];
	unsigned int v;

	if (!nvme_cmdline_get(key, buf, sizeof(buf)))
		return;
	if (nvme_pci_parse_uint(buf, &v) || v < min || v > max) {
		kprintf(NVME_PCI_LOG "ignoring %s=%s (range %u..%u)\n", key, buf,
			min, max);
		return;
	}
	*dst = v;
}

static void nvme_pci_knob_bool(const char *key, int *dst)
{
	char buf[16];
	int v;

	if (!nvme_cmdline_get(key, buf, sizeof(buf)))
		return;
	if (nvme_pci_parse_bool(buf, &v)) {
		kprintf(NVME_PCI_LOG "ignoring %s=%s\n", key, buf);
		return;
	}
	*dst = v;
}

void nvme_pci_parse_cmdline(void)
{
	unsigned int max_queues = blk_mq_num_possible_queues(0);
	char buf[32];

	if (!max_queues)
		max_queues = MAX_CPUS;
	nvme_pci_knob_bool("nvme.use_threaded_interrupts",
			   &nvme_pci_param.use_threaded_interrupts);
	nvme_pci_knob_bool("nvme.use_cmb_sqes", &nvme_pci_param.use_cmb_sqes);
	nvme_pci_knob_uint("nvme.max_host_mem_size_mb",
			   &nvme_pci_param.max_host_mem_size_mb, 0, 1u << 20);
	nvme_pci_knob_uint("nvme.sgl_threshold", &nvme_pci_param.sgl_threshold,
			   0, 0xffffffffu);
	nvme_pci_knob_uint("nvme.io_queue_depth",
			   &nvme_pci_param.io_queue_depth,
			   NVME_PCI_MIN_QUEUE_SIZE, NVME_PCI_MAX_QUEUE_SIZE);
	nvme_pci_knob_uint("nvme.write_queues", &nvme_pci_param.write_queues,
			   0, max_queues);
	nvme_pci_knob_uint("nvme.poll_queues", &nvme_pci_param.poll_queues, 0,
			   max_queues);
	nvme_pci_knob_bool("nvme.noacpi", &nvme_pci_param.noacpi);
	nvme_pci_knob_bool("nvme.pmr", &nvme_pci_param.use_pmr);
	if (nvme_cmdline_get("nvme.irq_coalesce_time", buf, sizeof(buf)) ||
	    nvme_cmdline_get("nvme.irq_coalesce_thr", buf, sizeof(buf)))
		nvme_pci_param.irq_coalesce_set = 1;
	nvme_pci_knob_uint("nvme.irq_coalesce_time",
			   &nvme_pci_param.irq_coalesce_time, 0, 255);
	nvme_pci_knob_uint("nvme.irq_coalesce_thr",
			   &nvme_pci_param.irq_coalesce_thr, 0, 255);
	if (!nvme_cmdline_get("nvme.quirks", nvme_pci_param.quirks,
			      sizeof(nvme_pci_param.quirks)))
		nvme_pci_param.quirks[0] = 0;
}

/* ==========================================================================
 * Register access and the controller operations
 * ========================================================================== */

/* All-ones from a register the device would never return it from: is
 * the device still there? */
static int nvme_pci_check_gone(struct nvme_dev *dev)
{
	if (pci_dev_is_disconnected(dev->pdev))
		return -ENODEV;
	if (!pci_device_is_present(dev->pdev))
		return -ENODEV;
	return 0;
}

static int nvme_pci_reg_read32(struct nvme_ctrl *ctrl, uint32_t off,
			       uint32_t *val)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	if (!dev->bar || off + 4 > dev->bar_mapped_size) {
		*val = 0xffffffffu;
		return -ENODEV;
	}
	*val = readl((volatile uint8_t *)dev->bar + off);
	if (*val == 0xffffffffu)
		return nvme_pci_check_gone(dev);
	return 0;
}

static int nvme_pci_reg_write32(struct nvme_ctrl *ctrl, uint32_t off,
				uint32_t val)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	if (!dev->bar || off + 4 > dev->bar_mapped_size)
		return -ENODEV;
	writel(val, (volatile uint8_t *)dev->bar + off);
	return 0;
}

static int nvme_pci_reg_read64(struct nvme_ctrl *ctrl, uint32_t off,
			       uint64_t *val)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	if (!dev->bar || off + 8 > dev->bar_mapped_size) {
		*val = ~0ull;
		return -ENODEV;
	}
	*val = lo_hi_readq((volatile uint8_t *)dev->bar + off);
	if (*val == ~0ull)
		return nvme_pci_check_gone(dev);
	return 0;
}

static int nvme_pci_get_address(struct nvme_ctrl *ctrl, char *buf, int size)
{
	char name[24];

	if (!buf || size <= 0)
		return 0;
	pci_name(to_nvme_dev(ctrl)->pdev, name, sizeof(name));
	return ksnprintf(buf, (size_t)size, "%s\n", name);
}

static void nvme_pci_print_device_info(struct nvme_ctrl *ctrl)
{
	pci_device_t *pdev = to_nvme_dev(ctrl)->pdev;
	struct nvme_subsystem *subsys = ctrl->subsys;
	char model[NVME_MODEL_LEN + 1], fw[NVME_FW_REV_LEN + 1];

	kmemset(model, 0, sizeof(model));
	kmemset(fw, 0, sizeof(fw));
	if (subsys) {
		kmemcpy(model, subsys->model, NVME_MODEL_LEN);
		kmemcpy(fw, subsys->firmware_rev, NVME_FW_REV_LEN);
		/* Trailing blanks of the identify strings. */
		for (int i = NVME_MODEL_LEN - 1; i >= 0 && (model[i] == ' ' ||
							   !model[i]); i--)
			model[i] = 0;
		for (int i = NVME_FW_REV_LEN - 1; i >= 0 && (fw[i] == ' ' ||
							    !fw[i]); i--)
			fw[i] = 0;
	}
	kprintf(NVME_PCI_LOG "%s: VID:DID %04x:%04x model:%s firmware:%s\n",
		ctrl->name, pdev->vendor_id, pdev->device_id, model, fw);
}

static bool nvme_pci_supports_pci_p2pdma(struct nvme_ctrl *ctrl)
{
	(void)ctrl;
	return false; /* no peer-to-peer DMA framework */
}

static unsigned long nvme_pci_get_virt_boundary(struct nvme_ctrl *ctrl,
						bool is_admin)
{
	if (!nvme_ctrl_sgl_supported(ctrl) || is_admin)
		return NVME_CTRL_PAGE_SIZE - 1;
	return 0;
}

static void nvme_pci_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	might_sleep();
	nvme_free_tagset(dev);
	WARN_ON(dev->in_list);
	kfree(dev->queues);
	dev->queues = NULL;
	kfree(dev);
}

static int nvme_pci_subsystem_reset(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);
	int ret = 0;

	might_sleep();
	/* shutdown_lock keeps the BAR mapping stable against reset_work,
	 * and taken before the state change it keeps removal from
	 * unmapping it until we are done. */
	mutex_lock(&dev->ctrl.shutdown_lock);
	if (!dev->bar_mapped_size) {
		ret = -ENODEV;
		goto unlock;
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING)) {
		ret = -EBUSY;
		goto unlock;
	}

	writel(NVME_SUBSYS_RESET,
	       (volatile uint8_t *)dev->bar + NVME_REG_NSSR);

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING) ||
	    !nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))
		goto unlock;

	/* Flush the write; the read fails if the link went down, which
	 * starts the error recovery. */
	(void)readl((volatile uint8_t *)dev->bar + NVME_REG_CSTS);
unlock:
	mutex_unlock(&dev->ctrl.shutdown_lock);
	return ret;
}

const struct nvme_ctrl_ops nvme_pci_ctrl_ops = {
	.name = "pcie",
	.flags = NVME_F_METADATA_SUPPORTED,
	.reg_read32 = nvme_pci_reg_read32,
	.reg_write32 = nvme_pci_reg_write32,
	.reg_read64 = nvme_pci_reg_read64,
	.free_ctrl = nvme_pci_free_ctrl,
	.submit_async_event = nvme_pci_submit_async_event,
	.subsystem_reset = nvme_pci_subsystem_reset,
	.get_address = nvme_pci_get_address,
	.print_device_info = nvme_pci_print_device_info,
	.supports_pci_p2pdma = nvme_pci_supports_pci_p2pdma,
	.get_virt_boundary = nvme_pci_get_virt_boundary,
};

/* ==========================================================================
 * BAR0
 * ========================================================================== */

int nvme_remap_bar(struct nvme_dev *dev, unsigned long size)
{
	pci_device_t *pdev = dev->pdev;
	volatile void *bar;

	if (size <= dev->bar_mapped_size)
		return 0;
	if (size > pci_resource_len(pdev, 0))
		return -ENOMEM;
	if (dev->bar)
		pci_iounmap(pdev, dev->bar);
	bar = pci_iomap(pdev, 0, size);
	if (!bar) {
		dev->bar = NULL;
		dev->dbs = NULL;
		dev->bar_mapped_size = 0;
		return -ENOMEM;
	}
	dev->bar = bar;
	dev->bar_mapped_size = size;
	dev->dbs = (volatile uint32_t *)((volatile uint8_t *)bar +
					 NVME_REG_DBS);
	return 0;
}

static int nvme_dev_map(struct nvme_dev *dev)
{
	if (nvme_remap_bar(dev, NVME_REG_DBS + 4096))
		return -ENODEV;
	return 0;
}

static void nvme_dev_unmap(struct nvme_dev *dev)
{
	mutex_lock(&dev->ctrl.shutdown_lock);
	if (dev->bar)
		pci_iounmap(dev->pdev, dev->bar);
	dev->bar = NULL;
	dev->dbs = NULL;
	dev->bar_mapped_size = 0;
	mutex_unlock(&dev->ctrl.shutdown_lock);
}

/* ==========================================================================
 * Health
 * ========================================================================== */

bool nvme_should_reset(struct nvme_dev *dev, uint32_t csts)
{
	/* Communication with the adapter lost, possibly through an NVM
	 * subsystem reset. */
	bool nssro = dev->subsystem && (csts & NVME_CSTS_NSSRO);

	/* A reset is under way already. */
	switch (nvme_ctrl_state(&dev->ctrl)) {
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return false;
	default:
		break;
	}

	/* Only a fatal controller status or a lost controller resets. */
	if (!(csts & NVME_CSTS_CFS) && !nssro)
		return false;
	return true;
}

void nvme_warn_reset(struct nvme_dev *dev, uint32_t csts)
{
	uint16_t pci_status = 0;
	int result;

	/* A config register tells what died. */
	result = pci_read_config_word(dev->pdev, PCI_STATUS, &pci_status);
	if (result == PCIBIOS_SUCCESSFUL)
		kprintf(NVME_PCI_LOG
			"%s: controller is down; will reset: CSTS=0x%x, PCI_STATUS=0x%x\n",
			dev->ctrl.name, csts, pci_status);
	else
		kprintf(NVME_PCI_LOG
			"%s: controller is down; will reset: CSTS=0x%x, PCI_STATUS read failed (%d)\n",
			dev->ctrl.name, csts, result);

	if (csts != 0xffffffffu)
		return;
	kprintf(NVME_PCI_LOG
		"%s: does the device have a faulty power saving mode enabled?\n",
		dev->ctrl.name);
	kprintf(NVME_PCI_LOG
		"%s: try \"nvme_core.default_ps_max_latency_us=0\" and report it\n",
		dev->ctrl.name);
}

static bool nvme_pci_ctrl_is_dead(struct nvme_dev *dev)
{
	pci_device_t *pdev = dev->pdev;
	uint32_t csts;

	if (!pci_is_enabled(pdev) || !pci_device_is_present(pdev))
		return true;
	if (pci_channel_offline(pdev))
		return true;
	if (!dev->bar)
		return true;
	csts = readl((volatile uint8_t *)dev->bar + NVME_REG_CSTS);
	return (csts & NVME_CSTS_CFS) || !(csts & NVME_CSTS_RDY);
}

/* ==========================================================================
 * Enable / disable
 * ========================================================================== */

int nvme_pci_enable(struct nvme_dev *dev)
{
	pci_device_t *pdev = dev->pdev;
	unsigned int flags = PCI_IRQ_ALL_TYPES;
	volatile uint8_t *bar;
	uint32_t mqes1;
	int result = -ENOMEM;

	might_sleep();
	lockdep_assert_held(&dev->ctrl.shutdown_lock);
	if (!dev->bar)
		return -ENODEV;
	bar = dev->bar;
	if (pci_enable_device_mem(pdev))
		return result;

	if (readl(bar + NVME_REG_CSTS) == 0xffffffffu) {
		result = -ENODEV;
		goto disable;
	}

	/*
	 * A controller the firmware (a boot driver) left enabled may still
	 * own an admin queue, an outstanding Asynchronous Event Request or an
	 * MSI-X table in memory that is the kernel's now.  Clearing CC.EN is
	 * a register write and needs no bus mastering, so the controller is
	 * reset with mastering still off: nothing it had queued can reach
	 * memory once mastering is turned on below.  A controller that does
	 * not come back here gets the full treatment (disable again, FLR)
	 * from nvme_pci_configure_admin_queue().
	 */
	dev->ctrl.cap = lo_hi_readq(bar + NVME_REG_CAP);
	if ((readl(bar + NVME_REG_CC) & NVME_CC_ENABLE) ||
	    (readl(bar + NVME_REG_CSTS) & NVME_CSTS_RDY)) {
		int ret = nvme_disable_ctrl(&dev->ctrl, false);

		if (ret)
			kprintf(NVME_PCI_LOG
				"%s: controller left enabled did not reset with bus mastering off (%d)\n",
				dev->ctrl.name, ret);
	}

	pci_set_master(pdev);

	/* Some devices or platforms do not route INTx: one MSI-X or MSI
	 * vector for the bring-up, adjusted once the queue count is known. */
	if (dev->ctrl.quirks & NVME_QUIRK_BROKEN_MSI)
		flags &= ~PCI_IRQ_MSI;
	result = pci_alloc_irq_vectors(pdev, 1, 1, flags);
	if (result < 0)
		goto disable;

	dev->ctrl.cap = lo_hi_readq(bar + NVME_REG_CAP);

	mqes1 = (uint32_t)NVME_CAP_MQES(dev->ctrl.cap) + 1;
	dev->q_depth = mqes1 < nvme_pci_param.io_queue_depth ?
			       mqes1 :
			       nvme_pci_param.io_queue_depth;
	dev->db_stride = 1u << NVME_CAP_STRIDE(dev->ctrl.cap);
	dev->dbs = (volatile uint32_t *)(bar + NVME_REG_DBS);

	/* Some Apple controllers need 128-byte SQ entries (and ignore
	 * CC.IOSQES, which therefore stays the standard value). */
	if (dev->ctrl.quirks & NVME_QUIRK_128_BYTES_SQES)
		dev->io_sqes = 7;
	else
		dev->io_sqes = NVME_NVM_IOSQES;

	if (dev->ctrl.quirks & NVME_QUIRK_QDEPTH_ONE) {
		dev->q_depth = 2;
	} else if (pdev->vendor_id == PCI_VENDOR_ID_SAMSUNG &&
		   (pdev->device_id == 0xa821 || pdev->device_id == 0xa822) &&
		   NVME_CAP_MQES(dev->ctrl.cap) == 0) {
		dev->q_depth = 64;
		kprintf(NVME_PCI_LOG
			"%s: detected PM1725 NVMe controller, set queue depth=%u\n",
			dev->ctrl.name, dev->q_depth);
	}
	if (WARN_ON(dev->q_depth < NVME_PCI_MIN_QUEUE_SIZE))
		dev->q_depth = NVME_PCI_MIN_QUEUE_SIZE;

	/* Shared tags (Apple) need 32 tags for the admin queue out of the
	 * I/O queue's. */
	if ((dev->ctrl.quirks & NVME_QUIRK_SHARED_TAGS) &&
	    dev->q_depth < NVME_AQ_DEPTH + 2) {
		dev->q_depth = NVME_AQ_DEPTH + 2;
		kprintf(NVME_PCI_LOG "%s: IO queue depth clamped to %u\n",
			dev->ctrl.name, dev->q_depth);
	}
	dev->ctrl.sqsize = (uint16_t)(dev->q_depth - 1); /* 0's based */

	nvme_map_cmb(dev);
	if (nvme_pmr_enable(dev) < 0)
		kprintf(NVME_PCI_LOG "%s: PMR not enabled\n", dev->ctrl.name);
	nvme_pci_configure_aspm(dev);

	pci_save_state(pdev);

	result = nvme_pci_configure_admin_queue(dev);
	if (result)
		goto free_irq;
	return result;

free_irq:
	pci_free_irq_vectors(pdev);
disable:
	pci_disable_device(pdev);
	return result;
}

void nvme_dev_disable(struct nvme_dev *dev, bool shutdown)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(&dev->ctrl);
	pci_device_t *pdev = dev->pdev;
	bool dead;

	might_sleep();
	mutex_lock(&dev->ctrl.shutdown_lock);
	dead = nvme_pci_ctrl_is_dead(dev);
	if (state == NVME_CTRL_LIVE || state == NVME_CTRL_RESETTING) {
		if (pci_is_enabled(pdev))
			nvme_start_freeze(&dev->ctrl);
		/* A clean shutdown lets what entered complete. */
		if (!dead && shutdown)
			nvme_wait_freeze_timeout(&dev->ctrl,
						 (long)nvme_pci_io_timeout_ms());
	}

	nvme_quiesce_io_queues(&dev->ctrl);

	if (!dead && dev->ctrl.queue_count > 0) {
		nvme_delete_io_queues(dev);
		nvme_disable_ctrl(&dev->ctrl, shutdown);
		nvme_poll_irqdisable(&dev->queues[0]);
	}
	nvme_suspend_io_queues(dev);
	nvme_suspend_queue(dev, 0);
	pci_free_irq_vectors(pdev);
	/* No DMA from here on, even from a controller that never finished
	 * CC.EN=0 (dead, stuck): the cancel below frees what it pointed at. */
	if (pci_is_enabled(pdev)) {
		pci_clear_master(pdev);
		pci_disable_device(pdev);
	}
	/* The controller let go of the host memory buffer (CC.EN=0, or no
	 * bus mastering any more): it is the host's again.  A Set Features
	 * in flight now (sysfs `hmb') must not turn it back on when its
	 * completion is reaped below (nvme_set_host_mem). */
	{
		uint64_t fl;

		spin_lock_irqsave(&dev->ctrl.lock, &fl);
		dev->hmb_disable_seq++;
		dev->hmb = false;
		spin_unlock_irqrestore(&dev->ctrl.lock, fl);
	}
	nvme_reap_pending_cqes(dev);

	nvme_cancel_tagset(&dev->ctrl);
	nvme_cancel_admin_tagset(&dev->ctrl);

	/* No restart follows a shutdown: every request that entered must
	 * reach its failed completion now. */
	if (shutdown) {
		nvme_unquiesce_io_queues(&dev->ctrl);
		if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q))
			nvme_unquiesce_admin_queue(&dev->ctrl);
	}
	mutex_unlock(&dev->ctrl.shutdown_lock);
}

int nvme_disable_prepare_reset(struct nvme_dev *dev, bool shutdown)
{
	might_sleep();
	if (!nvme_wait_reset(&dev->ctrl))
		return -EBUSY;
	nvme_dev_disable(dev, shutdown);
	return 0;
}

void nvme_remove_dead_ctrl(struct nvme_dev *dev)
{
	might_sleep();
	/* DELETING first: nvme_wait_reset callers stop waiting. */
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
	nvme_dev_disable(dev, true);
	if (dev->ctrl.admin_q)
		nvme_sync_queues(&dev->ctrl);
	nvme_mark_namespaces_dead(&dev->ctrl);
	nvme_unquiesce_io_queues(&dev->ctrl);
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DEAD);
}

/* ==========================================================================
 * Reset work
 * ========================================================================== */

void nvme_reset_work(void *arg)
{
	struct nvme_dev *dev = arg;
	bool was_suspend =
		(nvme_ctrl_config(&dev->ctrl) & NVME_CC_SHN_NORMAL) != 0;
	int result;

	might_sleep();
	if (nvme_ctrl_state(&dev->ctrl) != NVME_CTRL_RESETTING) {
		kprintf(NVME_PCI_LOG "%s: ctrl state %d is not RESETTING\n",
			dev->ctrl.name, (int)nvme_ctrl_state(&dev->ctrl));
		result = -ENODEV;
		goto out;
	}
	dev->last_reset_ns = hrtimer_now_ns();

	/* A live controller is shut down first. */
	if (nvme_ctrl_config(&dev->ctrl) & NVME_CC_ENABLE)
		nvme_dev_disable(dev, false);
	if (dev->ctrl.admin_q)
		nvme_sync_queues(&dev->ctrl);

	mutex_lock(&dev->ctrl.shutdown_lock);
	result = nvme_pci_enable(dev);
	if (result)
		goto out_unlock;
	if (dev->ctrl.admin_q)
		nvme_unquiesce_admin_queue(&dev->ctrl);
	mutex_unlock(&dev->ctrl.shutdown_lock);

	/* The first bring-up creates the admin tag set (probe ran before
	 * anything could sleep). */
	if (!dev->ctrl.admin_q) {
		result = nvme_alloc_admin_tag_set(&dev->ctrl,
						  &dev->admin_tagset,
						  &nvme_mq_admin_ops,
						  sizeof(struct nvme_iod));
		if (result)
			goto out;
	}

	/* CONNECTING marks the initialisation: a timeout now disables the
	 * controller instead of trying to recover it. */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_CONNECTING)) {
		kprintf(NVME_PCI_LOG "%s: failed to mark controller CONNECTING\n",
			dev->ctrl.name);
		result = -EBUSY;
		goto out;
	}

	result = nvme_init_ctrl_finish(&dev->ctrl, was_suspend);
	if (result)
		goto out;

	if (nvme_ctrl_meta_sgl_supported(&dev->ctrl))
		dev->ctrl.max_integrity_segments = NVME_MAX_META_SEGS;
	else
		dev->ctrl.max_integrity_segments = 1;

	nvme_dbbuf_dma_alloc(dev);

	mutex_lock(&dev->hmb_lock);
	result = nvme_setup_host_mem(dev);
	mutex_unlock(&dev->hmb_lock);
	if (result < 0)
		goto out;

	result = nvme_setup_io_queues(dev);
	if (result)
		goto out;

	/* The I/O queue count may have changed: freeze, resize, unfreeze.
	 * Without any I/O queue left the controller stays (admin commands
	 * still work) but its namespaces go. */
	if (dev->online_queues > 1) {
		nvme_dbbuf_set(dev);
		nvme_unquiesce_io_queues(&dev->ctrl);
		nvme_wait_freeze(&dev->ctrl);
		if (!nvme_pci_update_nr_queues(dev))
			goto out;
		nvme_unfreeze(&dev->ctrl);
	} else {
		kprintf(NVME_PCI_LOG "%s: IO queues %s\n", dev->ctrl.name,
			dev->ctrl.tagset ? "lost" : "not created");
		nvme_mark_namespaces_dead(&dev->ctrl);
		nvme_unquiesce_io_queues(&dev->ctrl);
		nvme_remove_namespaces(&dev->ctrl);
		/* Under shutdown_lock: the debug interface walks the tag set
		 * under it (as nvme_pci_update_nr_queues frees it). */
		mutex_lock(&dev->ctrl.shutdown_lock);
		nvme_free_tagset(dev);
		mutex_unlock(&dev->ctrl.shutdown_lock);
	}

	if (nvme_pci_param.irq_coalesce_set && dev->online_queues > 1)
		nvme_set_irq_coalescing(dev,
					(uint8_t)nvme_pci_param.irq_coalesce_time,
					(uint8_t)nvme_pci_param.irq_coalesce_thr);

	/* With only the admin queue the controller stays for diagnosis or
	 * a firmware update. */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_LIVE)) {
		kprintf(NVME_PCI_LOG "%s: failed to mark controller live state\n",
			dev->ctrl.name);
		result = -ENODEV;
		goto out;
	}

	dev->reset_failures = 0;
	nvme_start_ctrl(&dev->ctrl);
	return;

out_unlock:
	mutex_unlock(&dev->ctrl.shutdown_lock);
out:
	/* DELETING now, so nvme_wait_reset waiters are not blocked. */
	dev->reset_failures++;
	kprintf(NVME_PCI_LOG "%s: Disabling device after reset failure: %d\n",
		dev->ctrl.name, result);
	nvme_remove_dead_ctrl(dev);
}

/* ==========================================================================
 * Probe and removal
 * ========================================================================== */

static struct nvme_dev *nvme_pci_alloc_dev(pci_device_t *pdev,
					   const struct pci_device_id *id,
					   int *err)
{
	unsigned long quirks = id ? (unsigned long)id->driver_data : 0;
	unsigned long platform;
	struct nvme_dev *dev;
	int ret;

	*err = -ENOMEM;
	dev = kcalloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->pdev = pdev;
	dev->cmb_bar = -1;
	dev->pmr_bar = -1;
	dev->last_ps = 0xffffffffu;
	INIT_LIST_HEAD(&dev->dev_list);
	mutex_init(&dev->hmb_lock, "nvme_hmb");

	dev->nr_write_queues = nvme_pci_param.write_queues;
	dev->nr_poll_queues = nvme_pci_param.poll_queues;
	dev->nr_allocated_queues = nvme_max_io_queues(dev) + 1;
	dev->queues = kcalloc(dev->nr_allocated_queues,
			      sizeof(struct nvme_queue));
	if (!dev->queues)
		goto out_free_dev;
	for (unsigned int i = 0; i < dev->nr_allocated_queues; i++)
		dev->queues[i].irq = -1;

	platform = nvme_pci_platform_quirks(pdev);
	dev->platform_quirks = platform;
	quirks |= platform;
	nvme_pci_cmdline_quirks(pdev, &quirks);

	dev->ctrl.pdev = pdev;
	ret = nvme_init_ctrl(&dev->ctrl, pdev, &nvme_pci_ctrl_ops, quirks);
	if (ret) {
		*err = ret;
		goto out_free_queues;
	}
	dev->ctrl.pdev = pdev;
	blk_work_init(&dev->ctrl.reset_work, nvme_reset_work, dev);

	if (dev->ctrl.quirks & NVME_QUIRK_DMA_ADDRESS_BITS_48) {
		if (dma_set_mask_and_coherent(pdev, DMA_BIT_MASK(48)))
			dma_set_mask_and_coherent(pdev, DMA_BIT_MASK(32));
	} else if (dma_set_mask_and_coherent(pdev, DMA_BIT_MASK(64))) {
		kprintf(NVME_PCI_LOG "%s: 64-bit DMA refused, using 32-bit\n",
			dev->ctrl.name);
		dma_set_mask_and_coherent(pdev, DMA_BIT_MASK(32));
	}
	dev->dma_mask = dma_get_mask(pdev);

	/* Commands stay within one PRP list set / SGL list page. */
	dev->ctrl.max_hw_sectors = NVME_MAX_BYTES >> 9;
	dev->ctrl.max_segments = NVME_MAX_SEGS;
	dev->ctrl.max_integrity_segments = 1;
	*err = 0;
	return dev;

out_free_queues:
	kfree(dev->queues);
out_free_dev:
	kfree(dev);
	return NULL;
}

/* Probe-time look at the registers: the controller answers and can use
 * the host's 4 KiB pages.  Bus mastering stays off until the reset work:
 * a controller the firmware left enabled cannot DMA into memory that is
 * the kernel's now. */
static int nvme_pci_probe_check(struct nvme_dev *dev)
{
	pci_device_t *pdev = dev->pdev;
	volatile uint8_t *bar = dev->bar;
	uint32_t csts, vs;
	uint64_t cap;
	char name[24];
	int ret = 0;

	if (pci_enable_device_mem(pdev))
		return -ENODEV;
	pci_clear_master(pdev);
	csts = readl(bar + NVME_REG_CSTS);
	if (csts == 0xffffffffu) {
		ret = -ENODEV;
		goto out;
	}
	cap = lo_hi_readq(bar + NVME_REG_CAP);
	vs = readl(bar + NVME_REG_VS);
	dev->ctrl.cap = cap;
	if (12u + NVME_CAP_MPSMIN(cap) > NVME_CTRL_PAGE_SHIFT) {
		kprintf(NVME_PCI_LOG
			"%s: minimum device page size %u too large for host (%u)\n",
			dev->ctrl.name, 1u << (12u + NVME_CAP_MPSMIN(cap)),
			NVME_CTRL_PAGE_SIZE);
		ret = -ENODEV;
		goto out;
	}
	pci_name(pdev, name, sizeof(name));
	kprintf(NVME_PCI_LOG
		"%s: pci function %s [%04x:%04x], NVMe %u.%u.%u, MQES %u, DSTRD %u%s\n",
		dev->ctrl.name, name, pdev->vendor_id, pdev->device_id,
		vs >> 16, (vs >> 8) & 0xff, vs & 0xff,
		(unsigned)NVME_CAP_MQES(cap) + 1, (unsigned)NVME_CAP_STRIDE(cap),
		(csts & NVME_CSTS_RDY) ? ", enabled by firmware" : "");
out:
	pci_disable_device(pdev);
	return ret;
}

int nvme_pci_probe(pci_device_t *pdev, const struct pci_device_id *id)
{
	struct nvme_dev *dev;
	int result;

	dev = nvme_pci_alloc_dev(pdev, id, &result);
	if (!dev)
		return result ? result : -ENOMEM;

	result = nvme_add_ctrl(&dev->ctrl);
	if (result)
		goto out_put_ctrl;

	result = nvme_dev_map(dev);
	if (result)
		goto out_uninit_ctrl;

	result = nvme_setup_descriptor_pools(dev);
	if (result)
		goto out_dev_unmap;

	result = nvme_pci_probe_check(dev);
	if (result)
		goto out_release_pools;

	pci_set_drvdata(pdev, dev);
	mutex_lock(&nvme_pci_devs_lock);
	list_add_tail(&dev->dev_list, &nvme_pci_devs);
	dev->in_list = 1;
	dev->probed = 1;
	mutex_unlock(&nvme_pci_devs_lock);

	/* The probe's reference; the one nvme_add_ctrl took stays until
	 * nvme_uninit_ctrl at removal. */
	nvme_put_ctrl(&dev->ctrl);

	/* Hot-plugged after boot: bring it up now. */
	if (__atomic_load_n(&nvme_pci_late_done, __ATOMIC_ACQUIRE)) {
		mutex_lock(&nvme_pci_devs_lock);
		if (dev->probed) {
			dev->probed = 0;
			if (nvme_reset_ctrl(&dev->ctrl))
				kprintf(NVME_PCI_LOG
					"%s: could not queue the bring-up\n",
					dev->ctrl.name);
		}
		mutex_unlock(&nvme_pci_devs_lock);
	}
	return 0;

out_release_pools:
	nvme_release_descriptor_pools(dev);
out_dev_unmap:
	nvme_dev_unmap(dev);
out_uninit_ctrl:
	nvme_uninit_ctrl(&dev->ctrl);
out_put_ctrl:
	nvme_put_ctrl(&dev->ctrl);
	kprintf(NVME_PCI_LOG "probe of %02x:%02x.%x failed: %d\n", pdev->bus,
		pdev->device, pdev->function, result);
	return result;
}

/* The device may be in any state of its bring-up here, or gone. */
void nvme_pci_remove(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	might_sleep();
	if (!dev)
		return;
	mutex_lock(&nvme_pci_devs_lock);
	dev->removing = 1;
	dev->probed = 0; /* late init must not start it any more */
	mutex_unlock(&nvme_pci_devs_lock);

	/* Never brought up (removed before late init): through RESETTING,
	 * the only way out of NEW. */
	if (nvme_ctrl_state(&dev->ctrl) == NVME_CTRL_NEW)
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_RESETTING);
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
	pci_set_drvdata(pdev, NULL);

	if (!pci_device_is_present(pdev)) {
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DEAD);
		nvme_dev_disable(dev, true);
	}

	nvme_flush_work(&dev->ctrl.reset_work);
	nvme_stop_ctrl(&dev->ctrl);
	nvme_remove_namespaces(&dev->ctrl);
	nvme_dev_disable(dev, true);
	mutex_lock(&dev->hmb_lock);
	nvme_free_host_mem(dev);
	mutex_unlock(&dev->hmb_lock);
	nvme_dev_remove_admin(dev);
	nvme_dbbuf_dma_free(dev);
	nvme_free_queues(dev, 0);
	nvme_release_descriptor_pools(dev);
	nvme_release_cmb(dev);
	nvme_pmr_disable(dev);
	nvme_dev_unmap(dev);

	mutex_lock(&nvme_pci_devs_lock);
	if (dev->in_list) {
		list_del_init(&dev->dev_list);
		dev->in_list = 0;
	}
	mutex_unlock(&nvme_pci_devs_lock);
	nvme_uninit_ctrl(&dev->ctrl);
}

void nvme_pci_shutdown(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	might_sleep();
	if (!dev)
		return;
	if (__atomic_exchange_n(&dev->shutdown_done, 1, __ATOMIC_ACQ_REL))
		return;
	nvme_disable_prepare_reset(dev, true);
}

/* ==========================================================================
 * PCI error recovery and function reset
 * ========================================================================== */

static void nvme_reset_prepare(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (!dev)
		return;
	/* The PCI core serialises against ->remove: the result of waiting
	 * for the reset state does not matter. */
	nvme_disable_prepare_reset(dev, false);
	if (dev->ctrl.admin_q)
		nvme_sync_queues(&dev->ctrl);
}

static void nvme_reset_done(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (!dev)
		return;
	if (!nvme_try_sched_reset(&dev->ctrl))
		nvme_flush_work(&dev->ctrl.reset_work);
}

static pci_ers_result_t nvme_error_detected(pci_device_t *pdev,
					    pci_channel_state_t state)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (!dev)
		return PCI_ERS_RESULT_NONE;
	/* A frozen channel needs a reset: quiesce now, restart after the
	 * slot reset. */
	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		kprintf(NVME_PCI_LOG
			"%s: frozen state error detected, reset controller\n",
			dev->ctrl.name);
		if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_RESETTING)) {
			nvme_dev_disable(dev, true);
			return PCI_ERS_RESULT_DISCONNECT;
		}
		nvme_dev_disable(dev, false);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		kprintf(NVME_PCI_LOG
			"%s: failure state error detected, request disconnect\n",
			dev->ctrl.name);
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t nvme_slot_reset(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (!dev)
		return PCI_ERS_RESULT_DISCONNECT;
	kprintf(NVME_PCI_LOG "%s: restart after slot reset\n", dev->ctrl.name);
	pci_restore_state(pdev);
	if (nvme_try_sched_reset(&dev->ctrl))
		nvme_unquiesce_io_queues(&dev->ctrl);
	return PCI_ERS_RESULT_RECOVERED;
}

static void nvme_error_resume(pci_device_t *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (dev)
		nvme_flush_work(&dev->ctrl.reset_work);
}

static const struct pci_error_handlers nvme_err_handler = {
	.error_detected = nvme_error_detected,
	.slot_reset = nvme_slot_reset,
	.resume = nvme_error_resume,
	.reset_prepare = nvme_reset_prepare,
	.reset_done = nvme_reset_done,
};

struct pci_driver nvme_pci_driver = {
	.name = "nvme",
	.id_table = nvme_id_table,
	.probe = nvme_pci_probe,
	.remove = nvme_pci_remove,
	.shutdown = nvme_pci_shutdown,
	.suspend = nvme_pci_suspend,
	.resume = nvme_pci_resume,
	.err_handler = &nvme_err_handler,
	.next = NULL,
};

/* ==========================================================================
 * Entry points
 * ========================================================================== */

int nvme_pci_init(void)
{
	_nvme_pci_check();
	nvme_pci_parse_cmdline();
	nvme_pci_quirks_init();
	return pci_register_driver(&nvme_pci_driver);
}

void nvme_pci_late_init(void)
{
	struct nvme_dev *dev;

	mutex_lock(&nvme_pci_devs_lock);
	__atomic_store_n(&nvme_pci_late_done, 1, __ATOMIC_RELEASE);
	list_for_each_entry(dev, &nvme_pci_devs, dev_list) {
		if (!dev->probed || dev->removing)
			continue;
		dev->probed = 0;
		if (nvme_reset_ctrl(&dev->ctrl))
			kprintf(NVME_PCI_LOG "%s: could not queue the bring-up\n",
				dev->ctrl.name);
	}
	mutex_unlock(&nvme_pci_devs_lock);
}

void nvme_shutdown_all(void)
{
	struct nvme_dev *dev;

	/*
	 * One controller at a time, any number of them: pick the next one
	 * not shut down yet under the registry lock, take a reference, and
	 * shut it down without the lock (the shutdown sleeps, and removal
	 * takes the lock).  shutdown_done marks the ones done, so every
	 * pass makes progress.
	 */
	for (;;) {
		bool found = false;

		mutex_lock(&nvme_pci_devs_lock);
		list_for_each_entry(dev, &nvme_pci_devs, dev_list) {
			if (dev->removing ||
			    __atomic_load_n(&dev->shutdown_done,
					    __ATOMIC_ACQUIRE))
				continue;
			nvme_get_ctrl(&dev->ctrl);
			found = true;
			break;
		}
		mutex_unlock(&nvme_pci_devs_lock);
		if (!found)
			break;

		if (!__atomic_exchange_n(&dev->shutdown_done, 1,
					 __ATOMIC_ACQ_REL)) {
			if (nvme_disable_prepare_reset(dev, true) == 0)
				kprintf(NVME_PCI_LOG "%s: shut down\n",
					dev->ctrl.name);
		}
		nvme_put_ctrl(&dev->ctrl);
	}
}
