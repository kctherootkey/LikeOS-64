// LikeOS - Intel Volume Management Device domains
//
// A VMD (Intel RST "VMD mode", common on laptops of the last generations)
// hides root ports and the NVMe drives behind them from the firmware's
// PCI hierarchy: they live in a PCI domain of their own whose
// configuration space is the VMD's CFGBAR (ECAM layout, starting at bus
// busn_start), whose memory windows are the VMD's MEMBAR1 (non-
// prefetchable) and MEMBAR2 (prefetchable, its first 8 KiB reserved), and
// whose MSI/MSI-X messages are -- unless the VMD can bypass that --
// captured by the VMD and raised as one of its own MSI-X vectors (the
// message's destination ID names the VMD vector).
//
// The driver binds the VMD functions through the PCI driver model and,
// per VMD:
//   * finds the domain's bus range (bus restriction: 0, 128 or 224) and,
//     for a VMD passed through to a guest, the host-physical addresses of
//     its MEMBARs (the shadow registers or the hypervisor's "SHDW"
//     vendor capability) -- BARs below hold host addresses, the CPU uses
//     guest ones;
//   * gives the domain a segment number >= 0x10000 and registers config
//     accessors for it (serialised: some VMDs hang on concurrent config
//     cycles);
//   * closes the bridge windows the firmware left, scans the domain
//     (assigning bus numbers where needed), resets the first root port's
//     link (the firmware did not), assigns BARs and bridge windows from
//     the MEMBARs, sets the platform's default LTR on client platforms,
//     sets MPS, and lets drivers probe;
//   * with MSI remapping: allocates its MSI-X vectors when the first
//     child interrupt is set up (process context, all CPUs up) and runs,
//     on each vector, the handlers of every child interrupt attached to
//     it; NVMe functions are spread over the vectors after the first.
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

#define VMD_CFGBAR 0
#define VMD_MEMBAR1 2
#define VMD_MEMBAR2 4

#define PCI_REG_VMCAP 0x40
#define BUS_RESTRICT_CAP(vmcap) ((vmcap) & 0x1)
#define PCI_REG_VMCONFIG 0x44
#define BUS_RESTRICT_CFG(vmcfg) (((vmcfg) >> 8) & 0x3)
#define VMCONFIG_MSI_REMAP 0x2
#define PCI_REG_VMLOCK 0x70
#define MB2_SHADOW_EN(vmlock) ((vmlock) & 0x2)

#define MB2_SHADOW_OFFSET 0x2000
#define MB2_SHADOW_SIZE 16

#define VMD_FEAT_HAS_MEMBAR_SHADOW (1u << 0)
#define VMD_FEAT_HAS_BUS_RESTRICTIONS (1u << 1)
#define VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP (1u << 2)
#define VMD_FEAT_OFFSET_FIRST_VECTOR (1u << 3)
#define VMD_FEAT_CAN_BYPASS_MSI_REMAP (1u << 4)
#define VMD_FEAT_BIOS_PM_QUIRK (1u << 5)

#define VMD_FEATS_CLIENT                                                 \
	(VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP | VMD_FEAT_HAS_BUS_RESTRICTIONS | \
	 VMD_FEAT_OFFSET_FIRST_VECTOR | VMD_FEAT_BIOS_PM_QUIRK)

#define VMD_BIOS_PM_QUIRK_LTR 0x1003 /* 3145728 ns */
#define VMD_MAX_DOMAINS 8
#define VMD_MAX_CHILD_IRQS 64 /* per VMD vector */

struct vmd_irq_list {
	spinlock_t lock; /* the attached children (leaf; held by the handler) */
	int irqs[VMD_MAX_CHILD_IRQS];
	int nirqs;
	unsigned int count; /* children assigned (load balance) */
	int virq; /* the VMD's own interrupt */
};

