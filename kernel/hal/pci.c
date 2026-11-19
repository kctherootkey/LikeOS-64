// LikeOS - PCI core: configuration access, enumeration, the device table,
// capabilities, PCI Express capability and link accessors, BARs, bridge
// resources and INTx routing.
//
// The original minimal interface (port-I/O config access on bus/dev/fn,
// one MSI message, one MSI-X entry per call, the flat device table) is
// kept exactly: every driver written against it keeps working.  The core
// interface of kernel/hal/pci.h is implemented here and in pci_ecam.c,
// pci_msi.c, pci_pm.c, pci_vmd.c and pci_driver.c.
//
// Configuration access follows the PC rule: the first 256 bytes of
// segment 0 go through configuration mechanism #1 (ports 0xCF8/0xCFC),
// the extended space and every other segment through ECAM, a VMD domain
// through its own window (registered segment accessors).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/mm/memory.h>
#include <kernel/fs/sysfs.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/pci_internal.h>
#include <kernel/hal/pci_capwalk.h>
#include <kernel/hal/acpi.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/io.h>
#include <kernel/hal/lapic.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h> /* errno values */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static struct pci_dev_priv g_pci_priv[PCI_MAX_DEVICES];
static volatile int g_pci_count = 0;

// Spinlock for PCI config space access (address + data must be atomic)
static spinlock_t pci_lock = SPINLOCK_INIT("pci");
spinlock_t g_pci_cfg_lock = SPINLOCK_INIT("pci_cfg");
DEFINE_MUTEX(g_pci_rescan_mutex);
/* Table appends (hot-plug, VMD, rescan) against each other. */
static spinlock_t g_pci_table_lock = SPINLOCK_INIT("pci_table");

static inline void outl(unsigned short port, unsigned int val)
{
	__asm__ volatile("outl %0,%1" ::"a"(val), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
	unsigned int v;
	__asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outw(unsigned short port, uint16_t val)
{
	__asm__ volatile("outw %0,%1" ::"a"(val), "Nd"(port));
}

static inline uint16_t inw(unsigned short port)
{
	uint16_t v;
	__asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outb_p(unsigned short port, uint8_t val)
{
	__asm__ volatile("outb %0,%1" ::"a"(val), "Nd"(port));
}

static inline uint8_t inb_p(unsigned short port)
{
	uint8_t v;
	__asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
	return v;
}

unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev,
			    unsigned char func, unsigned char off)
{
	BUG_ON(dev > 31 || func > 7);
	WARN_ON(off & 0x3); /* PCI config read must be 4-byte aligned */
	unsigned int address =
		(unsigned int)((1u << 31) | ((unsigned int)bus << 16) |
			       ((unsigned int)dev << 11) |
			       ((unsigned int)func << 8) | (off & 0xFC));
	uint64_t flags;
	spin_lock_irqsave(&pci_lock, &flags);
	outl(PCI_CONFIG_ADDRESS, address);
	unsigned int value = inl(PCI_CONFIG_DATA);
	spin_unlock_irqrestore(&pci_lock, flags);
	return value;
}

void pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func,
		     unsigned char off, unsigned int value)
{
	BUG_ON(dev > 31 || func > 7);
	WARN_ON(off & 0x3); /* PCI config write must be 4-byte aligned */
	unsigned int address =
		(unsigned int)((1u << 31) | ((unsigned int)bus << 16) |
			       ((unsigned int)dev << 11) |
			       ((unsigned int)func << 8) | (off & 0xFC));
	uint64_t flags;
	spin_lock_irqsave(&pci_lock, &flags);
	outl(PCI_CONFIG_ADDRESS, address);
	outl(PCI_CONFIG_DATA, value);
	spin_unlock_irqrestore(&pci_lock, flags);
}

/* ==========================================================================
 * Configuration access: mechanism #1, segment accessors, dispatch
 * ========================================================================== */

/* Mechanism #1 with the access width the caller asked for: a byte or
 * word write must not be widened to a dword read-modify-write, which
 * would write back (and so clear) write-1-to-clear status bits next to
 * the target. */
int pci_conf1_read(uint8_t bus, uint8_t devfn, int where, int size,
		   uint32_t *val)
{
	uint32_t address;
	uint64_t flags;

	if (where < 0 || where > 255 || (where & (size - 1))) {
		*val = PCI_ERROR_RESPONSE;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)devfn << 8) |
		  ((uint32_t)where & 0xFC);
	spin_lock_irqsave(&pci_lock, &flags);
	outl(PCI_CONFIG_ADDRESS, address);
	switch (size) {
	case 1:
		*val = inb_p((unsigned short)(PCI_CONFIG_DATA + (where & 3)));
		break;
	case 2:
		*val = inw((unsigned short)(PCI_CONFIG_DATA + (where & 2)));
		break;
	default:
		*val = inl(PCI_CONFIG_DATA);
		break;
	}
	spin_unlock_irqrestore(&pci_lock, flags);
	return PCIBIOS_SUCCESSFUL;
}

int pci_conf1_write(uint8_t bus, uint8_t devfn, int where, int size,
		    uint32_t val)
{
	uint32_t address;
	uint64_t flags;

	if (where < 0 || where > 255 || (where & (size - 1)))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	address = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)devfn << 8) |
		  ((uint32_t)where & 0xFC);
	spin_lock_irqsave(&pci_lock, &flags);
	outl(PCI_CONFIG_ADDRESS, address);
	switch (size) {
	case 1:
		outb_p((unsigned short)(PCI_CONFIG_DATA + (where & 3)),
		       (uint8_t)val);
		break;
	case 2:
		outw((unsigned short)(PCI_CONFIG_DATA + (where & 2)),
		     (uint16_t)val);
		break;
	default:
		outl(PCI_CONFIG_DATA, val);
		break;
	}
	spin_unlock_irqrestore(&pci_lock, flags);
	return PCIBIOS_SUCCESSFUL;
}

#define PCI_MAX_SEG_OPS 8
static struct {
	uint32_t segment;
	struct pci_cfg_ops ops;
	int used;
} g_seg_ops[PCI_MAX_SEG_OPS];
static spinlock_t g_seg_ops_lock = SPINLOCK_INIT("pci_seg_ops");

