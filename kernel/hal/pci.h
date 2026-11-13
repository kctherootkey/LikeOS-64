// LikeOS - PCI: enumeration, configuration space, BARs, MSI/MSI-X, and the
// PCI core contract (driver model, interrupt vectors with affinity, power
// management, resets, error recovery)
//
// The first part of this header is the original minimal interface (port-I/O
// configuration mechanism #1, one MSI message, one MSI-X entry per call);
// every existing driver keeps using it unchanged.  The second part, "PCI core
// contract", is implemented by kernel/hal/pci_*.c (ECAM, MSI, PM, VMD)
// and is what new drivers (NVMe) are written against.
// Register and bit names: <kernel/hal/pci_regs.h> (not included here).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_PCI_H
#define LIKEOS_PCI_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/types.h>
#include <kernel/ke/cpumask.h>

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

	/* ---- PCI core state (kernel/hal/pci*.c).  Zero until the PCI core
	 * fills it at enumeration; drivers read it, only the PCI core
	 * writes it (through the non-const pointer the core hands to
	 * probe()). --------------------------------------------------------- */
	uint32_t segment; /* PCI segment group (ECAM, MCFG); a VMD domain
			   * gets a number >= 0x10000 */
	uint16_t subsystem_vendor; /* config 0x2c */
	uint16_t subsystem_device; /* config 0x2e */
	uint8_t revision; /* config 0x08 */
	uint8_t hdr_type; /* config 0x0e, multifunction bit masked off */
	uint32_t class_full; /* class << 16 | subclass << 8 | prog_if */
	uint16_t cfg_size; /* 256, or 4096 when ECAM reaches it */
	uint8_t pcie_cap; /* PCI Express capability offset, 0 none */
	uint8_t pcie_flags_type; /* PCI_EXP_TYPE_* (pcie_cap only) */
	uint8_t msi_cap; /* MSI capability offset, 0 none */
	uint8_t msix_cap; /* MSI-X capability offset, 0 none */
	uint8_t pm_cap; /* power management capability offset, 0 none */
	uint8_t af_cap; /* Advanced Features (conventional FLR), 0 none */
	uint16_t acs_cap, l1ss_cap, ltr_cap, dpc_cap;
	uint8_t d3hot_delay_ms; /* PCI_PM_D3HOT_WAIT unless quirked */
	uint8_t pad0;
	int16_t current_state; /* PCI_D0 .. PCI_D3cold, PCI_UNKNOWN */
	/* Error and presence state (pci_channel_state_t), changed
	 * atomically: pci_channel_io_perm_failure == disconnected. */
	volatile int error_state;
	volatile int is_removed; /* hot-removed: no further config access */
	uint64_t dma_mask; /* DMA addressing limit the driver set */
	uint32_t flags; /* PCI_DEV_FL_* */
	/* Interrupts allocated by pci_alloc_irq_vectors_affinity(). */
	uint16_t irq_type; /* PCI_IRQ_INTX / _MSI / _MSIX, 0 none */
	uint16_t nr_irq_vecs;
	struct pci_irq_vec *irq_vecs; /* nr_irq_vecs entries, the core's */
	volatile uint32_t *msix_table; /* mapped once at the first use */
	uint16_t msix_table_size; /* entries the capability offers */
	uint16_t msi_max_vecs; /* MSI messages the capability offers */
	/* Saved configuration (pci_save_state). */
	struct pci_saved_state *saved;
	/* Driver binding (pci_register_driver). */
	struct pci_driver *driver;
	void *driver_data;
	/* The bridge above (NULL on the root bus). */
	struct pci_device *bus_self;
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

/* ==========================================================================
 * PCI core contract (implemented by kernel/hal/pci_*.c -- Phase 1 agent P1)
 * ==========================================================================
 *
 * Context rules: "any context" = interrupts may be off, spinlocks held;
 * "may sleep" = process context.  Config space accessors are any context
 * (they take an internal spinlock for port I/O; ECAM needs none).
 * Return conventions: 0 / -errno unless stated.
 */

