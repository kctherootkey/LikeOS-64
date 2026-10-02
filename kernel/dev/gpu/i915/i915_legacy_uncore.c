// LikeOS -- registers of the pre-Broadwell parts: the BARs, forcewake,
// the GT FIFO, and the side channels to the power controllers.
//
// Up to Ironlake the GT never sleeps behind the driver's back and a
// register is just a register (Ironlake wants a dummy write first, to
// pull it out of render standby).  Sandy Bridge introduced forcewake:
// one domain, the whole GT below 0x40000, requested through FORCEWAKE
// and acknowledged in FORCEWAKE_ACK, with the GT's thread status to be
// polled out of C6 afterwards; Ivy Bridge has a multi-threaded variant
// the firmware may or may not have enabled, Haswell only that one, and
// Valleyview two domains (render and media) with a range table.  Writes
// on these parts take no forcewake at all: they go through a FIFO that
// holds them while the GT sleeps, and the driver waits for a free entry
// before each one.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

struct i915_legacy_state g_leg;

static spinlock_t g_fifo_lock;

/* ---- what kind of part ----------------------------------------------------- */

static int leg_is_mobile(struct i915_device *i915)
{
	switch (i915->info->platform) {
	case I915_PLATFORM_I830:
	case I915_PLATFORM_I85X:
	case I915_PLATFORM_I915GM:
	case I915_PLATFORM_I945GM:
	case I915_PLATFORM_I965GM:
	case I915_PLATFORM_GM45:
		return 1;
	case I915_PLATFORM_PINEVIEW:
		return i915->devid == 0xa011;
	case I915_PLATFORM_IRONLAKE:
		return i915->devid == 0x0046;
	case I915_PLATFORM_SANDYBRIDGE:
		return i915->devid == 0x0106 || i915->devid == 0x0116 || i915->devid == 0x0126;
	case I915_PLATFORM_IVYBRIDGE:
		return i915->devid == 0x0156 || i915->devid == 0x0166;
	default:
		return 0;
	}
}

static void leg_platform_init(struct i915_device *i915)
{
	int p = i915->info->platform;
	int gen = leg_gen(i915);

	g_leg.i915 = i915;
	g_leg.mobile = leg_is_mobile(i915);
	g_leg.is_g4x = p == I915_PLATFORM_G45 || p == I915_PLATFORM_GM45;
	g_leg.is_g33 = p == I915_PLATFORM_G33 || p == I915_PLATFORM_PINEVIEW;
	/* the parts whose engines may snoop the processor's caches */
	g_leg.has_snoop = gen <= 3 || g_leg.is_g4x || gen == 5 ||
			  p == I915_PLATFORM_VALLEYVIEW;
	/* the status page by physical address: everything before G33,
	 * and the two non-G4x gen4 parts */
	g_leg.hws_physical = (gen <= 3 && !g_leg.is_g33) || p == I915_PLATFORM_I965G ||
			     p == I915_PLATFORM_I965GM;
	if (gen <= 3 && !g_leg.is_g33)
		g_leg.dma_limit = 1ULL << 32;
	else if (gen <= 5)
		g_leg.dma_limit = 1ULL << 36;
	else if (p == I915_PLATFORM_HASWELL)
		g_leg.dma_limit = 1ULL << 39;
	else
		g_leg.dma_limit = 1ULL << 40;
	g_leg.gpu_reset_clobbers_display = gen <= 4 && !g_leg.is_g4x;
	g_leg.has_fifo = gen == 6 || gen == 7;
	g_leg.has_fpga_dbg = p == I915_PLATFORM_HASWELL;
}

/* ---- the BARs ---------------------------------------------------------------- */