struct vmd_dev {
	pci_device_t *dev;
	spinlock_t cfg_lock;
	volatile uint8_t *cfgbar;
	uint64_t cfgbar_size;
	uint32_t features;
	uint32_t domain;
	int instance;
	uint8_t busn_start;
	unsigned int bus_count;
	uint8_t first_vec;
	int msi_remap;
	struct mutex irq_mutex;
	int irqs_ready; /* -1 failed */
	int msix_count;
	struct vmd_irq_list *irqs;
	uint64_t mb1_cpu, mb1_end, mb2_cpu, mb2_end; /* CPU addresses */
	uint64_t offset1, offset2; /* cpu - bus */
	char name[12];
};

static struct vmd_dev *g_vmd[VMD_MAX_DOMAINS];
static int g_vmd_count;

static struct vmd_dev *vmd_of_segment(uint32_t segment)
{
	for (int i = 0; i < g_vmd_count; i++)
		if (g_vmd[i] && g_vmd[i]->domain == segment)
			return g_vmd[i];
	return NULL;
}

int pci_vmd_owns_segment(uint32_t segment)
{
	return vmd_of_segment(segment) != NULL;
}

int pci_vmd_msi_remapped(const pci_device_t *dev)
{
	struct vmd_dev *vmd;

	if (!dev || dev->segment < PCI_VMD_DOMAIN_BASE)
		return 0;
	vmd = vmd_of_segment(dev->segment);
	return vmd && vmd->msi_remap;
}

uint64_t pci_vmd_bus_to_cpu(uint32_t segment, uint64_t bus_addr)
{
	struct vmd_dev *vmd = vmd_of_segment(segment);

	if (!vmd || !bus_addr)
		return bus_addr;
	if (bus_addr + vmd->offset1 >= vmd->mb1_cpu &&
	    bus_addr + vmd->offset1 <= vmd->mb1_end)
		return bus_addr + vmd->offset1;
	if (bus_addr + vmd->offset2 >= vmd->mb2_cpu &&
	    bus_addr + vmd->offset2 <= vmd->mb2_end)
		return bus_addr + vmd->offset2;
	return bus_addr;
}

/* ---- configuration space ---------------------------------------------- */

static volatile uint8_t *vmd_cfg_addr(struct vmd_dev *vmd, uint8_t bus,
				      uint8_t devfn, int reg, int len)
{
	uint64_t off;

	if (bus < vmd->busn_start)
		return NULL;
	off = pci_ecam_offset(vmd->busn_start, bus, devfn, reg);
	if (off + (uint64_t)len >= vmd->cfgbar_size)
		return NULL;
	return vmd->cfgbar + off;
}

static int vmd_pci_read(void *ctx, uint32_t segment, uint8_t bus,
			uint8_t devfn, int where, int size, uint32_t *val)
{
	struct vmd_dev *vmd = ctx;
	volatile uint8_t *addr;
	uint64_t fl;

	(void)segment;
	addr = vmd_cfg_addr(vmd, bus, devfn, where, size);
	if (!addr) {
		*val = PCI_ERROR_RESPONSE;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	/* Some VMDs deadlock the CPU on unserialised config cycles. */
	spin_lock_irqsave(&vmd->cfg_lock, &fl);
	switch (size) {
	case 1:
		*val = readb(addr);
		break;
	case 2:
		*val = readw(addr);
		break;
	default:
		*val = readl(addr);
		break;
	}
	spin_unlock_irqrestore(&vmd->cfg_lock, fl);
	return PCIBIOS_SUCCESSFUL;
}

static int vmd_pci_write(void *ctx, uint32_t segment, uint8_t bus,
			 uint8_t devfn, int where, int size, uint32_t val)
{
	struct vmd_dev *vmd = ctx;
	volatile uint8_t *addr;
	uint64_t fl;

	(void)segment;
	addr = vmd_cfg_addr(vmd, bus, devfn, where, size);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;
	spin_lock_irqsave(&vmd->cfg_lock, &fl);
	switch (size) {
	case 1:
		writeb((uint8_t)val, addr);
		(void)readb(addr);
		break;
	case 2:
		writew((uint16_t)val, addr);
		(void)readw(addr);
		break;
	default:
		writel(val, addr);
		(void)readl(addr);
		break;
	}
	spin_unlock_irqrestore(&vmd->cfg_lock, fl);
	return PCIBIOS_SUCCESSFUL;
}

/* ---- interrupts --------------------------------------------------------- */

static int vmd_irq(void *arg)
{
	struct vmd_irq_list *l = arg;
	uint64_t fl;

	/* Every child attached to this vector: the VMD cannot say which
	 * one sent the message; each handler checks its own device. */
	spin_lock_irqsave(&l->lock, &fl);
	for (int i = 0; i < l->nirqs; i++)
		irq_handle_virtual(l->irqs[i]);
	spin_unlock_irqrestore(&l->lock, fl);
	return IRQ_HANDLED;
}

static void vmd_set_msi_remapping(struct vmd_dev *vmd, int enable)
{
	uint16_t reg = 0;

	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg);
	reg = enable ? (uint16_t)(reg & ~VMCONFIG_MSI_REMAP) :
		       (uint16_t)(reg | VMCONFIG_MSI_REMAP);
	pci_write_config_word(vmd->dev, PCI_REG_VMCONFIG, reg);
}

