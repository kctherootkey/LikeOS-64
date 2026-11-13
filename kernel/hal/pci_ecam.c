// LikeOS - PCI configuration space through ECAM (MCFG), all segments
//
// The Enhanced Configuration Access Mechanism maps every function's 4 KiB
// configuration space into memory: function (bus, dev, fn) of a region
// that starts at bus S lives at base + ((bus - S) << 20 | dev << 15 |
// fn << 12).  The ACPI MCFG table lists one region per segment group (and
// bus range).  Every region is recorded; a bus is mapped (1 MiB) the first
// time it is scanned or used -- the boot scan maps all buses that hold a
// device or lie below a bridge -- so the accessors themselves never map
// and work in any context.
//
// Like the PC platform code, segment 0 keeps using port I/O for the first
// 256 bytes (pci.c); ECAM serves the extended space and the other
// segments.  A segment-0 region is only trusted when it reads the same
// host bridge ID as port I/O does (a firmware that advertises a window it
// does not decode would otherwise give every device an extended space of
// garbage).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/pci.h>
#include <kernel/hal/pci_internal.h>
#include <kernel/hal/acpi.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/io.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define PCI_ECAM_MAX_REGIONS 16
#define PCI_ECAM_BUS_PAGES 256 /* 1 MiB per bus */

struct pci_ecam_region {
	uint64_t phys; /* base of bus `bus_start' */
	uint32_t segment;
	uint8_t bus_start, bus_end;
	int valid;
	uint64_t bus_va[256]; /* by bus - bus_start; 0 = not mapped */
};

static struct pci_ecam_region g_ecam[PCI_ECAM_MAX_REGIONS];
static int g_ecam_nr;
static spinlock_t g_ecam_map_lock = SPINLOCK_INIT("pci_ecam_map");

/* MCFG: the SDT header, 8 reserved bytes, then 16-byte allocations. */
struct __attribute__((packed)) mcfg_alloc {
	uint64_t base;
	uint16_t segment;
	uint8_t bus_start;
	uint8_t bus_end;
	uint32_t reserved;
};

static struct pci_ecam_region *ecam_region(uint32_t segment, uint8_t bus)
{
	for (int i = 0; i < g_ecam_nr; i++) {
		struct pci_ecam_region *r = &g_ecam[i];

		if (r->valid && r->segment == segment && bus >= r->bus_start &&
		    bus <= r->bus_end)
			return r;
	}
	return NULL;
}

int pci_ecam_covers(uint32_t segment, uint8_t bus)
{
	return ecam_region(segment, bus) != NULL;
}

int pci_ecam_segment_at(int idx, uint32_t *segment, uint8_t *bus_start,
			uint8_t *bus_end)
{
	if (idx < 0 || idx >= g_ecam_nr || !g_ecam[idx].valid)
		return -ENOENT;
	*segment = g_ecam[idx].segment;
	*bus_start = g_ecam[idx].bus_start;
	*bus_end = g_ecam[idx].bus_end;
	return 0;
}

int pci_ecam_map_bus(uint32_t segment, uint8_t bus)
{
	struct pci_ecam_region *r = ecam_region(segment, bus);
	uint64_t va, fl;
	unsigned int idx;

	if (!r)
		return -ENOENT;
	idx = (unsigned int)(uint8_t)(bus - r->bus_start);
	if (__atomic_load_n(&r->bus_va[idx], __ATOMIC_ACQUIRE))
		return 0;
	va = mm_map_mmio_flags(r->phys + ((uint64_t)idx << 20),
			       PCI_ECAM_BUS_PAGES, MM_MMIO_UC);
	if (!va)
		return -ENOMEM;
	spin_lock_irqsave(&g_ecam_map_lock, &fl);
	if (!r->bus_va[idx]) {
		__atomic_store_n(&r->bus_va[idx], va, __ATOMIC_RELEASE);
		va = 0;
	}
	spin_unlock_irqrestore(&g_ecam_map_lock, fl);
	if (va) /* lost a race: the other mapping stays */
		mm_unmap_mmio(va, PCI_ECAM_BUS_PAGES);
	return 0;
}

volatile void *pci_ecam_map(uint16_t segment, uint8_t bus, uint8_t devfn)
{
	struct pci_ecam_region *r = ecam_region(segment, bus);
	uint64_t va;

	if (!r)
		return NULL;
	va = __atomic_load_n(&r->bus_va[(uint8_t)(bus - r->bus_start)],
			     __ATOMIC_ACQUIRE);
	if (!va)
		return NULL;
	return (volatile void *)(uintptr_t)(va +
					    pci_ecam_offset(bus, bus, devfn, 0));
}

