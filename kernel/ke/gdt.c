// LikeOS-64 Kernel Executive - GDT Management
// Global Descriptor Table setup with TSS support for 64-bit mode

#include "../../include/kernel/interrupt.h"

// GDT structure
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// GDT with TSS entry (need extra space for 128-bit TSS descriptor)
static struct gdt_entry gdt[8];  // Increased to accommodate 128-bit TSS
static struct gdt_ptr gdt_pointer;

// External function to load GDT
extern void gdt_flush(uint64_t);

// Set GDT entry
static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

// Set TSS entry (128-bit entry for 64-bit mode)
static void gdt_set_tss(int num, uint64_t base, uint64_t limit) {
    // TSS descriptor is 16 bytes (128 bits) in 64-bit mode
    // We need to set up two consecutive GDT entries
    
    // First 8 bytes (standard descriptor format)
    gdt[num].limit_low = limit & 0xFFFF;
    gdt[num].base_low = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access = 0x89;  // Present, Ring 0, TSS Available
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].base_high = (base >> 24) & 0xFF;
    
    // Second 8 bytes (upper 32 bits of base address)
    gdt[num + 1].limit_low = (base >> 32) & 0xFFFF;
    gdt[num + 1].base_low = (base >> 48) & 0xFFFF;
    gdt[num + 1].base_middle = 0;
    gdt[num + 1].access = 0;
    gdt[num + 1].granularity = 0;
    gdt[num + 1].base_high = 0;
}

// Initialize GDT with basic segments and TSS
void gdt_init() {
    gdt_pointer.limit = (sizeof(struct gdt_entry) * 8) - 1;  // Updated for 8 entries
    gdt_pointer.base = (uint64_t)&gdt;
    
    // Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // Kernel code segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);  // AF = Long mode, 4KB granularity
    
    // Kernel data segment 
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    // User code segment
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xAF);
    
    // User data segment
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    // TSS entry - entries 5 and 6 (128-bit TSS descriptor)
    gdt_set_gate(5, 0, 0, 0, 0);  // Will be set up later
    gdt_set_gate(6, 0, 0, 0, 0);  // Second half of TSS descriptor
    
    // Reserve entry 7
    gdt_set_gate(7, 0, 0, 0, 0);
    
    // Load the GDT
    gdt_flush((uint64_t)&gdt_pointer);
    
    kprintf("GDT initialized\n");
}

// Install TSS in the GDT
void gdt_install_tss_real(uint64_t tss_base, uint64_t tss_size) {
    // Set up TSS entry in GDT
    gdt_set_tss(5, tss_base, tss_size);

    // Reload GDT with TSS
    gdt_flush((uint64_t)&gdt_pointer);

    // Load TSS register
    __asm__ volatile ("ltr %0" : : "r" ((uint16_t)0x28)); // 5 * 8 = 0x28

    kprintf("TSS installed in GDT\n");
}
