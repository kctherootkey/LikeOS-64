// LikeOS I2C - DesignWare controllers described by ACPI
//
// Controllers that firmware lists as ACPI devices with a _HID rather than
// as PCI functions:
//
//   * Intel Bay Trail / Cherry Trail (80860F41, 808622C1) and Haswell /
//     Broadwell (INT33C2/3, INT3432/3) LPSS controllers: a private block
//     at +0x800 (resets, clock gate; on Bay Trail a non-zero clock
//     parameter register means a 133 MHz input clock, else 100 MHz);
//   * Skylake-era LPSS functions in ACPI mode (INT3442-INT3447,
//     INT34B2-INT34B7, Broxton 80860AAC, Apollo Lake 80865AAC): the PCI
//     layout, private block at +0x200;
//   * AMD FCH controllers (AMD0010 133 MHz, AMDI0010 150 MHz, AMDI0019
//     150 MHz with PSP arbitration, AMDI0510 without a fixed clock),
//     Hygon (HYGO0010 150 MHz), and the generic ids other platforms use
//     for the same IP (their input clock is not known here: the counts
//     firmware programmed, or ACPI's, are kept).
//
// Registers come from the first memory resource of _CRS, the interrupt
// from the first Interrupt/ExtendedIRQ (none: polled transfers).  The
// device is put in D0 (_PS0, power resources) first.  The device itself
// is the adapter's ACPI companion: its children's I2cSerialBus resources
// name it.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2006 Texas Instruments, 2007 MontaVista Software
// Inc., 2009 Provigent Ltd., 2014-2016 Intel Corporation

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define DRIVER_NAME "i2c_designware"
#define DW_ACPI_MAX_CONTROLLERS 16

enum dw_acpi_kind {
	DW_ACPI_PLAIN = 0, /* no private block */
	DW_ACPI_LPSS_OLD, /* private block at +0x800 */
	DW_ACPI_LPSS_NEW, /* private block at +0x200 (PCI layout) */
};

struct dw_acpi_cfg {
	enum dw_acpi_kind kind;
	uint32_t flags; /* ACCESS_* ... */
	uint32_t clk_hz; /* 0: unknown */
	/* LPSS: SDA hold / fall times in ns (0: none given). */
	uint32_t sda_hold_ns, sda_fall_ns, scl_fall_ns;
	uint8_t clk_gate; /* +0x800 clock gate bit to set */
	uint8_t byt_clk; /* clock from the clock parameter register */
	const char *platform;
};

enum {
	CFG_BYT,
	CFG_CHT,
	CFG_HSW,
	CFG_BDW,
	CFG_SPT,
	CFG_CNL,
	CFG_BXT,
	CFG_APL,
	CFG_AMD_CZ,
	CFG_AMD,
	CFG_AMD_PSP,
	CFG_AMD_0510,
	CFG_INTR_MASK,
	CFG_HYGON,
	CFG_GENERIC,
	CFG_COUNT,
};

