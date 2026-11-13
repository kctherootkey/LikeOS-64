// LikeOS - PCI power management (D-states, PME), save/restore of config
// state, FLR and bus resets
//
// Power states: the Power Management capability's PMCSR, with the
// mandated transition delays (10 ms after leaving or entering D3hot,
// 200 us around D2) and the BAR restore a device that resets itself on
// D3hot -> D0 (No_Soft_Reset clear) needs.  D3cold needs platform power
// control (ACPI _PR3/_PS3), which LikeOS does not drive: a request for it
// leaves the function in D3hot.
//
// Saved state: the standard header, the PCI Express control registers
// (device, link, slot, root, and their "2" versions), LTR, L1 PM
// substates, DPC control, ACS control, and MSI / MSI-X (rewritten from
// the interrupt core's records).  The restore order follows the
// dependencies: the capability registers first, the header with the BARs
// before the command register, MSI last.
//
// Resets, in the order they are tried: PCIe FLR, Advanced Features FLR,
// PM reset (D3hot -> D0 on a function without No_Soft_Reset), secondary
// bus reset of the parent bridge (only when the function is alone below
// it).
// After a reset the function gets 100 ms, then is polled with config
// reads (doubling delays) for up to 60 s until it stops answering with
// all-ones.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/pci_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define PCIE_RESET_READY_POLL_MS 60000
#define PCI_RESET_WAIT 1000 /* ms, conventional reset recovery */
#define PCIE_LINK_RETRAIN_TIMEOUT_MS 1000
#define PCI_PM_D3COLD_DELAY_DEFAULT 100 /* ms */


static const char *pci_power_name(int state)
{
	static const char *const names[] = { "D0",     "D1",	  "D2",
					     "D3hot",  "D3cold", "unknown" };

	if (state < PCI_D0 || state > PCI_UNKNOWN)
		return "error";
	return names[state];
}

/* ==========================================================================
 * Power states
 * ========================================================================== */

void pci_pm_init_dev(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint16_t pmc = 0, pmcsr = 0, status = 0;

	BUG_ON(pp == NULL);
	pci_read_config_word(dev, PCI_STATUS, &status);
	pp->imm_ready = !PCI_POSSIBLE_ERROR(status) &&
			(status & PCI_STATUS_IMM_READY);
	if (!dev->pm_cap) {
		dev->current_state = PCI_D0;
		return;
	}
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_PMC, &pmc);
	if (PCI_POSSIBLE_ERROR(pmc) || (pmc & PCI_PM_CAP_VER_MASK) > 3) {
		/* An unknown PM capability version: do not drive it. */
		dev->pm_cap = 0;
		dev->current_state = PCI_D0;
		return;
	}
	pp->d1_support = (pmc & PCI_PM_CAP_D1) != 0;
	pp->d2_support = (pmc & PCI_PM_CAP_D2) != 0;
	pp->pme_support = (pmc & PCI_PM_CAP_PME_MASK) >> PCI_PM_CAP_PME_SHIFT;
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	if (!PCI_POSSIBLE_ERROR(pmcsr)) {
		dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK;
		pp->no_soft_reset_known = (pmcsr & PCI_PM_CTRL_NO_SOFT_RESET) !=
					  0;
	}
	if (!dev->d3hot_delay_ms)
		dev->d3hot_delay_ms = PCI_PM_D3HOT_WAIT;
}

static void pci_dev_d3_sleep(pci_device_t *dev)
{
	unsigned int delay = dev->d3hot_delay_ms;

	if (delay < PCI_PM_D3HOT_WAIT)
		delay = PCI_PM_D3HOT_WAIT;
	pci_msleep(delay);
}

/* Rewrite the BARs from the record (the device may have lost them on a
 * D3hot -> D0 internal reset). */
static void pci_restore_bars(pci_device_t *dev)
{
	int nbars = dev->hdr_type == PCI_HEADER_TYPE_NORMAL ? 6 :
		    dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ? 2 :
							      0;

	for (int i = 0; i < nbars; i++)
		pci_write_config_dword(dev, PCI_BASE_ADDRESS_0 + i * 4,
				       dev->bar[i]);
}

