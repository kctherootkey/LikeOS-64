// LikeOS-64 - HID over I2C Driver
// Intel DesignWare I2C controller + Microsoft HID-over-I2C protocol
//
// Discovers Intel LPSS Serial IO I2C controllers via PCI, initializes the
// DesignWare I2C IP core, scans I2C buses for HID devices, and injects
// mouse/touchpad input into the existing input subsystems.
//
// I2C transfers use polled mode (busy-wait ~1ms per transfer at 400kHz).
// HID input reports are polled from the main kernel loop.

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

// ============================================================================
// Private data
// ============================================================================

static i2c_dw_controller_t g_i2c_controllers[I2C_DW_MAX_CONTROLLERS];
static int g_i2c_controller_count = 0;

static i2c_hid_device_t g_i2c_hid_devices[I2C_HID_MAX_DEVICES];
static int g_i2c_hid_device_count = 0;

// LPSS I2C uses the DesignWare block at 0x000-0x1FF and LPSS private regs at
// 0x200-0x2FF. One 4KB page covers the entire window we access here.

#define I2C_LPSS_MMIO_PAGES 1

// Polling rate limiter (poll every N calls to reduce bus traffic)
static uint64_t g_poll_counter = 0;
#define I2C_HID_POLL_DIVISOR 1  // Poll every call (main loop is already ~100 Hz)

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


static const char *i2c_fw_status_name(int status)
{
    switch (status) {
        case ACPI_FW_STATUS_OK:
            return "ok";
        case ACPI_FW_STATUS_NO_PS0:
            return "no-ps0";
        case ACPI_FW_STATUS_FAILED:
            return "fail";
        case ACPI_FW_STATUS_UNSUPPORTED:
            return "need-aml";
        default:
            return "none";
    }
}

static int i2c_hid_log_acpi_hid_group(const char *tag,
                                      const char *const *hids,
                                      int hid_count,
                                      int line_budget)
{
    int printed = 0;

    for (int i = 0; i < hid_count && printed < line_budget; i++) {
        acpi_aml_device_info_t matches[2];
        acpi_aml_device_info_t fw_info;
        int found;
        int fw_status;

        i2c_memset(matches, 0, sizeof(matches));
        i2c_memset(&fw_info, 0, sizeof(fw_info));

        found = acpi_aml_find_devices_by_hid(hids[i], matches, 2);
        if (found <= 0)
            continue;

        fw_status = acpi_fw_power_on_device(hids[i], &fw_info);
        kprintf("[I2C-ACPI] %s %s n=%d%s fw=%s hid=%s cid=%s ps0=%u sta=%u crs=%u dep=%u %s pwr=%s\n",
                tag,
                hids[i],
                found,
                found > 1 ? "+" : "",
                i2c_fw_status_name(fw_status),
            matches[0].hid[0] ? matches[0].hid : "-",
            matches[0].cid[0] ? matches[0].cid : "-",
                matches[0].has_ps0,
                matches[0].has_sta,
                matches[0].has_crs,
                matches[0].has_dep,
                matches[0].path,
                fw_info.power_path[0] ? fw_info.power_path : "-");
        printed++;
    }

    if (printed == 0) {
        kprintf("[I2C-ACPI] %s none\n", tag);
    }

    return printed;
}

static void i2c_hid_probe_acpi_firmware(void)
{
    static const char *const lpss_i2c_hids[] = {
        "80860F41", "808622C1", "INT33C2", "INT33C3",
        "INT3432", "INT3433", "INT3442", "INT3443",
        "INT3444", "INT3445", "INT3446", "INT3447",
        "INT34B2", "INT34B3", "INT34B4", "INT34B5",
        "INT34B6", "INT34B7", "80860AAC", "80865AAC",
        "INTC1057", "INTC1058", "INTC1059", "INTC105A",
        "INTC105B", "INTC105C",
        "INTC10D0", "INTC10D1", "INTC10D2", "INTC10D3",
        "INTC10EF", "INTC10F0", "INTC10F1", "INTC10F2",
        "INTC10F3"
    };
    static const char *const hid_i2c_hids[] = {
        "ACPI0C50", "PNP0C50"
    };
    int printed;

    printed = i2c_hid_log_acpi_hid_group("LPSS", lpss_i2c_hids,
                                         (int)(sizeof(lpss_i2c_hids) / sizeof(lpss_i2c_hids[0])),
                                         4);
    i2c_hid_log_acpi_hid_group(printed ? "HID" : "HID",
                               hid_i2c_hids,
                               (int)(sizeof(hid_i2c_hids) / sizeof(hid_i2c_hids[0])),
                               2);
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
    // Master mode, fast speed (400kHz), 7-bit addressing, restart enable
    uint32_t ic_con = DW_IC_CON_MASTER | DW_IC_CON_SPEED_FS |
                      DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE;
    dw_write(ctrl, DW_IC_CON, ic_con);

    // Fast mode SCL timing (conservative for ~120 MHz input clock)
    dw_write(ctrl, DW_IC_FS_SCL_HCNT, 160);
    dw_write(ctrl, DW_IC_FS_SCL_LCNT, 320);

    // SDA hold time
    dw_write(ctrl, DW_IC_SDA_HOLD, 0x001C001C);

    // Spike suppression filter
    dw_write(ctrl, DW_IC_FS_SPKLEN, 5);

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
    return 0;
}

