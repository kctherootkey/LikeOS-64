// LikeOS-64 - ACPI Implementation
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration

#include "../../include/kernel/acpi.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/smp.h"

// ============================================================================
// Global ACPI State
// ============================================================================

static acpi_info_t g_acpi_info = {0};

// ============================================================================
// Helper Functions
// ============================================================================

static int my_memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p1 = (const uint8_t*)a;
    const uint8_t* p2 = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

// Validate checksum of ACPI table
static bool acpi_validate_checksum(void* table, size_t length) {
    uint8_t* ptr = (uint8_t*)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += ptr[i];
    }
    return sum == 0;
}

// ============================================================================
// RSDP Discovery
// ============================================================================

// Search for RSDP in a memory range
static acpi_rsdp_t* acpi_find_rsdp_in_range(uint64_t start, uint64_t end) {
    // RSDP is always aligned to 16 bytes
    for (uint64_t addr = start; addr < end; addr += 16) {
        void* ptr = phys_to_virt(addr);
        if (my_memcmp(ptr, ACPI_SIG_RSDP, 8) == 0) {
            acpi_rsdp_t* rsdp = (acpi_rsdp_t*)ptr;
            // Validate ACPI 1.0 checksum (first 20 bytes)
            if (acpi_validate_checksum(rsdp, 20)) {
                // For ACPI 2.0+, also validate extended checksum
                if (rsdp->revision >= 2) {
                    if (!acpi_validate_checksum(rsdp, rsdp->length)) {
                        continue;
                    }
                }
                return rsdp;
            }
        }
    }
    return NULL;
}

// Find RSDP from UEFI hint or by searching BIOS areas
static acpi_rsdp_t* acpi_find_rsdp(uint64_t rsdp_hint) {
    acpi_rsdp_t* rsdp = NULL;
    
    // If we have a hint (e.g., from UEFI), try that first
    if (rsdp_hint != 0) {
        void* ptr = phys_to_virt(rsdp_hint);
        if (my_memcmp(ptr, ACPI_SIG_RSDP, 8) == 0) {
            rsdp = (acpi_rsdp_t*)ptr;
            if (acpi_validate_checksum(rsdp, 20)) {
                smp_dbg("ACPI: RSDP found at hint address 0x%lx\n", rsdp_hint);
                return rsdp;
            }
        }
    }
    
    // Search EBDA (Extended BIOS Data Area) - first 1KB at segment in [0x40:0x0E]
    // This area might not be accessible with UEFI, skip if unavailable
    
    // Search BIOS ROM area: 0xE0000 - 0xFFFFF
    rsdp = acpi_find_rsdp_in_range(0xE0000, 0x100000);
    if (rsdp) {
        smp_dbg("ACPI: RSDP found in BIOS ROM area at 0x%lx\n", 
                (uint64_t)rsdp - PHYS_MAP_BASE);
        return rsdp;
    }
    
    return NULL;
}

// ============================================================================
// ACPI Table Access
// ============================================================================