int pci_register_segment_ops(uint32_t segment, const struct pci_cfg_ops *ops)
{
	uint64_t fl;
	int rc = -ENOSPC;

	if (!ops || !ops->read || !ops->write)
		return -EINVAL;
	spin_lock_irqsave(&g_seg_ops_lock, &fl);
	for (int i = 0; i < PCI_MAX_SEG_OPS; i++) {
		if (g_seg_ops[i].used && g_seg_ops[i].segment == segment) {
			rc = -EBUSY;
			break;
		}
	}
	if (rc != -EBUSY) {
		for (int i = 0; i < PCI_MAX_SEG_OPS; i++) {
			if (!g_seg_ops[i].used) {
				g_seg_ops[i].segment = segment;
				g_seg_ops[i].ops = *ops;
				__atomic_store_n(&g_seg_ops[i].used, 1,
						 __ATOMIC_RELEASE);
				rc = 0;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&g_seg_ops_lock, fl);
	return rc;
}

void pci_unregister_segment_ops(uint32_t segment)
{
	uint64_t fl;

	spin_lock_irqsave(&g_seg_ops_lock, &fl);
	for (int i = 0; i < PCI_MAX_SEG_OPS; i++)
		if (g_seg_ops[i].used && g_seg_ops[i].segment == segment)
			__atomic_store_n(&g_seg_ops[i].used, 0,
					 __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_seg_ops_lock, fl);
}

static const struct pci_cfg_ops *pci_segment_ops(uint32_t segment)
{
	for (int i = 0; i < PCI_MAX_SEG_OPS; i++)
		if (__atomic_load_n(&g_seg_ops[i].used, __ATOMIC_ACQUIRE) &&
		    g_seg_ops[i].segment == segment)
			return &g_seg_ops[i].ops;
	return NULL;
}

int pci_raw_read(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		 int size, uint32_t *val)
{
	const struct pci_cfg_ops *ops;
	int rc;

	BUG_ON(val == NULL);
	if ((size != 1 && size != 2 && size != 4) || where < 0 ||
	    where >= PCI_CFG_SPACE_EXP_SIZE || (where & (size - 1))) {
		*val = PCI_ERROR_RESPONSE;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	if (segment >= PCI_VMD_DOMAIN_BASE) {
		ops = pci_segment_ops(segment);
		if (!ops) {
			*val = PCI_ERROR_RESPONSE;
			return PCIBIOS_DEVICE_NOT_FOUND;
		}
		rc = ops->read(ops->ctx, segment, bus, devfn, where, size, val);
	} else if (segment == 0 && where < PCI_CFG_SPACE_SIZE) {
		rc = pci_conf1_read(bus, devfn, where, size, val);
	} else {
		rc = pci_ecam_read(segment, bus, devfn, where, size, val);
	}
	if (rc != PCIBIOS_SUCCESSFUL)
		*val = size == 4 ? PCI_ERROR_RESPONSE :
				   (size == 2 ? 0xFFFFu : 0xFFu);
	return rc;
}

int pci_raw_write(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		  int size, uint32_t val)
{
	const struct pci_cfg_ops *ops;

	if ((size != 1 && size != 2 && size != 4) || where < 0 ||
	    where >= PCI_CFG_SPACE_EXP_SIZE || (where & (size - 1)))
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (segment >= PCI_VMD_DOMAIN_BASE) {
		ops = pci_segment_ops(segment);
		if (!ops)
			return PCIBIOS_DEVICE_NOT_FOUND;
		return ops->write(ops->ctx, segment, bus, devfn, where, size,
				  val);
	}
	if (segment == 0 && where < PCI_CFG_SPACE_SIZE)
		return pci_conf1_write(bus, devfn, where, size, val);
	return pci_ecam_write(segment, bus, devfn, where, size, val);
}

static inline uint8_t pci_dev_devfn(const pci_device_t *dev)
{
	return (uint8_t)PCI_DEVFN(dev->device, dev->function);
}

static int pci_dev_read(const pci_device_t *dev, int where, int size,
			uint32_t *val)
{
	if (!dev) {
		*val = PCI_ERROR_RESPONSE;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	if (__atomic_load_n(&dev->is_removed, __ATOMIC_ACQUIRE) ||
	    pci_dev_is_disconnected(dev)) {
		*val = size == 4 ? PCI_ERROR_RESPONSE :
				   (size == 2 ? 0xFFFFu : 0xFFu);
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	if (where >= (dev->cfg_size ? dev->cfg_size : PCI_CFG_SPACE_SIZE) &&
	    where >= PCI_CFG_SPACE_SIZE) {
		*val = size == 4 ? PCI_ERROR_RESPONSE :
				   (size == 2 ? 0xFFFFu : 0xFFu);
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	return pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev), where,
			    size, val);
}

static int pci_dev_write(const pci_device_t *dev, int where, int size,
			 uint32_t val)
{
	if (!dev)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (__atomic_load_n(&dev->is_removed, __ATOMIC_ACQUIRE) ||
	    pci_dev_is_disconnected(dev))
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (where >= (dev->cfg_size ? dev->cfg_size : PCI_CFG_SPACE_SIZE) &&
	    where >= PCI_CFG_SPACE_SIZE)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	return pci_raw_write(dev->segment, dev->bus, pci_dev_devfn(dev), where,
			     size, val);
}

int pci_read_config_byte(const pci_device_t *dev, int where, uint8_t *val)
{
	uint32_t v;
	int rc = pci_dev_read(dev, where, 1, &v);

	*val = (uint8_t)v;
	return rc;
}

int pci_read_config_word(const pci_device_t *dev, int where, uint16_t *val)
{
	uint32_t v;
	int rc = pci_dev_read(dev, where, 2, &v);

	*val = (uint16_t)v;
	return rc;
}

int pci_read_config_dword(const pci_device_t *dev, int where, uint32_t *val)
{
	return pci_dev_read(dev, where, 4, val);
}

int pci_write_config_byte(const pci_device_t *dev, int where, uint8_t val)
{
	return pci_dev_write(dev, where, 1, val);
}

int pci_write_config_word(const pci_device_t *dev, int where, uint16_t val)
{
	return pci_dev_write(dev, where, 2, val);
}

int pci_write_config_dword(const pci_device_t *dev, int where, uint32_t val)
{
	return pci_dev_write(dev, where, 4, val);
}

void pci_enable_busmaster_mem(const pci_device_t *dev)
{
	if (!dev) {
		return;
	}
	unsigned int cmd = pci_cfg_read32_dev(dev, 0x04);
	unsigned int newcmd = cmd | 0x6; /* MEM + BUS MASTER */
	if (newcmd != cmd) {
		pci_cfg_write32_dev(dev, 0x04, newcmd);
	}
	{
		pci_device_t *m = pci_dev_mut(dev);

		if (m)
			m->flags |= PCI_DEV_FL_ENABLED | PCI_DEV_FL_MASTER;
	}
}

void pci_init(void)
{
	g_pci_count = 0;
}

// BAR allocator: assigns from the PCI host bridge memory windows, growing
// downward.  On Intel Arrow Lake / Meteor Lake:
//   32-bit window: 0x70000000-0xBFFFFFFF
//   64-bit window: ~0x5000000000-0x501C2FFFFF (above 4 GB)
// The 64-bit BARs of LPSS I2C land in the 64-bit window (firmware
// assigns e.g. 0x501C2DD000 there).
static unsigned long g_next_bar_base = 0xBF000000UL;
static unsigned long g_next_bar64_base = 0x501C300000UL;

static unsigned long round_up_pow2(unsigned long size)
{
	if (size == 0)
		return 0x1000;
	if (size & (size - 1)) {
		unsigned long p = 1;
		while (p < size)
			p <<= 1;
		return p;
	}
	return size;
}

static unsigned long alloc_bar_region(unsigned long size)
{
	size = round_up_pow2(size);
	if (g_next_bar_base < size)
		return 0;
	g_next_bar_base -= size;
	g_next_bar_base &= ~(size - 1);
	return g_next_bar_base;
}

static unsigned long alloc_bar64_region(unsigned long size)
{
	size = round_up_pow2(size);
	if (g_next_bar64_base < size)
		return 0;
	g_next_bar64_base -= size;
	g_next_bar64_base &= ~(size - 1);
	return g_next_bar64_base;
}

/* May a BAR of `self' (index `self_bar') be placed at [base, base + size)?
 * Not over RAM the firmware's memory map describes, not over the boot
 * framebuffer, and not where another function's memory BAR already
 * starts.  Only the bases of the others are
 * known here: the boot enumeration never sizes a BAR, since sizing pauses
 * the decode of a function that may be live (the boot display). */
static int pci_bar_range_free(const pci_device_t *self, int self_bar,
			      unsigned long base, unsigned long size)
{
	uint64_t fb_phys, fb_size;

	if (mm_phys_range_is_ram(base, size))
		return 0;
	/* The boot framebuffer: on many machines it lies inside a graphics
	 * aperture whose base can be far below the candidate. */
	if (console_boot_fb_range(&fb_phys, &fb_size) == 0 && fb_size &&
	    fb_phys < base + size && base < fb_phys + fb_size)
		return 0;
	for (int i = 0; i < g_pci_count; i++) {
		const pci_device_t *d = &g_pci_devices[i];
		int nbars;

		/* A VMD domain's BARs hold bus addresses. */
		if (d->segment >= PCI_VMD_DOMAIN_BASE)
			continue;
		if (d->hdr_type == PCI_HEADER_TYPE_NORMAL)
			nbars = 6;
		else if (d->hdr_type == PCI_HEADER_TYPE_BRIDGE)
			nbars = 2;
		else
			continue;
		for (int j = 0; j < nbars; j++) {
			uint32_t lo = d->bar[j];
			unsigned long addr;
			int skip;

			if (lo & 1)
				continue; /* I/O */
			addr = lo & ~0xFUL;
			/* Not assigned, or the BAR being placed. */
			skip = addr < 0x1000 || (d == self && j == self_bar);
			if (((lo >> 1) & 3) == 2 && j + 1 < nbars) {
				addr |= (unsigned long)d->bar[j + 1] << 32;
				skip = addr < 0x1000 ||
				       (d == self && j == self_bar);
				j++; /* the upper dword is no BAR */
			}
			if (skip)
				continue;
			if (addr >= base && addr - base < size)
				return 0;
		}
	}
	return 1;
}

void pci_assign_unassigned_bars(void)
{
	for (int i = 0; i < g_pci_count; i++) {
		pci_device_t *p = &g_pci_devices[i];

		/* Only the boot-time segment-0 devices: a VMD domain and a
		 * hot-plug slot are assigned from their bridge windows. */
		if (p->segment != 0)
			continue;
		/* Skip bridges — their config registers at 0x10-0x24 are NOT
           standard BARs.  Writing 0xFFFFFFFF to probe size on an ISA/eSPI
           bridge (D31:F0) corrupts PCH IO decode and kills ALL legacy
           fixed IO (ports 0x60, 0x61, 0x64, 0x71, etc.).  PCI-to-PCI
           bridges also have non-standard layouts starting at 0x18. */
		if (p->class_code == 0x06) /* Bridge device */
			continue;

		for (int bar = 0; bar < 6; ++bar) {
			unsigned int off = 0x10 + bar * 4;
			unsigned int val = pci_cfg_read32(p->bus, p->device,
							  p->function, off);
			/* Treat zero, 0xFFFFFFFF, or suspicious tiny (<4KB) mem BARs as unassigned */
			int looks_mem = (val & 0x1) == 0; /* memory BAR */
			/* A 64-bit memory BAR's address is both dwords: one
			 * the firmware placed on a 4 GB boundary (a graphics
			 * aperture at 0x4000000000, say) has a low dword of
			 * just its flag bits, and is assigned all the same. */
			int cur64 = looks_mem && (val & 0x6) == 0x4 && bar < 5;
			unsigned long base_field = val & ~0xFUL;
			if (cur64)
				base_field |=
					(unsigned long)pci_cfg_read32(
						p->bus, p->device, p->function,
						off + 4)
					<< 32;
			if (!(val == 0 || val == 0xFFFFFFFF ||
			      (looks_mem && base_field < 0x1000))) {
				/* Assigned.  The upper dword of a 64-bit
				 * BAR is not a BAR of its own. */
				if (cur64)
					bar++;
				continue;
			}
			{
				/* Probe size */
				pci_cfg_write32(p->bus, p->device, p->function,
						off, 0xFFFFFFFF);
				unsigned int mask = pci_cfg_read32(
					p->bus, p->device, p->function, off);
				if (mask == 0 || mask == 0xFFFFFFFF) {
					/* restore original and skip */
					pci_cfg_write32(p->bus, p->device,
							p->function, off, val);
					continue; /* cannot size */
				}
				if (mask & 0x1) {
					continue; /* IO BAR skip */
				}
				int is64 = (mask & 0x4) ? 1 : 0;
				unsigned int mask_hi = 0;
				if (is64 && bar < 5) {
					unsigned int off_hi =
						0x10 + (bar + 1) * 4;
					pci_cfg_write32(p->bus, p->device,
							p->function, off_hi,
							0xFFFFFFFF);
					mask_hi = pci_cfg_read32(
						p->bus, p->device, p->function,
						off_hi);
				}
				/* Restore original mask probe scribbles (write 0 before assigning new base to avoid transient decode) */
				pci_cfg_write32(p->bus, p->device, p->function,
						off, 0);
				if (is64 && bar < 5) {
					unsigned int off_hi =
						0x10 + (bar + 1) * 4;
					pci_cfg_write32(p->bus, p->device,
							p->function, off_hi, 0);
				}
				unsigned long size_mask =
					(unsigned long)(mask & ~0xFUL);
				if (is64) {
					size_mask |=
						((unsigned long)mask_hi << 32);
				}
				unsigned long size = (~size_mask + 1);
				/* What the device decodes; `size' is the slot
				 * it is given, which may be larger. */
				unsigned long decode = size;
				if (size == 0 || size > (1UL << 24)) {
					/* Address bits that do not all reach
					 * bit 63 (a 32-bit BAR, a 64-bit one
					 * with fewer implemented bits): the
					 * size is the lowest set bit. */
					unsigned long real =
						size_mask & (~size_mask + 1);
					if (real == 0 || real > (1UL << 24)) {
						/* The device decodes all of
						 * it, more than this allocator
						 * places: leave it unassigned. */
						kprintf("PCI: %02x:%02x.%x BAR%d of 0x%lx bytes left unassigned\n",
							p->bus, p->device,
							p->function, bar, real);
						p->bar[bar] = pci_cfg_read32(
							p->bus, p->device,
							p->function, off);
						if (is64 && bar < 5) {
							p->bar[bar + 1] = 0;
							bar++;
						}
						continue;
					}
					/* At least the 64 KB slot such BARs
					 * have always had. */
					size = real < (1UL << 16) ? (1UL << 16) :
								    real;
					decode = real;
				}
				/* Use 64-bit window for Intel serial bus controllers
                   (class 0x0C, subclass 0x80 — covers all LPSS I2C/UART).
                   Everything else gets 32-bit addresses. */
				int use64 = is64 && p->vendor_id == 0x8086 &&
					    p->class_code == 0x0C &&
					    p->subclass == 0x80;
				unsigned long base =
					use64 ? alloc_bar64_region(size) :
						alloc_bar_region(size);
				if (base &&
				    !pci_bar_range_free(p, bar, base,
							round_up_pow2(decode))) {
					kprintf("PCI: %02x:%02x.%x BAR%d not placed at 0x%lx (RAM or another BAR there), left unassigned\n",
						p->bus, p->device, p->function,
						bar, base);
					base = 0;
				}
				if (base) {
					unsigned int low =
						(unsigned int)((base &
								0xFFFFFFFFUL) |
							       (mask & 0xF));
					pci_cfg_write32(p->bus, p->device,
							p->function, off, low);
					if (is64 && bar < 5) {
						unsigned int off_hi =
							0x10 + (bar + 1) * 4;
						pci_cfg_write32(
							p->bus, p->device,
							p->function, off_hi,
							(unsigned int)(base >>
								       32));
						/* consume next BAR slot for 64-bit */
						p->bar[bar] = low;
						p->bar[bar + 1] =
							(unsigned int)(base >>
								       32);
						bar++;
						continue;
					}
					/* Re-read and store */
					p->bar[bar] = pci_cfg_read32(
						p->bus, p->device, p->function,
						off);
				} else if (is64 && bar < 5) {
					bar++; /* its upper dword is no BAR */
				}
			}
		}
	}
}

/* ==========================================================================
 * The device table
 * ========================================================================== */

pci_device_t *pci_dev_table(int *count)
{
	if (count)
		*count = __atomic_load_n(&g_pci_count, __ATOMIC_ACQUIRE);
	return g_pci_devices;
}

int pci_dev_index(const pci_device_t *dev)
{
	uintptr_t base = (uintptr_t)g_pci_devices;
	uintptr_t p = (uintptr_t)dev;

	if (!dev || p < base ||
	    p >= base + sizeof(g_pci_devices) ||
	    (p - base) % sizeof(pci_device_t))
		return -1;
	return (int)((p - base) / sizeof(pci_device_t));
}

struct pci_dev_priv *pci_priv(const pci_device_t *dev)
{
	int i = pci_dev_index(dev);

	return i < 0 ? NULL : &g_pci_priv[i];
}

pci_device_t *pci_dev_mut(const pci_device_t *dev)
{
	int i = pci_dev_index(dev);

	/* Every pointer the PCI core hands out is into its table; anything
	 * else is a copy a driver made (NULL: the core's state cannot be
	 * written through it). */
	return i < 0 ? NULL : &g_pci_devices[i];
}

void pci_name(const pci_device_t *dev, char *out, size_t cap)
{
	if (!out || cap == 0)
		return;
	if (!dev) {
		ksnprintf(out, cap, "?");
		return;
	}
	ksnprintf(out, cap, "%04x:%02x:%02x.%x", dev->segment, dev->bus,
		  dev->device, dev->function);
}

void pci_dev_lock(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	might_sleep();
	BUG_ON(pp == NULL || !pp->lock_ready);
	mutex_lock(&pp->dev_lock);
}

void pci_dev_unlock(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	BUG_ON(pp == NULL || !pp->lock_ready);
	mutex_unlock(&pp->dev_lock);
}

int pci_dev_trylock(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	BUG_ON(pp == NULL || !pp->lock_ready);
	return mutex_trylock(&pp->dev_lock);
}

pci_device_t *pci_get_domain_bus_and_slot(uint32_t segment, uint8_t bus,
					  uint8_t devfn)
{
	int n = __atomic_load_n(&g_pci_count, __ATOMIC_ACQUIRE);

	for (int i = 0; i < n; i++) {
		pci_device_t *p = &g_pci_devices[i];

		if (p->is_removed)
			continue;
		if (p->segment == segment && p->bus == bus &&
		    p->device == PCI_SLOT(devfn) &&
		    p->function == PCI_FUNC(devfn))
			return p;
	}
	return NULL;
}

int pci_is_bridge(const pci_device_t *dev)
{
	return dev && (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ||
		       dev->hdr_type == PCI_HEADER_TYPE_CARDBUS);
}

pci_device_t *pci_upstream_bridge_of(uint32_t segment, uint8_t bus)
{
	int n = __atomic_load_n(&g_pci_count, __ATOMIC_ACQUIRE);

	for (int i = 0; i < n; i++) {
		pci_device_t *p = &g_pci_devices[i];
		struct pci_dev_priv *pp = &g_pci_priv[i];

		if (p->is_removed || p->segment != segment || !pci_is_bridge(p))
			continue;
		if (pp->secondary == bus && pp->secondary != 0)
			return p;
	}
	return NULL;
}

int pci_bridge_children(const pci_device_t *bridge, pci_device_t **out,
			int max)
{
	int n = __atomic_load_n(&g_pci_count, __ATOMIC_ACQUIRE), k = 0;

	for (int i = 0; i < n && k < max; i++) {
		pci_device_t *p = &g_pci_devices[i];

		if (p->is_removed || p->bus_self != bridge)
			continue;
		out[k++] = p;
	}
	return k;
}

/* Read the bus numbers of a bridge into its private record. */
static void pci_read_bridge_buses(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint32_t buses = 0;

	if (!pp || !pci_is_bridge(dev))
		return;
	pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
		     PCI_PRIMARY_BUS, 4, &buses);
	if (PCI_POSSIBLE_ERROR(buses))
		buses = 0;
	pp->secondary = (uint8_t)(buses >> 8);
	pp->subordinate = (uint8_t)(buses >> 16);
}

/* Size of the configuration space: 4096 when a PCI Express (or PCI-X
 * mode 2) function answers at 0x100 with something that is not all-ones
 * and not an alias of offset 0. */
static uint16_t pci_cfg_space_size(pci_device_t *dev)
{
	uint32_t status = 0, hdr0 = 0;

	if (dev->segment == 0 && !pci_ecam_covers(0, dev->bus))
		return PCI_CFG_SPACE_SIZE;
	if (!pci_is_pcie(dev)) {
		uint8_t pcix = pci_find_next_capability(dev, 0, PCI_CAP_ID_PCIX);
		uint32_t xstat = 0;

		if (!pcix)
			return PCI_CFG_SPACE_SIZE;
		pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
			     pcix + 4, 4, &xstat);
		/* PCI-X status: 266/533 MHz capable means mode 2. */
		if (!(xstat & ((1u << 30) | (1u << 31))))
			return PCI_CFG_SPACE_SIZE;
	}
	if (pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
			 PCI_CFG_SPACE_SIZE, 4, &status) != PCIBIOS_SUCCESSFUL)
		return PCI_CFG_SPACE_SIZE;
	if (PCI_POSSIBLE_ERROR(status))
		return PCI_CFG_SPACE_SIZE;
	pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev), 0, 4, &hdr0);
	if (status == hdr0)
		return PCI_CFG_SPACE_SIZE;
	return PCI_CFG_SPACE_EXP_SIZE;
}

