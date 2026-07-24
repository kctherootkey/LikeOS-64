// LikeOS-64 Interrupt Management
#include <kernel/ke/interrupt.h>
#include <kernel/dev/usb/xhci.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/signal.h>
#include <kernel/io/tty.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/smp.h>
#include <kernel/dev/hid/i2c_hid.h>
#include <kernel/net/net.h>
#include <kernel/dev/nic/e1000.h>
#include <kernel/net/softirq.h>
#include <kernel/uapi/bug.h>

// Write a formatted message to a task's controlling TTY.
// Falls back to kprintf (kernel console) if the task has no ctty.
// Safe to call from exception context: PTY path is lock-free (ring buffer);
// console TTY path holds a brief spinlock — acceptable since we're about to
// kill the process anyway.
static void __attribute__((format(printf, 2, 3)))
task_tty_printf(task_t *t, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	__builtin_va_start(args, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, args);
	__builtin_va_end(args);
	tty_printf(t ? t->ctty : NULL, "%s", buf);
}

#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_AUXDATA 0x20

static inline int ps2_output_is_aux(void)
{
	uint8_t status = inb(PS2_STATUS_PORT);
	return (status & (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_AUXDATA)) ==
	       (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_AUXDATA);
}

static inline int ps2_output_is_keyboard(void)
{
	uint8_t status = inb(PS2_STATUS_PORT);
	return (status & PS2_STATUS_OUTPUT_FULL) &&
	       !(status & PS2_STATUS_AUXDATA);
}

/* Fully drain the shared i8042 output buffer.
 *
 * The PS/2 keyboard (IRQ1) and mouse (IRQ12) share one byte of output
 * buffer behind port 0x60, and the IOAPIC delivers both lines EDGE
 * triggered: an interrupt fires only on the OBF (output-buffer-full)
 * 0->1 transition.  If a second byte is latched into the buffer in the
 * window between our read of 0x60 and the controller re-arming, no new
 * edge is generated.  OBF stays high, no further IRQ1/IRQ12 ever fires,
 * and ALL keyboard/mouse input is dead until something reads 0x60 again.
 * Nothing polls it, so the wedge is permanent — the classic symptom is a
 * keyboard that stops responding while the timer-driven cursor keeps
 * blinking.  Reading a single byte per IRQ (the old behaviour) walks
 * straight into this trap whenever two bytes arrive close together
 * (easy under SMP, or when mouse motion races a keystroke).
 *
 * Loop until OBF clears, dispatching each byte to the correct handler by
 * its AUXB source bit.  Both keyboard_irq_handler() and mouse_irq_handler()
 * read exactly one byte from 0x60, so every iteration makes progress.  The
 * bound is a safety net against a wedged/absent controller that reports OBF
 * (or 0xFF) forever; a real PS/2 device never has anywhere near 64 bytes
 * queued at human input rates. */
static void ps2_drain_output(void)
{
	for (int i = 0; i < 64; i++) {
		uint8_t status = inb(PS2_STATUS_PORT);
		if (status == 0xFF) /* controller gone */
			break;
		if (!(status & PS2_STATUS_OUTPUT_FULL)) /* buffer empty */
			break;
		if (status & PS2_STATUS_AUXDATA)
			mouse_irq_handler(); /* consumes one byte */
		else
			keyboard_irq_handler(); /* consumes one byte */
	}
}

// ACPI SCI dispatch (from oslikeos.c)
extern int acpi_sci_dispatch(void);
#define ACPI_SCI_VECTOR 58

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idt_desc;
static struct tss_entry tss; // BSP TSS

// ============================================================================
// INTERRUPT / IST STACK LAYOUT (guard pages)
// ============================================================================
// x86-64 stacks grow DOWNWARD.  A guard page at the LOWEST address of each
// stack catches overflow immediately instead of silently corrupting adjacent
// memory.  Layout per stack:
//
//   [base + 0          .. base + PAGE_SIZE - 1]   NOT PRESENT (guard)
//   [base + PAGE_SIZE  .. base + TOTAL_SIZE - 1]  present + writable (usable)
//
// The TSS RSP0 / IST pointers are set to (base + TOTAL_SIZE) so the CPU
// starts using the stack just below the usable region's top.
// mm_mark_guard_page() is called on 'base' during tss_init() / tss_init_ap()
// to make the guard page not-present.  Arrays are page-aligned so the guard
// occupies exactly one 4 KB PTE without overlapping adjacent variables.

// Usable sizes (same as before — we add one guard page on top)
#define IRQ_USABLE_SIZE 16384
#define IRQ_TOTAL_SIZE (PAGE_SIZE + IRQ_USABLE_SIZE)

// IST (Interrupt Stack Table) stacks for critical exceptions that need
// guaranteed separate stacks (double fault, NMI, machine check).
// These are per-CPU to avoid stack sharing in SMP.
//
// SIZING NOTE: 8 KiB usable (was 4 KiB).  IST stacks have a guard page
// below their usable region; a #PF on that guard, taken while we are
// already delivering #DF/NMI/MC, is a "fault during exception delivery"
// of an exception that is itself routed via the same IST entry — Intel
// SDM §6.15 escalates this to a shutdown / triple fault, NOT a clean
// re-dispatch.  Our kernel_oops printer (which #DF reaches under
// stack-overflow recovery) easily uses ~2 KiB of stack between its own
// frame, ksnprintf, and oops_pf_decode → kprintf → console_putchar,
// so a 4 KiB usable region was within one stack-protected nested call
// chain of triggering the guard.  Bumping to 8 KiB gives every IST
// handler a comfortable margin while keeping the guard page below for
// detection of any real runaway.
//
// Per-CPU rows are static arrays of IST_TOTAL_SIZE; the row stride
// MUST be a multiple of PAGE_SIZE so each AP's guard lands on its own
// 4 KiB PTE.  4096 + 8192 = 12288 = 3·PAGE_SIZE — OK.
#define IST_USABLE_SIZE 8192
#define IST_TOTAL_SIZE (PAGE_SIZE + IST_USABLE_SIZE)
_Static_assert((IST_TOTAL_SIZE & (PAGE_SIZE - 1)) == 0,
	       "IST_TOTAL_SIZE must be a multiple of PAGE_SIZE so per-AP "
	       "stack rows are page-aligned and guard pages do not "
	       "overlap adjacent CPUs' stacks");

// IST stack assignments:
//   IST1 = Double Fault (#DF, INT 8)  - CRITICAL: prevents triple fault
//   IST2 = NMI (INT 2)                - can interrupt at any time
//   IST3 = Machine Check (#MC, INT 18) - similar to NMI

// BSP stacks — must be page-aligned so guard page does not overlap neighbours
static uint8_t interrupt_stack[IRQ_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE)));