// Find an ACPI table by signature
static acpi_sdt_header_t* acpi_find_table(const char* signature) {
    if (!g_acpi_info.rsdp_found || g_acpi_info.rsdp_phys_addr == 0) {
        return NULL;
    }
    
    // Use the saved RSDP physical address
    acpi_rsdp_t* rsdp = (acpi_rsdp_t*)phys_to_virt(g_acpi_info.rsdp_phys_addr);
    
    if (g_acpi_info.acpi_revision >= 2 && rsdp->xsdt_address != 0) {
        // Use XSDT (64-bit entries)
        acpi_xsdt_t* xsdt = (acpi_xsdt_t*)phys_to_virt(rsdp->xsdt_address);
        if (!acpi_validate_checksum(xsdt, xsdt->header.length)) {
            smp_dbg("ACPI: XSDT checksum invalid\n");
            return NULL;
        }
        
        size_t entry_count = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
        for (size_t i = 0; i < entry_count; i++) {
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)phys_to_virt(xsdt->entries[i]);
            if (my_memcmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    } else {
        // Use RSDT (32-bit entries)
        acpi_rsdt_t* rsdt = (acpi_rsdt_t*)phys_to_virt(rsdp->rsdt_address);
        if (!acpi_validate_checksum(rsdt, rsdt->header.length)) {
            smp_dbg("ACPI: RSDT checksum invalid\n");
            return NULL;
        }
        
        size_t entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
        for (size_t i = 0; i < entry_count; i++) {
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)phys_to_virt(rsdt->entries[i]);
            if (my_memcmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    }
    
    return NULL;
}

// ============================================================================
// MADT Parsing
// ============================================================================

static void acpi_parse_madt(void) {
    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table(ACPI_SIG_MADT);
    if (!madt) {
        kprintf("ACPI: MADT not found\n");
        return;
    }
    
    smp_dbg("ACPI: MADT found, length=%u\n", madt->header.length);
    
    // Save LAPIC address
    g_acpi_info.lapic_address = madt->lapic_address;
    
    // Check for dual-8259
    g_acpi_info.dual_8259_present = (madt->flags & 1) != 0;
    
    // Parse MADT entries
    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;
    
    while (ptr < end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        
        if (entry->length == 0) {
            break;  // Prevent infinite loop
        }
        
        switch (entry->type) {
            case MADT_TYPE_LAPIC: {
                madt_lapic_t* lapic = (madt_lapic_t*)entry;
                if (g_acpi_info.cpu_count < MAX_CPUS) {
                    cpu_info_t* cpu = &g_acpi_info.cpus[g_acpi_info.cpu_count];
                    cpu->apic_id = lapic->apic_id;
                    cpu->acpi_processor_id = lapic->acpi_processor_id;
                    cpu->enabled = (lapic->flags & MADT_LAPIC_ENABLED) != 0;
                    cpu->online_capable = (lapic->flags & MADT_LAPIC_ONLINE_CAPABLE) != 0;
                    cpu->bsp = false;  // Will be set later
                    cpu->started = false;
                    
                    if (cpu->enabled || cpu->online_capable) {
                        g_acpi_info.cpu_count++;
                    }
                }
                break;
            }
            
            case MADT_TYPE_IOAPIC: {
                madt_ioapic_t* ioapic = (madt_ioapic_t*)entry;
                if (g_acpi_info.ioapic_count < MAX_IOAPICS) {
                    ioapic_info_t* info = &g_acpi_info.ioapics[g_acpi_info.ioapic_count];
                    info->id = ioapic->ioapic_id;
                    info->address = ioapic->ioapic_address;
                    info->gsi_base = ioapic->gsi_base;
                    g_acpi_info.ioapic_count++;
                }
                break;
            }
            
            case MADT_TYPE_ISO: {
                madt_iso_t* iso = (madt_iso_t*)entry;
                if (g_acpi_info.irq_override_count < MAX_IRQ_OVERRIDES) {
                    irq_override_t* override = &g_acpi_info.irq_overrides[g_acpi_info.irq_override_count];
                    override->bus_irq = iso->source;
                    override->gsi = iso->gsi;
                    override->polarity = iso->flags & MPS_INTI_POLARITY_MASK;
                    override->trigger_mode = (iso->flags & MPS_INTI_TRIGGER_MASK) >> 2;
                    g_acpi_info.irq_override_count++;
                }
                break;
            }
            
            case MADT_TYPE_LAPIC_ADDR: {
                madt_lapic_addr_t* addr = (madt_lapic_addr_t*)entry;
                g_acpi_info.lapic_address = addr->lapic_address;
                break;
            }
            
            default:
                // Ignore unknown entry types
                break;
        }
        
        ptr += entry->length;
    }
}

// ============================================================================
// Public API
// ============================================================================

int acpi_init(uint64_t rsdp_hint) {
    smp_dbg("ACPI: Initializing...\n");
    
    // Find RSDP
    acpi_rsdp_t* rsdp = acpi_find_rsdp(rsdp_hint);
    if (!rsdp) {
        kprintf("ACPI: RSDP not found!\n");
        return -1;
    }
    
    g_acpi_info.rsdp_found = true;
    g_acpi_info.acpi_revision = rsdp->revision;
    
    // Save the RSDP physical address for later use
    // Convert virtual back to physical
    g_acpi_info.rsdp_phys_addr = virt_to_phys((void*)rsdp);
    
    smp_dbg("ACPI: Revision %u, OEM: %.6s\n", rsdp->revision, rsdp->oem_id);
    
    if (rsdp->revision >= 2) {
        smp_dbg("ACPI: XSDT at 0x%lx\n", rsdp->xsdt_address);
    }
    smp_dbg("ACPI: RSDT at 0x%x\n", rsdp->rsdt_address);
    
    // Parse MADT for CPU information
    acpi_parse_madt();
    
    // Determine BSP (first CPU with lowest APIC ID that's enabled, or use LAPIC ID)
    if (g_acpi_info.cpu_count > 0) {
        // Mark BSP (assume CPU 0 is BSP for now; ideally read from LAPIC)
        g_acpi_info.cpus[0].bsp = true;
        g_acpi_info.cpus[0].started = true;
        g_acpi_info.bsp_apic_id = g_acpi_info.cpus[0].apic_id;
    }
    
    acpi_print_info();
    
    return 0;
}

acpi_info_t* acpi_get_info(void) {
    return &g_acpi_info;
}

uint32_t acpi_get_cpu_count(void) {
    return g_acpi_info.cpu_count;
}

cpu_info_t* acpi_get_cpu(uint32_t index) {
    if (index >= g_acpi_info.cpu_count) {
        return NULL;
    }
    return &g_acpi_info.cpus[index];
}

uint32_t acpi_get_bsp_apic_id(void) {
    return g_acpi_info.bsp_apic_id;
}

uint64_t acpi_get_lapic_address(void) {
    return g_acpi_info.lapic_address;
}

ioapic_info_t* acpi_get_ioapic(uint32_t index) {
    if (index >= g_acpi_info.ioapic_count) {
        return NULL;
    }
    return &g_acpi_info.ioapics[index];
}

irq_override_t* acpi_get_irq_override(uint8_t isa_irq) {
    for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
        if (g_acpi_info.irq_overrides[i].bus_irq == isa_irq) {
            return &g_acpi_info.irq_overrides[i];
        }
    }
    return NULL;
}

uint32_t acpi_irq_to_gsi(uint8_t isa_irq) {
    irq_override_t* override = acpi_get_irq_override(isa_irq);
    if (override) {
        return override->gsi;
    }
    // No override, identity mapping
    return isa_irq;
}

void acpi_print_info(void) {
    smp_dbg("ACPI: LAPIC address = 0x%lx\n", g_acpi_info.lapic_address);
    smp_dbg("ACPI: %u CPU(s) found:\n", g_acpi_info.cpu_count);
    
    for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
        cpu_info_t* cpu = &g_acpi_info.cpus[i];
        smp_dbg("  CPU %u: APIC ID=%u, %s%s%s\n",
                i, cpu->apic_id,
                cpu->enabled ? "enabled" : "disabled",
                cpu->bsp ? ", BSP" : "",
                cpu->online_capable ? ", online-capable" : "");
    }
    
    smp_dbg("ACPI: %u I/O APIC(s) found:\n", g_acpi_info.ioapic_count);
    for (uint32_t i = 0; i < g_acpi_info.ioapic_count; i++) {
        ioapic_info_t* ioapic = &g_acpi_info.ioapics[i];
        smp_dbg("  I/O APIC %u: ID=%u, addr=0x%x, GSI base=%u\n",
                i, ioapic->id, ioapic->address, ioapic->gsi_base);
    }
    
    if (g_acpi_info.irq_override_count > 0) {
        smp_dbg("ACPI: %u IRQ override(s):\n", g_acpi_info.irq_override_count);
        for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
            irq_override_t* ovr = &g_acpi_info.irq_overrides[i];
            smp_dbg("  IRQ %u -> GSI %u (pol=%u, trig=%u)\n",
                    ovr->bus_irq, ovr->gsi, ovr->polarity, ovr->trigger_mode);
        }
    }
    
    if (g_acpi_info.dual_8259_present) {
        smp_dbg("ACPI: PC-AT compatible dual-8259 present\n");
    }
}

