// LikeOS - MSI (multi-message) and MSI-X (table mapped once, per-vector
// mask), INTx through the interrupt core, pci_alloc_irq_vectors_affinity
//
// pci_alloc_irq_vectors_affinity() tries MSI-X, then MSI, then INTx
// (restricted to what the caller allows) and turns every vector into an
// interrupt-core descriptor (kernel/ke/irq.c) whose chip operations here
// program the device:
//
//   MSI-X  one descriptor per table entry; the table is mapped once per
//          device and kept; every entry starts masked (all entries are
//          masked while the function is set up, with Function Mask set
//          meanwhile) and is unmasked when its handler is requested.
//          Writing an entry's message masks it first when it is live, and
//          the write is flushed by reading the data word back.
//   MSI    one message: a regular descriptor; MSI is enabled once the
//          message is first written.  Several messages: the device adds
//          the message number to the data word, so the vectors must be a
//          contiguous, aligned block on one CPU -- allocated at once
//          (irq_alloc_desc_block), the message written for message 0.
//          Per-vector mask bits when the capability has them; otherwise
//          the interrupt core replays what arrived while disabled.
//   INTx   one descriptor on the routed I/O APIC line (ACPI _PRT through
//          the bridge swizzle); masking is the INTx disable bit.
//
// Vector affinity: irq_create_affinity_masks() spreads the vectors
// between affd->pre_vectors and the post vectors over the CPUs; those are
// MANAGED (fixed for life, the vector reserved on every CPU of the mask).
// A multi-message MSI block goes to one CPU (x86 messages carry one
// destination), so its vectors report that CPU and are not managed.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/pci_internal.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/io.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Mask registers (MSI mask word, MSI-X vector control cache) are updated
 * read-modify-write from several contexts (the interrupt core under the
 * descriptor lock, drivers, restore): one leaf lock. */
static spinlock_t g_pci_msi_lock = SPINLOCK_INIT("pci_msi");

struct pci_msi_vec {
	pci_device_t *dev;
	unsigned int nr; /* index in dev->irq_vecs */
	unsigned int entry; /* MSI-X table entry / MSI message number */
	int vmd_index; /* behind a remapping VMD: its vector, else -1 */
};

static inline unsigned int msix_table_size(uint16_t ctrl)
{
	return (unsigned int)(ctrl & PCI_MSIX_FLAGS_QSIZE) + 1;
}

void pci_msi_init_dev(pci_device_t *dev)
{
	uint16_t ctrl = 0;

	dev->msix_table_size = 0;
	dev->msi_max_vecs = 0;
	if (dev->msix_cap) {
		pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
		if (!PCI_POSSIBLE_ERROR(ctrl))
			dev->msix_table_size = (uint16_t)msix_table_size(ctrl);
	}
	if (dev->msi_cap) {
		pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
		if (!PCI_POSSIBLE_ERROR(ctrl))
			dev->msi_max_vecs =
				(uint16_t)(1u << ((ctrl & PCI_MSI_FLAGS_QMASK) >>
						  1));
	}
}

int pci_msix_vec_count(const pci_device_t *dev)
{
	uint16_t ctrl = 0;

	if (!dev || !dev->msix_cap)
		return -EINVAL;
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	if (PCI_POSSIBLE_ERROR(ctrl))
		return -EINVAL;
	return (int)msix_table_size(ctrl);
}

int pci_msi_vec_count(const pci_device_t *dev)
{
	uint16_t ctrl = 0;

	if (!dev || !dev->msi_cap)
		return -EINVAL;
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	if (PCI_POSSIBLE_ERROR(ctrl))
		return -EINVAL;
	return 1 << ((ctrl & PCI_MSI_FLAGS_QMASK) >> 1);
}

/* ==========================================================================
 * MSI-X table
 * ========================================================================== */