/* D0.  Returns 1 when the BARs need restoring, 0, or -EIO. */
static int pci_power_up(pci_device_t *dev)
{
	uint16_t pmcsr = 0;
	int state, need_restore;

	if (!dev->pm_cap) {
		dev->current_state = PCI_D0;
		return -EIO;
	}
	if (pci_dev_is_disconnected(dev)) {
		dev->current_state = PCI_D3cold;
		return -EIO;
	}
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	if (PCI_POSSIBLE_ERROR(pmcsr)) {
		pci_err(dev, "cannot change power state from %s to D0, device inaccessible\n",
			pci_power_name(dev->current_state));
		dev->current_state = PCI_D3cold;
		return -EIO;
	}
	state = pmcsr & PCI_PM_CTRL_STATE_MASK;
	need_restore = (state == PCI_D3hot || dev->current_state >= PCI_D3hot) &&
		       !(pmcsr & PCI_PM_CTRL_NO_SOFT_RESET);
	if (state != PCI_D0) {
		/* The whole word to 0: PME_Status untouched, PME_En off,
		 * PowerState D0. */
		pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, 0);
		if (state == PCI_D3hot)
			pci_dev_d3_sleep(dev);
		else if (state == PCI_D2)
			pci_udelay(PCI_PM_D2_DELAY);
	}
	dev->current_state = PCI_D0;
	return need_restore ? 1 : 0;
}

static int pci_set_full_power_state(pci_device_t *dev)
{
	uint16_t pmcsr = 0;
	int ret = pci_power_up(dev);

	if (ret < 0)
		return dev->current_state == PCI_D0 ? 0 : ret;
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK;
	if (dev->current_state != PCI_D0) {
		WARN_RATELIMIT(1, "pci %s: refused to change power state from %s to D0",
			       pci_priv(dev)->name,
			       pci_power_name(dev->current_state));
	} else if (ret > 0) {
		/* D3hot -> D0 may have reset the function internally ("D0
		 * uninitialised"): at least the BARs must come back. */
		pci_restore_bars(dev);
	}
	return 0;
}

static int pci_set_low_power_state(pci_device_t *dev, int state)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint16_t pmcsr = 0;

	if (!dev->pm_cap)
		return -EIO;
	/* From a low-power state only deeper (or D0). */
	if (dev->current_state <= PCI_D3cold && dev->current_state > state)
		return -EINVAL;
	if ((state == PCI_D1 && !pp->d1_support) ||
	    (state == PCI_D2 && !pp->d2_support))
		return -EIO;
	if (dev->current_state == state)
		return 0;
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	if (PCI_POSSIBLE_ERROR(pmcsr)) {
		pci_err(dev, "cannot change power state from %s to %s, device inaccessible\n",
			pci_power_name(dev->current_state),
			pci_power_name(state));
		dev->current_state = PCI_D3cold;
		return -EIO;
	}
	pmcsr &= ~PCI_PM_CTRL_STATE_MASK;
	pmcsr |= (uint16_t)state;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);
	if (state == PCI_D3hot)
		pci_dev_d3_sleep(dev);
	else if (state == PCI_D2)
		pci_udelay(PCI_PM_D2_DELAY);
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK;
	if (dev->current_state != state)
		WARN_RATELIMIT(1, "pci %s: refused to change power state from %s to %s",
			       pp->name, pci_power_name(dev->current_state),
			       pci_power_name(state));
	return 0;
}

int pci_set_power_state(pci_device_t *dev, pci_power_t state)
{
	might_sleep();
	if (!dev)
		return -EINVAL;
	if (state > PCI_D3cold)
		state = PCI_D3cold;
	else if (state < PCI_D0)
		state = PCI_D0;
	if (dev->current_state == state)
		return 0;
	if (state == PCI_D0)
		return pci_set_full_power_state(dev);
	if (state >= PCI_D3hot && (dev->flags & PCI_DEV_FL_NO_D3))
		return 0;
	if (state == PCI_D3cold) {
		int rc;

		/* D3hot natively; the power removal needs the platform. */
		rc = pci_set_low_power_state(dev, PCI_D3hot);
		if (rc)
			return rc;
		return 0;
	}
	return pci_set_low_power_state(dev, state);
}

int pci_enable_wake(pci_device_t *dev, pci_power_t state, int enable)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint16_t pmcsr = 0;

	if (!dev || !pp || !dev->pm_cap)
		return -EINVAL;
	if (pci_dev_is_disconnected(dev))
		return -ENODEV;
	if (enable) {
		int bit_state = state > PCI_D3cold ? PCI_D3cold : state;

		if (!(pp->pme_support & (1 << bit_state)))
			return -EINVAL;
	}
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
	/* PME_Status is write-1-to-clear: clear it before (re)arming. */
	pmcsr |= PCI_PM_CTRL_PME_STATUS;
	if (enable)
		pmcsr |= PCI_PM_CTRL_PME_ENABLE;
	else
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);
	return 0;
}