// ============================================================================
// ACPI Power Management — S5 (poweroff) and reset
// ============================================================================

static uint32_t g_pm1a_cnt_blk = 0;   // PM1a control block I/O port
static uint32_t g_pm1b_cnt_blk = 0;   // PM1b control block I/O port
static uint16_t g_slp_typa = 0;       // SLP_TYPa value for S5
static uint16_t g_slp_typb = 0;       // SLP_TYPb value for S5
static uint8_t  g_reset_reg_space = 0;
static uint64_t g_reset_reg_addr = 0;
static uint8_t  g_reset_value = 0;
static int      g_pm_initialized = 0;

// I/O port access helpers
static inline void acpi_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline void acpi_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t acpi_inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// Parse DSDT/SSDT for \_S5 object to get SLP_TYP values
static int acpi_parse_s5(uint8_t *aml, uint32_t length) {
    // Search for the byte sequence: '_' 'S' '5' '_'
    for (uint32_t i = 0; i + 4 < length; i++) {
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            // Found \_S5 name — skip to the package
            // Look for PackageOp (0x12) within a small window after the name
            for (uint32_t j = i + 4; j < i + 32 && j + 5 < length; j++) {
                if (aml[j] == 0x12) {
                    // PackageOp found: skip PkgLength and NumElements
                    uint32_t k = j + 1;
                    // Decode PkgLength (simple 1-byte form)
                    uint8_t pkg_lead = aml[k];
                    if (pkg_lead & 0xC0) {
                        // Multi-byte PkgLength
                        int extra = (pkg_lead >> 6) & 3;
                        k += 1 + extra;
                    } else {
                        k += 1;
                    }
                    // NumElements byte
                    k++;
                    // Now read SLP_TYPa — could be BytePrefix (0x0A) + byte, or raw byte
                    if (k < length) {
                        if (aml[k] == 0x0A && k + 1 < length) {
                            g_slp_typa = aml[k + 1];
                            k += 2;
                        } else {
                            g_slp_typa = aml[k];
                            k += 1;
                        }
                    }
                    // SLP_TYPb
                    if (k < length) {
                        if (aml[k] == 0x0A && k + 1 < length) {
                            g_slp_typb = aml[k + 1];
                            k += 2;
                        } else {
                            g_slp_typb = aml[k];
                            k += 1;
                        }
                    }
                    return 1; // Success
                }
            }
        }
    }
    return 0; // Not found
}