void pci_setup_device(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint32_t v = 0;
	uint16_t flags = 0;
	uint8_t devfn = pci_dev_devfn(dev);

	BUG_ON(pp == NULL);
	pci_raw_read(dev->segment, dev->bus, devfn, PCI_CLASS_REVISION, 4, &v);
	dev->revision = (uint8_t)v;
	dev->prog_if = (uint8_t)(v >> 8);
	dev->subclass = (uint8_t)(v >> 16);
	dev->class_code = (uint8_t)(v >> 24);
	dev->class_full = v >> 8;
	pci_raw_read(dev->segment, dev->bus, devfn, PCI_HEADER_TYPE, 1, &v);
	dev->hdr_type = (uint8_t)(v & PCI_HEADER_TYPE_MASK);
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		pci_raw_read(dev->segment, dev->bus, devfn,
			     PCI_SUBSYSTEM_VENDOR_ID, 4, &v);
		dev->subsystem_vendor = (uint16_t)v;
		dev->subsystem_device = (uint16_t)(v >> 16);
	}
	dev->cfg_size = PCI_CFG_SPACE_SIZE;
	dev->pm_cap = pci_find_next_capability(dev, 0, PCI_CAP_ID_PM);
	dev->msi_cap = pci_find_next_capability(dev, 0, PCI_CAP_ID_MSI);
	dev->msix_cap = pci_find_next_capability(dev, 0, PCI_CAP_ID_MSIX);
	dev->af_cap = pci_find_next_capability(dev, 0, PCI_CAP_ID_AF);
	dev->pcie_cap = pci_find_next_capability(dev, 0, PCI_CAP_ID_EXP);
	if (dev->pcie_cap) {
		pci_raw_read(dev->segment, dev->bus, devfn,
			     dev->pcie_cap + PCI_EXP_FLAGS, 2, &v);
		flags = (uint16_t)v;
		dev->pcie_flags_type = (uint8_t)((flags & PCI_EXP_FLAGS_TYPE) >> 4);
	}
	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		uint8_t ssvid = pci_find_next_capability(dev, 0,
							 PCI_CAP_ID_SSVID);

		if (ssvid) {
			pci_raw_read(dev->segment, dev->bus, devfn,
				     ssvid + PCI_SSVID_VENDOR_ID, 4, &v);
			dev->subsystem_vendor = (uint16_t)v;
			dev->subsystem_device = (uint16_t)(v >> 16);
		}
		pci_read_bridge_buses(dev);
	}
	dev->cfg_size = pci_cfg_space_size(dev);
	dev->acs_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ACS);
	dev->l1ss_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_L1SS);
	dev->ltr_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR);
	dev->dpc_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC);
	if (dev->pcie_cap) {
		uint16_t lnkctl = 0;

		pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnkctl);
		pp->aspm_lnkctl_orig = lnkctl;
	}
	pci_msi_init_dev(dev);
	pci_pm_init_dev(dev);
	pci_init_reset_methods(dev);
	pci_name(dev, pp->name, sizeof(pp->name));
}

/* A free slot (never used, or a removed device nobody holds any more);
 * the table lock is held. */
static int pci_table_slot_locked(void)
{
	if (g_pci_count < PCI_MAX_DEVICES)
		return g_pci_count;
	for (int i = 0; i < PCI_MAX_DEVICES; i++) {
		pci_device_t *p = &g_pci_devices[i];

		if (p->is_removed && p->driver == NULL && !g_pci_priv[i].msi_enabled &&
		    !g_pci_priv[i].msix_enabled && p->irq_type == 0)
			return i;
	}
	return -1;
}

/* Record (segment, bus, devfn); the caller made sure something answers.
 * Port I/O only for segment 0 (works before ECAM and ACPI). */
static pci_device_t *pci_record(uint32_t segment, uint8_t bus, uint8_t devfn,
				uint32_t id)
{
	struct pci_dev_priv *pp;
	pci_device_t *p;
	uint64_t fl;
	int idx, append;
	uint32_t v = 0;

	spin_lock_irqsave(&g_pci_table_lock, &fl);
	idx = pci_table_slot_locked();
	if (idx < 0) {
		spin_unlock_irqrestore(&g_pci_table_lock, fl);
		kprintf("PCI: device table full, %04x:%02x:%02x.%x ignored\n",
			segment, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
		return NULL;
	}
	append = idx == g_pci_count;
	p = &g_pci_devices[idx];
	pp = &g_pci_priv[idx];
	/* A reused slot: the old record is dead (removed, unbound). */
	mm_memset(p, 0, sizeof(*p));
	/* Nobody holds (or waits for) the lock of a dead record. */
	WARN_ON(pp->lock_ready && mutex_is_locked(&pp->dev_lock));
	mm_memset(pp, 0, sizeof(*pp));
	mutex_init(&pp->dev_lock, "pci_dev");
	pp->lock_ready = 1;
	p->segment = segment;
	p->bus = bus;
	p->device = PCI_SLOT(devfn);
	p->function = PCI_FUNC(devfn);
	p->vendor_id = (uint16_t)id;
	p->device_id = (uint16_t)(id >> 16);
	p->error_state = pci_channel_io_normal;
	p->current_state = PCI_UNKNOWN;
	p->d3hot_delay_ms = PCI_PM_D3HOT_WAIT;
	pp->in_table = 1;
	pp->intx_gsi = -1;
	if (append)
		__atomic_store_n(&g_pci_count, g_pci_count + 1,
				 __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_pci_table_lock, fl);

	for (int i = 0; i < 6; i++) {
		pci_raw_read(segment, bus, devfn, PCI_BASE_ADDRESS_0 + i * 4, 4,
			     &v);
		p->bar[i] = v;
	}
	pci_raw_read(segment, bus, devfn, PCI_INTERRUPT_LINE, 2, &v);
	p->interrupt_line = (uint8_t)v;
	p->interrupt_pin = (uint8_t)(v >> 8);
	pci_setup_device(p);
	if (bus != 0 || segment >= PCI_VMD_DOMAIN_BASE)
		p->bus_self = pci_upstream_bridge_of(segment, bus);
	return p;
}

pci_device_t *pci_scan_single_device(uint32_t segment, uint8_t bus,
				     uint8_t devfn)
{
	uint32_t id = PCI_ERROR_RESPONSE;
	pci_device_t *p;

	p = pci_get_domain_bus_and_slot(segment, bus, devfn);
	if (p)
		return p;
	if (pci_raw_read(segment, bus, devfn, PCI_VENDOR_ID, 4, &id) !=
	    PCIBIOS_SUCCESSFUL)
		return NULL;
	if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0 ||
	    (id & 0xFFFF) == 0x0001 /* CRS answer */)
		return NULL;
	p = pci_record(segment, bus, devfn, id);
	/* Found after boot (hot-plug, a VMD domain): its BARs are sized
	 * now, before any driver can own it, and it gets its /sys
	 * directory. */
	if (p) {
		pci_bars_ensure(p);
		sysfs_add_pci(p);
	}
	return p;
}

/* Give an unconfigured bridge bus numbers [*next_bus, max_bus]; the
 * subordinate is narrowed after the scan below it. */
static int pci_bridge_assign_busnr(pci_device_t *br, unsigned int *next_bus,
				   unsigned int max_bus)
{
	struct pci_dev_priv *pp = pci_priv(br);
	uint32_t buses = 0;
	uint8_t devfn = pci_dev_devfn(br);

	if (*next_bus > max_bus)
		return -ENOSPC;
	pci_raw_read(br->segment, br->bus, devfn, PCI_PRIMARY_BUS, 4, &buses);
	buses &= 0xFF000000u; /* keep the secondary latency timer */
	buses |= (uint32_t)br->bus | ((uint32_t)*next_bus << 8) |
		 ((uint32_t)max_bus << 16);
	pci_raw_write(br->segment, br->bus, devfn, PCI_PRIMARY_BUS, 4, buses);
	pp->secondary = (uint8_t)*next_bus;
	pp->subordinate = (uint8_t)max_bus;
	(*next_bus)++;
	return 0;
}

int pci_scan_bus(uint32_t segment, uint8_t bus, int assign_busnr,
		 unsigned int *next_bus, unsigned int max_bus)
{
	int added = 0;

	if (segment != 0 && segment < PCI_VMD_DOMAIN_BASE)
		pci_ecam_map_bus(segment, bus);
	else if (segment == 0 && pci_ecam_covers(0, bus))
		pci_ecam_map_bus(0, bus);
	for (unsigned int slot = 0; slot < 32; slot++) {
		uint32_t hdr = 0;
		int nfn = 1;

		for (int fn = 0; fn < nfn; fn++) {
			uint8_t devfn = (uint8_t)PCI_DEVFN(slot, fn);
			pci_device_t *p = pci_get_domain_bus_and_slot(segment, bus,
								      devfn);
			int is_new = p == NULL;

			if (!p)
				p = pci_scan_single_device(segment, bus, devfn);
			if (!p) {
				if (fn == 0)
					break;
				continue;
			}
			if (is_new)
				added++;
			if (fn == 0) {
				pci_raw_read(segment, bus, devfn,
					     PCI_HEADER_TYPE, 1, &hdr);
				if (hdr & PCI_HEADER_TYPE_MFD)
					nfn = 8;
			}
			if (pci_is_bridge(p)) {
				struct pci_dev_priv *pp = pci_priv(p);

				pci_read_bridge_buses(p);
				if ((pp->secondary == 0 ||
				     pp->secondary <= bus) &&
				    assign_busnr && next_bus) {
					if (pci_bridge_assign_busnr(p, next_bus,
								    max_bus))
						continue;
				}
				if (pp->secondary > bus) {
					unsigned int nb = next_bus ? *next_bus :
								     0;

					added += pci_scan_bus(segment,
							      pp->secondary,
							      assign_busnr,
							      next_bus, max_bus);
					/* Narrow the subordinate to what the
					 * scan below actually used. */
					if (assign_busnr && next_bus &&
					    *next_bus > nb) {
						uint32_t b = 0;

						pci_raw_read(segment, bus, devfn,
							     PCI_PRIMARY_BUS, 4,
							     &b);
						b = (b & 0xFF00FFFFu) |
						    ((*next_bus - 1) << 16);
						pci_raw_write(segment, bus,
							      devfn,
							      PCI_PRIMARY_BUS,
							      4, b);
						pp->subordinate =
							(uint8_t)(*next_bus -
								  1);
					}
				}
			}
		}
	}
	return added;
}