struct pci_device_id;
struct irq_affinity;

/* struct pci_device.flags */
#define PCI_DEV_FL_ENABLED (1u << 0) /* pci_enable_device_mem done */
#define PCI_DEV_FL_MASTER (1u << 1) /* bus mastering on */
#define PCI_DEV_FL_NO_D3 (1u << 2) /* quirk: never put in D3 */
#define PCI_DEV_FL_NO_FLR (1u << 3) /* quirk: FLR broken */
#define PCI_DEV_FL_BEHIND_VMD (1u << 4) /* in a VMD domain */
#define PCI_DEV_FL_STATE_SAVED (1u << 6) /* pci_save_state() valid */
#define PCI_DEV_FL_ASPM_DISABLED (1u << 7) /* ASPM turned off by quirk */

/* ---- configuration space (ECAM for all 4096 bytes where the firmware's
 * MCFG covers the segment/bus; port I/O for the first 256 otherwise) ---- */
#define PCIBIOS_SUCCESSFUL 0x00
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
/* Read/write at byte offset `where' (0..cfg_size-1, naturally aligned for
 * the width).  Return PCIBIOS_* (0 = success).  A read of a device that is
 * gone (hot-removed, disconnected, or not answering) returns all-ones in
 * *val; a read alone does not decide that the device is gone -- all-ones
 * is a legal value of many registers -- pci_device_is_present() does.
 * Segment 0 below offset 256 uses port I/O, everything else ECAM (or a
 * VMD domain's window).  Any context. */
int pci_read_config_byte(const pci_device_t *dev, int where, uint8_t *val);
int pci_read_config_word(const pci_device_t *dev, int where, uint16_t *val);
int pci_read_config_dword(const pci_device_t *dev, int where, uint32_t *val);
int pci_write_config_byte(const pci_device_t *dev, int where, uint8_t val);
int pci_write_config_word(const pci_device_t *dev, int where, uint16_t val);
int pci_write_config_dword(const pci_device_t *dev, int where, uint32_t val);
/* Raw ECAM window of a function, NULL when ECAM does not reach it. */
volatile void *pci_ecam_map(uint16_t segment, uint8_t bus, uint8_t devfn);
/* Called once by pci_init() after ACPI: maps every MCFG segment. */
void pci_ecam_init(void);

/* Extended capability (offset >= 0x100) with id PCI_EXT_CAP_ID_*: the
 * header offset, 0 when absent or no extended config space.  `start' 0
 * searches from the beginning; pass a previous result to find the next
 * instance.  Any context. */
uint16_t pci_find_ext_capability(const pci_device_t *dev, int cap);
uint16_t pci_find_next_ext_capability(const pci_device_t *dev, uint16_t start,
				      int cap);
/* Standard capability by PCI_CAP_ID_*, continuing after `pos' (0: from
 * the list head).  0 when absent. */
uint8_t pci_find_next_capability(const pci_device_t *dev, uint8_t pos,
				 int cap);

/* PCI Express capability register access (offsets PCI_EXP_*), which
 * hides the v1 capability's missing registers (reads 0, writes ignored).
 * Return PCIBIOS_*.  Any context. */
int pcie_capability_read_word(const pci_device_t *dev, int pos,
			      uint16_t *val);
int pcie_capability_read_dword(const pci_device_t *dev, int pos,
			       uint32_t *val);
int pcie_capability_write_word(const pci_device_t *dev, int pos, uint16_t val);
int pcie_capability_write_dword(const pci_device_t *dev, int pos,
				uint32_t val);
int pcie_capability_clear_and_set_word(const pci_device_t *dev, int pos,
				       uint16_t clear, uint16_t set);
int pcie_capability_clear_and_set_dword(const pci_device_t *dev, int pos,
					uint32_t clear, uint32_t set);
