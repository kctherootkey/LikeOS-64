// LikeOS-64 - HID over I2C Driver
// Intel DesignWare I2C controller + Microsoft HID-over-I2C protocol
//
// Discovers Intel LPSS Serial IO I2C controllers via PCI, initializes the
// DesignWare I2C IP core, scans I2C buses for HID devices, and injects
// mouse/touchpad input into the existing input subsystems.
//
// I2C transfers use standard mode (100kHz) with interrupt-driven GPIO
// handling.  Worker threads perform I2C reads; no polling from main loop.

#include "../../include/kernel/i2c_hid.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/acpi.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/ioapic.h"
#include "../../include/kernel/lapic.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/timer.h"

// Debug counters for interrupt-driven I2C HID
static volatile uint32_t g_dbg_gpio_isr_count = 0;     // Total GPIO ISR invocations
static volatile uint32_t g_dbg_gpio_isr_hit = 0;       // ISR found pending bit
static volatile uint32_t g_dbg_gpio_isr_miss = 0;      // ISR: no matching pending bit
static volatile uint32_t g_dbg_worker_wake = 0;        // Worker woke up
static volatile uint32_t g_dbg_worker_read = 0;        // Worker did an I2C read
static volatile uint32_t g_dbg_worker_xfer_err = 0;    // I2C transfer errors
static volatile uint32_t g_dbg_worker_null_pkt = 0;    // Null/short packets discarded
static volatile uint32_t g_dbg_worker_null_id = 0;     // Null report ID discarded
static volatile uint32_t g_dbg_worker_id_mismatch = 0; // Report ID mismatch
static volatile uint32_t g_dbg_worker_processed = 0;   // Reports processed into mouse
static volatile uint32_t g_dbg_worker_no_finger = 0;   // Reports skipped (no finger)
static volatile uint32_t g_dbg_ie_reenable = 0;        // GPI_IE re-enabled count
static volatile uint32_t g_dbg_i2c_isr_count = 0;      // Total I2C controller ISR invocations
static volatile uint32_t g_dbg_xfer_abort_count = 0;   // Total xfer abort events
static volatile uint32_t g_dbg_xfer_retry_count = 0;   // Total xfer retries
static volatile uint32_t g_dbg_xfer_total = 0;         // Total xfer calls
static volatile uint32_t g_dbg_xfer_ok = 0;            // Successful xfers
static volatile uint64_t g_dbg_last_print_uptime = 0;  // Last periodic debug print time

// ============================================================================
// Private data
// ============================================================================

static i2c_dw_controller_t g_i2c_controllers[I2C_DW_MAX_CONTROLLERS];
static int g_i2c_controller_count = 0;

static i2c_hid_device_t g_i2c_hid_devices[I2C_HID_MAX_DEVICES];
static int g_i2c_hid_device_count = 0;

// GPIO interrupt state
static gpio_community_t g_gpio_communities[GPIO_MAX_COMMUNITIES];
static int g_gpio_community_count = 0;
static uint64_t g_sbreg_bar = 0;     // P2SB sideband register BAR (physical)
static const gpio_platform_def_t *g_gpio_platform = NULL; // Detected platform
static uint8_t g_gpio_ioapic_polarity = 0; // From GPIO controller _CRS (0=high, 1=low)
static uint8_t g_gpio_ioapic_trigger  = 0; // From GPIO controller _CRS (0=level, 1=edge)

// ============================================================================
// Intel GPIO platform tables (derived from Linux pinctrl-intel drivers)
// Each INTEL_GPP(reg_num, base, last, gpio_base) defines a padgroup:
//   reg_num   = GPI_IS register index within community
//   base      = first sequential pin
//   last      = last sequential pin (size = last - base + 1)
//   gpio_base = ACPI GpioInt pin number base (or GPIO_NOMAP)
// ============================================================================

#define GPP(rn, b, e, gb) { .reg_num = (rn), .base = (b), .size = (uint8_t)((e)-(b)+1), .gpio_base = (gb) }

static const gpio_platform_def_t g_gpio_platforms[] = {
    // Meteor Lake-P (INTC105E, INTC1083)
    {
        .acpi_hids = { "INTC105E", "INTC1083", NULL },
        .gpi_is_offset = 0x200, .gpi_ie_offset = 0x210,
        .hostsw_own_offset = 0x140,
        .ncommunities = 5,
        .communities = {
            { .pin_base = 0, .npins = 53, .ngpps = 3, .gpps = {
                GPP(0, 0, 4, 0),        // CPU
                GPP(1, 5, 28, 32),       // GPP_V
                GPP(2, 29, 52, 64),      // GPP_C
            }},
            { .pin_base = 53, .npins = 50, .ngpps = 2, .gpps = {
                GPP(0, 53, 77, 96),      // GPP_A
                GPP(1, 78, 102, 128),    // GPP_E
            }},
            { .pin_base = 103, .npins = 81, .ngpps = 4, .gpps = {
                GPP(0, 103, 128, 160),   // GPP_H
                GPP(1, 129, 154, 192),   // GPP_F
                GPP(2, 155, 169, 224),   // SPI0
                GPP(3, 170, 183, 256),   // vGPIO_3
            }},
            { .pin_base = 184, .npins = 20, .ngpps = 2, .gpps = {
                GPP(0, 184, 191, 288),   // GPP_S
                GPP(1, 192, 203, 320),   // JTAG
            }},
            { .pin_base = 204, .npins = 85, .ngpps = 4, .gpps = {
                GPP(0, 204, 228, 352),   // GPP_B
                GPP(1, 229, 253, 384),   // GPP_D
                GPP(2, 254, 285, 416),   // vGPIO_0
                GPP(3, 286, 288, 448),   // vGPIO_1
            }},
        },
    },
    // Meteor Lake-S (INTC1082)
    {
        .acpi_hids = { "INTC1082", NULL },
        .gpi_is_offset = 0x200, .gpi_ie_offset = 0x210,
        .hostsw_own_offset = 0x110,
        .ncommunities = 3,
        .communities = {
            { .pin_base = 0, .npins = 74, .ngpps = 3, .gpps = {
                GPP(0, 0, 27, 0),        // GPP_A
                GPP(1, 28, 46, 32),      // vGPIO_0
                GPP(2, 47, 73, 64),      // GPP_C
            }},
            { .pin_base = 74, .npins = 46, .ngpps = 3, .gpps = {
                GPP(0, 74, 93, 96),      // GPP_B
                GPP(1, 94, 95, 128),     // vGPIO_3
                GPP(2, 96, 119, 160),    // GPP_D
            }},
            { .pin_base = 120, .npins = 28, .ngpps = 2, .gpps = {
                GPP(0, 120, 135, 192),   // JTAG_CPU
                GPP(1, 136, 147, 224),   // vGPIO_4
            }},
        },
    },
    // Tiger Lake-LP (INT34C5, INTC1055)
    {
        .acpi_hids = { "INT34C5", "INTC1055", NULL },
        .gpi_is_offset = 0x100, .gpi_ie_offset = 0x120,
        .hostsw_own_offset = 0x0b0,
        .ncommunities = 4,
        .communities = {
            { .pin_base = 0, .npins = 67, .ngpps = 3, .gpps = {
                GPP(0, 0, 25, 0),        // GPP_B
                GPP(1, 26, 41, 32),      // GPP_T
                GPP(2, 42, 66, 64),      // GPP_A
            }},
            { .pin_base = 67, .npins = 104, .ngpps = 5, .gpps = {
                GPP(0, 67, 74, 96),      // GPP_S
                GPP(1, 75, 98, 128),     // GPP_H
                GPP(2, 99, 119, 160),    // GPP_D
                GPP(3, 120, 143, 192),   // GPP_U
                GPP(4, 144, 170, 224),   // vGPIO
            }},
            { .pin_base = 171, .npins = 89, .ngpps = 5, .gpps = {
                GPP(0, 171, 194, 256),   // GPP_C
                GPP(1, 195, 219, 288),   // GPP_F
                GPP(2, 220, 225, GPIO_NOMAP), // HVCMOS
                GPP(3, 226, 250, 320),   // GPP_E
                GPP(4, 251, 259, GPIO_NOMAP), // JTAG
            }},
            { .pin_base = 260, .npins = 17, .ngpps = 2, .gpps = {
                GPP(0, 260, 267, 352),   // GPP_R
                GPP(1, 268, 276, GPIO_NOMAP), // SPI
            }},
        },
    },
    // Tiger Lake-H (INT34C6)
    {
        .acpi_hids = { "INT34C6", NULL },
        .gpi_is_offset = 0x100, .gpi_ie_offset = 0x120,
        .hostsw_own_offset = 0x0c0,
        .ncommunities = 5,
        .communities = {
            { .pin_base = 0, .npins = 79, .ngpps = 4, .gpps = {
                GPP(0, 0, 24, 0),        // GPP_A
                GPP(1, 25, 44, 32),      // GPP_R
                GPP(2, 45, 70, 64),      // GPP_B
                GPP(3, 71, 78, 96),      // vGPIO_0
            }},
            { .pin_base = 79, .npins = 102, .ngpps = 5, .gpps = {
                GPP(0, 79, 104, 128),    // GPP_D
                GPP(1, 105, 128, 160),   // GPP_C
                GPP(2, 129, 136, 192),   // GPP_S
                GPP(3, 137, 153, 224),   // GPP_G
                GPP(4, 154, 180, 256),   // vGPIO
            }},
            { .pin_base = 181, .npins = 37, .ngpps = 2, .gpps = {
                GPP(0, 181, 193, 288),   // GPP_E
                GPP(1, 194, 217, 320),   // GPP_F
            }},
            { .pin_base = 218, .npins = 49, .ngpps = 3, .gpps = {
                GPP(0, 218, 241, 352),   // GPP_H
                GPP(1, 242, 251, 384),   // GPP_J
                GPP(2, 252, 266, 416),   // GPP_K
            }},
            { .pin_base = 267, .npins = 24, .ngpps = 2, .gpps = {
                GPP(0, 267, 281, 448),   // GPP_I
                GPP(1, 282, 290, GPIO_NOMAP), // JTAG
            }},
        },
    },
    // Alder Lake-N (INTC1057)
    {
        .acpi_hids = { "INTC1057", NULL },
        .gpi_is_offset = 0x100, .gpi_ie_offset = 0x120,
        .hostsw_own_offset = 0x0b0,
        .ncommunities = 4,
        .communities = {
            { .pin_base = 0, .npins = 67, .ngpps = 3, .gpps = {
                GPP(0, 0, 25, 0),        // GPP_B
                GPP(1, 26, 41, 32),      // GPP_T
                GPP(2, 42, 66, 64),      // GPP_A
            }},
            { .pin_base = 67, .npins = 102, .ngpps = 5, .gpps = {
                GPP(0, 67, 74, 96),      // GPP_S
                GPP(1, 75, 94, 128),     // GPP_I
                GPP(2, 95, 118, 160),    // GPP_H
                GPP(3, 119, 139, 192),   // GPP_D
                GPP(4, 140, 168, 224),   // vGPIO
            }},
            { .pin_base = 169, .npins = 80, .ngpps = 4, .gpps = {
                GPP(0, 169, 192, 256),   // GPP_C
                GPP(1, 193, 217, 288),   // GPP_F
                GPP(2, 218, 223, GPIO_NOMAP), // HVCMOS
                GPP(3, 224, 248, 320),   // GPP_E
            }},
            { .pin_base = 249, .npins = 8, .ngpps = 1, .gpps = {
                GPP(0, 249, 256, 352),   // GPP_R
            }},
        },
    },
    // Alder Lake-S (INTC1056, INTC1085)
    {
        .acpi_hids = { "INTC1056", "INTC1085", NULL },
        .gpi_is_offset = 0x200, .gpi_ie_offset = 0x220,
        .hostsw_own_offset = 0x150,
        .ncommunities = 5,
        .communities = {
            { .pin_base = 0, .npins = 95, .ngpps = 5, .gpps = {
                GPP(0, 0, 24, 0),        // GPP_I
                GPP(1, 25, 47, 32),      // GPP_R
                GPP(2, 48, 59, 64),      // GPP_J
                GPP(3, 60, 86, 96),      // vGPIO
                GPP(4, 87, 94, 128),     // vGPIO_0
            }},
            { .pin_base = 95, .npins = 56, .ngpps = 3, .gpps = {
                GPP(0, 95, 118, 160),    // GPP_B
                GPP(1, 119, 126, 192),   // GPP_G
                GPP(2, 127, 150, 224),   // GPP_H
            }},
            { .pin_base = 151, .npins = 49, .ngpps = 3, .gpps = {
                GPP(0, 151, 159, GPIO_NOMAP), // SPI0
                GPP(1, 160, 175, 256),   // GPP_A
                GPP(2, 176, 199, 288),   // GPP_C
            }},
            { .pin_base = 200, .npins = 70, .ngpps = 4, .gpps = {
                GPP(0, 200, 207, 320),   // GPP_S
                GPP(1, 208, 230, 352),   // GPP_E
                GPP(2, 231, 245, 384),   // GPP_K
                GPP(3, 246, 269, 416),   // GPP_F
            }},
            { .pin_base = 270, .npins = 34, .ngpps = 2, .gpps = {
                GPP(0, 270, 294, 448),   // GPP_D
                GPP(1, 295, 303, GPIO_NOMAP), // JTAG
            }},
        },
    },
};

#define GPIO_NUM_PLATFORMS (sizeof(g_gpio_platforms) / sizeof(g_gpio_platforms[0]))

// LPSS I2C uses the DesignWare block at 0x000-0x1FF and LPSS private regs at
// 0x200-0x2FF. One 4KB page covers the entire window we access here.

#define I2C_LPSS_MMIO_PAGES 1

// ============================================================================
// Utility functions
// ============================================================================

static void i2c_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

static void i2c_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void i2c_delay_us(int us) {
    // Approximate microsecond delay via busy loop
    // At ~2 GHz, ~2000 iterations per us
    for (volatile int i = 0; i < us * 500; i++) {
        __asm__ volatile("nop");
    }
}

// ============================================================================
// DesignWare I2C Controller - Low-level register access
// ============================================================================

static inline uint32_t dw_read(i2c_dw_controller_t *ctrl, uint32_t reg) {
    return *(volatile uint32_t *)((uint8_t *)ctrl->base + reg);
}

static inline void dw_write(i2c_dw_controller_t *ctrl, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)ctrl->base + reg) = val;
}

// Read LPSS private register (at base + 0x200 + offset)
static inline uint32_t lpss_read(i2c_dw_controller_t *ctrl, uint32_t reg) {
    return *(volatile uint32_t *)((uint8_t *)ctrl->base + 0x200 + reg - 0x200);
}

static inline void lpss_write(i2c_dw_controller_t *ctrl, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)ctrl->base + reg) = val;
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t off)
{
    uint32_t value = pci_cfg_read32(bus, device, function, off & 0xFC);
    uint32_t shift = (off & 0x2) ? 16 : 0;
    return (uint16_t)((value >> shift) & 0xFFFF);
}

static void pci_cfg_write16(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t off, uint16_t value)
{
    uint8_t aligned = off & 0xFC;
    uint32_t shift = (off & 0x2) ? 16 : 0;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t current = pci_cfg_read32(bus, device, function, aligned);
    uint32_t new_value = (current & ~mask) | ((uint32_t)value << shift);
    pci_cfg_write32(bus, device, function, aligned, new_value);
}

// ============================================================================
// ECAM (PCIe memory-mapped config) — needed for proper PMC power notification
//
// On Arrow Lake the PMC monitors power-state transitions via the PCIe fabric.
// Writes through the legacy CF8/CFC I/O-port path may not trigger the sideband
// message that tells the PMC to ungate a device's power island.  Using ECAM
// (the memory-mapped config region advertised in the ACPI MCFG table) ensures
// the write goes through the PCIe fabric and the PMC sees it.
// ============================================================================

static volatile uint8_t *g_ecam_bus0_va;  // VA covering bus 0 ECAM (1 MB)

static int ecam_init_bus0(void)
{
    acpi_sdt_header_t *mcfg = acpi_find_table("MCFG");
    if (!mcfg) {
        kprintf("[I2C-ECAM] no MCFG table\n");
        return -1;
    }

    // MCFG layout: 36-byte SDT header + 8 reserved bytes + 16-byte allocs
    uint8_t *data = (uint8_t *)mcfg + 44;
    uint64_t base_phys;
    i2c_memcpy(&base_phys, data, 8);
    uint16_t seg;
    i2c_memcpy(&seg, data + 8, 2);
    uint8_t start_bus = data[10];
    uint8_t end_bus   = data[11];

    kprintf("[I2C-ECAM] MCFG base=0x%llx seg=%u bus=%u-%u\n",
            (unsigned long long)base_phys, seg, start_bus, end_bus);

    if (start_bus > 0 || base_phys == 0) {
        kprintf("[I2C-ECAM] bus 0 not in range or base invalid\n");
        return -1;
    }

    // Map bus 0: 32 devs * 8 funcs * 4 KB = 1 MB = 256 pages
    uint64_t va = mm_map_device_mmio(base_phys, 256);
    if (!va) {
        kprintf("[I2C-ECAM] mm_map_device_mmio failed\n");
        return -1;
    }
    g_ecam_bus0_va = (volatile uint8_t *)va;
    return 0;
}

// Return pointer into the ECAM mapping for (bus=0, dev, func, offset).
// Falls back to NULL if ECAM is not available.
static inline volatile void *ecam_addr(uint8_t dev, uint8_t func, uint16_t off)
{
    if (!g_ecam_bus0_va) return NULL;
    return g_ecam_bus0_va + ((uint32_t)dev << 15) +
                            ((uint32_t)func << 12) + off;
}

// ECAM config accessors — fall back to CF8/CFC when ECAM is unavailable.
static uint32_t ecam_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off)
{
    if (bus == 0) {
        volatile uint32_t *p = (volatile uint32_t *)ecam_addr(dev, func, off & ~3u);
        if (p) return *p;
    }
    return pci_cfg_read32(bus, dev, func, (uint8_t)off);
}

static void ecam_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t val)
{
    if (bus == 0) {
        volatile uint32_t *p = (volatile uint32_t *)ecam_addr(dev, func, off & ~3u);
        if (p) { *p = val; return; }
    }
    pci_cfg_write32(bus, dev, func, (uint8_t)off, val);
}

static uint16_t ecam_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off)
{
    if (bus == 0) {
        volatile uint16_t *p = (volatile uint16_t *)ecam_addr(dev, func, off & ~1u);
        if (p) return *p;
    }
    return pci_cfg_read16(bus, dev, func, (uint8_t)off);
}

static void ecam_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t val)
{
    if (bus == 0) {
        volatile uint16_t *p = (volatile uint16_t *)ecam_addr(dev, func, off & ~1u);
        if (p) { *p = val; return; }
    }
    pci_cfg_write16(bus, dev, func, (uint8_t)off, val);
}

// ============================================================================
// DesignWare I2C Controller - Initialization
// ============================================================================

static int dw_i2c_disable(i2c_dw_controller_t *ctrl) {
    dw_write(ctrl, DW_IC_ENABLE, 0);

    // On Arrow Lake, IC_ENABLE_STATUS (0x9C) reads as SRAM garbage when
    // the DW core's combinational logic isn't clocked. Use IC_ENABLE (0x6C)
    // readback as fallback — it's a R/W register that always works.
    for (int i = 0; i < 10000; i++) {
        uint32_t en_st = dw_read(ctrl, DW_IC_ENABLE_STATUS);
        if ((en_st & DW_IC_EN_STATUS_IC_EN) == 0)
            return 0;
        // Fallback: if IC_ENABLE reads back 0, assume disabled
        uint32_t en_reg = dw_read(ctrl, DW_IC_ENABLE);
        if (en_reg == 0) {
            i2c_delay_us(100);
            return 0;
        }
        i2c_delay_us(1);
    }
    // Even on timeout, the write probably took effect — proceed anyway
    i2c_delay_us(1000);
    return 0;
}

static int dw_i2c_enable(i2c_dw_controller_t *ctrl) {
    dw_write(ctrl, DW_IC_ENABLE, 1);

    for (int i = 0; i < 10000; i++) {
        uint32_t en_st = dw_read(ctrl, DW_IC_ENABLE_STATUS);
        if (en_st & DW_IC_EN_STATUS_IC_EN)
            return 0;
        // Fallback: if IC_ENABLE reads back 1, assume enabled
        uint32_t en_reg = dw_read(ctrl, DW_IC_ENABLE);
        if (en_reg == 1) {
            i2c_delay_us(100);
            return 0;
        }
        i2c_delay_us(1);
    }
    i2c_delay_us(1000);
    return 0;
}