/* ==========================================================================
 * Save / restore
 * ========================================================================== */

static const int g_pcie_save_regs[7] = { PCI_EXP_DEVCTL,  PCI_EXP_LNKCTL,
					 PCI_EXP_SLTCTL,  PCI_EXP_RTCTL,
					 PCI_EXP_DEVCTL2, PCI_EXP_LNKCTL2,
					 PCI_EXP_SLTCTL2 };

int pci_save_state(pci_device_t *dev)
{
	struct pci_saved_state *s;

	might_sleep();
	if (!dev)
		return -EINVAL;
	if (!dev->saved) {
		dev->saved = kcalloc(1, sizeof(*dev->saved));
		if (!dev->saved)
			return -ENOMEM;
	}
	s = dev->saved;
	for (int i = 0; i < 16; i++)
		pci_read_config_dword(dev, i * 4, &s->config[i]);
	s->pcie_valid = 0;
	if (pci_is_pcie(dev)) {
		for (int i = 0; i < 7; i++)
			pcie_capability_read_word(dev, g_pcie_save_regs[i],
						  &s->pcie[i]);
		s->pcie_valid = 1;
	}
	if (dev->ltr_cap)
		pci_read_config_dword(dev, dev->ltr_cap + PCI_LTR_MAX_SNOOP_LAT,
				      &s->ltr);
	if (dev->l1ss_cap) {
		pci_read_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL1,
				      &s->l1ss_ctl1);
		pci_read_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL2,
				      &s->l1ss_ctl2);
	}
	if (dev->dpc_cap)
		pci_read_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL,
				     &s->dpc_ctl);
	if (dev->acs_cap)
		pci_read_config_word(dev, dev->acs_cap + PCI_ACS_CTRL,
				     &s->acs_ctrl);
	pci_msi_save_state(dev);
	dev->flags |= PCI_DEV_FL_STATE_SAVED;
	return 0;
}

static void pci_restore_config_dword(pci_device_t *dev, int offset,
				     uint32_t saved, int retry, int force)
{
	uint32_t val = 0;

	pci_read_config_dword(dev, offset, &val);
	if (!force && val == saved)
		return;
	for (;;) {
		pci_write_config_dword(dev, offset, saved);
		if (retry-- <= 0)
			return;
		pci_read_config_dword(dev, offset, &val);
		if (val == saved)
			return;
		pci_msleep(1);
	}
}

static void pci_restore_config_range(pci_device_t *dev, int start, int end,
				     int retry, int force)
{
	for (int i = end; i >= start; i--)
		pci_restore_config_dword(dev, 4 * i, dev->saved->config[i],
					 retry, force);
}

static void pci_restore_config_space(pci_device_t *dev)
{
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		pci_restore_config_range(dev, 10, 15, 0, 0);
		/* BARs before the command register. */
		pci_restore_config_range(dev, 4, 9, 10, 0);
		pci_restore_config_range(dev, 0, 3, 0, 0);
	} else if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		pci_restore_config_range(dev, 12, 15, 0, 0);
		/* The prefetchable window is rewritten even when it reads
		 * back unchanged: some bridges lose it otherwise. */
		pci_restore_config_range(dev, 9, 11, 0, 1);
		pci_restore_config_range(dev, 0, 8, 0, 0);
	} else {
		pci_restore_config_range(dev, 0, 15, 0, 0);
	}
}

