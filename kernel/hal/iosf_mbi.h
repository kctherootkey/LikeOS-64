// LikeOS HAL - IOSF sideband mailbox (Atom SoCs) and the P-Unit PMIC bus
// semaphore
//
// Atom SoCs (Bay Trail, Cherry Trail / Braswell, Quark, Tangier) reach the
// units of their on-chip fabric through a mailbox in the configuration
// space of the host bridge: the message control register (MCR: opcode,
// port, low offset bits, byte enables), the extended offset (MCRX: the
// high offset bits) and the data register (MDR).  Writing MCR runs the
// message; a read leaves its result in MDR.
//
// On Bay Trail and Cherry Trail one I2C bus (the one whose controller has
// an ACPI _SEM method returning non-zero) carries the PMIC and is shared
// with the P-Unit, which talks to the PMIC on its own (voltage changes,
// C6/C7 entry).  Before the OS uses that bus it asks the P-Unit for its
// PMIC bus semaphore over the sideband and waits until the P-Unit grants
// it; the P-Unit then leaves the bus alone until the OS lets go.
//
// iosf_mbi_block_punit_i2c_access() takes the semaphore (nested callers
// share it: a client that wants several transfers in a row may already
// hold it when the controller asks again), iosf_mbi_unblock_punit_i2c_
// access() gives it back when the last holder is done.  Code that makes
// the P-Unit use the PMIC bus itself (P-Unit requests that do not check
// the semaphore) brackets those requests with iosf_mbi_punit_acquire() /
// iosf_mbi_punit_release(); the two sides exclude each other.  Code that
// must prepare for the bus being taken (from atomic context, where it
// could not wait) registers a notifier that runs at the begin and end of
// every blocked period.
//
// Entering C6/C7 makes the P-Unit talk to the PMIC, which hangs the SoC
// while the OS holds the semaphore, so the blocked period also carries a
// "no deep C-states" constraint (iosf_mbi_deep_cstates_allowed()).
// LikeOS idles with HLT (C1) and never asks for deeper states, so the
// constraint is met by construction; the count is kept for an idle
// driver that would.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2013-2014 Intel Corporation

#ifndef KERNEL_HAL_IOSF_MBI_H
#define KERNEL_HAL_IOSF_MBI_H

#include <kernel/uapi/types.h>

struct pci_device;

/* Host bridge configuration registers. */
#define MBI_MCR_OFFSET 0xD0
#define MBI_MDR_OFFSET 0xD4
#define MBI_MCRX_OFFSET 0xD8

#define MBI_RD_MASK 0xFEFFFFFFu
#define MBI_WR_MASK 0x01000000u

#define MBI_MASK_HI 0xFFFFFF00u
#define MBI_MASK_LO 0x000000FFu
#define MBI_ENABLE 0xF0u

/* Opcodes */
#define MBI_MMIO_READ 0x00
#define MBI_MMIO_WRITE 0x01
#define MBI_CFG_READ 0x04
#define MBI_CFG_WRITE 0x05
#define MBI_CR_READ 0x06
#define MBI_CR_WRITE 0x07
#define MBI_REG_READ 0x10
#define MBI_REG_WRITE 0x11
#define MBI_ESRAM_READ 0x12
#define MBI_ESRAM_WRITE 0x13

/* Bay Trail / Cherry Trail ports */
#define BT_MBI_UNIT_AUNIT 0x00
#define BT_MBI_UNIT_SMC 0x01
#define BT_MBI_UNIT_CPU 0x02
#define BT_MBI_UNIT_BUNIT 0x03
#define BT_MBI_UNIT_PMC 0x04
#define BT_MBI_UNIT_GFX 0x06 /* the GPU's own sideband: refused here */
#define BT_MBI_UNIT_SMI 0x0C
#define BT_MBI_UNIT_CCK 0x14
#define BT_MBI_UNIT_USB 0x43
#define BT_MBI_UNIT_SATA 0xA3
#define BT_MBI_UNIT_PCIE 0xA6

/* Quark ports */
#define QRK_MBI_UNIT_HBA 0x00
#define QRK_MBI_UNIT_HB 0x03
#define QRK_MBI_UNIT_RMU 0x04
#define QRK_MBI_UNIT_MM 0x05
#define QRK_MBI_UNIT_SOC 0x31

/* The P-Unit's PMIC bus semaphore register (in the PMC port) and its
 * bits. */