volatile uint32_t *pci_msix_map_table(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint32_t table = 0, lo = 0, hi = 0;
	uint16_t ctrl = 0;
	uint64_t base, phys, page, virt;
	unsigned int bir, tsize;
	size_t pages;
	uint64_t fl;

	if (!pp || !dev->msix_cap)
		return NULL;
	if (pp->msix_map)
		return pp->msix_map;
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE, &table);
	if (PCI_POSSIBLE_ERROR(table) || PCI_POSSIBLE_ERROR(ctrl))
		return NULL;
	tsize = msix_table_size(ctrl);
	bir = table & PCI_MSIX_TABLE_BIR;
	if (bir > 5)
		return NULL;
	/* The BAR as it is programmed now (no sizing: the device may be
	 * live). */
	pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + (int)bir * 4, &lo);
	if (lo & PCI_BASE_ADDRESS_SPACE_IO)
		return NULL; /* the table must be in memory space */
	base = lo & ~0xFULL;
	if ((lo & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
		    PCI_BASE_ADDRESS_MEM_TYPE_64 &&
	    bir < 5) {
		pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + (int)bir * 4 + 4,
				      &hi);
		base |= (uint64_t)hi << 32;
	}
	if (!base)
		return NULL;
	if (dev->segment >= PCI_VMD_DOMAIN_BASE)
		base = pci_vmd_bus_to_cpu(dev->segment, base);
	phys = base + (table & PCI_MSIX_TABLE_OFFSET);
	page = phys & ~0xFFFULL;
	pages = (size_t)(((phys - page) + (uint64_t)tsize * PCI_MSIX_ENTRY_SIZE +
			  0xFFF) >> 12);
	virt = mm_map_device_mmio(page, pages);
	if (!virt)
		return NULL;
	spin_lock_irqsave(&g_pci_msi_lock, &fl);
	if (!pp->msix_map) {
		pp->msix_map_virt = virt;
		pp->msix_map_pages = pages;
		pp->msix_bir = (uint8_t)bir;
		pp->msix_table_phys_off = table & PCI_MSIX_TABLE_OFFSET;
		pp->msix_table_bytes = tsize * PCI_MSIX_ENTRY_SIZE;
		pp->msix_map = (volatile uint32_t *)(uintptr_t)(virt +
								(phys - page));
		dev->msix_table = pp->msix_map;
	}
	spin_unlock_irqrestore(&g_pci_msi_lock, fl);
	return pp->msix_map;
}

static inline volatile uint32_t *msix_entry(pci_device_t *dev,
					    unsigned int entry)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	BUG_ON(!pp || !pp->msix_map);
	BUG_ON(entry >= dev->msix_table_size);
	return pp->msix_map + (size_t)entry * (PCI_MSIX_ENTRY_SIZE / 4);
}

static void msix_clear_and_set_ctrl(pci_device_t *dev, uint16_t clear,
				    uint16_t set)
{
	uint16_t ctrl = 0;

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	ctrl &= ~clear;
	ctrl |= set;
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl);
}

/* Vector control of one entry from the cache (g_pci_msi_lock held). */
static void msix_write_vector_ctrl(pci_device_t *dev, unsigned int entry,
				   uint32_t ctrl)
{
	volatile uint32_t *e = msix_entry(dev, entry);

	lockdep_assert_held(&g_pci_msi_lock);
	writel(ctrl, &e[PCI_MSIX_ENTRY_VECTOR_CTRL / 4]);
}

static void msix_mask_entry(pci_device_t *dev, unsigned int entry, int mask)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	if (!pp || !pp->msix_ctrl_cache || pci_dev_is_disconnected(dev))
		return;
	spin_lock_irqsave(&g_pci_msi_lock, &fl);
	if (mask)
		pp->msix_ctrl_cache[entry] |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
	else
		pp->msix_ctrl_cache[entry] &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	msix_write_vector_ctrl(dev, entry, pp->msix_ctrl_cache[entry]);
	/* A mask must have reached the device before the caller relies on
	 * it: read back (flushes the posted write). */
	if (mask)
		(void)readl(&msix_entry(dev, entry)[PCI_MSIX_ENTRY_VECTOR_CTRL /
						    4]);
	spin_unlock_irqrestore(&g_pci_msi_lock, fl);
}

/* Write an entry's message.  A live (unmasked) entry is masked around
 * the write so the device never sends a half-updated message. */
static void msix_write_msg(pci_device_t *dev, unsigned int entry,
			   uint32_t addr_lo, uint32_t addr_hi, uint32_t data)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	volatile uint32_t *e;
	uint32_t ctrl;
	int unmasked;
	uint64_t fl;

	if (!pp || !pp->msix_ctrl_cache || pci_dev_is_disconnected(dev))
		return;
	e = msix_entry(dev, entry);
	spin_lock_irqsave(&g_pci_msi_lock, &fl);
	ctrl = pp->msix_ctrl_cache[entry];
	unmasked = !(ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);
	if (unmasked)
		msix_write_vector_ctrl(dev, entry,
				       ctrl | PCI_MSIX_ENTRY_CTRL_MASKBIT);
	writel(addr_lo, &e[PCI_MSIX_ENTRY_LOWER_ADDR / 4]);
	writel(addr_hi, &e[PCI_MSIX_ENTRY_UPPER_ADDR / 4]);
	writel(data, &e[PCI_MSIX_ENTRY_DATA / 4]);
	if (unmasked)
		msix_write_vector_ctrl(dev, entry, ctrl);
	/* Make the writes visible to the device before anything relies on
	 * them. */
	(void)readl(&e[PCI_MSIX_ENTRY_DATA / 4]);
	spin_unlock_irqrestore(&g_pci_msi_lock, fl);
}