static int vmd_alloc_irqs(struct vmd_dev *vmd)
{
	pci_device_t *dev = vmd->dev;
	int n;

	might_sleep();
	if (vmd->irqs_ready)
		return vmd->irqs_ready > 0 ? 0 : -ENODEV;
	n = pci_msix_vec_count(dev);
	if (n < 0) {
		vmd->irqs_ready = -1;
		return -ENODEV;
	}
	n = pci_alloc_irq_vectors(dev, (unsigned int)vmd->first_vec + 1,
				  (unsigned int)n, PCI_IRQ_MSIX);
	if (n < 0) {
		pci_err(dev, "VMD: no MSI-X vectors (%d)\n", n);
		vmd->irqs_ready = -1;
		return n;
	}
	vmd->irqs = kcalloc((size_t)n, sizeof(*vmd->irqs));
	if (!vmd->irqs) {
		pci_free_irq_vectors(dev);
		vmd->irqs_ready = -1;
		return -ENOMEM;
	}
	vmd->msix_count = n;
	for (int i = 0; i < n; i++) {
		int rc;

		spinlock_init(&vmd->irqs[i].lock, "vmd_irq");
		vmd->irqs[i].virq = pci_irq_vector(dev, (unsigned int)i);
		rc = request_irq(vmd->irqs[i].virq, vmd_irq, 0, vmd->name,
				 &vmd->irqs[i]);
		if (rc) {
			pci_err(dev, "VMD: cannot request vector %d (%d)\n", i,
				rc);
			while (i--)
				(void)free_irq(vmd->irqs[i].virq,
					       &vmd->irqs[i]);
			pci_free_irq_vectors(dev);
			kfree(vmd->irqs);
			vmd->irqs = NULL;
			vmd->msix_count = 0;
			vmd->irqs_ready = -1;
			return rc;
		}
	}
	vmd->irqs_ready = 1;
	return 0;
}

int pci_vmd_irq_assign(const pci_device_t *dev)
{
	struct vmd_dev *vmd = dev ? vmd_of_segment(dev->segment) : NULL;
	int best, rc;
	uint64_t fl;

	might_sleep();
	if (!vmd)
		return -ENODEV;
	mutex_lock(&vmd->irq_mutex);
	rc = vmd_alloc_irqs(vmd);
	mutex_unlock(&vmd->irq_mutex);
	if (rc)
		return rc;
	/* Only one vector besides the reserved ones, or not an NVMe
	 * function (bridges, others): the first. */
	if (vmd->msix_count == 1 + vmd->first_vec ||
	    dev->class_full != 0x010802)
		best = vmd->first_vec;
	else {
		best = vmd->first_vec + 1;
		for (int i = best; i < vmd->msix_count; i++)
			if (vmd->irqs[i].count < vmd->irqs[best].count)
				best = i;
	}
	spin_lock_irqsave(&vmd->irqs[best].lock, &fl);
	vmd->irqs[best].count++;
	spin_unlock_irqrestore(&vmd->irqs[best].lock, fl);
	return best;
}