static inline int pci_is_pcie(const pci_device_t *dev)
{
	return dev->pcie_cap != 0;
}
static inline int pci_pcie_type(const pci_device_t *dev)
{
	return dev->pcie_flags_type;
}
/* Link properties.  Speeds are PCI_EXP_LNKSTA_CLS_* encodings (1 = 2.5 GT/s
 * ... 6 = 64 GT/s), widths lane counts.  0 / -ENODEV for non-PCIe. */
int pcie_link_status(const pci_device_t *dev, unsigned int *speed,
		     unsigned int *width);
int pcie_link_capability(const pci_device_t *dev, unsigned int *max_speed,
			 unsigned int *max_width);
/* Bandwidth the link to the root offers, in Mb/s (the narrowest link on
 * the way), 0 unknown. */
uint32_t pcie_bandwidth_available(const pci_device_t *dev);
/* Max Payload Size / Max Read Request Size in bytes. */
int pcie_get_mps(const pci_device_t *dev);
int pcie_set_mps(const pci_device_t *dev, int mps);
int pcie_get_readrq(const pci_device_t *dev);
int pcie_set_readrq(const pci_device_t *dev, int rq);
/* ASPM: disable L0s/L1 states on the link of `dev' (PCIE_LINK_STATE_*). */
#define PCIE_LINK_STATE_L0S (1u << 0)
#define PCIE_LINK_STATE_L1 (1u << 1)
#define PCIE_LINK_STATE_L1_1 (1u << 2)
#define PCIE_LINK_STATE_L1_2 (1u << 3)
#define PCIE_LINK_STATE_CLKPM (1u << 6)
#define PCIE_LINK_STATE_ALL 0x7fu
int pci_disable_link_state(const pci_device_t *dev, uint32_t state);
int pcie_aspm_enabled(const pci_device_t *dev);

/* ---- enabling, BARs, DMA ------------------------------------------------- */
/* Memory decoding on (and the device in D0).  May sleep. */
int pci_enable_device_mem(pci_device_t *dev);
/* Memory decoding and bus mastering off, interrupts released by the
 * driver before.  May sleep. */
void pci_disable_device(pci_device_t *dev);
int pci_is_enabled(const pci_device_t *dev);
void pci_set_master(pci_device_t *dev);
void pci_clear_master(pci_device_t *dev);
/* INTx disable bit (PCI_COMMAND_INTX_DISABLE): enable = 1 clears it. */
void pci_intx(pci_device_t *dev, int enable);
/* Map BAR `bar' uncached (MMIO), at most `maxlen' bytes (0: all of it).
 * NULL on failure.  pci_iomap_wc: write-combining (a CMB, a PMR).  May
 * sleep. */
volatile void *pci_iomap(pci_device_t *dev, int bar, uint64_t maxlen);
volatile void *pci_iomap_wc(pci_device_t *dev, int bar, uint64_t maxlen);
void pci_iounmap(pci_device_t *dev, volatile void *addr);
/* Physical base / length / PCI_BAR_* flags of a BAR (sized once before
 * a driver of the PCI core binds, or when found after boot, and cached; a
 * hot-plug assignment updates the base; a restore keeps it).  Never
 * sizes: safe on a live function, any time; 0 for a function that was
 * never sized. */
uint64_t pci_resource_start(const pci_device_t *dev, int bar);
uint64_t pci_resource_len(const pci_device_t *dev, int bar);
uint32_t pci_resource_flags(const pci_device_t *dev, int bar);
/* DMA addressing limit: the driver states what the device can address
 * (DMA_BIT_MASK(64) / (32)); allocations for it then stay below.  There
 * is no IOMMU: bus address == physical address. */
#define DMA_BIT_MASK(n) (((n) >= 64) ? ~0ULL : ((1ULL << (n)) - 1))
int dma_set_mask_and_coherent(pci_device_t *dev, uint64_t mask);
static inline uint64_t dma_get_mask(const pci_device_t *dev)
{
	return dev->dma_mask ? dev->dma_mask : DMA_BIT_MASK(32);
}