void pci_restore_state(pci_device_t *dev)
{
	struct pci_saved_state *s;

	might_sleep();
	if (!dev || !(dev->flags & PCI_DEV_FL_STATE_SAVED) || !dev->saved)
		return;
	s = dev->saved;
	if (s->pcie_valid && pci_is_pcie(dev)) {
		for (int i = 0; i < 7; i++)
			pcie_capability_write_word(dev, g_pcie_save_regs[i],
						   s->pcie[i]);
	}
	if (dev->ltr_cap)
		pci_write_config_dword(dev, dev->ltr_cap + PCI_LTR_MAX_SNOOP_LAT,
				       s->ltr);
	if (dev->l1ss_cap) {
		/* CTL2 first, then CTL1 with the enables cleared, then the
		 * enables (the substates must not be enabled while their
		 * timing is being set). */
		pci_write_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL2,
				       s->l1ss_ctl2);
		pci_write_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL1,
				       s->l1ss_ctl1 & ~PCI_L1SS_CTL1_L1SS_MASK);
		pci_write_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL1,
				       s->l1ss_ctl1);
	}
	if (dev->dpc_cap)
		pci_write_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL,
				      s->dpc_ctl);
	pci_restore_config_space(dev);
	pci_msi_restore_state(dev);
	if (dev->acs_cap)
		pci_write_config_word(dev, dev->acs_cap + PCI_ACS_CTRL,
				      s->acs_ctrl);
	/* The BARs were written back as the cached decode describes them:
	 * the cache stays (no sizing of a live function). */
	dev->flags &= ~PCI_DEV_FL_STATE_SAVED;
}

/* ==========================================================================
 * Waiting for a device after a reset
 * ========================================================================== */

int pci_dev_wait_ready(pci_device_t *dev, const char *reset_type,
		       int timeout_ms)
{
	int delay = 1;

	might_sleep();
	if (!dev)
		return -EINVAL;
	/* After a reset the function may answer Configuration Request
	 * Retry Status for a while; without RRS software visibility that
	 * reads as all-ones (the root complex gives up retrying), so the
	 * command register is read until it is something else. */
	for (;;) {
		uint32_t id = PCI_ERROR_RESPONSE;

		if (pci_dev_is_disconnected(dev))
			return -ENOTTY;
		pci_raw_read(dev->segment, dev->bus,
			     (uint8_t)PCI_DEVFN(dev->device, dev->function),
			     PCI_COMMAND, 4, &id);
		if (!PCI_POSSIBLE_ERROR(id))
			break;
		if (delay > timeout_ms) {
			pci_warn(dev, "not ready %dms after %s; giving up\n",
				 delay - 1, reset_type);
			return -ENOTTY;
		}
		if (delay > PCI_RESET_WAIT)
			pci_info(dev, "not ready %dms after %s; waiting\n",
				 delay - 1, reset_type);
		pci_msleep((unsigned int)delay);
		delay *= 2;
	}
	if (delay > PCI_RESET_WAIT)
		pci_info(dev, "ready %dms after %s\n", delay - 1, reset_type);
	return 0;
}

static int pci_wait_for_pending(pci_device_t *dev, int pos, uint16_t mask)
{
	for (int i = 0; i < 4; i++) {
		uint16_t status = 0;

		if (i)
			pci_msleep((1u << (i - 1)) * 100);
		pci_read_config_word(dev, pos, &status);
		if (!(status & mask))
			return 1;
	}
	return 0;
}

static int pci_wait_for_pending_transaction(pci_device_t *dev)
{
	if (!pci_is_pcie(dev))
		return 1;
	return pci_wait_for_pending(dev, dev->pcie_cap + PCI_EXP_DEVSTA,
				    PCI_EXP_DEVSTA_TRPND);
}

static int pcie_link_active_reporting(pci_device_t *port)
{
	uint32_t lnkcap = 0;

	pcie_capability_read_dword(port, PCI_EXP_LNKCAP, &lnkcap);
	return (lnkcap & PCI_EXP_LNKCAP_DLLLARC) != 0;
}

static int pcie_wait_for_link_status(pci_device_t *port, int use_lt, int active)
{
	uint16_t mask = use_lt ? PCI_EXP_LNKSTA_LT : PCI_EXP_LNKSTA_DLLLA;
	uint16_t match = active ? mask : 0;

	for (int waited = 0; waited <= PCIE_LINK_RETRAIN_TIMEOUT_MS; waited++) {
		uint16_t lnksta = 0;

		pcie_capability_read_word(port, PCI_EXP_LNKSTA, &lnksta);
		if ((lnksta & mask) == match)
			return 0;
		pci_msleep(1);
	}
	return -ETIMEDOUT;
}

static int pcie_wait_for_link_delay(pci_device_t *port, int active, int delay)
{
	int rc;

	if (!pcie_link_active_reporting(port)) {
		pci_msleep(PCIE_LINK_RETRAIN_TIMEOUT_MS + delay);
		return 1;
	}
	/* The LTSSM reaches Detect within 20 ms; the link is expected up
	 * after that, and the device gets `delay' more before config
	 * requests. */
	if (active)
		pci_msleep(20);
	rc = pcie_wait_for_link_status(port, 0, active);
	if (active) {
		if (rc)
			return 0;
		pci_msleep((unsigned int)delay);
		return 1;
	}
	return rc ? 0 : 1;
}

