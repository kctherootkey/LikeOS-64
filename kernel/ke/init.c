// LikeOS-64 Kernel Initialization
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/memory.h"
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

void system_startup(boot_info_t* boot_info);
void kernel_main(boot_info_t* boot_info);

static xhci_boot_state_t g_xhci_boot;
static storage_fs_state_t g_storage_state;
static boot_info_t* g_boot_info;

void kernel_main(boot_info_t* boot_info) {
    g_boot_info = boot_info;
    console_init((framebuffer_info_t*)&boot_info->fb_info);
    console_init_fb_optimization();
    system_startup(boot_info);
}

void system_startup(boot_info_t* boot_info) {
    kprintf("\nLikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");

    console_set_color(10, 0);
    kprintf("\nKernel initialization complete!\n");
    console_set_color(15, 0);

    interrupts_init();

    uint64_t memory_size = boot_info->mem_info.total_memory;
    if (memory_size < 256 * 1024 * 1024) {
        memory_size = 256 * 1024 * 1024;
    }
    
    mm_initialize_physical_memory(memory_size);
    mm_initialize_virtual_memory();
    mm_initialize_heap();
    mm_print_memory_stats();
    
    mm_enable_nx();
    mm_remap_kernel_with_nx();
    
    mm_initialize_syscall();

    pci_init();
    pci_enumerate();
    pci_assign_unassigned_bars();

    vfs_init();
    xhci_boot_init(&g_xhci_boot);
    storage_fs_init(&g_storage_state);

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

    __asm__ volatile ("sti");

    sched_init();

    timer_init(100);
    timer_start();

    shell_init();

    while (1) {
        __asm__ volatile ("sti");
        int handled_input = shell_tick();
        xhci_boot_poll(&g_xhci_boot);
        storage_fs_poll(&g_storage_state);
        sched_run_ready();

        if (!handled_input) {
            __asm__ volatile ("hlt");
        }
    }
}