int i915_legacy_mmio_map(struct i915_device *i915)
{
	const pci_device_t *pci = i915->pci;
	int gen = leg_gen(i915);
	int mmio_bar, gmadr_bar, io_bar;

	leg_platform_init(i915);
	switch (gen) {
	case 2:
		mmio_bar = GEN2_MMADR_BAR;
		gmadr_bar = GEN2_GMADR_BAR;
		io_bar = 2; /* 85x/865 */
		break;
	case 3:
		mmio_bar = GEN3_MMADR_BAR;
		gmadr_bar = GEN3_GMADR_BAR;
		io_bar = 1;
		break;
	default:
		mmio_bar = GEN4_GTTMMADR_BAR;
		gmadr_bar = GEN4_GMADR_BAR;
		io_bar = 4;
		break;
	}
	if (pci_bar_decode(pci, mmio_bar, &i915->bar_mmio) != 0 ||
	    (i915->bar_mmio.flags & PCI_BAR_IO)) {
		kprintf("[drm] i915: no register window (BAR%d)\n", mmio_bar);
		return -ENODEV;
	}
	if (pci_bar_decode(pci, gmadr_bar, &i915->bar_aperture) != 0 ||
	    (i915->bar_aperture.flags & PCI_BAR_IO))
		i915->bar_aperture.size = 0;
	if (pci_bar_decode(pci, io_bar, &i915->bar_io) != 0 || !(i915->bar_io.flags & PCI_BAR_IO))
		i915->bar_io.size = 0;

	/* Before gen4 the registers and the GTT sit behind different BARs;
	 * from gen4 on they share one, and only its register half is
	 * mapped here.  The register range is the same size on every part
	 * up to Ironlake, which doubled it. */
	i915->mmio_size = gen >= 5 ? (2u << 20) : (512u << 10);
	if (i915->bar_mmio.size < i915->mmio_size) {
		kprintf("[drm] i915: register window too small (%llu KB)\n",
			(unsigned long long)(i915->bar_mmio.size >> 10));
		return -ENODEV;
	}
	pci_enable_busmaster_mem(pci);
	i915->mmio_virt = mm_map_mmio_flags(i915->bar_mmio.base, i915->mmio_size / 4096,
					    MM_MMIO_UC);
	if (!i915->mmio_virt) {
		kprintf("[drm] i915: cannot map the registers\n");
		return -ENOMEM;
	}
	i915_dbg("[drm] i915: registers at %llx (%llu KB), aperture at %llx (%llu MB)\n",
		 (unsigned long long)i915->bar_mmio.base,
		 (unsigned long long)(i915->mmio_size >> 10),
		 (unsigned long long)i915->bar_aperture.base,
		 (unsigned long long)(i915->bar_aperture.size >> 20));
	return 0;
}

/* ---- the GT FIFO and the unclaimed-access detectors ------------------------- */

static void gen6_check_for_fifo_debug(struct i915_device *i915)
{
	uint32_t fifodbg = i915_read32_fw(i915, GTFIFODBG);

	if (fifodbg) {
		i915_dbg("[drm] i915: GTFIFODBG = %08x\n", fifodbg);
		i915_write32_fw(i915, GTFIFODBG, fifodbg);
	}
}

static uint32_t fifo_free_entries(struct i915_device *i915)
{
	return i915_read32_fw(i915, GTFIFOCTL) & GT_FIFO_FREE_ENTRIES_MASK;
}

/* A write needs a free entry in the FIFO in front of the sleeping GT; a
 * few are kept for the hardware's own use.  Valleyview shares the FIFO
 * with the hardware, so the count is read every time there. */
static void gen6_gt_wait_for_fifo(struct i915_device *i915)
{
	uint32_t n;

	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		n = fifo_free_entries(i915);
	else
		n = g_leg.fifo_count;
	if (n <= GT_FIFO_NUM_RESERVED_ENTRIES) {
		int t;
		for (t = 0; t < 10000; t++) {
			n = fifo_free_entries(i915);
			if (n > GT_FIFO_NUM_RESERVED_ENTRIES)
				break;
			lapic_delay_us(1);
		}
		if (t == 10000) {
			i915_dbg("[drm] i915: GT FIFO timeout, entries: %u\n", n);
			return;
		}
	}
	g_leg.fifo_count = n - 1;
}