int pci_enumerate(void)
{
	g_pci_count = 0;
	/* Every bus number: the firmware configured the bridges, and the
	 * brute-force walk finds devices below bridges the scan would not
	 * reach (hidden or misnumbered ones) exactly as before. */
	for (unsigned int bus = 0; bus < 256; ++bus) {
		for (unsigned char dev = 0; dev < 32; ++dev) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			if ((id & 0xFFFF) == 0xFFFF) {
				continue;
			}
			unsigned int header = pci_cfg_read32(bus, dev, 0, 0x0C);
			unsigned char multifunction =
				((header >> 16) & 0x80) ? 1 : 0;
			pci_record(0, (uint8_t)bus, (uint8_t)PCI_DEVFN(dev, 0),
				   id);
			if (multifunction) {
				for (unsigned char func = 1; func < 8; ++func) {
					unsigned int idf = pci_cfg_read32(
						bus, dev, func, 0x00);
					if ((idf & 0xFFFF) == 0xFFFF) {
						continue;
					}
					pci_record(0, (uint8_t)bus,
						   (uint8_t)PCI_DEVFN(dev, func),
						   idf);
				}
			}
		}
	}
	/* Upstream bridges once all bridges are known. */
	for (int i = 0; i < g_pci_count; i++) {
		pci_device_t *p = &g_pci_devices[i];

		if (p->bus != 0)
			p->bus_self = pci_upstream_bridge_of(0, p->bus);
	}
	kprintf("PCI: %d devices found\n", g_pci_count);
	return g_pci_count;
}

void pci_dev_forget(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	if (!pp)
		return;
	__atomic_store_n(&dev->is_removed, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&dev->error_state, pci_channel_io_perm_failure,
			 __ATOMIC_RELEASE);
	/* Hide it from the class/ID matches of the boot-time drivers. */
	dev->class_code = 0xFF;
	dev->subclass = 0xFF;
	dev->prog_if = 0xFF;
	dev->class_full = 0xFFFFFF;
}

const pci_device_t *pci_get_devices(int *count)
{
	BUG_ON(count == NULL);
	if (count) {
		*count = __atomic_load_n(&g_pci_count, __ATOMIC_ACQUIRE);
	}
	return g_pci_devices;
}

const pci_device_t *pci_get_first_xhci(void)
{
	return pci_get_xhci(0);
}

const pci_device_t *pci_get_xhci(int index)
{
	int found = 0;
	for (int i = 0; i < g_pci_count; i++) {
		const pci_device_t *p = &g_pci_devices[i];
		if (p->segment != 0 || p->is_removed)
			continue;
		if (p->class_code == 0x0C && p->subclass == 0x03 &&
		    p->prog_if == 0x30) {
			if (found == index)
				return p;
			found++;
		}
	}
	return 0;
}

const pci_device_t *pci_find_bridge_for_bus(unsigned char target_bus)
{
	if (target_bus == 0)
		return 0; // Bus 0 has no parent bridge
	for (int i = 0; i < g_pci_count; i++) {
		const pci_device_t *p = &g_pci_devices[i];
		if (p->segment != 0 || p->is_removed)
			continue;
		// PCI-to-PCI bridge: class 0x06, subclass 0x04
		if (p->class_code == 0x06 && p->subclass == 0x04) {
			// Config register 0x18: primary(7:0), secondary(15:8), subordinate(23:16)
			unsigned int buses = pci_cfg_read32(p->bus, p->device,
							    p->function, 0x18);
			unsigned char secondary = (buses >> 8) & 0xFF;
			unsigned char subordinate = (buses >> 16) & 0xFF;
			if (target_bus >= secondary &&
			    target_bus <= subordinate)
				return p;
		}
	}
	return 0;
}

const pci_device_t *pci_find_bdf(unsigned char bus, unsigned char device,
				 unsigned char function)
{
	for (int i = 0; i < g_pci_count; i++) {
		const pci_device_t *p = &g_pci_devices[i];
		if (p->segment != 0 || p->is_removed)
			continue;
		if (p->bus == bus && p->device == device &&
		    p->function == function)
			return p;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
//  PCI Capability list walking
// ---------------------------------------------------------------------------

uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id)
{
	if (!dev)
		return 0;

	// Check Status register bit 4 (Capabilities List)
	uint32_t status =
		pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
	if (!((status >> 16) & (1 << 4))) // Status is upper 16 bits of reg 0x04
		return 0;

	// Capabilities pointer is at config offset 0x34, low 8 bits
	uint32_t raw =
		pci_cfg_read32(dev->bus, dev->device, dev->function, 0x34);
	uint8_t ptr = (uint8_t)(raw & 0xFC); // Must be DWORD-aligned

	// Walk the linked list (max 48 entries to avoid infinite loops)
	for (int i = 0; i < 48 && ptr != 0; i++) {
		uint32_t cap_hdr = pci_cfg_read32(dev->bus, dev->device,
						  dev->function, ptr);
		uint8_t id = (uint8_t)(cap_hdr & 0xFF);
		uint8_t next = (uint8_t)((cap_hdr >> 8) & 0xFC);

		if (id == cap_id)
			return ptr;

		ptr = next;
	}
	return 0;
}

// ---------------------------------------------------------------------------
//  PCI MSI setup
// ---------------------------------------------------------------------------

int pci_enable_msi(const pci_device_t *dev, uint8_t vector)
{
	return pci_enable_msi_cpu(dev, vector, lapic_get_id_cpuid());
}

int pci_enable_msi_cpu(const pci_device_t *dev, uint8_t vector,
		       uint32_t apic_id)
{
	BUG_ON(dev == NULL);
	WARN_ON(vector <
		32); /* MSI vector must be >= 32 (0-31 are CPU exceptions) */
	uint8_t cap_off = pci_find_capability(dev, PCI_CAP_MSI);
	if (cap_off == 0) {
		kprintf("PCI MSI: no MSI capability found for %02x:%02x.%x\n",
			dev->bus, dev->device, dev->function);
		return -1;
	}

	// Read Message Control register (cap_off + 2, upper 16 bits of DWORD)
	uint32_t cap_dw0 =
		pci_cfg_read32(dev->bus, dev->device, dev->function, cap_off);
	uint16_t msg_ctrl = (uint16_t)(cap_dw0 >> 16);
	int is_64bit = (msg_ctrl >> 7) & 1; // Bit 7: 64-bit address capable

	// Disable MSI while programming (clear bit 0 of Message Control)
	cap_dw0 &= ~(
		1U
		<< 16); // Clear MSI Enable bit (bit 0 of msg_ctrl, which is bit 16 of DWORD)
	pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off, cap_dw0);

	// Program Message Address (cap_off + 4)
	// Use CPUID-based APIC ID — safe to call before lapic_init(), unlike
	// lapic_get_id() which needs x2APIC mode detection to be set up first.
	uint32_t bsp_apic_id = apic_id;
	uint32_t msi_addr = MSI_ADDR_BASE | ((bsp_apic_id & 0xFF) << 12);
	pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off + 0x04,
			msi_addr);

	// If 64-bit capable, zero upper address and write data at cap_off + 0x0C
	// If 32-bit, data is at cap_off + 0x08
	uint8_t data_off;
	if (is_64bit) {
		pci_cfg_write32(dev->bus, dev->device, dev->function,
				cap_off + 0x08, 0);
		data_off = cap_off + 0x0C;
	} else {
		data_off = cap_off + 0x08;
	}

	// Program Message Data: vector number, edge-triggered, fixed delivery
	// Data register is in the lower 16 bits of its DWORD
	uint32_t data_dw =
		pci_cfg_read32(dev->bus, dev->device, dev->function, data_off);
	data_dw = (data_dw & 0xFFFF0000) | (uint16_t)vector;
	pci_cfg_write32(dev->bus, dev->device, dev->function, data_off,
			data_dw);

	// Set Multiple Message Enable to 0 (allocate 1 vector) and enable MSI
	// Re-read cap DWORD, set bit 0 of msg_ctrl (MSI Enable), clear bits 4:6 (MME = 0)
	cap_dw0 = pci_cfg_read32(dev->bus, dev->device, dev->function, cap_off);
	cap_dw0 &= ~(0x0070U << 16); // Clear MME bits (bits 4-6 of msg_ctrl)
	cap_dw0 |= (1U << 16); // Set MSI Enable (bit 0 of msg_ctrl)
	pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off, cap_dw0);

	// Disable legacy INTx in PCI Command register (bit 10)
	uint32_t cmd =
		pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
	cmd |= PCI_CMD_INTX_DISABLE;
	pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04, cmd);

	kprintf("PCI MSI: enabled for %02x:%02x.%x - vector %d, addr 0x%x (APIC %d), %s\n",
		dev->bus, dev->device, dev->function, vector, msi_addr,
		bsp_apic_id, is_64bit ? "64-bit" : "32-bit");
	return 0;
}

/* MSI-X: a table of (address, data) entries in one of the device's BARs,
 * each its own interrupt.  Programs entry `entry' for `vector' on the CPU
 * with `apic_id', unmasks it, enables the MSI-X function, and disables
 * INTx. */
int pci_enable_msix(const pci_device_t *dev, int entry, uint8_t vector,
		    uint32_t apic_id)
{
	BUG_ON(dev == NULL);
	uint8_t cap = pci_find_capability(dev, PCI_CAP_MSIX);

	if (cap == 0)
		return -1;

	uint32_t dw0 = pci_cfg_read32(dev->bus, dev->device, dev->function, cap);
	uint16_t ctrl = (uint16_t)(dw0 >> 16);
	int table_size = (ctrl & 0x7FF) + 1;

	if (entry < 0 || entry >= table_size)
		return -1;

	uint32_t table = pci_cfg_read32(dev->bus, dev->device, dev->function,
					cap + 4);
	unsigned bir = table & 0x7;
	uint64_t table_off = table & ~0x7u;

	if (bir > 5)
		return -1;
	uint64_t bar = dev->bar[bir];
	if (bar & 1)
		return -1; /* I/O BAR cannot hold a table */
	uint64_t base = bar & ~0xFULL;
	if (((bar >> 1) & 0x3) == 0x2 && bir < 5) /* 64-bit BAR */
		base |= (uint64_t)dev->bar[bir + 1] << 32;
	if (!base)
		return -1;

	uint64_t ent_phys = base + table_off + (uint64_t)entry * 16;
	uint64_t page = ent_phys & ~0xFFFULL;
	uint64_t virt = mm_map_device_mmio(page, 2);
	if (!virt)
		return -1;
	volatile uint32_t *e = (volatile uint32_t *)(virt + (ent_phys - page));

	/* Mask the entry while it is programmed, then unmask. */
	e[3] |= 1;
	e[0] = MSI_ADDR_BASE | ((apic_id & 0xFF) << 12);
	e[1] = 0;
	e[2] = vector; /* edge, fixed delivery */
	e[3] &= ~1u;

	/* Function mask off, MSI-X enable on. */
	dw0 = pci_cfg_read32(dev->bus, dev->device, dev->function, cap);
	dw0 &= ~(1u << 30); /* function mask */
	dw0 |= (1u << 31); /* enable */
	pci_cfg_write32(dev->bus, dev->device, dev->function, cap, dw0);

	uint32_t cmd = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
	cmd |= PCI_CMD_INTX_DISABLE;
	pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04, cmd);

	kprintf("PCI MSI-X: %02x:%02x.%x entry %d -> vector %d (APIC %u), table of %d\n",
		dev->bus, dev->device, dev->function, entry, vector, apic_id,
		table_size);
	return 0;
}

/* ==========================================================================
 * Capabilities
 * ========================================================================== */

/* Readers for the walkers: raw (the scan, before the record is complete)
 * and through the record (honours cfg_size, removal). */
static int pci_capwalk_raw_rd(void *ctx, int where, uint32_t *val)
{
	const pci_device_t *dev = ctx;

	return pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev), where,
			    4, val);
}

static int pci_capwalk_dev_rd(void *ctx, int where, uint32_t *val)
{
	return pci_read_config_dword((const pci_device_t *)ctx, where, val);
}

uint8_t pci_find_next_capability(const pci_device_t *dev, uint8_t pos,
				 int cap)
{
	uint32_t v = 0;
	int head;

	if (!dev)
		return 0;
	if (pos == 0) {
		/* Status bit 4: the list exists at all. */
		pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
			     PCI_STATUS, 2, &v);
		if (PCI_POSSIBLE_ERROR((uint16_t)v) ||
		    !(v & PCI_STATUS_CAP_LIST))
			return 0;
	}
	pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
		     PCI_HEADER_TYPE, 1, &v);
	head = (v & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_CARDBUS ?
		       0x14 :
		       PCI_CAPABILITY_LIST;
	return pci_capwalk_std(pci_capwalk_raw_rd, (void *)dev, head, pos, cap);
}

uint16_t pci_find_next_ext_capability(const pci_device_t *dev, uint16_t start,
				      int cap)
{
	if (!dev)
		return 0;
	return pci_capwalk_ext(pci_capwalk_dev_rd, (void *)dev, dev->cfg_size,
			       start, cap);
}

uint16_t pci_find_ext_capability(const pci_device_t *dev, int cap)
{
	return pci_find_next_ext_capability(dev, 0, cap);
}

/* ==========================================================================
 * PCI Express capability accessors
 * ========================================================================== */