static void msix_mask_all(pci_device_t *dev, unsigned int tsize)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	spin_lock_irqsave(&g_pci_msi_lock, &fl);
	for (unsigned int i = 0; i < tsize; i++) {
		if (pp->msix_ctrl_cache)
			pp->msix_ctrl_cache[i] |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
		msix_write_vector_ctrl(dev, i, PCI_MSIX_ENTRY_CTRL_MASKBIT |
						       (pp->msix_ctrl_cache ?
								pp->msix_ctrl_cache[i] :
								0));
	}
	spin_unlock_irqrestore(&g_pci_msi_lock, fl);
}

/* ---- chip operations ------------------------------------------------- */

static uint32_t msi_addr_lo(uint32_t apic_id)
{
	return MSI_ADDR_BASE | ((apic_id & 0xFF) << 12);
}

static void msix_compose(int irq, void *data, uint8_t vector, uint32_t apic_id)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	msix_write_msg(v->dev, v->entry, msi_addr_lo(apic_id), 0, vector);
}

static void msix_mask(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	msix_mask_entry(v->dev, v->entry, 1);
}

static void msix_unmask(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	msix_mask_entry(v->dev, v->entry, 0);
}

static const struct irq_chip_ops g_msix_chip = {
	.compose_msg = msix_compose,
	.mask = msix_mask,
	.unmask = msix_unmask,
};

static void msi_set_enable(pci_device_t *dev, int enable)
{
	uint16_t ctrl = 0;

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	if (PCI_POSSIBLE_ERROR(ctrl))
		return;
	ctrl &= ~PCI_MSI_FLAGS_ENABLE;
	if (enable)
		ctrl |= PCI_MSI_FLAGS_ENABLE;
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, ctrl);
}

static int msi_mask_reg(const pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	return dev->msi_cap +
	       (pp->msi_64bit ? PCI_MSI_MASK_64 : PCI_MSI_MASK_32);
}

/* Update the MSI per-vector mask bits (when the capability has them). */
static void msi_update_mask(pci_device_t *dev, uint32_t clear, uint32_t set)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	if (!pp || !pp->msi_maskbit || pci_dev_is_disconnected(dev))
		return;
	spin_lock_irqsave(&g_pci_msi_lock, &fl);
	pp->msi_mask_cache &= ~clear;
	pp->msi_mask_cache |= set;
	pci_write_config_dword(dev, msi_mask_reg(dev), pp->msi_mask_cache);
	spin_unlock_irqrestore(&g_pci_msi_lock, fl);
}

static void msi_write_msg(pci_device_t *dev, uint32_t addr_lo, uint32_t data)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint16_t ctrl = 0;
	int pos = dev->msi_cap;

	if (pci_dev_is_disconnected(dev))
		return;
	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &ctrl);
	ctrl &= ~PCI_MSI_FLAGS_QSIZE;
	ctrl |= (uint16_t)((pp->msi_multiple << 4) & PCI_MSI_FLAGS_QSIZE);
	pci_write_config_word(dev, pos + PCI_MSI_FLAGS, ctrl);
	pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_LO, addr_lo);
	if (pp->msi_64bit) {
		pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_HI, 0);
		pci_write_config_word(dev, pos + PCI_MSI_DATA_64,
				      (uint16_t)data);
	} else {
		pci_write_config_word(dev, pos + PCI_MSI_DATA_32,
				      (uint16_t)data);
	}
	/* MSI is switched on with the first valid message: before it the
	 * message registers held nothing the CPU could take. */
	if (pp->msi_enabled && !(ctrl & PCI_MSI_FLAGS_ENABLE))
		msi_set_enable(dev, 1);
}

static void msi_compose(int irq, void *data, uint8_t vector, uint32_t apic_id)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	/* One message register for the whole block: message 0 carries the
	 * base vector, the device adds its message number. */
	if (v->entry != 0)
		return;
	msi_write_msg(v->dev, msi_addr_lo(apic_id), vector);
}

static void msi_mask(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	msi_update_mask(v->dev, 0, 1u << v->entry);
}

static void msi_unmask(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	msi_update_mask(v->dev, 1u << v->entry, 0);
}

static const struct irq_chip_ops g_msi_chip_maskable = {
	.compose_msg = msi_compose,
	.mask = msi_mask,
	.unmask = msi_unmask,
};

/* Without per-vector mask bits there is nothing to mask: the interrupt
 * core keeps what arrives while disabled pending and replays it. */
static const struct irq_chip_ops g_msi_chip_nomask = {
	.compose_msg = msi_compose,
	.mask = NULL,
	.unmask = NULL,
};

static void intx_mask(int irq, void *data)
{
	(void)irq;
	pci_intx((pci_device_t *)data, 0);
}

static void intx_unmask(int irq, void *data)
{
	(void)irq;
	pci_intx((pci_device_t *)data, 1);
}

