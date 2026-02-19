// LikeOS-64 Kernel Initialization
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/scrollbar.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/ps2.h"
#include "../../include/kernel/ioapic.h"
#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/storage.h"
#include "../../include/kernel/shell.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/devfs.h"
#include "../../include/kernel/acpi.h"
#include "../../include/kernel/percpu.h"
#include "../../include/kernel/smp.h"
#include "../../include/kernel/lapic.h"

void system_startup(boot_info_t* boot_info);
void kernel_main(boot_info_t* boot_info);

static xhci_boot_state_t g_xhci_boot;
static storage_fs_state_t g_storage_state;
static boot_info_t* g_boot_info;
static uint64_t g_rsdp_address;           // Saved RSDP address (copied before identity unmap)
static uint64_t g_smp_trampoline_address; // Saved SMP trampoline address from bootloader

void kernel_main(boot_info_t* boot_info) {
    g_boot_info = boot_info;
    console_init((framebuffer_info_t*)&boot_info->fb_info);
    console_init_fb_optimization();
    system_startup(boot_info);
}

void system_startup(boot_info_t* boot_info) {
    console_set_color(10, 0);
    kprintf("\nLikeOS-64 Kernel v0.2\n\n");
    console_set_color(15, 0);

    kprintf("64-bit Long Mode Active\n");

    interrupts_init();

    uint64_t memory_size = boot_info->mem_info.total_memory;
    if (memory_size < 256 * 1024 * 1024) {
        memory_size = 256 * 1024 * 1024;
    }
    
    // Store UEFI memory map before initializing memory manager
    // This allows us to mark UEFI reserved regions as off-limits
    mm_initialize_from_boot_info(boot_info);
    
    mm_initialize_physical_memory(memory_size);
    mm_initialize_virtual_memory();
    mm_initialize_heap();
    
    // Initialize page table pool (needs heap ready, before SLAB)
    mm_init_pt_pool();
    
    // Initialize SLAB allocator (dynamic kernel heap)
    slab_init();
    
    mm_print_memory_stats();
    
    mm_enable_nx();
    mm_remap_kernel_with_nx();
    mm_enable_smep_smap();
    
    // Before removing identity mapping, remap framebuffer pointers to direct map
    console_remap_to_direct_map();
    fb_optimize_remap_to_direct_map();
    
    // Save RSDP address before identity mapping is removed (boot_info is in low memory)
    g_rsdp_address = boot_info->rsdp_address;
    
    // Save SMP trampoline address from bootloader
    g_smp_trampoline_address = boot_info->smp_trampoline_addr;
    
    // Switch to a kernel stack in higher-half space before removing identity mapping
    // The current stack is in low memory (set up by bootloader)
    // This function does NOT return - it switches stacks and calls continue_system_startup
    mm_switch_to_kernel_stack();
    
    // Never reached - execution continues in continue_system_startup()
    for(;;) __asm__ volatile("hlt");
}

// This function continues system startup after switching to the kernel stack
// and removing identity mapping. Called from mm_switch_to_kernel_stack()
void continue_system_startup(void) {
    mm_initialize_syscall();

    pci_init();
    pci_enumerate();
    pci_assign_unassigned_bars();

    vfs_init();
    devfs_init();
    vfs_register_devfs(devfs_get_ops());
    tty_init();

    static scrollbar_t system_scrollbar;
    if (scrollbar_init_system_default(&system_scrollbar) == 0) {
        scrollbar_render(&system_scrollbar);
        fb_flush_dirty_regions();
    }

    ps2_init();
    keyboard_init();
    irq_enable(0);
    irq_enable(1);
    irq_enable(2);

    mouse_init();
    irq_enable(12);

    xhci_boot_init(&g_xhci_boot);
    storage_fs_init(&g_storage_state);

    __asm__ volatile ("sti");

    sched_init();

    // Initialize SMP support
    // This sets up per-CPU data and detects available CPUs via ACPI
    // Pass RSDP address from UEFI bootloader for proper ACPI table discovery
    acpi_init(g_rsdp_address);
    percpu_init();
    smp_init(g_smp_trampoline_address);
    
    // Boot Application Processors (APs)
    // After this, all CPUs are running and SMP is fully initialized
    smp_boot_aps();
    kprintf("SMP: %u CPU(s) online\n", smp_get_cpu_count());

    timer_init(100);
    timer_start();

    // Detect whether PIT (IRQ0) is reaching the BSP now that the LAPIC
    // is active.  QEMU delivers PIT via virtual wire (PIC→LINT0) fine,
    // but VMware stops PIT delivery, expecting I/O APIC routing.
    // If no PIT tick arrives within a brief spin, switch to LAPIC timer.
    if (lapic_is_available()) {
        uint64_t t0 = timer_ticks();
        for (volatile int i = 0; i < 10000000; i++) {
            if (timer_ticks() != t0) break;
        }
        if (timer_ticks() == t0) {
            irq_disable(0);
            lapic_timer_start(100);
            kprintf("Timer: PIT not delivering, using LAPIC timer at 100 Hz\n");
        }
    }

    shell_init();
    storage_fs_set_ready(&g_storage_state);

    while (1) {
        __asm__ volatile ("sti");
        int handled_input = shell_tick();
        xhci_boot_poll(&g_xhci_boot);
        storage_fs_poll(&g_storage_state);
        console_cursor_update();  // Update blinking cursor
        sched_run_ready();

        if (!handled_input) {
            __asm__ volatile ("hlt");
        }
    }
}