#define PUNIT_SEMAPHORE_BYT 0x7
#define PUNIT_SEMAPHORE_CHT 0x10e
#define PUNIT_SEMAPHORE_BIT (1u << 0)
#define PUNIT_SEMAPHORE_ACQUIRE (1u << 1)
/* How long the P-Unit may take to grant it (ms). */
#define PUNIT_SEMAPHORE_TIMEOUT_MS 500

/* Host bridge device ids. */
#define PCI_DEVICE_ID_INTEL_BAYTRAIL 0x0F00
#define PCI_DEVICE_ID_INTEL_BRASWELL 0x2280
#define PCI_DEVICE_ID_INTEL_QUARK_X1000 0x0958
#define PCI_DEVICE_ID_INTEL_TANGIER 0x1170

/* Notifier events. */
#define MBI_PMIC_BUS_ACCESS_BEGIN 1
#define MBI_PMIC_BUS_ACCESS_END 2

struct iosf_mbi_notifier {
	/* Called with the P-Unit access mutex held, in process context. */
	int (*fn)(struct iosf_mbi_notifier *nb, unsigned long action);
	struct iosf_mbi_notifier *next;
};

/* Find the host bridge (once; process context).  0, or -ENODEV on
 * machines without the mailbox. */
int iosf_mbi_init(void);
/* Use `dev' as the mailbox with the semaphore at `sem_address' (0: none,
 * as on Quark/Tangier) -- what iosf_mbi_init does once it found the host
 * bridge; NULL forgets the device (tests).  Not while the mailbox is in
 * use. */
void iosf_mbi_attach(const struct pci_device *dev, uint32_t sem_address);

/* Is the mailbox there. */
bool iosf_mbi_available(void);
/* Is there a P-Unit PMIC bus semaphore (Bay Trail, Cherry Trail). */
bool iosf_mbi_punit_semaphore_available(void);

/* One sideband message: 0 or -errno (-ENODEV no mailbox, -EPERM the GFX
 * port).  Any context (a spinlock serialises the mailbox). */
int iosf_mbi_read(uint8_t port, uint8_t opcode, uint32_t offset,
		  uint32_t *mdr);
int iosf_mbi_write(uint8_t port, uint8_t opcode, uint32_t offset,
		   uint32_t mdr);
/* Read, replace the bits of `mask' with those of `mdr', write back
 * (atomic against other mailbox users). */
int iosf_mbi_modify(uint8_t port, uint8_t opcode, uint32_t offset,
		    uint32_t mdr, uint32_t mask);

/* P-Unit requests that make it use the PMIC bus: wait until the OS's own
 * PMIC bus accesses are over, keep new ones out until released.  May
 * sleep. */
void iosf_mbi_punit_acquire(void);
void iosf_mbi_punit_release(void);
/* Warn when the caller does not hold a P-Unit acquire. */
void iosf_mbi_assert_punit_acquired(void);

/* Take the P-Unit PMIC bus semaphore for the OS (nested: shared with
 * holders already in): 0, -ENXIO (no semaphore), -ETIMEDOUT, -errno.
 * Every 0 is matched by one unblock.  May sleep. */
int iosf_mbi_block_punit_i2c_access(void);
void iosf_mbi_unblock_punit_i2c_access(void);
/* Does the P-Unit report the semaphore as given to the OS right now
 * (reads the register): 1, 0, or -errno. */
int iosf_mbi_punit_semaphore_held(void);

int iosf_mbi_register_pmic_bus_access_notifier(struct iosf_mbi_notifier *nb);
int iosf_mbi_unregister_pmic_bus_access_notifier(struct iosf_mbi_notifier *nb);
/* The same with the P-Unit access already acquired by the caller. */
int iosf_mbi_unregister_pmic_bus_access_notifier_unlocked(
	struct iosf_mbi_notifier *nb);

/* May the processors enter C6/C7 now (no blocked PMIC bus period). */
bool iosf_mbi_deep_cstates_allowed(void);

/* Counters for diagnostics (/proc/i2c) and the tests. */
struct iosf_mbi_stats {
	unsigned long acquires; /* semaphore granted (outermost) */
	unsigned long nested; /* block calls that found it held */
	unsigned long timeouts; /* the P-Unit did not grant it in time */
	unsigned long errors; /* mailbox access failed */
	unsigned long releases;
	uint64_t max_wait_us; /* longest wait for the grant */
	uint64_t max_hold_us; /* longest blocked period */
};
void iosf_mbi_get_stats(struct iosf_mbi_stats *out);

#endif /* KERNEL_HAL_IOSF_MBI_H */