void i915_legacy_mmio_read_prepare(struct i915_device *i915, uint32_t reg)
{
	(void)reg;
	/* WaIssueDummyWriteToWakeupFromRC6:ilk -- MI_MODE is masked, so a
	 * zero written to it changes nothing */
	if (leg_gen(i915) == 5)
		i915_write32_fw(i915, RING_MI_MODE(RENDER_RING_BASE), 0);
}

void i915_legacy_mmio_write_prepare(struct i915_device *i915, uint32_t reg)
{
	uint64_t fl;

	if (leg_gen(i915) == 5) {
		i915_write32_fw(i915, RING_MI_MODE(RENDER_RING_BASE), 0);
		return;
	}
	if (!g_leg.has_fifo || !(reg < 0x40000 || reg >= 0x116000))
		return;
	spin_lock_irqsave(&g_fifo_lock, &fl);
	gen6_gt_wait_for_fifo(i915);
	spin_unlock_irqrestore(&g_fifo_lock, fl);
}

void i915_legacy_check_unclaimed(struct i915_device *i915, uint32_t reg)
{
	if (i915->unclaimed_warned)
		return;
	if (g_leg.has_fpga_dbg) {
		uint32_t dbg = i915_read32_fw(i915, FPGA_DBG);
		if (!(dbg & FPGA_DBG_RM_NOCLAIM))
			return;
		if (dbg == ~0u)
			kprintf("[drm] i915: lost access to the register window; every register reads all ones\n");
		i915_write32_fw(i915, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
	} else if (leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		uint32_t cer = i915_read32_fw(i915, CLAIM_ER);
		if (!(cer & (CLAIM_ER_OVERFLOW | CLAIM_ER_CTR_MASK)))
			return;
		i915_write32_fw(i915, CLAIM_ER, CLAIM_ER_CLR);
	} else {
		return;
	}
	i915->unclaimed_warned = 1;
	kprintf("[drm] i915: unclaimed register access (last %05x)\n", reg);
}

/* ---- forcewake ----------------------------------------------------------------- */

#define FW_ACK_TIMEOUT_US 50000

static int fw_wait_ack(struct i915_device *i915, struct i915_fw_domain *d, uint32_t want)
{
	for (uint32_t t = 0; t < FW_ACK_TIMEOUT_US; t += 2) {
		if ((i915_read32_fw(i915, d->reg_ack) & FORCEWAKE_KERNEL) == want)
			return 0;
		lapic_delay_us(2);
	}
	return -ETIMEDOUT;
}

/* WaRsForcewakeWaitTC0:snb,ivb,hsw -- a read straight after the wake
 * can return zero until the GT thread has left C6. */
static void gen6_gt_wait_for_thread_c0(struct i915_device *i915)
{
	for (int t = 0; t < 5000; t++) {
		if (!(i915_read32_fw(i915, GEN6_GT_THREAD_STATUS_REG) &
		      GEN6_GT_THREAD_STATUS_CORE_MASK))
			return;
		lapic_delay_us(1);
	}
	i915_dbg("[drm] i915: GT thread status wait timed out\n");
}

void i915_legacy_fw_wake(struct i915_device *i915, struct i915_fw_domain *d)
{
	if (g_leg.has_fifo)
		gen6_check_for_fifo_debug(i915);
	if (fw_wait_ack(i915, d, 0) != 0) {
		d->timeouts++;
		if (i915_read32_fw(i915, d->reg_ack) == ~0u)
			kprintf("[drm] i915: forcewake: the register window reads all ones\n");
		else if (d->timeouts == 1)
			kprintf("[drm] i915: forcewake domain %x: the old acknowledge never cleared\n",
				d->mask);
	}
	i915_write32_fw(i915, d->reg_set, LEG_MASKED_EN(FORCEWAKE_KERNEL));
	if (fw_wait_ack(i915, d, FORCEWAKE_KERNEL) != 0) {
		d->timeouts++;
		if (d->timeouts == 1)
			kprintf("[drm] i915: forcewake domain %x: no wake acknowledge (ack=%08x)\n",
				d->mask, i915_read32_fw(i915, d->reg_ack));
	}
	if (!leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		gen6_gt_wait_for_thread_c0(i915);
}

void i915_legacy_fw_sleep(struct i915_device *i915, struct i915_fw_domain *d)
{
	i915_write32_fw(i915, d->reg_set, LEG_MASKED_DIS(FORCEWAKE_KERNEL));
	(void)i915_read32_fw(i915, d->reg_ack);
}

/* Valleyview: which domain a register lives in. */
static const struct {
	uint32_t start, end, domains;
} vlv_fw_ranges[] = {
	{ 0x2000, 0x3fff, I915_FW_RENDER },
	{ 0x5000, 0x7fff, I915_FW_RENDER },
	{ 0xb000, 0x11fff, I915_FW_RENDER },
	{ 0x12000, 0x13fff, I915_FW_MEDIA },
	{ 0x22000, 0x23fff, I915_FW_MEDIA },
	{ 0x2e000, 0x2ffff, I915_FW_RENDER },
	{ 0x30000, 0x3ffff, I915_FW_MEDIA },
};

uint32_t i915_legacy_fw_domains_for(struct i915_device *i915, uint32_t reg, int write)
{
	if (leg_gen(i915) < 6 || write)
		return 0;
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		for (unsigned i = 0; i < sizeof(vlv_fw_ranges) / sizeof(vlv_fw_ranges[0]); i++)
			if (reg >= vlv_fw_ranges[i].start && reg <= vlv_fw_ranges[i].end)
				return vlv_fw_ranges[i].domains & i915->fw_present;
		return 0;
	}
	return reg < 0x40000 ? (I915_FW_RENDER & i915->fw_present) : 0;
}

static void fw_domain_setup(struct i915_device *i915, int idx, uint32_t mask, uint32_t set,
			    uint32_t ack)
{
	struct i915_fw_domain *d = &i915->fw[idx];

	d->mask = mask;
	d->reg_set = set;
	d->reg_ack = ack;
	d->count = 0;
	d->timeouts = 0;
	i915->fw_present |= mask;
	/* WaRsClearFWBitsAtReset: every request bit released */
	i915_write32_fw(i915, set, LEG_MASKED_DIS(0xffff));
}

int i915_legacy_uncore_init(struct i915_device *i915)
{
	int gen = leg_gen(i915);

	if (!g_leg.i915)
		leg_platform_init(i915);
	spinlock_init(&i915->fw_lock, "i915_fw");
	spinlock_init(&g_fifo_lock, "i915_fifo");
	spinlock_init(&g_leg.sb_lock, "i915_sb");
	i915->fw_present = 0;
	i915->fw_held = 0;
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		i915->fw[i].mask = 0;
	if (gen < 6)
		return 0;

	if (g_leg.has_fifo)
		gen6_check_for_fifo_debug(i915);
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		fw_domain_setup(i915, 0, I915_FW_RENDER, FORCEWAKE_VLV, FORCEWAKE_ACK_VLV);
		fw_domain_setup(i915, 2, I915_FW_MEDIA, FORCEWAKE_MEDIA_VLV,
				FORCEWAKE_ACK_MEDIA_VLV);
	} else if (leg_is(i915, I915_PLATFORM_HASWELL)) {
		fw_domain_setup(i915, 0, I915_FW_RENDER, FORCEWAKE_MT, FORCEWAKE_ACK_HSW);
	} else if (leg_is(i915, I915_PLATFORM_IVYBRIDGE)) {
		/* Whether the firmware enabled the multi-threaded register
		 * is in ECOBUS -- readable only with the GT awake, so wake
		 * it the multi-threaded way and look.  A GT in RC6 that
		 * the MT request cannot wake reads zero there, which is
		 * the right answer too. */
		uint32_t ecobus;
		i915_write32_fw(i915, FORCEWAKE, 0);
		(void)i915_read32_fw(i915, ECOBUS);
		fw_domain_setup(i915, 0, I915_FW_RENDER, FORCEWAKE_MT, FORCEWAKE_MT_ACK);
		i915_legacy_fw_wake(i915, &i915->fw[0]);
		ecobus = i915_read32_fw(i915, ECOBUS);
		i915_legacy_fw_sleep(i915, &i915->fw[0]);
		if (!(ecobus & FORCEWAKE_MT_ENABLE)) {
			kprintf("[drm] i915: no multi-threaded forcewake on this Ivy Bridge\n");
			i915->fw_present = 0;
			fw_domain_setup(i915, 0, I915_FW_RENDER, FORCEWAKE, FORCEWAKE_ACK);
		}
	} else {
		fw_domain_setup(i915, 0, I915_FW_RENDER, FORCEWAKE, FORCEWAKE_ACK);
	}
	if (g_leg.has_fifo)
		g_leg.fifo_count = fifo_free_entries(i915);

	/* the unclaimed-access flag the firmware may have left */
	i915_legacy_check_unclaimed(i915, 0);
	i915->unclaimed_warned = 0;

	/* Prove the mechanism on the render domain. */
	{
		struct i915_fw_domain *d = &i915->fw[0];
		uint64_t fl;
		int rc;

		spin_lock_irqsave(&i915->fw_lock, &fl);
		i915_legacy_fw_wake(i915, d);
		rc = (i915_read32_fw(i915, d->reg_ack) & FORCEWAKE_KERNEL) ? 0 : -ETIMEDOUT;
		i915_legacy_fw_sleep(i915, d);
		spin_unlock_irqrestore(&i915->fw_lock, fl);
		if (rc)
			return rc;
	}
	return 0;
}