static inline int pcie_cap_version(const pci_device_t *dev)
{
	uint16_t flags = 0;

	pci_read_config_word(dev, dev->pcie_cap + PCI_EXP_FLAGS, &flags);
	return flags & PCI_EXP_FLAGS_VERS;
}

static int pcie_downstream_port(const pci_device_t *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ROOT_PORT ||
	       type == PCI_EXP_TYPE_DOWNSTREAM ||
	       type == PCI_EXP_TYPE_PCIE_BRIDGE;
}

static int pcie_cap_has_lnkctl(const pci_device_t *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ENDPOINT || type == PCI_EXP_TYPE_LEG_END ||
	       type == PCI_EXP_TYPE_ROOT_PORT || type == PCI_EXP_TYPE_UPSTREAM ||
	       type == PCI_EXP_TYPE_DOWNSTREAM ||
	       type == PCI_EXP_TYPE_PCI_BRIDGE ||
	       type == PCI_EXP_TYPE_PCIE_BRIDGE;
}

static int pcie_cap_has_sltctl(const pci_device_t *dev)
{
	uint16_t flags = 0;

	if (!pcie_downstream_port(dev))
		return 0;
	pci_read_config_word(dev, dev->pcie_cap + PCI_EXP_FLAGS, &flags);
	return (flags & PCI_EXP_FLAGS_SLOT) != 0;
}

static int pcie_cap_has_rtctl(const pci_device_t *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ROOT_PORT || type == PCI_EXP_TYPE_RC_EC;
}

/* Does the (possibly version 1) capability implement register `pos'? */
static int pcie_capability_reg_implemented(const pci_device_t *dev, int pos)
{
	if (!pci_is_pcie(dev))
		return 0;
	switch (pos) {
	case PCI_EXP_FLAGS:
		return 1;
	case PCI_EXP_DEVCAP:
	case PCI_EXP_DEVCTL:
	case PCI_EXP_DEVSTA:
		return 1;
	case PCI_EXP_LNKCAP:
	case PCI_EXP_LNKCTL:
	case PCI_EXP_LNKSTA:
		return pcie_cap_has_lnkctl(dev);
	case PCI_EXP_SLTCAP:
	case PCI_EXP_SLTCTL:
	case PCI_EXP_SLTSTA:
		return pcie_cap_has_sltctl(dev);
	case PCI_EXP_RTCTL:
	case PCI_EXP_RTCAP:
	case PCI_EXP_RTSTA:
		return pcie_cap_has_rtctl(dev);
	case PCI_EXP_DEVCAP2:
	case PCI_EXP_DEVCTL2:
	case PCI_EXP_DEVSTA2:
	case PCI_EXP_LNKCAP2:
	case PCI_EXP_LNKCTL2:
	case PCI_EXP_LNKSTA2:
	case PCI_EXP_SLTCAP2:
	case PCI_EXP_SLTCTL2:
	case PCI_EXP_SLTSTA2:
		return pcie_cap_version(dev) > 1;
	default:
		return 0;
	}
}

int pcie_capability_read_word(const pci_device_t *dev, int pos, uint16_t *val)
{
	int ret;

	*val = 0;
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (pcie_capability_reg_implemented(dev, pos)) {
		ret = pci_read_config_word(dev, dev->pcie_cap + pos, val);
		/* A failed read reads as 0 for these registers. */
		if (ret)
			*val = 0;
		return ret;
	}
	/* Version 1 downstream ports without a slot: emulate "presence
	 * detected" so a caller does not take an empty port for an empty
	 * slot. */
	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTSTA)
		*val = PCI_EXP_SLTSTA_PDS;
	return 0;
}

int pcie_capability_read_dword(const pci_device_t *dev, int pos,
			       uint32_t *val)
{
	int ret;

	*val = 0;
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (pcie_capability_reg_implemented(dev, pos)) {
		ret = pci_read_config_dword(dev, dev->pcie_cap + pos, val);
		if (ret)
			*val = 0;
		return ret;
	}
	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTCTL)
		*val = PCI_EXP_SLTSTA_PDS << 16;
	return 0;
}

int pcie_capability_write_word(const pci_device_t *dev, int pos, uint16_t val)
{
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_capability_reg_implemented(dev, pos))
		return 0;
	return pci_write_config_word(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_write_dword(const pci_device_t *dev, int pos,
				uint32_t val)
{
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (!pcie_capability_reg_implemented(dev, pos))
		return 0;
	return pci_write_config_dword(dev, dev->pcie_cap + pos, val);
}

/* Read-modify-write of the control registers, serialised (the link and
 * slot control registers are shared by several users -- ASPM, hot-plug,
 * the bandwidth controller). */
static spinlock_t g_pcie_cap_lock = SPINLOCK_INIT("pcie_cap");

int pcie_capability_clear_and_set_word(const pci_device_t *dev, int pos,
				       uint16_t clear, uint16_t set)
{
	uint16_t val;
	uint64_t fl;
	int ret;

	spin_lock_irqsave(&g_pcie_cap_lock, &fl);
	ret = pcie_capability_read_word(dev, pos, &val);
	if (!ret) {
		val &= ~clear;
		val |= set;
		ret = pcie_capability_write_word(dev, pos, val);
	}
	spin_unlock_irqrestore(&g_pcie_cap_lock, fl);
	return ret;
}

int pcie_capability_clear_and_set_dword(const pci_device_t *dev, int pos,
					uint32_t clear, uint32_t set)
{
	uint32_t val;
	uint64_t fl;
	int ret;

	spin_lock_irqsave(&g_pcie_cap_lock, &fl);
	ret = pcie_capability_read_dword(dev, pos, &val);
	if (!ret) {
		val &= ~clear;
		val |= set;
		ret = pcie_capability_write_dword(dev, pos, val);
	}
	spin_unlock_irqrestore(&g_pcie_cap_lock, fl);
	return ret;
}

int pcie_link_status(const pci_device_t *dev, unsigned int *speed,
		     unsigned int *width)
{
	uint16_t lnksta = 0;

	if (!dev || !pci_is_pcie(dev) || !pcie_cap_has_lnkctl(dev))
		return -ENODEV;
	if (pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta))
		return -EIO;
	if (speed)
		*speed = lnksta & PCI_EXP_LNKSTA_CLS;
	if (width)
		*width = (lnksta & PCI_EXP_LNKSTA_NLW) >>
			 PCI_EXP_LNKSTA_NLW_SHIFT;
	return 0;
}

/* The highest supported speed: the Supported Link Speeds vector of
 * LNKCAP2 when it has one, else LNKCAP's Max Link Speed. */
static unsigned int pcie_get_speed_cap(const pci_device_t *dev)
{
	uint32_t lnkcap2 = 0, lnkcap = 0;

	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2);
	if (lnkcap2 & PCI_EXP_LNKCAP2_SLS) {
		uint32_t sls = (lnkcap2 & PCI_EXP_LNKCAP2_SLS) >> 1;

		/* Bit n-1 of the vector = speed n. */
		return 32u - (unsigned int)__builtin_clz(sls);
	}
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	return lnkcap & PCI_EXP_LNKCAP_SLS;
}

int pcie_link_capability(const pci_device_t *dev, unsigned int *max_speed,
			 unsigned int *max_width)
{
	uint32_t lnkcap = 0;

	if (!dev || !pci_is_pcie(dev) || !pcie_cap_has_lnkctl(dev))
		return -ENODEV;
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (max_speed)
		*max_speed = pcie_get_speed_cap(dev);
	if (max_width)
		*max_width = (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4;
	return 0;
}

/* Mb/s per lane after encoding overhead, by PCI_EXP_LNKSTA_CLS value. */
static uint32_t pcie_speed_mbps(unsigned int speed)
{
	switch (speed) {
	case 1:
		return 2500 * 8 / 10;
	case 2:
		return 5000 * 8 / 10;
	case 3:
		return 8000 * 128 / 130;
	case 4:
		return 16000 * 128 / 130;
	case 5:
		return 32000 * 128 / 130;
	case 6:
		return 64000 * 242 / 256;
	default:
		return 0;
	}
}

uint32_t pcie_bandwidth_available(const pci_device_t *dev)
{
	uint32_t bw = 0;
	const pci_device_t *p = dev;
	int depth = 0;

	while (p && depth++ < 32) {
		unsigned int speed = 0, width = 0;

		if (pci_is_pcie(p) && pcie_cap_has_lnkctl(p) &&
		    !pcie_link_status(p, &speed, &width)) {
			uint32_t this_bw = width * pcie_speed_mbps(speed);

			if (this_bw && (!bw || this_bw < bw))
				bw = this_bw;
		}
		p = p->bus_self;
	}
	return bw;
}

int pcie_get_mps(const pci_device_t *dev)
{
	uint16_t ctl = 0;

	if (!pci_is_pcie(dev))
		return -ENODEV;
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	return 128 << ((ctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5);
}

int pcie_set_mps(const pci_device_t *dev, int mps)
{
	uint32_t devcap = 0;
	int v;

	if (!pci_is_pcie(dev))
		return -ENODEV;
	if (mps < 128 || mps > 4096 || (mps & (mps - 1)))
		return -EINVAL;
	v = __builtin_ctz((unsigned int)mps) - 7;
	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &devcap);
	if (v > (int)(devcap & PCI_EXP_DEVCAP_PAYLOAD))
		return -EINVAL;
	return pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_PAYLOAD,
						  (uint16_t)(v << 5)) ?
		       -EIO :
		       0;
}

int pcie_get_readrq(const pci_device_t *dev)
{
	uint16_t ctl = 0;

	if (!pci_is_pcie(dev))
		return -ENODEV;
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	return 128 << ((ctl & PCI_EXP_DEVCTL_READRQ) >> 12);
}

int pcie_set_readrq(const pci_device_t *dev, int rq)
{
	uint16_t v;

	if (!pci_is_pcie(dev))
		return -ENODEV;
	if (rq < 128 || rq > 4096 || (rq & (rq - 1)))
		return -EINVAL;
	v = (uint16_t)((__builtin_ctz((unsigned int)rq) - 7) << 12);
	return pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_READRQ, v) ?
		       -EIO :
		       0;
}

/* ASPM: turn the requested states off on both ends of the link (the
 * upstream port's LNKCTL controls the link of a device below it, and the
 * device's own end must agree); L1 substates in the L1 PM Substates
 * capability of both.  There is no policy that turns ASPM back on: what
 * the firmware set stays, minus what a driver turned off. */
static void pcie_aspm_disable_one(pci_device_t *dev, uint32_t state)
{
	uint16_t clear = 0;

	if (!pci_is_pcie(dev) || !pcie_cap_has_lnkctl(dev))
		return;
	if (state & PCIE_LINK_STATE_L0S)
		clear |= PCI_EXP_LNKCTL_ASPM_L0S;
	if (state & PCIE_LINK_STATE_L1)
		clear |= PCI_EXP_LNKCTL_ASPM_L1;
	if (state & PCIE_LINK_STATE_CLKPM)
		clear |= PCI_EXP_LNKCTL_CLKREQ_EN;
	/* L1 substates first: they must not stay enabled without L1. */
	if (dev->l1ss_cap &&
	    (state & (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_1 |
		      PCIE_LINK_STATE_L1_2))) {
		uint32_t l1clear = 0, ctl1 = 0;

		if (state & (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_1))
			l1clear |= PCI_L1SS_CTL1_ASPM_L1_1 |
				   PCI_L1SS_CTL1_PCIPM_L1_1;
		if (state & (PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_L1_2))
			l1clear |= PCI_L1SS_CTL1_ASPM_L1_2 |
				   PCI_L1SS_CTL1_PCIPM_L1_2;
		pci_read_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL1, &ctl1);
		if (ctl1 & l1clear)
			pci_write_config_dword(dev, dev->l1ss_cap + PCI_L1SS_CTL1,
					       ctl1 & ~l1clear);
	}
	if (clear)
		pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL, clear,
						   0);
}

int pci_disable_link_state(const pci_device_t *cdev, uint32_t state)
{
	pci_device_t *dev = pci_dev_mut(cdev);
	pci_device_t *parent;

	if (!dev || !pci_is_pcie(dev))
		return -EINVAL;
	parent = dev->bus_self;
	/* The upstream end first (the spec's order for disabling). */
	if (parent)
		pcie_aspm_disable_one(parent, state);
	pcie_aspm_disable_one(dev, state);
	if (state & (PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1))
		dev->flags |= PCI_DEV_FL_ASPM_DISABLED;
	return 0;
}

int pcie_aspm_enabled(const pci_device_t *dev)
{
	uint16_t ctl = 0;
	const pci_device_t *p;

	if (!dev || !pci_is_pcie(dev))
		return 0;
	p = dev->bus_self ? dev->bus_self : dev;
	pcie_capability_read_word(p, PCI_EXP_LNKCTL, &ctl);
	return (ctl & PCI_EXP_LNKCTL_ASPMC) != 0;
}

/* ---- narrow config access (the original helpers) --------------------- */