int pci_bridge_wait_for_secondary_bus(pci_device_t *bridge,
				      const char *reset_type)
{
	pci_device_t *kids[1];
	pci_device_t *child;
	int delay = PCI_PM_D3COLD_DELAY_DEFAULT;
	unsigned int speed = 0;

	if (pci_dev_is_disconnected(bridge) || !pci_is_bridge(bridge))
		return 0;
	if (pci_bridge_children(bridge, kids, 1) < 1)
		return 0;
	child = kids[0];
	/* Conventional PCI / PCI-X: Tpvrh + Trhfa. */
	if (!pci_is_pcie(bridge)) {
		pci_msleep(1000 + (unsigned int)delay);
		return 0;
	}
	if (pci_pcie_type(bridge) != PCI_EXP_TYPE_ROOT_PORT &&
	    pci_pcie_type(bridge) != PCI_EXP_TYPE_DOWNSTREAM &&
	    pci_pcie_type(bridge) != PCI_EXP_TYPE_PCIE_BRIDGE)
		return 0;
	pcie_link_capability(bridge, &speed, NULL);
	if (speed <= 2 /* 5 GT/s */) {
		uint16_t status = 0;

		pci_msleep((unsigned int)delay);
		if (!pci_dev_wait_ready(child, reset_type,
					PCI_RESET_WAIT - delay))
			return 0;
		if (!pcie_link_active_reporting(bridge))
			return -ENOTTY;
		pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &status);
		if (!(status & PCI_EXP_LNKSTA_DLLLA))
			return -ENOTTY;
		return pci_dev_wait_ready(child, reset_type,
					  PCIE_RESET_READY_POLL_MS -
						  PCI_RESET_WAIT);
	}
	if (!pcie_wait_for_link_delay(bridge, 1, delay)) {
		pci_info(bridge, "Data Link Layer Link Active not set in %d msec\n",
			 delay);
		return -ENOTTY;
	}
	return pci_dev_wait_ready(child, reset_type,
				  PCIE_RESET_READY_POLL_MS - delay);
}

/* ==========================================================================
 * Reset methods
 * ========================================================================== */

int pcie_flr(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	might_sleep();
	if (!dev || !pp)
		return -EINVAL;
	if (!pci_wait_for_pending_transaction(dev))
		pci_err(dev, "timed out waiting for pending transaction; performing function level reset anyway\n");
	pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL, 0,
					   PCI_EXP_DEVCTL_BCR_FLR);
	if (pp->imm_ready)
		return 0;
	/* 100 ms before the function may be accessed. */
	pci_msleep(100);
	return pci_dev_wait_ready(dev, "FLR", PCIE_RESET_READY_POLL_MS);
}

int pcie_reset_flr(pci_device_t *dev, int probe)
{
	uint32_t cap = 0;

	if (!dev || (dev->flags & PCI_DEV_FL_NO_FLR))
		return -ENOTTY;
	if (!pci_is_pcie(dev))
		return -ENOTTY;
	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	if (!(cap & PCI_EXP_DEVCAP_FLR))
		return -ENOTTY;
	if (probe)
		return 0;
	return pcie_flr(dev);
}

static int pci_af_flr(pci_device_t *dev, int probe)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint8_t cap = 0;
	int pos = dev->af_cap;

	if (!pos || (dev->flags & PCI_DEV_FL_NO_FLR))
		return -ENOTTY;
	pci_read_config_byte(dev, pos + PCI_AF_CAP, &cap);
	if (!(cap & PCI_AF_CAP_TP) || !(cap & PCI_AF_CAP_FLR))
		return -ENOTTY;
	if (probe)
		return 0;
	/* Transactions Pending is the status byte above the control byte. */
	if (!pci_wait_for_pending(dev, pos + PCI_AF_CTRL,
				  PCI_AF_STATUS_TP << 8))
		pci_err(dev, "timed out waiting for pending transaction; performing AF function level reset anyway\n");
	pci_write_config_byte(dev, pos + PCI_AF_CTRL, PCI_AF_CTRL_FLR);
	if (pp && pp->imm_ready)
		return 0;
	pci_msleep(100);
	return pci_dev_wait_ready(dev, "AF_FLR", PCIE_RESET_READY_POLL_MS);
}