/* ---- interrupt vectors ---------------------------------------------------
 *
 * pci_alloc_irq_vectors_affinity(dev, min, max, flags, affd) allocates
 * between min and max interrupt vectors of one type -- MSI-X first, then
 * MSI, then INTx, restricted to the PCI_IRQ_* types in `flags' -- and
 * returns how many it got (>= min) or -ENOSPC (fewer than min possible),
 * -EINVAL, -ENOMEM.  With PCI_IRQ_AFFINITY and `affd', the vectors are
 * spread over the CPUs the way irq_create_affinity_masks() does it:
 * affd->pre_vectors (e.g. the NVMe admin queue's vector 0) and
 * affd->post_vectors get the default affinity (all online CPUs, not
 * managed); the vectors in between are split into affd->nr_sets sets of
 * set_size[] (calc_sets() may adjust them for the count really obtained)
 * and each set is spread evenly over all CPUs with group_cpus_evenly()
 * (cores sharing a cache kept together); those are MANAGED: their
 * affinity never changes.  MSI multi-message is limited to a power of two
 * and to one target CPU per device (x86 MSI); MSI-X gets per-vector
 * targets.  INTx: exactly one vector.
 *
 * Vector `nr' (0-based) is then an interrupt number pci_irq_vector(dev,
 * nr) for request_irq()/free_irq() (kernel/ke/irq.h), and its CPU mask is
 * pci_irq_get_affinity(dev, nr).  The handler of a managed vector runs on
 * a CPU of that mask.  pci_free_irq_vectors() releases them all (every
 * handler must have been freed).  May sleep.
 */
#define PCI_IRQ_INTX (1u << 0)
#define PCI_IRQ_LEGACY PCI_IRQ_INTX
#define PCI_IRQ_MSI (1u << 1)
#define PCI_IRQ_MSIX (1u << 2)
#define PCI_IRQ_AFFINITY (1u << 3)
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)

/* One allocated vector (the PCI core's; drivers use the accessors). */
struct pci_irq_vec {
	int irq; /* interrupt number (irq.h), -1 unused */
	uint16_t msix_entry; /* MSI-X table entry */
	uint16_t managed; /* affinity fixed by the spread */
	cpumask_t affinity;
};

int pci_alloc_irq_vectors_affinity(pci_device_t *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd);
static inline int pci_alloc_irq_vectors(pci_device_t *dev,
					unsigned int min_vecs,
					unsigned int max_vecs,
					unsigned int flags)
{
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs, flags,
					      NULL);
}
void pci_free_irq_vectors(pci_device_t *dev);
/* Interrupt number of vector `nr', -EINVAL when not allocated. */
int pci_irq_vector(const pci_device_t *dev, unsigned int nr);
/* CPU mask of vector `nr' (NULL when not allocated). */
const cpumask_t *pci_irq_get_affinity(const pci_device_t *dev,
				      unsigned int nr);
/* MSI-X entries the device offers / MSI messages; -EINVAL when none. */
int pci_msix_vec_count(const pci_device_t *dev);
int pci_msi_vec_count(const pci_device_t *dev);
/* Per-vector mask of MSI-X (and per-vector-masking MSI): the vector does
 * not fire while masked; a pending one is delivered on unmask.  Any
 * context. */
void pci_msix_mask_vector(pci_device_t *dev, unsigned int nr);
void pci_msix_unmask_vector(pci_device_t *dev, unsigned int nr);

/* ---- power management and reset ------------------------------------------ */
/* Power states are PCI_D0 .. PCI_D3cold, PCI_UNKNOWN (pci_regs.h). */
typedef int pci_power_t;
/* Move to state (PCI_D0 / PCI_D3hot / PCI_D3cold), with the mandated delays (D3hot->D0 10 ms,
 * no-soft-reset restore of BARs).  May sleep. */