void i915_legacy_uncore_fini(struct i915_device *i915)
{
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		if (i915->fw[i].mask)
			i915_write32_fw(i915, i915->fw[i].reg_set, LEG_MASKED_DIS(0xffff));
	i915->fw_present = 0;
	i915->fw_held = 0;
}

/* ---- the PCU mailbox (Gen6/7) ------------------------------------------------ */

static int gen6_check_mailbox_status(uint32_t mbox)
{
	switch (mbox & GEN6_PCODE_ERROR_MASK) {
	case GEN6_PCODE_SUCCESS:
		return 0;
	case GEN6_PCODE_UNIMPLEMENTED_CMD:
		return -ENODEV;
	case GEN6_PCODE_ILLEGAL_CMD:
		return -ENXIO;
	case GEN6_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE:
		return -ERANGE;
	case GEN6_PCODE_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return 0;
	}
}

static int gen7_check_mailbox_status(uint32_t mbox)
{
	switch (mbox & GEN6_PCODE_ERROR_MASK) {
	case GEN6_PCODE_SUCCESS:
		return 0;
	case GEN6_PCODE_ILLEGAL_CMD:
		return -ENXIO;
	case GEN7_PCODE_TIMEOUT:
		return -ETIMEDOUT;
	case GEN7_PCODE_ILLEGAL_DATA:
		return -EINVAL;
	case GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE:
		return -ERANGE;
	default:
		return 0;
	}
}

