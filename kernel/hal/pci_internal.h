// LikeOS - PCI core internals shared by kernel/hal/pci*.c
//
// Not for drivers: the driver-facing interface is kernel/hal/pci.h.
//
// The pieces:
//   * configuration access: per-segment accessor tables -- port I/O
//     (mechanism #1) for the first 256 bytes of segment 0, ECAM for the
//     rest and for every other MCFG segment, a VMD domain's own window
//     for the segments VMD creates;
//   * the device table (the fixed array the old pci_get_devices() hands
//     out; hot-plugged and VMD devices are appended to it) with the core's
//     private per-device state beside it;
//   * the topology lock and the per-device lock (driver callbacks);
//   * cross-file hooks of the MSI, PM and VMD parts.
//
// Lock order (outermost first): g_pci_rescan_mutex -> per-device lock
// (pci_dev_lock) -> the driver's own locks -> the config spinlock /
// MSI-X table lock / interrupt-core locks.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_PCI_INTERNAL_H
#define KERNEL_HAL_PCI_INTERNAL_H

#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/pci_capwalk.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/list.h>

/* devfn packing */
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)

/* An all-ones read: the device did not answer. */
#define PCI_ERROR_RESPONSE (~0u)
#define PCI_POSSIBLE_ERROR(val) ((val) == (__typeof__(val))~0ULL)

/* Segment numbers a VMD domain is given (above every MCFG segment). */
#define PCI_VMD_DOMAIN_BASE 0x10000u

/* ---- configuration access ----------------------------------------------- */

/* One segment's accessors: `where' 0..4095, size 1/2/4 (aligned). */
struct pci_cfg_ops {
	int (*read)(void *ctx, uint32_t segment, uint8_t bus, uint8_t devfn,
		    int where, int size, uint32_t *val);
	int (*write)(void *ctx, uint32_t segment, uint8_t bus, uint8_t devfn,
		     int where, int size, uint32_t val);
	void *ctx;
};

/* Raw access to any segment/bus/devfn (no device record needed: the
 * scan uses it).  Return PCIBIOS_*; a read that fails gives all-ones. */
int pci_raw_read(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		 int size, uint32_t *val);
int pci_raw_write(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		  int size, uint32_t val);
/* Port I/O mechanism #1 (segment 0, where < 256). */
int pci_conf1_read(uint8_t bus, uint8_t devfn, int where, int size,
		   uint32_t *val);
int pci_conf1_write(uint8_t bus, uint8_t devfn, int where, int size,
		    uint32_t val);
/* Register the accessors of a segment (VMD domains).  0 / -EBUSY. */
int pci_register_segment_ops(uint32_t segment, const struct pci_cfg_ops *ops);
void pci_unregister_segment_ops(uint32_t segment);

/* ECAM (pci_ecam.c). */
int pci_ecam_read(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		  int size, uint32_t *val);
int pci_ecam_write(uint32_t segment, uint8_t bus, uint8_t devfn, int where,
		   int size, uint32_t val);
/* Does an MCFG region cover (segment, bus)? */
int pci_ecam_covers(uint32_t segment, uint8_t bus);
/* Make sure the bus is mapped (process context; boot or rescan). */
int pci_ecam_map_bus(uint32_t segment, uint8_t bus);
/* Segments the MCFG table describes (bitmap of the first 64 numbers is
 * enough for every machine LikeOS meets; more are logged and ignored). */
int pci_ecam_segment_at(int idx, uint32_t *segment, uint8_t *bus_start,
			uint8_t *bus_end);
/* ECAM byte offset math: pci_ecam_offset() in pci_capwalk.h. */

/* ---- the device table ------------------------------------------------- */

