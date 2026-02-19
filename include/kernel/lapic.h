// LikeOS-64 - Local APIC (LAPIC) Support for SMP
// Provides per-CPU interrupt controller, timer, and IPI support

#ifndef _KERNEL_LAPIC_H_
#define _KERNEL_LAPIC_H_

#include "types.h"

// ============================================================================
// LAPIC Register Offsets (from LAPIC base address)
// ============================================================================
#define LAPIC_ID            0x020   // Local APIC ID
#define LAPIC_VERSION       0x030   // Local APIC Version
#define LAPIC_TPR           0x080   // Task Priority Register
#define LAPIC_APR           0x090   // Arbitration Priority Register
#define LAPIC_PPR           0x0A0   // Processor Priority Register
#define LAPIC_EOI           0x0B0   // End Of Interrupt
#define LAPIC_RRD           0x0C0   // Remote Read Register
#define LAPIC_LDR           0x0D0   // Logical Destination Register
#define LAPIC_DFR           0x0E0   // Destination Format Register
#define LAPIC_SVR           0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_ISR_BASE      0x100   // In-Service Register (8 x 32-bit)
#define LAPIC_TMR_BASE      0x180   // Trigger Mode Register (8 x 32-bit)
#define LAPIC_IRR_BASE      0x200   // Interrupt Request Register (8 x 32-bit)
#define LAPIC_ESR           0x280   // Error Status Register
#define LAPIC_LVT_CMCI      0x2F0   // LVT Corrected Machine Check Interrupt
#define LAPIC_ICR_LOW       0x300   // Interrupt Command Register (low)
#define LAPIC_ICR_HIGH      0x310   // Interrupt Command Register (high)
#define LAPIC_LVT_TIMER     0x320   // LVT Timer
#define LAPIC_LVT_THERMAL   0x330   // LVT Thermal Sensor
#define LAPIC_LVT_PMC       0x340   // LVT Performance Monitoring Counters
#define LAPIC_LVT_LINT0     0x350   // LVT LINT0
#define LAPIC_LVT_LINT1     0x360   // LVT LINT1
#define LAPIC_LVT_ERROR     0x370   // LVT Error
#define LAPIC_TIMER_ICR     0x380   // Timer Initial Count Register
#define LAPIC_TIMER_CCR     0x390   // Timer Current Count Register
#define LAPIC_TIMER_DCR     0x3E0   // Timer Divide Configuration Register

// ============================================================================
// LAPIC Constants
// ============================================================================

// Spurious Vector Register bits
#define LAPIC_SVR_ENABLE    0x100   // APIC Software Enable

// LVT Timer modes
#define LAPIC_TIMER_ONESHOT     0x00000000  // One-shot mode
#define LAPIC_TIMER_PERIODIC    0x00020000  // Periodic mode
#define LAPIC_TIMER_TSC_DEADLINE 0x00040000 // TSC-Deadline mode (if supported)

// LVT common bits
#define LAPIC_LVT_MASKED    0x00010000  // Interrupt masked

// Delivery modes for ICR
#define LAPIC_ICR_FIXED         0x00000000
#define LAPIC_ICR_LOWEST        0x00000100
#define LAPIC_ICR_SMI           0x00000200
#define LAPIC_ICR_NMI           0x00000400
#define LAPIC_ICR_INIT          0x00000500
#define LAPIC_ICR_STARTUP       0x00000600

// ICR destination modes
#define LAPIC_ICR_PHYSICAL      0x00000000
#define LAPIC_ICR_LOGICAL       0x00000800

// ICR level
#define LAPIC_ICR_DEASSERT      0x00000000
#define LAPIC_ICR_ASSERT        0x00004000

// ICR trigger mode
#define LAPIC_ICR_EDGE          0x00000000
#define LAPIC_ICR_LEVEL         0x00008000

// ICR destination shorthand
#define LAPIC_ICR_NO_SHORTHAND  0x00000000
#define LAPIC_ICR_SELF          0x00040000
#define LAPIC_ICR_ALL_INCL_SELF 0x00080000
#define LAPIC_ICR_ALL_EXCL_SELF 0x000C0000

// ICR delivery status
#define LAPIC_ICR_PENDING       0x00001000

// Timer divide values (for LAPIC_TIMER_DCR)
#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

// Interrupt vectors for LAPIC
#define LAPIC_TIMER_VECTOR      0x20    // Timer interrupt (same as legacy IRQ0)
#define LAPIC_ERROR_VECTOR      0x2F    // LAPIC error interrupt
#define LAPIC_SPURIOUS_VECTOR   0xFF    // Spurious interrupt
#define IPI_RESCHEDULE_VECTOR   0xFE    // IPI: Reschedule
#define IPI_HALT_VECTOR         0xFD    // IPI: Halt CPU
#define IPI_TLB_SHOOTDOWN       0xFC    // IPI: TLB shootdown

// ============================================================================
// LAPIC Functions
// ============================================================================

// Initialize the Local APIC on the current CPU
void lapic_init(void);

// Read/write LAPIC registers
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// Get the current CPU's APIC ID
uint32_t lapic_get_id(void);

// Send End-Of-Interrupt to LAPIC
void lapic_eoi(void);

// ============================================================================
// LAPIC Timer Functions
// ============================================================================

// Calibrate LAPIC timer using PIT
void lapic_timer_calibrate(void);

// Start LAPIC timer in periodic mode
// frequency: desired interrupt frequency in Hz
void lapic_timer_start(uint32_t frequency);

// Stop LAPIC timer
void lapic_timer_stop(void);

// Get LAPIC timer ticks per second (after calibration)
uint64_t lapic_timer_get_frequency(void);

// ============================================================================
// Inter-Processor Interrupt (IPI) Functions
// ============================================================================

// Send IPI to a specific CPU by APIC ID
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);

// Send INIT IPI to a CPU
void lapic_send_init(uint32_t apic_id);

// Send SIPI (Startup IPI) to a CPU
// The vector should be the physical address >> 12 (i.e., 0x08 for 0x8000)
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);

// Send IPI to all CPUs except self
void lapic_send_ipi_all_excl_self(uint32_t vector);

// Send IPI to all CPUs including self
void lapic_send_ipi_all_incl_self(uint32_t vector);

// Send IPI to self
void lapic_send_ipi_self(uint32_t vector);

// Wait for IPI delivery to complete
void lapic_ipi_wait(void);

// ============================================================================
// SMP Support Functions
// ============================================================================

// Check if LAPIC is available (via CPUID)
bool lapic_is_available(void);

// Get the LAPIC base address from MSR
uint64_t lapic_get_base(void);

// Enable the LAPIC
void lapic_enable(void);

// Disable the LAPIC
void lapic_disable(void);

// Set up logical destination for this CPU
void lapic_setup_logical_dest(uint32_t logical_id);

#endif // _KERNEL_LAPIC_H_
