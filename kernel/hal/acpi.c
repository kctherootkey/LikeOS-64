// LikeOS-64 - ACPI Implementation
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration

#include "../../include/kernel/acpi.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"

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
                kprintf("ACPI: RSDP found at hint address 0x%lx\n", rsdp_hint);
                return rsdp;
            }
        }
    }
    
    // Search EBDA (Extended BIOS Data Area) - first 1KB at segment in [0x40:0x0E]
    // This area might not be accessible with UEFI, skip if unavailable
    
    // Search BIOS ROM area: 0xE0000 - 0xFFFFF
    rsdp = acpi_find_rsdp_in_range(0xE0000, 0x100000);
    if (rsdp) {
        kprintf("ACPI: RSDP found in BIOS ROM area at 0x%lx\n", 
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
            kprintf("ACPI: XSDT checksum invalid\n");
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
            kprintf("ACPI: RSDT checksum invalid\n");
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
    
    kprintf("ACPI: MADT found, length=%u\n", madt->header.length);
    
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
    kprintf("ACPI: Initializing...\n");
    
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
    
    kprintf("ACPI: Revision %u, OEM: %.6s\n", rsdp->revision, rsdp->oem_id);
    
    if (rsdp->revision >= 2) {
        kprintf("ACPI: XSDT at 0x%lx\n", rsdp->xsdt_address);
    }
    kprintf("ACPI: RSDT at 0x%x\n", rsdp->rsdt_address);
    
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
    kprintf("ACPI: LAPIC address = 0x%lx\n", g_acpi_info.lapic_address);
    kprintf("ACPI: %u CPU(s) found:\n", g_acpi_info.cpu_count);
    
    for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
        cpu_info_t* cpu = &g_acpi_info.cpus[i];
        kprintf("  CPU %u: APIC ID=%u, %s%s%s\n",
                i, cpu->apic_id,
                cpu->enabled ? "enabled" : "disabled",
                cpu->bsp ? ", BSP" : "",
                cpu->online_capable ? ", online-capable" : "");
    }
    
    kprintf("ACPI: %u I/O APIC(s) found:\n", g_acpi_info.ioapic_count);
    for (uint32_t i = 0; i < g_acpi_info.ioapic_count; i++) {
        ioapic_info_t* ioapic = &g_acpi_info.ioapics[i];
        kprintf("  I/O APIC %u: ID=%u, addr=0x%x, GSI base=%u\n",
                i, ioapic->id, ioapic->address, ioapic->gsi_base);
    }
    
    if (g_acpi_info.irq_override_count > 0) {
        kprintf("ACPI: %u IRQ override(s):\n", g_acpi_info.irq_override_count);
        for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
            irq_override_t* ovr = &g_acpi_info.irq_overrides[i];
            kprintf("  IRQ %u -> GSI %u (pol=%u, trig=%u)\n",
                    ovr->bus_irq, ovr->gsi, ovr->polarity, ovr->trigger_mode);
        }
    }
    
    if (g_acpi_info.dual_8259_present) {
        kprintf("ACPI: PC-AT compatible dual-8259 present\n");
    }
}