static int dw_i2c_init_controller(i2c_dw_controller_t *ctrl) {
    volatile uint8_t *priv = (volatile uint8_t *)ctrl->base + 0x200;
    uint32_t bar_lo = (uint32_t)(ctrl->bar_phys & 0xFFFFFFFF);
    uint32_t bar_hi = (uint32_t)(ctrl->bar_phys >> 32);
    int dw_alive = 0;

    // ---- intel_lpss_init_dev() equivalent ----
    // Step 1: Assert reset (put device in reset state)
    *(volatile uint32_t *)(priv + (LPSS_PRIV_RESETS - 0x200)) = 0;
    i2c_delay_us(2000);

    // Step 2: Deassert reset (bring device out of reset)
    *(volatile uint32_t *)(priv + (LPSS_PRIV_RESETS - 0x200)) =
        LPSS_RESETS_FUNC | LPSS_RESETS_IDMA;
    i2c_delay_us(2000);

    // Step 3: Set remap address (BAR physical address)
    *(volatile uint32_t *)(priv + (LPSS_PRIV_REMAP_ADDR_LO - 0x200)) = bar_lo;
    *(volatile uint32_t *)(priv + (LPSS_PRIV_REMAP_ADDR_HI - 0x200)) = bar_hi;
    i2c_delay_us(1000);

    // Disable controller
    dw_write(ctrl, DW_IC_ENABLE, 0);
    i2c_delay_us(1000);

    // Poll for comp_type to confirm DW core is alive (up to 1s)
    for (int attempt = 0; attempt < 100; attempt++) {
        uint32_t ct = dw_read(ctrl, DW_IC_COMP_TYPE);
        if (ct == DW_IC_COMP_TYPE_VALUE) {
            kprintf("[I2C%d] DW core alive (attempt %d)\n",
                    ctrl->bus_id, attempt);
            dw_alive = 1;
            break;
        }
        i2c_delay_us(10000);  // 10ms between attempts
    }

    if (!dw_alive) {
        kprintf("[I2C%d] FAIL ct=0x%08x\n", ctrl->bus_id,
                dw_read(ctrl, DW_IC_COMP_TYPE));
        return -1;
    }

    // Verify LPSS CAPABILITIES confirms I2C type
    uint32_t caps = *(volatile uint32_t *)(priv + (LPSS_PRIV_CAPABILITIES - 0x200));
    uint32_t caps_type = (caps & LPSS_CAPS_TYPE_MASK) >> LPSS_CAPS_TYPE_SHIFT;
    if (caps_type != LPSS_DEV_TYPE_I2C) {
        kprintf("[I2C%d] FAIL caps type=%u val=0x%08x\n",
                ctrl->bus_id, caps_type, caps);
        return -1;
    }

    // ---- Step 5: Disable controller for configuration ----
    dw_write(ctrl, DW_IC_ENABLE, 0);
    for (int w = 0; w < 500000; w++) {
        if ((dw_read(ctrl, DW_IC_ENABLE_STATUS) & DW_IC_EN_STATUS_IC_EN) == 0)
            break;
        i2c_delay_us(1);
    }

    // ---- Step 6: Read hardware parameters ----
    uint32_t comp_param = dw_read(ctrl, DW_IC_COMP_PARAM_1);
    uint32_t rx_depth = ((comp_param >> 8) & 0xFF) + 1;
    uint32_t tx_depth = ((comp_param >> 16) & 0xFF) + 1;
    if (rx_depth > 256 || tx_depth > 256 || rx_depth == 1) {
        rx_depth = 64;
        tx_depth = 64;
    }
    ctrl->rx_fifo_depth = rx_depth;
    ctrl->tx_fifo_depth = tx_depth;

    // ---- Step 7: Configure controller ----
    // Master mode, standard speed (100kHz), 7-bit addressing, restart enable
    uint32_t ic_con = DW_IC_CON_MASTER | DW_IC_CON_SPEED_SS |
                      DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE;
    dw_write(ctrl, DW_IC_CON, ic_con);

    // Standard mode SCL timing (100kHz, per PCH 600/700 datasheet defaults)
    dw_write(ctrl, DW_IC_SS_SCL_HCNT, 500);
    dw_write(ctrl, DW_IC_SS_SCL_LCNT, 588);

    // SDA hold time
    dw_write(ctrl, DW_IC_SDA_HOLD, 0x001C001C);

    // Spike suppression filter (SS/FS datasheet default = 7)
    dw_write(ctrl, DW_IC_FS_SPKLEN, 7);

    // FIFO thresholds
    dw_write(ctrl, DW_IC_RX_TL, 0);
    dw_write(ctrl, DW_IC_TX_TL, 0);

    // Disable all interrupts (we use polling)
    dw_write(ctrl, DW_IC_INTR_MASK, 0);

    // Clear any pending interrupts/aborts (SRAM garbage may set flags)
    (void)dw_read(ctrl, DW_IC_CLR_INTR);
    (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
    (void)dw_read(ctrl, DW_IC_CLR_RX_OVER);
    (void)dw_read(ctrl, DW_IC_CLR_RX_UNDER);
    (void)dw_read(ctrl, DW_IC_CLR_TX_OVER);

    // Dump status while disabled
    kprintf("[I2C%d] After config (disabled): IC_CON=0x%x IC_ENABLE=0x%x "
            "STATUS=0x%x EN_ST=0x%x RAW_INTR=0x%x\n",
            ctrl->bus_id,
            dw_read(ctrl, DW_IC_CON),
            dw_read(ctrl, DW_IC_ENABLE),
            dw_read(ctrl, DW_IC_STATUS),
            dw_read(ctrl, DW_IC_ENABLE_STATUS),
            dw_read(ctrl, DW_IC_RAW_INTR_STAT));

    // ---- Now ENABLE the controller and check if status comes alive ----
    dw_write(ctrl, DW_IC_ENABLE, 1);
    i2c_delay_us(5000);  // 5ms for clocks to stabilize

    // Clear interrupts again after enable
    (void)dw_read(ctrl, DW_IC_CLR_INTR);
    (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);

    uint32_t en_status = dw_read(ctrl, DW_IC_STATUS);
    uint32_t en_en_st = dw_read(ctrl, DW_IC_ENABLE_STATUS);
    uint32_t en_raw = dw_read(ctrl, DW_IC_RAW_INTR_STAT);
    uint32_t en_enable = dw_read(ctrl, DW_IC_ENABLE);
    kprintf("[I2C%d] After enable: IC_ENABLE=0x%x STATUS=0x%x EN_ST=0x%x "
            "RAW_INTR=0x%x\n",
            ctrl->bus_id, en_enable, en_status, en_en_st, en_raw);

    // Disable again before returning (set_target will re-enable)
    dw_write(ctrl, DW_IC_ENABLE, 0);
    i2c_delay_us(1000);

    kprintf("[I2C%d] Initialized (FIFO: RX=%u TX=%u)\n",
            ctrl->bus_id, ctrl->rx_fifo_depth, ctrl->tx_fifo_depth);

    ctrl->active = 1;
    ctrl->current_target = 0xFFFF;  // Force first set_target to program TAR
    return 0;
}

// ============================================================================
// DesignWare I2C Controller - Transfer primitives
// ============================================================================

// Forward declaration for interrupt-driven transfer
static int dw_i2c_xfer_irq(i2c_dw_controller_t *ctrl, uint16_t addr,
                            const uint8_t *wbuf, int wlen,
                            uint8_t *rbuf, int rlen);

// Forward declaration for worker thread
static void i2c_hid_worker_thread(void *arg);

// Forward declaration for length-first I2C HID read
static int i2c_hid_read_length_first(i2c_hid_device_t *dev,
                                     uint8_t *buf, uint16_t buf_size);

// Wait for TX FIFO to have space (not full)
static int dw_i2c_wait_tx_not_full(i2c_dw_controller_t *ctrl, int timeout_us) {
    for (int i = 0; i < timeout_us; i++) {
        if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_TFNF)
            return 0;
        // Check for abort
        if (dw_read(ctrl, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT)
            return -2;
        i2c_delay_us(1);
    }
    return -1;
}

// Wait for RX FIFO to have data
static int dw_i2c_wait_rx_not_empty(i2c_dw_controller_t *ctrl, int timeout_us) {
    for (int i = 0; i < timeout_us; i++) {
        if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
            return 0;
        if (dw_read(ctrl, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT)
            return -2;
        i2c_delay_us(1);
    }
    return -1;
}

// Wait for all bus activity to complete
static int dw_i2c_wait_idle(i2c_dw_controller_t *ctrl, int timeout_us) {
    for (int i = 0; i < timeout_us; i++) {
        uint32_t status = dw_read(ctrl, DW_IC_STATUS);
        if (!(status & DW_IC_STATUS_MST_ACTIVITY) &&
            (status & DW_IC_STATUS_TFE))
            return 0;
        i2c_delay_us(1);
    }
    return -1;
}

// Set target address and enable controller.
// Like Linux i2c-designware-master.c: skip the disable/enable cycle
// when the target address hasn't changed.  The disable/enable sends
// an ABORT on the bus and takes ~300µs; at 100 reads/sec (LEVEL mode)
// that's 30ms/sec wasted + bus disruption that causes silent data loss.
static int dw_i2c_set_target(i2c_dw_controller_t *ctrl, uint16_t addr) {
    if (ctrl->current_target == addr) {
        // Same address — just clear stale status, no bus disruption
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        return 0;
    }

    dw_i2c_disable(ctrl);

    // Clear any stale abort status
    (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
    (void)dw_read(ctrl, DW_IC_CLR_INTR);

    // Set target address (7-bit)
    dw_write(ctrl, DW_IC_TAR, addr & 0x7F);

    dw_i2c_enable(ctrl);
    ctrl->current_target = addr;
    return 0;
}

// Convenience: write-only transfer
static int dw_i2c_write(i2c_dw_controller_t *ctrl, uint16_t addr,
                         const uint8_t *buf, int len) {
    return dw_i2c_xfer_irq(ctrl, addr, buf, len, NULL, 0);
}

// Convenience: read with 2-byte register address prefix
static int dw_i2c_read_reg16(i2c_dw_controller_t *ctrl, uint16_t addr,
                              uint16_t reg, uint8_t *buf, int len) {
    uint8_t regbuf[2] = { (uint8_t)(reg & 0xFF), (uint8_t)((reg >> 8) & 0xFF) };
    return dw_i2c_xfer_irq(ctrl, addr, regbuf, 2, buf, len);
}

// ============================================================================
// I2C Bus Scan
// ============================================================================

// Probe a single I2C address — returns 0 if device ACKs, -1 if NACK
static int dw_i2c_probe_addr(i2c_dw_controller_t *ctrl, uint16_t addr) {
    dw_i2c_set_target(ctrl, addr);

    // Issue a single read byte with STOP
    uint32_t cmd = DW_IC_DATA_CMD_READ | DW_IC_DATA_CMD_STOP;

    int rc = dw_i2c_wait_tx_not_full(ctrl, 5000);
    if (rc < 0) return -1;

    dw_write(ctrl, DW_IC_DATA_CMD, cmd);

    // Wait for response or abort
    for (int i = 0; i < 10000; i++) {
        // Check for abort (NACK)
        if (dw_read(ctrl, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT) {
            (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
            (void)dw_read(ctrl, DW_IC_CLR_INTR);
            return -1;
        }
        // Check for received data (ACK)
        if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE) {
            (void)dw_read(ctrl, DW_IC_DATA_CMD);  // consume byte
            return 0;  // Device responded
        }
        i2c_delay_us(1);
    }

    // Timeout — treat as no device
    (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
    (void)dw_read(ctrl, DW_IC_CLR_INTR);
    return -1;
}

static void dw_i2c_scan_bus(i2c_dw_controller_t *ctrl) {
    kprintf("[I2C%d] Scanning bus for devices...\n", ctrl->bus_id);
    int found = 0;

    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (dw_i2c_probe_addr(ctrl, addr) == 0) {
            kprintf("[I2C%d]   Device found at address 0x%02x\n", ctrl->bus_id, addr);
            found++;
        }
    }

    kprintf("[I2C%d] Bus scan complete: %d device(s) found\n", ctrl->bus_id, found);
}

// ============================================================================
// PCI Detection — find Intel LPSS I2C controllers
// ============================================================================

// Check if a PCI device ID is a known Intel LPSS I2C controller
static int is_intel_lpss_i2c(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != INTEL_LPSS_I2C_VENDOR)
        return 0;

    switch (device_id) {
        // Arrow Lake
        case INTEL_LPSS_I2C_ARL_H_0: case INTEL_LPSS_I2C_ARL_H_1:
        case INTEL_LPSS_I2C_ARL_H_2: case INTEL_LPSS_I2C_ARL_H_3:
        // Meteor Lake
        case INTEL_LPSS_I2C_MTL_0: case INTEL_LPSS_I2C_MTL_1:
        case INTEL_LPSS_I2C_MTL_2: case INTEL_LPSS_I2C_MTL_3:
        // Raptor Lake
        case INTEL_LPSS_I2C_RPL_S_0: case INTEL_LPSS_I2C_RPL_S_1:
        case INTEL_LPSS_I2C_RPL_P_0: case INTEL_LPSS_I2C_RPL_P_1:
        // Alder Lake
        case INTEL_LPSS_I2C_ADL_S_0: case INTEL_LPSS_I2C_ADL_S_1:
        case INTEL_LPSS_I2C_ADL_P_0: case INTEL_LPSS_I2C_ADL_P_1:
        case INTEL_LPSS_I2C_ADL_N_0: case INTEL_LPSS_I2C_ADL_N_1:
        // Tiger Lake
        case INTEL_LPSS_I2C_TGL_0: case INTEL_LPSS_I2C_TGL_1:
        case INTEL_LPSS_I2C_TGL_2: case INTEL_LPSS_I2C_TGL_3:
            return 1;
        default:
            return 0;
    }
}

static int has_intel_lpss_i2c_controller(void)
{
    const pci_device_t *devices;
    int pci_count = 0;

    devices = pci_get_devices(&pci_count);
    if (!devices || pci_count == 0)
        return 0;

    for (int i = 0; i < pci_count; i++) {
        if (is_intel_lpss_i2c(devices[i].vendor_id, devices[i].device_id))
            return 1;
    }

    return 0;
}

// MSI vector base for I2C controllers (50-53 for up to 4 controllers)
#define I2C_MSI_VECTOR_BASE  50

// Map PMC PWRMBASE and return virtual address (0 on failure).
// Strategy 1: ACPI HID search
// Strategy 2: ACPI known paths
// Strategy 3: P2SB unhide trick (D31:F1 → D31:F2)
// Strategy 4: Direct PCI D31:F2
// Strategy 5: Well-known default addresses probed via GEN_PMCON_A
static volatile uint8_t *pmc_map_pwrmbase(void)
{
    uint64_t phys = 0;

    // ---- Strategy 1: Find PMC via ACPI HID ----
    static const char *pmc_hids[] = {
        "INTC10B5", "INTC1026", "INT34BB", "INTC1025", NULL
    };
    for (int h = 0; !phys && pmc_hids[h]; h++) {
        acpi_aml_device_info_t info;
        if (acpi_aml_find_devices_by_hid(pmc_hids[h], &info, 1) > 0) {
            acpi_crs_result_t crs;
            if (acpi_aml_eval_crs(info.path, &crs) == 0 && crs.mmio_base)
                phys = crs.mmio_base;
        }
    }

    // ---- Strategy 2: Try known ACPI paths ----
    if (!phys) {
        static const char *pmc_paths[] = {
            "\\_SB.PC00.PMC", "\\_SB.PCI0.PMC",
            "\\_SB.PC00.PPMC", "\\_SB.PCI0.PPMC", NULL
        };
        for (int p = 0; !phys && pmc_paths[p]; p++) {
            acpi_crs_result_t crs;
            if (acpi_aml_eval_crs(pmc_paths[p], &crs) == 0 && crs.mmio_base)
                phys = crs.mmio_base;
        }
    }

    // ---- Strategy 3: Unhide D31:F2 via P2SB trick ----
    // P2SB (D31:F1) E0h bit 8 = HIDE. Linux writes 0 to entire dword to
    // unhide.  We MUST NOT use read-modify-write because reads return
    // 0xFFFFFFFF on a hidden device, which poisons the reserved bits.
    // Try both ECAM and CF8 paths — Arrow Lake may block one but not the other.
    if (!phys) {
        // Try ECAM path first (MMIO config write may bypass CF8/CFC filter)
        if (g_ecam_bus0_va) {
            ecam_write32(0, 31, 1, 0xE0, 0);  // unhide via ECAM
            i2c_delay_us(1000);
            uint32_t vid_e = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC, 0x00);
            uint32_t vid_c = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                            PMC_PCI_FUNC, 0x00);
            kprintf("[I2C-PMC] P2SB ECAM unhide: PMC vid ecam=0x%x cf8=0x%x\n",
                    vid_e, vid_c);
            uint32_t vid = (vid_e != 0xFFFFFFFF) ? vid_e : vid_c;
            if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
                // Use whichever path found the PMC
                int use_ecam = (vid_e != 0xFFFFFFFF);
                uint32_t bar_lo, bar_hi;
                if (use_ecam) {
                    uint32_t cmd = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC,
                                              PMC_PCI_CMD);
                    if (!(cmd & 0x02))
                        ecam_write32(0, PMC_PCI_DEV, PMC_PCI_FUNC,
                                     PMC_PCI_CMD, cmd | 0x02);
                    bar_lo = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC,
                                         PMC_PCI_PWRMBASE);
                    bar_hi = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC,
                                         PMC_PCI_PWRMBASE_HI);
                } else {
                    uint32_t cmd = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                                  PMC_PCI_FUNC, PMC_PCI_CMD);
                    if (!(cmd & 0x02))
                        pci_cfg_write32(PMC_PCI_BUS, PMC_PCI_DEV,
                                        PMC_PCI_FUNC, PMC_PCI_CMD, cmd | 0x02);
                    bar_lo = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                            PMC_PCI_FUNC, PMC_PCI_PWRMBASE);
                    bar_hi = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                            PMC_PCI_FUNC, PMC_PCI_PWRMBASE_HI);
                }
                phys = ((uint64_t)bar_hi << 32) | (bar_lo & ~0x1FFFULL);
            }
            ecam_write32(0, 31, 1, 0xE0, (1 << 8));  // re-hide via ECAM
        }

        // CF8/CFC fallback
        if (!phys) {
            pci_cfg_write32(0, 31, 1, 0xE0, 0);
            i2c_delay_us(1000);
            uint32_t vid = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                          PMC_PCI_FUNC, 0x00);
            if (!g_ecam_bus0_va)  // only log if ECAM path didn't already
                kprintf("[I2C-PMC] P2SB CF8 unhide: PMC vid=0x%x\n", vid);
            if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
                uint32_t cmd = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                              PMC_PCI_FUNC, PMC_PCI_CMD);
                if (!(cmd & 0x02))
                    pci_cfg_write32(PMC_PCI_BUS, PMC_PCI_DEV,
                                    PMC_PCI_FUNC, PMC_PCI_CMD, cmd | 0x02);
                uint32_t bar_lo = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                                 PMC_PCI_FUNC, PMC_PCI_PWRMBASE);
                uint32_t bar_hi = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                                 PMC_PCI_FUNC, PMC_PCI_PWRMBASE_HI);
                phys = ((uint64_t)bar_hi << 32) | (bar_lo & ~0x1FFFULL);
            }
            pci_cfg_write32(0, 31, 1, 0xE0, (1 << 8));
        }
    }

    // ---- Strategy 4: Direct PCI D31:F2 (visible on some platforms) ----
    if (!phys) {
        uint32_t vid = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                      PMC_PCI_FUNC, 0x00);
        if (vid != 0xFFFFFFFF && (vid & 0xFFFF) != 0xFFFF) {
            uint32_t cmd = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                          PMC_PCI_FUNC, PMC_PCI_CMD);
            if (!(cmd & 0x02))
                pci_cfg_write32(PMC_PCI_BUS, PMC_PCI_DEV,
                                PMC_PCI_FUNC, PMC_PCI_CMD, cmd | 0x02);
            uint32_t bar_lo = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                             PMC_PCI_FUNC, PMC_PCI_PWRMBASE);
            uint32_t bar_hi = pci_cfg_read32(PMC_PCI_BUS, PMC_PCI_DEV,
                                             PMC_PCI_FUNC, PMC_PCI_PWRMBASE_HI);
            phys = ((uint64_t)bar_hi << 32) | (bar_lo & ~0x1FFFULL);
        }
    }

    // ---- Strategy 5b: Try PMC D31:F2 via ECAM ----
    // BIOS hides PMC from CF8/CFC, but ECAM (memory-mapped config) may
    // still expose it because P2SB HIDE only blocks I/O-port legacy path.
    if (!phys && g_ecam_bus0_va) {
        uint32_t vid = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC, 0x00);
        if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
            uint32_t cmd = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC, PMC_PCI_CMD);
            if (!(cmd & 0x02))
                ecam_write32(0, PMC_PCI_DEV, PMC_PCI_FUNC, PMC_PCI_CMD, cmd | 0x02);
            uint32_t bar_lo = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC, PMC_PCI_PWRMBASE);
            uint32_t bar_hi = ecam_read32(0, PMC_PCI_DEV, PMC_PCI_FUNC, PMC_PCI_PWRMBASE_HI);
            phys = ((uint64_t)bar_hi << 32) | (bar_lo & ~0x1FFFULL);
        }
    }

    // ---- Strategy 6: Probe well-known default PWRMBASE addresses ----
    // On Intel PCH the BIOS programs PWRMBASE.  Validate by mapping and
    // reading GEN_PMCON_A (offset 0x1020) which has non-zero defaults on
    // every Intel PCH (SLP_S4#, SLP_S3# assertion stretches etc).
    if (!phys) {
        static const uint64_t known_bases[] = {
            0xFE000000ULL,  // ADL/RPL/MTL/ARL most common
            0
        };
        for (int k = 0; !phys && known_bases[k]; k++) {
            // Need 2+ pages to reach offset 0x1020
            uint64_t probe_va = mm_map_device_mmio(known_bases[k], 2);
            if (!probe_va)
                continue;
            volatile uint32_t *gen_pmcon_a =
                (volatile uint32_t *)(probe_va + 0x1020);
            uint32_t val = *gen_pmcon_a;
            // Valid: non-zero, non-ones (unmapped returns 0xFF..FF)
            if (val != 0xFFFFFFFF && val != 0x00000000) {
                phys = known_bases[k];
            }
        }
    }

    if (!phys) {
        kprintf("[I2C-PMC] not found (all strategies failed)\n");
        return 0;
    }

    // Map 8 pages (32KB) to cover PMC MMIO space including FDIS registers
    // at offsets 0x1E24, 0x1E44 etc.
    uint64_t va = mm_map_device_mmio(phys, 8);
    if (!va) {
        kprintf("[I2C-PMC] map failed for 0x%llx\n", (unsigned long long)phys);
        return 0;
    }


    kprintf("[I2C-PMC] PWRMBASE=0x%llx\n", (unsigned long long)phys);
    return (volatile uint8_t *)va;
}

// Check PMC for I2C function/fuse disable.  Returns 0 if I2C is usable.
static int pmc_check_i2c_enabled(volatile uint8_t *pmc, int i2c_index)
{
    if (!pmc) return 0;  // no PMC mapped, assume OK

    uint32_t fuse = *(volatile uint32_t *)(pmc + PMC_FUSE_SS_DIS_RD_2);
    if (fuse & (1 << 6)) {
        kprintf("[I2C-PMC] Serial IO fuse-disabled (FUSE=0x%08x)\n", fuse);
        return -1;
    }

    uint32_t fdis2 = *(volatile uint32_t *)(pmc + PMC_ST_PG_FDIS2);
    if (fdis2 & (1 << i2c_index)) {
        kprintf("[I2C-PMC] I2C%d function-disabled (FDIS2=0x%08x)\n",
                i2c_index, fdis2);
        return -1;
    }

    // PPASR0 bit 14 = Serial IO Power Gate Ack Status:
    //   0 = controller may be power gated
    //   1 = controller may NOT be power gated (active)
    uint32_t ppasr0 = *(volatile uint32_t *)(pmc + PMC_PPASR0);
    int sio_pg_ack = (ppasr0 >> 14) & 1;

    kprintf("[I2C-PMC] I2C%d fdis=0x%x ppasr0=0x%x(%s)\n",
            i2c_index, fdis2, ppasr0,
            sio_pg_ack ? "ACTIVE" : "GATED");
    return 0;
}

// Determine the I2C controller logical index (0-5) from the PCI function number
// and device number.  Returns -1 if unknown.
static int lpss_i2c_index(uint8_t dev_nr, uint8_t func_nr)
{
    // Arrow Lake / Alder Lake: D21:F0-F3 → I2C 0-3; different chips may vary.
    // Fall back to function number for controllers on a single device.
    if (dev_nr == 0x15)       // D21 = 0x15
        return (int)func_nr;  // F0-F3 → I2C 0-3
    if (dev_nr == 0x19)       // D25 = 0x19
        return 4 + (int)func_nr;
    return (int)func_nr;
}

// Helper: extract parent path from a dotted ACPI path.
// E.g. "_SB.PC00.I2C3.TPD0" → "_SB.PC00.I2C3"
// Returns 0 on success, -1 if no parent.
static int acpi_parent_path(const char *child, char *parent, int parent_sz)
{
    int len = 0;
    while (child[len]) len++;
    int last_dot = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (child[i] == '.') { last_dot = i; break; }
    }
    if (last_dot <= 0 || last_dot >= parent_sz) return -1;
    for (int i = 0; i < last_dot; i++) parent[i] = child[i];
    parent[last_dot] = 0;
    return 0;
}

// Helper: check if a controller path is already in the list
static int path_already_listed(char paths[][ACPI_AML_MAX_PATH], int n,
                               const char *path)
{
    for (int i = 0; i < n; i++) {
        if (kstrcmp(paths[i], path) == 0) return 1;
    }
    return 0;
}

// ============================================================================
// IOAPIC interrupt probe for I2C controllers
//
// Intel PCH 600/700 Serial IO (D21) uses "direct" IOAPIC entries (GSI 24+),
// NOT PIRQ routing.  The firmware (FSP) programs the PCH ITSS to wire each
// controller's interrupt output to a specific IOAPIC pin.  Neither _PRT,
// _CRS, INTLINE, nor MSI exposes which pin was assigned.
//
// We discover the pin empirically: enable the DW core's TX_EMPTY interrupt
// (which fires immediately because the FIFO is empty), then sweep IOAPIC
// entries 24-119.  When we unmask the correct entry the interrupt reaches
// the CPU and our ISR sets a flag.
//
// Diagnostic strategy:
// - Read ITSS IPC0-3 via P2SB PCR to learn actual polarity config
// - Dump all non-default IOAPIC RTEs before masking (firmware breadcrumbs)
// - Write PCI INTLINE to potentially activate ITSS direct routing
// - Sweep with 4 combos: {active-HIGH, active-LOW} × {level, edge}
// - Check LAPIC ISR/IRR after each RTE to detect "stuck" delivery
// ============================================================================