/* The mailbox is outside every forcewake domain. */
static int snb_pcode_rw(struct i915_device *i915, uint32_t mbox, uint32_t *val,
			uint32_t *val1, int is_read)
{
	uint64_t fl;
	int rc = -ETIMEDOUT;

	spin_lock_irqsave(&g_leg.sb_lock, &fl);
	if (i915_read32_fw(i915, GEN6_PCODE_MAILBOX) & GEN6_PCODE_READY) {
		spin_unlock_irqrestore(&g_leg.sb_lock, fl);
		return -EAGAIN;
	}
	i915_write32_fw(i915, GEN6_PCODE_DATA, *val);
	i915_write32_fw(i915, GEN6_PCODE_DATA1, val1 ? *val1 : 0);
	i915_write32_fw(i915, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | mbox);
	for (int t = 0; t < 20000; t++) {
		mbox = i915_read32_fw(i915, GEN6_PCODE_MAILBOX);
		if (!(mbox & GEN6_PCODE_READY)) {
			rc = 0;
			break;
		}
		lapic_delay_us(1);
	}
	if (rc == 0) {
		if (is_read)
			*val = i915_read32_fw(i915, GEN6_PCODE_DATA);
		if (is_read && val1)
			*val1 = i915_read32_fw(i915, GEN6_PCODE_DATA1);
		rc = leg_gen(i915) > 6 ? gen7_check_mailbox_status(mbox) :
					 gen6_check_mailbox_status(mbox);
	}
	spin_unlock_irqrestore(&g_leg.sb_lock, fl);
	return rc;
}

