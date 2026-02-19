// LikeOS-64 Kernel Executive - Interrupt Management
// IDT, IRQ, and exception handling for 64-bit kernel

#ifndef _KERNEL_INTERRUPT_H_
#define _KERNEL_INTERRUPT_H_

#include "console.h"

// I/O port functions
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// IDT entry structure for 64-bit long mode
struct idt_entry {
    uint16_t offset_low;    // Lower 16 bits of handler address
    uint16_t selector;      // Code segment selector
    uint8_t ist;           // Interrupt Stack Table offset (0 for now)
    uint8_t type_attr;     // Type and attributes
    uint16_t offset_mid;   // Middle 16 bits of handler address
    uint32_t offset_high;  // Upper 32 bits of handler address
    uint32_t zero;         // Reserved, must be zero
} __attribute__((packed));

// IDT descriptor
struct idt_descriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// TSS structure for 64-bit mode
struct tss_entry {
    uint32_t reserved1;
    uint64_t rsp0;          // Stack pointer for privilege level 0
    uint64_t rsp1;          // Stack pointer for privilege level 1
    uint64_t rsp2;          // Stack pointer for privilege level 2
    uint64_t reserved2;
    uint64_t ist1;          // Interrupt Stack Table entries
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iopb_offset;   // I/O Permission Bitmap offset
} __attribute__((packed));

// GDT entry structure for TSS
struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

// IDT constants
#define IDT_ENTRIES 256
#define IDT_TYPE_INTERRUPT_GATE 0x8E
#define IDT_TYPE_TRAP_GATE 0x8F

// IRQ constants
#define IRQ_BASE 32  // IRQs mapped to interrupts 32-47
#define IRQ_KEYBOARD 1
#define IRQ_TIMER 0

// PIC ports
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

// PIC commands
#define PIC_EOI 0x20

// Function prototypes
void interrupts_init(void);
void pic_init(void);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);
void pic_send_eoi(uint8_t irq);
void idt_debug_entry(uint8_t num);
void tss_init(void);
void gdt_install_tss(void);

// Exception handlers
void exception_handler(uint64_t *regs);

// IRQ handlers
void irq_handler(uint64_t *regs);
void keyboard_irq_handler(void);
void mouse_irq_handler(void);

// Assembly interrupt stubs (defined in assembly)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

// TSS management for user mode
void tss_set_kernel_stack(uint64_t stack_top);
uint64_t tss_get_kernel_stack(void);

// Get IDT and GDT descriptors for AP initialization
void* interrupts_get_idt_descriptor(void);
void* gdt_get_descriptor(void);

// IRQ stubs
extern void irq32(void);  // Timer
extern void irq33(void);  // Keyboard
extern void irq34(void);
extern void irq35(void);
extern void irq36(void);
extern void irq37(void);
extern void irq38(void);
extern void irq39(void);
extern void irq40(void);
extern void irq41(void);
extern void irq42(void);
extern void irq43(void);
extern void irq44(void);
extern void irq45(void);
extern void irq46(void);
extern void irq47(void);

#endif // _KERNEL_INTERRUPT_H_