static int pci_pm_reset(pci_device_t *dev, int probe)
{
	uint16_t csr = 0;

	if (!dev->pm_cap)
		return -ENOTTY;
	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &csr);
	if (PCI_POSSIBLE_ERROR(csr) || (csr & PCI_PM_CTRL_NO_SOFT_RESET))
		return -ENOTTY;
	if (probe)
		return 0;
	if (dev->current_state != PCI_D0)
		return -EINVAL;
	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D3hot;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);
	pci_dev_d3_sleep(dev);
	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D0;
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr);
	pci_dev_d3_sleep(dev);
	return pci_dev_wait_ready(dev, "PM D3hot->D0", PCIE_RESET_READY_POLL_MS);
}

static void pci_reset_secondary_bus(pci_device_t *bridge)
{
	uint16_t ctrl = 0;

	pci_read_config_word(bridge, PCI_BRIDGE_CONTROL, &ctrl);
	ctrl |= PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, ctrl);
	/* Trst is at least 1 ms; twice that. */
	pci_msleep(2);
	ctrl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, ctrl);
}

int pci_bridge_secondary_bus_reset(pci_device_t *bridge)
{
	might_sleep();
	if (!bridge || !pci_is_bridge(bridge))
		return -ENOTTY;
	pci_reset_secondary_bus(bridge);
	return pci_bridge_wait_for_secondary_bus(bridge, "bus reset");
}

/* The parent bridge's bus reset, when this function is the only thing
 * below it (a bus reset resets everybody on the bus). */
static int pci_parent_bus_reset(pci_device_t *dev, int probe)
{
	pci_device_t *bridge = dev->bus_self;
	pci_device_t *kids[2];

	if (!bridge || pci_is_bridge(dev))
		return -ENOTTY;
	if (pci_bridge_children(bridge, kids, 2) != 1 || kids[0] != dev)
		return -ENOTTY;
	if (probe)
		return 0;
	return pci_bridge_secondary_bus_reset(bridge);
}

void pci_init_reset_methods(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint32_t m = 0;

	BUG_ON(pp == NULL);
	if (pcie_reset_flr(dev, 1) == 0)
		m |= PCI_RESET_FLR;
	if (pci_af_flr(dev, 1) == 0)
		m |= PCI_RESET_AF_FLR;
	if (pci_pm_reset(dev, 1) == 0)
		m |= PCI_RESET_PM;
	if (dev->bus_self && !pci_is_bridge(dev))
		m |= PCI_RESET_BUS; /* rechecked when used (bus contents) */
	pp->reset_methods = m;
}

int pci_reset_function_locked(pci_device_t *dev)
{
	const struct pci_error_handlers *eh;
	int rc;

	might_sleep();
	if (!dev || !pci_priv(dev))
		return -EINVAL;
	eh = dev->driver ? dev->driver->err_handler : NULL;
	/* Save and quiesce: the driver first, then D0, the state, and the
	 * command register to "INTx disabled, nothing else" (no decode, no
	 * bus mastering: no DMA, no MSI). */
	if (eh && eh->reset_prepare)
		eh->reset_prepare(dev);
	pci_set_power_state(dev, PCI_D0);
	pci_save_state(dev);
	pci_write_config_word(dev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);

	rc = -ENOTTY;
	{
		int (*const methods[])(pci_device_t *, int) = {
			pcie_reset_flr, pci_af_flr, pci_pm_reset,
			pci_parent_bus_reset
		};

		for (unsigned int i = 0; i < 4; i++) {
			rc = methods[i](dev, 0);
			if (rc == 0 || rc != -ENOTTY)
				break;
		}
	}

	pci_restore_state(dev);
	if (eh && eh->reset_done)
		eh->reset_done(dev);
	return rc;
}

int pci_reset_function(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	pci_device_t *bridge;
	int rc;

	might_sleep();
	if (!dev || !pp)
		return -EINVAL;
	if (!pp->reset_methods)
		return -ENOTTY;
	/* The bridge above is locked too: its configuration (the bus
	 * reset) must stay consistent during the reset. */
	bridge = dev->bus_self;
	if (bridge && pci_priv(bridge))
		pci_dev_lock(bridge);
	pci_dev_lock(dev);
	rc = pci_reset_function_locked(dev);
	pci_dev_unlock(dev);
	if (bridge && pci_priv(bridge))
		pci_dev_unlock(bridge);
	return rc;
}