// BSP IST stacks
static uint8_t bsp_ist1_stack[IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // Double Fault
static uint8_t bsp_ist2_stack[IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // NMI
static uint8_t bsp_ist3_stack[IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // Machine Check

// Per-CPU TSS for SMP (index 0 = BSP, 1..MAX_CPUS-1 = APs)
#define MAX_CPUS_TSS 64
static struct tss_entry ap_tss[MAX_CPUS_TSS] __attribute__((aligned(16)));
static uint8_t ap_interrupt_stacks[MAX_CPUS_TSS][IRQ_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE)));

// Per-AP IST stacks — IRQ_TOTAL_SIZE = 5*PAGE_SIZE, IST_TOTAL_SIZE = 3*PAGE_SIZE
// (1 guard + 8 KiB usable), so every row starts at a page boundary (the
// _Static_assert above on IST_TOTAL_SIZE % PAGE_SIZE enforces this).
static uint8_t ap_ist1_stacks[MAX_CPUS_TSS][IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // Double Fault
static uint8_t ap_ist2_stacks[MAX_CPUS_TSS][IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // NMI
static uint8_t ap_ist3_stacks[MAX_CPUS_TSS][IST_TOTAL_SIZE]
	__attribute__((aligned(PAGE_SIZE))); // Machine Check

static void my_memset(void *dest, int val, size_t len)
{
	uint8_t *ptr = (uint8_t *)dest;
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
extern void irq16();
extern void irq17();
extern void irq18();
extern void irq19();
extern void irq20();
extern void irq21();
extern void irq22();
extern void irq23();
extern void irq24();
extern void irq25();
extern void irq26(); // ACPI SCI
extern void irq27(); // E1000 NIC MSI
extern void irq28(); // e1000e NIC MSI
extern void irq29(); // vmxnet3 NIC MSI

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

// Track whether LAPIC is active (set by lapic_init on BSP)
static volatile int g_lapic_active = 0;

// Forward declarations — needed by pic_send_eoi below
static inline uint8_t pic_read_isr(void);
static inline uint8_t pic2_read_isr(void);

void interrupts_set_lapic_active(int active)
{
	g_lapic_active = active;
}

void pic_send_eoi(uint8_t irq)
{
	// When LAPIC is active, check if this interrupt came through the LAPIC.
	// Two cases set the LAPIC ISR bit:
	//   1. AP LAPIC timer: only LAPIC EOI needed, do NOT touch the 8259 PIC
	//   2. BSP virtual wire (PIC via LINT0=ExtINT): BOTH LAPIC EOI and PIC EOI
	//      needed — LAPIC EOI clears the ISR bit, PIC EOI un-stalls the 8259
	// Sending PIC EOI from an AP would corrupt the shared 8259 PIC state.
	//
	// NOTE: g_lapic_active is set on the BSP after lapic_init(), but APs also
	// have a live LAPIC (their timer fires at this same vector 0x20).  Use
	// lapic_is_available() as the per-CPU test so APs send LAPIC EOI too.
	if (g_lapic_active || lapic_is_available()) {
		uint8_t vector = irq + 32;
		uint32_t isr_reg = LAPIC_ISR_BASE + (vector / 32) * 0x10;
		uint32_t isr_bit = 1U << (vector % 32);
		if (lapic_read(isr_reg) & isr_bit) {
			// LAPIC ISR bit is set — always send LAPIC EOI
			lapic_eoi();
			// On BSP (CPU 0) we must also send PIC EOI when the
			// interrupt actually came from the 8259 PIC (virtual wire
			// via LINT0=ExtINT).  However, the BSP's LAPIC timer
			// fires at the SAME vector (0x20) and the PIC is NOT
			// involved in that case.  Blindly sending PIC EOI would
			// acknowledge whatever the PIC considers highest priority,
			// potentially swallowing a keyboard or mouse IRQ.
			// Fix: read the PIC ISR and only ACK the PIC when the
			// corresponding IRQ bit is actually in-service.
			// Note: before percpu_init(), GS base is 0 and this_cpu_id()
			// would fault.  If GS is unset we are always the BSP (CPU 0).
			uint32_t cpu = read_gs_base_msr() ? this_cpu_id() : 0;
			if (cpu == 0) {
				if (irq < 8) {
					uint8_t isr = pic_read_isr();
					if (isr & (1u << irq)) {
						outb(PIC1_CMD, 0x20);
					}
				} else {
					uint8_t isr2 = pic2_read_isr();
					if (isr2 & (1u << (irq - 8))) {
						outb(PIC2_CMD, 0x20);
						outb(PIC1_CMD, 0x20);
					}
				}
			}
			return;
		}
	}

	// PIC interrupt without LAPIC involvement (pre-LAPIC init)
	if (irq >= 8) {
		outb(PIC2_CMD, 0x20);
	}
	outb(PIC1_CMD, 0x20);
}

void irq_enable(uint8_t irq)
{
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

void irq_disable(uint8_t irq)
{
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

void pic_init()
{
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

static void imcr_route_to_pic(void)
{
	outb(0x22, 0x70);
	uint8_t val = inb(0x23);
	val &= ~0x01;
	outb(0x23, val);
}

void idt_set_entry(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags)
{
	BUG_ON(num >= IDT_ENTRIES);
	idt[num].offset_low = base & 0xFFFF;
	idt[num].offset_mid = (base >> 16) & 0xFFFF;
	idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
	idt[num].selector = sel;
	idt[num].ist = 0;
	idt[num].type_attr = flags;
	idt[num].zero = 0;
}

// Set IDT entry with IST (Interrupt Stack Table) index
// ist_index: 1-7 for IST1-IST7, 0 for no IST (use RSP0)
void idt_set_entry_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags,
		       uint8_t ist_index)
{
	BUG_ON(num >= IDT_ENTRIES);
	WARN_ON(ist_index == 0 ||
		ist_index > 7); /* IST index must be 1-7 for IST entries */
	idt[num].offset_low = base & 0xFFFF;
	idt[num].offset_mid = (base >> 16) & 0xFFFF;
	idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
	idt[num].selector = sel;
	idt[num].ist = ist_index & 0x7; // IST is 3 bits (0-7)
	idt[num].type_attr = flags;
	idt[num].zero = 0;
}

void idt_init()
{
	BUILD_BUG_ON(sizeof(struct idt_entry) != 16);
	BUILD_BUG_ON(IDT_ENTRIES != 256);
	idt_desc.limit = sizeof(idt) - 1;
	idt_desc.base = (uint64_t)&idt;

	for (int i = 0; i < IDT_ENTRIES; i++) {
		idt_set_entry(i, 0, 0, 0);
	}

	idt_set_entry(0, (uint64_t)isr0, 0x08, 0x8E);
	idt_set_entry(1, (uint64_t)isr1, 0x08, 0x8E);
	idt_set_entry_ist(2, (uint64_t)isr2, 0x08, 0x8E, 2); // NMI -> IST2
	idt_set_entry(3, (uint64_t)isr3, 0x08, 0x8E);
	idt_set_entry(4, (uint64_t)isr4, 0x08, 0x8E);
	idt_set_entry(5, (uint64_t)isr5, 0x08, 0x8E);
	idt_set_entry(6, (uint64_t)isr6, 0x08, 0x8E);
	idt_set_entry(7, (uint64_t)isr7, 0x08, 0x8E);
	idt_set_entry_ist(8, (uint64_t)isr8, 0x08, 0x8E,
			  1); // Double Fault -> IST1 (CRITICAL!)
	idt_set_entry(9, (uint64_t)isr9, 0x08, 0x8E);
	idt_set_entry(10, (uint64_t)isr10, 0x08, 0x8E);
	idt_set_entry(11, (uint64_t)isr11, 0x08, 0x8E);
	idt_set_entry(12, (uint64_t)isr12, 0x08, 0x8E);
	idt_set_entry(13, (uint64_t)isr13, 0x08, 0x8E);
	idt_set_entry(14, (uint64_t)isr14, 0x08, 0x8E);
	idt_set_entry(15, (uint64_t)isr15, 0x08, 0x8E);
	idt_set_entry(16, (uint64_t)isr16, 0x08, 0x8E);
	idt_set_entry(17, (uint64_t)isr17, 0x08, 0x8E);
	idt_set_entry_ist(18, (uint64_t)isr18, 0x08, 0x8E,
			  3); // Machine Check -> IST3
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
	idt_set_entry(48, (uint64_t)irq16, 0x08,
		      0x8E); // MSI: xHCI USB controller 0
	idt_set_entry(49, (uint64_t)irq17, 0x08,
		      0x8E); // MSI: xHCI USB controller 1

	// I2C LPSS interrupt vectors
	idt_set_entry(50, (uint64_t)irq18, 0x08, 0x8E); // I2C LPSS 0
	idt_set_entry(51, (uint64_t)irq19, 0x08, 0x8E); // I2C LPSS 1
	idt_set_entry(52, (uint64_t)irq20, 0x08, 0x8E); // I2C LPSS 2
	idt_set_entry(53, (uint64_t)irq21, 0x08, 0x8E); // I2C LPSS 3

	// GPIO interrupt vectors for I2C HID
	idt_set_entry(54, (uint64_t)irq22, 0x08, 0x8E); // GPIO 0
	idt_set_entry(55, (uint64_t)irq23, 0x08, 0x8E); // GPIO 1
	idt_set_entry(56, (uint64_t)irq24, 0x08, 0x8E); // GPIO 2
	idt_set_entry(57, (uint64_t)irq25, 0x08, 0x8E); // GPIO 3
	idt_set_entry(58, (uint64_t)irq26, 0x08, 0x8E); // ACPI SCI
	idt_set_entry(59, (uint64_t)irq27, 0x08, 0x8E); // MSI: E1000 NIC
	idt_set_entry(60, (uint64_t)irq28, 0x08,
		      0x8E); // MSI: e1000e NIC (82574L/82583V)
	idt_set_entry(61, (uint64_t)irq29, 0x08,
		      0x8E); // MSI: vmxnet3 paravirt NIC

	// IPI vectors for SMP
	idt_set_entry(0xFC, (uint64_t)ipi_vector_0xFC, 0x08,
		      0x8E); // TLB shootdown
	idt_set_entry(0xFD, (uint64_t)ipi_vector_0xFD, 0x08, 0x8E); // Halt CPU
	idt_set_entry(0xFE, (uint64_t)ipi_vector_0xFE, 0x08,
		      0x8E); // Reschedule
	idt_set_entry(0xFF, (uint64_t)ipi_vector_0xFF, 0x08,
		      0x8E); // LAPIC spurious

	idt_flush((uint64_t)&idt_desc);
	kprintf("IDT initialized\n");
}

static const char *exception_messages[] = { "Division by Zero",
					    "Debug",
					    "Non-Maskable Interrupt",
					    "Breakpoint",
					    "Overflow",
					    "Bound Range Exceeded",
					    "Invalid Opcode",
					    "Device Not Available",
					    "Double Fault",
					    "Coprocessor Segment Overrun",
					    "Invalid TSS",
					    "Segment Not Present",
					    "Stack-Segment Fault",
					    "General Protection Fault",
					    "Page Fault",
					    "Reserved",
					    "x87 FP Exception",
					    "Alignment Check",
					    "Machine Check",
					    "SIMD FP Exception",
					    "Virtualization Exception",
					    "Control Protection Exception",
					    "Reserved",
					    "Reserved",
					    "Reserved",
					    "Reserved",
					    "Reserved",
					    "Reserved",
					    "Hypervisor Injection",
					    "VMM Communication Exception",
					    "Security Exception",
					    "Reserved" };

/* Register frame indices — must match isr_common_stub push order in interrupt.asm.
 * Push sequence: rax,rbx,rcx,rdx,rsi,rdi,rbp,r8,r9,r10,r11,r12,r13,r14,r15
 * After push r15, RSP points to r15; rdi=RSP before XMM save, so regs[0]=r15. */
#define REGS_R15 0
#define REGS_R14 1
#define REGS_R13 2
#define REGS_R12 3
#define REGS_R11 4
#define REGS_R10 5
#define REGS_R9 6
#define REGS_R8 7
#define REGS_RBP 8
#define REGS_RDI 9
#define REGS_RSI 10
#define REGS_RDX 11
#define REGS_RCX 12
#define REGS_RBX 13
#define REGS_RAX 14
#define REGS_INTNO 15
#define REGS_ERRC 16
#define REGS_RIP 17
#define REGS_CS 18
#define REGS_RFLAGS 19
#define REGS_RSP 20 /* always pushed by x86-64 CPU, even same-privilege */
#define REGS_SS 21

/* Safe kernel-address check for stack trace walking */
static int is_kernel_addr(uint64_t addr)
{
	return addr >= 0xffff800000000000ULL && addr < 0xfffffffffffff000ULL;
}

/* Dump 32 bytes centered on RIP (16 before, 16 starting at RIP) so the
 * faulting instruction can be disassembled post-mortem with
 *     printf '<bytes>' | x86_64-objdump -D -b binary -m i386:x86-64 \
 *         -M intel --adjust-vma=<rip-16> /dev/stdin
 * or recompiled into a hex blob and dropped on top of the unstripped
 * kernel ELF.  Best-effort: skip pages whose presence we cannot verify
 * without risking a recursive #PF inside the oops handler. */
static void oops_rip_bytes(uint64_t rip)
{
#ifndef DEBUG
	/* The raw faulting-instruction bytes are only useful with the DWARF
     * symbols / unstripped ELF that DEBUG=1 keeps; a CRASH_VERBOSE-only
     * (production-like) build cannot disassemble them, so skip the dump. */
	(void)rip;
#else
	if (!is_kernel_addr(rip))
		return;
	uint64_t start = rip - 16;
	/* Bail out if [start, start+32) straddles a not-present page.
     * Probe the first and last bytes via the same PT-walk used by
     * oops_pf_decode — but cheaper: just check the PTE is present.
     * Walking via the direct map is safe because the bootloader-mapped
     * range covers physical RAM that holds the page tables. */
	/* mm_user_addr_mapped walks the active CR3 PT; it is safe to call on
     * kernel addresses too — returns false on any not-present entry. */
	if (!mm_user_addr_mapped(start, 32))
		return;

	kprintf("\n--- Bytes around RIP (RIP-16 .. RIP+15) ---\n");
	const uint8_t *p = (const uint8_t *)start;
	/* Two rows of 16 bytes each.  Mark the byte at RIP with [..]. */
	for (int row = 0; row < 2; row++) {
		kprintf("  %016llx:", (unsigned long long)(start + row * 16));
		for (int col = 0; col < 16; col++) {
			uint64_t addr = start + row * 16 + col;
			if (addr == rip)
				kprintf(" [%02x]", p[row * 16 + col]);
			else
				kprintf(" %02x", p[row * 16 + col]);
		}
		kprintf("\n");
	}
#endif /* DEBUG */
}

/* Walk the RBP frame chain and print return addresses via kprintf */
static void oops_stack_trace(uint64_t rbp, uint64_t fault_rip)
{
	kprintf("\nCall Trace:\n");
	kprintf("  [<%016llx>] (fault RIP)\n", fault_rip);
	int depth = 0;
	while (is_kernel_addr(rbp) && depth < 20) {
		uint64_t *frame = (uint64_t *)rbp;
		uint64_t ret_addr = frame[1]; /* [rbp+8] = return address */
		uint64_t next_rbp = frame[0]; /* [rbp+0] = saved RBP */
		if (!ret_addr || !is_kernel_addr(ret_addr))
			break;
		kprintf("  [<%016llx>]\n", ret_addr);
		if (next_rbp <= rbp) /* prevent loops / upward drift */
			break;
		rbp = next_rbp;
		depth++;
	}
}

/* Decode page-fault error code bits into human-readable form.
 *
 * Walking the page tables here is delicate: CR3 and the next-level table
 * pointers inside each PTE are PHYSICAL addresses.  The bootloader's
 * identity mapping is torn down by mm_remove_identity_mapping(), so we
 * MUST go through the direct map (phys_to_virt) — dereferencing the raw
 * physical address as a virtual pointer faults, and since we are already
 * inside the oops handler that recursive #PF turns into an infinite
 * exception loop (the symptom the user is hitting under SMP stress).
 *
 * The direct map covers the first 16 GB of physical RAM.  Page tables
 * always live inside that range (we allocate them from the kernel page
 * allocator), but be defensive anyway and bail out if a phys addr falls
 * outside it — keeping the oops printer working is more important than
 * decoding the PTE bits.
 */
static void oops_pf_decode(uint64_t err, uint64_t cr2)
{
	kprintf("  Fault address : 0x%016llx\n", cr2);
	kprintf("  Error code    : 0x%016llx  [%s | %s | %s%s%s]\n", err,
		(err & 1) ? "protection-violation" : "not-present",
		(err & 2) ? "write" : "read",
		(err & 4) ? "user-mode" : "kernel-mode",
		(err & 8) ? " | reserved-bit" : "",
		(err & 16) ? " | insn-fetch" : "");

	/* Only walk canonical addresses; non-canonical CR2 means the access
     * itself was a GP-fault-style mistake and the page tables have nothing
     * to say about it. */
	if (!is_kernel_addr(cr2) && cr2 >= 0x0000800000000000ULL)
		return;

	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	uint64_t pml4_phys = cr3 & ~0xFFFULL;
	if (!is_phys_in_direct_map(pml4_phys)) {
		kprintf("  (PML4 phys 0x%016llx outside direct map; skipping PTE walk)\n",
			pml4_phys);
		return;
	}

	uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
	uint64_t idx4 = (cr2 >> 39) & 0x1FF;
	uint64_t idx3 = (cr2 >> 30) & 0x1FF;
	uint64_t idx2 = (cr2 >> 21) & 0x1FF;
	uint64_t idx1 = (cr2 >> 12) & 0x1FF;

	if (!(pml4[idx4] & 1)) {
		kprintf("  PML4[%llu]: not present\n", idx4);
		return;
	}
	uint64_t pdpt_phys = pml4[idx4] & ~0xFFFULL;
	if (!is_phys_in_direct_map(pdpt_phys)) {
		kprintf("  PDPT phys 0x%016llx outside direct map\n",
			pdpt_phys);
		return;
	}
	uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

	if (!(pdpt[idx3] & 1)) {
		kprintf("  PDPT[%llu]: not present\n", idx3);
		return;
	}
	if (pdpt[idx3] & (1ULL << 7)) {
		kprintf("  1-GB page: PTE=0x%016llx  [%s%s%s%s]\n", pdpt[idx3],
			(pdpt[idx3] & 1) ? "P " : "",
			(pdpt[idx3] & 2) ? "W " : "",
			(pdpt[idx3] & 4) ? "U " : "",
			(pdpt[idx3] & (1ULL << 63)) ? "NX" : "");
		return;
	}
	uint64_t pd_phys = pdpt[idx3] & ~0xFFFULL;
	if (!is_phys_in_direct_map(pd_phys)) {
		kprintf("  PD phys 0x%016llx outside direct map\n", pd_phys);
		return;
	}
	uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);

	if (!(pd[idx2] & 1)) {
		kprintf("  PD[%llu]: not present\n", idx2);
		return;
	}
	if (pd[idx2] & (1ULL << 7)) {
		kprintf("  2-MB page: PTE=0x%016llx  [%s%s%s%s]\n", pd[idx2],
			(pd[idx2] & 1) ? "P " : "", (pd[idx2] & 2) ? "W " : "",
			(pd[idx2] & 4) ? "U " : "",
			(pd[idx2] & (1ULL << 63)) ? "NX" : "");
		return;
	}
	uint64_t pt_phys = pd[idx2] & ~0xFFFULL;
	if (!is_phys_in_direct_map(pt_phys)) {
		kprintf("  PT phys 0x%016llx outside direct map\n", pt_phys);
		return;
	}
	uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);
	uint64_t pte = pt[idx1];
	kprintf("  PTE[%llu]:   0x%016llx  [%s%s%s%s]\n", idx1, pte,
		(pte & 1) ? "P " : "not-present ", (pte & 2) ? "W " : "RO ",
		(pte & 4) ? "U " : "S ", (pte & (1ULL << 63)) ? "NX" : "X");
}

/* Re-entry guards and locking discipline for the oops printer
 * ============================================================
 *
 * What we need from the oops path:
 *
 *   (1) Survive recursive faults — a bug inside the printer (e.g. the
 *       earlier oops_pf_decode that walked page tables via raw physical
 *       pointers) must not loop forever.
 *
 *   (2) Survive concurrent faults on multiple CPUs without garbling
 *       output and without deadlocking.
 *
 *   (3) NEVER block waiting for another CPU.  Other CPUs may be holding
 *       arbitrary locks (console_lock in particular) when we trigger an
 *       oops; if we stop them via IPI *before* we print, kprintf will
 *       deadlock acquiring a lock whose holder is now permanently parked.
 *       That deadlock is silent (no oops output, no serial, mouse stops)
 *       and was the actual bug behind "QEMU just hangs".
 *
 * The discipline below:
 *
 *   * Per-CPU `g_oops_in_progress` short-circuits same-CPU recursion to
 *     a one-line halt — no further regs/PTE/locks touched.
 *
 *   * `g_oops_writer` is a global atomic CAS-claim: only one CPU at a
 *     time gets to be the active oops printer.  Any other CPU that
 *     simultaneously oopses spins waiting for the first one to finish.
 *     (We do this *instead of* halting the others, which is what caused
 *     the deadlock above.)
 *
 *   * Other CPUs are halted only AFTER all printing has completed —
 *     see the call to smp_halt_others() at the bottom of kernel_oops.
 *     By then console_lock is no longer needed.
 *
 *   * Recursion-safe printer: even the recursive-entry path uses kprintf
 *     because console_lock itself is recursion-safe via try-acquisition
 *     semantics in our serial path — at worst the inner line is dropped.
 */
static volatile uint32_t g_oops_in_progress[MAX_CPUS];
static volatile uint32_t g_oops_writer =
	0xFFFFFFFFu; /* CPU id of active printer, or ~0 */

void kernel_oops(const char *reason, uint64_t *regs)
{
	__asm__ volatile("cli" ::: "memory");

	/* Determine our CPU id, but don't fault doing it: percpu/GS may not be
     * set up (early boot) or may itself be the thing that broke. */
	uint32_t cpu_id_safe = read_gs_base_msr() ? this_cpu_id() : 0;
	if (cpu_id_safe >= MAX_CPUS)
		cpu_id_safe = 0;

	if (__atomic_exchange_n(&g_oops_in_progress[cpu_id_safe], 1,
				__ATOMIC_ACQ_REL)) {
		/* Recursive entry on the same CPU — almost certainly a fault
         * inside our own printers.  Don't try to print anything that
         * touches regs again; just halt. */
		kprintf("\n[oops: recursive entry on CPU %u — halting]\n",
			cpu_id_safe);
		for (;;)
			__asm__ volatile("cli; hlt");
	}

	/* Claim the global oops-writer slot.  Concurrent oopses on other CPUs
     * spin here until we're done printing; they then take over and print
     * their own report.  This serialises output WITHOUT halting the other
     * CPUs first (which would deadlock the printer if they were holding
     * console_lock).  No timeout — we'd rather wait than print into a
     * lock that the previous writer still owns.
     *
     * Note: this CAS is the ONLY place we synchronise with other CPUs
     * before printing.  We never call smp_halt_others() here.  Other
     * CPUs are halted at the very END of kernel_oops, after all kprintf
     * calls have released console_lock for the last time. */
	uint32_t expected = 0xFFFFFFFFu;
	while (!__atomic_compare_exchange_n(
		&g_oops_writer, &expected, cpu_id_safe,
		/* weak */ 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
		if (expected == cpu_id_safe)
			break; /* shouldn't happen, but be defensive */
		expected = 0xFFFFFFFFu;
		__asm__ volatile("pause" ::: "memory");
	}

	/* Sanity-check regs.  If it isn't a canonical kernel pointer, we
     * cannot dereference it — print what we can and halt.  This is what
     * was crashing recursively: the fault inside the page-table walk in
     * oops_pf_decode caused exception_handler to re-enter kernel_oops
     * with a regs frame on the new (recursive) fault stack, and at some
     * point in the cascade the value we read back from -0x90(%rbp) here
     * was no longer a valid pointer. */
	if (!regs || !is_kernel_addr((uint64_t)regs)) {
		kprintf("\n");
		kprintf("============================================================\n");
		kprintf("Oops: %s\n", reason ? reason : "kernel exception");
		kprintf("(regs pointer invalid: %p — cannot decode register frame.\n",
			(void *)regs);
		kprintf(" Likely a recursive fault inside the oops handler on CPU %u.)\n",
			cpu_id_safe);
		kprintf("============================================================\n");
		kprintf("System halted.\n");
		for (;;)
			__asm__ volatile("cli; hlt");
	}

	uint64_t rip = regs[REGS_RIP];
	uint64_t rsp = regs[REGS_RSP];
	uint64_t rbp = regs[REGS_RBP];
	uint64_t cs = regs[REGS_CS];
	uint64_t rflags = regs[REGS_RFLAGS];
	uint64_t int_no = regs[REGS_INTNO];
	uint64_t err_code = regs[REGS_ERRC];
	int user_mode = (cs & 0x3) == 0x3;

	uint64_t cr2, cr3, cr4;
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

	console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

	kprintf("\n");
	kprintf("============================================================\n");
	kprintf("Oops: %s\n", reason ? reason : "kernel exception");
	kprintf("============================================================\n");

	/* Exception identity */
	const char *exc_name =
		(int_no < 32) ? exception_messages[int_no] : "Unknown";
	kprintf("Exception : INT %llu  (%s)\n", int_no, exc_name);
	kprintf("Mode      : %s\n", user_mode ? "user" : "kernel");

	/* CPU / thread identification (GS may be unset during early boot) */
	uint32_t cpu_id = 0;
	task_t *cur = NULL;
	if (read_gs_base_msr()) {
		cpu_id = this_cpu_id();
		cur = sched_current();
		percpu_t *pcpu = this_cpu();
		int syscall_nr = pcpu ? pcpu->current_syscall_nr : -1;
		kprintf("CPU       : %u\n", cpu_id);
		if (cur) {
			kprintf("Process   : pid=%d tgid=%d comm=\"%s\"\n",
				cur->id, cur->tgid,
				cur->comm[0] ? cur->comm : "(anon)");
			kprintf("Thread    : tid=%d\n", cur->id);
		}
		if (syscall_nr >= 0)
			kprintf("Syscall   : nr=%d (in-progress)\n",
				syscall_nr);
		else
			kprintf("Syscall   : not in syscall\n");
	} else {
		kprintf("CPU       : (GS not set — early boot)\n");
	}

	/* Instruction / stack pointers */
	kprintf("\n--- Fault Location ---\n");
	kprintf("RIP       : 0x%016llx\n", rip);
	kprintf("RSP       : 0x%016llx\n", rsp);
	kprintf("RFLAGS    : 0x%016llx\n", rflags);
	kprintf("CS        : 0x%04llx\n", cs);

	/* Page-fault specific info */
	if (int_no == 14) {
		kprintf("\n--- Page Fault ---\n");
		oops_pf_decode(err_code, cr2);
	} else if (err_code) {
		kprintf("Error Code: 0x%016llx\n", err_code);
	}

	/* Full register dump */
	kprintf("\n--- Registers ---\n");
	kprintf("RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx\n",
		regs[REGS_RAX], regs[REGS_RBX], regs[REGS_RCX]);
	kprintf("RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx\n",
		regs[REGS_RDX], regs[REGS_RSI], regs[REGS_RDI]);
	kprintf("RBP: 0x%016llx  RSP: 0x%016llx\n", rbp, rsp);
	kprintf("R8 : 0x%016llx  R9 : 0x%016llx  R10: 0x%016llx\n",
		regs[REGS_R8], regs[REGS_R9], regs[REGS_R10]);
	kprintf("R11: 0x%016llx  R12: 0x%016llx  R13: 0x%016llx\n",
		regs[REGS_R11], regs[REGS_R12], regs[REGS_R13]);
	kprintf("R14: 0x%016llx  R15: 0x%016llx\n", regs[REGS_R14],
		regs[REGS_R15]);
	kprintf("CR2: 0x%016llx  CR3: 0x%016llx  CR4: 0x%016llx\n", cr2, cr3,
		cr4);

	/* Faulting-instruction bytes (post-mortem disassembly) */
	oops_rip_bytes(rip);

	/* Stack trace */
	oops_stack_trace(rbp, rip);

	/* Scheduler state */
	kprintf("\n--- Scheduler State ---\n");
	sched_print_tasks();

	kprintf("\n============================================================\n");
	kprintf("System halted.\n");
	console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

	/* All printing done — NOW safe to stop the other CPUs.  Doing this
     * earlier would risk console_lock being held by a CPU we just parked,
     * causing kprintf to deadlock and the system to hang silently.
     * Best-effort: skip if SMP isn't initialised.  Note we keep this
     * CPU's oops-writer claim — we are about to halt forever, releasing
     * it would just let the next faulting CPU print on top of us. */
	if (sched_is_smp()) {
		smp_halt_others();
	}

	/* Halt this CPU.  exception_handler's epilogue will spin on hlt
     * with IRQs disabled too, but make it explicit here so the function
     * is self-contained — there are kernel paths that may call
     * kernel_oops() directly in the future. */
	for (;;)
		__asm__ volatile("cli; hlt");
}

void panic(const char *fmt, ...)
{
	__asm__ volatile("cli" ::: "memory");
	char msg[256];
	va_list ap;
	__builtin_va_start(ap, fmt);
	kvsnprintf(msg, sizeof(msg), fmt, ap);
	__builtin_va_end(ap);

	console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
	kprintf("\n============================================================\n");
	kprintf("KERNEL PANIC: %s\n", msg);
	kprintf("============================================================\n");
	console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

	for (;;) {
		__asm__ volatile("hlt");
	}
}

#ifdef CRASH_VERBOSE
static void report_userspace_crash_detailed(task_t *cur, uint64_t *regs,
					    int signum, const char *signame,
					    uint64_t cr2, uint64_t int_no)
{
	(void)signum;
	uint64_t err_code = regs[REGS_ERRC];
	uint64_t rip = regs[REGS_RIP];
	uint64_t rsp = regs[REGS_RSP];
	uint64_t rbp = regs[REGS_RBP];

	const char *reason = "unknown";
	const char *access = "-";
	if (int_no == 14) {
		reason = (err_code & 1) ? "protection violation" :
					  "page not present";
		if (err_code & 0x10)
			access = "exec";
		else if (err_code & 0x2)
			access = "write";
		else
			access = "read";
	} else if (int_no == 6) {
		reason = "illegal instruction";
	} else if (int_no == 0) {
		reason = "divide by zero";
	} else if (int_no == 4) {
		reason = "integer overflow";
	} else if (int_no == 13) {
		reason = "general protection fault";
	} else if (int_no == 17) {
		reason = "alignment check";
	}

	task_tty_printf(cur, "\n========================================\n");
	task_tty_printf(cur, "USERSPACE CRASH\n\n");
	task_tty_printf(cur, "Process:   %s\n", cur ? cur->comm : "?");
	task_tty_printf(cur, "PID:       %d\n", cur ? cur->tgid : -1);
	task_tty_printf(cur, "Thread:    %d\n", cur ? (int)cur->id : -1);
	task_tty_printf(cur, "CPU:       %u\n\n", this_cpu_id());
	task_tty_printf(cur, "Signal:    %s\n", signame);
	task_tty_printf(cur, "Reason:    %s\n", reason);
	if (int_no == 14)
		task_tty_printf(cur, "Fault VA:  %016llx\n", cr2);
	task_tty_printf(cur, "Access:    %s\n", access);
	task_tty_printf(cur, "Mode:      user\n\n");
	/* err_code is the smoking gun for #GP / #PF.  For #GP: zero ⇒
     * instruction-level cause (SSE alignment, non-canonical operand,
     * privileged instruction); non-zero ⇒ segment selector index +
     * EXT/IDT/TI bits.  For #PF: bit 0 P, bit 1 W, bit 2 U, bit 3 RSVD,
     * bit 4 I/D, bit 5 PK, bit 15 SGX. */
	task_tty_printf(cur, "err_code:  %016llx\n", err_code);
	task_tty_printf(cur, "RIP:       %016llx\n", rip);
	task_tty_printf(cur, "RSP:       %016llx\n", rsp);
	task_tty_printf(cur, "RBP:       %016llx\n\n", rbp);
	task_tty_printf(cur, "Registers:\n");
	task_tty_printf(cur, "RAX: %016llx  RBX: %016llx\n", regs[REGS_RAX],
			regs[REGS_RBX]);
	task_tty_printf(cur, "RCX: %016llx  RDX: %016llx\n", regs[REGS_RCX],
			regs[REGS_RDX]);
	task_tty_printf(cur, "RSI: %016llx  RDI: %016llx\n", regs[REGS_RSI],
			regs[REGS_RDI]);
	task_tty_printf(cur, "R8:  %016llx  R9:  %016llx\n", regs[REGS_R8],
			regs[REGS_R9]);
	task_tty_printf(cur, "R10: %016llx  R11: %016llx\n", regs[REGS_R10],
			regs[REGS_R11]);
	task_tty_printf(cur, "R12: %016llx  R13: %016llx\n", regs[REGS_R12],
			regs[REGS_R13]);
	task_tty_printf(cur, "R14: %016llx  R15: %016llx\n", regs[REGS_R14],
			regs[REGS_R15]);
	task_tty_printf(cur, "RFLAGS: %016llx\n", regs[REGS_RFLAGS]);

	/* User-segment / TLS state.  If FS_BASE or GS_BASE got clobbered to a
     * non-canonical value, every TLS load via %fs: from userspace would
     * #GP — and #GP just after sysret is exactly the symptom of a bad
     * FS_BASE.  Reading these MSRs is privileged and safe in kernel. */
	{
		uint64_t fs_base = 0, gs_base = 0, kgs_base = 0;
		uint32_t lo, hi;
		__asm__ volatile("rdmsr"
				 : "=a"(lo), "=d"(hi)
				 : "c"(0xC0000100u));
		fs_base = ((uint64_t)hi << 32) | lo;
		__asm__ volatile("rdmsr"
				 : "=a"(lo), "=d"(hi)
				 : "c"(0xC0000101u));
		gs_base = ((uint64_t)hi << 32) | lo;
		__asm__ volatile("rdmsr"
				 : "=a"(lo), "=d"(hi)
				 : "c"(0xC0000102u));
		kgs_base = ((uint64_t)hi << 32) | lo;
		task_tty_printf(cur, "FS_BASE:        %016llx%s\n", fs_base,
				((fs_base >> 47) != 0 &&
				 (fs_base >> 47) != 0x1FFFFULL) ?
					"  (NON-CANONICAL!)" :
					"");
		task_tty_printf(cur, "GS_BASE:        %016llx\n", gs_base);
		task_tty_printf(cur, "KERNEL_GS_BASE: %016llx\n", kgs_base);
		if (cur)
			task_tty_printf(cur, "task->fs_base:  %016llx\n",
					cur->fs_base);
	}

	/* Bytes around RIP for post-mortem disassembly.  Only emitted in a full
     * DEBUG build: without the DWARF symbols and unstripped ELF that DEBUG=1
     * preserves, the raw bytes cannot be mapped back to source and are just
     * noise — so a CRASH_VERBOSE-only (production-like) build skips them.
     * Guarded by mm_user_addr_mapped so we don't take a nested #PF inside the
     * crash handler.  Goes via task_tty_printf so output lands in the same
     * terminal as the rest of the report. */
#ifdef DEBUG
	if ((rip >> 47) == 0 && mm_user_addr_mapped(rip - 16, 32)) {
		/* Snapshot the user bytes into a kernel buffer first, inside a
	     * tight SMAP window.  Reading the user page directly from the
	     * printf argument list faulted under SMAP (supervisor read of a
	     * user page with AC clear) and crashed the crash handler
	     * recursively, destroying the original report. */
		uint8_t b[32];
		const uint8_t *p = (const uint8_t *)(rip - 16);
		smap_disable();
		for (int i = 0; i < 32; i++)
			b[i] = p[i];
		smap_enable();
		task_tty_printf(cur,
				"\nBytes around RIP (RIP-16 .. RIP+15):\n");
		for (int row = 0; row < 2; row++) {
			uint64_t base = rip - 16 + row * 16;
			task_tty_printf(
				cur,
				"  %016llx: %02x %02x %02x %02x %02x %02x %02x %02x"
				" %02x %02x %02x %02x %02x %02x %02x %02x\n",
				base, b[row * 16 + 0], b[row * 16 + 1],
				b[row * 16 + 2], b[row * 16 + 3],
				b[row * 16 + 4], b[row * 16 + 5],
				b[row * 16 + 6], b[row * 16 + 7],
				b[row * 16 + 8], b[row * 16 + 9],
				b[row * 16 + 10], b[row * 16 + 11],
				b[row * 16 + 12], b[row * 16 + 13],
				b[row * 16 + 14], b[row * 16 + 15]);
		}
		task_tty_printf(
			cur,
			"  (the byte at RIP is at offset 16 of the first row)\n");
	} else {
		task_tty_printf(cur,
				"\nBytes around RIP: <not safely readable>\n");
	}

	/* User stack snapshot: the top 16 qwords at RSP.  Return addresses
	 * here reveal where a wild jump or corrupted return came from —
	 * without this, a crash at a bogus RIP leaves no trail.  Same SMAP
	 * discipline as the RIP bytes: snapshot under a tight STAC window,
	 * print from the kernel copy. */
	if ((rsp >> 47) == 0 && (rsp & 7) == 0 &&
	    mm_user_addr_mapped(rsp, 128)) {
		uint64_t q[16];
		const uint64_t *up = (const uint64_t *)rsp;
		smap_disable();
		for (int i = 0; i < 16; i++)
			q[i] = up[i];
		smap_enable();
		task_tty_printf(cur, "\nStack (16 qwords at RSP):\n");
		for (int i = 0; i < 16; i += 2)
			task_tty_printf(cur,
					"  %016llx: %016llx %016llx\n",
					rsp + (uint64_t)i * 8, q[i], q[i + 1]);
	} else {
		task_tty_printf(cur, "\nStack: <not safely readable>\n");
	}
#endif /* DEBUG */

	task_tty_printf(
		cur, "\nMemory map (mmap regions tracked by syscall layer):\n");
	if (cur) {
		mmap_region_t *regions;
		int count;
		if (cur->mm) {
			regions = cur->mm->mmap_regions;
			count = cur->mm->mmap_count;
		} else {
			regions = cur->mmap_regions;
			count = TASK_MAX_MMAP;
		}
		for (int i = 0; i < count; i++) {
			mmap_region_t *r = &regions[i];
			if (!r->in_use)
				continue;
			char perms[5];
			perms[0] = (r->prot & 0x1) ? 'r' : '-';
			perms[1] = (r->prot & 0x2) ? 'w' : '-';
			perms[2] = (r->prot & 0x4) ? 'x' : '-';
			perms[3] = '-';
			perms[4] = '\0';
			task_tty_printf(cur, "  %016llx-%016llx %s\n", r->start,
					r->start + r->length, perms);
		}
	}

	/* Page-table walk of the faulting address — gives the definitive
     * mapping state (present / writable / user / NX) for cr2.
     *
     * Goes through task_tty_printf so it lands in the same terminal as
     * the rest of the crash report.  The PTY-slave write path is a
     * non-blocking ring enqueue and the faulting (slave-side) thread
     * does not normally hold pty->lock, so this is safe from #PF
     * context.
     *
     * Each level checks _PAGE_PSE (bit 7): a PDPT entry with PSE is a
     * 1 GB huge page (terminal), not a PD pointer; a PD entry with PSE
     * is a 2 MB huge page (terminal), not a PT pointer.  Walking
     * "through" a huge entry would treat the mapped data page as a
     * page table and dereference garbage physical addresses → nested
     * fault → triple fault → silent freeze. */
	if (int_no == 14 && cur && cur->pml4) {
		uint64_t va = cr2 & ~0xFFFULL;
		uint64_t pml4i = (va >> 39) & 0x1FF;
		uint64_t pdpti = (va >> 30) & 0x1FF;
		uint64_t pdi = (va >> 21) & 0x1FF;
		uint64_t pti = (va >> 12) & 0x1FF;
		uint64_t pml4e = cur->pml4[pml4i];
		task_tty_printf(cur, "\nPage table walk for VA %016llx:\n",
				cr2);
		task_tty_printf(cur, "  PML4[%lu] = %016llx %s\n", pml4i, pml4e,
				(pml4e & 1) ? "present" : "NOT PRESENT");
		if (pml4e & 1) {
			uint64_t pdpt_phys = pml4e & 0x000FFFFFFFFFF000ULL;
			uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
			uint64_t pdpte = pdpt[pdpti];
			int pdpt_huge = (pdpte & 0x80) != 0;
			task_tty_printf(cur, "  PDPT[%lu] = %016llx %s%s\n",
					pdpti, pdpte,
					(pdpte & 1) ? "present" : "NOT PRESENT",
					pdpt_huge ? " 1G-huge" : "");
			if ((pdpte & 1) && !pdpt_huge) {
				uint64_t pd_phys =
					pdpte & 0x000FFFFFFFFFF000ULL;
				uint64_t *pd =
					(uint64_t *)phys_to_virt(pd_phys);
				uint64_t pde = pd[pdi];
				int pd_huge = (pde & 0x80) != 0;
				task_tty_printf(
					cur, "  PD[%lu]   = %016llx %s%s\n",
					pdi, pde,
					(pde & 1) ? "present" : "NOT PRESENT",
					pd_huge ? " 2M-huge" : "");
				if ((pde & 1) && !pd_huge) {
					uint64_t pt_phys =
						pde & 0x000FFFFFFFFFF000ULL;
					uint64_t *pt = (uint64_t *)phys_to_virt(
						pt_phys);
					uint64_t pte = pt[pti];
					task_tty_printf(
						cur,
						"  PT[%lu]   = %016llx %s%s%s%s%s\n",
						pti, pte,
						(pte & 1) ? " present" :
							    " NOT-PRESENT",
						(pte & 2) ? " W" : " R-only",
						(pte & 4) ? " U" : " S-only",
						(pte & 0x200) ? " COW" : "",
						(pte & (1ULL << 63)) ? " NX" :
								       "");
				}
			}
		}
	}
	task_tty_printf(cur, "\nSYSTEM ACTION:\n  process terminated\n");
	task_tty_printf(cur, "========================================\n");
}
#endif /* CRASH_VERBOSE */

static void report_userspace_crash(task_t *cur, uint64_t *regs, int signum,
				   const char *signame, uint64_t cr2,
				   uint64_t int_no)
{
#ifdef CRASH_VERBOSE
	report_userspace_crash_detailed(cur, regs, signum, signame, cr2,
					int_no);
#else
	task_tty_printf(cur, "User process %d killed by %s\n",
			cur ? (int)cur->id : -1, signame);
#endif
}

/*
 * The single "return from an interrupt" work point.  Every interrupt and
 * exception path that can hand control back to user mode funnels through here
 * instead of open-coding signal delivery and preemption in each handler, so the
 * two things that must happen on the way out — and their ordering — live in one
 * place:
 *
 *   1. If returning to user mode, deliver a pending signal.  This may rewrite
 *      the frame to enter a handler, or terminate the task outright.
 *   2. Only then, honour a pending reschedule.
 *
 * Parameters:
 *   regs          - saved GPR + iret frame (REGS_* layout, alias of
 *                   interrupt_frame_t).
 *   allow_preempt - honour need_resched.  The IRQ and IPI paths pass 1; the
 *                   exception path passes 0, because a fault may have occurred
 *                   inside an atomic kernel section that must not be preempted.
 */
static void irqentry_exit(uint64_t *regs, int allow_preempt)
{
	int from_user = (regs[REGS_CS] & 3) == 3;

	// Signal delivery only makes sense when returning to user mode.
	if (from_user) {
		task_t *cur = sched_current();
		if (cur && cur->privilege == TASK_USER && signal_pending(cur)) {
			signal_deliver_irq(cur, (interrupt_frame_t *)regs);
			// A fatal default action marked us exited: never IRET to
			// user as a zombie.  Schedule away for good — actively,
			// because a passive hlt would rely on a preemption that
			// may never arrive on an otherwise-idle CPU.
			if (cur->has_exited || cur->state == TASK_ZOMBIE) {
				for (;;) {
					sched_schedule();
					__asm__ volatile("cli; hlt");
				}
			}
		}
	}

	// Reschedule only from a preemptible context: returning to user, or the
	// interrupted kernel code had IRQs enabled (i.e. not inside an IRQ-off
	// critical section such as a held spinlock).  sched_preempt() applies
	// its own in_context_switch / bootstrap / try-lock guards on top.
	if (allow_preempt && sched_need_resched() &&
	    (from_user || (regs[REGS_RFLAGS] & 0x200)))
		sched_preempt((interrupt_frame_t *)regs);
}

void exception_handler(uint64_t *regs)
{
	uint64_t int_no = regs[REGS_INTNO];
	uint64_t err_code = regs[REGS_ERRC];
	uint64_t rip = regs[REGS_RIP];
	uint64_t cs = regs[REGS_CS];
	int user_mode = (cs & 0x3) == 0x3;

	/* INT 2 (NMI): if this NMI is a diagnostic probe armed by another CPU
     * via smp_tlb_shootdown_sync's timeout path, record this CPU's state
     * and return.  Only real, un-armed NMIs fall through to the
     * kernel_oops path below. */
	if (int_no == 2) {
		if (smp_nmi_capture_record(regs))
			return;
		/* Real NMI in kernel mode: fall through. */
	}

	if (int_no == 14) {
		uint64_t cr2;
		__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
		// Demand paging: not-present fault (err bit 0 clear) on a user
		// address.  Like COW below, this fires from BOTH CPU modes —
		// kernel code touches lazy user pages via copy_to_user etc.
		//
		// CRITICAL: the exception gate cleared IF.  File page-in
		// sleeps on disk I/O (ext4 → USB, whose completions arrive by
		// interrupt), so re-enable interrupts for the handler IF the
		// interrupted context had them enabled (always true for
		// user-mode faults).  A context that faulted with IRQs off
		// (spinlock held) stays off — its faults can only be lazy
		// zero-fill, which never sleeps.
		if (!(err_code & 0x1) && cr2 < 0x8000000000000000ULL) {
			int irqs_were_on =
				(regs[REGS_RFLAGS] & 0x200) != 0;
			if (irqs_were_on)
				__asm__ volatile("sti" ::: "memory");
			int resolved = mm_handle_demand_fault(cr2, !user_mode);
			if (irqs_were_on)
				__asm__ volatile("cli" ::: "memory");
			if (resolved) {
				/* Resolved fault returns to the interrupted
				 * instruction.  Run the common return-to-user
				 * work (deliver a pending signal so a fault-heavy
				 * loop stays killable) but do NOT preempt — a
				 * kernel-mode fault may hold a spinlock. */
				irqentry_exit(regs, 0);
				return;
			}
		}
		// Handle COW faults on user-space addresses (write + present)
		// Note: Kernel code can trigger COW when accessing user pages (copy_to_user etc)
		// so we check the ADDRESS is in user space, not the mode of the fault
		if ((err_code & 0x3) == 0x3 && cr2 < 0x8000000000000000ULL) {
			if (mm_handle_cow_fault(cr2)) {
				irqentry_exit(regs, 0);
				return;
			}
		}

		// Handle page faults in kernel mode when accessing user memory
		// (e.g., during syscall, copy_to_user, copy_from_user)
		// If the faulting address is in user space and we have a user task context,
		// kill the user process instead of panicking the kernel.
		if (!user_mode && cr2 < 0x8000000000000000ULL) {
			task_t *cur = sched_current();
			if (cur && cur->privilege == TASK_USER) {
				report_userspace_crash(cur, regs, SIGSEGV,
						       "SIGSEGV", cr2, 14);
				sched_signal_task(cur, SIGSEGV);
				// Enable interrupts and halt - timer will preempt us to another task
				for (;;) {
					__asm__ volatile("sti; hlt");
				}
			}
		}
	}

	if (user_mode) {
		task_t *cur = sched_current();
		int fault_sig;
		const char *fault_name;
		uint64_t fault_addr = 0;
		switch (int_no) {
		case 14:
			__asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
			fault_sig = SIGSEGV;
			fault_name = "SIGSEGV";
			break;
		case 6:
			fault_sig = SIGILL;
			fault_name = "SIGILL";
			break;
		case 0:
		case 4:
			fault_sig = SIGFPE;
			fault_name = "SIGFPE";
			break;
		case 3:
		case 5:
			fault_sig = SIGTRAP;
			fault_name = "SIGTRAP";
			break;
		case 13:
			fault_sig = SIGSEGV;
			fault_name = "SIGSEGV";
			break;
		case 17:
			fault_sig = SIGBUS;
			fault_name = "SIGBUS";
			break;
		default:
			fault_sig = SIGABRT;
			fault_name = "SIGABRT";
			break;
		}
		report_userspace_crash(cur, regs, fault_sig, fault_name,
				       fault_addr, (int)int_no);
		sched_signal_task(cur, fault_sig);
		/* The old tail parked in sti;hlt unconditionally ("timer will
		 * preempt us away"), assuming the signal is fatal.  When the
		 * process has a user handler installed (or the signal blocked),
		 * a caught fault signal is only deliverable on return to USER
		 * mode — which the park never performs — leaving an immortal
		 * READY task ping-ponging between the run queue and the halt
		 * loop (observed: testlibc thread stranded on CPU0's queue
		 * after a teardown SIGSEGV, hanging teststress's waitpid until
		 * Ctrl+C).  Deliver on THIS exception frame instead. */
		if (cur && !cur->has_exited && cur->state != TASK_ZOMBIE) {
			interrupt_frame_t *frame = (interrupt_frame_t *)regs;
			uint64_t old_rip = frame->rip;
			signal_deliver_irq(cur, frame);
			if (!cur->has_exited && cur->state != TASK_ZOMBIE) {
				if (frame->rip != old_rip) {
					/* Frame redirected — IRET enters the
					 * user handler. */
					return;
				}
				/* Undeliverable fault signal (blocked or
				 * ignored): returning would re-execute the
				 * faulting instruction forever.  Force-exit. */
				cur->exit_code = 128 + fault_sig;
				sched_mark_task_exited(cur, 128 + fault_sig);
			}
		}
		/* Task is dead: park until the timer preempts us away; the
		 * zombie is reaped via the deferred_zombie path. */
		for (;;) {
			__asm__ volatile("sti; hlt");
		}
	}

	/* Kernel-mode fatal exception — print Oops and halt */
	{
		const char *exc_name =
			(int_no < 32) ? exception_messages[int_no] : "Unknown";
		char reason[64];
		ksnprintf(reason, sizeof(reason), "%s (INT %llu)", exc_name,
			  int_no);
		kernel_oops(reason, regs);
	}

	for (;;) {
		__asm__ volatile("hlt");
	}
}

volatile uint64_t g_irq0_count = 0;
volatile uint64_t g_irq1_count = 0;
volatile uint64_t g_irq12_count = 0;
volatile uint64_t g_spurious_irq_count = 0;
volatile uint64_t g_total_irq_count = 0;

// Read the PIC In-Service Register to check for spurious IRQs
static inline uint8_t pic_read_isr(void)
{
	outb(PIC1_CMD, 0x0B); // Read ISR command
	return inb(PIC1_CMD);
}

static inline uint8_t pic2_read_isr(void)
{
	outb(PIC2_CMD, 0x0B); // Read ISR command
	return inb(PIC2_CMD);
}

void irq_handler(uint64_t *regs)
{
	BUG_ON(regs == NULL);
	uint64_t int_no = regs[REGS_INTNO];
	WARN_RATELIMIT(int_no >= 256, "irq_handler: bogus int_no=%lu", int_no);
	uint8_t irq = (uint8_t)(int_no - 32);

	g_total_irq_count++;

	// Legacy INTx dispatch for E1000 / e1000e NICs (when MSI is not
	// available, e.g. VirtualBox).  These MUST be checked BEFORE any
	// fixed-vector branches below, because the IOAPIC may map the NIC's
	// PCI INTA pin to a GSI whose vector collides with one we reserve
	// for an MSI device (e.g. VBox routes 82545EM INTA -> GSI 17 ->
	// vector 49, which is XHCI_MSI_VECTOR_2).  The level-triggered NIC
	// line would otherwise be ACK'd by the wrong handler and assert
	// forever, hanging the system in an IRQ storm.
	{
		extern int g_e1000_initialized;
		extern int g_e1000_legacy_irq;
		if (g_e1000_initialized && g_e1000_legacy_irq >= 0 &&
		    irq == g_e1000_legacy_irq) {
			e1000_irq_handler();
			softirq_drain(); // process queued RX skbs in deferred context
			return; // e1000_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_e1000e_initialized;
		extern int g_e1000e_legacy_irq;
		extern void e1000e_irq_handler(void);
		if (g_e1000e_initialized && g_e1000e_legacy_irq >= 0 &&
		    irq == g_e1000e_legacy_irq) {
			e1000e_irq_handler();
			softirq_drain();
			return; // e1000e_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_rtl8139_initialized;
		extern int g_rtl8139_legacy_irq;
		extern void rtl8139_irq_handler(void);
		if (g_rtl8139_initialized && g_rtl8139_legacy_irq >= 0 &&
		    irq == g_rtl8139_legacy_irq) {
			rtl8139_irq_handler();
			softirq_drain();
			return; // rtl8139_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_pcnet_initialized;
		extern int g_pcnet_legacy_irq;
		extern void pcnet32_irq_handler(void);
		if (g_pcnet_initialized && g_pcnet_legacy_irq >= 0 &&
		    irq == g_pcnet_legacy_irq) {
			pcnet32_irq_handler();
			softirq_drain();
			return; // pcnet32_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_ne2k_initialized;
		extern int g_ne2k_legacy_irq;
		extern void ne2k_irq_handler(void);
		if (g_ne2k_initialized && g_ne2k_legacy_irq >= 0 &&
		    irq == g_ne2k_legacy_irq) {
			ne2k_irq_handler();
			softirq_drain();
			return; // ne2k_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_vmxnet3_initialized;
		extern int g_vmxnet3_legacy_irq;
		extern void vmxnet3_irq_handler(void);
		if (g_vmxnet3_initialized && g_vmxnet3_legacy_irq >= 0 &&
		    irq == g_vmxnet3_legacy_irq) {
			vmxnet3_irq_handler();
			softirq_drain();
			return; // vmxnet3_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_eepro100_initialized;
		extern int g_eepro100_legacy_irq;
		extern void eepro100_irq_handler(void);
		if (g_eepro100_initialized && g_eepro100_legacy_irq >= 0 &&
		    irq == g_eepro100_legacy_irq) {
			eepro100_irq_handler();
			softirq_drain();
			return; // eepro100_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_igb_initialized;
		extern int g_igb_legacy_irq;
		extern void igb_irq_handler(void);
		if (g_igb_initialized && g_igb_legacy_irq >= 0 &&
		    irq == g_igb_legacy_irq) {
			igb_irq_handler();
			softirq_drain();
			return; // igb_irq_handler calls lapic_eoi()
		}
	}
	{
		extern int g_tulip_initialized;
		extern int g_tulip_legacy_irq;
		extern void tulip_irq_handler(void);
		if (g_tulip_initialized && g_tulip_legacy_irq >= 0 &&
		    irq == g_tulip_legacy_irq) {
			tulip_irq_handler();
			softirq_drain();
			return; // tulip_irq_handler calls lapic_eoi()
		}
	}

	{
		// VMware SVGA II fence/FIFO interrupt (legacy INTx, may share
		// its line): only claim it when the device's IRQ status port
		// was actually asserted.
		extern volatile int g_vmsvga_initialized;
		extern volatile int g_vmsvga_legacy_irq;
		extern int vmsvga2_irq(void);
		if (g_vmsvga_initialized && g_vmsvga_legacy_irq >= 0 &&
		    irq == g_vmsvga_legacy_irq) {
			if (vmsvga2_irq()) {
				lapic_eoi();
				return;
			}
		}
	}

	// MSI vector for xHCI USB — vector 48 (irq == 16 after subtracting IRQ_BASE).
	// MSI bypasses the PIC entirely; requires LAPIC EOI, not PIC EOI.
	if (int_no == XHCI_MSI_VECTOR) {
		extern xhci_controller_t g_xhci;
		if (g_xhci.initialized && g_xhci.irq_enabled) {
			xhci_irq_service(&g_xhci);
		}
		lapic_eoi();
		return; // Do NOT send PIC EOI for MSI interrupts
	}

	// MSI vector for second xHCI USB controller — vector 49.
	if (int_no == XHCI_MSI_VECTOR_2) {
		extern xhci_controller_t g_xhci_hid;
		if (g_xhci_hid.initialized && g_xhci_hid.irq_enabled) {
			xhci_irq_service(&g_xhci_hid);
		}
		lapic_eoi();
		return;
	}

	// (Legacy NIC dispatch was moved above — see top of irq_handler.)

	// MSI/IOAPIC vectors for I2C LPSS controllers (vectors 50-53)
	if (int_no >= 50 && int_no <= 53) {
		i2c_hid_irq_handler((uint8_t)int_no);
		// i2c_hid_irq_handler calls lapic_eoi() internally
		return;
	}

	// GPIO interrupt vectors for I2C HID devices (vectors 54-57)
	if (int_no >= 54 && int_no <= 57) {
		i2c_hid_gpio_irq_handler((uint8_t)int_no);
		// i2c_hid_gpio_irq_handler calls lapic_eoi() internally
		return;
	}

	// ACPI SCI interrupt (vector 58) — level-triggered via IOAPIC
	if (int_no == ACPI_SCI_VECTOR) {
		acpi_sci_dispatch();
		lapic_eoi();
		return;
	}

	// E1000 NIC MSI interrupt (vector 59)
	if (int_no == E1000_MSI_VECTOR) {
		e1000_irq_handler();
		softirq_drain();
		return; // e1000_irq_handler calls lapic_eoi()
	}

	// e1000e NIC MSI interrupt (vector 60)
	if (int_no == E1000E_MSI_VECTOR) {
		extern void e1000e_irq_handler(void);
		e1000e_irq_handler();
		softirq_drain();
		return; // e1000e_irq_handler calls lapic_eoi()
	}

	// vmxnet3 paravirt NIC MSI interrupt (vector 61)
	if (int_no == VMXNET3_MSI_VECTOR) {
		extern void vmxnet3_irq_handler(void);
		vmxnet3_irq_handler();
		softirq_drain();
		return; // vmxnet3_irq_handler calls lapic_eoi()
	}

	// Check for spurious IRQ7 (from master PIC)
	if (irq == 7) {
		uint8_t isr = pic_read_isr();
		if ((isr & 0x80) == 0) {
			// Spurious IRQ7 — PIC ISR bit not set, but LAPIC ISR IS set
			// when delivered via LINT0 ExtINT (virtual wire).  Must clear it.
			g_spurious_irq_count++;
			if (g_lapic_active || lapic_is_available())
				lapic_eoi();
			return;
		}
	}

	// Check for spurious IRQ15 (from slave PIC)
	if (irq == 15) {
		uint8_t isr = pic2_read_isr();
		if ((isr & 0x80) == 0) {
			// Spurious IRQ15 — send EOI to master PIC (cascade) and LAPIC.
			// LAPIC ISR bit IS set via LINT0 ExtINT delivery.
			g_spurious_irq_count++;
			outb(PIC1_CMD,
			     0x20); // EOI to master only (no slave — spurious)
			if (g_lapic_active || lapic_is_available())
				lapic_eoi();
			return;
		}
	}

	// Handle known IRQs
	switch (int_no) {
	case 32: {
		g_irq0_count++;

		timer_irq_handler();
		net_timer_tick();
		// Send EOI before preemption to avoid missing ticks
		pic_send_eoi(irq);

		/* Common return-to-user work: deliver a pending signal (on the
		 * IRETQ frame, not the SYSRET state — timer returns via iretq)
		 * and honour a pending reschedule.  Same code path as every
		 * other interrupt return; see irqentry_exit(). */
		irqentry_exit(regs, 1);
		return; // EOI already sent
	}
	case 33:
		g_irq1_count++;
		/* Drain fully: a single read per IRQ can lose the edge and
             * permanently wedge keyboard+mouse input (see ps2_drain_output). */
		ps2_drain_output();
		break;
	case 34:
		// IRQ2 is cascade - should never fire, just ACK it
		break;
	case 44:
		g_irq12_count++;
		ps2_drain_output();
		break;
	default: {
		// Legacy INTx dispatch for XHCI (when MSI is not available)
		extern xhci_controller_t g_xhci;
		if (g_xhci.initialized && g_xhci.irq_enabled &&
		    !g_xhci.msi_enabled && irq == g_xhci.irq) {
			xhci_irq_service(&g_xhci);
		}
		break;
	}
	}

	// Send EOI after handling the interrupt
	pic_send_eoi(irq);

	// Drain any softirqs raised by this IRQ before returning to the
	// interrupted context.  Runs with interrupts enabled internally so it
	// never delays TLB-shootdown IPIs.
	softirq_drain();

	/* Common return-to-user work — deliver a pending signal / reschedule.
	 * Placed after softirq_drain so a wakeup raised by a softirq (e.g. a
	 * NIC rx that made a polled task runnable) is seen by the preempt
	 * check. */
	irqentry_exit(regs, 1);
}

// ============================================================================
// IPI Handler (called from ipi_common_stub in assembly)
// ============================================================================

void ipi_handler(uint64_t *regs)
{
	uint64_t vector = regs[15]; // Vector number pushed by IPI stub

	switch (vector) {
	case 0xFE: // IPI_RESCHEDULE_VECTOR
		// Mark current task as needing reschedule; the actual signal
		// delivery + preemption happen in the shared irqentry_exit()
		// call at the tail of this handler.  (sched_remove_task sends
		// this IPI to force a zombie off a CPU — the tail preempt is
		// what switches it away, so the sender's spin-wait completes.)
		{
			task_t *cur = sched_current();
			if (cur) {
				sched_set_need_resched(cur);
			}
		}
		lapic_eoi();
		break;

	case 0xFD: // IPI_HALT_VECTOR
		// Halt this CPU
		lapic_eoi();
		__asm__ volatile("cli");
		while (1) {
			__asm__ volatile("hlt");
		}
		break;

	case 0xFC: // IPI_TLB_SHOOTDOWN
		// Invalidate TLB on this CPU and acknowledge
		{
			// Memory barrier to ensure we see all page table updates
			// from the CPU that initiated the shootdown
			__asm__ volatile("mfence" ::: "memory");
			uint64_t cr3;
			__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
			__asm__ volatile("mov %0, %%cr3"
					 :
					 : "r"(cr3)
					 : "memory");
			smp_tlb_shootdown_ack();
		}
		lapic_eoi();
		break;

	case 0xFF: // LAPIC_SPURIOUS_VECTOR
		// Spurious interrupt - do NOT send EOI
		break;

	default:
		// Unknown IPI vector - just send EOI
		lapic_eoi();
		break;
	}

	/* Common return-to-user work — one shared path for every IPI vector
	 * that returns (the halt vector loops forever and never reaches here).
	 * Delivers a pending signal on return to user and performs the pending
	 * reschedule (need_resched set by the 0xFE case above, or by a wakeup). */
	irqentry_exit(regs, 1);
}

void interrupts_init()
{
	extern void gdt_init();
	gdt_init();
	tss_init();
	gdt_install_tss();
	pic_init();
	imcr_route_to_pic();
	idt_init();
	kprintf("Interrupt system initialized\n");
}

void tss_init()
{
	BUILD_BUG_ON(IST_TOTAL_SIZE < PAGE_SIZE + 512);
	my_memset(&tss, 0, sizeof(tss));
	tss.rsp0 = (uint64_t)(interrupt_stack + sizeof(interrupt_stack));

	// Configure IST entries for critical exceptions
	// IST1: Double Fault - MUST have separate stack to avoid triple fault
	tss.ist1 = (uint64_t)(bsp_ist1_stack + IST_TOTAL_SIZE);
	// IST2: NMI - can interrupt at any time, needs guaranteed stack
	tss.ist2 = (uint64_t)(bsp_ist2_stack + IST_TOTAL_SIZE);
	// IST3: Machine Check - similar to NMI
	tss.ist3 = (uint64_t)(bsp_ist3_stack + IST_TOTAL_SIZE);

	tss.iopb_offset = sizeof(tss);

	// Install guard pages on all BSP interrupt/IST stacks.
	// The guard page is the first PAGE_SIZE bytes of each array (lowest address).
	// Any stack overflow will immediately fault on the guard page.
	mm_mark_guard_page((uint64_t)interrupt_stack);
	mm_mark_guard_page((uint64_t)bsp_ist1_stack);
	mm_mark_guard_page((uint64_t)bsp_ist2_stack);
	mm_mark_guard_page((uint64_t)bsp_ist3_stack);

	kprintf("TSS initialized with IST entries\n");
}

void gdt_install_tss()
{
	extern void gdt_install_tss_real(uint64_t tss_base, uint64_t tss_size);
	gdt_install_tss_real((uint64_t)&tss, sizeof(tss) - 1);
}

void tss_set_kernel_stack(uint64_t stack_top)
{
	// In SMP mode, set the per-CPU TSS RSP0
	if (sched_is_smp()) {
		uint32_t cpu = this_cpu_id();
		if (cpu == 0) {
			tss.rsp0 = stack_top;
		} else if (cpu < MAX_CPUS_TSS) {
			ap_tss[cpu].rsp0 = stack_top;
		}
	} else {
		tss.rsp0 = stack_top;
	}
}

uint64_t tss_get_kernel_stack(void)
{
	if (sched_is_smp()) {
		uint32_t cpu = this_cpu_id();
		if (cpu == 0) {
			return tss.rsp0;
		} else if (cpu < MAX_CPUS_TSS) {
			return ap_tss[cpu].rsp0;
		}
	}
	return tss.rsp0;
}

// Initialize TSS for an Application Processor
// Each AP needs its own TSS with a unique RSP0 for kernel entry
// CRITICAL: We must NOT call gdt_flush() here because it reloads GS with
// selector 0x10 (data segment), which zeroes the GS base and destroys the
// per-CPU data pointer set up by percpu_init_cpu().
// Instead, we directly update the GDT TSS entry and do LTR.
// This is safe because APs start serially (one at a time in smp_boot_aps).
void tss_init_ap(uint32_t cpu_id)
{
	BUG_ON(cpu_id >=
	       MAX_CPUS_TSS); /* cpu_id exceeds ap_tss[] array bounds: MAX_CPUS_TSS must be increased */
	if (cpu_id == 0 || cpu_id >= MAX_CPUS_TSS) {
		return;
	}

	// Initialize the per-CPU TSS
	my_memset(&ap_tss[cpu_id], 0, sizeof(struct tss_entry));
	ap_tss[cpu_id].rsp0 = (uint64_t)(ap_interrupt_stacks[cpu_id] +
					 sizeof(ap_interrupt_stacks[cpu_id]));

	// Configure IST entries for this AP - each AP MUST have its own IST stacks
	// to avoid stack corruption when handling critical exceptions on different CPUs
	ap_tss[cpu_id].ist1 = (uint64_t)(ap_ist1_stacks[cpu_id] +
					 IST_TOTAL_SIZE); // Double Fault
	ap_tss[cpu_id].ist2 =
		(uint64_t)(ap_ist2_stacks[cpu_id] + IST_TOTAL_SIZE); // NMI
	ap_tss[cpu_id].ist3 = (uint64_t)(ap_ist3_stacks[cpu_id] +
					 IST_TOTAL_SIZE); // Machine Check

	ap_tss[cpu_id].iopb_offset = sizeof(struct tss_entry);

	// Install guard pages on this AP's interrupt/IST stacks.
	mm_mark_guard_page((uint64_t)ap_interrupt_stacks[cpu_id]);
	mm_mark_guard_page((uint64_t)ap_ist1_stacks[cpu_id]);
	mm_mark_guard_page((uint64_t)ap_ist2_stacks[cpu_id]);
	mm_mark_guard_page((uint64_t)ap_ist3_stacks[cpu_id]);

	// Directly update the shared GDT's TSS entry (slots 5-6) without gdt_flush.
	// The GDT is already loaded via lgdt in ap_entry(); we just need to update
	// the in-memory TSS descriptor and then LTR to load it into this CPU's TR.
	//
	// Access the GDT through the pointer returned by gdt_get_descriptor().
	// The gdt_ptr structure: { uint16_t limit; uint64_t base; }
	struct {
		uint16_t limit;
		uint64_t base;
	} __attribute__((packed)) *gdtp = gdt_get_descriptor();

	// GDT entry is 8 bytes each; TSS descriptor is at entries 5-6 (offset 40)
	uint8_t *gdt_base = (uint8_t *)(gdtp->base);
	uint8_t *tss_desc = gdt_base + (5 * 8); // Entry 5

	uint64_t base = (uint64_t)&ap_tss[cpu_id];
	uint64_t limit = sizeof(struct tss_entry) - 1;

	// First 8 bytes (standard TSS descriptor format)
	tss_desc[0] = limit & 0xFF; // Limit low byte 0
	tss_desc[1] = (limit >> 8) & 0xFF; // Limit low byte 1
	tss_desc[2] = base & 0xFF; // Base low byte 0
	tss_desc[3] = (base >> 8) & 0xFF; // Base low byte 1
	tss_desc[4] = (base >> 16) & 0xFF; // Base middle
	tss_desc[5] =
		0x89; // Access: Present, Ring 0, TSS Available (not Busy!)
	tss_desc[6] = (limit >> 16) & 0x0F; // Granularity + limit high nibble
	tss_desc[7] = (base >> 24) & 0xFF; // Base high byte

	// Second 8 bytes (upper 32 bits of base for 64-bit TSS)
	tss_desc[8] = (base >> 32) & 0xFF;
	tss_desc[9] = (base >> 40) & 0xFF;
	tss_desc[10] = (base >> 48) & 0xFF;
	tss_desc[11] = (base >> 56) & 0xFF;
	tss_desc[12] = 0;
	tss_desc[13] = 0;
	tss_desc[14] = 0;
	tss_desc[15] = 0;

	// Memory barrier to ensure GDT writes are visible before LTR
	__asm__ volatile("mfence" ::: "memory");

	// Load Task Register (selector 0x28 = entry 5)
	__asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));
}

// Get IDT descriptor for AP initialization
void *interrupts_get_idt_descriptor(void)
{
	return &idt_desc;
}