static const struct irq_chip_ops g_intx_chip = {
	.compose_msg = NULL,
	.mask = intx_mask,
	.unmask = intx_unmask,
};

/* Behind a VMD that remaps messages: the message names the VMD vector
 * (its destination ID field), the data is unused. */
static uint32_t vmd_msg_addr(const struct pci_msi_vec *v)
{
	return MSI_ADDR_BASE | (((uint32_t)v->vmd_index & 0xFF) << 12);
}

static void msix_vmd_compose(int irq, void *data, uint8_t vector,
			     uint32_t apic_id)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	(void)vector;
	(void)apic_id;
	msix_write_msg(v->dev, v->entry, vmd_msg_addr(v), 0, 0);
}

static void msi_vmd_compose(int irq, void *data, uint8_t vector,
			    uint32_t apic_id)
{
	struct pci_msi_vec *v = data;

	(void)irq;
	(void)vector;
	(void)apic_id;
	if (v->entry == 0)
		msi_write_msg(v->dev, vmd_msg_addr(v), 0);
}

static void vmd_child_startup(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	pci_vmd_irq_enable(v->dev, v->vmd_index, irq);
}

static void vmd_child_shutdown(int irq, void *data)
{
	struct pci_msi_vec *v = data;

	pci_vmd_irq_disable(v->dev, v->vmd_index, irq);
}

static const struct irq_chip_ops g_msix_vmd_chip = {
	.compose_msg = msix_vmd_compose,
	.mask = msix_mask,
	.unmask = msix_unmask,
	.startup = vmd_child_startup,
	.shutdown = vmd_child_shutdown,
};

static const struct irq_chip_ops g_msi_vmd_chip_maskable = {
	.compose_msg = msi_vmd_compose,
	.mask = msi_mask,
	.unmask = msi_unmask,
	.startup = vmd_child_startup,
	.shutdown = vmd_child_shutdown,
};

static const struct irq_chip_ops g_msi_vmd_chip_nomask = {
	.compose_msg = msi_vmd_compose,
	.mask = NULL,
	.unmask = NULL,
	.startup = vmd_child_startup,
	.shutdown = vmd_child_shutdown,
};

/* A descriptor for vector `vd' of `dev': through the VMD when the domain
 * remaps messages, else a per-CPU vector of its own. */
static int pci_msi_alloc_one(pci_device_t *dev, const struct irq_chip_ops *chip,
			     const struct irq_chip_ops *vmd_chip,
			     struct pci_msi_vec *vd, const cpumask_t *aff,
			     int managed)
{
	int irq;

	vd->vmd_index = -1;
	if (pci_vmd_msi_remapped(dev)) {
		int idx = pci_vmd_irq_assign(dev);

		if (idx < 0)
			return idx;
		vd->vmd_index = idx;
		irq = irq_alloc_desc_virtual(vmd_chip, vd, aff, managed);
		if (irq < 0) {
			pci_vmd_irq_unassign(dev, idx);
			vd->vmd_index = -1;
		}
		return irq;
	}
	return irq_alloc_desc(chip, vd, aff, managed);
}

/* ==========================================================================
 * Allocation
 * ========================================================================== */

/* Free the bookkeeping of a failed or finished allocation. */
static void pci_irq_vecs_release(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	struct pci_msi_vec *vd = pp ? pp->msi_vecdata : NULL;

	if (dev->irq_vecs) {
		for (unsigned int i = 0; i < dev->nr_irq_vecs; i++) {
			if (dev->irq_vecs[i].irq > 0)
				irq_free_desc(dev->irq_vecs[i].irq);
			if (vd && dev->irq_type != PCI_IRQ_INTX &&
			    vd[i].vmd_index >= 0) {
				pci_vmd_irq_unassign(dev, vd[i].vmd_index);
				vd[i].vmd_index = -1;
			}
		}
		kfree(dev->irq_vecs);
	}
	dev->irq_vecs = NULL;
	dev->nr_irq_vecs = 0;
	dev->irq_type = 0;
	if (pp) {
		kfree(pp->msi_vecdata);
		pp->msi_vecdata = NULL;
	}
}