void pci_vmd_irq_unassign(const pci_device_t *dev, int index)
{
	struct vmd_dev *vmd = dev ? vmd_of_segment(dev->segment) : NULL;
	uint64_t fl;

	if (!vmd || !vmd->irqs || index < 0 || index >= vmd->msix_count)
		return;
	spin_lock_irqsave(&vmd->irqs[index].lock, &fl);
	if (!WARN_ON_ONCE(vmd->irqs[index].count == 0))
		vmd->irqs[index].count--;
	spin_unlock_irqrestore(&vmd->irqs[index].lock, fl);
}

void pci_vmd_irq_enable(const pci_device_t *dev, int index, int irq)
{
	struct vmd_dev *vmd = dev ? vmd_of_segment(dev->segment) : NULL;
	struct vmd_irq_list *l;
	uint64_t fl;

	if (!vmd || !vmd->irqs || index < 0 || index >= vmd->msix_count)
		return;
	l = &vmd->irqs[index];
	spin_lock_irqsave(&l->lock, &fl);
	for (int i = 0; i < l->nirqs; i++) {
		if (l->irqs[i] == irq) {
			WARN_ON_ONCE(1);
			spin_unlock_irqrestore(&l->lock, fl);
			return;
		}
	}
	if (WARN_ON(l->nirqs >= VMD_MAX_CHILD_IRQS)) {
		spin_unlock_irqrestore(&l->lock, fl);
		return;
	}
	l->irqs[l->nirqs++] = irq;
	spin_unlock_irqrestore(&l->lock, fl);
}

void pci_vmd_irq_disable(const pci_device_t *dev, int index, int irq)
{
	struct vmd_dev *vmd = dev ? vmd_of_segment(dev->segment) : NULL;
	struct vmd_irq_list *l;
	uint64_t fl;

	if (!vmd || !vmd->irqs || index < 0 || index >= vmd->msix_count)
		return;
	l = &vmd->irqs[index];
	/* Under the lock the handler holds: once this returns, no CPU is
	 * inside a handler of this child through the VMD. */
	spin_lock_irqsave(&l->lock, &fl);
	for (int i = 0; i < l->nirqs; i++) {
		if (l->irqs[i] != irq)
			continue;
		for (int j = i + 1; j < l->nirqs; j++)
			l->irqs[j - 1] = l->irqs[j];
		l->nirqs--;
		break;
	}
	spin_unlock_irqrestore(&l->lock, fl);
}

/* ---- the domain ---------------------------------------------------------- */

static int vmd_get_phys_offsets(struct vmd_dev *vmd, int native_hint)
{
	pci_device_t *dev = vmd->dev;
	uint64_t phys1, phys2;

	if (native_hint) {
		uint32_t vmlock = 0;
		volatile uint8_t *membar2;

		if (pci_read_config_dword(dev, PCI_REG_VMLOCK, &vmlock) ||
		    PCI_POSSIBLE_ERROR(vmlock))
			return -ENODEV;
		if (!MB2_SHADOW_EN(vmlock))
			return 0;
		membar2 = pci_iomap(dev, VMD_MEMBAR2, MB2_SHADOW_OFFSET +
							      MB2_SHADOW_SIZE);
		if (!membar2)
			return -ENOMEM;
		phys1 = readq(membar2 + MB2_SHADOW_OFFSET);
		phys2 = readq(membar2 + MB2_SHADOW_OFFSET + 8);
		pci_iounmap(dev, membar2);
	} else {
		/* The hypervisor's "SHDW" vendor-specific capability. */
		uint8_t pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
		uint32_t reg = 0, regu = 0;

		if (!pos)
			return 0;
		pci_read_config_dword(dev, pos + 4, &reg);
		if (reg != 0x53484457)
			return 0;
		pci_read_config_dword(dev, pos + 8, &reg);
		pci_read_config_dword(dev, pos + 12, &regu);
		phys1 = ((uint64_t)regu << 32) | reg;
		pci_read_config_dword(dev, pos + 16, &reg);
		pci_read_config_dword(dev, pos + 20, &regu);
		phys2 = ((uint64_t)regu << 32) | reg;
	}
	vmd->offset1 = pci_resource_start(dev, VMD_MEMBAR1) - (phys1 & ~0xFULL);
	vmd->offset2 = pci_resource_start(dev, VMD_MEMBAR2) - (phys2 & ~0xFULL);
	return 0;
}

