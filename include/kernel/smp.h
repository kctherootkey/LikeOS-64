// LikeOS-64 - SMP (Symmetric Multi-Processing) Support
// AP startup, CPU synchronization, and SMP management

#ifndef _KERNEL_SMP_H_
#define _KERNEL_SMP_H_

#include "types.h"
#include "percpu.h"

// ============================================================================
// SMP Debug
// ============================================================================

// Set to 1 to enable verbose ACPI/PERCPU/SMP/LAPIC boot messages
#define SMP_DEBUG 0

#if SMP_DEBUG
#define smp_dbg(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#else
#define smp_dbg(fmt, ...) do {} while(0)
#endif

// ============================================================================
// SMP Constants
// ============================================================================

// Default AP trampoline location (must be in low memory, below 1MB)
// The actual address is provided by bootloader via boot_info.smp_trampoline_addr
// Fallback to 0x8000 if bootloader doesn't provide one
#define AP_TRAMPOLINE_ADDR_DEFAULT  0x8000

// AP stack size
#define AP_STACK_SIZE           8192

// Timeout for AP startup (in milliseconds)
#define AP_STARTUP_TIMEOUT_MS   200

// ============================================================================
// SMP State
// ============================================================================

typedef enum {
    SMP_STATE_BSP_ONLY = 0,     // Only BSP running
    SMP_STATE_STARTING_APS,     // APs being started
    SMP_STATE_RUNNING           // All CPUs running
} smp_state_t;

// ============================================================================
// SMP Functions
// ============================================================================

// Initialize SMP: detect CPUs, prepare for AP startup
// trampoline_addr: Physical address for AP trampoline (from bootloader), or 0 for default
void smp_init(uint64_t trampoline_addr);

// Start all Application Processors
void smp_boot_aps(void);

// Wait for all APs to start
void smp_wait_for_aps(void);

// Get number of CPUs (total, including BSP)
uint32_t smp_get_cpu_count(void);

// Get number of APs started
uint32_t smp_get_aps_started(void);

// Check if SMP is enabled (more than one CPU)
bool smp_is_enabled(void);

// Get current SMP state
smp_state_t smp_get_state(void);

// ============================================================================
// AP Entry Point
// ============================================================================

// C entry point for APs (called from trampoline)
void ap_entry(void);

// ============================================================================
// CPU Synchronization Barriers
// ============================================================================

// Simple barrier for CPU synchronization
typedef struct {
    volatile uint32_t count;
    volatile uint32_t waiting;
    volatile uint32_t sense;
} smp_barrier_t;

// Initialize barrier for given number of CPUs
void smp_barrier_init(smp_barrier_t* barrier, uint32_t count);

// Wait at barrier (all CPUs must reach before any can proceed)
void smp_barrier_wait(smp_barrier_t* barrier);

// ============================================================================
// Cross-CPU Function Calls (IPIs)
// ============================================================================

// Send reschedule IPI to a specific CPU
void smp_send_reschedule(uint32_t cpu_id);

// Send reschedule IPI to all other CPUs
void smp_send_reschedule_all(void);

// Send TLB shootdown IPI to all other CPUs
void smp_tlb_shootdown(void);

// Halt all other CPUs (for panic)
void smp_halt_others(void);

#endif // _KERNEL_SMP_H_