static const struct dw_acpi_cfg dw_acpi_cfgs[CFG_COUNT] = {
	[CFG_BYT] = { DW_ACPI_LPSS_OLD, ACCESS_NO_IRQ_SUSPEND, 100000000, 0, 0,
		      0, 0, 1, "Bay Trail" },
	[CFG_CHT] = { DW_ACPI_LPSS_OLD, ACCESS_NO_IRQ_SUSPEND, 100000000, 0, 0,
		      0, 0, 1, "Cherry Trail" },
	[CFG_HSW] = { DW_ACPI_LPSS_OLD, 0, 100000000, 0, 0, 0, 1, 0,
		      "Lynx Point" },
	[CFG_BDW] = { DW_ACPI_LPSS_OLD, 0, 100000000, 0, 0, 0, 1, 0,
		      "Wildcat Point" },
	[CFG_SPT] = { DW_ACPI_LPSS_NEW, 0, 120000000, 230, 0, 0, 0, 0,
		      "Sunrise Point" },
	[CFG_CNL] = { DW_ACPI_LPSS_NEW, 0, 216000000, 230, 0, 0, 0, 0,
		      "Cannon Lake" },
	[CFG_BXT] = { DW_ACPI_LPSS_NEW, 0, 133000000, 42, 171, 208, 0, 0,
		      "Broxton" },
	[CFG_APL] = { DW_ACPI_LPSS_NEW, 0, 133000000, 207, 171, 208, 0, 0,
		      "Apollo Lake" },
	[CFG_AMD_CZ] = { DW_ACPI_PLAIN, ACCESS_INTR_MASK, 133000000, 0, 0, 0,
			 0, 0, "AMD Carrizo" },
	[CFG_AMD] = { DW_ACPI_PLAIN, ACCESS_INTR_MASK, 150000000, 0, 0, 0, 0,
		      0, "AMD FCH" },
	[CFG_AMD_PSP] = { DW_ACPI_PLAIN,
			  ACCESS_INTR_MASK | ARBITRATION_SEMAPHORE, 150000000,
			  0, 0, 0, 0, 0, "AMD FCH (PSP arbitrated)" },
	[CFG_AMD_0510] = { DW_ACPI_PLAIN, 0, 0, 0, 0, 0, 0, 0, "AMD" },
	[CFG_INTR_MASK] = { DW_ACPI_PLAIN, ACCESS_INTR_MASK, 0, 0, 0, 0, 0, 0,
			    "DesignWare" },
	[CFG_HYGON] = { DW_ACPI_PLAIN, ACCESS_INTR_MASK, 150000000, 0, 0, 0, 0,
			0, "Hygon FCH" },
	[CFG_GENERIC] = { DW_ACPI_PLAIN, 0, 0, 0, 0, 0, 0, 0, "DesignWare" },
};

static const struct acpi_device_id dw_i2c_acpi_match[] = {
	{ "80860F41", CFG_BYT },
	{ "808622C1", CFG_CHT },
	{ "AMD0010", CFG_AMD_CZ },
	{ "AMDI0010", CFG_AMD },
	{ "AMDI0019", CFG_AMD_PSP },
	{ "AMDI0510", CFG_AMD_0510 },
	{ "APMC0D0F", CFG_GENERIC },
	{ "FUJI200B", CFG_GENERIC },
	{ "GOOG5000", CFG_GENERIC },
	{ "HISI02A1", CFG_GENERIC },
	{ "HISI02A2", CFG_GENERIC },
	{ "HISI02A3", CFG_GENERIC },
	{ "HJMC3001", CFG_INTR_MASK },
	{ "HYGO0010", CFG_HYGON },
	{ "INT33C2", CFG_HSW },
	{ "INT33C3", CFG_HSW },
	{ "INT3432", CFG_BDW },
	{ "INT3433", CFG_BDW },
	{ "INTC10EF", CFG_GENERIC },
	{ "LECA0003", CFG_GENERIC },
	/* LPSS functions in ACPI mode */
	{ "INT3442", CFG_SPT },
	{ "INT3443", CFG_SPT },
	{ "INT3444", CFG_SPT },
	{ "INT3445", CFG_SPT },
	{ "INT3446", CFG_SPT },
	{ "INT3447", CFG_SPT },
	{ "INT34B2", CFG_CNL },
	{ "INT34B3", CFG_CNL },
	{ "INT34B4", CFG_CNL },
	{ "INT34B5", CFG_CNL },
	{ "INT34B6", CFG_CNL },
	{ "INT34B7", CFG_CNL },
	{ "80860AAC", CFG_BXT },
	{ "80865AAC", CFG_APL },
	{ "", 0 },
};

/* Machines whose DesignWare buses carry hardware monitoring chips. */
static const struct dmi_system_id dw_i2c_hwmon_class_dmi[] = {
	{
		.ident = "Qtechnology QT5222",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Qtechnology"),
			DMI_MATCH(DMI_PRODUCT_NAME, "QT5222"),
		},
	},
	{ 0 }
};