int leg_pcode_read(struct i915_device *i915, uint32_t mbox, uint32_t *val, uint32_t *val1)
{
	int rc = snb_pcode_rw(i915, mbox, val, val1, 1);
	if (rc)
		i915_dbg("[drm] i915: PCU mailbox read %x failed (%d)\n", mbox, rc);
	return rc;
}

int leg_pcode_write(struct i915_device *i915, uint32_t mbox, uint32_t val)
{
	int rc = snb_pcode_rw(i915, mbox, &val, NULL, 0);
	if (rc)
		i915_dbg("[drm] i915: PCU mailbox write %x failed (%d)\n", mbox, rc);
	return rc;
}

/* ---- the IOSF sideband (Valleyview, Cherryview) ---------------------------------- */

/* The doorbell, address and data registers are shared by every unit and
 * every user (the GT's power code, the display's clocks and PHYs): one
 * message at a time.  Initialised statically -- Cherryview's GT is not
 * this backend's, and nothing here runs its setup there. */
static spinlock_t g_iosf_lock = SPINLOCK_INIT("i915_iosf");

int i915_legacy_vlv_sideband(struct i915_device *i915, uint32_t devfn, uint32_t port,
			     uint32_t opcode, uint32_t addr, uint32_t *val)
{
	int is_read = (opcode == SB_MRD_NP || opcode == SB_CRRDDA_NP);
	uint64_t fl;
	int rc = -ETIMEDOUT;
	int t;

	spin_lock_irqsave(&g_iosf_lock, &fl);
	/* flush whatever a failed earlier transaction left */
	for (t = 0; t < 5000; t++) {
		if (!(i915_read32_fw(i915, VLV_IOSF_DOORBELL_REQ) & IOSF_SB_BUSY))
			break;
		lapic_delay_us(1);
	}
	if (t == 5000) {
		spin_unlock_irqrestore(&g_iosf_lock, fl);
		i915_dbg("[drm] i915: IOSF sideband idle wait timed out\n");
		return -EAGAIN;
	}
	i915_write32_fw(i915, VLV_IOSF_ADDR, addr);
	i915_write32_fw(i915, VLV_IOSF_DATA, is_read ? 0 : *val);
	i915_write32_fw(i915, VLV_IOSF_DOORBELL_REQ,
			(devfn << IOSF_DEVFN_SHIFT) | (opcode << IOSF_OPCODE_SHIFT) |
				(port << IOSF_PORT_SHIFT) | (0xf << IOSF_BYTE_ENABLES_SHIFT) |
				(0 << IOSF_BAR_SHIFT) | IOSF_SB_BUSY);
	for (t = 0; t < 10000; t++) {
		if (!(i915_read32_fw(i915, VLV_IOSF_DOORBELL_REQ) & IOSF_SB_BUSY)) {
			if (is_read)
				*val = i915_read32_fw(i915, VLV_IOSF_DATA);
			rc = 0;
			break;
		}
		lapic_delay_us(1);
	}
	spin_unlock_irqrestore(&g_iosf_lock, fl);
	if (rc)
		i915_dbg("[drm] i915: IOSF sideband finish wait timed out\n");
	return rc;
}

int i915_legacy_vlv_punit_read(struct i915_device *i915, uint32_t addr, uint32_t *val)
{
	return i915_legacy_vlv_sideband(i915, 0, IOSF_PORT_PUNIT, SB_CRRDDA_NP, addr, val);
}