// Local rdmsr/wrmsr for pre-init diagnostics (lapic_init not yet called)
static inline uint64_t i2c_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void i2c_wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}
#define IA32_APIC_BASE_MSR       0x1B
#define X2APIC_MSR_BASE_LOCAL    0x800
#define X2APIC_SVR_MSR           (X2APIC_MSR_BASE_LOCAL + (0x0F0 >> 4))  // 0x80F
#define X2APIC_TPR_MSR           (X2APIC_MSR_BASE_LOCAL + (0x080 >> 4))  // 0x808
#define X2APIC_ID_MSR            (X2APIC_MSR_BASE_LOCAL + (0x020 >> 4))  // 0x802
#define X2APIC_EOI_MSR           (X2APIC_MSR_BASE_LOCAL + (0x0B0 >> 4))  // 0x80B

// ITSS sideband port and register offsets (PCH 600/700 / Arrow Lake)
#define ITSS_PORT_ID       0xC4
#define ITSS_IPC0          0x3200   // Interrupt Polarity Control 0 (IRQ 0-31)
#define ITSS_IPC1          0x3204   // Interrupt Polarity Control 1 (IRQ 32-63)
#define ITSS_IPC2          0x3208   // Interrupt Polarity Control 2 (IRQ 64-95)
#define ITSS_IPC3          0x320C   // Interrupt Polarity Control 3 (IRQ 96-119)
#define ITSS_PIR0          0x3140   // PCI Interrupt Route 0 (D31)
#define ITSS_MMC           0x3334   // Master Message Control

static volatile int g_i2c_probe_mode = 0;       // 1 = probe in progress
static volatile uint32_t g_i2c_probe_hit = 0;   // set by ISR during probe
static int g_i2c_claimed_gsi[I2C_DW_MAX_CONTROLLERS]; // GSIs already assigned
static int g_i2c_claimed_gsi_count = 0;

// Read a 32-bit ITSS PCR register via P2SB sideband.
// Returns 0xFFFFFFFF on failure (P2SB not accessible).
// The ITSS region is mapped UC via mm_map_device_mmio on first access.
static uint64_t g_itss_va = 0;  // UC-mapped VA of ITSS port region

static uint32_t itss_pcr_read32(uint32_t offset)
{
    // Map the ITSS sideband region on first use
    if (!g_itss_va) {
        // Get SBREG_BAR if not cached
        if (!g_sbreg_bar) {
            if (g_ecam_bus0_va) {
                // Try unhide first
                ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                             P2SB_P2SBC, 0);
                i2c_delay_us(500);
                uint32_t vid = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                           P2SB_PCI_FUNC, 0x00);
                if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
                    uint32_t cmd = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                               P2SB_PCI_FUNC, 0x04);
                    if (!(cmd & 0x02))
                        ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                                     0x04, cmd | 0x02);
                }
                // Read BAR even if HIDE is sticky (BAR often readable regardless)
                uint32_t bl = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                          P2SB_PCI_FUNC, P2SB_SBREG_BAR);
                uint32_t bh = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                          P2SB_PCI_FUNC, P2SB_SBREG_BARH);
                uint64_t bar = ((uint64_t)bh << 32) | (bl & ~0xFULL);
                kprintf("[ITSS] P2SB VID=0x%x BAR raw lo=0x%x hi=0x%x => 0x%llx\n",
                        vid, bl, bh, (unsigned long long)bar);
                // Accept if BAR looks like a valid physical address (non-zero,
                // not all-F, and above 1GB typical for PCH SBREG)
                if (bar && bar != 0xFFFFFFFFFFFFFFF0ULL && bar >= 0x40000000ULL)
                    g_sbreg_bar = bar;
                // Re-hide
                ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                             P2SB_P2SBC, P2SB_HIDE_BIT);
            }
        }
        if (!g_sbreg_bar) {
            kprintf("[ITSS] No SBREG_BAR, cannot read ITSS IPC\n");
            return 0xFFFFFFFF;
        }
        // Map the ITSS port region (64KB) as UC via mm_map_device_mmio.
        // SBREG_BAR + (PortID << 16) gives the 64KB window for this port.
        uint64_t itss_pa = g_sbreg_bar + ((uint64_t)ITSS_PORT_ID << 16);
        g_itss_va = mm_map_device_mmio(itss_pa, 16);  // 16 pages = 64KB
        if (!g_itss_va) {
            kprintf("[ITSS] mm_map_device_mmio failed for 0x%llx\n",
                    (unsigned long long)itss_pa);
            return 0xFFFFFFFF;
        }
        kprintf("[ITSS] P2SB SBREG_BAR=0x%llx ITSS mapped at VA=0x%llx\n",
                (unsigned long long)g_sbreg_bar,
                (unsigned long long)g_itss_va);
    }

    if (offset > 0xFFFF) return 0xFFFFFFFF;
    return *(volatile uint32_t *)((volatile uint8_t *)g_itss_va + offset);
}

// Dump ITSS IPC registers and firmware IOAPIC state for diagnostics.
static void probe_dump_diagnostics(uint32_t max_gsi)
{
    // ---- ITSS IPC polarity registers ----
    uint32_t ipc0 = itss_pcr_read32(ITSS_IPC0);
    uint32_t ipc1 = itss_pcr_read32(ITSS_IPC1);
    uint32_t ipc2 = itss_pcr_read32(ITSS_IPC2);
    uint32_t ipc3 = itss_pcr_read32(ITSS_IPC3);
    kprintf("[ITSS] IPC0=0x%08x IPC1=0x%08x IPC2=0x%08x IPC3=0x%08x\n",
            ipc0, ipc1, ipc2, ipc3);
    // Bit=1 means "active-HIGH polarity disabled" → signal is active-LOW
    // Bit=0 means signal is active-HIGH (default for IRQ 24+)
    kprintf("[ITSS] IPC decode: IRQ24-31 pol=%s, IRQ32-63 pol=%s\n",
            (ipc0 >> 24) ? "mixed" : "all-HIGH",
            ipc1 ? "mixed/LOW" : "all-HIGH");

    // ---- ITSS MMC and PIR0 ----
    uint32_t mmc = itss_pcr_read32(ITSS_MMC);
    uint32_t pir0 = itss_pcr_read32(ITSS_PIR0);
    kprintf("[ITSS] MMC=0x%04x PIR0=0x%04x\n", mmc & 0xFFFF, pir0 & 0xFFFF);

    // ---- IOAPIC RTE dump: show all entries 16-63 that are not fully masked+zero ----
    kprintf("[IOAPIC] Firmware RTE dump (entries 16-%u):\n",
            max_gsi > 63 ? 63 : max_gsi);
    for (uint32_t g = 16; g <= max_gsi && g <= 63; g++) {
        uint32_t lo, hi;
        ioapic_read_rte((uint8_t)g, &lo, &hi);
        // Skip fully masked default entries (masked + vector 0)
        if ((lo & 0x1FFFF) == 0x10000 && hi == 0) continue;  // masked, vec=0
        if (lo == 0 && hi == 0) continue;
        kprintf("  GSI %u: lo=0x%08x hi=0x%08x [vec=%u del=%u pol=%s trig=%s %s dest=%u]\n",
                g, lo, hi,
                lo & 0xFF,            // vector
                (lo >> 8) & 7,        // delivery mode
                (lo & (1u<<13)) ? "low" : "high",
                (lo & (1u<<15)) ? "level" : "edge",
                (lo & (1u<<16)) ? "MASKED" : "unmask",
                (hi >> 24) & 0xFF);   // destination
    }
}

static int probe_ioapic_i2c_gsi(i2c_dw_controller_t *ctrl, uint8_t vector)
{
    uint32_t max_gsi = ioapic_max_gsi();
    if (max_gsi < 24) return -1;

    // Dump firmware state before we touch anything
    probe_dump_diagnostics(max_gsi);

    // Ensure interrupts are enabled (needed for ISR to fire)
    uint64_t rflags;
    __asm__ volatile("pushf; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) {
        __asm__ volatile("sti");
        kprintf("[I2C%d] probe: enabled IF\n", ctrl->bus_id);
    }

    // Ensure LAPIC is ready to deliver IOAPIC fixed interrupts.
    // lapic_init() hasn't run yet, so we must configure via raw MSR.
    uint64_t apic_base_val = i2c_rdmsr(IA32_APIC_BASE_MSR);
    int probe_x2apic = (apic_base_val >> 10) & 1;
    uint32_t bsp_apic_id = 0;  // physical APIC ID of BSP for RTE destination
    if (probe_x2apic) {
        bsp_apic_id = (uint32_t)i2c_rdmsr(X2APIC_ID_MSR);
        uint32_t svr_val   = (uint32_t)i2c_rdmsr(X2APIC_SVR_MSR);
        uint32_t tpr_val   = (uint32_t)i2c_rdmsr(X2APIC_TPR_MSR);
        kprintf("[I2C%d] LAPIC pre-init: ID=%u SVR=0x%x TPR=0x%x\n",
                ctrl->bus_id, bsp_apic_id, svr_val, tpr_val);

        // SVR bit 8 must be set for the LAPIC to deliver fixed interrupts.
        // ExtINT/PIC via LINT0 works even when disabled, but IOAPIC doesn't.
        if (!(svr_val & (1u << 8))) {
            i2c_wrmsr(X2APIC_SVR_MSR, (uint64_t)(svr_val | (1u << 8) | 0xFF));
            kprintf("[I2C%d] LAPIC: software-enabled (SVR was 0x%x)\n",
                    ctrl->bus_id, svr_val);
        }
        // TPR must be 0 to allow delivery of vector 50 (priority 3)
        if (tpr_val > 0) {
            i2c_wrmsr(X2APIC_TPR_MSR, 0);
            kprintf("[I2C%d] LAPIC: TPR cleared (was 0x%x)\n",
                    ctrl->bus_id, tpr_val);
        }
    }

    // Mask every unclaimed RTE 24-max so no stale entry fires during sweep
    for (uint32_t g = 24; g <= max_gsi; g++) {
        int claimed = 0;
        for (int k = 0; k < g_i2c_claimed_gsi_count; k++)
            if (g_i2c_claimed_gsi[k] == (int)g) { claimed = 1; break; }
        if (!claimed)
            ioapic_mask_gsi((uint8_t)g);
    }

    // Drain any stuck ISR entries so same-priority vectors can be delivered.
    // (e.g., xHCI vec 49 stuck because lapic_eoi() used MMIO pre-detect)
    if (probe_x2apic) {
        for (int eoi_drain = 0; eoi_drain < 16; eoi_drain++) {
            int any = 0;
            for (int r = 0; r < 8; r++) {
                uint32_t isr = (uint32_t)i2c_rdmsr(
                    X2APIC_MSR_BASE_LOCAL + ((LAPIC_ISR_BASE + r * 0x10) >> 4));
                if (isr) { any = 1; break; }
            }
            if (!any) break;
            i2c_wrmsr(X2APIC_EOI_MSR, 0);
        }
    }

    // Enable probe mode in the ISR
    g_i2c_probe_mode = 1;
    g_i2c_probe_hit = 0;

    // The init function disables the controller after verifying it works
    // (IC_ENABLE=0).  We must re-enable it so TX_EMPTY asserts on the
    // physical interrupt output pin.
    {
        uint32_t en = dw_read(ctrl, DW_IC_ENABLE);
        if (!(en & 1)) {
            dw_write(ctrl, DW_IC_ENABLE, 1);
            i2c_delay_us(2000);  // let clocks stabilize
            kprintf("[I2C%d] probe: re-enabled DW core (was IC_ENABLE=0x%x)\n",
                    ctrl->bus_id, en);
        }
    }

    // Make the DW core assert its interrupt output by enabling TX_EMPTY.
    // TX FIFO is empty after init, so the interrupt is asserted immediately.
    (void)dw_read(ctrl, DW_IC_CLR_INTR);
    dw_write(ctrl, DW_IC_INTR_MASK, DW_IC_INTR_TX_EMPTY);

    // Diagnostic: verify the DW core thinks it is asserting an interrupt
    uint32_t diag_raw = dw_read(ctrl, DW_IC_RAW_INTR_STAT);
    uint32_t diag_stat = dw_read(ctrl, DW_IC_INTR_STAT);
    kprintf("[I2C%d] probe: RAW_INTR=0x%x INTR_STAT=0x%x (expect 0x10)\n",
            ctrl->bus_id, diag_raw, diag_stat);

    // If TX_EMPTY still not asserted, try harder: disable/re-enable core,
    // reset TX threshold to 0, clear all status.
    if (!(diag_raw & DW_IC_INTR_TX_EMPTY)) {
        kprintf("[I2C%d] probe: TX_EMPTY not asserted, resetting core...\n",
                ctrl->bus_id);
        dw_write(ctrl, DW_IC_ENABLE, 0);
        i2c_delay_us(2000);
        dw_write(ctrl, DW_IC_TX_TL, 0);
        dw_write(ctrl, DW_IC_INTR_MASK, 0);
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
        dw_write(ctrl, DW_IC_ENABLE, 1);
        i2c_delay_us(5000);
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        dw_write(ctrl, DW_IC_INTR_MASK, DW_IC_INTR_TX_EMPTY);
        diag_raw = dw_read(ctrl, DW_IC_RAW_INTR_STAT);
        diag_stat = dw_read(ctrl, DW_IC_INTR_STAT);
        kprintf("[I2C%d] probe: after reset: RAW_INTR=0x%x INTR_STAT=0x%x "
                "IC_ENABLE=0x%x STATUS=0x%x\n",
                ctrl->bus_id, diag_raw, diag_stat,
                dw_read(ctrl, DW_IC_ENABLE),
                dw_read(ctrl, DW_IC_STATUS));
    }

    int found_gsi = -1;

    // Pre-compute LAPIC IRR access for fallback detection
    uint32_t vec_reg_sw = (uint32_t)vector / 32;
    uint32_t vec_bit_sw = 1u << ((uint32_t)vector % 32);
    uint32_t irr_msr_sw = X2APIC_MSR_BASE_LOCAL +
                           ((LAPIC_IRR_BASE + vec_reg_sw * 0x10) >> 4);
    uint32_t isr_msr_sw = X2APIC_MSR_BASE_LOCAL +
                           ((LAPIC_ISR_BASE + vec_reg_sw * 0x10) >> 4);

    // Read actual IPC polarity for the gsi range to decide sweep order.
    // IPC bit=1 → AHPOLDIS → signal is active-LOW to IOAPIC.
    // IPC bit=0 → signal is active-HIGH to IOAPIC (default for IRQ 24+).
    uint32_t ipc0 = itss_pcr_read32(ITSS_IPC0);
    uint32_t ipc1 = itss_pcr_read32(ITSS_IPC1);

    // Sweep with 4 combinations: {pol-high, pol-low} × {level, edge}.
    // PCH 600/700 defaults: IRQ 24+ active-HIGH, level-triggered.
    // But firmware may have changed IPC or the device may use edge mode.
    static const struct {
        uint32_t pol_bit;   // bit 13 of RTE low word
        uint32_t trig_bit;  // bit 15 of RTE low word
        const char *name;
    } combos[] = {
        { 0,        (1u<<15), "high/level" },
        { (1u<<13), (1u<<15), "low/level"  },
        { 0,        0,        "high/edge"  },
        { (1u<<13), 0,        "low/edge"   },
    };

    for (int ci = 0; ci < 4 && found_gsi < 0; ci++) {
        for (uint32_t gsi = 24; gsi <= max_gsi && found_gsi < 0; gsi++) {
            // Skip GSIs already claimed by a previous controller
            int claimed = 0;
            for (int k = 0; k < g_i2c_claimed_gsi_count; k++)
                if (g_i2c_claimed_gsi[k] == (int)gsi) { claimed = 1; break; }
            if (claimed) continue;

            g_i2c_probe_hit = 0;

            // Re-enable DW interrupt output (ISR disables IC_INTR_MASK
            // to deassert the level-triggered line before EOI).
            dw_write(ctrl, DW_IC_INTR_MASK, DW_IC_INTR_TX_EMPTY);

            // Program RTE: fixed delivery to BSP (physical dest mode)
            uint32_t low = (uint32_t)vector   // IDT vector
                         | (0u << 8)           // fixed delivery
                         | (0u << 11)          // physical dest
                         | combos[ci].pol_bit  // polarity
                         | combos[ci].trig_bit;// trigger mode
            // bit 16 = 0 → unmasked
            uint32_t high = (bsp_apic_id & 0xFF) << 24;  // dest = BSP LAPIC ID

            ioapic_write_rte((uint8_t)gsi, low, high);

            // Give time for the interrupt to propagate
            // (DW core → ITSS → IOAPIC → LAPIC → ISR)
            for (volatile int d = 0; d < 8000; d++)
                __asm__ volatile("pause");

            if (g_i2c_probe_hit) {
                found_gsi = (int)gsi;
                kprintf("[I2C%d] IOAPIC probe HIT: GSI %u vector %u (%s)\n",
                        ctrl->bus_id, gsi, vector, combos[ci].name);
            } else if (probe_x2apic) {
                // Fallback: check LAPIC IRR/ISR directly via x2APIC MSR.
                // If the interrupt reached the LAPIC but the ISR didn't fire
                // (e.g. LAPIC state issue), we can still detect the correct GSI.
                uint32_t irr = (uint32_t)i2c_rdmsr(irr_msr_sw);
                uint32_t isr = (uint32_t)i2c_rdmsr(isr_msr_sw);
                if ((irr | isr) & vec_bit_sw) {
                    found_gsi = (int)gsi;
                    kprintf("[I2C%d] IOAPIC probe HIT (LAPIC IRR/ISR): GSI %u "
                            "vector %u (%s) IRR=0x%x ISR=0x%x\n",
                            ctrl->bus_id, gsi, vector, combos[ci].name,
                            irr, isr);
                    // Clear the stuck ISR/IRR by sending EOI
                    if (isr & vec_bit_sw)
                        i2c_wrmsr(X2APIC_EOI_MSR, 0);
                }
            }

            // Mask this entry regardless (if found, we'll reprogram later)
            ioapic_mask_gsi((uint8_t)gsi);
        }
    }

    // If sweep failed, do a final diagnostic: try GSI 24-39 with the
    // actual IPC polarity and check LAPIC ISR/IRR to see if the interrupt
    // is arriving at the LAPIC but not being delivered to the CPU.
    // Detect x2APIC locally (lapic_init hasn't run yet at this init stage).
    if (found_gsi < 0) {
        kprintf("[I2C%d] probe: sweep failed, checking LAPIC ISR/IRR...\n",
                ctrl->bus_id);
        uint32_t vec_reg = (uint32_t)vector / 32;
        uint32_t vec_bit = 1u << ((uint32_t)vector % 32);

        // Check IA32_APIC_BASE MSR bit 10 for x2APIC mode
        uint64_t apic_base_msr = i2c_rdmsr(IA32_APIC_BASE_MSR);
        int x2apic = (apic_base_msr >> 10) & 1;
        kprintf("[I2C%d] LAPIC diag: IA32_APIC_BASE=0x%llx x2APIC=%d\n",
                ctrl->bus_id, (unsigned long long)apic_base_msr, x2apic);

        for (uint32_t gsi = 24; gsi <= 39 && gsi <= max_gsi; gsi++) {
            dw_write(ctrl, DW_IC_INTR_MASK, DW_IC_INTR_TX_EMPTY);

            // Determine polarity from IPC: active-HIGH unless AHPOLDIS set
            uint32_t ahpoldis;
            if (gsi < 32) ahpoldis = (ipc0 >> gsi) & 1;
            else          ahpoldis = (ipc1 >> (gsi - 32)) & 1;
            uint32_t pol = ahpoldis ? (1u << 13) : 0;

            uint32_t low = (uint32_t)vector | pol | (1u << 15);
            ioapic_write_rte((uint8_t)gsi, low, (bsp_apic_id & 0xFF) << 24);

            for (volatile int d = 0; d < 8000; d++)
                __asm__ volatile("pause");

            // Read LAPIC ISR/IRR — use MSR if x2APIC, else lapic_read()
            uint32_t isr_val, irr_val;
            if (x2apic) {
                uint32_t isr_msr = X2APIC_MSR_BASE_LOCAL +
                                   ((LAPIC_ISR_BASE + vec_reg * 0x10) >> 4);
                uint32_t irr_msr = X2APIC_MSR_BASE_LOCAL +
                                   ((LAPIC_IRR_BASE + vec_reg * 0x10) >> 4);
                isr_val = (uint32_t)i2c_rdmsr(isr_msr);
                irr_val = (uint32_t)i2c_rdmsr(irr_msr);
            } else {
                uint32_t isr_off = LAPIC_ISR_BASE + vec_reg * 0x10;
                uint32_t irr_off = LAPIC_IRR_BASE + vec_reg * 0x10;
                isr_val = lapic_read(isr_off);
                irr_val = lapic_read(irr_off);
            }

            if ((isr_val & vec_bit) || (irr_val & vec_bit)) {
                kprintf("[I2C%d] GSI %u: LAPIC vec %u set ISR=0x%x IRR=0x%x "
                        "(pol=%s) — interrupt REACHED LAPIC\n",
                        ctrl->bus_id, gsi, vector, isr_val, irr_val,
                        ahpoldis ? "low" : "high");
            }
            ioapic_mask_gsi((uint8_t)gsi);
        }
    }

    // Disable DW interrupt output and clear pending
    dw_write(ctrl, DW_IC_INTR_MASK, 0);
    (void)dw_read(ctrl, DW_IC_CLR_INTR);

    // Disable controller again (set_target will re-enable for real transfers)
    dw_write(ctrl, DW_IC_ENABLE, 0);
    i2c_delay_us(1000);

    g_i2c_probe_mode = 0;

    // Restore IF to original state
    if (!(rflags & 0x200))
        __asm__ volatile("cli");

    if (found_gsi < 0)
        kprintf("[I2C%d] IOAPIC probe: no GSI found (swept 24-%u, 4 combos)\n",
                ctrl->bus_id, max_gsi);

    return found_gsi;
}