struct dw_acpi_ctrl {
	struct dw_i2c_dev *dev;
	uint64_t phys;
	uint32_t priv_ctx[LPSS_PRIV_REG_COUNT];
};

static struct dw_acpi_ctrl g_ctrls[DW_ACPI_MAX_CONTROLLERS];
static int g_nctrls;

static uint32_t dw_acpi_clk_khz(struct dw_i2c_dev *dev)
{
	return dev->clk_khz;
}

/* Clock gate on, resets released (Bay Trail ... Broadwell). */
static void dw_lpss_old_setup(struct dw_i2c_dev *dev,
			      const struct dw_acpi_cfg *cfg)
{
	lpss_priv_writel(dev, LPSS_PRIV_RESETS_FUNC, LPSS_PRIV_RESETS);
	if (cfg->clk_gate)
		lpss_priv_writel(dev,
				 lpss_priv_readl(dev, LPSS_PRIV_CLOCK_PARAMS) |
					 LPSS_PRIV_CLOCK_GATE_EN,
				 LPSS_PRIV_CLOCK_PARAMS);
	if (cfg->byt_clk && lpss_priv_readl(dev, LPSS_PRIV_CLOCK_PARAMS))
		dev->clk_khz = 133000;
	/* the controller disabled until it is configured */
	dw_mmio_writel(0, dev->base + DW_IC_ENABLE);
}

static void dw_lpss_old_suspend(struct dw_i2c_dev *dev)
{
	uint32_t *ctx = dev->glue;

	for (unsigned int i = 0; i < LPSS_PRIV_REG_COUNT; i++)
		ctx[i] = lpss_priv_readl(dev, i * 4);
}

static void dw_lpss_old_resume(struct dw_i2c_dev *dev)
{
	uint32_t *ctx = dev->glue;

	lpss_priv_writel(dev, LPSS_PRIV_RESETS_FUNC, LPSS_PRIV_RESETS);
	for (unsigned int i = 0; i < LPSS_PRIV_REG_COUNT; i++)
		if (i * 4 != LPSS_PRIV_RESETS)
			lpss_priv_writel(dev, ctx[i], i * 4);
}

enum {
	DW_SEM_BAYTRAIL,
	DW_SEM_AMDPSP,
};

int i2c_dw_probe_lock_support(struct dw_i2c_dev *dev)
{
	int (*const probes[])(struct dw_i2c_dev *) = {
		[DW_SEM_BAYTRAIL] = i2c_dw_baytrail_probe_lock_support,
		[DW_SEM_AMDPSP] = i2c_dw_amdpsp_probe_lock_support,
	};

	dev->semaphore_idx = -1;
	dev->needs_arbitration = 0;
	for (unsigned int i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		int ret = probes[i](dev);

		if (ret == -ENODEV && !dev->needs_arbitration)
			continue; /* no semaphore of this kind */
		if (ret) {
			/* The bus is shared with firmware and cannot be
			 * arbitrated: running it anyway would corrupt the
			 * firmware's transfers (PMIC, PSP), so no adapter. */
			kprintf("%s: bus shared with firmware, arbitration unavailable, adapter disabled\n",
				dev->name);
			dev->acquire_lock = NULL;
			dev->release_lock = NULL;
			dev->shared_with_punit = 0;
			return ret == -ENODEV ? -EIO : ret;
		}
		dev->semaphore_idx = (int)i;
		break;
	}
	return 0;
}

void i2c_dw_remove_lock_support(struct dw_i2c_dev *dev)
{
	if (dev->semaphore_idx == DW_SEM_AMDPSP)
		i2c_dw_amdpsp_remove_lock_support(dev);
	dev->semaphore_idx = -1;
	dev->acquire_lock = NULL;
	dev->release_lock = NULL;
}