static int msix_capability_init(pci_device_t *dev, unsigned int nvec,
				struct irq_affinity *affd)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	struct irq_affinity_desc *masks = NULL;
	struct pci_msi_vec *vd = NULL;
	unsigned int tsize;
	uint16_t ctrl = 0;
	int rc = 0;

	/* Some devices only let the table be written with MSI-X enabled;
	 * keep every vector masked (Function Mask) meanwhile. */
	msix_clear_and_set_ctrl(dev, 0,
				PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE);
	pp->msix_enabled = 1;
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	tsize = msix_table_size(ctrl);
	if (!pci_msix_map_table(dev)) {
		rc = -ENOMEM;
		goto out_disable;
	}
	if (!pp->msix_ctrl_cache) {
		pp->msix_ctrl_cache = kcalloc(tsize, sizeof(uint32_t));
		if (!pp->msix_ctrl_cache) {
			rc = -ENOMEM;
			goto out_disable;
		}
	}
	for (unsigned int i = 0; i < tsize; i++)
		pp->msix_ctrl_cache[i] =
			readl(&msix_entry(dev, i)[PCI_MSIX_ENTRY_VECTOR_CTRL /
						 4]) |
			PCI_MSIX_ENTRY_CTRL_MASKBIT;

	if (affd)
		masks = irq_create_affinity_masks(nvec, affd);
	dev->irq_vecs = kcalloc(nvec, sizeof(*dev->irq_vecs));
	vd = kcalloc(nvec, sizeof(*vd));
	if (!dev->irq_vecs || !vd) {
		kfree(vd);
		vd = NULL;
		rc = -ENOMEM;
		goto out_free;
	}
	pp->msi_vecdata = vd;
	dev->nr_irq_vecs = (uint16_t)nvec;
	for (unsigned int i = 0; i < nvec; i++) {
		dev->irq_vecs[i].irq = -1;
		vd[i].vmd_index = -1;
	}
	for (unsigned int i = 0; i < nvec; i++) {
		const cpumask_t *aff = masks ? &masks[i].mask : NULL;
		int managed = masks ? masks[i].is_managed : 0;
		const cpumask_t *eff;
		int irq;

		vd[i].dev = dev;
		vd[i].nr = i;
		vd[i].entry = i;
		irq = pci_msi_alloc_one(dev, &g_msix_chip, &g_msix_vmd_chip,
					&vd[i], aff, managed);
		if (irq < 0) {
			rc = irq;
			goto out_free;
		}
		dev->irq_vecs[i].irq = irq;
		dev->irq_vecs[i].msix_entry = (uint16_t)i;
		dev->irq_vecs[i].managed = (uint16_t)managed;
		eff = irq_get_affinity_mask(irq);
		if (eff)
			cpumask_copy(&dev->irq_vecs[i].affinity, eff);
	}
	kfree(masks);
	masks = NULL;

	/* INTx off; every entry masked (done late: some controllers honour
	 * the per-entry masks even with MSI-X off); Function Mask off. */
	pci_intx(dev, 0);
	msix_mask_all(dev, tsize);
	msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);
	dev->irq_type = PCI_IRQ_MSIX;
	return 0;

out_free:
	kfree(masks);
	pci_irq_vecs_release(dev);
out_disable:
	pp->msix_enabled = 0;
	msix_clear_and_set_ctrl(dev,
				PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE, 0);
	return rc;
}

static int msi_capability_init(pci_device_t *dev, unsigned int nvec,
			       struct irq_affinity *affd)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	struct irq_affinity_desc *masks = NULL;
	struct pci_msi_vec *vd;
	const struct irq_chip_ops *chip;
	unsigned int hw = 1, log2 = 0;
	uint16_t ctrl = 0;
	int rc = 0;

	msi_set_enable(dev, 0);
	/* Behind a remapping VMD one message is all there is (the block
	 * would need consecutive vectors of one CPU). */
	if (pci_vmd_msi_remapped(dev))
		nvec = 1;
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	pp->msi_64bit = (ctrl & PCI_MSI_FLAGS_64BIT) ? 1 : 0;
	pp->msi_maskbit = (ctrl & PCI_MSI_FLAGS_MASKBIT) ? 1 : 0;
	while (hw < nvec) {
		hw <<= 1;
		log2++;
	}
	pp->msi_multiple = (uint8_t)log2;
	pp->msi_hw_vecs = hw;
	chip = pp->msi_maskbit ? &g_msi_chip_maskable : &g_msi_chip_nomask;

	/* All messages masked until their handlers come. */
	if (pp->msi_maskbit) {
		pp->msi_mask_cache = 0;
		msi_update_mask(dev, 0, hw >= 32 ? ~0u : ((1u << hw) - 1));
	}
	if (affd)
		masks = irq_create_affinity_masks(nvec, affd);

	dev->irq_vecs = kcalloc(hw, sizeof(*dev->irq_vecs));
	vd = kcalloc(hw, sizeof(*vd));
	if (!dev->irq_vecs || !vd) {
		kfree(vd);
		rc = -ENOMEM;
		goto out;
	}
	pp->msi_vecdata = vd;
	for (unsigned int i = 0; i < hw; i++) {
		vd[i].dev = dev;
		vd[i].nr = i;
		vd[i].entry = i;
		vd[i].vmd_index = -1;
		dev->irq_vecs[i].irq = -1;
	}
	dev->nr_irq_vecs = (uint16_t)hw;
	pp->msi_enabled = 1;
	if (hw == 1) {
		const cpumask_t *eff;
		int irq = pci_msi_alloc_one(
			dev, chip,
			pp->msi_maskbit ? &g_msi_vmd_chip_maskable :
					  &g_msi_vmd_chip_nomask,
			&vd[0], masks ? &masks[0].mask : NULL,
			masks ? masks[0].is_managed : 0);

		if (irq < 0) {
			rc = irq;
			goto out;
		}
		dev->irq_vecs[0].irq = irq;
		dev->irq_vecs[0].managed =
			(uint16_t)(masks ? masks[0].is_managed : 0);
		eff = irq_get_affinity_mask(irq);
		if (eff)
			cpumask_copy(&dev->irq_vecs[0].affinity, eff);
	} else {
		void *datas[32];
		int irqs[32];
		unsigned int cpu = 0;

		for (unsigned int i = 0; i < hw; i++)
			datas[i] = &vd[i];
		rc = irq_alloc_desc_block(chip, datas, hw,
					  masks ? &masks[affd ? affd->pre_vectors :
								0]
							   .mask :
						  NULL,
					  irqs);
		if (rc)
			goto out;
		for (unsigned int i = 0; i < hw; i++) {
			dev->irq_vecs[i].irq = irqs[i];
			dev->irq_vecs[i].managed = 0;
		}
		if (irq_desc_target(irqs[0], &cpu, NULL, NULL) == 0) {
			for (unsigned int i = 0; i < hw; i++) {
				cpumask_clear(&dev->irq_vecs[i].affinity);
				cpumask_set_cpu(cpu,
						&dev->irq_vecs[i].affinity);
			}
		}
		/* The message is written (block vectors are fixed): enable
		 * now; every message stays masked or unclaimed until its
		 * handler is requested. */
		msi_set_enable(dev, 1);
	}
	kfree(masks);
	/* The driver sees the count it asked for; the rest of the block
	 * (rounded up to a power of two for the device) stays unused. */
	dev->nr_irq_vecs = (uint16_t)nvec;
	pci_intx(dev, 0);
	dev->irq_type = PCI_IRQ_MSI;
	return 0;