static int detect_i2c_controllers(void) {
    int count = 0;
    const pci_device_t *devices;
    int pci_count = 0;

    devices = pci_get_devices(&pci_count);
    if (!devices || pci_count == 0) return 0;

    // Initialize ECAM (PCIe memory-mapped config)
    ecam_init_bus0();

    // ---- Discovery strategy: PCI-first (like Linux intel-lpss driver) ----
    // Linux discovers LPSS I2C controllers purely via PCI device ID matching.
    // The BDF and BAR come straight from PCI config space — no ACPI needed
    // for controller discovery.  ACPI is only used later to find HID child
    // devices (PNP0C50/ACPI0C50) and to get I2C slave addresses via _CRS.
    //
    // 1. Scan PCI bus for Intel LPSS I2C device IDs
    // 2. Get BDF directly from pci_device_t
    // 3. Optionally resolve ACPI path (for child discovery), but don't require it

    // Collect PCI devices that match known LPSS I2C device IDs
    typedef struct {
        const pci_device_t *pci;
        char acpi_path[64];
    } lpss_ctrl_info_t;

    lpss_ctrl_info_t ctrl_info[I2C_DW_MAX_CONTROLLERS];
    int n_ctrl = 0;

    for (int i = 0; i < pci_count && n_ctrl < I2C_DW_MAX_CONTROLLERS; i++) {
        if (!is_intel_lpss_i2c(devices[i].vendor_id, devices[i].device_id))
            continue;

        lpss_ctrl_info_t *ci = &ctrl_info[n_ctrl];
        ci->pci = &devices[i];
        ci->acpi_path[0] = '\0';

        // Try to find ACPI path for this PCI device (best-effort)
        acpi_find_pci_acpi_path(devices[i].bus, devices[i].device,
                                devices[i].function,
                                ci->acpi_path, sizeof(ci->acpi_path));

        kprintf("[I2C] PCI %02x:%02x.%x DEV_%04x -> %s\n",
                devices[i].bus, devices[i].device, devices[i].function,
                devices[i].device_id,
                ci->acpi_path[0] ? ci->acpi_path : "(no ACPI)");
        n_ctrl++;
    }

    // If PCI scan found nothing, try ACPI child-based discovery as fallback
    if (n_ctrl == 0) {
        static const char *const hid_ids[] = { "PNP0C50", "ACPI0C50", NULL };
        char ctrl_paths[I2C_DW_MAX_CONTROLLERS][ACPI_AML_MAX_PATH];
        int n_acpi = 0;

        for (int h = 0; hid_ids[h] && n_acpi < I2C_DW_MAX_CONTROLLERS; h++) {
            acpi_aml_device_info_t devs[8];
            int ndev = acpi_aml_find_devices_by_hid(hid_ids[h], devs, 8);
            for (int d = 0; d < ndev && n_acpi < I2C_DW_MAX_CONTROLLERS; d++) {
                char parent[ACPI_AML_MAX_PATH];
                if (acpi_parent_path(devs[d].path, parent,
                                     ACPI_AML_MAX_PATH) != 0)
                    continue;
                if (path_already_listed(ctrl_paths, n_acpi, parent))
                    continue;

                // Resolve _ADR → PCI BDF
                uint64_t adr_val = 0;
                if (acpi_aml_exec_device_method(parent, "_ADR",
                                                &adr_val) != 0)
                    continue;
                uint8_t dev_a = (uint8_t)((adr_val >> 16) & 0xFF);
                uint8_t fn_a  = (uint8_t)(adr_val & 0xFF);

                // Find matching PCI device
                const pci_device_t *pdev = NULL;
                for (int p = 0; p < pci_count; p++) {
                    if (devices[p].bus == 0 &&
                        devices[p].device == dev_a &&
                        devices[p].function == fn_a) {
                        pdev = &devices[p];
                        break;
                    }
                }
                if (!pdev) continue;

                lpss_ctrl_info_t *ci = &ctrl_info[n_ctrl];
                ci->pci = pdev;
                i2c_memcpy(ci->acpi_path, parent, sizeof(ci->acpi_path));
                i2c_memcpy(ctrl_paths[n_acpi], parent, ACPI_AML_MAX_PATH);
                kprintf("[I2C] ACPI child %s -> ctrl %s (%02x:%02x.%x)\n",
                        devs[d].path, parent, 0, dev_a, fn_a);
                n_ctrl++;
                n_acpi++;
            }
        }
    }

    kprintf("[I2C] %d controller(s)\n", n_ctrl);

    for (int ci = 0; ci < n_ctrl && count < I2C_DW_MAX_CONTROLLERS; ci++) {
        const pci_device_t *pci_dev = ctrl_info[ci].pci;
        uint8_t bus_nr  = pci_dev->bus;
        uint8_t dev_nr  = pci_dev->device;
        uint8_t func_nr = pci_dev->function;

        kprintf("[I2C%d] PCI %02x:%02x.%x DEV_%04x VID_%04x\n",
                ci, bus_nr, dev_nr, func_nr,
                pci_dev->device_id, pci_dev->vendor_id);

        i2c_dw_controller_t *ctrl = &g_i2c_controllers[count];
        i2c_memset(ctrl, 0, sizeof(*ctrl));
        ctrl->pci_dev = pci_dev;
        ctrl->bus_id = ci;
        if (ctrl_info[ci].acpi_path[0])
            i2c_memcpy(ctrl->acpi_path, ctrl_info[ci].acpi_path,
                       sizeof(ctrl->acpi_path));

        // ====================================================================
        // Follow the Linux intel-lpss PCI probe sequence exactly:
        //   intel_lpss_pci_probe():
        //     1. pcim_enable_device()  → PMCSR D0, enable BAR (MSE)
        //     2. pci_set_master()      → BME
        //     3. intel_lpss_probe()    → ioremap BAR+0x200, init_dev()
        //   intel_lpss_init_dev():
        //     a. Assert reset   (write 0 to RESETS  @ priv+0x04)
        //     b. Deassert reset (write 0x7 to RESETS)
        //     c. Set remap addr (write BAR phys to priv+0x40)
        //
        // Linux does NOT touch D0I3C, PG_CONFIG, or any other
        // vendor-specific PCI config registers during probe.
        // ====================================================================

        // ---- Step 1: Read BAR0 from config space ----
        // Linux PCI core reads BARs during enumeration (before any driver).
        // pci_assign_unassigned_bars() already assigned 64-bit BARs from the
        // host bridge 64-bit window (~0x501C2xxxxx on Arrow Lake).
        // We just read the result here.
        uint32_t bar0_low = ecam_read32(bus_nr, dev_nr, func_nr, 0x10);
        uint64_t bar0_phys;

        if ((bar0_low & 0x6) == 0x4) {  // 64-bit BAR
            uint32_t bar1_high = ecam_read32(bus_nr, dev_nr, func_nr, 0x14);
            bar0_phys = ((uint64_t)bar1_high << 32) | (bar0_low & ~0xFULL);
        } else {
            bar0_phys = bar0_low & ~0xFULL;
        }

        kprintf("[I2C%d] BAR=0x%llx\n", ci, (unsigned long long)bar0_phys);

        if (bar0_phys == 0) {
            kprintf("[I2C%d] ERROR: BAR0 not configured\n", ci);
            continue;
        }

        ctrl->bar_phys = bar0_phys;

        // ---- Step 2: ACPI power-on (like Linux pcim_enable_device →
        //              pci_power_up → platform_pci_set_power_state →
        //              acpi_pci_set_power_state → acpi_device_set_power →
        //              _PS0) ----
        {
            int ps0_rc = acpi_fw_power_on_pci_device(bus_nr, dev_nr, func_nr);
            kprintf("[I2C%d] ACPI power-on rc=%d\n", ci, ps0_rc);
        }

        // ---- Step 3: PMCSR → D0 ----
        uint8_t pm_cap = pci_find_capability(pci_dev, 0x01);
        if (pm_cap) {
            uint16_t pmcsr = ecam_read16(bus_nr, dev_nr, func_nr, pm_cap + 4);
            uint16_t old_pm = pmcsr;
            uint16_t new_pm = (pmcsr & ~PCI_PMCSR_POWER_STATE_MASK) |
                              PCI_PMCSR_PME_STATUS;
            ecam_write16(bus_nr, dev_nr, func_nr, pm_cap + 4, new_pm);
            i2c_delay_us(50000);
            kprintf("[I2C%d] PMCSR 0x%x->0x%x\n", ci, old_pm,
                    ecam_read16(bus_nr, dev_nr, func_nr, pm_cap + 4));
        }

        // ---- Step 4: Enable Memory Space + Bus Master ----
        {
            uint32_t cmd = ecam_read32(bus_nr, dev_nr, func_nr, 0x04);
            kprintf("[I2C%d] CMD 0x%x->0x%x\n", ci, cmd, cmd | 0x6);
            ecam_write32(bus_nr, dev_nr, func_nr, 0x04, cmd | 0x6);
        }

        // ---- Step 5: Map BAR0 ----
        uint64_t mapped = mm_map_device_mmio(bar0_phys, I2C_LPSS_MMIO_PAGES);
        if (!mapped) {
            kprintf("[I2C%d] ERROR: map BAR0 failed\n", ci);
            continue;
        }
        ctrl->base = (volatile uint32_t *)mapped;

        // ---- Step 5: Initialize the DesignWare I2C controller ----
        // dw_i2c_init_controller() does exactly what Linux intel_lpss_init_dev()
        // does: assert reset, deassert reset, set remap addr.
        // Plus DW core configuration (speed, FIFO, etc).
        if (dw_i2c_init_controller(ctrl) == 0) {
            uint8_t vector = I2C_MSI_VECTOR_BASE + (uint8_t)count;
            int irq_ok = 0;

            // Dump PCI capability list via ECAM for diagnostics
            {
                uint32_t sts = ecam_read32(bus_nr, dev_nr, func_nr, 0x04);
                kprintf("[I2C%d] PCI STS=0x%04x CMD=0x%04x caplist=%d\n",
                        ci, (sts >> 16) & 0xFFFF, sts & 0xFFFF,
                        (int)((sts >> 16) & (1 << 4)) != 0);
                if ((sts >> 16) & (1 << 4)) {
                    uint8_t ptr = (uint8_t)(ecam_read32(bus_nr, dev_nr, func_nr, 0x34) & 0xFC);
                    for (int ci2 = 0; ci2 < 48 && ptr; ci2++) {
                        uint32_t hdr = ecam_read32(bus_nr, dev_nr, func_nr, ptr);
                        uint8_t cap_id = (uint8_t)(hdr & 0xFF);
                        kprintf("[I2C%d]   CAP @ 0x%02x: id=0x%02x\n",
                                ci, ptr, cap_id);
                        ptr = (uint8_t)((hdr >> 8) & 0xFC);
                    }
                }
            }

            // Try MSI first (cap 0x05)
            if (pci_enable_msi(pci_dev, vector) == 0) {
                kprintf("[I2C%d] MSI vector %u enabled\n", ci, vector);
                irq_ok = 1;
            }

            // MSI unavailable — try ACPI _CRS on the controller's own
            // ACPI device node.  Intel LPSS Serial I/O controllers on
            // Arrow Lake (and similar PCHs) are absent from the root
            // bridge's _PRT; instead the BIOS puts an Extended IRQ
            // resource directly in the controller's _CRS.
            if (!irq_ok && ctrl->acpi_path[0]) {
                acpi_crs_result_t crs;
                if (acpi_aml_eval_crs(ctrl->acpi_path, &crs) == 0 &&
                    crs.irq_count > 0) {
                    uint32_t gsi = crs.irqs[0];
                    uint8_t pol = crs.irq_polarity
                                  ? IOAPIC_POLARITY_LOW : IOAPIC_POLARITY_HIGH;
                    uint8_t trig = crs.irq_triggering
                                   ? IOAPIC_TRIGGER_EDGE : IOAPIC_TRIGGER_LEVEL;
                    kprintf("[I2C%d] _CRS GSI %u (pol=%u trig=%u)\n",
                            ci, gsi, pol, trig);
                    if (ioapic_configure_legacy_irq((uint8_t)gsi, vector,
                                                    pol, trig) == 0) {
                        kprintf("[I2C%d] IOAPIC GSI %u -> vector %u\n",
                                ci, gsi, vector);
                        irq_ok = 1;
                    }
                }
            }

            // Fall back to IOAPIC via ACPI _PRT on the parent bridge.
            // Re-read INTPIN from config space (BIOS may program it even
            // though the PCH datasheet default is 0).
            if (!irq_ok && ctrl->acpi_path[0]) {
                char bridge[64];
                if (acpi_parent_path(ctrl->acpi_path, bridge, sizeof(bridge)) == 0) {
                    // Read INTPIN fresh via ECAM — the cached pci_device_t
                    // may have been read via legacy CF8/CFC before BIOS
                    // finished programming.
                    uint32_t ireg = ecam_read32(bus_nr, dev_nr, func_nr, 0x3C);
                    uint8_t pin = (uint8_t)((ireg >> 8) & 0xFF);
                    uint32_t gsi = 0;

                    kprintf("[I2C%d] PCI INTPIN=%u (fresh ECAM read)\n",
                            ci, pin);

                    if (pin >= 1 && pin <= 4 &&
                        acpi_pci_lookup_irq(bridge, dev_nr, pin - 1, &gsi) == 0) {
                        if (ioapic_configure_legacy_irq((uint8_t)gsi, vector,
                                                        IOAPIC_POLARITY_LOW,
                                                        IOAPIC_TRIGGER_LEVEL) == 0) {
                            kprintf("[I2C%d] IOAPIC GSI %u -> vector %u (PRT pin %u)\n",
                                    ci, gsi, vector, pin);
                            irq_ok = 1;
                        }
                    }

                    // If INTPIN=0, try each pin (INTA-INTD) against _PRT.
                    // Some BIOS leave INTPIN=0 but the _PRT still has an
                    // entry for the device slot.
                    if (!irq_ok && pin == 0) {
                        for (uint8_t try_pin = 0; try_pin < 4 && !irq_ok; try_pin++) {
                            gsi = 0;
                            if (acpi_pci_lookup_irq(bridge, dev_nr, try_pin, &gsi) == 0) {
                                if (ioapic_configure_legacy_irq((uint8_t)gsi, vector,
                                                                IOAPIC_POLARITY_LOW,
                                                                IOAPIC_TRIGGER_LEVEL) == 0) {
                                    kprintf("[I2C%d] IOAPIC GSI %u -> vector %u (PRT scan pin %u)\n",
                                            ci, gsi, vector, try_pin);
                                    irq_ok = 1;
                                }
                            }
                        }
                    }
                }
            }

            // Last resort: PCI interrupt_line as direct IOAPIC GSI.
            // On Intel PCH, BIOS often programs INTLINE with the IOAPIC
            // GSI even when INTPIN=0.  Try using it directly.
            if (!irq_ok) {
                uint32_t ireg = ecam_read32(bus_nr, dev_nr, func_nr, 0x3C);
                uint8_t intline = (uint8_t)(ireg & 0xFF);
                uint8_t pin = (uint8_t)((ireg >> 8) & 0xFF);

                if (intline != 0 && intline != 0xFF) {
                    kprintf("[I2C%d] PCI INTx fallback: line=%u pin=%u\n",
                            ci, intline, pin);
                    if (ioapic_configure_legacy_irq(intline, vector,
                                                    IOAPIC_POLARITY_LOW,
                                                    IOAPIC_TRIGGER_LEVEL) == 0) {
                        kprintf("[I2C%d] IOAPIC GSI %u -> vector %u\n",
                                ci, intline, vector);
                        irq_ok = 1;
                    }
                }
            }

            if (irq_ok) {
                ctrl->irq_vector = vector;
                kprintf("[I2C%d] PCI IRQ configured (vector %u)\n", ci, vector);
            } else {
                // All standard methods failed (MSI, _CRS, _PRT, INTLINE).
                // On Arrow Lake PCH 600/700 the LPSS I2C controllers at D21
                // use direct IOAPIC entries (GSI 24+) programmed by the
                // firmware through the ITSS.  Neither _PRT nor _CRS exposes
                // the mapping; Linux relies on VT-d IR.
                //
                // Experiment: Write PCI INTLINE with a chosen GSI before
                // probing. On some PCH revisions the ITSS uses INTLINE to
                // determine which IOAPIC entry the device's interrupt
                // routes to. If INTLINE=0 (Dell default), no routing
                // occurs and the probe sweep will never fire.
                uint8_t chosen_gsi = (uint8_t)(25 + count);
                uint32_t ireg_before = ecam_read32(bus_nr, dev_nr, func_nr, 0x3C);
                ecam_write32(bus_nr, dev_nr, func_nr, 0x3C,
                             (ireg_before & 0xFFFFFF00u) | chosen_gsi);
                uint32_t ireg_after = ecam_read32(bus_nr, dev_nr, func_nr, 0x3C);
                kprintf("[I2C%d] PCI INTLINE: 0x%x -> 0x%x (wrote GSI %u)\n",
                        ci, ireg_before & 0xFF, ireg_after & 0xFF, chosen_gsi);

                // Probe the IOAPIC: enable the DW core's TX_EMPTY interrupt
                // and sweep entries 24-119 to find which one fires.
                kprintf("[I2C%d] Probing IOAPIC for direct GSI (vector %u)...\n",
                        ci, vector);
                int gsi = probe_ioapic_i2c_gsi(ctrl, vector);
                if (gsi >= 0) {
                    // Found it — configure the IOAPIC entry permanently.
                    // PCH ITSS IPC default for IRQ 24+: active-HIGH.
                    if (ioapic_configure_legacy_irq((uint8_t)gsi, vector,
                                                    IOAPIC_POLARITY_HIGH,
                                                    IOAPIC_TRIGGER_LEVEL) == 0) {
                        kprintf("[I2C%d] IOAPIC GSI %d -> vector %u (probed)\n",
                                ci, gsi, vector);
                        ctrl->irq_vector = vector;
                        ctrl->irq_gsi = (uint32_t)gsi;
                        irq_ok = 1;
                        // Mask the IOAPIC entry — only unmask during active
                        // transfers.  The DW core's interrupt output stays
                        // HIGH even with IC_INTR_MASK=0 (Intel LPSS quirk),
                        // which causes a 60 kHz ISR storm if left unmasked.
                        ioapic_mask_gsi((uint8_t)gsi);
                        // Record this GSI so the next probe doesn't mask it
                        if (g_i2c_claimed_gsi_count < I2C_DW_MAX_CONTROLLERS)
                            g_i2c_claimed_gsi[g_i2c_claimed_gsi_count++] = gsi;
                    }
                }
            }

            if (irq_ok) {
                // Start with interrupts masked; the transfer routine
                // enables specific interrupts per-transfer.
                dw_write(ctrl, DW_IC_INTR_MASK, 0);
                (void)dw_read(ctrl, DW_IC_CLR_INTR);
            } else {
                kprintf("[I2C%d] WARNING: no interrupt source found\n", ci);
            }
            count++;
        }
    }

    g_i2c_controller_count = count;
    return count;
}

// ============================================================================
// HID over I2C — Descriptor fetch and device probe
// ============================================================================

// Try to read HID descriptor from a device at a given register address
static int i2c_hid_fetch_descriptor(i2c_dw_controller_t *ctrl, uint16_t addr,
                                     uint16_t desc_reg, i2c_hid_desc_t *desc) {
    uint8_t buf[30];
    i2c_memset(buf, 0, sizeof(buf));

    int rc = dw_i2c_read_reg16(ctrl, addr, desc_reg, buf, 30);
    if (rc < 0) return -1;

    // Copy to descriptor struct (already little-endian on x86)
    i2c_memcpy(desc, buf, 30);

    // Validate
    if (desc->wHIDDescLength != 30) return -1;
    if (desc->bcdVersion < 0x0100 || desc->bcdVersion > 0x0300) return -1;
    if (desc->wReportDescLength == 0 || desc->wReportDescLength > 4096) return -1;
    if (desc->wMaxInputLength < 3) return -1;

    return 0;
}

// Try to discover an I2C HID device at a given address
static int i2c_hid_probe_device(i2c_dw_controller_t *ctrl, uint16_t addr) {
    if (g_i2c_hid_device_count >= I2C_HID_MAX_DEVICES)
        return -1;

    i2c_hid_device_t *dev = &g_i2c_hid_devices[g_i2c_hid_device_count];
    i2c_memset(dev, 0, sizeof(*dev));
    dev->ctrl = ctrl;
    dev->i2c_addr = addr;

    // Try common HID descriptor register addresses
    static const uint16_t desc_regs[] = { 0x0001, 0x0020, 0x0030 };

    int found = 0;
    for (int r = 0; r < 3; r++) {
        if (i2c_hid_fetch_descriptor(ctrl, addr, desc_regs[r], &dev->desc) == 0) {
            dev->hid_desc_reg = desc_regs[r];
            found = 1;
            break;
        }
    }

    if (!found) return -1;

    kprintf("[I2C-HID] Device at I2C%d:0x%02x  VID=%04x PID=%04x  "
            "descReg=0x%04x  input=%u bytes  reportDesc=%u bytes\n",
            ctrl->bus_id, addr,
            dev->desc.wVendorID, dev->desc.wProductID,
            dev->hid_desc_reg,
            dev->desc.wMaxInputLength, dev->desc.wReportDescLength);

    g_i2c_hid_device_count++;
    return 0;
}

// ============================================================================
// HID over I2C — Commands
// ============================================================================

static int i2c_hid_set_power(i2c_hid_device_t *dev, uint16_t power_state) {
    uint16_t cmd_reg = dev->desc.wCommandRegister;
    uint16_t opcode = I2C_HID_OPCODE_SET_POWER | power_state;
    uint8_t buf[4] = {
        (uint8_t)(cmd_reg & 0xFF),
        (uint8_t)((cmd_reg >> 8) & 0xFF),
        (uint8_t)(opcode & 0xFF),
        (uint8_t)((opcode >> 8) & 0xFF)
    };
    kprintf("[I2C-HID] SET_POWER wire: [%02x %02x %02x %02x]\n",
            buf[0], buf[1], buf[2], buf[3]);
    return dw_i2c_write(dev->ctrl, dev->i2c_addr, buf, 4);
}

static int i2c_hid_reset(i2c_hid_device_t *dev) {
    uint16_t cmd_reg = dev->desc.wCommandRegister;
    uint16_t opcode = I2C_HID_OPCODE_RESET;
    uint8_t buf[4] = {
        (uint8_t)(cmd_reg & 0xFF),
        (uint8_t)((cmd_reg >> 8) & 0xFF),
        (uint8_t)(opcode & 0xFF),
        (uint8_t)((opcode >> 8) & 0xFF)
    };
    kprintf("[I2C-HID] RESET wire: [%02x %02x %02x %02x]\n",
            buf[0], buf[1], buf[2], buf[3]);
    int rc = dw_i2c_write(dev->ctrl, dev->i2c_addr, buf, 4);
    if (rc < 0) return rc;

    // Wait for reset to complete (spec says up to 5 seconds, typically <100ms)
    // The device will pull INT low and send a reset-completion 2-byte message
    i2c_delay_us(100000);  // 100ms

    // Read and discard the reset-completion response using length-first
    // protocol: read 2-byte header, then actual report length.
    uint16_t resp_buf_size = dev->input_buf_size;
    if (resp_buf_size < I2C_HID_LENGTH_HDR_SIZE)
        resp_buf_size = I2C_HID_LENGTH_HDR_SIZE;
    uint8_t *resp = dev->input_buf;
    if (!resp) {
        // Fallback: stack buffer for minimal drain during early init
        uint8_t resp_stack[I2C_HID_LENGTH_HDR_SIZE];
        dw_i2c_xfer_irq(dev->ctrl, dev->i2c_addr, NULL, 0,
                         resp_stack, I2C_HID_LENGTH_HDR_SIZE);
        uint16_t rst_len = (uint16_t)resp_stack[0] | ((uint16_t)resp_stack[1] << 8);
        kprintf("[I2C-HID] RESET completion (minimal): len=%u [%02x %02x]\n",
                rst_len, resp_stack[0], resp_stack[1]);
        return 0;
    }

    int drain_rc = i2c_hid_read_length_first(dev, resp, resp_buf_size);
    uint16_t rst_len = (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);
    kprintf("[I2C-HID] RESET completion: len=%u rc=%d [%02x %02x %02x %02x]\n",
            rst_len, drain_rc, resp[0], resp[1],
            resp_buf_size > 2 ? resp[2] : 0, resp_buf_size > 3 ? resp[3] : 0);

    return 0;
}

static int i2c_hid_set_idle(i2c_hid_device_t *dev, uint8_t duration,
                             uint8_t report_id) {
    uint16_t cmd_reg = dev->desc.wCommandRegister;
    // Low byte: reportID, high byte: opcode 0x05
    // Duration goes in a 3rd byte appended to the data register
    // For the simple 4-byte form: low byte = reportID, opcode in high byte
    uint16_t opcode = I2C_HID_OPCODE_SET_IDLE | report_id;
    uint8_t buf[6] = {
        (uint8_t)(cmd_reg & 0xFF),
        (uint8_t)((cmd_reg >> 8) & 0xFF),
        (uint8_t)(opcode & 0xFF),
        (uint8_t)((opcode >> 8) & 0xFF),
        (uint8_t)(dev->desc.wDataRegister & 0xFF),
        (uint8_t)((dev->desc.wDataRegister >> 8) & 0xFF),
    };
    // SET_IDLE with duration in wData register: 6 bytes total
    // But if duration is 0 (infinite) and reportID is 0, the simple
    // 4-byte command form works too.
    int len = (duration == 0) ? 4 : 6;
    kprintf("[I2C-HID] SET_IDLE wire: [%02x %02x %02x %02x] dur=%u rid=%u\n",
            buf[0], buf[1], buf[2], buf[3], duration, report_id);
    return dw_i2c_write(dev->ctrl, dev->i2c_addr, buf, len);
}

static int i2c_hid_set_protocol(i2c_hid_device_t *dev, uint16_t protocol) {
    uint16_t cmd_reg = dev->desc.wCommandRegister;
    uint16_t opcode = I2C_HID_OPCODE_SET_PROTOCOL | protocol;
    uint8_t buf[4] = {
        (uint8_t)(cmd_reg & 0xFF),
        (uint8_t)((cmd_reg >> 8) & 0xFF),
        (uint8_t)(opcode & 0xFF),
        (uint8_t)((opcode >> 8) & 0xFF)
    };
    kprintf("[I2C-HID] SET_PROTOCOL wire: [%02x %02x %02x %02x] proto=%u\n",
            buf[0], buf[1], buf[2], buf[3], protocol);
    return dw_i2c_write(dev->ctrl, dev->i2c_addr, buf, 4);
}