static int vmd_get_bus_number_start(struct vmd_dev *vmd)
{
	uint16_t reg = 0;

	pci_read_config_word(vmd->dev, PCI_REG_VMCAP, &reg);
	if (!BUS_RESTRICT_CAP(reg))
		return 0;
	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg);
	switch (BUS_RESTRICT_CFG(reg)) {
	case 0:
		vmd->busn_start = 0;
		break;
	case 1:
		vmd->busn_start = 128;
		break;
	case 2:
		vmd->busn_start = 224;
		break;
	default:
		pci_err(vmd->dev, "VMD: unknown bus offset setting (%d)\n",
			BUS_RESTRICT_CFG(reg));
		return -ENODEV;
	}
	return 0;
}

/* Close the I/O and memory windows the firmware left on every bridge of
 * the domain (they are reassigned from the MEMBARs). */
static void vmd_domain_reset(struct vmd_dev *vmd)
{
	for (unsigned int b = 0; b < vmd->bus_count; b++) {
		uint8_t bus = (uint8_t)(vmd->busn_start + b);

		for (unsigned int slot = 0; slot < 32; slot++) {
			volatile uint8_t *base;
			unsigned int functions;
			uint8_t hdr;

			base = vmd_cfg_addr(vmd, bus, (uint8_t)PCI_DEVFN(slot, 0),
					    0, 4);
			if (!base)
				continue;
			hdr = readb(base + PCI_HEADER_TYPE);
			functions = (hdr & PCI_HEADER_TYPE_MFD) ? 8 : 1;
			for (unsigned int fn = 0; fn < functions; fn++) {
				base = vmd_cfg_addr(vmd, bus,
						    (uint8_t)PCI_DEVFN(slot, fn),
						    0, 4);
				if (!base)
					continue;
				hdr = readb(base + PCI_HEADER_TYPE) &
				      PCI_HEADER_TYPE_MASK;
				if (hdr != PCI_HEADER_TYPE_BRIDGE ||
				    readw(base + PCI_CLASS_DEVICE) !=
					    PCI_CLASS_BRIDGE_PCI)
					continue;
				/* I/O off before its base changes. */
				writel(0x0000ffff, base + PCI_IO_BASE_UPPER16);
				writew(0x00f0, base + PCI_IO_BASE);
				writel(0, base + PCI_IO_BASE_UPPER16);
				writel(0x0000fff0, base + PCI_MEMORY_BASE);
				writel(0, base + PCI_PREF_LIMIT_UPPER32);
				writel(0x0000fff0, base + PCI_PREF_MEMORY_BASE);
				writel(0xffffffff, base + PCI_PREF_BASE_UPPER32);
			}
		}
	}
}

static int vmd_pm_quirk(pci_device_t *pdev, void *data)
{
	uint32_t features = *(uint32_t *)data;
	uint32_t ltr = 0;

	if (!(features & VMD_FEAT_BIOS_PM_QUIRK))
		return 0;
	if (pdev->ltr_cap) {
		pci_read_config_dword(pdev, pdev->ltr_cap + PCI_LTR_MAX_SNOOP_LAT,
				      &ltr);
		/* The firmware set a value: keep it. */
		if (!(ltr & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK))) {
			ltr = ((uint32_t)VMD_BIOS_PM_QUIRK_LTR << 16) |
			      VMD_BIOS_PM_QUIRK_LTR;
			pci_write_config_dword(
				pdev, pdev->ltr_cap + PCI_LTR_MAX_SNOOP_LAT, ltr);
			pci_info(pdev, "VMD: default LTR value set\n");
		}
	}
	/* D0 before L1 substates may be used. */
	(void)pci_set_power_state(pdev, PCI_D0);
	return 0;
}

