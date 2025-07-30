// LikeOS-64 Interrupt System Implementation
// 64-bit IDT and IRQ management

#include "interrupts.h"

// IDT table
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idt_desc;

// TSS and interrupt stack
static struct tss_entry tss;
static uint8_t interrupt_stack[8192] __attribute__((aligned(16))); // 8KB stack for interrupts

// Simple memset implementation
static void my_memset(void *dest, int val, size_t len) {
    uint8_t *ptr = (uint8_t*)dest;
    while (len--) {
        *ptr++ = val;
    }
}

// External assembly functions
extern void idt_flush(uint32_t);
extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

// Exception handlers
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

// PIC helper functions
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

void irq_enable(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void irq_disable(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

// Initialize the 8259 PIC
void pic_init() {
    // ICW1: Start initialization sequence
    outb(PIC1_CMD, 0x11);  // ICW1: Initialize + ICW4 needed
    outb(PIC2_CMD, 0x11);
    
    // ICW2: Set vector offsets
    outb(PIC1_DATA, 0x20);  // IRQ 0-7 → interrupts 32-39
    outb(PIC2_DATA, 0x28);  // IRQ 8-15 → interrupts 40-47
    
    // ICW3: Set cascade connection
    outb(PIC1_DATA, 0x04);  // Tell master PIC there's a slave at IRQ2
    outb(PIC2_DATA, 0x02);  // Tell slave PIC its cascade identity
    
    // Set mode
    outb(PIC1_DATA, 0x01);  // ICW4: 8086/88 mode
    outb(PIC2_DATA, 0x01);
    
    // Disable all IRQs initially
    outb(PIC1_DATA, 0xFF);  // Mask all IRQs on master
    outb(PIC2_DATA, 0xFF);  // Mask all IRQs on slave
    
    kprintf("PIC initialized\n");
}

// Set IDT entry
void idt_set_entry(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].type_attr = flags;
    idt[num].zero = 0;
}

// Initialize IDT
void idt_init() {
    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)&idt;
    
    // Clear IDT
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, 0, 0, 0);
    }
    
    // Set up exception handlers
    idt_set_entry(0, (uint64_t)isr0, 0x08, 0x8E);
    idt_set_entry(1, (uint64_t)isr1, 0x08, 0x8E);
    idt_set_entry(2, (uint64_t)isr2, 0x08, 0x8E);
    idt_set_entry(3, (uint64_t)isr3, 0x08, 0x8E);
    idt_set_entry(4, (uint64_t)isr4, 0x08, 0x8E);
    idt_set_entry(5, (uint64_t)isr5, 0x08, 0x8E);
    idt_set_entry(6, (uint64_t)isr6, 0x08, 0x8E);
    idt_set_entry(7, (uint64_t)isr7, 0x08, 0x8E);
    idt_set_entry(8, (uint64_t)isr8, 0x08, 0x8E);
    idt_set_entry(9, (uint64_t)isr9, 0x08, 0x8E);
    idt_set_entry(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_entry(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_entry(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_entry(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_entry(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_entry(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_entry(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_entry(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_entry(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_entry(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_entry(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_entry(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_entry(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_entry(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_entry(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_entry(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_entry(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_entry(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_entry(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_entry(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_entry(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_entry(31, (uint64_t)isr31, 0x08, 0x8E);
    
    // Set up IRQ handlers
    idt_set_entry(32, (uint64_t)irq0, 0x08, 0x8E);
    idt_set_entry(33, (uint64_t)irq1, 0x08, 0x8E);
    idt_set_entry(34, (uint64_t)irq2, 0x08, 0x8E);
    idt_set_entry(35, (uint64_t)irq3, 0x08, 0x8E);
    idt_set_entry(36, (uint64_t)irq4, 0x08, 0x8E);
    idt_set_entry(37, (uint64_t)irq5, 0x08, 0x8E);
    idt_set_entry(38, (uint64_t)irq6, 0x08, 0x8E);
    idt_set_entry(39, (uint64_t)irq7, 0x08, 0x8E);
    idt_set_entry(40, (uint64_t)irq8, 0x08, 0x8E);
    idt_set_entry(41, (uint64_t)irq9, 0x08, 0x8E);
    idt_set_entry(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_entry(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_entry(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_entry(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_entry(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_entry(47, (uint64_t)irq15, 0x08, 0x8E);
    
    // Load IDT
    idt_flush((uint64_t)&idt_desc);
    
    kprintf("IDT initialized\n");
}

// Exception handler - called from assembly with register structure pointer
void exception_handler(uint64_t *regs) {
    uint64_t int_no = regs[15];  // Interrupt number at offset 15*8 = 120
    uint64_t err_code = regs[16]; // Error code at offset 16*8 = 128
    
    kprintf("Exception %d occurred!\n", int_no);
    
    // If this was an error-code exception, print the error code
    if (int_no == 8 || (int_no >= 10 && int_no <= 14) || int_no == 17) {
        kprintf("Error code: %d\n", err_code);
    }
    
    // Halt the system for serious exceptions
    for (;;);
}

// IRQ handler - called from assembly with register structure pointer
void irq_handler(uint64_t *regs) {
    uint64_t int_no = regs[15];  // Interrupt number at offset 15*8 = 120
    
    // Send EOI
    pic_send_eoi(int_no - 32);
    
    // Handle specific IRQs
    switch (int_no) {
        case 33:  // IRQ1 - Keyboard
            keyboard_irq_handler();
            break;
        default:
            kprintf("Unhandled IRQ %d\n", int_no - 32);
            break;
    }
}

// Initialize interrupt system
void interrupts_init() {
    kprintf("Initializing interrupt system...\n");
    
    // Initialize GDT first
    extern void gdt_init();
    gdt_init();
    
    // Initialize TSS
    tss_init();
    
    // Install TSS in GDT
    gdt_install_tss();
    
    // Initialize PIC
    pic_init();
    
    // Initialize IDT
    idt_init();
    
    kprintf("Interrupt system initialized\n");
}

// Initialize TSS
void tss_init() {
    // Clear TSS structure
    my_memset(&tss, 0, sizeof(tss));
    
    // Set up RSP0 (ring 0 stack pointer) for interrupt handling
    tss.rsp0 = (uint64_t)(interrupt_stack + sizeof(interrupt_stack));
    
    // Set I/O permission bitmap offset (no I/O bitmap)
    tss.iopb_offset = sizeof(tss);
    
    kprintf("TSS initialized, interrupt stack at 0x%p\n", (void*)tss.rsp0);
}

// Install TSS in GDT
void gdt_install_tss() {
    extern void gdt_install_tss_real(uint64_t tss_base, uint64_t tss_size);
    gdt_install_tss_real((uint64_t)&tss, sizeof(tss) - 1);
}

// Debug function to print IDT entry details
void idt_debug_entry(uint8_t num) {
    if (num >= IDT_ENTRIES) {
        kprintf("  Invalid IDT entry number: %d\n", num);
        return;
    }
    
    // Reconstruct the full 64-bit address from the IDT entry
    uint64_t handler_addr = 0;
    handler_addr |= idt[num].offset_low;
    handler_addr |= ((uint64_t)idt[num].offset_mid) << 16;
    handler_addr |= ((uint64_t)idt[num].offset_high) << 32;
    
    kprintf("  Entry %d: Handler=0x%p, Selector=0x%04x, Type=0x%02x\n", 
           num, (void*)handler_addr, idt[num].selector, idt[num].type_attr);
}
