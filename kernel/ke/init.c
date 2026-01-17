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
static void spawn_user_test_task(void);

extern char user_test_start[];
extern char user_test_end[];

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

    spawn_user_test_task();

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

static void spawn_user_test_task(void) {
    size_t user_code_size = (size_t)(user_test_end - user_test_start);
    
    // Allow up to 64KB for user test program (16 pages)
    #define USER_CODE_MAX_SIZE (64 * 1024)
    
    if (user_code_size == 0 || user_code_size > USER_CODE_MAX_SIZE) {
        kprintf("ERROR: Invalid user code size (%u bytes)\n", (unsigned)user_code_size);
        return;
    }
    
    uint64_t* user_pml4 = mm_create_user_address_space();
    if (!user_pml4) {
        kprintf("ERROR: Failed to create user address space\n");
        return;
    }
    
    #define USER_CODE_VADDR 0x400000
    
    // Calculate number of pages needed
    size_t pages_needed = (user_code_size + 4095) / 4096;
    uint64_t code_phys_pages[16];  // Max 16 pages (64KB)
    
    for (size_t i = 0; i < pages_needed; i++) {
        code_phys_pages[i] = mm_allocate_physical_page();
        if (!code_phys_pages[i]) {
            kprintf("ERROR: Failed to allocate physical page for user code\n");
            for (size_t j = 0; j < i; j++) {
                mm_free_physical_page(code_phys_pages[j]);
            }
            mm_destroy_address_space(user_pml4);
            return;
        }
    }
    
    // Copy user code to physical pages
    uint8_t* src = (uint8_t*)user_test_start;
    size_t remaining = user_code_size;
    for (size_t i = 0; i < pages_needed; i++) {
        size_t copy_size = (remaining > 4096) ? 4096 : remaining;
        mm_memcpy((void*)code_phys_pages[i], src, copy_size);
        src += copy_size;
        remaining -= copy_size;
    }
    
    // User program pages: user, readable, writable, executable
    // For simplicity, map all pages as RWX. A real loader would parse ELF sections.
    uint64_t code_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    for (size_t i = 0; i < pages_needed; i++) {
        if (!mm_map_page_in_address_space(user_pml4, USER_CODE_VADDR + i * 4096, code_phys_pages[i], code_flags)) {
            kprintf("ERROR: Failed to map user code page %u\n", (unsigned)i);
            for (size_t j = 0; j < pages_needed; j++) {
                mm_free_physical_page(code_phys_pages[j]);
            }
            mm_destroy_address_space(user_pml4);
            return;
        }
    }
    
    kprintf("User code: %u bytes (%u pages) loaded at 0x%x\n", 
            (unsigned)user_code_size, (unsigned)pages_needed, USER_CODE_VADDR);
    
    // Stack at 128MB - plenty of room for heap to grow from 5MB
    #define USER_TEST_STACK_TOP  0x8000000ULL
    #define USER_TEST_STACK_SIZE (64 * 1024)
    
    if (!mm_map_user_stack(user_pml4, USER_TEST_STACK_TOP, USER_TEST_STACK_SIZE)) {
        kprintf("ERROR: Failed to map user stack\n");
        for (size_t j = 0; j < pages_needed; j++) {
            mm_free_physical_page(code_phys_pages[j]);
        }
        mm_destroy_address_space(user_pml4);
        return;
    }
    
    task_t* user_task = sched_add_user_task(
        (task_entry_t)USER_CODE_VADDR,
        NULL,
        user_pml4,
        USER_TEST_STACK_TOP,
        0
    );
    
    if (!user_task) {
        kprintf("ERROR: Failed to create user task\n");
        mm_destroy_address_space(user_pml4);
        return;
    }
    
    kprintf("User task created: id=%d\n", user_task->id);
}