/* Around the bus reset of the first root port: every function below it
 * keeps the configuration the firmware gave it (BARs, bus numbers,
 * command register).  The BAR cache was sized from those values when the
 * scan recorded the function, and the assignment keeps a BAR that already
 * lies inside the MEMBARs -- without the restore that BAR would be zero
 * in the device while the cache still says it is assigned. */
static int vmd_save_and_disable(pci_device_t *pdev, void *data)
{
	(void)data;
	/* The PM registers come back as D0 after the reset. */
	(void)pci_set_power_state(pdev, PCI_D0);
	(void)pci_save_state(pdev);
	/* No decode and no bus mastering across the reset. */
	pci_write_config_word(pdev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);
	return 0;
}

static int vmd_restore(pci_device_t *pdev, void *data)
{
	(void)data;
	pci_restore_state(pdev);
	return 0;
}

static int vmd_configure_dev(pci_device_t *pdev, void *data)
{
	pci_device_t *bridge = pdev->bus_self;

	(void)data;
	/* Max Payload Size: what the port above uses, when supported. */
	if (bridge && pci_is_pcie(pdev) && pci_is_pcie(bridge)) {
		int mps = pcie_get_mps(pdev), p_mps = pcie_get_mps(bridge);
		uint32_t devcap = 0;

		pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP, &devcap);
		if (mps > 0 && p_mps > 0 && mps != p_mps &&
		    p_mps <= (128 << (devcap & PCI_EXP_DEVCAP_PAYLOAD)))
			(void)pcie_set_mps(pdev, p_mps);
	}
	return 0;
}