static int dw_acpi_probe_one(struct acpi_device *adev,
			     const struct dw_acpi_cfg *cfg)
{
	struct dw_acpi_ctrl *ctrl;
	struct dw_i2c_dev *dev;
	struct acpi_res_list res;
	const struct acpi_res *mem, *irqres;
	uint64_t phys, len, page, off, va;
	size_t pages, priv_off = 0;
	int ret, irq = 0;

	if (g_nctrls >= DW_ACPI_MAX_CONTROLLERS)
		return -ENOSPC;
	kmemset(&res, 0, sizeof(res));
	ret = acpi_dev_get_resources(adev, &res);
	if (ret)
		return ret;
	mem = acpi_res_find(&res, ACPI_RES_MEM, 0);
	if (!mem || mem->mem.length < DW_IC_REG_SPACE) {
		acpi_dev_free_resource_list(&res);
		kprintf(DRIVER_NAME " %s: no register window\n", adev->path);
		return -ENODEV;
	}
	phys = mem->mem.start;
	len = mem->mem.length;
	irqres = acpi_res_find(&res, ACPI_RES_IRQ, 0);

	if (acpi_device_power_manageable(adev)) {
		ret = acpi_device_set_power(adev, ACPI_DEV_D0);
		if (ret)
			kprintf(DRIVER_NAME " %s: _PS0 failed (%d)\n",
				adev->path, ret);
	}

	page = phys & ~0xfffull;
	off = phys - page;
	pages = (size_t)((off + len + 0xfff) >> 12);
	va = mm_map_device_mmio(page, pages);
	if (!va) {
		acpi_dev_free_resource_list(&res);
		return -ENOMEM;
	}

	dev = kcalloc(1, sizeof(*dev));
	if (!dev) {
		acpi_dev_free_resource_list(&res);
		return -ENOMEM;
	}
	ctrl = &g_ctrls[g_nctrls];
	ctrl->dev = dev;
	ctrl->phys = phys;
	ksnprintf(dev->name, sizeof(dev->name), DRIVER_NAME " %s:%s",
		  adev->hid, adev->has_uid ? adev->uid : "0");
	dev->base = (volatile uint8_t *)(unsigned long)(va + off);
	dev->acpi_dev = acpi_dev_get(adev);
	dev->flags = cfg->flags;
	dev->adapter.nr = -1;
	dev->adapter.class = dmi_first_match(dw_i2c_hwmon_class_dmi) ?
				     I2C_CLASS_HWMON :
				     I2C_CLASS_DEPRECATED;
	dev->clk_khz = cfg->clk_hz / 1000;
	dev->get_clk_rate_khz = cfg->clk_hz ? dw_acpi_clk_khz : NULL;
	dev->timings.sda_hold_ns = cfg->sda_hold_ns;
	dev->timings.sda_fall_ns = cfg->sda_fall_ns;
	dev->timings.scl_fall_ns = cfg->scl_fall_ns;

	if (cfg->kind == DW_ACPI_LPSS_OLD)
		priv_off = LPSS_PRIV_OFFSET_LEGACY;
	else if (cfg->kind == DW_ACPI_LPSS_NEW)
		priv_off = LPSS_PRIV_OFFSET_PCI;
	if (priv_off && len < priv_off + LPSS_PRIV_SIZE) {
		kprintf(DRIVER_NAME " %s: window of %lu bytes has no private block\n",
			adev->path, (unsigned long)len);
		priv_off = 0;
	}
	if (priv_off) {
		dev->lpss_priv = dev->base + priv_off;
		dev->glue = ctrl->priv_ctx;
		if (cfg->kind == DW_ACPI_LPSS_OLD) {
			dw_lpss_old_setup(dev, cfg);
			dev->glue_suspend = dw_lpss_old_suspend;
			dev->glue_resume = dw_lpss_old_resume;
		} else {
			i2c_dw_lpss_init_priv(dev, phys);
			dev->glue_suspend = i2c_dw_lpss_suspend;
			dev->glue_resume = i2c_dw_lpss_resume;
		}
	}

	if (irqres && !irqres->irq.source[0]) {
		irq = irq_alloc_desc_gsi(NULL, dev, (int)irqres->irq.gsi,
					 irqres->irq.polarity ==
						 ACPI_RES_ACTIVE_LOW,
					 irqres->irq.triggering ==
						 ACPI_RES_LEVEL);
		if (irq <= 0) {
			kprintf(DRIVER_NAME " %s: GSI %u unavailable (%d), polled\n",
				adev->path, irqres->irq.gsi, irq);
			irq = 0;
		}
	}
	acpi_dev_free_resource_list(&res);
	dev->irq = irq;
	dev->irq_kind = "GSI";
	if (!irq)
		dev->flags |= ACCESS_POLLING;

	kprintf(DRIVER_NAME " %s: %s (%s) at 0x%lx, %s\n", adev->path,
		adev->hid, cfg->platform, (unsigned long)phys,
		irq ? "interrupt" : "no interrupt, polled");

	ret = i2c_dw_fw_parse_and_configure(dev);
	if (ret)
		goto err;
	/* A bus shared with firmware gets its semaphore here, or no
	 * adapter (the refusal is logged). */
	ret = i2c_dw_probe_lock_support(dev);
	if (ret)
		goto err;
	i2c_dw_configure_master(dev);
	if (dev->get_clk_rate_khz && !dev->sda_hold_time &&
	    dev->timings.sda_hold_ns)
		dev->sda_hold_time = (uint32_t)(((uint64_t)dev->clk_khz *
							 dev->timings.sda_hold_ns +
						 500000ull) /
						1000000ull);
	ret = i2c_dw_probe(dev);
	if (ret) {
		i2c_dw_remove_lock_support(dev);
		goto err;
	}
	g_nctrls++;
	/* The P-Unit expects its bus powered: no runtime suspend. */
	if (!dev->shared_with_punit)
		i2c_dw_rpm_enable(dev, DW_IC_DEFAULT_AUTOSUSPEND_MS);
	return 0;
err:
	if (irq > 0)
		irq_free_desc(irq);
	if (dev->lpss_priv && cfg->kind == DW_ACPI_LPSS_NEW)
		lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
	acpi_dev_put(dev->acpi_dev);
	kfree(dev);
	kmemset(ctrl, 0, sizeof(*ctrl));
	return ret;
}