out:
	kfree(masks);
	dev->nr_irq_vecs = (uint16_t)hw;
	pci_irq_vecs_release(dev);
	pp->msi_enabled = 0;
	if (pp->msi_maskbit)
		msi_update_mask(dev, ~0u, 0);
	return rc;
}

static int pci_enable_msix_range(pci_device_t *dev, unsigned int minvec,
				 unsigned int maxvec, struct irq_affinity *affd)
{
	int hwsize;
	unsigned int nvec = maxvec;

	if (!dev->msix_cap)
		return -EINVAL;
	if (pci_priv(dev)->msi_enabled)
		return -EINVAL;
	if (dev->current_state != PCI_D0 && dev->current_state != PCI_UNKNOWN)
		return -EINVAL;
	hwsize = pci_msix_vec_count(dev);
	if (hwsize < 0)
		return hwsize;
	if ((unsigned int)hwsize < nvec)
		nvec = (unsigned int)hwsize;
	if (nvec < minvec)
		return -ENOSPC;
	if (affd) {
		nvec = irq_calc_affinity_vectors(minvec, nvec, affd);
		if (nvec < minvec)
			return -ENOSPC;
	}
	{
		int rc = msix_capability_init(dev, nvec, affd);

		if (rc)
			return rc;
	}
	return (int)nvec;
}

static int pci_enable_msi_range(pci_device_t *dev, unsigned int minvec,
				unsigned int maxvec, struct irq_affinity *affd)
{
	int nvec;

	if (!dev->msi_cap)
		return -EINVAL;
	if (pci_priv(dev)->msix_enabled)
		return -EINVAL;
	if (dev->current_state != PCI_D0 && dev->current_state != PCI_UNKNOWN)
		return -EINVAL;
	nvec = pci_msi_vec_count(dev);
	if (nvec < 0)
		return nvec;
	if ((unsigned int)nvec < minvec)
		return -ENOSPC;
	if ((unsigned int)nvec > maxvec)
		nvec = (int)maxvec;
	/* Behind a remapping VMD: one message (see msi_capability_init). */
	if (pci_vmd_msi_remapped(dev) && nvec > 1) {
		if (minvec > 1)
			return -ENOSPC;
		nvec = 1;
	}
	if (affd) {
		nvec = (int)irq_calc_affinity_vectors(minvec, (unsigned int)nvec,
						      affd);
		if ((unsigned int)nvec < minvec)
			return -ENOSPC;
	}
	{
		int rc = msi_capability_init(dev, (unsigned int)nvec, affd);

		if (rc)
			return rc;
	}
	return nvec;
}