int pci_set_power_state(pci_device_t *dev, pci_power_t state);
/* Wake events from D3 (PME). */
int pci_enable_wake(pci_device_t *dev, pci_power_t state, int enable);
/* Save / restore the standard header, the PCIe capability control
 * registers, MSI/MSI-X state (including every MSI-X table entry and its
 * mask), LTR, L1SS.  May sleep. */
int pci_save_state(pci_device_t *dev);
void pci_restore_state(pci_device_t *dev);
/* Function Level Reset (PCIe DevCtl initiate FLR, waits for pending
 * transactions first, 100 ms after, then for the device to answer config
 * reads with something other than CRS -- up to 60 s).  -ENOTTY when not
 * supported.  May sleep. */
int pcie_flr(pci_device_t *dev);
int pcie_reset_flr(pci_device_t *dev, int probe);
/* Reset the function by the best method available (FLR, AF FLR, PM
 * D3hot->D0 when No_Soft_Reset is clear, secondary bus reset of the
 * parent bridge when the device is alone below it), saving and restoring
 * its state around it and calling the driver's err_handler->reset_prepare
 * / reset_done.  pci_reset_function_locked: the same when the caller
 * already serialises against other resets of the device.  May sleep. */
int pci_reset_function(pci_device_t *dev);
int pci_reset_function_locked(pci_device_t *dev);
/* Secondary bus reset of `bridge'. */
int pci_bridge_secondary_bus_reset(pci_device_t *bridge);
/* Wait (up to `timeout_ms') until the device answers config reads with a
 * valid vendor ID after a reset (CRS / 0xffff handling).  May sleep. */
int pci_dev_wait_ready(pci_device_t *dev, const char *reset_type,
		       int timeout_ms);

/* ---- errors, presence, surprise removal ---------------------------------- */
typedef enum pci_channel_state {
	pci_channel_io_normal = 1, /* I/O channel is in normal state */
	pci_channel_io_frozen = 2, /* I/O to the device is blocked */
	pci_channel_io_perm_failure = 3, /* the device is gone for good */
} pci_channel_state_t;

typedef enum pci_ers_result {
	PCI_ERS_RESULT_NONE = 1, /* no result / none / not supported */
	PCI_ERS_RESULT_CAN_RECOVER = 2, /* device driver can recover */
	PCI_ERS_RESULT_NEED_RESET = 3, /* device driver wants a reset */
	PCI_ERS_RESULT_DISCONNECT = 4, /* device has completely failed */
	PCI_ERS_RESULT_RECOVERED = 5, /* device driver is fully recovered */
	PCI_ERS_RESULT_NO_AER_DRIVER = 6, /* no AER-capable driver */
} pci_ers_result_t;

/* Has the device vanished (hot-removed, or reads all-ones)?  Any
 * context. */
static inline int pci_dev_is_disconnected(const pci_device_t *dev)
{
	return __atomic_load_n(&dev->error_state, __ATOMIC_ACQUIRE) ==
	       pci_channel_io_perm_failure;
}
/* Frozen or gone: no MMIO may be expected to work. */
static inline int pci_channel_offline(const pci_device_t *dev)
{
	return __atomic_load_n(&dev->error_state, __ATOMIC_ACQUIRE) !=
		       pci_channel_io_normal &&
	       __atomic_load_n(&dev->error_state, __ATOMIC_ACQUIRE) != 0;
}
/* Is the device still there (a config read of the vendor ID answers)?
 * Marks it disconnected when not.  Any context. */
int pci_device_is_present(pci_device_t *dev);
/* The PCI core's own record of a surprise removal (all-ones). */
void pci_dev_set_disconnected(pci_device_t *dev);

/* ---- driver model --------------------------------------------------------- */
#define PCI_ANY_ID (~0u)

struct pci_device_id {
	uint32_t vendor, device; /* PCI_ANY_ID matches any */
	uint32_t subvendor, subdevice; /* PCI_ANY_ID matches any */
	uint32_t class, class_mask; /* (class_full & class_mask) == class */
	uint64_t driver_data; /* the driver's (quirk flags) */
};

