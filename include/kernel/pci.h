// LikeOS-64 - Minimal PCI enumeration (legacy config mechanism #1)
#ifndef LIKEOS_PCI_H
#define LIKEOS_PCI_H

#include "status.h"

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

#endif // LIKEOS_PCI_H