static int vmd_enable_domain(struct vmd_dev *vmd)
{
	pci_device_t *dev = vmd->dev;
	uint64_t membar2_offset = 0x2000;
	struct pci_cfg_ops ops;
	pci_device_t *devs;
	int count = 0, before = 0, rc;
	unsigned int next_bus;
	uint8_t last_bus;

	if (vmd->features & VMD_FEAT_HAS_MEMBAR_SHADOW) {
		membar2_offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE;
		rc = vmd_get_phys_offsets(vmd, 1);
		if (rc)
			return rc;
	} else if (vmd->features & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) {
		rc = vmd_get_phys_offsets(vmd, 0);
		if (rc)
			return rc;
	}
	if (vmd->features & VMD_FEAT_HAS_BUS_RESTRICTIONS) {
		rc = vmd_get_bus_number_start(vmd);
		if (rc)
			return rc;
	}
	vmd->bus_count = (unsigned int)(vmd->cfgbar_size >> 20);
	if (vmd->bus_count == 0)
		return -ENODEV;
	if (vmd->busn_start + vmd->bus_count > 256)
		vmd->bus_count = 256u - vmd->busn_start;
	last_bus = (uint8_t)(vmd->busn_start + vmd->bus_count - 1);
	vmd->mb1_cpu = pci_resource_start(dev, VMD_MEMBAR1);
	vmd->mb1_end = vmd->mb1_cpu + pci_resource_len(dev, VMD_MEMBAR1) - 1;
	vmd->mb2_cpu = pci_resource_start(dev, VMD_MEMBAR2) + membar2_offset;
	vmd->mb2_end = pci_resource_start(dev, VMD_MEMBAR2) +
		       pci_resource_len(dev, VMD_MEMBAR2) - 1;
	if (!vmd->mb1_cpu || !pci_resource_len(dev, VMD_MEMBAR1) ||
	    !pci_resource_len(dev, VMD_MEMBAR2) || vmd->mb2_cpu > vmd->mb2_end)
		return -ENODEV;

	/* Messages: remapped through the VMD unless it can let them pass
	 * (and its windows are not translated, where the bypass would send
	 * host addresses to the guest's APIC page). */
	if (!(vmd->features & VMD_FEAT_CAN_BYPASS_MSI_REMAP) || vmd->offset1 ||
	    vmd->offset2) {
		vmd->msi_remap = 1;
		vmd_set_msi_remapping(vmd, 1);
	} else {
		vmd->msi_remap = 0;
		vmd_set_msi_remapping(vmd, 0);
	}

	vmd->domain = PCI_VMD_DOMAIN_BASE + (uint32_t)vmd->instance;
	ops.read = vmd_pci_read;
	ops.write = vmd_pci_write;
	ops.ctx = vmd;
	rc = pci_register_segment_ops(vmd->domain, &ops);
	if (rc)
		return rc;
	g_vmd[g_vmd_count++] = vmd;

	vmd_domain_reset(vmd);
	pci_dev_table(&before);
	next_bus = (unsigned int)vmd->busn_start + 1;
	(void)pci_scan_bus(vmd->domain, vmd->busn_start, 1, &next_bus, last_bus);

	/* The firmware did not train the links behind the root ports the
	 * OS now owns: reset the first one's secondary bus. */
	devs = pci_dev_table(&count);
	for (int i = before; i < count; i++) {
		pci_device_t *d = &devs[i];

		if (d->is_removed || d->segment != vmd->domain ||
		    !d->bus_self || d->bus_self->bus != vmd->busn_start)
			continue;
		/* Parents restored before their children: a bridge's bus
		 * numbers must be back before anything below it is reached. */
		pci_walk_bridge(d->bus_self, 0, 1, vmd_save_and_disable, NULL);
		rc = pci_bridge_secondary_bus_reset(d->bus_self);
		if (rc)
			pci_warn(d, "VMD: can't reset device: %d\n", rc);
		pci_walk_bridge(d->bus_self, 0, 0, vmd_restore, NULL);
		break;
	}

	rc = pci_assign_bus_resources(vmd->domain, vmd->busn_start,
				      vmd->mb1_cpu - vmd->offset1,
				      vmd->mb1_end - vmd->offset1,
				      vmd->mb2_cpu - vmd->offset2,
				      vmd->mb2_end - vmd->offset2,
				      (pci_resource_flags(dev, VMD_MEMBAR2) &
				       PCI_BAR_MEM64) != 0);
	if (rc)
		pci_warn(dev, "VMD: not every BAR could be assigned (%d)\n", rc);

	devs = pci_dev_table(&count);
	for (int i = before; i < count; i++) {
		pci_device_t *d = &devs[i];

		if (d->is_removed || d->segment != vmd->domain)
			continue;
		d->flags |= PCI_DEV_FL_BEHIND_VMD;
		/* Assigned above, before any driver bound: the cache is
		 * sized again with the domain's translation in place. */
		pci_bars_refresh(d);
		pci_priv(d)->vmd = vmd;
		(void)vmd_pm_quirk(d, &vmd->features);
		(void)vmd_configure_dev(d, NULL);
	}
	pci_info(dev, "VMD: bound to PCI domain %04x, buses %02x-%02x, %d device(s)%s\n",
		 vmd->domain, vmd->busn_start, last_bus, count - before,
		 vmd->msi_remap ? ", MSI remapped" : "");
	/* Drivers registered already get their devices now; later ones
	 * find them at registration. */
	for (int i = before; i < count; i++) {
		pci_device_t *d = &devs[i];

		if (!d->is_removed && d->segment == vmd->domain)
			pci_bus_add_device(d);
	}
	return 0;
}