int i915_legacy_vlv_punit_write(struct i915_device *i915, uint32_t addr, uint32_t val)
{
	return i915_legacy_vlv_sideband(i915, 0, IOSF_PORT_PUNIT, SB_CRWRDA_NP, addr, &val);
}

int i915_legacy_vlv_nc_read(struct i915_device *i915, uint32_t addr, uint32_t *val)
{
	return i915_legacy_vlv_sideband(i915, 0, IOSF_PORT_NC, SB_CRRDDA_NP, addr, val);
}

/* ---- clocks and topology ---------------------------------------------------------- */

/* The front-side bus strap (kHz), which the gen4 timestamp counts. */
static uint32_t i9xx_fsb_freq(struct i915_device *i915)
{
	uint32_t fsb = i915_read32(i915, CLKCFG) & CLKCFG_FSB_MASK;

	if (leg_is(i915, I915_PLATFORM_PINEVIEW) || g_leg.mobile) {
		switch (fsb) {
		case CLKCFG_FSB_400: return 400000;
		case CLKCFG_FSB_533: return 533333;
		case CLKCFG_FSB_667: return 666667;
		case CLKCFG_FSB_800: return 800000;
		case CLKCFG_FSB_1067: return 1066667;
		case CLKCFG_FSB_1333: return 1333333;
		default: return 1333333;
		}
	}
	switch (fsb) {
	case CLKCFG_FSB_400_ALT: return 400000;
	case CLKCFG_FSB_533: return 533333;
	case CLKCFG_FSB_667: return 666667;
	case CLKCFG_FSB_800: return 800000;
	case CLKCFG_FSB_1067_ALT: return 1066667;
	case CLKCFG_FSB_1333_ALT: return 1333333;
	case CLKCFG_FSB_1600_ALT: return 1600000;
	default: return 1333333;
	}
}

uint32_t i915_legacy_timestamp_hz(struct i915_device *i915)
{
	int gen = leg_gen(i915);

	/* Gen6/7: the PCU's 10 ns counter, bits 38:3 -- 80 ns a tick */
	if (gen >= 6)
		return 12500000;
	/* Ironlake: the upper dword counts microseconds */
	if (gen == 5)
		return 1000000;
	/* G4x: the upper dword counts 1024 ns */
	if (g_leg.is_g4x)
		return 1000000000u / 1024;
	/* gen4: once per four FSB clocks, whatever the documents say */
	if (gen == 4)
		return (i9xx_fsb_freq(i915) + 2) / 4 * 1000;
	return 0;
}

#define HSW_PAVP_FUSE1 0x911c
#define HSW_F1_EU_DIS_SHIFT 16
#define HSW_F1_EU_DIS_MASK (3u << 16)

void i915_legacy_read_topology(struct i915_device *i915)
{
	int gt = (i915->id && i915->id->gt) ? i915->id->gt : i915->info->gt;
	uint32_t ss_mask, eus;

	i915->slice_mask = 0;
	for (int s = 0; s < 4; s++)
		i915->subslice_mask[s] = 0;
	i915->eu_total = 0;
	if (!leg_is(i915, I915_PLATFORM_HASWELL))
		return;
	/* No register says how many slices and subslices a Haswell has;
	 * the SKU does. */
	switch (gt) {
	case 2:
		i915->slice_mask = 1;
		ss_mask = 3;
		break;
	case 3:
		i915->slice_mask = 3;
		ss_mask = 3;
		break;
	default:
		i915->slice_mask = 1;
		ss_mask = 1;
		break;
	}
	switch ((i915_read32(i915, HSW_PAVP_FUSE1) & HSW_F1_EU_DIS_MASK) >> HSW_F1_EU_DIS_SHIFT) {
	case 1:
		eus = 8;
		break;
	case 2:
		eus = 6;
		break;
	default:
		eus = 10;
		break;
	}
	for (int s = 0; s < 2; s++) {
		if (!(i915->slice_mask & (1u << s)))
			continue;
		i915->subslice_mask[s] = ss_mask;
		i915->eu_total += (ss_mask == 3 ? 2 : 1) * eus;
	}
	i915->eus_per_subslice = eus;
}
