// LikeOS - Minimal PCI enumeration (legacy config mechanism #1)
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_PCI_H
#define LIKEOS_PCI_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/types.h>

#define PCI_MAX_DEVICES 256

typedef struct pci_device {
	unsigned char bus;
	unsigned char device;
	unsigned char function;
	unsigned short vendor_id;
	unsigned short device_id;
	unsigned char class_code;
	unsigned char subclass;
	unsigned char prog_if;
	unsigned int bar[6];
	unsigned char interrupt_line; /* legacy INTx line (0-15 or 0xFF) */
	unsigned char interrupt_pin; /* 1=INTA ... */
} pci_device_t;

void pci_init(void);
int pci_enumerate(void); // returns number of devices recorded
const pci_device_t *pci_get_devices(int *count);
const pci_device_t *pci_get_first_xhci(void);
const pci_device_t *
pci_get_xhci(int index); // Get Nth xHCI controller (0-based)
// Raw config access helpers (temporary exposure for early drivers)
unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev,
			    unsigned char func, unsigned char off);
void pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func,
		     unsigned char off, unsigned int value);
void pci_enable_busmaster_mem(const pci_device_t *dev);
void pci_assign_unassigned_bars(void);

/* Narrow config accesses, built on the dword ones: a read-modify-write of
 * the containing dword under the same lock, so a driver that needs to
 * poke one byte (a capability's control word, the VGA decode enable) does
 * not clobber its neighbours. */
uint8_t pci_cfg_read8(const pci_device_t *dev, unsigned char off);
uint16_t pci_cfg_read16(const pci_device_t *dev, unsigned char off);
uint32_t pci_cfg_read32_dev(const pci_device_t *dev, unsigned char off);
void pci_cfg_write8(const pci_device_t *dev, unsigned char off, uint8_t v);
void pci_cfg_write16(const pci_device_t *dev, unsigned char off, uint16_t v);
void pci_cfg_write32_dev(const pci_device_t *dev, unsigned char off,
			 uint32_t v);

/* A decoded base address register.
 *
 * The raw dwords in pci_device_t.bar[] carry the type bits and, for a
 * 64-bit memory BAR, only the low half; nothing there says how large the
 * window is.  This reads the pair, sizes the window the standard way
 * (all-ones written and read back with decoding paused, the original
 * restored) and hands back one description.  `index' is the BAR number
 * 0-5; a 64-bit BAR consumes index+1 as well, and asking for that upper
 * half returns -EINVAL.  Returns 0 when the BAR is implemented, -ENOENT
 * when it is not. */
#define PCI_BAR_IO 0x01
#define PCI_BAR_MEM64 0x02
#define PCI_BAR_PREFETCH 0x04
struct pci_bar {
	uint64_t base;
	uint64_t size;
	uint32_t flags; /* PCI_BAR_* */
};
int pci_bar_decode(const pci_device_t *dev, int index, struct pci_bar *out);

/* The expansion ROM register (0x30): base and size when one is present
 * (the enable bit is left as found), -ENOENT otherwise. */
int pci_rom_decode(const pci_device_t *dev, struct pci_bar *out);

/* The device on `bus' at `device'.`function', or NULL. */
const pci_device_t *pci_find_bdf(unsigned char bus, unsigned char device,
				 unsigned char function);

// PCI Capability IDs
#define PCI_CAP_MSI 0x05
#define PCI_CAP_MSIX 0x11

// PCI Command register bits
#define PCI_CMD_INTX_DISABLE (1 << 10)

// MSI Address/Data format for x86 LAPIC
// Address: 0xFEE00000 | (dest_apic_id << 12)
// Data:    vector | (0 = fixed delivery, edge trigger)
#define MSI_ADDR_BASE 0xFEE00000

// Find a PCI capability by ID.  Returns the config-space offset of the
// capability header, or 0 if not found.
uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id);

// Enable MSI for a PCI device.  Programs MSI address/data with the given
// vector targeting the BSP's APIC ID, enables MSI, and disables legacy INTx.
// Returns 0 on success, -1 if device has no MSI capability.
int pci_enable_msi(const pci_device_t *dev, uint8_t vector);
/* MSI aimed at a particular local APIC; MSI-X table entry `entry'. */
int pci_enable_msi_cpu(const pci_device_t *dev, uint8_t vector,
		       uint32_t apic_id);
int pci_enable_msix(const pci_device_t *dev, int entry, uint8_t vector,
		    uint32_t apic_id);

// Find the PCI-to-PCI bridge whose secondary bus matches target_bus.
// Returns the bridge pci_device_t* or NULL if not found.
const pci_device_t *pci_find_bridge_for_bus(unsigned char target_bus);

#endif // LIKEOS_PCI_H