static int vmd_probe(pci_device_t *dev, const struct pci_device_id *id)
{
	struct vmd_dev *vmd;
	int rc;

	if (g_vmd_count >= VMD_MAX_DOMAINS)
		return -ENOSPC;
	if (pci_resource_len(dev, VMD_CFGBAR) < (1u << 20))
		return -ENOMEM;
	vmd = kcalloc(1, sizeof(*vmd));
	if (!vmd)
		return -ENOMEM;
	vmd->dev = dev;
	vmd->features = (uint32_t)id->driver_data;
	vmd->instance = g_vmd_count;
	ksnprintf(vmd->name, sizeof(vmd->name), "vmd%d", vmd->instance);
	spinlock_init(&vmd->cfg_lock, "vmd_cfg");
	mutex_init(&vmd->irq_mutex, "vmd_irq");
	rc = pci_enable_device_mem(dev);
	if (rc)
		goto fail;
	vmd->cfgbar_size = pci_resource_len(dev, VMD_CFGBAR);
	vmd->cfgbar = pci_iomap(dev, VMD_CFGBAR, 0);
	if (!vmd->cfgbar) {
		rc = -ENOMEM;
		goto fail;
	}
	pci_set_master(dev);
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)) &&
	    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))) {
		rc = -ENODEV;
		goto fail;
	}
	if (vmd->features & VMD_FEAT_OFFSET_FIRST_VECTOR)
		vmd->first_vec = 1;
	pci_set_drvdata(dev, vmd);
	rc = vmd_enable_domain(vmd);
	if (rc)
		goto fail;
	return 0;
fail:
	pci_set_drvdata(dev, NULL);
	kfree(vmd);
	return rc;
}

static int vmd_unbind_one(pci_device_t *d, void *data)
{
	(void)data;
	pci_device_release_driver(d);
	return 0;
}

static void vmd_remove(pci_device_t *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);
	pci_device_t *devs;
	int count = 0;

	if (!vmd)
		return;
	/* The domain's devices go first (their drivers still need the
	 * config window and the vectors). */
	devs = pci_dev_table(&count);
	for (int i = count - 1; i >= 0; i--) {
		pci_device_t *d = &devs[i];

		if (d->is_removed || d->segment != vmd->domain)
			continue;
		(void)vmd_unbind_one(d, NULL);
		pci_stop_and_remove_bus_device(d);
	}
	if (vmd->irqs) {
		for (int i = 0; i < vmd->msix_count; i++)
			(void)free_irq(vmd->irqs[i].virq, &vmd->irqs[i]);
		pci_free_irq_vectors(dev);
	}
	vmd_set_msi_remapping(vmd, 1);
	pci_unregister_segment_ops(vmd->domain);
	for (int i = 0; i < g_vmd_count; i++)
		if (g_vmd[i] == vmd)
			g_vmd[i] = NULL;
	/* vmd stays allocated: lookups by segment may still be in flight
	 * on other CPUs; the record is small. */
	pci_set_drvdata(dev, NULL);
}

static const struct pci_device_id g_vmd_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x201d),
	  .driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP },
	{ PCI_VDEVICE(INTEL, 0x28c0),
	  .driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW |
			 VMD_FEAT_HAS_BUS_RESTRICTIONS |
			 VMD_FEAT_CAN_BYPASS_MSI_REMAP },
	{ PCI_VDEVICE(INTEL, 0x467f), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0x4c3d), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0xa77f), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0x7d0b), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0xad0b), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0x9a0b), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0xb60b), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0xb06f), .driver_data = VMD_FEATS_CLIENT },
	{ PCI_VDEVICE(INTEL, 0xb07f), .driver_data = VMD_FEATS_CLIENT },
	{ 0 }
};

static struct pci_driver g_vmd_driver = {
	.name = "vmd",
	.id_table = g_vmd_ids,
	.probe = vmd_probe,
	.remove = vmd_remove,
};

void pci_vmd_init(void)
{
	int rc = pci_register_driver(&g_vmd_driver);

	if (rc)
		kprintf("PCI: VMD driver registration failed (%d)\n", rc);
}