// SET_REPORT: send a feature/output report to the device.
// report_type: 1=Input, 2=Output, 3=Feature
// report_id:   the report ID
// data/data_len: report payload (NOT including report ID)
static int i2c_hid_set_report(i2c_hid_device_t *dev, uint8_t report_type,
                               uint8_t report_id, const uint8_t *data,
                               int data_len) {
    uint16_t cmd_reg  = dev->desc.wCommandRegister;
    uint16_t data_reg = dev->desc.wDataRegister;

    // Command register word:
    //   low byte  = reportID (bits 0-3) | reportType (bits 4-5)
    //   high byte = opcode (SET_REPORT = 0x03)
    uint8_t cmd_lo = (report_id & 0x0F) | ((report_type & 0x03) << 4);
    uint8_t cmd_hi = 0x03;  // SET_REPORT opcode

    // Data register payload length:
    // 2 (length field) + 1 (reportID) + data_len
    uint16_t payload_len = 2 + 1 + (uint16_t)data_len;

    // Total wire message:
    // [cmdReg_lo, cmdReg_hi, cmd_lo, cmd_hi,
    //  dataReg_lo, dataReg_hi, len_lo, len_hi, reportID, data...]
    int total = 4 + 2 + 2 + 1 + data_len;  // 9 + data_len
    if (total > 64) return -1;  // Safety limit

    uint8_t wire[64];
    wire[0] = (uint8_t)(cmd_reg & 0xFF);
    wire[1] = (uint8_t)((cmd_reg >> 8) & 0xFF);
    wire[2] = cmd_lo;
    wire[3] = cmd_hi;
    wire[4] = (uint8_t)(data_reg & 0xFF);
    wire[5] = (uint8_t)((data_reg >> 8) & 0xFF);
    wire[6] = (uint8_t)(payload_len & 0xFF);
    wire[7] = (uint8_t)((payload_len >> 8) & 0xFF);
    wire[8] = report_id;
    for (int i = 0; i < data_len; i++)
        wire[9 + i] = data[i];

    kprintf("[I2C-HID] SET_REPORT wire(%d): "
            "[%02x %02x %02x %02x %02x %02x %02x %02x %02x",
            total, wire[0], wire[1], wire[2], wire[3],
            wire[4], wire[5], wire[6], wire[7], wire[8]);
    for (int i = 0; i < data_len && i < 4; i++)
        kprintf(" %02x", wire[9 + i]);
    kprintf("]\n");

    return dw_i2c_write(dev->ctrl, dev->i2c_addr, wire, total);
}

// ============================================================================
// Minimal HID Report Descriptor Parser
// ============================================================================

// Parse a HID report descriptor to determine device type and report layout
// ============================================================================
// HID Usage Definitions (usage_page << 16 | usage)
// ============================================================================

#define HID_GD_MOUSE          0x00010002
#define HID_GD_X              0x00010030
#define HID_GD_Y              0x00010031
#define HID_GD_WHEEL          0x00010038
#define HID_DG_TOUCHSCREEN    0x000D0004
#define HID_DG_TOUCHPAD       0x000D0005
#define HID_DG_TIPSWITCH      0x000D0042
#define HID_DG_INPUTMODE      0x000D0052
#define HID_DG_CONTACTCOUNT   0x000D0054
#define HID_UP_BUTTON         0x0009

// Parsed field entry (used locally during descriptor parsing)
typedef struct {
    uint32_t usage;         // (usage_page << 16) | usage_id
    uint16_t bit_offset;    // Bit position in report (after report ID byte)
    uint16_t bit_size;      // Size in bits for this single field
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  flags;         // bit 0 = constant, bit 2 = relative
    uint8_t  report_type;   // 0 = Input, 2 = Feature
    uint8_t  report_id;
    uint8_t  app_type;      // 0 = unknown, 1 = touchpad/touchscreen, 2 = mouse
} hid_parsed_field_t;

#define HID_FIELD_MAX   128
#define HID_USAGE_MAX   16

// ============================================================================
// Linux-style HID Report Descriptor Parser
//
// Phase 1: Walk the descriptor building a flat field table.  Every
//          non-constant Input/Feature field gets an entry with its full
//          32-bit usage, bit offset, size, and logical range.
//
// Phase 2: Resolve specific usages from the table:
//          - TIP_SWITCH  for finger-down detection
//          - X, Y        for coordinates (within each contact slot)
//          - CONTACT_CNT for multi-touch count
//          - BUTTON      for physical clicks
//          - INPUT_MODE  feature for SET_REPORT
//
// This avoids fragile collection-depth heuristics: the app_type tag on
// each field is used only to prefer Touchpad fields over Mouse fields
// when both are present.
// ============================================================================

static void i2c_hid_parse_report_desc(i2c_hid_device_t *dev) {
    uint8_t *rd = dev->report_desc;
    uint16_t rd_len = dev->report_desc_len;

    // Flat field table — built in Phase 1, resolved in Phase 2
    hid_parsed_field_t fields[HID_FIELD_MAX];
    int field_count = 0;

    // ---- Parser global state ----
    uint16_t usage_page   = 0;
    int32_t  logical_min  = 0;
    int32_t  logical_max  = 0;
    uint32_t report_size  = 0;
    uint32_t report_count = 0;
    uint8_t  report_id    = 0;

    // Per-type bit offset tracking (Input and Feature have separate bit streams)
    uint16_t input_bit_offset = 0;
    uint16_t feat_bit_offset  = 0;
    uint8_t  input_last_rid   = 0xFF;  // sentinel
    uint8_t  feat_last_rid    = 0xFF;

    // ---- Local state (reset after each Main item) ----
    uint16_t usage_stack[HID_USAGE_MAX];
    int      usage_count     = 0;
    uint16_t usage_min       = 0;
    uint16_t usage_max       = 0;
    int      has_usage_range = 0;

    // ---- Collection tracking (for app_type classification) ----
    uint8_t  collection_depth = 0;
    uint8_t  app_type         = 0;   // 0=unknown, 1=touchpad, 2=mouse
    uint8_t  app_stack[16];
    int      app_stack_depth  = 0;

    // ================================================================
    //  Phase 1 — walk descriptor, populate field table
    // ================================================================

    uint16_t pos = 0;
    while (pos < rd_len) {
        uint8_t prefix = rd[pos];

        // Long items (prefix == 0xFE) — skip
        if (prefix == 0xFE) {
            if (pos + 2 >= rd_len) break;
            pos += 3 + rd[pos + 1];
            continue;
        }

        // Short items
        uint8_t bSize = prefix & 0x03;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bTag  = (prefix >> 4) & 0x0F;
        int size = (bSize == 3) ? 4 : bSize;

        if (pos + 1 + size > rd_len) break;

        // Extract item data
        uint32_t data_unsigned = 0;
        int32_t  data_signed   = 0;
        if (size >= 1) data_unsigned  = rd[pos + 1];
        if (size >= 2) data_unsigned |= (uint32_t)rd[pos + 2] << 8;
        if (size >= 3) data_unsigned |= (uint32_t)rd[pos + 3] << 16;
        if (size >= 4) data_unsigned |= (uint32_t)rd[pos + 4] << 24;

        if (size == 1)      data_signed = (int8_t)(data_unsigned & 0xFF);
        else if (size == 2) data_signed = (int16_t)(data_unsigned & 0xFFFF);
        else                data_signed = (int32_t)data_unsigned;

        switch (bType) {
        case 0: // Main
            switch (bTag) {
            case 0x08: // Input
            case 0x0B: // Feature
            {
                uint8_t is_const    = data_unsigned & 0x01;
                uint8_t is_relative = (data_unsigned & 0x04) ? 1 : 0;
                uint8_t rtype       = (bTag == 0x0B) ? 2 : 0;

                // Select the correct per-type bit offset
                uint16_t *cur_offset;
                uint8_t  *cur_rid;
                if (rtype == 0) {
                    cur_offset = &input_bit_offset;
                    cur_rid    = &input_last_rid;
                } else {
                    cur_offset = &feat_bit_offset;
                    cur_rid    = &feat_last_rid;
                }
                if (report_id != *cur_rid) {
                    *cur_offset = 0;
                    *cur_rid    = report_id;
                }

                // Detect Input Mode feature inline (before field table,
                // so it works even if the table overflows).
                if (rtype == 2 && !is_const && usage_count > 0 &&
                    (((uint32_t)usage_page << 16) | usage_stack[0])
                        == HID_DG_INPUTMODE &&
                    dev->input_mode_rid == 0) {
                    dev->input_mode_rid  = report_id;
                    dev->input_mode_size =
                        (uint8_t)((report_size * report_count + 7) / 8);
                    kprintf("[I2C-HID] Found Input Mode feature: "
                            "reportID=%u size=%u bits\n",
                            report_id, report_size * report_count);
                }

                // Record non-constant fields into the table.
                // Expand per-item only when there's a usage range or
                // multiple local usages; otherwise collapse into one
                // entry to avoid blowing through the table limit.
                if (!is_const) {
                    if (has_usage_range) {
                        // Usage Min/Max range — one entry per item
                        for (uint32_t i = 0; i < report_count &&
                                              field_count < HID_FIELD_MAX; i++) {
                            uint16_t u = (uint16_t)(usage_min + i);
                            if (u > usage_max) u = usage_max;
                            hid_parsed_field_t *f = &fields[field_count++];
                            f->usage       = ((uint32_t)usage_page << 16) | u;
                            f->bit_offset  = *cur_offset +
                                             (uint16_t)(i * report_size);
                            f->bit_size    = (uint16_t)report_size;
                            f->logical_min = logical_min;
                            f->logical_max = logical_max;
                            f->flags       = (is_relative ? 4 : 0);
                            f->report_type = rtype;
                            f->report_id   = report_id;
                            f->app_type    = app_type;
                        }
                    } else if (usage_count > 1) {
                        // Multiple distinct usages — one entry per usage
                        for (uint32_t i = 0; i < report_count &&
                                              field_count < HID_FIELD_MAX; i++) {
                            uint16_t u = ((int)i < usage_count)
                                             ? usage_stack[i]
                                             : usage_stack[usage_count - 1];
                            hid_parsed_field_t *f = &fields[field_count++];
                            f->usage       = ((uint32_t)usage_page << 16) | u;
                            f->bit_offset  = *cur_offset +
                                             (uint16_t)(i * report_size);
                            f->bit_size    = (uint16_t)report_size;
                            f->logical_min = logical_min;
                            f->logical_max = logical_max;
                            f->flags       = (is_relative ? 4 : 0);
                            f->report_type = rtype;
                            f->report_id   = report_id;
                            f->app_type    = app_type;
                        }
                    } else if (field_count < HID_FIELD_MAX) {
                        // Single usage (or none) — collapse entire item
                        // into one entry covering all report_count values.
                        uint16_t u = (usage_count > 0) ? usage_stack[0] : 0;
                        hid_parsed_field_t *f = &fields[field_count++];
                        f->usage       = ((uint32_t)usage_page << 16) | u;
                        f->bit_offset  = *cur_offset;
                        f->bit_size    = (uint16_t)report_size;
                        f->logical_min = logical_min;
                        f->logical_max = logical_max;
                        f->flags       = (is_relative ? 4 : 0);
                        f->report_type = rtype;
                        f->report_id   = report_id;
                        f->app_type    = app_type;
                    }
                }

                // Always advance bit offset (including const/padding)
                *cur_offset += (uint16_t)(report_size * report_count);

                // Reset local state after Main item
                usage_count     = 0;
                has_usage_range = 0;
                usage_min = usage_max = 0;
                break;
            }
            case 0x0A: // Collection
            {
                collection_depth++;
                uint16_t coll_usage = (usage_count > 0) ? usage_stack[0] : 0;
                uint32_t coll_u32   = ((uint32_t)usage_page << 16) | coll_usage;

                // Push parent app_type
                if (app_stack_depth < 16)
                    app_stack[app_stack_depth++] = app_type;

                // Detect application type from collection usage
                if (coll_u32 == HID_DG_TOUCHPAD ||
                    coll_u32 == HID_DG_TOUCHSCREEN)
                    app_type = 1;
                else if (coll_u32 == HID_GD_MOUSE)
                    app_type = 2;
                // else: inherit parent app_type

                // Reset local state
                usage_count     = 0;
                has_usage_range = 0;
                break;
            }
            case 0x0C: // End Collection
            {
                if (app_stack_depth > 0)
                    app_type = app_stack[--app_stack_depth];
                else
                    app_type = 0;
                if (collection_depth > 0) collection_depth--;
                break;
            }
            default:
                usage_count     = 0;
                has_usage_range = 0;
                break;
            }
            break;

        case 1: // Global
            switch (bTag) {
            case 0x00: usage_page   = (uint16_t)data_unsigned; break;
            case 0x01: logical_min  = data_signed; break;
            case 0x02: logical_max  = data_signed; break;
            case 0x07: report_size  = data_unsigned; break;
            case 0x08: // Report ID
                report_id = (uint8_t)data_unsigned;
                break;
            case 0x09: report_count = data_unsigned; break;
            }
            break;

        case 2: // Local
            switch (bTag) {
            case 0x00: // Usage
                if (usage_count < HID_USAGE_MAX)
                    usage_stack[usage_count++] = (uint16_t)data_unsigned;
                break;
            case 0x01: // Usage Minimum
                usage_min = (uint16_t)data_unsigned;
                has_usage_range = 1;
                break;
            case 0x02: // Usage Maximum
                usage_max = (uint16_t)data_unsigned;
                has_usage_range = 1;
                break;
            }
            break;
        }

        pos += 1 + size;
    }

    // ================================================================
    //  Phase 2 — resolve specific usages from the field table
    // ================================================================

    // --- Input Mode feature (0x000D0052) ---
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type == 2 && fields[i].usage == HID_DG_INPUTMODE
            && dev->input_mode_rid == 0) {
            dev->input_mode_rid  = fields[i].report_id;
            dev->input_mode_size = (uint8_t)((fields[i].bit_size + 7) / 8);
            kprintf("[I2C-HID] Found Input Mode feature: "
                    "reportID=%u size=%u bits\n",
                    fields[i].report_id, fields[i].bit_size);
            break;
        }
    }

    // --- Find TIP_SWITCH fields (each marks a contact slot) ---
    int tip_idx[8];
    int tip_count      = 0;
    uint8_t tp_rid     = 0;
    int tp_found       = 0;

    // First pass: prefer touchpad app_type (== 1)
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type != 0)         continue;
        if (fields[i].usage != HID_DG_TIPSWITCH) continue;
        if (fields[i].app_type != 1)             continue;
        if (!tp_found) { tp_rid = fields[i].report_id; tp_found = 1; }
        if (fields[i].report_id == tp_rid && tip_count < 8)
            tip_idx[tip_count++] = i;
    }

    // Second pass: any TIP_SWITCH
    if (!tp_found) {
        for (int i = 0; i < field_count; i++) {
            if (fields[i].report_type != 0)         continue;
            if (fields[i].usage != HID_DG_TIPSWITCH) continue;
            if (!tp_found) { tp_rid = fields[i].report_id; tp_found = 1; }
            if (fields[i].report_id == tp_rid && tip_count < 8)
                tip_idx[tip_count++] = i;
        }
    }

    // Fallback: find a report with GD_X (mouse-only device)
    if (!tp_found) {
        for (int i = 0; i < field_count; i++) {
            if (fields[i].report_type == 0 && fields[i].usage == HID_GD_X) {
                tp_rid   = fields[i].report_id;
                tp_found = 1;
                break;
            }
        }
    }

    if (!tp_found) {
        dev->dev_type = I2C_HID_DEV_UNKNOWN;
        kprintf("[I2C-HID] Could not determine device type from "
                "report descriptor (%d fields parsed)\n", field_count);
        return;
    }

    // --- Populate mouse_report from resolved fields ---
    i2c_hid_report_info_t *ri = &dev->mouse_report;
    i2c_memset(ri, 0, sizeof(*ri));
    ri->report_id     = tp_rid;
    ri->has_report_id = (tp_rid != 0);
    ri->dev_type      = (tip_count > 0) ? I2C_HID_DEV_TOUCHPAD
                                        : I2C_HID_DEV_MOUSE;
    dev->dev_type     = ri->dev_type;

    // Contact 0 TIP_SWITCH
    if (tip_count > 0) {
        int ti = tip_idx[0];
        ri->tip_switch.report_id  = fields[ti].report_id;
        ri->tip_switch.bit_offset = fields[ti].bit_offset;
        ri->tip_switch.bit_size   = fields[ti].bit_size;
        ri->tip_switch.logical_min = 0;
        ri->tip_switch.logical_max = 1;
        ri->has_tip_switch = 1;
    }

    // Contact 0 X, Y — fields between first and second TIP_SWITCH
    int c0_start = (tip_count > 0) ? tip_idx[0] : 0;
    int c0_end   = (tip_count > 1) ? tip_idx[1] : field_count;
    for (int i = c0_start; i < c0_end; i++) {
        if (fields[i].report_type != 0)        continue;
        if (fields[i].report_id != tp_rid)     continue;
        if (fields[i].usage == HID_GD_X && ri->x.bit_size == 0) {
            ri->x.report_id   = fields[i].report_id;
            ri->x.bit_offset  = fields[i].bit_offset;
            ri->x.bit_size    = fields[i].bit_size;
            ri->x.logical_min = fields[i].logical_min;
            ri->x.logical_max = fields[i].logical_max;
            ri->x.is_relative = (fields[i].flags & 0x04) ? 1 : 0;
        } else if (fields[i].usage == HID_GD_Y && ri->y.bit_size == 0) {
            ri->y.report_id   = fields[i].report_id;
            ri->y.bit_offset  = fields[i].bit_offset;
            ri->y.bit_size    = fields[i].bit_size;
            ri->y.logical_min = fields[i].logical_min;
            ri->y.logical_max = fields[i].logical_max;
            ri->y.is_relative = (fields[i].flags & 0x04) ? 1 : 0;
        }
    }

    // Fallback: search all fields for X/Y (mouse-only descriptor)
    if (ri->x.bit_size == 0 || ri->y.bit_size == 0) {
        for (int i = 0; i < field_count; i++) {
            if (fields[i].report_type != 0)    continue;
            if (fields[i].report_id != tp_rid) continue;
            if (fields[i].usage == HID_GD_X && ri->x.bit_size == 0) {
                ri->x.report_id   = fields[i].report_id;
                ri->x.bit_offset  = fields[i].bit_offset;
                ri->x.bit_size    = fields[i].bit_size;
                ri->x.logical_min = fields[i].logical_min;
                ri->x.logical_max = fields[i].logical_max;
                ri->x.is_relative = (fields[i].flags & 0x04) ? 1 : 0;
            } else if (fields[i].usage == HID_GD_Y && ri->y.bit_size == 0) {
                ri->y.report_id   = fields[i].report_id;
                ri->y.bit_offset  = fields[i].bit_offset;
                ri->y.bit_size    = fields[i].bit_size;
                ri->y.logical_min = fields[i].logical_min;
                ri->y.logical_max = fields[i].logical_max;
                ri->y.is_relative = (fields[i].flags & 0x04) ? 1 : 0;
            }
        }
    }

    // Contact Count (0x000D0054)
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type != 0)    continue;
        if (fields[i].report_id != tp_rid) continue;
        if (fields[i].usage == HID_DG_CONTACTCOUNT) {
            ri->contact_count.report_id  = fields[i].report_id;
            ri->contact_count.bit_offset = fields[i].bit_offset;
            ri->contact_count.bit_size   = fields[i].bit_size;
            ri->contact_count.logical_min = fields[i].logical_min;
            ri->contact_count.logical_max = fields[i].logical_max;
            ri->has_contact_count = 1;
            break;
        }
    }

    // Button — prefer touchpad app_type (== 1) over mouse (== 2)
    int btn_idx = -1;
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type != 0)        continue;
        if (fields[i].report_id != tp_rid)     continue;
        if ((fields[i].usage >> 16) != HID_UP_BUTTON) continue;
        if (btn_idx < 0 || fields[i].app_type == 1) {
            btn_idx = i;
            if (fields[i].app_type == 1) break;  // best match
        }
    }
    if (btn_idx >= 0) {
        // Count consecutive button fields for combined extraction
        uint16_t bstart = fields[btn_idx].bit_offset;
        uint16_t bbits  = fields[btn_idx].bit_size;
        for (int j = btn_idx + 1; j < field_count; j++) {
            if (fields[j].report_type != 0)        break;
            if (fields[j].report_id != tp_rid)     break;
            if ((fields[j].usage >> 16) != HID_UP_BUTTON) break;
            if (fields[j].bit_offset != bstart + bbits)    break;
            bbits += fields[j].bit_size;
        }
        ri->buttons.report_id  = fields[btn_idx].report_id;
        ri->buttons.bit_offset = bstart;
        ri->buttons.bit_size   = bbits;
        ri->buttons.count      = bbits / fields[btn_idx].bit_size;
        ri->buttons.logical_min = 0;
        ri->buttons.logical_max = 1;
    }

    // Wheel (0x00010038) — only useful for mouse devices
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type != 0)    continue;
        if (fields[i].report_id != tp_rid) continue;
        if (fields[i].usage == HID_GD_WHEEL) {
            ri->wheel.report_id   = fields[i].report_id;
            ri->wheel.bit_offset  = fields[i].bit_offset;
            ri->wheel.bit_size    = fields[i].bit_size;
            ri->wheel.logical_min = fields[i].logical_min;
            ri->wheel.logical_max = fields[i].logical_max;
            ri->wheel.is_relative = 1;
            ri->has_wheel = 1;
            break;
        }
    }

    // Calculate total report size from highest bit position
    uint16_t max_bit = 0;
    for (int i = 0; i < field_count; i++) {
        if (fields[i].report_type == 0 && fields[i].report_id == tp_rid) {
            uint16_t end = fields[i].bit_offset + fields[i].bit_size;
            if (end > max_bit) max_bit = end;
        }
    }
    ri->report_bytes = (max_bit + 7) / 8;

    kprintf("[I2C-HID] Parsed %s report (Linux-style, %d fields): "
            "ID=%d  tip@%u(%u)  buttons@%u(%u bits)  "
            "X@%u(%u bits%s)  Y@%u(%u bits%s)  "
            "cc@%u(%u)  wheel=%s  total=%u bytes\n",
            ri->dev_type == I2C_HID_DEV_TOUCHPAD ? "touchpad" : "mouse",
            field_count, ri->report_id,
            ri->tip_switch.bit_offset, ri->tip_switch.bit_size,
            ri->buttons.bit_offset, ri->buttons.bit_size,
            ri->x.bit_offset, ri->x.bit_size,
            ri->x.is_relative ? ",rel" : ",abs",
            ri->y.bit_offset, ri->y.bit_size,
            ri->y.is_relative ? ",rel" : ",abs",
            ri->has_contact_count ? ri->contact_count.bit_offset : 0,
            ri->has_contact_count ? ri->contact_count.bit_size : 0,
            ri->has_wheel ? "yes" : "no",
            ri->report_bytes);

    kprintf("[I2C-HID]   X range: %d..%d  Y range: %d..%d\n",
            ri->x.logical_min, ri->x.logical_max,
            ri->y.logical_min, ri->y.logical_max);
}

// ============================================================================
// HID over I2C — Device initialization
// ============================================================================