/* Segment 0 keeps the original dword port-I/O semantics exactly; a device
 * in another segment (a VMD domain) goes through its own accessors. */
uint32_t pci_cfg_read32_dev(const pci_device_t *dev, unsigned char off)
{
	if (dev->segment != 0) {
		uint32_t v;

		pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
			     off & 0xFC, 4, &v);
		return v;
	}
	return pci_cfg_read32(dev->bus, dev->device, dev->function, off);
}

void pci_cfg_write32_dev(const pci_device_t *dev, unsigned char off,
			 uint32_t v)
{
	if (dev->segment != 0) {
		pci_raw_write(dev->segment, dev->bus, pci_dev_devfn(dev),
			      off & 0xFC, 4, v);
		return;
	}
	pci_cfg_write32(dev->bus, dev->device, dev->function, off, v);
}

uint8_t pci_cfg_read8(const pci_device_t *dev, unsigned char off)
{
	uint32_t v = pci_cfg_read32_dev(dev, off & 0xFC);
	return (uint8_t)(v >> (8 * (off & 3)));
}

uint16_t pci_cfg_read16(const pci_device_t *dev, unsigned char off)
{
	uint32_t v = pci_cfg_read32_dev(dev, off & 0xFC);
	return (uint16_t)(v >> (8 * (off & 2)));
}

/* The dword is read and written under one hold of the config lock, so two
 * CPUs poking different bytes of the same dword cannot lose each other's
 * write. */
static void pci_cfg_rmw(const pci_device_t *dev, unsigned char off,
			uint32_t mask, uint32_t value)
{
	uint64_t flags;

	if (dev->segment != 0) {
		uint32_t v = 0;

		spin_lock_irqsave(&g_pci_cfg_lock, &flags);
		pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev),
			     off & 0xFC, 4, &v);
		v = (v & ~mask) | (value & mask);
		pci_raw_write(dev->segment, dev->bus, pci_dev_devfn(dev),
			      off & 0xFC, 4, v);
		spin_unlock_irqrestore(&g_pci_cfg_lock, flags);
		return;
	}
	unsigned int address =
		(unsigned int)((1u << 31) | ((unsigned int)dev->bus << 16) |
			       ((unsigned int)dev->device << 11) |
			       ((unsigned int)dev->function << 8) | (off & 0xFC));

	spin_lock_irqsave(&pci_lock, &flags);
	outl(PCI_CONFIG_ADDRESS, address);
	uint32_t v = inl(PCI_CONFIG_DATA);
	v = (v & ~mask) | (value & mask);
	outl(PCI_CONFIG_ADDRESS, address);
	outl(PCI_CONFIG_DATA, v);
	spin_unlock_irqrestore(&pci_lock, flags);
}

void pci_cfg_write8(const pci_device_t *dev, unsigned char off, uint8_t v)
{
	unsigned shift = 8 * (off & 3);
	pci_cfg_rmw(dev, off, 0xFFu << shift, (uint32_t)v << shift);
}

void pci_cfg_write16(const pci_device_t *dev, unsigned char off, uint16_t v)
{
	unsigned shift = 8 * (off & 2);
	pci_cfg_rmw(dev, off, 0xFFFFu << shift, (uint32_t)v << shift);
}

/* ==========================================================================
 * BARs
 * ========================================================================== */

/* Size a base address register: with the device's decoding paused, write
 * all ones, read back, restore.  The bits that stayed zero are the
 * window's alignment, which is its size. */
static uint64_t pci_size_register(const pci_device_t *dev, unsigned char off,
				  uint32_t orig, uint32_t typebits)
{
	pci_cfg_write32_dev(dev, off, 0xFFFFFFFFu);
	uint32_t v = pci_cfg_read32_dev(dev, off);
	pci_cfg_write32_dev(dev, off, orig);
	v &= ~typebits;
	if (!v)
		return 0;
	return (uint64_t)(~v + 1u); /* lowest set bit */
}

/* Every sizing of a BAR (it pauses decode and writes all ones) and every
 * read or update of the cached decode: one sizer at a time, and nobody
 * reads a half-written cache entry.  Nests outside the config locks. */
static spinlock_t g_pci_bars_lock = SPINLOCK_INIT("pci_bars");

static int pci_bar_decode_locked(const pci_device_t *dev, int index,
				 struct pci_bar *out)
{
	lockdep_assert_held(&g_pci_bars_lock);
	if (!dev || !out || index < 0 || index > 5)
		return -EINVAL;
	unsigned char off = (unsigned char)(0x10 + index * 4);
	/* The upper half of a 64-bit BAR is not a BAR of its own. */
	if (index > 0) {
		uint32_t prev = dev->bar[index - 1];
		if (!(prev & 1) && ((prev >> 1) & 3) == 2)
			return -EINVAL;
	}
	uint32_t lo = pci_cfg_read32_dev(dev, off);
	mm_memset(out, 0, sizeof(*out));

	/* Pause decoding while the register is probed: a write of all ones
	 * to a live BAR moves the window for the instant between the two
	 * config cycles, and a device that is being scanned out of during
	 * that instant (the boot framebuffer) shows it. */
	uint16_t cmd = pci_cfg_read16(dev, 0x04);
	uint16_t paused = cmd & (uint16_t)~0x3;
	if (paused != cmd)
		pci_cfg_write16(dev, 0x04, paused);

	if (lo & 1) {
		uint64_t size = pci_size_register(dev, off, lo, 0x3);
		if (size)
			size &= 0xFFFF; /* I/O windows are 16-bit */
		out->base = lo & ~0x3ULL;
		out->size = size;
		out->flags = PCI_BAR_IO;
	} else {
		int is64 = ((lo >> 1) & 3) == 2;
		uint32_t hi = 0;
		uint64_t size_lo = pci_size_register(dev, off, lo, 0xF);
		uint64_t size = size_lo;

		if (is64 && index < 5) {
			unsigned char offh = (unsigned char)(off + 4);
			hi = pci_cfg_read32_dev(dev, offh);
			pci_cfg_write32_dev(dev, offh, 0xFFFFFFFFu);
			uint32_t vh = pci_cfg_read32_dev(dev, offh);
			pci_cfg_write32_dev(dev, offh, hi);
			/* The low half sized to zero means the window is at
			 * least 4 GB: its size lives entirely in the high
			 * half. */
			if (size_lo == 0) {
				if (vh)
					size = (uint64_t)(~vh + 1u) << 32;
			}
		}
		out->base = (lo & ~0xFULL) | ((uint64_t)hi << 32);
		out->size = size;
		out->flags = is64 ? PCI_BAR_MEM64 : 0;
		if (lo & 0x8)
			out->flags |= PCI_BAR_PREFETCH;
	}
	if (paused != cmd)
		pci_cfg_write16(dev, 0x04, cmd);
	if (out->size == 0)
		return -ENOENT;
	return 0;
}

int pci_bar_decode(const pci_device_t *dev, int index, struct pci_bar *out)
{
	uint64_t fl;
	int rc;

	spin_lock_irqsave(&g_pci_bars_lock, &fl);
	rc = pci_bar_decode_locked(dev, index, out);
	spin_unlock_irqrestore(&g_pci_bars_lock, fl);
	return rc;
}

int pci_rom_decode(const pci_device_t *dev, struct pci_bar *out)
{
	if (!dev || !out)
		return -EINVAL;
	uint32_t orig = pci_cfg_read32_dev(dev, 0x30);
	mm_memset(out, 0, sizeof(*out));
	uint16_t cmd = pci_cfg_read16(dev, 0x04);
	uint16_t paused = cmd & (uint16_t)~0x3;
	if (paused != cmd)
		pci_cfg_write16(dev, 0x04, paused);
	/* Bit 0 is the enable; the address field starts at bit 11. */
	pci_cfg_write32_dev(dev, 0x30, 0xFFFFF800u | (orig & 1));
	uint32_t v = pci_cfg_read32_dev(dev, 0x30);
	pci_cfg_write32_dev(dev, 0x30, orig);
	if (paused != cmd)
		pci_cfg_write16(dev, 0x04, cmd);
	v &= 0xFFFFF800u;
	if (!v)
		return -ENOENT;
	out->base = orig & 0xFFFFF800u;
	out->size = (uint64_t)(~v + 1u);
	out->flags = 0;
	return 0;
}

/* The cached decode of every BAR.  Sized exactly once per function, with
 * no driver bound: a function found after boot (hot-plug, a VMD domain)
 * when it is recorded, a boot-time one only when a driver of the PCI core
 * is about to bind (pci_bars_ensure) -- the boot enumeration never sizes,
 * so the functions the boot-time drivers use (display, ISA bridge ...)
 * never see their decode paused.  Resized only where the BARs were
 * (re)assigned before any driver bound (a VMD domain's) --
 * pci_bars_refresh().  A hot-plug
 * assignment moves bases, never sizes, and updates the cache itself
 * (pci_write_bar).  A restore after a reset writes back the values the
 * cache describes, so the cache stays.  Readers never size: sizing pauses
 * the function's decode and writes all ones into a live register, which a
 * bound device must never see (a sysfs read of `resource' by anybody). */
static void pci_bars_size_locked(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);

	lockdep_assert_held(&g_pci_bars_lock);
	BUG_ON(pp == NULL);
	for (int i = 0; i < 6; i++) {
		mm_memset(&pp->bars[i], 0, sizeof(pp->bars[i]));
		pp->bar_bus[i] = 0;
	}
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		for (int i = 0; i < 6; i++) {
			struct pci_bar b;

			if (pci_bar_decode_locked(dev, i, &b) != 0)
				continue;
			pp->bar_bus[i] = b.base;
			/* A VMD domain's BARs hold bus addresses. */
			if (dev->segment >= PCI_VMD_DOMAIN_BASE &&
			    !(b.flags & PCI_BAR_IO))
				b.base = pci_vmd_bus_to_cpu(dev->segment, b.base);
			pp->bars[i] = b;
			if (b.flags & PCI_BAR_MEM64)
				i++;
		}
	}
	__atomic_store_n(&pp->bars_valid, 1, __ATOMIC_RELEASE);
}

/* First sizing of a function (called again for a sized one, which keeps
 * its cache).  Never a bridge-class function (class 0x06): sizing the
 * ISA/eSPI bridge corrupts the chipset's legacy I/O decode. */
void pci_bars_ensure(pci_device_t *dev)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	if (!dev || dev->class_code == 0x06)
		return;
	spin_lock_irqsave(&g_pci_bars_lock, &fl);
	if (pp && !pp->bars_valid)
		pci_bars_size_locked(dev);
	spin_unlock_irqrestore(&g_pci_bars_lock, fl);
}

/* The BARs were assigned anew before any driver bound: size them again.
 * Never for a function a driver owns (the sizing would pause its decode
 * under it); such a caller is a bug. */
void pci_bars_refresh(pci_device_t *dev)
{
	uint64_t fl;

	if (WARN(dev->driver != NULL,
		 "pci %04x:%02x:%02x.%x: BARs resized under a bound driver, skipped",
		 dev->segment, dev->bus, dev->device, dev->function))
		return;
	spin_lock_irqsave(&g_pci_bars_lock, &fl);
	pci_bars_size_locked(dev);
	spin_unlock_irqrestore(&g_pci_bars_lock, fl);
}

/* A copy of one cached BAR; `bus': the base as the bus sees it (a VMD
 * domain's), else as the CPU does.  -ENOENT: not implemented. */
static int pci_bar_cached(const pci_device_t *dev, int bar,
			  struct pci_bar *out, int bus)
{
	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	mm_memset(out, 0, sizeof(*out));
	if (!pp || bar < 0 || bar > 5)
		return -EINVAL;
	spin_lock_irqsave(&g_pci_bars_lock, &fl);
	if (pp->bars_valid) {
		*out = pp->bars[bar];
		if (bus)
			out->base = pp->bar_bus[bar];
	}
	spin_unlock_irqrestore(&g_pci_bars_lock, fl);
	return out->size ? 0 : -ENOENT;
}

uint64_t pci_resource_start(const pci_device_t *dev, int bar)
{
	struct pci_bar b;

	return pci_bar_cached(dev, bar, &b, 0) == 0 ? b.base : 0;
}

uint64_t pci_resource_len(const pci_device_t *dev, int bar)
{
	struct pci_bar b;

	return pci_bar_cached(dev, bar, &b, 0) == 0 ? b.size : 0;
}

uint32_t pci_resource_flags(const pci_device_t *dev, int bar)
{
	struct pci_bar b;

	return pci_bar_cached(dev, bar, &b, 0) == 0 ? b.flags : 0;
}

/* The mappings pci_iomap() made, so pci_iounmap() releases exactly the
 * pages it mapped (never a neighbour's). */
#define PCI_IOMAP_MAX 128
static struct {
	uint64_t virt; /* page-aligned */
	size_t pages;
	const pci_device_t *dev;
} g_iomaps[PCI_IOMAP_MAX];
static spinlock_t g_iomap_lock = SPINLOCK_INIT("pci_iomap");