/* Core-private state beside each pci_device_t (same index). */
struct pci_dev_priv {
	struct mutex dev_lock; /* driver callbacks, resets */
	int lock_ready;
	int in_table; /* the slot holds a device (live or removed) */
	/* MSI / MSI-X */
	int msi_enabled, msix_enabled;
	uint8_t msi_multiple; /* log2 of messages enabled */
	uint8_t msi_64bit, msi_maskbit;
	uint32_t msi_mask_cache; /* per-vector mask bits written */
	uint32_t msix_table_phys_off; /* table offset in its BAR */
	uint8_t msix_bir;
	uint32_t msix_table_bytes;
	volatile uint32_t *msix_map; /* the mapping (msix_table aliases it) */
	uint64_t msix_map_virt; /* page-aligned mapping and its pages */
	size_t msix_map_pages;
	uint32_t *msix_ctrl_cache; /* vector control word per entry */
	void *msi_vecdata; /* chip data per allocated vector */
	unsigned int msi_hw_vecs; /* MSI messages enabled (power of 2) */
	int intx_irq; /* INTx descriptor, 0 none */
	int intx_gsi; /* routed GSI, -1 none */
	/* Cached BAR decode (pci.c: sized once before a driver binds, kept
	 * across a restore; g_pci_bars_lock).  bar_bus: the base as the bus sees it
	 * (differs from bars[].base in a VMD domain). */
	struct pci_bar bars[6];
	uint64_t bar_bus[6];
	int bars_valid;
	/* Bridge windows (type 1 header): the address ranges below. */
	uint8_t secondary, subordinate;
	/* VMD: the domain this device sits in (NULL: none). */
	void *vmd;
	/* Reset method bits (PCI_RESET_*). */
	uint32_t reset_methods;
	int imm_ready; /* Immediate Readiness */
	int no_soft_reset_known;
	int pme_support; /* PCI_PM_CAP_PME_* >> shift */
	int d1_support, d2_support;
	int pme_poll;
	/* ASPM state as found (for pci_disable_link_state()). */
	uint16_t aspm_lnkctl_orig;
	int probe_order; /* order in which the driver bound */
	char name[20];
};

/* pci_save_state() (pci_pm.c). */
struct pci_saved_state {
	uint32_t config[16];
	uint16_t pcie[7]; /* DEVCTL LNKCTL SLTCTL RTCTL DEVCTL2 LNKCTL2 SLTCTL2 */
	int pcie_valid;
	uint32_t ltr; /* max snoop | max no-snoop << 16 */
	uint32_t l1ss_ctl1, l1ss_ctl2;
	uint16_t dpc_ctl;
	uint16_t acs_ctrl;
};

#define PCI_RESET_FLR (1u << 0)
#define PCI_RESET_AF_FLR (1u << 1)
#define PCI_RESET_PM (1u << 2)
#define PCI_RESET_BUS (1u << 3)

extern struct mutex g_pci_rescan_mutex;
extern spinlock_t g_pci_cfg_lock; /* port I/O and narrow ECAM RMW */

pci_device_t *pci_dev_table(int *count);
struct pci_dev_priv *pci_priv(const pci_device_t *dev);
/* Index of a device in the table, -1 when the pointer is not one. */
int pci_dev_index(const pci_device_t *dev);
/* Find a recorded device. */
pci_device_t *pci_get_domain_bus_and_slot(uint32_t segment, uint8_t bus,
					  uint8_t devfn);
/* Probe (segment, bus, devfn) and record it when something answers (and
 * it is not recorded yet).  Fills every field (capabilities, cfg size,
 * PM state, bridge numbers, the upstream bridge).  Returns the device,
 * NULL when nothing is there or the table is full. */
pci_device_t *pci_scan_single_device(uint32_t segment, uint8_t bus,
				     uint8_t devfn);
/* Scan a whole bus (all 32 slots, multi-function aware), recursing into
 * bridges that already have a secondary bus number.  Returns the number
 * of NEW devices.  `assign_busnr': give unconfigured bridges bus numbers
 * from *next_bus (<= max_bus) -- the VMD domain and hot-plug. */
int pci_scan_bus(uint32_t segment, uint8_t bus, int assign_busnr,
		 unsigned int *next_bus, unsigned int max_bus);
/* (Re)fill the derived fields of a recorded device (after ECAM became
 * available, after a reset). */
void pci_setup_device(pci_device_t *dev);
/* The bridge whose secondary bus is `bus' in `segment', NULL on a root
 * bus. */
pci_device_t *pci_upstream_bridge_of(uint32_t segment, uint8_t bus);
static inline pci_device_t *pci_upstream_bridge(const pci_device_t *dev)
{
	return dev->bus_self;
}
int pci_is_bridge(const pci_device_t *dev);
/* Devices whose upstream bridge is `bridge' (direct children). */
int pci_bridge_children(const pci_device_t *bridge, pci_device_t **out,
			int max);
/* Size the BARs of a function once (no-op when sized already, or for a
 * bridge-class function): before a driver binds, or when the function
 * is found after boot. */
void pci_bars_ensure(pci_device_t *dev);
/* Size the BARs of a function again after they were assigned anew; only
 * before any driver bound (WARNs and does nothing otherwise). */
void pci_bars_refresh(pci_device_t *dev);
/* Forget a device (it was removed): the table slot is marked removed.
 * Process context, may sleep. */
void pci_dev_forget(pci_device_t *dev);

/* Per-device lock (driver callbacks: probe, remove, error handlers,
 * reset). */
void pci_dev_lock(pci_device_t *dev);
void pci_dev_unlock(pci_device_t *dev);
int pci_dev_trylock(pci_device_t *dev);

