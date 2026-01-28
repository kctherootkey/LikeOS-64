// LikeOS-64 - Minimal IOAPIC support
#include "../../include/kernel/console.h"
#include "../../include/kernel/ioapic.h"
#include "../../include/kernel/interrupt.h" // for idt vector constants if needed
#include "../../include/kernel/memory.h"    // for phys_to_virt

// Default IOAPIC MMIO base (commonly 0xFEC00000). No ACPI parsing yet.
#define IOAPIC_DEFAULT_BASE 0xFEC00000UL

// IOAPIC register selectors
#define IOAPIC_REG_ID      0x00
#define IOAPIC_REG_VER     0x01
#define IOAPIC_REG_ARB     0x02
#define IOAPIC_REG_REDIR(n) (0x10 + (n) * 2) // low 32 bits; high at +1

// IOAPIC base pointer - initialized on first use via direct map
static volatile uint32_t *ioapic_base = NULL;
static int g_ioapic_present = 0;

static inline volatile uint32_t* get_ioapic_base(void) {
    if (!ioapic_base) {
        // Convert physical MMIO address to virtual via direct map
        ioapic_base = (volatile uint32_t*)phys_to_virt(IOAPIC_DEFAULT_BASE);
    }
    return ioapic_base;
}

static inline void ioapic_write(uint8_t reg, uint32_t value) {
    volatile uint32_t* base = get_ioapic_base();
    base[0] = reg;
    base[4] = value; // data register at base+0x10 (index 4 of 32-bit array)
}
static inline uint32_t ioapic_read(uint8_t reg) {
    volatile uint32_t* base = get_ioapic_base();
    base[0] = reg;
    return base[4];
}

int ioapic_detect(void) {
    // Very naive detection: attempt to read version register; ensure reasonable max redirs (< 240)
    volatile uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t max_redir = (ver >> 16) & 0xFF;
    if (max_redir == 0 || max_redir > 0xF0) {
        kprintf("IOAPIC: not detected (ver=0x%08x)\n", ver);
        return -1;
    }
    g_ioapic_present = 1;
    kprintf("IOAPIC: detected (ver=0x%08x, max_redir=%u)\n", ver, max_redir);
    return 0;
}

int ioapic_configure_legacy_irq(uint8_t gsi, uint8_t vector, uint8_t polarity, uint8_t trigger_mode) {
    if (!g_ioapic_present) {
        if (ioapic_detect() != 0) {
            return -1;
        }
    }
    if (gsi > 23) { // support only legacy range for now
        return -2;
    }
    // Build redirection entry low dword
    uint32_t low = 0;
    low |= vector; // bits 0-7
    // Delivery mode 000 (fixed)
    // Destination mode 0 (physical)
    // Polarity (bit 13)
    if (polarity == IOAPIC_POLARITY_LOW) {
        low |= (1u << 13);
    }
    // Trigger mode (bit 15)
    if (trigger_mode == IOAPIC_TRIGGER_LEVEL) {
        low |= (1u << 15);
    }
    // Mask bit 16 = 0 (enabled)
    uint32_t high = 0; // destination APIC ID 0
    // Program high then low (Intel recommendation: write high first then low)
    ioapic_write(IOAPIC_REG_REDIR(gsi) + 1, high);
    ioapic_write(IOAPIC_REG_REDIR(gsi), low);
    kprintf("IOAPIC: GSI %u -> vector 0x%02x (polarity=%s, trigger=%s)\n", gsi, vector,
            (polarity==IOAPIC_POLARITY_LOW)?"low":"high",
            (trigger_mode==IOAPIC_TRIGGER_LEVEL)?"level":"edge");
    return 0;
}