static volatile void *pci_iomap_attr(pci_device_t *dev, int bar,
				     uint64_t maxlen, int attr)
{
	uint64_t start, len, page, off, virt, fl;
	size_t pages;
	int slot = -1;

	might_sleep();
	if (!dev || bar < 0 || bar > 5)
		return NULL;
	start = pci_resource_start(dev, bar);
	len = pci_resource_len(dev, bar);
	if (!start || !len || (pci_resource_flags(dev, bar) & PCI_BAR_IO))
		return NULL;
	if (maxlen && maxlen < len)
		len = maxlen;
	page = start & ~0xFFFULL;
	off = start - page;
	pages = (size_t)((off + len + 0xFFF) >> 12);
	virt = mm_map_mmio_flags(page, pages, attr);
	if (!virt)
		return NULL;
	spin_lock_irqsave(&g_iomap_lock, &fl);
	for (int i = 0; i < PCI_IOMAP_MAX; i++) {
		if (!g_iomaps[i].virt) {
			g_iomaps[i].virt = virt & ~0xFFFULL;
			g_iomaps[i].pages = pages;
			g_iomaps[i].dev = dev;
			slot = i;
			break;
		}
	}
	spin_unlock_irqrestore(&g_iomap_lock, fl);
	/* Not recorded: the mapping stays for good (never unmapped). */
	WARN_ON_ONCE(slot < 0);
	return (volatile void *)(uintptr_t)((virt & ~0xFFFULL) + off);
}

volatile void *pci_iomap(pci_device_t *dev, int bar, uint64_t maxlen)
{
	return pci_iomap_attr(dev, bar, maxlen, MM_MMIO_UC);
}

volatile void *pci_iomap_wc(pci_device_t *dev, int bar, uint64_t maxlen)
{
	return pci_iomap_attr(dev, bar, maxlen, MM_MMIO_WC);
}