/* Assign memory BARs (and bridge windows of bridges, depth first) for a
 * root bus with explicit windows (a VMD domain's MEMBARs, as bus
 * addresses): every function on (segment, bus) and below.  0 / -ENOSPC. */
int pci_assign_bus_resources(uint32_t segment, uint8_t bus, uint64_t np_base,
			     uint64_t np_limit, uint64_t pf_base,
			     uint64_t pf_limit, int pf64);
/* The memory windows (non-prefetchable / prefetchable) of a bridge:
 * returns 0 and fills base/limit (limit < base: closed). */
int pci_bridge_window(const pci_device_t *bridge, int prefetch,
		      uint64_t *base, uint64_t *limit);

/* PCI_COMMAND = (PCI_COMMAND & ~clear) | set as one read-modify-write
 * under the command-register lock (every update of the register that
 * can race another -- enable, bus mastering, the INTx mask from hard IRQ
 * context -- goes through here), written only when it changes.  The
 * value before the update in *old (when non-NULL).  0, or -ENODEV when
 * the device answers all-ones.  Any context. */
int pci_command_update(const pci_device_t *dev, uint16_t clear, uint16_t set,
		       uint16_t *old);

/* INTx routing (ACPI _PRT through the bridge swizzle). */
int pci_intx_gsi(const pci_device_t *dev, int *active_low, int *level);

/* Sleep (process context) or busy-wait (boot). */
void pci_msleep(unsigned int ms);
void pci_udelay(unsigned int us);
int pci_can_sleep(void);

/* ---- cross-file hooks --------------------------------------------------- */

/* pci_msi.c */
void pci_msi_init_dev(pci_device_t *dev);
void pci_msi_save_state(pci_device_t *dev);
void pci_msi_restore_state(pci_device_t *dev);
/* Device gone: release what the interrupt core holds for it. */
void pci_msi_dev_removed(pci_device_t *dev);
volatile uint32_t *pci_msix_map_table(pci_device_t *dev);

/* pci_pm.c */
void pci_pm_init_dev(pci_device_t *dev);
void pci_init_reset_methods(pci_device_t *dev);
/* Bridge: wait for the devices below after a reset / link up. */
int pci_bridge_wait_for_secondary_bus(pci_device_t *bridge,
				      const char *reset_type);

/* pci_driver.c */
/* A new device: bind a registered driver to it (rescan / hot-plug /
 * VMD).  May sleep. */
void pci_bus_add_device(pci_device_t *dev);
/* Unbind the driver of `dev' (remove()), if any.  May sleep. */
void pci_device_release_driver(pci_device_t *dev);
/* Call fn for every device below `bridge' (children first when
 * `post_order'), including bridge itself when `self'. */
void pci_walk_bridge(pci_device_t *bridge, int self, int post_order,
		     int (*fn)(pci_device_t *dev, void *data), void *data);

/* pci_vmd.c */
/* Is `segment' a VMD domain? */
int pci_vmd_owns_segment(uint32_t segment);
/* Are the MSI/MSI-X messages of `dev' remapped by its VMD (delivered
 * through the VMD's own vectors)? */
int pci_vmd_msi_remapped(const pci_device_t *dev);
/* The VMD vector a new child interrupt goes through (NVMe functions are
 * spread over the vectors after the first, everything else uses the
 * first); sets up the VMD's own vectors on first use.  >= 0 or -errno.
 * May sleep. */
int pci_vmd_irq_assign(const pci_device_t *dev);
void pci_vmd_irq_unassign(const pci_device_t *dev, int index);
/* A started child interrupt joins / leaves the list the VMD vector's
 * handler runs.  May sleep (process context). */
void pci_vmd_irq_enable(const pci_device_t *dev, int index, int irq);
void pci_vmd_irq_disable(const pci_device_t *dev, int index, int irq);
/* Bus address (what a BAR holds) -> CPU physical address in a VMD domain
 * (the MEMBAR shadow offsets of a passed-through VMD). */
uint64_t pci_vmd_bus_to_cpu(uint32_t segment, uint64_t bus_addr);

/* Logging */
#define pci_info(dev, fmt, ...)                                         \
	kprintf("pci %s: " fmt, pci_priv(dev) ? pci_priv(dev)->name : "?", \
		##__VA_ARGS__)
#define pci_warn(dev, fmt, ...)                                         \
	kprintf("pci %s: warning: " fmt,                                \
		pci_priv(dev) ? pci_priv(dev)->name : "?", ##__VA_ARGS__)
#define pci_err(dev, fmt, ...)                                          \
	kprintf("pci %s: error: " fmt,                                  \
		pci_priv(dev) ? pci_priv(dev)->name : "?", ##__VA_ARGS__)

#endif /* KERNEL_HAL_PCI_INTERNAL_H */