static int i2c_hid_init_device(i2c_hid_device_t *dev) {
    // Power on
    int rc = i2c_hid_set_power(dev, I2C_HID_POWER_ON);
    if (rc < 0) {
        kprintf("[I2C-HID] SET_POWER ON failed for I2C%d:0x%02x\n",
                dev->ctrl->bus_id, dev->i2c_addr);
        return -1;
    }
    i2c_delay_us(10000);  // 10ms settle

    // Reset device
    rc = i2c_hid_reset(dev);
    if (rc < 0) {
        kprintf("[I2C-HID] RESET failed for I2C%d:0x%02x\n",
                dev->ctrl->bus_id, dev->i2c_addr);
        // Continue anyway — some devices work without reset
    }

    // Re-read HID descriptor (may have changed after reset)
    i2c_hid_fetch_descriptor(dev->ctrl, dev->i2c_addr,
                              dev->hid_desc_reg, &dev->desc);

    // Allocate dynamic input buffer based on wMaxInputLength
    {
        uint16_t alloc_len = dev->desc.wMaxInputLength;
        if (alloc_len < I2C_HID_LENGTH_HDR_SIZE)
            alloc_len = I2C_HID_LENGTH_HDR_SIZE;
        if (dev->input_buf) {
            slab_free(dev->input_buf);
            dev->input_buf = NULL;
        }
        dev->input_buf = (uint8_t *)slab_alloc(alloc_len);
        if (!dev->input_buf) {
            kprintf("[I2C-HID] Failed to allocate input buffer (%u bytes)\n",
                    alloc_len);
            return -1;
        }
        dev->input_buf_size = alloc_len;
        i2c_memset(dev->input_buf, 0, alloc_len);
        kprintf("[I2C-HID] Allocated input buffer: %u bytes "
                "(wMaxInputLength=%u)\n",
                alloc_len, dev->desc.wMaxInputLength);
    }

    // Read report descriptor
    dev->report_desc_len = dev->desc.wReportDescLength;
    if (dev->report_desc_len > 0 && dev->report_desc_len <= 4096) {
        dev->report_desc = (uint8_t *)slab_alloc(dev->report_desc_len);
        if (dev->report_desc) {
            rc = dw_i2c_read_reg16(dev->ctrl, dev->i2c_addr,
                                    dev->desc.wReportDescRegister,
                                    dev->report_desc, dev->report_desc_len);
            if (rc < 0) {
                kprintf("[I2C-HID] Failed to read report descriptor\n");
                slab_free(dev->report_desc);
                dev->report_desc = NULL;
                // Try to use device as generic mouse/keyboard based on vendor
                if (dev->desc.wVendorID == 0x0488) {
                    dev->dev_type = I2C_HID_DEV_TOUCHPAD;
                    kprintf("[I2C-HID] Assuming ITE Tech touchpad (VID 0x0488)\n");
                }
            } else {
                // Parse report descriptor
                i2c_hid_parse_report_desc(dev);
            }
        }
    }

    // If we still don't know the type, guess based on vendor
    if (dev->dev_type == I2C_HID_DEV_UNKNOWN) {
        if (dev->desc.wVendorID == 0x0488) {
            dev->dev_type = I2C_HID_DEV_TOUCHPAD;
            kprintf("[I2C-HID] Guessing ITE Tech (0x0488) = touchpad\n");
        }
    }

    // Power on again (some devices need this after reset)
    i2c_hid_set_power(dev, I2C_HID_POWER_ON);
    i2c_delay_us(10000);

    // SET_IDLE(0) — infinite idle: only report when data changes
    i2c_hid_set_idle(dev, 0, 0);
    i2c_delay_us(1000);

    // SET_PROTOCOL — ensure Report Protocol mode (not Boot Protocol)
    i2c_hid_set_protocol(dev, I2C_HID_PROTOCOL_REPORT);
    i2c_delay_us(1000);

    // SET_REPORT (Feature): Enable touchpad Input Mode (value = 3)
    // Windows Precision Touchpads require this to switch from mouse
    // emulation to native touchpad reports with coordinates.
    if (dev->input_mode_rid != 0) {
        uint8_t mode_val = 0x03;  // Touchpad collection mode
        int sr_rc = i2c_hid_set_report(dev, 3, dev->input_mode_rid,
                                        &mode_val, 1);
        kprintf("[I2C-HID] SET_REPORT input mode=3 rid=%u rc=%d\n",
                dev->input_mode_rid, sr_rc);
        i2c_delay_us(10000);  // 10ms for mode switch
    } else {
        kprintf("[I2C-HID] No Input Mode feature found in report desc\n");
    }

    // Drain any pending reports left over from reset / init.
    if (dev->input_buf && dev->input_buf_size >= I2C_HID_LENGTH_HDR_SIZE) {
        for (int drain = 0; drain < 10; drain++) {
            int drc = i2c_hid_read_length_first(dev, dev->input_buf,
                                                 dev->input_buf_size);
            if (drc < 0) break;
            uint16_t dlen = (uint16_t)dev->input_buf[0] |
                            ((uint16_t)dev->input_buf[1] << 8);
            kprintf("[I2C-HID] drain[%d]: len=%u [%02x %02x %02x %02x]\n",
                    drain, dlen, dev->input_buf[0], dev->input_buf[1],
                    drc > 2 ? dev->input_buf[2] : 0,
                    drc > 3 ? dev->input_buf[3] : 0);
            if (dlen <= 2) break;
        }
    }

    dev->active = 1;

    kprintf("[I2C-HID] Device I2C%d:0x%02x initialized as %s "
            "(VID=%04x PID=%04x)\n",
            dev->ctrl->bus_id, dev->i2c_addr,
            dev->dev_type == I2C_HID_DEV_TOUCHPAD ? "touchpad" :
            dev->dev_type == I2C_HID_DEV_MOUSE ? "mouse" : "unknown",
            dev->desc.wVendorID, dev->desc.wProductID);

    return 0;
}

// ============================================================================
// Interrupt-Driven I2C Transfer
// ============================================================================

// ISR: called from irq_handler when an I2C MSI/IOAPIC interrupt fires
void i2c_hid_irq_handler(uint8_t vector) {
    // ---- Probe mode: just record that the vector fired and exit ----
    if (g_i2c_probe_mode) {
        g_i2c_probe_hit = 1;
        // Clear interrupt source on all active controllers so the
        // level-triggered line deasserts before we EOI.
        for (int c = 0; c < g_i2c_controller_count; c++) {
            i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
            if (ctrl->active && ctrl->base) {
                dw_write(ctrl, DW_IC_INTR_MASK, 0);
                (void)dw_read(ctrl, DW_IC_CLR_INTR);
            }
        }
        // EOI: use x2APIC MSR directly since lapic_init() hasn't run yet
        // and lapic_eoi() would use MMIO which is a no-op in x2APIC mode.
        {
            uint64_t ab = i2c_rdmsr(IA32_APIC_BASE_MSR);
            if ((ab >> 10) & 1)
                i2c_wrmsr(X2APIC_EOI_MSR, 0);
            else
                lapic_eoi();
        }
        return;
    }

    // Find which controller this vector belongs to
    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active)
            continue;
        if (ctrl->irq_vector != vector)
            continue;

        // Read and clear interrupt status
        uint32_t stat = dw_read(ctrl, DW_IC_INTR_STAT);
        g_dbg_i2c_isr_count++;

        // Spurious: DW output stuck HIGH with INTR_MASK=0 (LPSS quirk).
        // Mask the IOAPIC entry to stop the storm; the transfer function
        // will unmask before the next active transfer.
        if (stat == 0) {
            ioapic_mask_gsi((uint8_t)ctrl->irq_gsi);
            lapic_eoi();
            return;
        }

        if (stat & DW_IC_INTR_TX_ABRT) {
            ctrl->abort_source = dw_read(ctrl, DW_IC_TX_ABRT_SOURCE);
            (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
            ctrl->xfer_error = 1;
            ctrl->irq_pending = 1;
        }

        if (stat & DW_IC_INTR_RX_FULL) {
            ctrl->rx_ready = 1;
            ctrl->irq_pending = 1;
        }

        if (stat & DW_IC_INTR_TX_EMPTY) {
            ctrl->tx_complete = 1;
            ctrl->irq_pending = 1;
        }

        if (stat & DW_IC_INTR_STOP_DET) {
            (void)dw_read(ctrl, DW_IC_CLR_STOP_DET);
            ctrl->irq_pending = 1;
        }

        // Clear remaining interrupts
        (void)dw_read(ctrl, DW_IC_CLR_INTR);

        // Signal EOI
        lapic_eoi();
        return;
    }

    // Unknown vector — still EOI
    lapic_eoi();
}

// Interrupt-driven write-then-read transfer
// Uses interrupts to wait for TX/RX completion instead of busy-waiting
static int dw_i2c_xfer_irq(i2c_dw_controller_t *ctrl, uint16_t addr,
                            const uint8_t *wbuf, int wlen,
                            uint8_t *rbuf, int rlen)
{
    int timeout;
    int retry = 0;

    g_dbg_xfer_total++;

retry_xfer:
    dw_i2c_set_target(ctrl, addr);

    // Clear state flags
    ctrl->irq_pending = 0;
    ctrl->tx_complete = 0;
    ctrl->rx_ready = 0;
    ctrl->xfer_error = 0;
    ctrl->abort_source = 0;

    // Ensure bus is idle before starting a new transfer.
    // With set_target skip (no disable/enable), the previous transfer's
    // STOP may still be in-flight.  Wait for master to be inactive.
    dw_i2c_wait_idle(ctrl, 10000);

    // ---- Write phase ----
    // Push slave address (implicit) + register bytes.
    // Do NOT issue STOP here when a read phase follows — the bus must
    // stay active so the read phase can begin with a RESTART.
    // Only issue STOP on the last write byte for write-only transfers.
    for (int i = 0; i < wlen; i++) {
        uint32_t cmd = (uint32_t)wbuf[i];
        if (i == wlen - 1 && rlen == 0)
            cmd |= DW_IC_DATA_CMD_STOP;
        // No STOP when rlen > 0: RESTART will bridge write→read

        // Wait for TX FIFO space
        for (timeout = 0; timeout < 200000; timeout++) {
            if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_TFNF)
                break;
            if (ctrl->xfer_error) goto abort;
            i2c_delay_us(1);
        }
        if (timeout >= 200000) goto abort;

        dw_write(ctrl, DW_IC_DATA_CMD, cmd);
    }

    // ---- Read phase with interrupt-driven RX ----
    // RESTART replaces STOP between write and read — bus remains active.
    // The controller sends ACK for every byte except the last, where
    // STOP is issued (hardware sends NACK + STOP on the final byte).
    int rx_idx = 0;
    int tx_issued = 0;

    if (rlen > 0) {
        // Unmask IOAPIC entry for this transfer (masked when idle to
        // prevent ISR storm from Intel LPSS DW interrupt output quirk).
        if (ctrl->irq_gsi)
            ioapic_unmask_gsi((uint8_t)ctrl->irq_gsi);

        // Enable RX_FULL and TX_ABRT interrupts
        dw_write(ctrl, DW_IC_INTR_MASK,
                 DW_IC_INTR_RX_FULL | DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET);

        while (rx_idx < rlen) {
            // Issue read commands into the command queue.
            // First read gets RESTART (transitions from write to read
            // on the same slave address without releasing the bus).
            // Last read gets STOP (controller sends NACK then STOP).
            while (tx_issued < rlen) {
                uint32_t cmd = DW_IC_DATA_CMD_READ;
                if (tx_issued == 0 && wlen > 0)
                    cmd |= DW_IC_DATA_CMD_RESTART;
                if (tx_issued == rlen - 1)
                    cmd |= DW_IC_DATA_CMD_STOP;

                if (!(dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_TFNF))
                    break;

                dw_write(ctrl, DW_IC_DATA_CMD, cmd);
                tx_issued++;

                if (tx_issued - rx_idx >= (int)ctrl->tx_fifo_depth - 1)
                    break;
            }

            // Wait for RX data via interrupt or poll fallback
            ctrl->rx_ready = 0;
            for (timeout = 0; timeout < 200000; timeout++) {
                if (ctrl->xfer_error) {
                    dw_write(ctrl, DW_IC_INTR_MASK, 0);
                    goto abort;
                }
                // Check RX FIFO directly (in case interrupt was coalesced)
                if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
                    break;
                if (ctrl->rx_ready)
                    break;
                // Direct abort check — catches TX_ABRT even if MSI
                // delivery is delayed (no dependency on ISR)
                if (dw_read(ctrl, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT) {
                    dw_write(ctrl, DW_IC_INTR_MASK, 0);
                    goto abort;
                }
                i2c_delay_us(1);
            }
            if (timeout >= 200000) {
                dw_write(ctrl, DW_IC_INTR_MASK, 0);
                goto abort;
            }

            // Drain RX FIFO
            while (rx_idx < rlen &&
                   (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)) {
                uint32_t data = dw_read(ctrl, DW_IC_DATA_CMD);
                if (rbuf)
                    rbuf[rx_idx] = (uint8_t)(data & 0xFF);
                rx_idx++;
            }
        }

        // Disable interrupts after transfer
        dw_write(ctrl, DW_IC_INTR_MASK, 0);

        // Re-mask IOAPIC to prevent ISR storm while idle
        if (ctrl->irq_gsi)
            ioapic_mask_gsi((uint8_t)ctrl->irq_gsi);
    }

    // Wait for bus idle
    dw_i2c_wait_idle(ctrl, 10000);
    g_dbg_xfer_ok++;
    return 0;

abort:
    {
        // 1. Read abort reason (must read before clearing)
        uint32_t abort_src = dw_read(ctrl, DW_IC_TX_ABRT_SOURCE);
        // If the I2C ISR already read and cleared TX_ABRT_SOURCE (race
        // between ISR and this handler), use the ISR-saved value.
        if (abort_src == 0 && ctrl->xfer_error)
            abort_src = ctrl->abort_source;

        // 2. Disable all interrupts for this transfer
        dw_write(ctrl, DW_IC_INTR_MASK, 0);

        // 3. Disable controller — waits for IC_ENABLE_STATUS to confirm.
        //    This flushes the TX FIFO and triggers STOP on the bus.
        dw_write(ctrl, DW_IC_ENABLE, 0);
        for (int w = 0; w < 10000; w++) {
            if ((dw_read(ctrl, DW_IC_ENABLE_STATUS) & DW_IC_EN_STATUS_IC_EN) == 0)
                break;
            // Fallback: IC_ENABLE readback 0 means disabled
            if (dw_read(ctrl, DW_IC_ENABLE) == 0)
                break;
            i2c_delay_us(1);
        }

        // 4. Clear all pending interrupts
        (void)dw_read(ctrl, DW_IC_CLR_INTR);

        // 5. Clear abort flag explicitly
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);

        // 6. Flush RX FIFO — discard any stale data
        while (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
            (void)dw_read(ctrl, DW_IC_DATA_CMD);

        // 6b. Issue an explicit STOP on the bus.  Re-enable the controller
        //     briefly, push a dummy read command with STOP, then disable
        //     again.  This ensures the bus is released even if the abort
        //     did not generate an automatic STOP condition.
        dw_write(ctrl, DW_IC_ENABLE, 1);
        for (int w = 0; w < 5000; w++) {
            if (dw_read(ctrl, DW_IC_ENABLE_STATUS) & DW_IC_EN_STATUS_IC_EN)
                break;
            if (dw_read(ctrl, DW_IC_ENABLE) == 1)
                break;
            i2c_delay_us(1);
        }
        dw_write(ctrl, DW_IC_DATA_CMD, DW_IC_DATA_CMD_STOP);
        i2c_delay_us(200);  // Let STOP propagate on the bus
        dw_write(ctrl, DW_IC_ENABLE, 0);
        for (int w = 0; w < 10000; w++) {
            if ((dw_read(ctrl, DW_IC_ENABLE_STATUS) & DW_IC_EN_STATUS_IC_EN) == 0)
                break;
            if (dw_read(ctrl, DW_IC_ENABLE) == 0)
                break;
            i2c_delay_us(1);
        }
        // Clear any status from the STOP command
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);

        // 7. Re-init key registers (controller must be disabled to write these)
        dw_write(ctrl, DW_IC_TAR, addr & 0x7F);
        dw_write(ctrl, DW_IC_CON, DW_IC_CON_MASTER | DW_IC_CON_SPEED_SS |
                                  DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE);

        // 8. Re-enable controller
        dw_write(ctrl, DW_IC_ENABLE, 1);
        for (int w = 0; w < 10000; w++) {
            if (dw_read(ctrl, DW_IC_ENABLE_STATUS) & DW_IC_EN_STATUS_IC_EN)
                break;
            if (dw_read(ctrl, DW_IC_ENABLE) == 1)
                break;
            i2c_delay_us(1);
        }

        // 9. Save error state for caller diagnostics
        ctrl->current_target = addr;  // TAR was just re-programmed
        ctrl->abort_source = abort_src;
        ctrl->xfer_error = 0;
        ctrl->irq_pending = 0;
        ctrl->tx_complete = 0;
        ctrl->rx_ready = 0;

        g_dbg_xfer_abort_count++;

        kprintf("[I2C-XFER] ABORT bus=%u addr=0x%02x abort_src=0x%08x "
                "rx=%d/%d tx=%d/%d retry=%d "
                "isr=%u gpio_isr=%u hit=%u miss=%u\n",
                ctrl->bus_id, addr, abort_src,
                rx_idx, rlen, tx_issued, rlen, retry,
                g_dbg_i2c_isr_count, g_dbg_gpio_isr_count,
                g_dbg_gpio_isr_hit, g_dbg_gpio_isr_miss);

        // Re-mask IOAPIC to prevent ISR storm while idle
        if (ctrl->irq_gsi)
            ioapic_mask_gsi((uint8_t)ctrl->irq_gsi);

        // 10. Retry once on NACK, ARB_LOST, or pure timeout (abort_src==0
        //     means the hardware didn't report an error but we timed out
        //     waiting for RX data — transient, worth one retry).
        if (retry < 1) {
            retry++;
            g_dbg_xfer_retry_count++;
            goto retry_xfer;
        }
        return -1;
    }
}

// ============================================================================
// GPIO Interrupt Support (like Linux pinctrl-intel + i2c-hid-acpi)
// ============================================================================

// Read P2SB SBREG_BAR: unhide P2SB D31:F1, read BAR, re-hide.
// Returns physical base address of P2SB sideband register window, or 0.
static uint64_t p2sb_get_sbreg_bar(void)
{
    uint64_t bar = 0;

    // Unhide P2SB via ECAM (preferred) and CF8
    if (g_ecam_bus0_va) {
        ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                     P2SB_P2SBC, 0);
        i2c_delay_us(1000);

        uint32_t vid = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                   P2SB_PCI_FUNC, 0x00);
        if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
            // Ensure MSE is enabled
            uint32_t cmd = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                       P2SB_PCI_FUNC, 0x04);
            if (!(cmd & 0x02))
                ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                             0x04, cmd | 0x02);

            uint32_t bar_lo = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                          P2SB_PCI_FUNC, P2SB_SBREG_BAR);
            uint32_t bar_hi = ecam_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                          P2SB_PCI_FUNC, P2SB_SBREG_BARH);
            bar = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFULL);
        }

        // Re-hide P2SB
        ecam_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                     P2SB_P2SBC, P2SB_HIDE_BIT);
    }

    // CF8/CFC fallback
    if (!bar) {
        pci_cfg_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                        P2SB_P2SBC, 0);
        i2c_delay_us(1000);

        uint32_t vid = pci_cfg_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                      P2SB_PCI_FUNC, 0x00);
        if (vid != 0xFFFFFFFF && (vid & 0xFFFF) == 0x8086) {
            uint32_t cmd = pci_cfg_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                          P2SB_PCI_FUNC, 0x04);
            if (!(cmd & 0x02))
                pci_cfg_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                                0x04, cmd | 0x02);

            uint32_t bar_lo = pci_cfg_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                             P2SB_PCI_FUNC, P2SB_SBREG_BAR);
            uint32_t bar_hi = pci_cfg_read32(P2SB_PCI_BUS, P2SB_PCI_DEV,
                                             P2SB_PCI_FUNC, P2SB_SBREG_BARH);
            bar = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFULL);
        }

        pci_cfg_write32(P2SB_PCI_BUS, P2SB_PCI_DEV, P2SB_PCI_FUNC,
                        P2SB_P2SBC, P2SB_HIDE_BIT);
    }

    return bar;
}

// Read a 32-bit register from a GPIO community's MMIO space
static inline uint32_t gpio_comm_read32(gpio_community_t *comm, uint32_t offset)
{
    volatile uint32_t *addr = (volatile uint32_t *)
        ((volatile uint8_t *)comm->base + offset);
    return *addr;
}

// Write a 32-bit register in a GPIO community's MMIO space
static inline void gpio_comm_write32(gpio_community_t *comm, uint32_t offset,
                                     uint32_t value)
{
    volatile uint32_t *addr = (volatile uint32_t *)
        ((volatile uint8_t *)comm->base + offset);
    *addr = value;
}

// Detect GPIO platform by checking which known ACPI _HID matches the GPIO
// controller. Uses acpi_aml_find_devices_by_hid to search for each platform's
// HIDs. There is only one GPIO controller per Intel PCH, so finding any device
// with a matching _HID is sufficient.
// Returns pointer to matching platform table, or NULL if unknown.
static const gpio_platform_def_t *gpio_detect_platform(const char *gpio_path)
{
    (void)gpio_path;

    for (unsigned i = 0; i < GPIO_NUM_PLATFORMS; i++) {
        for (int h = 0; g_gpio_platforms[i].acpi_hids[h]; h++) {
            acpi_aml_device_info_t devs[1];
            int ndev = acpi_aml_find_devices_by_hid(
                g_gpio_platforms[i].acpi_hids[h], devs, 1);
            if (ndev > 0) {
                kprintf("[GPIO] Detected platform via _HID %s (path=%s)\n",
                        g_gpio_platforms[i].acpi_hids[h], devs[0].path);
                return &g_gpio_platforms[i];
            }
        }
    }

    kprintf("[GPIO] No matching platform table for GPIO controller\n");
    return NULL;
}

// Resolve an ACPI GpioInt pin number to hardware location using platform tables.
// Returns 0 on success, -1 if pin not found.
// Sets *out_community_idx, *out_pad_offset (pad index in community),
// *out_gpi_reg (GPI_IS register index), *out_gpi_bit (bit within register).
static int gpio_resolve_acpi_pin(uint16_t acpi_pin,
                                 int *out_community_idx,
                                 uint16_t *out_pad_index,
                                 uint8_t *out_gpi_reg,
                                 uint8_t *out_gpi_bit)
{
    if (!g_gpio_platform) return -1;

    for (int ci = 0; ci < g_gpio_platform->ncommunities; ci++) {
        const gpio_community_def_t *cdef = &g_gpio_platform->communities[ci];
        uint16_t pad_offset_in_comm = 0;

        for (int gi = 0; gi < cdef->ngpps; gi++) {
            const gpio_padgroup_def_t *grp = &cdef->gpps[gi];

            if (grp->gpio_base != GPIO_NOMAP &&
                acpi_pin >= (uint16_t)grp->gpio_base &&
                acpi_pin < (uint16_t)grp->gpio_base + grp->size) {
                uint8_t offset_in_group = (uint8_t)(acpi_pin - grp->gpio_base);
                *out_community_idx = ci;
                *out_pad_index = pad_offset_in_comm + offset_in_group;
                *out_gpi_reg = grp->reg_num;
                *out_gpi_bit = offset_in_group;
                return 0;
            }
            pad_offset_in_comm += grp->size;
        }
    }
    return -1;
}