void pci_iounmap(pci_device_t *dev, volatile void *addr)
{
	uint64_t va = (uint64_t)(uintptr_t)addr & ~0xFFFULL, fl;
	size_t pages = 0;

	if (!addr)
		return;
	spin_lock_irqsave(&g_iomap_lock, &fl);
	for (int i = 0; i < PCI_IOMAP_MAX; i++) {
		if (g_iomaps[i].virt == va && (!dev || g_iomaps[i].dev == dev)) {
			pages = g_iomaps[i].pages;
			g_iomaps[i].virt = 0;
			g_iomaps[i].pages = 0;
			g_iomaps[i].dev = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&g_iomap_lock, fl);
	if (WARN(!pages, "pci: pci_iounmap of an address pci_iomap did not return"))
		return;
	mm_unmap_mmio(va, pages);
}

int dma_set_mask_and_coherent(pci_device_t *dev, uint64_t mask)
{
	if (!dev)
		return -EINVAL;
	/* No IOMMU: any mask that covers the low 4 GiB can be honoured
	 * (allocations prefer memory below 4 GiB). */
	if (mask < DMA_BIT_MASK(32))
		return -EIO;
	dev->dma_mask = mask;
	return 0;
}

/* ==========================================================================
 * Enable / disable / bus master / INTx
 * ========================================================================== */

/* The command register is read-modified-written from process context
 * (enable, bus mastering, disable) and from hard interrupt context (the
 * INTx mask): every such update holds this lock across the read and the
 * write, so none is lost.  Nested inside: the config access locks. */
static spinlock_t g_pci_cmd_lock = SPINLOCK_INIT("pci_cmd");

int pci_command_update(const pci_device_t *dev, uint16_t clear, uint16_t set,
		       uint16_t *old)
{
	uint16_t cmd = 0, newcmd;
	uint64_t fl;
	int rc = 0;

	spin_lock_irqsave(&g_pci_cmd_lock, &fl);
	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (PCI_POSSIBLE_ERROR(cmd)) {
		rc = -ENODEV;
	} else {
		newcmd = (uint16_t)((cmd & ~clear) | set);
		if (newcmd != cmd)
			pci_write_config_word(dev, PCI_COMMAND, newcmd);
	}
	spin_unlock_irqrestore(&g_pci_cmd_lock, fl);
	if (old)
		*old = cmd;
	return rc;
}

int pci_enable_device_mem(pci_device_t *dev)
{
	int rc;

	might_sleep();
	if (!dev)
		return -EINVAL;
	if (pci_dev_is_disconnected(dev))
		return -ENODEV;
	/* D0 first: memory decode does not work in D3hot. */
	rc = pci_set_power_state(dev, PCI_D0);
	if (rc && rc != -EIO)
		return rc;
	/* The bridges above must forward memory and allow mastering. */
	for (pci_device_t *br = dev->bus_self; br; br = br->bus_self)
		(void)pci_command_update(br, 0,
					 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
					 NULL);
	if (pci_command_update(dev, 0, PCI_COMMAND_MEMORY, NULL) != 0)
		return -ENODEV;
	dev->flags |= PCI_DEV_FL_ENABLED;
	return 0;
}

void pci_disable_device(pci_device_t *dev)
{
	might_sleep();
	if (!dev)
		return;
	WARN(dev->irq_type != 0 && dev->nr_irq_vecs != 0 &&
		     !pci_dev_is_disconnected(dev),
	     "pci: device disabled with its interrupt vectors still allocated");
	if (!pci_dev_is_disconnected(dev))
		(void)pci_command_update(dev, PCI_COMMAND_MASTER, 0, NULL);
	dev->flags &= ~(PCI_DEV_FL_ENABLED | PCI_DEV_FL_MASTER);
}

int pci_is_enabled(const pci_device_t *dev)
{
	return dev && (dev->flags & PCI_DEV_FL_ENABLED);
}

static void pci_set_master_bit(pci_device_t *dev, int enable)
{
	if (!dev || pci_dev_is_disconnected(dev))
		return;
	if (pci_command_update(dev, enable ? 0 : PCI_COMMAND_MASTER,
			       enable ? PCI_COMMAND_MASTER : 0, NULL) != 0)
		return;
	if (enable)
		dev->flags |= PCI_DEV_FL_MASTER;
	else
		dev->flags &= ~PCI_DEV_FL_MASTER;
}

void pci_set_master(pci_device_t *dev)
{
	pci_set_master_bit(dev, 1);
}

void pci_clear_master(pci_device_t *dev)
{
	pci_set_master_bit(dev, 0);
}

void pci_intx(pci_device_t *dev, int enable)
{
	if (!dev || pci_dev_is_disconnected(dev))
		return;
	/* Called from hard interrupt context too (the INTx mask of the
	 * interrupt core): the update is one locked read-modify-write. */
	(void)pci_command_update(dev,
				 enable ? PCI_COMMAND_INTX_DISABLE : 0,
				 enable ? 0 : PCI_COMMAND_INTX_DISABLE, NULL);
}

/* ==========================================================================
 * Presence and disconnection
 * ========================================================================== */

void pci_dev_set_disconnected(pci_device_t *dev)
{
	if (!dev)
		return;
	__atomic_store_n(&dev->error_state, pci_channel_io_perm_failure,
			 __ATOMIC_RELEASE);
}

int pci_device_is_present(pci_device_t *dev)
{
	uint32_t v = PCI_ERROR_RESPONSE;

	if (!dev)
		return 0;
	if (pci_dev_is_disconnected(dev) ||
	    __atomic_load_n(&dev->is_removed, __ATOMIC_ACQUIRE))
		return 0;
	pci_raw_read(dev->segment, dev->bus, pci_dev_devfn(dev), PCI_VENDOR_ID,
		     4, &v);
	/* CRS (vendor 0x0001) while the device is still initialising is
	 * "there"; all-ones is gone. */
	if (PCI_POSSIBLE_ERROR(v) || (v & 0xFFFF) == 0xFFFF) {
		pci_dev_set_disconnected(dev);
		return 0;
	}
	return 1;
}

/* ==========================================================================
 * Bridge windows and resource assignment
 * ========================================================================== */

int pci_bridge_window(const pci_device_t *bridge, int prefetch, uint64_t *base,
		      uint64_t *limit)
{
	uint16_t b16 = 0, l16 = 0;

	if (!pci_is_bridge(bridge) || bridge->hdr_type != PCI_HEADER_TYPE_BRIDGE)
		return -EINVAL;
	if (!prefetch) {
		pci_read_config_word(bridge, PCI_MEMORY_BASE, &b16);
		pci_read_config_word(bridge, PCI_MEMORY_LIMIT, &l16);
		*base = ((uint64_t)(b16 & PCI_MEMORY_RANGE_MASK)) << 16;
		*limit = (((uint64_t)(l16 & PCI_MEMORY_RANGE_MASK)) << 16) |
			 0xFFFFF;
	} else {
		uint32_t bu = 0, lu = 0;

		pci_read_config_word(bridge, PCI_PREF_MEMORY_BASE, &b16);
		pci_read_config_word(bridge, PCI_PREF_MEMORY_LIMIT, &l16);
		if ((b16 & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) {
			pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32, &bu);
			pci_read_config_dword(bridge, PCI_PREF_LIMIT_UPPER32,
					      &lu);
		}
		*base = (((uint64_t)(b16 & PCI_PREF_RANGE_MASK)) << 16) |
			((uint64_t)bu << 32);
		*limit = (((uint64_t)(l16 & PCI_PREF_RANGE_MASK)) << 16) |
			 0xFFFFF | ((uint64_t)lu << 32);
	}
	return 0;
}

static int pci_bridge_pref64(const pci_device_t *bridge)
{
	uint16_t b16 = 0;

	pci_read_config_word(bridge, PCI_PREF_MEMORY_BASE, &b16);
	return (b16 & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64;
}

static void pci_bridge_set_window(pci_device_t *bridge, int prefetch,
				  uint64_t base, uint64_t limit)
{
	if (!prefetch) {
		pci_write_config_word(bridge, PCI_MEMORY_BASE,
				      (uint16_t)((base >> 16) & 0xFFF0));
		pci_write_config_word(bridge, PCI_MEMORY_LIMIT,
				      (uint16_t)((limit >> 16) & 0xFFF0));
		return;
	}
	/* Close first (base above limit) so the window never covers a
	 * half-written range. */
	pci_write_config_word(bridge, PCI_PREF_MEMORY_BASE, 0xFFF0);
	pci_write_config_word(bridge, PCI_PREF_MEMORY_LIMIT, 0x0000);
	if (pci_bridge_pref64(bridge)) {
		pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32,
				       (uint32_t)(base >> 32));
		pci_write_config_dword(bridge, PCI_PREF_LIMIT_UPPER32,
				       (uint32_t)(limit >> 32));
	}
	pci_write_config_word(bridge, PCI_PREF_MEMORY_LIMIT,
			      (uint16_t)((limit >> 16) & 0xFFF0));
	pci_write_config_word(bridge, PCI_PREF_MEMORY_BASE,
			      (uint16_t)((base >> 16) & 0xFFF0));
}

#define PCI_RES_MAX_USED 128

/* One window being filled: its range and what is taken in it. */
struct pci_res_window {
	uint64_t base, limit; /* limit < base: closed */
	int is64; /* may hold addresses above 4 GiB */
	int nused;
	struct {
		uint64_t start, end;
	} used[PCI_RES_MAX_USED];
};

static int res_window_open(const struct pci_res_window *w)
{
	return w->limit > w->base;
}

static int res_inside(const struct pci_res_window *w, uint64_t start,
		      uint64_t size)
{
	return res_window_open(w) && size && start >= w->base &&
	       start + size - 1 <= w->limit && start + size > start;
}

static void res_reserve(struct pci_res_window *w, uint64_t start,
			uint64_t size)
{
	if (w->nused >= PCI_RES_MAX_USED) {
		WARN_ON_ONCE(1);
		return;
	}
	w->used[w->nused].start = start;
	w->used[w->nused].end = start + size - 1;
	w->nused++;
}

/* First fit: the lowest `align'-aligned address in the window where
 * `size' bytes overlap nothing taken.  0 when nothing fits. */
static uint64_t res_alloc(struct pci_res_window *w, uint64_t size,
			  uint64_t align)
{
	uint64_t cand;

	if (!res_window_open(w) || !size)
		return 0;
	if (align < 0x1000)
		align = 0x1000;
	cand = (w->base + align - 1) & ~(align - 1);
	while (cand + size - 1 <= w->limit && cand + size > cand) {
		int clash = 0;

		if (!w->is64 && cand + size - 1 > 0xFFFFFFFFULL)
			return 0;
		for (int i = 0; i < w->nused; i++) {
			if (cand <= w->used[i].end &&
			    cand + size - 1 >= w->used[i].start) {
				uint64_t next = w->used[i].end + 1;

				cand = (next + align - 1) & ~(align - 1);
				clash = 1;
				break;
			}
		}
		if (!clash) {
			res_reserve(w, cand, size);
			return cand;
		}
	}
	return 0;
}

static void pci_write_bar(pci_device_t *dev, int bar, uint64_t addr, int is64)
{
	int off = PCI_BASE_ADDRESS_0 + bar * 4;
	uint32_t lo = 0;

	struct pci_dev_priv *pp = pci_priv(dev);
	uint64_t fl;

	pci_read_config_dword(dev, off, &lo);
	lo = (lo & 0xF) | (uint32_t)(addr & ~0xFULL);
	pci_write_config_dword(dev, off, lo);
	dev->bar[bar] = lo;
	if (is64 && bar < 5) {
		pci_write_config_dword(dev, off + 4, (uint32_t)(addr >> 32));
		dev->bar[bar + 1] = (uint32_t)(addr >> 32);
	}
	/* The window moved, its size did not: the cache follows. */
	if (!pp)
		return;
	spin_lock_irqsave(&g_pci_bars_lock, &fl);
	if (pp->bars_valid && pp->bars[bar].size) {
		pp->bar_bus[bar] = addr;
		pp->bars[bar].base = dev->segment >= PCI_VMD_DOMAIN_BASE ?
					     pci_vmd_bus_to_cpu(dev->segment,
								addr) :
					     addr;
	}
	spin_unlock_irqrestore(&g_pci_bars_lock, fl);
}

/* Memory needed below a bridge: non-prefetchable and prefetchable totals
 * (each BAR at its natural alignment, sums rounded to the 1 MiB window
 * granularity) and the largest alignment. */
static void pci_bus_size(pci_device_t *bridge, uint64_t *np, uint64_t *pf,
			 uint64_t *np_align, uint64_t *pf_align, int depth)
{
	pci_device_t *kids[64];
	int n;

	*np = *pf = 0;
	*np_align = *pf_align = 0x100000;
	if (depth > 16)
		return;
	n = pci_bridge_children(bridge, kids, 64);
	for (int k = 0; k < n; k++) {
		pci_device_t *c = kids[k];

		if (pci_is_bridge(c)) {
			uint64_t a, b, aa, ba;

			pci_bus_size(c, &a, &b, &aa, &ba, depth + 1);
			*np += a;
			*pf += b;
			if (aa > *np_align)
				*np_align = aa;
			if (ba > *pf_align)
				*pf_align = ba;
			continue;
		}
		for (int i = 0; i < 6; i++) {
			struct pci_bar b;

			/* The cached sizes: a function that is already
			 * live must not be sized again. */
			if (pci_bar_cached(c, i, &b, 1))
				continue;
			if (b.flags & PCI_BAR_IO)
				continue;
			if (b.flags & PCI_BAR_PREFETCH) {
				*pf = ((*pf + b.size - 1) & ~(b.size - 1)) +
				      b.size;
				if (b.size > *pf_align)
					*pf_align = b.size;
			} else {
				*np = ((*np + b.size - 1) & ~(b.size - 1)) +
				      b.size;
				if (b.size > *np_align)
					*np_align = b.size;
			}
			if (b.flags & PCI_BAR_MEM64)
				i++;
		}
	}
	*np = (*np + 0xFFFFF) & ~0xFFFFFULL;
	*pf = (*pf + 0xFFFFF) & ~0xFFFFFULL;
}

static int pci_assign_below(uint32_t segment, uint8_t bus, pci_device_t *bridge,
			    struct pci_res_window *np, struct pci_res_window *pf,
			    int depth);

/* Assign the memory BARs of one function from the windows. */
static int pci_assign_dev_bars(pci_device_t *c, struct pci_res_window *np,
			       struct pci_res_window *pf)
{
	uint16_t cmd = 0;
	int rc = 0, changed = 0;

	pci_read_config_word(c, PCI_COMMAND, &cmd);
	for (int i = 0; i < 6; i++) {
		struct pci_bar b;
		struct pci_res_window *w;
		uint64_t addr;

		if (pci_bar_cached(c, i, &b, 1))
			continue;
		if (b.flags & PCI_BAR_IO) {
			continue;
		}
		w = ((b.flags & PCI_BAR_PREFETCH) && res_window_open(pf) &&
		     ((b.flags & PCI_BAR_MEM64) || !pf->is64 ||
		      pf->base < 0x100000000ULL)) ?
			    pf :
			    np;
		if (res_inside(np, b.base, b.size) ||
		    res_inside(pf, b.base, b.size)) {
			/* Already assigned inside the windows: keep. */
		} else {
			if (!changed) {
				pci_write_config_word(c, PCI_COMMAND,
						      cmd & ~(PCI_COMMAND_MEMORY |
							      PCI_COMMAND_IO));
				changed = 1;
			}
			addr = res_alloc(w, b.size, b.size);
			if (!addr && w == pf)
				addr = res_alloc(np, b.size, b.size);
			if (!addr) {
				pci_warn(c, "BAR %d: no room for %llu bytes\n",
					 i, (unsigned long long)b.size);
				rc = -ENOSPC;
			} else {
				pci_write_bar(c, i,
					      addr, (b.flags & PCI_BAR_MEM64) != 0);
			}
		}
		if (b.flags & PCI_BAR_MEM64)
			i++;
	}
	if (changed)
		pci_write_config_word(c, PCI_COMMAND, cmd);
	return rc;
}

static int pci_assign_below(uint32_t segment, uint8_t bus, pci_device_t *bridge,
			    struct pci_res_window *np, struct pci_res_window *pf,
			    int depth)
{
	pci_device_t *kids[64];
	int n, rc = 0;

	if (depth > 16)
		return -ELOOP;
	if (bridge) {
		n = pci_bridge_children(bridge, kids, 64);
	} else {
		/* A root bus (a VMD domain): every function on it. */
		pci_device_t *devs;
		int count = 0;

		devs = pci_dev_table(&count);
		n = 0;
		for (int i = 0; i < count && n < 64; i++)
			if (!devs[i].is_removed && devs[i].segment == segment &&
			    devs[i].bus == bus)
				kids[n++] = &devs[i];
	}
	/* Pass 1: what is validly assigned stays and is reserved. */
	for (int k = 0; k < n; k++) {
		pci_device_t *c = kids[k];

		if (pci_is_bridge(c)) {
			uint64_t b, l;

			if (!pci_bridge_window(c, 0, &b, &l) && l > b &&
			    res_inside(np, b, l - b + 1))
				res_reserve(np, b, l - b + 1);
			if (!pci_bridge_window(c, 1, &b, &l) && l > b &&
			    res_inside(pf, b, l - b + 1))
				res_reserve(pf, b, l - b + 1);
			continue;
		}
		for (int i = 0; i < 6; i++) {
			struct pci_bar bb;

			if (pci_bar_cached(c, i, &bb, 1))
				continue;
			if (!(bb.flags & PCI_BAR_IO)) {
				if (res_inside(np, bb.base, bb.size))
					res_reserve(np, bb.base, bb.size);
				else if (res_inside(pf, bb.base, bb.size))
					res_reserve(pf, bb.base, bb.size);
			}
			if (bb.flags & PCI_BAR_MEM64)
				i++;
		}
	}
	/* Pass 2: bridges (their windows, then below them), then the
	 * functions' BARs. */
	for (int k = 0; k < n; k++) {
		pci_device_t *c = kids[k];
		struct pci_res_window *cnp, *cpf;
		uint64_t b, l;

		if (!pci_is_bridge(c))
			continue;
		cnp = kcalloc(1, sizeof(*cnp));
		cpf = kcalloc(1, sizeof(*cpf));
		if (!cnp || !cpf) {
			kfree(cnp);
			kfree(cpf);
			return -ENOMEM;
		}
		cnp->is64 = 0;
		cpf->is64 = pci_bridge_pref64(c);
		if (!pci_bridge_window(c, 0, &b, &l) && l > b &&
		    res_inside(np, b, l - b + 1)) {
			cnp->base = b;
			cnp->limit = l;
		}
		if (!pci_bridge_window(c, 1, &b, &l) && l > b &&
		    res_inside(pf, b, l - b + 1)) {
			cpf->base = b;
			cpf->limit = l;
		}
		if (!res_window_open(cnp) || !res_window_open(cpf)) {
			uint64_t need_np, need_pf, al_np, al_pf;

			pci_bus_size(c, &need_np, &need_pf, &al_np, &al_pf,
				     depth + 1);
			if (!res_window_open(cnp) && need_np) {
				uint64_t a = res_alloc(np, need_np, al_np);

				if (a) {
					cnp->base = a;
					cnp->limit = a + need_np - 1;
					pci_bridge_set_window(c, 0, cnp->base,
							      cnp->limit);
				} else {
					rc = -ENOSPC;
				}
			}
			if (!res_window_open(cpf) && need_pf) {
				struct pci_res_window *src =
					res_window_open(pf) ? pf : np;
				uint64_t a;

				if (!cpf->is64 && src->is64)
					src = np;
				a = res_alloc(src, need_pf, al_pf);
				if (a) {
					cpf->base = a;
					cpf->limit = a + need_pf - 1;
					pci_bridge_set_window(c, 1, cpf->base,
							      cpf->limit);
				} else {
					rc = -ENOSPC;
				}
			}
		}
		if (pci_priv(c))
			rc |= pci_assign_below(segment, pci_priv(c)->secondary,
					       c, cnp, cpf, depth + 1);
		kfree(cnp);
		kfree(cpf);
	}
	for (int k = 0; k < n; k++) {
		pci_device_t *c = kids[k];

		if (pci_is_bridge(c))
			continue;
		if (pci_assign_dev_bars(c, np, pf))
			rc = -ENOSPC;
	}
	return rc ? -ENOSPC : 0;
}

int pci_assign_bus_resources(uint32_t segment, uint8_t bus, uint64_t np_base,
			     uint64_t np_limit, uint64_t pf_base,
			     uint64_t pf_limit, int pf64)
{
	struct pci_res_window *np, *pf;
	int rc;

	might_sleep();
	np = kcalloc(1, sizeof(*np));
	pf = kcalloc(1, sizeof(*pf));
	if (!np || !pf) {
		kfree(np);
		kfree(pf);
		return -ENOMEM;
	}
	np->base = np_base;
	np->limit = np_limit;
	np->is64 = np_limit > 0xFFFFFFFFULL;
	pf->base = pf_base;
	pf->limit = pf_limit;
	pf->is64 = pf64;
	rc = pci_assign_below(segment, bus, NULL, np, pf, 0);
	kfree(np);
	kfree(pf);
	return rc;
}

/* ==========================================================================
 * INTx routing
 * ========================================================================== */

/* The root bridge names firmware commonly uses for the host bridge. */
static const char *const g_pci_root_bridges[] = {
	"\\\\_SB_.PCI0",
	"\\\\_SB_.PC00",
	"\\\\_SB_.PCI1",
	NULL,
};

int pci_intx_gsi(const pci_device_t *dev, int *active_low, int *level)
{
	uint8_t pin = 0;
	uint32_t gsi = 0;

	if (!dev)
		return -EINVAL;
	/* PCI interrupts are level-triggered and active-low by definition;
	 * an ACPI link device could say otherwise, but the routing helper
	 * reports only the GSI. */
	if (active_low)
		*active_low = 1;
	if (level)
		*level = 1;
	/* Behind VMD the pins do not reach the I/O APIC. */
	if (dev->segment >= PCI_VMD_DOMAIN_BASE)
		return -ENODEV;
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	if (pin == 0 || pin == 0xFF)
		return -ENODEV;
	if (pin >= 1 && pin <= 4) {
		uint8_t lpin = (uint8_t)(pin - 1);
		uint8_t ldev = dev->device;
		const pci_device_t *p = dev;
		int depth = 0;

		/* Swizzle the pin through every bridge up to the root bus:
		 * a device behind a bridge on slot D raises the bridge's pin
		 * (pin + D) mod 4. */
		while (p->bus != 0 && p->bus_self && depth < 16) {
			lpin = (uint8_t)((lpin + ldev) % 4);
			p = p->bus_self;
			ldev = p->device;
			depth++;
		}
		if (p->bus == 0 && dev->segment == 0) {
			for (int i = 0; g_pci_root_bridges[i]; i++) {
				if (acpi_pci_lookup_irq(g_pci_root_bridges[i],
							ldev, lpin, &gsi) == 0 &&
				    gsi <= 223)
					return (int)gsi;
			}
		}
	}
	/* No _PRT entry: the firmware's interrupt line (legacy PIC range,
	 * identity mapped to the I/O APIC). */
	if (dev->interrupt_line != 0xFF && dev->interrupt_line != 0 &&
	    dev->interrupt_line <= 23)
		return dev->interrupt_line;
	return -ENOENT;
}

/* ==========================================================================
 * Delays
 * ========================================================================== */

int pci_can_sleep(void)
{
	return sched_current() != NULL && !irqs_disabled();
}

void pci_msleep(unsigned int ms)
{
	if (ms == 0)
		return;
	if (pci_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			; /* a signal cut it short: sleep the rest */
	} else {
		lapic_delay_ms(ms);
	}
}

void pci_udelay(unsigned int us)
{
	lapic_delay_us(us);
}