int i2c_dw_platform_init(void)
{
	struct acpi_device *found[DW_ACPI_MAX_CONTROLLERS];
	int n, ok = 0;

	might_sleep();
	n = acpi_dev_collect_matches(dw_i2c_acpi_match, 1, found,
				     DW_ACPI_MAX_CONTROLLERS);
	if (n <= 0)
		return n;
	for (int i = 0; i < n; i++) {
		const struct acpi_device_id *id =
			acpi_match_device_ids(found[i], dw_i2c_acpi_match);

		if (id && id->driver_data < CFG_COUNT &&
		    !dw_acpi_probe_one(found[i],
				       &dw_acpi_cfgs[id->driver_data]))
			ok++;
		acpi_dev_put(found[i]);
	}
	return ok;
}

void i2c_dw_platform_exit(void)
{
	might_sleep();
	for (int i = g_nctrls - 1; i >= 0; i--) {
		struct dw_acpi_ctrl *ctrl = &g_ctrls[i];
		struct dw_i2c_dev *dev = ctrl->dev;

		if (!dev)
			continue;
		i2c_dw_remove(dev);
		if (dev->irq > 0)
			irq_free_desc(dev->irq);
		if (dev->lpss_priv && dev->glue_suspend == i2c_dw_lpss_suspend &&
		    !dev->rpm_suspended)
			lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
		acpi_dev_put(dev->acpi_dev);
		kfree(dev);
		kmemset(ctrl, 0, sizeof(*ctrl));
	}
	g_nctrls = 0;
}

void i2c_designware_init(void)
{
	int rc;

	rc = i2c_dw_pci_init();
	if (rc)
		kprintf(DRIVER_NAME ": PCI driver not registered (%d)\n", rc);
	rc = i2c_dw_platform_init();
	if (rc < 0 && rc != -ENODEV)
		kprintf(DRIVER_NAME ": ACPI controllers not probed (%d)\n",
			rc);
}