// ============================================================================
// DesignWare I2C Controller - Transfer primitives
// ============================================================================

// Forward declaration for interrupt-driven transfer
static int dw_i2c_xfer_irq(i2c_dw_controller_t *ctrl, uint16_t addr,
                            const uint8_t *wbuf, int wlen,
                            uint8_t *rbuf, int rlen);

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

// Set target address and enable controller
static int dw_i2c_set_target(i2c_dw_controller_t *ctrl, uint16_t addr) {
    dw_i2c_disable(ctrl);

    // Clear any stale abort status
    (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
    (void)dw_read(ctrl, DW_IC_CLR_INTR);

    // Set target address (7-bit)
    dw_write(ctrl, DW_IC_TAR, addr & 0x7F);

    dw_i2c_enable(ctrl);
    return 0;
}

// Write bytes to I2C device, then read bytes (write-then-read transaction)
// wbuf/wlen: bytes to write first (register address, commands, etc.)
// rbuf/rlen: bytes to read back (0 for write-only)
// Returns 0 on success, <0 on error
static int dw_i2c_xfer(i2c_dw_controller_t *ctrl, uint16_t addr,
                        const uint8_t *wbuf, int wlen,
                        uint8_t *rbuf, int rlen) {
    int rc;

    dw_i2c_set_target(ctrl, addr);

    // ---- Write phase ----
    for (int i = 0; i < wlen; i++) {
        uint32_t cmd = (uint32_t)wbuf[i];

        // If this is the last write byte AND there's no read phase, send STOP
        if (i == wlen - 1 && rlen == 0)
            cmd |= DW_IC_DATA_CMD_STOP;

        rc = dw_i2c_wait_tx_not_full(ctrl, 50000);
        if (rc < 0) goto abort;

        dw_write(ctrl, DW_IC_DATA_CMD, cmd);
    }

    // ---- Read phase ----
    int rx_remain = rlen;
    int rx_idx = 0;
    int tx_issued = 0;

    while (rx_remain > 0 || tx_issued < rlen) {
        // Issue read commands (fill TX FIFO with read requests)
        while (tx_issued < rlen) {
            uint32_t cmd = DW_IC_DATA_CMD_READ;

            // If this was a write-then-read, RESTART before first read
            if (tx_issued == 0 && wlen > 0)
                cmd |= DW_IC_DATA_CMD_RESTART;

            // STOP on last read
            if (tx_issued == rlen - 1)
                cmd |= DW_IC_DATA_CMD_STOP;

            rc = dw_i2c_wait_tx_not_full(ctrl, 10000);
            if (rc < 0) goto abort;

            dw_write(ctrl, DW_IC_DATA_CMD, cmd);
            tx_issued++;

            // Don't overfill TX FIFO — leave room for RX reads
            if (tx_issued - rx_idx >= (int)ctrl->tx_fifo_depth - 1)
                break;
        }

        // Collect received bytes
        while (rx_idx < tx_issued) {
            rc = dw_i2c_wait_rx_not_empty(ctrl, 50000);
            if (rc < 0) goto abort;

            uint32_t data = dw_read(ctrl, DW_IC_DATA_CMD);
            if (rbuf && rx_idx < rlen)
                rbuf[rx_idx] = (uint8_t)(data & 0xFF);
            rx_idx++;
            rx_remain--;
        }
    }

    // Wait for bus idle
    dw_i2c_wait_idle(ctrl, 10000);
    return 0;

abort:
    {
        uint32_t abort_src = dw_read(ctrl, DW_IC_TX_ABRT_SOURCE);
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        // Drain RX FIFO
        while (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
            (void)dw_read(ctrl, DW_IC_DATA_CMD);
        // Brief delay for bus recovery
        i2c_delay_us(100);
        (void)abort_src;  // suppress unused warning
        return -1;
    }
}

// Convenience: write-only transfer
static int dw_i2c_write(i2c_dw_controller_t *ctrl, uint16_t addr,
                         const uint8_t *buf, int len) {
    if (ctrl->use_interrupts)
        return dw_i2c_xfer_irq(ctrl, addr, buf, len, NULL, 0);
    return dw_i2c_xfer(ctrl, addr, buf, len, NULL, 0);
}

// Convenience: read with 2-byte register address prefix
static int dw_i2c_read_reg16(i2c_dw_controller_t *ctrl, uint16_t addr,
                              uint16_t reg, uint8_t *buf, int len) {
    uint8_t regbuf[2] = { (uint8_t)(reg & 0xFF), (uint8_t)((reg >> 8) & 0xFF) };
    if (ctrl->use_interrupts)
        return dw_i2c_xfer_irq(ctrl, addr, regbuf, 2, buf, len);
    return dw_i2c_xfer(ctrl, addr, regbuf, 2, buf, len);
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
            uint8_t msi_vector = I2C_MSI_VECTOR_BASE + (uint8_t)count;
            if (pci_enable_msi(pci_dev, msi_vector) == 0) {
                ctrl->irq_vector = msi_vector;
                ctrl->use_interrupts = 1;
                kprintf("[I2C%d] MSI vector %u enabled\n", ci, msi_vector);
                dw_write(ctrl, DW_IC_INTR_MASK, 0);
                (void)dw_read(ctrl, DW_IC_CLR_INTR);
            } else {
                ctrl->use_interrupts = 0;
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
    int rc = dw_i2c_write(dev->ctrl, dev->i2c_addr, buf, 4);
    if (rc < 0) return rc;

    // Wait for reset to complete (spec says up to 5 seconds, typically <100ms)
    // The device will pull INT low and send a reset-completion 2-byte message
    i2c_delay_us(100000);  // 100ms

    // Read and discard the reset-completion response
    uint8_t resp[2];
    if (dev->ctrl->use_interrupts)
        dw_i2c_xfer_irq(dev->ctrl, dev->i2c_addr, NULL, 0, resp, 2);
    else
        dw_i2c_xfer(dev->ctrl, dev->i2c_addr, NULL, 0, resp, 2);

    return 0;
}

// ============================================================================
// Minimal HID Report Descriptor Parser
// ============================================================================

// Parse a HID report descriptor to determine device type and report layout
static void i2c_hid_parse_report_desc(i2c_hid_device_t *dev) {
    uint8_t *rd = dev->report_desc;
    uint16_t rd_len = dev->report_desc_len;

    // Parser state
    uint16_t usage_page = 0;
    uint16_t usage = 0;
    uint8_t  report_id = 0;
    int32_t  logical_min = 0;
    int32_t  logical_max = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint16_t bit_offset = 0;     // Running bit offset for current report
    uint8_t  collection_depth = 0;
    uint8_t  in_mouse_collection = 0;
    uint8_t  in_touchpad_collection = 0;
    (void)0;  // device type tracked via in_*_collection flags

    // We track the first mouse/touchpad report found
    int mouse_found = 0;

    uint16_t pos = 0;
    while (pos < rd_len) {
        uint8_t prefix = rd[pos];

        // Long items (prefix == 0xFE)
        if (prefix == 0xFE) {
            if (pos + 2 >= rd_len) break;
            uint8_t data_size = rd[pos + 1];
            pos += 3 + data_size;
            continue;
        }

        // Short items
        uint8_t bSize = prefix & 0x03;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bTag = (prefix >> 4) & 0x0F;
        int size = (bSize == 3) ? 4 : bSize;

        if (pos + 1 + size > rd_len) break;

        // Get item data (up to 4 bytes, signed extension for some items)
        int32_t data_signed = 0;
        uint32_t data_unsigned = 0;
        if (size >= 1) data_unsigned = rd[pos + 1];
        if (size >= 2) data_unsigned |= (uint32_t)rd[pos + 2] << 8;
        if (size >= 3) data_unsigned |= (uint32_t)rd[pos + 3] << 16;
        if (size >= 4) data_unsigned |= (uint32_t)rd[pos + 4] << 24;

        // Sign-extend for signed items
        if (size == 1) data_signed = (int8_t)(data_unsigned & 0xFF);
        else if (size == 2) data_signed = (int16_t)(data_unsigned & 0xFFFF);
        else if (size == 4) data_signed = (int32_t)data_unsigned;
        else data_signed = (int32_t)data_unsigned;

        switch (bType) {
            case 0: // Main
                switch (bTag) {
                    case 0x08: // Input
                    {
                        uint8_t is_const = data_unsigned & 0x01;
                        uint8_t is_relative = (data_unsigned & 0x04) ? 1 : 0;

                        if (!is_const && (in_mouse_collection || in_touchpad_collection) && !mouse_found) {
                            i2c_hid_report_info_t *ri = &dev->mouse_report;
                            ri->report_id = report_id;
                            ri->has_report_id = (report_id != 0);
                            ri->dev_type = in_touchpad_collection ? I2C_HID_DEV_TOUCHPAD : I2C_HID_DEV_MOUSE;

                            // Identify the field by usage
                            if (usage_page == 0x09) {
                                // Buttons (Usage Page: Button)
                                ri->buttons.report_id = report_id;
                                ri->buttons.bit_offset = bit_offset;
                                ri->buttons.bit_size = report_size * report_count;
                                ri->buttons.count = report_count;
                                ri->buttons.logical_min = logical_min;
                                ri->buttons.logical_max = logical_max;
                            } else if (usage_page == 0x01 && usage == 0x30) {
                                // X (Generic Desktop: X)
                                ri->x.report_id = report_id;
                                ri->x.bit_offset = bit_offset;
                                ri->x.bit_size = report_size;
                                ri->x.count = 1;
                                ri->x.logical_min = logical_min;
                                ri->x.logical_max = logical_max;
                                ri->x.is_relative = is_relative;
                            } else if (usage_page == 0x01 && usage == 0x31) {
                                // Y (Generic Desktop: Y)
                                ri->y.report_id = report_id;
                                ri->y.bit_offset = bit_offset;
                                ri->y.bit_size = report_size;
                                ri->y.count = 1;
                                ri->y.logical_min = logical_min;
                                ri->y.logical_max = logical_max;
                                ri->y.is_relative = is_relative;
                            } else if (usage_page == 0x01 && usage == 0x38) {
                                // Wheel (Generic Desktop: Wheel)
                                ri->wheel.report_id = report_id;
                                ri->wheel.bit_offset = bit_offset;
                                ri->wheel.bit_size = report_size;
                                ri->wheel.count = 1;
                                ri->wheel.logical_min = logical_min;
                                ri->wheel.logical_max = logical_max;
                                ri->wheel.is_relative = 1;
                                ri->has_wheel = 1;
                            }
                        }
                        bit_offset += report_size * report_count;
                        usage = 0;  // Reset local usage
                        break;
                    }
                    case 0x0A: // Collection
                        collection_depth++;
                        if (usage_page == 0x01 && usage == 0x02) {
                            in_mouse_collection = collection_depth;
                        }
                        if (usage_page == 0x0D && (usage == 0x05 || usage == 0x04)) {
                            // Touchpad or Touchscreen
                            in_touchpad_collection = collection_depth;
                        }
                        usage = 0;
                        break;
                    case 0x0C: // End Collection
                        if (in_mouse_collection == collection_depth) {
                            in_mouse_collection = 0;
                            if (dev->mouse_report.x.bit_size > 0)
                                mouse_found = 1;
                        }
                        if (in_touchpad_collection == collection_depth) {
                            in_touchpad_collection = 0;
                            if (dev->mouse_report.x.bit_size > 0)
                                mouse_found = 1;
                        }
                        if (collection_depth > 0) collection_depth--;
                        break;
                }
                break;

            case 1: // Global
                switch (bTag) {
                    case 0x00: usage_page = (uint16_t)data_unsigned; break;
                    case 0x01: logical_min = data_signed; break;
                    case 0x02: logical_max = data_signed; break;
                    case 0x07: report_size = data_unsigned; break;
                    case 0x08: // Report ID
                        report_id = (uint8_t)data_unsigned;
                        bit_offset = 0;  // Reset bit offset for new report
                        break;
                    case 0x09: report_count = data_unsigned; break;
                }
                break;

            case 2: // Local
                switch (bTag) {
                    case 0x00: usage = (uint16_t)data_unsigned; break;
                }
                break;
        }

        pos += 1 + size;
    }

    // Determine device type based on what was found
    if (mouse_found) {
        dev->dev_type = dev->mouse_report.dev_type;
        // Calculate total report size
        uint16_t max_bit = 0;
        if (dev->mouse_report.buttons.bit_offset + dev->mouse_report.buttons.bit_size > max_bit)
            max_bit = dev->mouse_report.buttons.bit_offset + dev->mouse_report.buttons.bit_size;
        if (dev->mouse_report.x.bit_offset + dev->mouse_report.x.bit_size > max_bit)
            max_bit = dev->mouse_report.x.bit_offset + dev->mouse_report.x.bit_size;
        if (dev->mouse_report.y.bit_offset + dev->mouse_report.y.bit_size > max_bit)
            max_bit = dev->mouse_report.y.bit_offset + dev->mouse_report.y.bit_size;
        if (dev->mouse_report.has_wheel &&
            dev->mouse_report.wheel.bit_offset + dev->mouse_report.wheel.bit_size > max_bit)
            max_bit = dev->mouse_report.wheel.bit_offset + dev->mouse_report.wheel.bit_size;
        dev->mouse_report.report_bytes = (max_bit + 7) / 8;

        kprintf("[I2C-HID] Parsed %s report: ID=%d  buttons@%u(%u bits)  "
                "X@%u(%u bits%s)  Y@%u(%u bits%s)  wheel=%s  total=%u bytes\n",
                dev->dev_type == I2C_HID_DEV_TOUCHPAD ? "touchpad" : "mouse",
                dev->mouse_report.report_id,
                dev->mouse_report.buttons.bit_offset, dev->mouse_report.buttons.bit_size,
                dev->mouse_report.x.bit_offset, dev->mouse_report.x.bit_size,
                dev->mouse_report.x.is_relative ? ",rel" : ",abs",
                dev->mouse_report.y.bit_offset, dev->mouse_report.y.bit_size,
                dev->mouse_report.y.is_relative ? ",rel" : ",abs",
                dev->mouse_report.has_wheel ? "yes" : "no",
                dev->mouse_report.report_bytes);
    } else {
        dev->dev_type = I2C_HID_DEV_UNKNOWN;
        kprintf("[I2C-HID] Could not determine device type from report descriptor\n");
    }
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
// HID over I2C — Input report reading and processing
// ============================================================================

// Extract a signed integer from a bit field in a report buffer
static int32_t extract_field(const uint8_t *data, uint16_t bit_offset,
                              uint16_t bit_size) {
    if (bit_size == 0 || bit_size > 32) return 0;

    int32_t value = 0;
    for (uint16_t i = 0; i < bit_size; i++) {
        uint16_t bit_pos = bit_offset + i;
        uint16_t byte_pos = bit_pos / 8;
        uint8_t bit_idx = bit_pos % 8;
        if (data[byte_pos] & (1 << bit_idx))
            value |= (1 << i);
    }

    // Sign-extend if the top bit is set
    if (bit_size < 32 && (value & (1 << (bit_size - 1)))) {
        value |= ~((1 << bit_size) - 1);
    }

    return value;
}

// Process a mouse/touchpad input report using parsed field info
static void i2c_hid_process_mouse(i2c_hid_device_t *dev,
                                    const uint8_t *data, int data_len) {
    i2c_hid_report_info_t *ri = &dev->mouse_report;

    if (ri->x.bit_size == 0) return;  // No parsed layout

    int data_bits = data_len * 8;

    // Bounds-check field positions
    if ((int)(ri->buttons.bit_offset + ri->buttons.bit_size) > data_bits) return;
    if ((int)(ri->x.bit_offset + ri->x.bit_size) > data_bits) return;
    if ((int)(ri->y.bit_offset + ri->y.bit_size) > data_bits) return;

    // Extract fields
    uint8_t buttons = (uint8_t)extract_field(data, ri->buttons.bit_offset,
                                              ri->buttons.bit_size);
    int32_t x = extract_field(data, ri->x.bit_offset, ri->x.bit_size);
    int32_t y = extract_field(data, ri->y.bit_offset, ri->y.bit_size);
    int8_t wheel = 0;
    if (ri->has_wheel) {
        int32_t w = extract_field(data, ri->wheel.bit_offset, ri->wheel.bit_size);
        wheel = (int8_t)(w > 127 ? 127 : (w < -128 ? -128 : w));
    }

    // For absolute touchpad data, convert to relative deltas
    if (!ri->x.is_relative) {
        // Scale absolute coordinates to screen-relative deltas
        static int32_t last_abs_x = -1, last_abs_y = -1;
        static uint8_t last_touch = 0;

        // Touchpads typically have a "tip switch" or button set when finger is down
        uint8_t touching = buttons & 0x01;

        if (touching && last_touch && last_abs_x >= 0) {
            // Calculate relative delta from absolute position change
            int32_t range_x = ri->x.logical_max - ri->x.logical_min;
            int32_t range_y = ri->y.logical_max - ri->y.logical_min;
            if (range_x <= 0) range_x = 4096;
            if (range_y <= 0) range_y = 4096;

            // Scale: absolute range → reasonable pixel movement
            int dx = (int)((x - last_abs_x) * 1024 / range_x);
            int dy = (int)((y - last_abs_y) * 768 / range_y);

            // Clamp to reasonable range
            if (dx > 50) dx = 50;
            if (dx < -50) dx = -50;
            if (dy > 50) dy = 50;
            if (dy < -50) dy = -50;

            // For touchpad, use button 0 as click, not tip-down
            uint8_t mouse_btns = (buttons >> 1) & 0x07;  // buttons 1-3 are actual clicks
            mouse_inject_usb_movement(dx, dy, mouse_btns, wheel);
        }

        last_abs_x = touching ? x : -1;
        last_abs_y = touching ? y : -1;
        last_touch = touching;
    } else {
        // Relative mode — direct injection
        int dx = (int)x;
        int dy = (int)y;
        // Clamp large values
        if (dx > 127) dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127) dy = 127;
        if (dy < -127) dy = -127;
        mouse_inject_usb_movement(dx, dy, buttons, wheel);
    }
}

// Read one input report from the device
static int i2c_hid_read_input(i2c_hid_device_t *dev) {
    if (!dev->active) return -1;

    uint16_t max_len = dev->desc.wMaxInputLength;
    if (max_len > I2C_HID_MAX_REPORT_SIZE) max_len = I2C_HID_MAX_REPORT_SIZE;
    if (max_len < 2) return -1;

    // Read from input register
    int rc = dw_i2c_read_reg16(dev->ctrl, dev->i2c_addr,
                                dev->desc.wInputRegister,
                                dev->input_buf, max_len);
    if (rc < 0) return -1;

    // First 2 bytes are the length field
    uint16_t report_len = (uint16_t)dev->input_buf[0] |
                          ((uint16_t)dev->input_buf[1] << 8);

    // No data available
    if (report_len == 0 || report_len == 0xFFFF || report_len <= 2)
        return 0;

    // Sanity check
    if (report_len > max_len) return -1;

    uint8_t *report_data = &dev->input_buf[2];
    int report_data_len = report_len - 2;

    if (report_data_len <= 0) return 0;

    // Handle report ID
    uint8_t report_id = 0;
    if (dev->mouse_report.has_report_id) {
        report_id = report_data[0];
        report_data++;
        report_data_len--;
    }

    // Dispatch mouse/touchpad reports
    if (dev->dev_type == I2C_HID_DEV_MOUSE ||
        dev->dev_type == I2C_HID_DEV_TOUCHPAD) {
        if (dev->mouse_report.has_report_id &&
            report_id != dev->mouse_report.report_id) {
            return 0;  // Not our report — skip
        }
        i2c_hid_process_mouse(dev, report_data, report_data_len);
    }

    return 1;  // Report processed
}

// ============================================================================
// Interrupt-Driven I2C Transfer
// ============================================================================

// ISR: called from irq_handler when an I2C MSI/IOAPIC interrupt fires
void i2c_hid_irq_handler(uint8_t vector) {
    // Find which controller this vector belongs to
    for (int c = 0; c < g_i2c_controller_count; c++) {
        i2c_dw_controller_t *ctrl = &g_i2c_controllers[c];
        if (!ctrl->active || !ctrl->use_interrupts)
            continue;
        if (ctrl->irq_vector != vector)
            continue;

        // Read and clear interrupt status
        uint32_t stat = dw_read(ctrl, DW_IC_INTR_STAT);

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

    dw_i2c_set_target(ctrl, addr);

    // Clear state flags
    ctrl->irq_pending = 0;
    ctrl->tx_complete = 0;
    ctrl->rx_ready = 0;
    ctrl->xfer_error = 0;
    ctrl->abort_source = 0;

    // ---- Write phase ----
    for (int i = 0; i < wlen; i++) {
        uint32_t cmd = (uint32_t)wbuf[i];
        if (i == wlen - 1 && rlen == 0)
            cmd |= DW_IC_DATA_CMD_STOP;

        // Wait for TX FIFO space (quick poll — FIFO is typically 64 deep)
        for (timeout = 0; timeout < 50000; timeout++) {
            if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_TFNF)
                break;
            if (ctrl->xfer_error) goto abort;
            i2c_delay_us(1);
        }
        if (timeout >= 50000) goto abort;

        dw_write(ctrl, DW_IC_DATA_CMD, cmd);
    }

    // ---- Read phase with interrupt-driven RX ----
    int rx_idx = 0;
    int tx_issued = 0;

    if (rlen > 0) {
        // Enable RX_FULL and TX_ABRT interrupts
        dw_write(ctrl, DW_IC_INTR_MASK,
                 DW_IC_INTR_RX_FULL | DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET);

        while (rx_idx < rlen) {
            // Issue read commands
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
            for (timeout = 0; timeout < 100000; timeout++) {
                if (ctrl->xfer_error) {
                    dw_write(ctrl, DW_IC_INTR_MASK, 0);
                    goto abort;
                }
                // Check RX FIFO directly (in case interrupt was coalesced)
                if (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
                    break;
                if (ctrl->rx_ready)
                    break;
                // Brief pause, but much shorter than full polling
                // because the interrupt will unblock us fast in practice
                i2c_delay_us(1);
            }
            if (timeout >= 100000) {
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
    }

    // Wait for bus idle
    dw_i2c_wait_idle(ctrl, 10000);
    return 0;

abort:
    {
        (void)dw_read(ctrl, DW_IC_CLR_TX_ABRT);
        (void)dw_read(ctrl, DW_IC_CLR_INTR);
        dw_write(ctrl, DW_IC_INTR_MASK, 0);
        while (dw_read(ctrl, DW_IC_STATUS) & DW_IC_STATUS_RFNE)
            (void)dw_read(ctrl, DW_IC_DATA_CMD);
        i2c_delay_us(100);
        return -1;
    }
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

            for (int hi = 0; hi < 2; hi++) {
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
                                i2c_hid_probe_device(ctrl, addr);
                                found_via_acpi++;
                            }
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
        i2c_hid_init_device(&g_i2c_hid_devices[d]);
    }

    if (g_i2c_hid_device_count > 0) {
        kprintf("[I2C-HID] %d HID device(s) active\n", g_i2c_hid_device_count);
    } else {
        kprintf("[I2C-HID] No I2C HID devices found\n");
    }
    return g_i2c_controller_count;
}

void i2c_hid_poll(void) {
    if (g_i2c_hid_device_count == 0) return;

    g_poll_counter++;
    if ((g_poll_counter % I2C_HID_POLL_DIVISOR) != 0) return;

    static int debug_cnt = 0;

    for (int d = 0; d < g_i2c_hid_device_count; d++) {
        i2c_hid_device_t *dev = &g_i2c_hid_devices[d];
        if (!dev->active) continue;

        // Read up to 8 reports per poll cycle (drain pending data)
        for (int rep = 0; rep < 8; rep++) {
            int rc = i2c_hid_read_input(dev);
            if (debug_cnt < 5) {
                kprintf("[I2C-POLL] dev%d rc=%d len=%u%u\n",
                        d, rc,
                        (unsigned)dev->input_buf[0],
                        (unsigned)dev->input_buf[1]);
                debug_cnt++;
            }
            if (rc <= 0)
                break;
        }
    }
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
