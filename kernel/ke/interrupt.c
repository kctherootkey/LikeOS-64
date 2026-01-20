// LikeOS-64 Interrupt Management
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/sched.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idt_desc;
static struct tss_entry tss;
static uint8_t interrupt_stack[8192] __attribute__((aligned(16)));

static void my_memset(void *dest, int val, size_t len) {
    uint8_t *ptr = (uint8_t*)dest;
    while (len--) {
        *ptr++ = val;
    }
}

extern void idt_flush(uint64_t);
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

void pic_init() {
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    kprintf("PIC initialized\n");
}

static void imcr_route_to_pic(void) {
    outb(0x22, 0x70);
    uint8_t val = inb(0x23);
    val &= ~0x01;
    outb(0x23, val);
}

void idt_set_entry(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].type_attr = flags;
    idt[num].zero = 0;
}

void idt_init() {
    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, 0, 0, 0);
    }

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
    
    idt_flush((uint64_t)&idt_desc);
    kprintf("IDT initialized\n");
}

static const char* exception_messages[] = {
    "Division by Zero", "Debug", "Non-Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication Exception", "Security Exception", "Reserved"
};

void exception_handler(uint64_t *regs) {
    uint64_t int_no = regs[15];
    uint64_t err_code = regs[16];
    uint64_t rip = regs[17];

    if (int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if ((err_code & 0x3) == 0x3) {
            if (mm_handle_cow_fault(cr2)) {
                return;
            }
        }
    }

    console_set_color(VGA_COLOR_RED, VGA_COLOR_BLACK);
    kprintf("=== EXCEPTION: %s (INT %llu) ===\n",
           (int_no < 32) ? exception_messages[int_no] : "Unknown", int_no);
    kprintf("RIP: 0x%016llx  RSP: 0x%016llx  RBP: 0x%016llx\n", rip, regs[20], regs[6]);
    kprintf("RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx\n", regs[0], regs[3], regs[1]);
    kprintf("RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx\n", regs[2], regs[4], regs[5]);

    if (int_no == 8 || (int_no >= 10 && int_no <= 14) || int_no == 17 || int_no == 21 || int_no == 29 || int_no == 30) {
        kprintf("Error Code: 0x%016llx\n", err_code);
        if (int_no == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("Page Fault Address: 0x%016llx\n", cr2);
        }
    }

    uint64_t cr0, cr2, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kprintf("CR0: 0x%016llx  CR2: 0x%016llx  CR3: 0x%016llx\n", cr0, cr2, cr3);

    console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\nSystem halted.\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

volatile uint64_t g_irq0_count = 0;
volatile uint64_t g_irq1_count = 0;
volatile uint64_t g_irq12_count = 0;

void irq_handler(uint64_t *regs) {
    uint64_t int_no = regs[15];
    pic_send_eoi(int_no - 32);

    switch (int_no) {
        case 32:
            g_irq0_count++;
            timer_irq_handler();
            break;
        case 33:
            g_irq1_count++;
            keyboard_irq_handler();
            break;
        case 44:
            g_irq12_count++;
            mouse_irq_handler();
            break;
        default: {
            extern xhci_controller_t g_xhci;
            xhci_irq_service(&g_xhci);
            break;
        }
    }
}

void interrupts_init() {
    extern void gdt_init();
    gdt_init();
    tss_init();
    gdt_install_tss();
    pic_init();
    imcr_route_to_pic();
    idt_init();
    kprintf("Interrupt system initialized\n");
}

void tss_init() {
    my_memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)(interrupt_stack + sizeof(interrupt_stack));
    tss.iopb_offset = sizeof(tss);
    kprintf("TSS initialized\n");
}

void gdt_install_tss() {
    extern void gdt_install_tss_real(uint64_t tss_base, uint64_t tss_size);
    gdt_install_tss_real((uint64_t)&tss, sizeof(tss) - 1);
}

void tss_set_kernel_stack(uint64_t stack_top) {
    tss.rsp0 = stack_top;
}

uint64_t tss_get_kernel_stack(void) {
    return tss.rsp0;
}