#define PCI_DEVICE(vend, dev)                                            \
	.vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID,      \
	.subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, subvend, subdev)                       \
	.vendor = (vend), .device = (dev), .subvendor = (subvend),       \
	.subdevice = (subdev)
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask)                      \
	.vendor = PCI_ANY_ID, .device = PCI_ANY_ID,                      \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID,                \
	.class = (dev_class), .class_mask = (dev_class_mask)
#define PCI_VDEVICE(vend, dev)                                           \
	.vendor = PCI_VENDOR_ID_##vend, .device = (dev),                 \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

struct pci_error_handlers {
	/* An error was reported (state normal/frozen/perm_failure). */
	pci_ers_result_t (*error_detected)(pci_device_t *dev,
					   pci_channel_state_t state);
	/* MMIO works again (after CAN_RECOVER). */
	pci_ers_result_t (*mmio_enabled)(pci_device_t *dev);
	/* The slot was reset (after NEED_RESET). */
	pci_ers_result_t (*slot_reset)(pci_device_t *dev);
	/* Around pci_reset_function(). */
	void (*reset_prepare)(pci_device_t *dev);
	void (*reset_done)(pci_device_t *dev);
	/* Normal I/O may resume. */
	void (*resume)(pci_device_t *dev);
	/* A correctable error was logged (informational). */
	void (*cor_error_detected)(pci_device_t *dev);
};

struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table; /* ends with a zero entry */
	/* Bind: 0 claims the device.  Called from pci_register_driver()
	 * for devices present (boot thread or the caller's context) and
	 * for devices found later (a VMD domain).  May sleep.  A driver
	 * that brings the device up asynchronously (NVMe: reset_work)
	 * returns 0 and finishes in its own work item. */
	int (*probe)(pci_device_t *dev, const struct pci_device_id *id);
	/* Unbind (driver unload, hot-removal -- then the device may be
	 * gone already: pci_dev_is_disconnected()).  May sleep. */
	void (*remove)(pci_device_t *dev);
	/* System shutdown/reboot (pci_shutdown_drivers()).  May sleep. */
	void (*shutdown)(pci_device_t *dev);
	/* System suspend/resume of the device.  May sleep. */
	int (*suspend)(pci_device_t *dev);
	int (*resume)(pci_device_t *dev);
	const struct pci_error_handlers *err_handler;
	struct pci_driver *next; /* the PCI core's list */
};

/* Register: probe() every unbound matching device now.  Returns 0 (also
 * when no device matched) or -errno.  May sleep. */
int pci_register_driver(struct pci_driver *drv);
/* remove() every bound device, then forget the driver.  May sleep. */
void pci_unregister_driver(struct pci_driver *drv);
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
					 const pci_device_t *dev);
static inline void pci_set_drvdata(pci_device_t *dev, void *data)
{
	dev->driver_data = data;
}
static inline void *pci_get_drvdata(const pci_device_t *dev)
{
	return dev->driver_data;
}
/* shutdown() of every bound driver, in reverse probe order (system
 * shutdown path, after blk_shutdown_all).  May sleep. */
void pci_shutdown_drivers(void);
/* "SSSS:BB:DD.F" into out (at least 13 bytes; 14 for a VMD domain,
 * whose segment has five hex digits -- 16 is always enough). */
void pci_name(const pci_device_t *dev, char *out, size_t cap);
/* The non-const pointer of a device handed out by the enumeration. */
pci_device_t *pci_dev_mut(const pci_device_t *dev);

/* ---- removal ---------------------------------------------------------------- */
/* Remove `dev' (and the devices below it if a bridge): driver remove(),
 * then the device is forgotten.  May sleep. */
void pci_stop_and_remove_bus_device(pci_device_t *dev);
/* Intel VMD domains: enumerate devices behind every VMD controller. */
void pci_vmd_init(void);

#endif // LIKEOS_PCI_H
