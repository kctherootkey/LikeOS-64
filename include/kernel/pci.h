// LikeOS-64 - Minimal PCI enumeration (legacy config mechanism #1)
#ifndef LIKEOS_PCI_H
#define LIKEOS_PCI_H

#include "status.h"
#include "types.h"

#define PCI_MAX_DEVICES 256

typedef struct {
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
    unsigned char interrupt_pin;  /* 1=INTA ... */
} pci_device_t;

void pci_init(void);
int  pci_enumerate(void); // returns number of devices recorded
const pci_device_t* pci_get_devices(int* count);
const pci_device_t* pci_get_first_xhci(void);
// Raw config access helpers (temporary exposure for early drivers)
unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off);
void pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off, unsigned int value);
void pci_enable_busmaster_mem(const pci_device_t* dev);
void pci_assign_unassigned_bars(void);

// PCI Capability IDs
#define PCI_CAP_MSI         0x05
#define PCI_CAP_MSIX        0x11

// PCI Command register bits
#define PCI_CMD_INTX_DISABLE  (1 << 10)

// MSI Address/Data format for x86 LAPIC
// Address: 0xFEE00000 | (dest_apic_id << 12)
// Data:    vector | (0 = fixed delivery, edge trigger)
#define MSI_ADDR_BASE       0xFEE00000

// Find a PCI capability by ID.  Returns the config-space offset of the
// capability header, or 0 if not found.
uint8_t pci_find_capability(const pci_device_t* dev, uint8_t cap_id);

// Enable MSI for a PCI device.  Programs MSI address/data with the given
// vector targeting BSP APIC ID 0, enables MSI, and disables legacy INTx.
// Returns 0 on success, -1 if device has no MSI capability.
int pci_enable_msi(const pci_device_t* dev, uint8_t vector);

#endif // LIKEOS_PCI_H