static int pci_enable_intx_vector(pci_device_t *dev, struct irq_affinity *affd)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	int gsi, active_low = 1, level = 1, irq;
	const cpumask_t *eff;

	gsi = pci_intx_gsi(dev, &active_low, &level);
	if (gsi < 0)
		return -ENOSPC;
	/* The driver adjusts its queue sets for the single vector. */
	if (affd)
		kfree(irq_create_affinity_masks(1, affd));
	dev->irq_vecs = kcalloc(1, sizeof(*dev->irq_vecs));
	if (!dev->irq_vecs)
		return -ENOMEM;
	irq = irq_alloc_desc_gsi(&g_intx_chip, dev, gsi, active_low, level);
	if (irq < 0) {
		kfree(dev->irq_vecs);
		dev->irq_vecs = NULL;
		return irq;
	}
	pp->intx_irq = irq;
	pp->intx_gsi = gsi;
	dev->irq_vecs[0].irq = irq;
	eff = irq_get_affinity_mask(irq);
	if (eff)
		cpumask_copy(&dev->irq_vecs[0].affinity, eff);
	dev->nr_irq_vecs = 1;
	dev->irq_type = PCI_IRQ_INTX;
	/* INTx stays disabled at the device until a handler is requested
	 * (the line may be shared with a driver whose handler would not
	 * claim our level interrupt). */
	pci_intx(dev, 0);
	return 1;
}

int pci_alloc_irq_vectors_affinity(pci_device_t *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd)
{
	struct irq_affinity msi_default_affd;
	int nvecs = -ENOSPC;

	might_sleep();
	if (!dev || !pci_priv(dev) || min_vecs == 0 || max_vecs < min_vecs)
		return -EINVAL;
	if (WARN(dev->irq_type != 0, "pci: interrupt vectors allocated twice"))
		return -EINVAL;
	if (pci_dev_is_disconnected(dev))
		return -ENODEV;
	if (flags & PCI_IRQ_AFFINITY) {
		if (!affd) {
			__builtin_memset(&msi_default_affd, 0,
					 sizeof(msi_default_affd));
			affd = &msi_default_affd;
		}
	} else {
		if (WARN_ON(affd))
			affd = NULL;
	}
	if (flags & PCI_IRQ_MSIX) {
		nvecs = pci_enable_msix_range(dev, min_vecs, max_vecs, affd);
		if (nvecs > 0)
			return nvecs;
	}
	if (flags & PCI_IRQ_MSI) {
		nvecs = pci_enable_msi_range(dev, min_vecs, max_vecs, affd);
		if (nvecs > 0)
			return nvecs;
	}
	if ((flags & PCI_IRQ_INTX) && min_vecs == 1) {
		int rc = pci_enable_intx_vector(dev, affd);

		if (rc > 0)
			return rc;
		nvecs = rc;
	}
	return nvecs;
}

void pci_free_irq_vectors(pci_device_t *dev)
{
	struct pci_dev_priv *pp;

	might_sleep();
	if (!dev || !(pp = pci_priv(dev)))
		return;
	switch (dev->irq_type) {
	case PCI_IRQ_MSIX:
		if (!pci_dev_is_disconnected(dev)) {
			uint16_t ctrl = 0;

			pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
					     &ctrl);
			msix_mask_all(dev, msix_table_size(ctrl));
			msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_ENABLE, 0);
			pci_intx(dev, 1);
		}
		pp->msix_enabled = 0;
		break;
	case PCI_IRQ_MSI:
		if (!pci_dev_is_disconnected(dev)) {
			msi_set_enable(dev, 0);
			pci_intx(dev, 1);
			/* The device goes back with MSI unmasked, as it came. */
			if (pp->msi_maskbit)
				msi_update_mask(dev, ~0u, 0);
		}
		pp->msi_enabled = 0;
		/* Free the whole block, not only what the driver saw. */
		dev->nr_irq_vecs = (uint16_t)pp->msi_hw_vecs;
		break;
	case PCI_IRQ_INTX:
		pp->intx_irq = 0;
		pp->intx_gsi = -1;
		break;
	default:
		return;
	}
	pci_irq_vecs_release(dev);
}

int pci_irq_vector(const pci_device_t *dev, unsigned int nr)
{
	if (!dev || !dev->irq_type || !dev->irq_vecs || nr >= dev->nr_irq_vecs)
		return -EINVAL;
	return dev->irq_vecs[nr].irq > 0 ? dev->irq_vecs[nr].irq : -EINVAL;
}

const cpumask_t *pci_irq_get_affinity(const pci_device_t *dev,
				      unsigned int nr)
{
	if (!dev || !dev->irq_type || !dev->irq_vecs || nr >= dev->nr_irq_vecs)
		return NULL;
	return &dev->irq_vecs[nr].affinity;
}

