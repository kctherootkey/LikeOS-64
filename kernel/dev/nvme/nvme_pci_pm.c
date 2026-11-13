// LikeOS - NVMe PCIe power management: suspend/resume, PCI PM, ASPM decisions
//
// Two ways to suspend, chosen the established way:
//
//   * HOST MANAGED (the platform keeps power, the link has ASPM, the
//     controller has non-operational power states): freeze the queues,
//     give the host memory buffer back (the controller may not reach host
//     memory while the system sleeps), remember the current power state
//     and move to the deepest one (NPSS) with Set Features Power
//     Management.  The PCI state is saved so the PCI layer does not put
//     the function into D3 behind the protocol's back.  Resume restores
//     the power state and the HMB; anything failing there resets the
//     controller.  Low idle power with a quick resume.
//
//   * SIMPLE (firmware involved in the sleep, no ASPM -- the link must go
//     down for the platform's lowest state --, no power states, or the
//     SIMPLE_SUSPEND quirk / ACPI StorageD3Enable): a full shutdown with
//     CC.SHN, the controller reset on resume.
//
// The power state Set/Get Features and APST belong to the core; the PCI
// D-state and ASPM machinery to the PCI core.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/io/console.h>

#define NVME_PCI_PM_LOG NVME_LOG_PREFIX

/* LikeOS has no firmware-assisted sleep state: the platform never removes
 * power behind a suspend, so the host-managed path is always possible
 * when the controller and the link allow it. */
static bool nvme_pm_suspend_via_firmware(void)
{
	return false;
}

void nvme_pci_configure_aspm(struct nvme_dev *dev)
{
	pci_device_t *pdev = dev->pdev;
	bool on;

	/* The ASPM policy is the PCI core's; the driver only needs to know
	 * whether the link may enter L0s/L1 (suspend policy below). */
	on = pci_is_pcie(pdev) && pcie_aspm_enabled(pdev) > 0;
	if (on != dev->aspm_enabled)
		kprintf(NVME_PCI_PM_LOG "%s: link ASPM %s\n", dev->ctrl.name,
			on ? "enabled" : "disabled");
	dev->aspm_enabled = on;
}

bool nvme_pci_simple_suspend(struct nvme_dev *dev)
{
	struct nvme_ctrl *ctrl = &dev->ctrl;

	/* Firmware involved after the suspend, no non-default power state,
	 * or a link without ASPM (shut down so the link can go down and the
	 * platform reach its lowest state): a full shutdown. */
	nvme_pci_configure_aspm(dev);
	return nvme_pm_suspend_via_firmware() || !ctrl->npss ||
	       !dev->aspm_enabled || (ctrl->quirks & NVME_QUIRK_SIMPLE_SUSPEND);
}

int nvme_pci_simple_suspend_dev(struct nvme_dev *dev)
{
	might_sleep();
	return nvme_disable_prepare_reset(dev, true);
}

int nvme_pci_simple_resume_dev(struct nvme_dev *dev)
{
	might_sleep();
	return nvme_try_sched_reset(&dev->ctrl);
}

int nvme_pci_suspend(pci_device_t *pdev)
{
	struct nvme_dev *ndev = pci_get_drvdata(pdev);
	struct nvme_ctrl *ctrl;
	int ret = -EBUSY;

	might_sleep();
	if (!ndev)
		return 0;
	ctrl = &ndev->ctrl;
	ndev->last_ps = 0xffffffffu;

	if (nvme_pci_simple_suspend(ndev))
		return nvme_pci_simple_suspend_dev(ndev);

	nvme_start_freeze(ctrl);
	nvme_wait_freeze(ctrl);
	nvme_sync_queues(ctrl);

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
		goto unfreeze;

	/* Host memory may be unreachable while the system sleeps, but the
	 * controller may use it in a non-operational state: take it back. */
	ret = 0;
	mutex_lock(&ndev->hmb_lock);
	if (ndev->hmb)
		ret = nvme_set_host_mem(ndev, 0);
	mutex_unlock(&ndev->hmb_lock);
	if (ret < 0)
		goto unfreeze;

	ret = nvme_get_power_state(ctrl, &ndev->last_ps);
	if (ret < 0)
		goto unfreeze;

	/* A saved state keeps the PCI layer from changing the function's
	 * power on its own while the protocol's states are in use. */
	pci_save_state(pdev);

	ret = nvme_set_power_state(ctrl, ctrl->npss);
	if (ret < 0)
		goto unfreeze;

	if (ret) {
		/* The controller refused the state (an NVMe status): shut
		 * it down instead; clearing npss forces a reset on resume,
		 * which finds the right value again. */
		ret = nvme_disable_prepare_reset(ndev, true);
		ctrl->npss = 0;
		ndev->last_ps = 0xffffffffu;
	}
unfreeze:
	nvme_unfreeze(ctrl);
	return ret;
}

int nvme_pci_resume(pci_device_t *pdev)
{
	struct nvme_dev *ndev = pci_get_drvdata(pdev);
	struct nvme_ctrl *ctrl;

	might_sleep();
	if (!ndev)
		return 0;
	ctrl = &ndev->ctrl;

	if (ndev->last_ps == 0xffffffffu ||
	    nvme_set_power_state(ctrl, ndev->last_ps) != 0)
		goto reset;
	if (ctrl->hmpre) {
		int ret;

		mutex_lock(&ndev->hmb_lock);
		ret = nvme_setup_host_mem(ndev);
		mutex_unlock(&ndev->hmb_lock);
		if (ret)
			goto reset;
	}
	ndev->last_ps = 0xffffffffu;
	return 0;
reset:
	ndev->last_ps = 0xffffffffu;
	return nvme_pci_simple_resume_dev(ndev);
}