// Discover GPIO communities from the ACPI GPIO controller's _CRS.
// Each MMIO range in _CRS = one GPIO community.
// Uses platform tables for group structure if platform is known.
// Returns number of communities found.
static int gpio_discover_communities(const char *gpio_ctrl_path)
{
    acpi_crs_result_t crs;
    i2c_memset(&crs, 0, sizeof(crs));

    if (acpi_aml_eval_crs(gpio_ctrl_path, &crs) != 0) {
        kprintf("[GPIO] Failed to evaluate _CRS on %s\n", gpio_ctrl_path);
        return 0;
    }

    kprintf("[GPIO] %s: %d MMIO ranges, %d IRQs\n",
            gpio_ctrl_path, crs.mmio_range_count, crs.irq_count);

    // Detect platform from ACPI _HID
    g_gpio_platform = gpio_detect_platform(gpio_ctrl_path);

    // The IRQ resource gives us the IOAPIC GSI for this GPIO controller
    uint32_t gpio_gsi = 0;
    if (crs.irq_count > 0) {
        gpio_gsi = crs.irqs[0];
        g_gpio_ioapic_polarity = crs.irq_polarity;
        g_gpio_ioapic_trigger  = crs.irq_triggering;
        kprintf("[GPIO] IOAPIC GSI from _CRS: %u (trigger=%s, pol=%s)\n",
                gpio_gsi,
                crs.irq_triggering ? "edge" : "level",
                crs.irq_polarity ? "low" : "high");
    }

    int count = 0;

    for (int i = 0; i < crs.mmio_range_count && count < GPIO_MAX_COMMUNITIES; i++) {
        uint64_t phys = crs.mmio_ranges[i].base;
        uint32_t len = crs.mmio_ranges[i].length;

        if (phys == 0 || len == 0) continue;

        // Map enough pages to cover the full community MMIO region
        uint32_t npages = (len + 0xFFF) >> 12;
        if (npages == 0) npages = 1;
        uint64_t va = mm_map_device_mmio(phys, npages);
        if (!va) {
            kprintf("[GPIO] Failed to map community %d at 0x%llx\n",
                    i, (unsigned long long)phys);
            continue;
        }

        gpio_community_t *comm = &g_gpio_communities[count];
        i2c_memset(comm, 0, sizeof(*comm));
        comm->base = (volatile uint32_t *)va;
        comm->phys = phys;

        // Read MISCCFG to get GPDMINTSEL (IOAPIC IRQ line for this community)
        uint32_t misccfg = gpio_comm_read32(comm, GPIO_MISCCFG);
        comm->irq_line = (misccfg & GPIO_MISCCFG_GPDMINTSEL_MASK)
                         >> GPIO_MISCCFG_GPDMINTSEL_SHIFT;

        // Override IRQ line from _CRS if available (more reliable)
        if (gpio_gsi)
            comm->irq_line = (uint8_t)gpio_gsi;

        // Read PADBAR to find where pad configs start
        uint32_t padbar = gpio_comm_read32(comm, GPIO_PADBAR) & 0xFFFF;

        // Populate group structure from platform table if available
        if (g_gpio_platform && count < g_gpio_platform->ncommunities) {
            const gpio_community_def_t *cdef = &g_gpio_platform->communities[count];
            comm->pin_base = cdef->pin_base;
            comm->pin_count = cdef->npins;
            comm->num_groups = cdef->ngpps;

            uint16_t pad_offset = 0;
            for (int g = 0; g < cdef->ngpps && g < GPIO_MAX_GROUPS_PER_COMM; g++) {
                comm->groups[g].pad_cfg_offset = padbar + pad_offset * GPIO_PAD_STRIDE;
                comm->groups[g].pad_count = cdef->gpps[g].size;
                comm->groups[g].gpi_reg_index = cdef->gpps[g].reg_num;
                comm->groups[g].gpio_base = cdef->gpps[g].gpio_base;
                pad_offset += cdef->gpps[g].size;
            }
        } else {
            // Unknown platform: scan hardware to estimate pad count
            uint16_t scan_limit = 256;
            if (padbar + scan_limit * GPIO_PAD_STRIDE > len)
                scan_limit = (uint16_t)((len - padbar) / GPIO_PAD_STRIDE);

            uint16_t npads = 0;
            for (uint16_t p = 0; p < scan_limit; p++) {
                uint32_t off = padbar + p * GPIO_PAD_STRIDE;
                uint32_t dw0 = gpio_comm_read32(comm, off + GPIO_PAD_CFG_DW0);
                uint32_t dw1 = gpio_comm_read32(comm, off + GPIO_PAD_CFG_DW1);
                if (dw0 != 0 || dw1 != 0)
                    npads = p + 1;
            }

            comm->pin_base = 0; // Unknown — cumulative not meaningful
            comm->pin_count = npads;
            comm->num_groups = 1;
            comm->groups[0].pad_cfg_offset = padbar;
            comm->groups[0].pad_count = (uint8_t)(npads > 255 ? 255 : npads);
            comm->groups[0].gpi_reg_index = 0;
            comm->groups[0].gpio_base = GPIO_NOMAP;
        }

        kprintf("[GPIO] Community %d: phys=0x%llx MISCCFG=0x%08x "
                "GPDMINTSEL=%u PADBAR=0x%x pins=%u-%u (%u pads, %u groups)%s\n",
                count, (unsigned long long)phys, misccfg,
                comm->irq_line, padbar,
                comm->pin_base,
                comm->pin_count > 0 ? comm->pin_base + comm->pin_count - 1 : comm->pin_base,
                comm->pin_count, comm->num_groups,
                g_gpio_platform ? " [table]" : " [scan]");

        count++;
    }

    g_gpio_community_count = count;
    return count;
}

// Find which community contains the given ACPI GpioInt pin number.
// Uses platform gpio_base tables to resolve the pin to a physical pad.
//
// Returns community index (into g_gpio_communities), or -1 if not found.
// Also sets *out_pad_offset to the PAD_CFG MMIO offset for the pin,
// *out_gpi_group to the GPI register index, *out_gpi_bit to the bit position.
static int gpio_find_pin_community(uint16_t pin, uint32_t *out_pad_offset,
                                   uint8_t *out_gpi_group, uint8_t *out_gpi_bit)
{
    int ci_idx = -1;
    uint16_t pad_index = 0;
    uint8_t gpi_reg = 0, gpi_bit = 0;

    if (g_gpio_platform) {
        // Use platform table to resolve ACPI pin → community/pad
        if (gpio_resolve_acpi_pin(pin, &ci_idx, &pad_index,
                                  &gpi_reg, &gpi_bit) != 0) {
            kprintf("[GPIO] Pin %u not found in platform table\n", pin);
            return -1;
        }

        if (ci_idx >= g_gpio_community_count) {
            kprintf("[GPIO] Pin %u maps to community %d but only %d mapped\n",
                    pin, ci_idx, g_gpio_community_count);
            return -1;
        }

        gpio_community_t *comm = &g_gpio_communities[ci_idx];
        uint32_t padbar = gpio_comm_read32(comm, GPIO_PADBAR) & 0xFFFF;
        *out_pad_offset = padbar + pad_index * GPIO_PAD_STRIDE;
        *out_gpi_group = gpi_reg;
        *out_gpi_bit = gpi_bit;

        kprintf("[GPIO] Pin %u: community %d pad_idx %u "
                "offset=0x%x gpi_reg=%u bit=%u (gpio_base table)\n",
                pin, ci_idx, pad_index, *out_pad_offset,
                gpi_reg, gpi_bit);
        return ci_idx;
    }

    // Fallback for unknown platforms: try sequential pin ranges
    for (int ci = 0; ci < g_gpio_community_count; ci++) {
        gpio_community_t *comm = &g_gpio_communities[ci];

        if (pin < comm->pin_base || pin >= comm->pin_base + comm->pin_count)
            continue;

        uint16_t pad_idx = pin - comm->pin_base;
        uint32_t padbar = gpio_comm_read32(comm, GPIO_PADBAR) & 0xFFFF;
        *out_pad_offset = padbar + pad_idx * GPIO_PAD_STRIDE;
        *out_gpi_group = (uint8_t)(pad_idx / 32);
        *out_gpi_bit = (uint8_t)(pad_idx % 32);

        kprintf("[GPIO] Pin %u: community %d pad %u "
                "offset=0x%x group=%u bit=%u (fallback)\n",
                pin, ci, pad_idx, *out_pad_offset,
                *out_gpi_group, *out_gpi_bit);
        return ci;
    }

    kprintf("[GPIO] Pin %u not found in any community\n", pin);
    return -1;
}

// Configure a GPIO pad for interrupt delivery via IOAPIC (GPIO Driver Mode).
// Like Linux intel_gpio_irq_enable() + intel_gpio_set_gpio_mode().
static int gpio_configure_pad_interrupt(gpio_community_t *comm,
                                        uint32_t pad_offset,
                                        uint8_t gpi_group,
                                        uint8_t gpi_bit,
                                        uint8_t triggering,  // 0=level, 1=edge
                                        uint8_t polarity)    // 0=high, 1=low, 2=both
{
    // 1. Set HOSTSW_OWN = 1 (GPIO Driver Mode) for this pad
    //    This makes GPI_IS (instead of GPI_GPE_STS) track interrupt status
    uint16_t hostsw_base = g_gpio_platform ? g_gpio_platform->hostsw_own_offset : 0x0b0;
    uint32_t hostsw_reg = hostsw_base + gpi_group * 4;
    uint32_t hostsw = gpio_comm_read32(comm, hostsw_reg);
    hostsw |= (1u << gpi_bit);
    gpio_comm_write32(comm, hostsw_reg, hostsw);
    kprintf("[GPIO] HOSTSW_OWN[%u] = 0x%08x (set bit %u)\n",
            gpi_group, gpio_comm_read32(comm, hostsw_reg), gpi_bit);

    // 2. Configure PAD_CFG_DW0:
    //    - Set pad mode to GPIO (PMode=0) — should already be set by BIOS
    //    - RX buffer enabled (GPIORXDIS=0) — ensure input is active
    //    - Set RXEVCFG for edge or level trigger
    //    - Set RXINV if active-low
    //    - Clear all routing bits, then set GPIROUTIOXAPIC=1
    //      (Actually, for GPIO Driver Mode, we DON'T set GPIROUTIOXAPIC.
    //       The interrupt goes through GPI_IS/GPI_IE → GPDMINTSEL → IOAPIC.
    //       GPIROUTIOXAPIC is for direct per-pad IOAPIC routing which is a
    //       different mechanism. Linux does NOT use GPIROUTIOXAPIC.)
    uint32_t dw0 = gpio_comm_read32(comm, pad_offset + GPIO_PAD_CFG_DW0);
    kprintf("[GPIO] PAD_CFG_DW0 before: 0x%08x\n", dw0);

    // Clear routing bits (we use GPIO Driver Mode, not direct IOAPIC routing)
    dw0 &= ~(GPIO_DW0_GPIROUTIOXAPIC | GPIO_DW0_GPIROUTSCI |
              GPIO_DW0_GPIROUTSMI | GPIO_DW0_GPIROUTNMI);

    // Ensure GPIO mode (PMode = 0)
    dw0 &= ~GPIO_DW0_PMODE_MASK;

    // Enable RX (clear RX disable)
    dw0 &= ~GPIO_DW0_GPIORXDIS;

    // Set RXEVCFG to LEVEL mode (like Linux IRQF_TRIGGER_LOW).
    //
    // EDGE mode is WRONG for HID-over-I2C: with RXINV=1, edge detection
    // fires on BOTH the falling edge (INT# assert = device has data) AND
    // the rising edge (INT# deassert = device done).  The deassert-edge
    // triggers a read when the device has NO data, causing timeouts
    // (abort_src=0x0).
    //
    // LEVEL mode fires GPI_IS as long as INT# is LOW (data ready).  Once
    // the host reads the report, INT# goes HIGH and IS auto-clears.  The
    // GPIO ISR masks IE before EOI, preventing re-delivery flood.
    // gpio_reenable_ie() re-enables IE after the read completes.
    dw0 &= ~GPIO_DW0_RXEVCFG_MASK;
    dw0 |= GPIO_DW0_RXEVCFG_LEVEL;

    // Always set RXINV for active-low (HID INT# is active-low).
    // With RXEVCFG_LEVEL + RXINV, IS is set whenever the physical
    // pin is LOW = INT# asserted = device has data.
    dw0 |= GPIO_DW0_RXINV;

    gpio_comm_write32(comm, pad_offset + GPIO_PAD_CFG_DW0, dw0);
    kprintf("[GPIO] PAD_CFG_DW0 after:  0x%08x\n",
            gpio_comm_read32(comm, pad_offset + GPIO_PAD_CFG_DW0));

    // 3. Clear any pending interrupt status
    uint16_t is_base = g_gpio_platform ? g_gpio_platform->gpi_is_offset : 0x100;
    uint32_t is_reg = is_base + gpi_group * 4;
    gpio_comm_write32(comm, is_reg, (1u << gpi_bit));

    // 4. Enable interrupt in GPI_IE
    uint16_t ie_base = g_gpio_platform ? g_gpio_platform->gpi_ie_offset : 0x120;
    uint32_t ie_reg = ie_base + gpi_group * 4;
    uint32_t ie = gpio_comm_read32(comm, ie_reg);
    ie |= (1u << gpi_bit);
    gpio_comm_write32(comm, ie_reg, ie);
    kprintf("[GPIO] GPI_IE[%u] = 0x%08x (set bit %u)\n",
            gpi_group, gpio_comm_read32(comm, ie_reg), gpi_bit);

    return 0;
}

// Set up GPIO interrupt for an I2C HID device.
// Called after parsing the device's ACPI _CRS GpioInt resource.
// Returns 0 on success, -1 on failure.
static int gpio_setup_hid_interrupt(i2c_hid_device_t *dev,
                                    const acpi_crs_result_t *crs)
{
    if (!crs->gpio_int.valid) return -1;

    uint16_t pin = crs->gpio_int.pin;
    const char *gpio_path = crs->gpio_int.resource_source;

    kprintf("[GPIO] Setting up interrupt: pin=%u trigger=%s polarity=%s "
            "source=%s\n",
            pin,
            crs->gpio_int.triggering ? "edge" : "level",
            crs->gpio_int.polarity == 0 ? "high" :
            crs->gpio_int.polarity == 1 ? "low" : "both",
            gpio_path);

    // Step 1: Discover GPIO communities from the controller's ACPI _CRS
    if (g_gpio_community_count == 0 && gpio_path[0]) {
        gpio_discover_communities(gpio_path);
    }

    if (g_gpio_community_count == 0) {
        // Fallback: try P2SB SBREG_BAR + known PortIDs
        kprintf("[GPIO] No communities from ACPI, trying P2SB...\n");
        g_sbreg_bar = p2sb_get_sbreg_bar();
        if (g_sbreg_bar) {
            kprintf("[GPIO] P2SB SBREG_BAR = 0x%llx\n",
                    (unsigned long long)g_sbreg_bar);
            // Try known GPIO PortIDs for Intel PCH 600/700
            // PortIDs: 0xD1-0xD6 are typical for GPIO communities
            static const uint8_t port_ids[] = {
                0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6
            };
            for (int i = 0; i < 6 && g_gpio_community_count < GPIO_MAX_COMMUNITIES; i++) {
                uint64_t phys = g_sbreg_bar + ((uint64_t)port_ids[i] << 16);
                uint64_t va = mm_map_device_mmio(phys, 1);
                if (!va) continue;

                gpio_community_t *comm = &g_gpio_communities[g_gpio_community_count];
                i2c_memset(comm, 0, sizeof(*comm));
                comm->base = (volatile uint32_t *)va;
                comm->phys = phys;

                uint32_t misccfg = gpio_comm_read32(comm, GPIO_MISCCFG);
                uint32_t padbar = gpio_comm_read32(comm, GPIO_PADBAR) & 0xFFFF;

                // Validate: PADBAR should be a reasonable value (0x400-0xC00)
                if (padbar < 0x400 || padbar > 0xC00) continue;

                comm->irq_line = (misccfg & GPIO_MISCCFG_GPDMINTSEL_MASK)
                                 >> GPIO_MISCCFG_GPDMINTSEL_SHIFT;

                kprintf("[GPIO] P2SB PortID 0x%02x: MISCCFG=0x%08x "
                        "GPDMINTSEL=%u PADBAR=0x%x\n",
                        port_ids[i], misccfg, comm->irq_line, padbar);

                g_gpio_community_count++;
            }
        }
    }

    if (g_gpio_community_count == 0) {
        kprintf("[GPIO] No GPIO communities found\n");
        return -1;
    }

    // Step 2: Find which community contains our pin
    uint32_t pad_offset = 0;
    uint8_t gpi_group = 0, gpi_bit = 0;
    int ci = gpio_find_pin_community(pin, &pad_offset, &gpi_group, &gpi_bit);
    if (ci < 0) return -1;

    gpio_community_t *comm = &g_gpio_communities[ci];

    // Step 3: Configure the pad for interrupt delivery
    gpio_configure_pad_interrupt(comm, pad_offset, gpi_group, gpi_bit,
                                 crs->gpio_int.triggering,
                                 crs->gpio_int.polarity);

    // Step 4: Route IOAPIC GSI → IDT vector
    uint8_t gsi = comm->irq_line;
    uint8_t vector = GPIO_IRQ_VECTOR_BASE;  // All GPIO communities share one vector

    // Use the GPIO controller's _CRS IRQ polarity and trigger for the IOAPIC.
    // This describes the GPIO community's interrupt output to the IOAPIC,
    // NOT the individual pad's polarity.  Typically active-low, level-triggered.
    uint8_t ioapic_polarity = g_gpio_ioapic_polarity ?
                              IOAPIC_POLARITY_LOW : IOAPIC_POLARITY_HIGH;
    uint8_t ioapic_trigger  = g_gpio_ioapic_trigger ?
                              IOAPIC_TRIGGER_EDGE : IOAPIC_TRIGGER_LEVEL;

    kprintf("[GPIO] Routing IOAPIC GSI %u -> vector %u\n", gsi, vector);
    if (ioapic_configure_legacy_irq(gsi, vector, ioapic_polarity,
                                    ioapic_trigger) != 0) {
        kprintf("[GPIO] IOAPIC routing failed for GSI %u\n", gsi);
        return -1;
    }

    // Step 5: Store GPIO state in the HID device
    dev->gpio_pin = pin;
    dev->gpio_community = (uint8_t)ci;
    dev->gpio_gpi_group = gpi_group;
    dev->gpio_gpi_bit = gpi_bit;
    dev->gpio_irq_vector = vector;
    dev->gpio_irq_active = 1;
    dev->gpio_irq_pending = 0;
    dev->gpio_pad_offset = pad_offset;
    // Save the expected PAD_CFG_DW0 (critical bits) for runtime verification.
    // If BIOS/SMI changes these, we can detect and repair.
    dev->gpio_pad_dw0 = gpio_comm_read32(comm, pad_offset + GPIO_PAD_CFG_DW0);

    // Step 6: Clear any pending GPI_IS from init
    {
        uint16_t is_base = g_gpio_platform ? g_gpio_platform->gpi_is_offset : 0x100;
        uint32_t is_reg = is_base + gpi_group * 4;
        gpio_comm_write32(comm, is_reg, (1u << gpi_bit));
    }

    kprintf("[GPIO] Interrupt configured: pin=%u community=%d "
            "GSI=%u vector=%u\n", pin, ci, gsi, vector);
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

int i2c_hid_init(void) {
    kprintf("[I2C-HID] Initializing I2C HID subsystem\n");
#ifdef BUILD_DATE
    kprintf("[I2C-HID] Build: %s\n", BUILD_DATE);
#endif

    i2c_memset(g_i2c_controllers, 0, sizeof(g_i2c_controllers));
    i2c_memset(g_i2c_hid_devices, 0, sizeof(g_i2c_hid_devices));
    g_i2c_controller_count = 0;
    g_i2c_hid_device_count = 0;

    if (!has_intel_lpss_i2c_controller()) {
        kprintf("[I2C-HID] No Intel LPSS I2C PCI controllers present\n");
        return 0;
    }

    // Detect and initialize I2C controllers
    int nctrl = detect_i2c_controllers();

    if (nctrl == 0) {
        kprintf("[I2C-HID] No Intel LPSS I2C controllers initialized\n");
        return -1;  // Hardware present but init failed — caller can debug halt
    }
    kprintf("[I2C-HID] Found %d I2C controller(s)\n", nctrl);

    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active) continue;

        int found_via_acpi = 0;

        // Discover I2C HID devices via ACPI: find PNP0C50/ACPI0C50 child
        // devices whose ACPI path starts with this controller's path, then
        // evaluate _CRS on the child to get the I2C slave address + speed.
        if (ctrl->acpi_path[0]) {
            static const char *const hid_ids[] = { "PNP0C50", "ACPI0C50" };
            size_t ctrl_path_len = kstrlen(ctrl->acpi_path);

            for (int hi = 0; hi < 2 && !found_via_acpi; hi++) {
                acpi_aml_device_info_t devs[4];
                int ndev = acpi_aml_find_devices_by_hid(hid_ids[hi], devs, 4);
                for (int di = 0; di < ndev; di++) {
                    // Check if this HID device is a child of our controller
                    if (kstrncmp(devs[di].path, ctrl->acpi_path, ctrl_path_len) != 0)
                        continue;
                    // Must be a direct child: next char after prefix is '.'
                    if (devs[di].path[ctrl_path_len] != '.')
                        continue;

                    kprintf("[I2C%d] ACPI child: %s (HID=%s)\n",
                            ctrl->bus_id, devs[di].path, hid_ids[hi]);

                    acpi_crs_result_t crs;
                    if (acpi_aml_eval_crs(devs[di].path, &crs) == 0) {
                        for (uint8_t ci = 0; ci < crs.i2c_device_count; ci++) {
                            uint16_t addr = crs.i2c_devices[ci].slave_addr;
                            if (addr >= 0x08 && addr <= 0x77) {
                                kprintf("[I2C%d] _CRS: slave 0x%02x speed=%u\n",
                                        ctrl->bus_id, addr,
                                        crs.i2c_devices[ci].connection_speed);
                                int dev_idx = g_i2c_hid_device_count;
                                i2c_hid_probe_device(ctrl, addr);
                                if (dev_idx < g_i2c_hid_device_count) {
                                    // Store ACPI path for power management
                                    int plen = 0;
                                    while (devs[di].path[plen] && plen < 63) plen++;
                                    i2c_memcpy(g_i2c_hid_devices[dev_idx].acpi_path,
                                               devs[di].path, plen);
                                    g_i2c_hid_devices[dev_idx].acpi_path[plen] = '\0';
                                }
                                // Log and save GpioInt info for later interrupt setup
                                if (crs.gpio_int.valid &&
                                    dev_idx < g_i2c_hid_device_count) {
                                    kprintf("[I2C%d] _CRS GpioInt: pin=%u "
                                            "trigger=%s pol=%s src=%s\n",
                                            ctrl->bus_id,
                                            crs.gpio_int.pin,
                                            crs.gpio_int.triggering ? "edge" : "level",
                                            crs.gpio_int.polarity == 0 ? "high" :
                                            crs.gpio_int.polarity == 1 ? "low" : "both",
                                            crs.gpio_int.resource_source);
                                    g_i2c_hid_devices[dev_idx].gpio_pin =
                                        crs.gpio_int.pin;
                                }
                                found_via_acpi++;
                            }
                        }
                    }
                }
            }

            // Fallback: walk direct children of controller ACPI node.
            // Some devices (e.g. ITE VEN_0488) use vendor _HID with _CID=PNP0C50
            // but AcpiGetDevices() may not match _CID on all ACPICA builds.
            if (!found_via_acpi) {
                acpi_aml_device_info_t children[4];
                int nch = acpi_aml_find_children(ctrl->acpi_path, children, 4);
                for (int di = 0; di < nch; di++) {
                    // Only consider children that have _CRS
                    if (!children[di].has_crs) continue;

                    acpi_crs_result_t crs;
                    if (acpi_aml_eval_crs(children[di].path, &crs) != 0)
                        continue;
                    if (crs.i2c_device_count == 0) continue;

                    kprintf("[I2C%d] ACPI child: %s (HID=%s CID=%s)\n",
                            ctrl->bus_id, children[di].path,
                            children[di].hid, children[di].cid);

                    for (uint8_t ci = 0; ci < crs.i2c_device_count; ci++) {
                        uint16_t addr = crs.i2c_devices[ci].slave_addr;
                        if (addr >= 0x08 && addr <= 0x77) {
                            kprintf("[I2C%d] _CRS: slave 0x%02x speed=%u\n",
                                    ctrl->bus_id, addr,
                                    crs.i2c_devices[ci].connection_speed);
                            int dev_idx = g_i2c_hid_device_count;
                            i2c_hid_probe_device(ctrl, addr);
                            if (dev_idx < g_i2c_hid_device_count) {
                                // Store ACPI path for power management
                                int plen = 0;
                                while (children[di].path[plen] && plen < 63) plen++;
                                i2c_memcpy(g_i2c_hid_devices[dev_idx].acpi_path,
                                           children[di].path, plen);
                                g_i2c_hid_devices[dev_idx].acpi_path[plen] = '\0';
                            }
                            if (crs.gpio_int.valid &&
                                dev_idx < g_i2c_hid_device_count) {
                                kprintf("[I2C%d] _CRS GpioInt: pin=%u "
                                        "trigger=%s pol=%s src=%s\n",
                                        ctrl->bus_id,
                                        crs.gpio_int.pin,
                                        crs.gpio_int.triggering ? "edge" : "level",
                                        crs.gpio_int.polarity == 0 ? "high" :
                                        crs.gpio_int.polarity == 1 ? "low" : "both",
                                        crs.gpio_int.resource_source);
                                g_i2c_hid_devices[dev_idx].gpio_pin =
                                    crs.gpio_int.pin;
                            }
                            found_via_acpi++;
                        }
                    }
                }
            }
        }

        // Fall back to full bus scan if ACPI didn't find devices
        if (!found_via_acpi) {
            dw_i2c_scan_bus(ctrl);
            for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
                if (dw_i2c_probe_addr(ctrl, addr) != 0)
                    continue;
                i2c_hid_probe_device(ctrl, addr);
            }
        }
    }

    // Initialize all discovered HID devices
    for (int d = 0; d < g_i2c_hid_device_count; d++) {
        i2c_hid_device_t *dev = &g_i2c_hid_devices[d];

        // ACPI power management: call _PS0 and resolve _DEP dependencies
        // This is critical — without it, the device firmware may not fully
        // initialize and will return empty input reports.
        if (dev->acpi_path[0]) {
            kprintf("[I2C-HID] ACPI power-on: %s\n", dev->acpi_path);

            // Power on device and all _DEP dependencies (GPIO, I2C host, etc.)
            int pw_rc = acpi_power_on_device_with_deps(dev->acpi_path);
            kprintf("[I2C-HID] acpi_power_on_device_with_deps: rc=%d\n", pw_rc);

            // Also try direct _PS0 on the device itself
            uint64_t ps0_ret = 0;
            int ps0_rc = acpi_aml_exec_device_method(dev->acpi_path, "_PS0",
                                                      &ps0_ret);
            kprintf("[I2C-HID] _PS0: rc=%d ret=%llu\n", ps0_rc,
                    (unsigned long long)ps0_ret);

            // Call _DSM function 1 (HID descriptor register address)
            // Microsoft HID-over-I2C _DSM: 3cdff6f7-4267-4555-ad05-b30a3d8938de
            // This may have firmware-level side effects that enable the device.
            static const uint8_t hid_i2c_dsm_uuid[16] = {
                0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
                0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde
            };
            aml_value_t dsm_result;
            i2c_memset(&dsm_result, 0, sizeof(dsm_result));
            int dsm_rc = acpi_aml_call_dsm(dev->acpi_path, hid_i2c_dsm_uuid,
                                            1, 1, &dsm_result);
            if (dsm_rc == 0) {
                uint64_t dsm_val = aml_value_to_integer(&dsm_result);
                kprintf("[I2C-HID] _DSM(1): rc=%d val=0x%llx\n", dsm_rc,
                        (unsigned long long)dsm_val);
            } else {
                kprintf("[I2C-HID] _DSM(1): rc=%d (no _DSM)\n", dsm_rc);
            }

            i2c_delay_us(10000);  // 10ms settle after ACPI power-on
        }

        i2c_hid_init_device(dev);
    }

    // Set up GPIO interrupts for active HID devices that have GpioInt resources.
    // This must happen AFTER device init (SET_POWER + RESET) so the device is
    // ready to signal on the GPIO line.
    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active || !ctrl->acpi_path[0]) continue;

        // Walk direct children of the controller — works regardless of _HID
        acpi_aml_device_info_t children[4];
        int nch = acpi_aml_find_children(ctrl->acpi_path, children, 4);
        for (int di = 0; di < nch; di++) {
            if (!children[di].has_crs) continue;

            acpi_crs_result_t crs;
            i2c_memset(&crs, 0, sizeof(crs));
            if (acpi_aml_eval_crs(children[di].path, &crs) != 0)
                continue;
            if (!crs.gpio_int.valid || crs.i2c_device_count == 0)
                continue;

            // Find the HID device matching this I2C address
            for (uint8_t ci2 = 0; ci2 < crs.i2c_device_count; ci2++) {
                uint16_t addr = crs.i2c_devices[ci2].slave_addr;
                for (int d = 0; d < g_i2c_hid_device_count; d++) {
                    i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
                    if (dev->ctrl == ctrl && dev->i2c_addr == addr &&
                        dev->active) {
                        gpio_setup_hid_interrupt(dev, &crs);
                    }
                }
            }
        }
    }

    if (g_i2c_hid_device_count > 0) {
        kprintf("[I2C-HID] %d HID device(s) active\n", g_i2c_hid_device_count);
    } else {
        kprintf("[I2C-HID] No I2C HID devices found\n");
    }

    // ---- Initialize spinlocks on controllers and devices ----
    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active) continue;
        spinlock_init(&ctrl->lock, "i2c_ctrl");
        ctrl->worker_running = 0;
    }
    for (int d = 0; d < g_i2c_hid_device_count; d++) {
        i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
        spinlock_init(&dev->dev_lock, "i2c_hid_dev");
        dev->work_pending = 0;
        // Use the device struct address itself as the unique wake channel
        dev->worker_channel = (void *)dev;
    }

    // ---- Spawn one worker thread per controller that has active HID devices ----
    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active) continue;

        // Check if any active HID device lives on this controller
        int has_dev = 0;
        for (int d = 0; d < g_i2c_hid_device_count; d++) {
            if (g_i2c_hid_devices[d].ctrl == ctrl && g_i2c_hid_devices[d].active) {
                has_dev = 1;
                break;
            }
        }
        if (!has_dev) continue;

        void *stack = kalloc(8192);
        if (!stack) {
            kprintf("[I2C-HID] Failed to allocate worker stack for I2C%d\n",
                    ctrl->bus_id);
            continue;
        }
        ctrl->worker_running = 1;
        sched_add_task(i2c_hid_worker_thread, ctrl, stack, 8192);
        kprintf("[I2C-HID] Worker thread created for I2C%d\n", ctrl->bus_id);
    }

    return g_i2c_controller_count;
}