int pci_ecam_read(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		  int size, uint32_t *val)
{
	volatile uint8_t *base;

	if (segment > 0xFFFF)
		return PCIBIOS_DEVICE_NOT_FOUND;
	base = (volatile uint8_t *)pci_ecam_map((uint16_t)segment, bus, devfn);
	if (!base) {
		*val = PCI_ERROR_RESPONSE;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	switch (size) {
	case 1:
		*val = readb(base + where);
		break;
	case 2:
		*val = readw(base + where);
		break;
	default:
		*val = readl(base + where);
		break;
	}
	return PCIBIOS_SUCCESSFUL;
}

int pci_ecam_write(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		   int size, uint32_t val)
{
	volatile uint8_t *base;

	if (segment > 0xFFFF)
		return PCIBIOS_DEVICE_NOT_FOUND;
	base = (volatile uint8_t *)pci_ecam_map((uint16_t)segment, bus, devfn);
	if (!base)
		return PCIBIOS_DEVICE_NOT_FOUND;
	switch (size) {
	case 1:
		writeb((uint8_t)val, base + where);
		break;
	case 2:
		writew((uint16_t)val, base + where);
		break;
	default:
		writel(val, base + where);
		break;
	}
	return PCIBIOS_SUCCESSFUL;
}

/* Does the segment-0 region decode what port I/O sees?  The host bridge
 * (or the first device on the start bus) must read identically. */
static int ecam_validate_seg0(struct pci_ecam_region *r)
{
	for (unsigned int slot = 0; slot < 32; slot++) {
		uint32_t io = 0, mm = 0;
		uint8_t devfn = (uint8_t)PCI_DEVFN(slot, 0);

		pci_conf1_read(r->bus_start, devfn, PCI_VENDOR_ID, 4, &io);
		if ((io & 0xFFFF) == 0xFFFF)
			continue;
		if (pci_ecam_read(0, r->bus_start, devfn, PCI_VENDOR_ID, 4,
				  &mm) != PCIBIOS_SUCCESSFUL)
			return 0;
		return io == mm;
	}
	/* Nothing on the start bus to compare against: trust it. */
	return 1;
}

static int ecam_parse_mcfg(void)
{
	acpi_sdt_header_t *mcfg = acpi_find_table("MCFG");
	const uint8_t *p;
	uint32_t n;

	if (!mcfg) {
		kprintf("PCI: no MCFG table, extended configuration space unavailable\n");
		return 0;
	}
	if (mcfg->length < 44)
		return 0;
	n = (mcfg->length - 44) / (uint32_t)sizeof(struct mcfg_alloc);
	p = (const uint8_t *)mcfg + 44;
	BUILD_BUG_ON(sizeof(struct mcfg_alloc) != 16);
	for (uint32_t i = 0; i < n; i++) {
		struct mcfg_alloc a;
		struct pci_ecam_region *r;

		__builtin_memcpy(&a, p + i * sizeof(a), sizeof(a));
		if (!a.base || a.bus_end < a.bus_start ||
		    (a.base & 0xFFFFFULL)) {
			kprintf("PCI: MCFG entry %u (segment %u, buses %02x-%02x, base 0x%llx) rejected\n",
				i, a.segment, a.bus_start, a.bus_end,
				(unsigned long long)a.base);
			continue;
		}
		if (g_ecam_nr >= PCI_ECAM_MAX_REGIONS) {
			kprintf("PCI: MCFG: more than %d regions, the rest ignored\n",
				PCI_ECAM_MAX_REGIONS);
			break;
		}
		r = &g_ecam[g_ecam_nr];
		mm_memset(r, 0, sizeof(*r));
		/* The base in MCFG is that of bus 0 of the segment even
		 * when the range starts higher. */
		r->phys = a.base + ((uint64_t)a.bus_start << 20);
		r->segment = a.segment;
		r->bus_start = a.bus_start;
		r->bus_end = a.bus_end;
		r->valid = 1;
		g_ecam_nr++;
		kprintf("PCI: ECAM [mem 0x%llx-0x%llx] for segment %04x buses %02x-%02x\n",
			(unsigned long long)r->phys,
			(unsigned long long)(r->phys +
					     ((uint64_t)(a.bus_end - a.bus_start +
							 1)
					      << 20) -
					     1),
			r->segment, r->bus_start, r->bus_end);
	}
	return g_ecam_nr;
}

void pci_ecam_init(void)
{
	int count = 0;
	pci_device_t *devs;

	if (g_ecam_nr)
		return; /* once */
	if (!ecam_parse_mcfg())
		return;

	/* Segment 0: validate, then map every bus a device or a bridge
	 * range names (hot-plug slots get their bus mapped too). */
	for (int i = 0; i < g_ecam_nr; i++) {
		struct pci_ecam_region *r = &g_ecam[i];

		if (r->segment != 0)
			continue;
		if (pci_ecam_map_bus(0, r->bus_start) ||
		    !ecam_validate_seg0(r)) {
			kprintf("PCI: ECAM for segment 0 does not decode, not used\n");
			r->valid = 0;
		}
	}
	devs = pci_dev_table(&count);
	for (int i = 0; i < count; i++) {
		pci_device_t *d = &devs[i];
		struct pci_dev_priv *pp = pci_priv(d);

		if (d->segment != 0 || d->is_removed)
			continue;
		(void)pci_ecam_map_bus(0, d->bus);
		if (pci_is_bridge(d) && pp && pp->secondary) {
			for (unsigned int b = pp->secondary;
			     b <= pp->subordinate && b < 256; b++)
				(void)pci_ecam_map_bus(0, (uint8_t)b);
		}
	}
	/* Now that the extended space answers: capabilities above 0x100,
	 * the space size, everything derived from them. */
	for (int i = 0; i < count; i++) {
		pci_device_t *d = &devs[i];

		if (d->segment == 0 && !d->is_removed)
			pci_setup_device(d);
	}

	/* Other segments: nothing found them yet (port I/O reaches only
	 * segment 0).  Scan each from its first bus; the scan follows the
	 * bridges the firmware configured. */
	for (int i = 0; i < g_ecam_nr; i++) {
		struct pci_ecam_region *r = &g_ecam[i];
		int added;

		if (!r->valid || r->segment == 0)
			continue;
		added = pci_scan_bus(r->segment, r->bus_start, 0, NULL,
				     r->bus_end);
		if (added)
			kprintf("PCI: segment %04x: %d devices\n", r->segment,
				added);
	}
}