void pci_msix_mask_vector(pci_device_t *dev, unsigned int nr)
{
	if (!dev || !dev->irq_vecs || nr >= dev->nr_irq_vecs)
		return;
	if (dev->irq_type == PCI_IRQ_MSIX)
		msix_mask_entry(dev, dev->irq_vecs[nr].msix_entry, 1);
	else if (dev->irq_type == PCI_IRQ_MSI)
		msi_update_mask(dev, 0, 1u << nr);
}

void pci_msix_unmask_vector(pci_device_t *dev, unsigned int nr)
{
	if (!dev || !dev->irq_vecs || nr >= dev->nr_irq_vecs)
		return;
	if (dev->irq_type == PCI_IRQ_MSIX)
		msix_mask_entry(dev, dev->irq_vecs[nr].msix_entry, 0);
	else if (dev->irq_type == PCI_IRQ_MSI)
		msi_update_mask(dev, 1u << nr, 0);
}

/* ==========================================================================
 * Save / restore (after a reset or D3cold the device forgot all of it)
 * ========================================================================== */

void pci_msi_save_state(pci_device_t *dev)
{
	/* Interrupt-core users: everything needed lives in the core's
	 * records (the messages in the interrupt descriptors, the masks in
	 * the caches); nothing to read back. */
	(void)dev;
}

void pci_msi_restore_state(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	if (!pp || pci_dev_is_disconnected(dev))
		return;
	if (dev->irq_type == PCI_IRQ_MSI && pp->msi_enabled) {
		uint16_t ctrl = 0;

		pci_intx(dev, 0);
		msi_set_enable(dev, 0);
		if (dev->irq_vecs && dev->irq_vecs[0].irq > 0)
			irq_desc_recompose(dev->irq_vecs[0].irq);
		if (pp->msi_maskbit) {
			spin_lock_irqsave(&g_pci_msi_lock, &fl);
			pci_write_config_dword(dev, msi_mask_reg(dev),
					       pp->msi_mask_cache);
			spin_unlock_irqrestore(&g_pci_msi_lock, fl);
		}
		pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
		ctrl &= ~PCI_MSI_FLAGS_QSIZE;
		ctrl |= (uint16_t)((pp->msi_multiple << 4) & PCI_MSI_FLAGS_QSIZE);
		/* Enabled only when a message was ever written (a started
		 * vector, or a block). */
		if (dev->irq_vecs && dev->irq_vecs[0].irq > 0 &&
		    irq_desc_target(dev->irq_vecs[0].irq, NULL, NULL, NULL) == 0)
			ctrl |= PCI_MSI_FLAGS_ENABLE;
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, ctrl);
	} else if (dev->irq_type == PCI_IRQ_MSIX && pp->msix_enabled &&
		   pp->msix_map) {
		pci_intx(dev, 0);
		msix_clear_and_set_ctrl(dev, 0,
					PCI_MSIX_FLAGS_ENABLE |
						PCI_MSIX_FLAGS_MASKALL);
		/* Entries the driver did not get stay masked. */
		{
			uint16_t ctrl = 0;
			unsigned int tsize;

			pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
					     &ctrl);
			tsize = msix_table_size(ctrl);
			spin_lock_irqsave(&g_pci_msi_lock, &fl);
			for (unsigned int i = 0; i < tsize; i++)
				msix_write_vector_ctrl(
					dev, i,
					pp->msix_ctrl_cache ?
						(pp->msix_ctrl_cache[i] |
						 PCI_MSIX_ENTRY_CTRL_MASKBIT) :
						PCI_MSIX_ENTRY_CTRL_MASKBIT);
			spin_unlock_irqrestore(&g_pci_msi_lock, fl);
		}
		for (unsigned int i = 0; i < dev->nr_irq_vecs; i++) {
			unsigned int e = dev->irq_vecs[i].msix_entry;

			if (dev->irq_vecs[i].irq > 0)
				irq_desc_recompose(dev->irq_vecs[i].irq);
			spin_lock_irqsave(&g_pci_msi_lock, &fl);
			if (pp->msix_ctrl_cache)
				msix_write_vector_ctrl(dev, e,
						       pp->msix_ctrl_cache[e]);
			spin_unlock_irqrestore(&g_pci_msi_lock, fl);
		}
		msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);
	} else if (dev->irq_type == PCI_IRQ_INTX) {
		/* INTx: the enable follows the descriptor's mask state. */
		if (pp->intx_irq > 0)
			pci_intx(dev, !irq_desc_masked(pp->intx_irq));
	}
}

void pci_msi_dev_removed(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	if (!pp)
		return;
	/* A driver that did not free its vectors on remove(): the
	 * descriptors must not outlive the record (WARN and free them). */
	if (WARN(dev->irq_type != 0, "pci %s: removed with interrupt vectors allocated",
		 pp->name))
		pci_free_irq_vectors(dev);
	kfree(pp->msix_ctrl_cache);
	pp->msix_ctrl_cache = NULL;
}