// ============================================================================
// Input Report Processing (bit-field extraction + mouse injection)
// ============================================================================

// Extract a signed field from a HID input report at arbitrary bit offset/size
static int32_t extract_field(const uint8_t *data, uint16_t bit_offset,
                             uint16_t bit_size)
{
    if (bit_size == 0 || bit_size > 32) return 0;
    uint32_t val = 0;
    for (uint16_t i = 0; i < bit_size; i++) {
        uint16_t bit = bit_offset + i;
        if (data[bit / 8] & (1u << (bit % 8)))
            val |= (1u << i);
    }
    // Sign-extend if MSB is set
    if (val & (1u << (bit_size - 1)))
        val |= ~((1u << bit_size) - 1);
    return (int32_t)val;
}

// Extract an unsigned field from a HID input report
static uint32_t extract_field_unsigned(const uint8_t *data, uint16_t bit_offset,
                                       uint16_t bit_size)
{
    if (bit_size == 0 || bit_size > 32) return 0;
    uint32_t val = 0;
    for (uint16_t i = 0; i < bit_size; i++) {
        uint16_t bit = bit_offset + i;
        if (data[bit / 8] & (1u << (bit % 8)))
            val |= (1u << i);
    }
    return val;
}

// Process a mouse/touchpad input report and inject into the mouse subsystem
static void i2c_hid_process_mouse(i2c_hid_device_t *dev,
                                   const uint8_t *report, uint16_t report_len)
{
    i2c_hid_report_info_t *info = &dev->mouse_report;
    if (info->dev_type != I2C_HID_DEV_MOUSE &&
        info->dev_type != I2C_HID_DEV_TOUCHPAD)
        return;

    // Determine if finger is on surface
    int finger_down = 1;
    uint32_t contacts = 0, tip = 0;
    if (info->has_contact_count) {
        contacts = extract_field_unsigned(report,
            info->contact_count.bit_offset, info->contact_count.bit_size);
        if (contacts == 0) finger_down = 0;
    }
    if (info->has_tip_switch) {
        tip = extract_field_unsigned(report,
            info->tip_switch.bit_offset, info->tip_switch.bit_size);
        if (!tip) finger_down = 0;
    }

    // When finger lifts, reset absolute position tracking
    if (!finger_down) {
        g_dbg_worker_no_finger++;
        dev->has_prev_pos = 0;
        return;
    }

    uint8_t buttons = 0;
    if (info->buttons.bit_size > 0) {
        uint32_t btn_raw = extract_field_unsigned(report,
            info->buttons.bit_offset, info->buttons.bit_size);
        buttons = (uint8_t)(btn_raw & 0x07);
    }

    int8_t wheel = 0;
    if (info->has_wheel && info->wheel.bit_size > 0) {
        wheel = (int8_t)extract_field(report,
            info->wheel.bit_offset, info->wheel.bit_size);
    }

    int32_t dx, dy;

    if (info->x.is_relative) {
        // Relative mode (mice): use values directly as deltas
        dx = extract_field(report, info->x.bit_offset, info->x.bit_size);
        dy = extract_field(report, info->y.bit_offset, info->y.bit_size);
    } else {
        // Absolute mode (touchpads): compute deltas from previous position
        int32_t abs_x = (int32_t)extract_field_unsigned(report,
            info->x.bit_offset, info->x.bit_size);
        int32_t abs_y = (int32_t)extract_field_unsigned(report,
            info->y.bit_offset, info->y.bit_size);

        if (!dev->has_prev_pos) {
            // First touch — record position, no movement
            dev->prev_x = abs_x;
            dev->prev_y = abs_y;
            dev->has_prev_pos = 1;
            // Still send button events if any
            if (buttons != dev->prev_buttons) {
                mouse_inject_usb_movement(0, 0, buttons, wheel);
                dev->prev_buttons = buttons;
            }
            return;
        }

        dx = abs_x - dev->prev_x;
        dy = abs_y - dev->prev_y;
        dev->prev_x = abs_x;
        dev->prev_y = abs_y;
    }

    mouse_inject_usb_movement(dx, dy, buttons, wheel);
    dev->prev_buttons = buttons;
    g_dbg_worker_processed++;

    // Log every 50th processed report
    if ((g_dbg_worker_processed % 50) == 1) {
        kprintf("[I2C-DBG] #%u dx=%d dy=%d btn=0x%x "
                "isr=%u hit=%u miss=%u wake=%u read=%u proc=%u "
                "nofing=%u xferr=%u null=%u nid=%u idmm=%u ie=%u\n",
                g_dbg_worker_processed, dx, dy, buttons,
                g_dbg_gpio_isr_count, g_dbg_gpio_isr_hit,
                g_dbg_gpio_isr_miss, g_dbg_worker_wake,
                g_dbg_worker_read, g_dbg_worker_processed,
                g_dbg_worker_no_finger, g_dbg_worker_xfer_err,
                g_dbg_worker_null_pkt, g_dbg_worker_null_id,
                g_dbg_worker_id_mismatch, g_dbg_ie_reenable);
    }
}

// Read from I2C HID device using 2-byte length-first protocol.
// Reads the 2-byte length prefix first, then reads exactly the reported
// number of bytes.  This avoids over-reading wMaxInputLength when the
// actual report is shorter.
// Returns bytes placed in buf (>= 2), or <0 on I2C error.
static int i2c_hid_read_length_first(i2c_hid_device_t *dev,
                                     uint8_t *buf, uint16_t buf_size)
{
    uint8_t hdr[I2C_HID_LENGTH_HDR_SIZE];
    int rc;

    // Step 1: Read 2-byte length prefix
    rc = dw_i2c_xfer_irq(dev->ctrl, dev->i2c_addr, NULL, 0,
                          hdr, I2C_HID_LENGTH_HDR_SIZE);
    if (rc < 0) return rc;

    uint16_t pkt_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);

    // Null / invalid / header-only packet — return just the header
    if (pkt_len == 0 || pkt_len == 0xFFFF || pkt_len <= I2C_HID_LENGTH_HDR_SIZE) {
        buf[0] = hdr[0];
        buf[1] = hdr[1];
        return I2C_HID_LENGTH_HDR_SIZE;
    }

    // Clamp to caller's buffer
    if (pkt_len > buf_size)
        pkt_len = buf_size;

    // Step 2: Read the full report (device re-sends from the beginning)
    rc = dw_i2c_xfer_irq(dev->ctrl, dev->i2c_addr, NULL, 0,
                          buf, pkt_len);
    if (rc < 0) return rc;

    return (int)pkt_len;
}

// Read one input report from the device and process it
static void i2c_hid_read_and_process(i2c_hid_device_t *dev)
{
    uint16_t buf_size = dev->input_buf_size;
    if (buf_size < I2C_HID_LENGTH_HDR_SIZE || !dev->input_buf)
        return;

    uint8_t *buf = dev->input_buf;
    i2c_memset(buf, 0, buf_size);

    // Read using 2-byte length-first protocol
    int rc = i2c_hid_read_length_first(dev, buf, buf_size);

    g_dbg_worker_read++;

    if (rc < 0) {
        dev->error_count++;
        g_dbg_worker_xfer_err++;
        kprintf("[I2C-DBG] xfer ERR #%u dev=0x%02x errcnt=%u "
                "abort=0x%x\n",
                g_dbg_worker_xfer_err, dev->i2c_addr,
                dev->error_count, dev->ctrl->abort_source);
        return;
    }
    dev->error_count = 0;

    // First 2 bytes are the HID-over-I2C length prefix (LE)
    uint16_t pkt_len = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    // Discard null-size packets
    if (pkt_len == 0 || pkt_len == 0xFFFF || pkt_len <= 2) {
        g_dbg_worker_null_pkt++;
        return;
    }

    // Clamp to buffer
    if (pkt_len > buf_size) pkt_len = buf_size;

    // Data starts after the 2-byte length prefix
    const uint8_t *data = buf + 2;
    uint16_t data_len = pkt_len - 2;

    // If device uses report IDs, first data byte is report ID
    uint8_t report_id = 0;
    const uint8_t *report_data = data;
    uint16_t report_data_len = data_len;

    if (dev->mouse_report.has_report_id) {
        if (data_len < 1) return;
        report_id = data[0];

        // Discard null report IDs
        if (report_id == 0) {
            g_dbg_worker_null_id++;
            return;
        }

        report_data = data + 1;
        report_data_len = data_len - 1;

        // Only process if report ID matches our mouse/touchpad report
        if (report_id != dev->mouse_report.report_id) {
            g_dbg_worker_id_mismatch++;
            return;
        }
    }

    if (report_data_len == 0) return;

    i2c_hid_process_mouse(dev, report_data, report_data_len);
}

// ============================================================================
// Worker Thread (Threaded IRQ Bottom-Half)
// ============================================================================

// Per-controller worker thread: blocks until woken by GPIO ISR, then reads
// input reports from all pending devices on this controller.
static void i2c_hid_worker_thread(void *arg)
{
    i2c_dw_controller_t *ctrl = (i2c_dw_controller_t *)arg;

    kprintf("[I2C-HID] Worker thread running for I2C%d\n", ctrl->bus_id);

    while (ctrl->worker_running) {
        // Set BLOCKED *before* checking for work — prevents lost wakes.
        // If the GPIO ISR fires between here and the check below,
        // sched_wake_channel() will find us BLOCKED and set us READY,
        // so sched_schedule() will return immediately.
        task_t *self = sched_current();
        self->wait_channel = (void *)ctrl;
        self->state = TASK_BLOCKED;
        __asm__ volatile("" ::: "memory");

        int any_pending = 0;
        for (int d = 0; d < g_i2c_hid_device_count; d++) {
            i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
            if (dev->ctrl == ctrl && dev->active && dev->work_pending) {
                any_pending = 1;
                break;
            }
        }

        if (!any_pending) {
            // Periodic debug print (every 1 second) even when idle
            uint64_t now = timer_get_uptime();
            if (now > g_dbg_last_print_uptime) {
                g_dbg_last_print_uptime = now;
                kprintf("[I2C-DBG] t=%llu "
                        "i2c_isr=%u gpio_isr=%u hit=%u miss=%u "
                        "wake=%u read=%u proc=%u nofing=%u "
                        "xfer: tot=%u ok=%u err=%u abort=%u retry=%u "
                        "null=%u nid=%u idmm=%u ie=%u\n",
                        (unsigned long long)now,
                        g_dbg_i2c_isr_count, g_dbg_gpio_isr_count,
                        g_dbg_gpio_isr_hit, g_dbg_gpio_isr_miss,
                        g_dbg_worker_wake, g_dbg_worker_read,
                        g_dbg_worker_processed, g_dbg_worker_no_finger,
                        g_dbg_xfer_total, g_dbg_xfer_ok,
                        g_dbg_worker_xfer_err, g_dbg_xfer_abort_count,
                        g_dbg_xfer_retry_count,
                        g_dbg_worker_null_pkt, g_dbg_worker_null_id,
                        g_dbg_worker_id_mismatch, g_dbg_ie_reenable);
            }
            // Set wakeup_tick so the timer IRQ wakes us after ~1 second
            // even if no GPIO interrupt fires. Without this, the thread
            // stays BLOCKED forever when idle and never prints.
            self->wakeup_tick = timer_ticks() + timer_get_frequency();
            g_dbg_worker_wake++;
            sched_schedule();
            continue;
        }

        // Work available — restore running state before processing
        self->state = TASK_RUNNING;
        self->wait_channel = NULL;

        // Process all pending devices on this controller
        for (int d = 0; d < g_i2c_hid_device_count; d++) {
            i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
            if (dev->ctrl != ctrl || !dev->active || !dev->work_pending)
                continue;

            // Acquire controller lock to serialize I2C bus access.
            // Use irqsave — keeps IRQs disabled so the DW I2C ISR
            // cannot race with the transfer loop's direct register polls.
            uint64_t flags;
            spin_lock_irqsave(&ctrl->lock, &flags);

            // Read and process the input report
            i2c_hid_read_and_process(dev);

            spin_unlock_irqrestore(&ctrl->lock, flags);

            // Clear work_pending BEFORE the delay so the ISR can set
            // it again only after we re-enable IE.
            uint64_t dflags;
            spin_lock_irqsave(&dev->dev_lock, &dflags);
            dev->work_pending = 0;
            spin_unlock_irqrestore(&dev->dev_lock, dflags);
        }

        // ---- Rate-limit: wait with IE masked ----
        // The touchpad's level-triggered INT# re-asserts instantly
        // after each read.  If we re-enabled IE immediately, the ISR
        // would fire before we can block, creating a tight ~500 Hz
        // polling loop that stresses the I2C bus into eventual lockup.
        //
        // Keep IE masked during this gap so no ISR fires.  8 ms gives
        // the bus and device firmware breathing room and yields a
        // natural ~100 Hz read rate (matching the touchpad report rate).
        i2c_delay_us(8000);

        // NOW re-enable GPI_IE — any pending INT# assertion will
        // fire the ISR which sets work_pending and wakes us.
        for (int d = 0; d < g_i2c_hid_device_count; d++) {
            i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
            if (dev->ctrl != ctrl || !dev->active)
                continue;
            if (!dev->gpio_irq_active || dev->gpio_community >= g_gpio_community_count)
                continue;

            gpio_community_t *comm = &g_gpio_communities[dev->gpio_community];
            uint16_t ie_base = g_gpio_platform ?
                g_gpio_platform->gpi_ie_offset : 0x120;
            uint32_t ie_reg = ie_base + dev->gpio_gpi_group * 4;

            uint64_t gflags;
            gflags = local_irq_save();
            uint32_t ie_val = gpio_comm_read32(comm, ie_reg);
            gpio_comm_write32(comm, ie_reg,
                              ie_val | (1u << dev->gpio_gpi_bit));
            local_irq_restore(gflags);

            g_dbg_ie_reenable++;
        }
    }

    kprintf("[I2C-HID] Worker thread exiting for I2C%d\n", ctrl->bus_id);
}

// ============================================================================
// GPIO Interrupt Service Routine
// ============================================================================

void i2c_hid_gpio_irq_handler(uint8_t vector)
{
    g_dbg_gpio_isr_count++;

    if (!g_gpio_platform) {
        lapic_eoi();
        return;
    }

    uint16_t is_base = g_gpio_platform->gpi_is_offset;
    uint16_t ie_base = g_gpio_platform->gpi_ie_offset;
    int found_any = 0;

    for (int d = 0; d < g_i2c_hid_device_count; d++) {
        i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
        if (!dev->active || !dev->gpio_irq_active)
            continue;
        if (dev->gpio_irq_vector != vector)
            continue;
        if (dev->gpio_community >= g_gpio_community_count)
            continue;

        gpio_community_t *comm = &g_gpio_communities[dev->gpio_community];
        uint32_t is_reg = is_base + dev->gpio_gpi_group * 4;
        uint32_t is_val = gpio_comm_read32(comm, is_reg);
        uint32_t ie_reg_off = ie_base + dev->gpio_gpi_group * 4;
        uint32_t ie_cur = gpio_comm_read32(comm, ie_reg_off);

        if (!(is_val & (1u << dev->gpio_gpi_bit))) {
            // IS bit not set for our pin
            g_dbg_gpio_isr_miss++;
            if ((g_dbg_gpio_isr_miss % 100) == 1) {
                kprintf("[I2C-ISR] miss #%u vec=%u IS=0x%x IE=0x%x "
                        "bit=%u wp=%d\n",
                        g_dbg_gpio_isr_miss, vector, is_val, ie_cur,
                        dev->gpio_gpi_bit, dev->work_pending);
            }
            continue;
        }

        found_any = 1;
        g_dbg_gpio_isr_hit++;

        // Clear the pending interrupt status (write-1-to-clear)
        gpio_comm_write32(comm, is_reg, (1u << dev->gpio_gpi_bit));

        // Mask GPI_IE for this pin to prevent re-fire until worker processes it
        uint32_t ie_val = ie_cur & ~(1u << dev->gpio_gpi_bit);
        gpio_comm_write32(comm, ie_reg_off, ie_val);

        // Set work_pending flag (spinlock-protected)
        spin_lock(&dev->dev_lock);
        dev->work_pending = 1;
        spin_unlock(&dev->dev_lock);

        // Wake the worker thread for this controller
        sched_wake_channel((void *)dev->ctrl);

        // Log every 50th hit
        if ((g_dbg_gpio_isr_hit % 50) == 1) {
            kprintf("[I2C-ISR] hit #%u vec=%u IS=0x%x->clr "
                    "IE 0x%x->0x%x bit=%u\n",
                    g_dbg_gpio_isr_hit, vector, is_val,
                    ie_cur, ie_val, dev->gpio_gpi_bit);
        }
    }

    if (!found_any) {
        // ISR fired but no device had a pending bit — spurious
        if ((g_dbg_gpio_isr_count % 100) == 1) {
            kprintf("[I2C-ISR] spurious #%u vec=%u\n",
                    g_dbg_gpio_isr_count, vector);
        }
    }

    lapic_eoi();
}

int i2c_hid_has_touchpad(void) {
    for (int d = 0; d < g_i2c_hid_device_count; d++) {
        if (g_i2c_hid_devices[d].active &&
            (g_i2c_hid_devices[d].dev_type == I2C_HID_DEV_TOUCHPAD ||
             g_i2c_hid_devices[d].dev_type == I2C_HID_DEV_MOUSE))
            return 1;
    }
    return 0;
}

int i2c_hid_device_count(void) {
    return g_i2c_hid_device_count;
}