void acpi_pm_init(void) {
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table(ACPI_SIG_FADT);
    if (!fadt) {
        kprintf("ACPI PM: FADT not found, power management unavailable\n");
        return;
    }

    // Read PM1a/PM1b control block I/O port addresses directly from FADT
    g_pm1a_cnt_blk = fadt->pm1a_control_block;
    g_pm1b_cnt_blk = fadt->pm1b_control_block;

    // Save reset register info
    if (fadt->header.length >= 129) {
        g_reset_reg_space = fadt->reset_reg_addr_space;
        g_reset_reg_addr = fadt->reset_reg_address;
        g_reset_value = fadt->reset_value;
    }

    // Find the DSDT to parse \_S5 for sleep type values
    uint64_t dsdt_addr = 0;
    if (fadt->header.length >= 148 && fadt->x_dsdt != 0) {
        dsdt_addr = fadt->x_dsdt;
    } else {
        dsdt_addr = fadt->dsdt;
    }

    if (dsdt_addr) {
        acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)phys_to_virt(dsdt_addr);
        uint8_t *aml = (uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
        uint32_t aml_len = dsdt->length - sizeof(acpi_sdt_header_t);
        if (acpi_parse_s5(aml, aml_len)) {
            kprintf("ACPI PM: S5 sleep type: SLP_TYPa=%u SLP_TYPb=%u\n",
                    g_slp_typa, g_slp_typb);
        } else {
            kprintf("ACPI PM: \\_S5 not found in DSDT, using defaults\n");
            g_slp_typa = 5;  // Common default for S5
            g_slp_typb = 0;
        }
    }

    g_pm_initialized = 1;
    kprintf("ACPI PM: initialized (PM1a=0x%x, PM1b=0x%x, reset=0x%lx)\n",
            g_pm1a_cnt_blk, g_pm1b_cnt_blk, (unsigned long)g_reset_reg_addr);
}

void acpi_poweroff(void) {
    if (!g_pm_initialized) {
        kprintf("ACPI: power management not initialized, trying QEMU exit\n");
        // QEMU debug exit (ISA debug port)
        acpi_outw(0x604, 0x2000);
        // If that didn't work, halt
        for (;;) __asm__ volatile("cli; hlt");
    }

    kprintf("ACPI: powering off...\n");

    // Disable all interrupts
    __asm__ volatile("cli");

    // Write SLP_TYPa | SLP_EN to PM1a control register
    uint16_t val = ACPI_PM1_SLP_TYP(g_slp_typa) | ACPI_PM1_SLP_EN;
    acpi_outw((uint16_t)g_pm1a_cnt_blk, val);

    // If PM1b exists, write there too
    if (g_pm1b_cnt_blk) {
        val = ACPI_PM1_SLP_TYP(g_slp_typb) | ACPI_PM1_SLP_EN;
        acpi_outw((uint16_t)g_pm1b_cnt_blk, val);
    }

    // If ACPI poweroff failed, try QEMU debug port
    acpi_outw(0x604, 0x2000);

    // Should not reach here
    for (;;) __asm__ volatile("hlt");
}

void acpi_reset(void) {
    kprintf("ACPI: resetting system...\n");

    // Disable all interrupts
    __asm__ volatile("cli");

    // Method 1: ACPI reset register (ACPI 2.0+)
    if (g_reset_reg_addr != 0) {
        if (g_reset_reg_space == 1) {
            // System I/O space
            acpi_outb((uint16_t)g_reset_reg_addr, g_reset_value);
        }
        // Small delay
        for (volatile int i = 0; i < 1000000; i++);
    }

    // Method 2: Keyboard controller reset (8042)
    // Pulse the CPU reset line via the keyboard controller
    uint8_t good = 0x02;
    while (good & 0x02) {
        __asm__ volatile("inb $0x64, %0" : "=a"(good));
    }
    __asm__ volatile("outb %0, $0x64" :: "a"((uint8_t)0xFE));

    // Small delay
    for (volatile int i = 0; i < 1000000; i++);

    // Method 3: Triple fault — load a null IDT and trigger an interrupt
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int $3" :: "m"(null_idt));

    // Should never reach here
    for (;;) __asm__ volatile("hlt");
}
