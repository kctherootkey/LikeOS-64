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

// Function prototypes
void KiSystemStartup(void);
void kernel_main(framebuffer_info_t* fb_info);

static xhci_boot_state_t g_xhci_boot;
static storage_fs_state_t g_storage_state;

// UEFI kernel entry point - called by bootloader
void kernel_main(framebuffer_info_t* fb_info) {
    // Initialize console system with framebuffer info
    console_init(fb_info);
    // Initialize framebuffer optimization after console is ready
    console_init_fb_optimization();
    // Call the main system initialization
    KiSystemStartup();
}

// Kernel Executive entry point
void KiSystemStartup(void) {
    // Console is already initialized by kernel_main()

    kprintf("\nLikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");
    kprintf("Higher Half Kernel loaded at virtual address %p\n", (void*)KiSystemStartup);

    console_set_color(10, 0); // Light Green on Black
    kprintf("\nKernel initialization complete!\n");
    console_set_color(15, 0); // White on Black

    // Initialize interrupt system
    kprintf("\nInitializing interrupt system...\n");
    interrupts_init();
    kprintf("Interrupt system initialized successfully\n");

    // Initialize memory management subsystem
    kprintf("\nInitializing Memory Management Subsystem...\n");
    MmDetectMemory();
    MmInitializePhysicalMemory(32 * 1024 * 1024); // 32MB minimum requirement
    MmInitializeVirtualMemory();
    MmInitializeHeap();
    MmPrintMemoryStats();

    // Initialize PCI and enumerate devices
    kprintf("\nInitializing PCI subsystem...\n");
    pci_init();
    int pci_count = pci_enumerate();
    kprintf("PCI enumeration complete (%d devices)\n", pci_count);
    pci_assign_unassigned_bars();

    // Initialize VFS
    vfs_init();

    // Locate and initialize XHCI/USB stack
    xhci_boot_init(&g_xhci_boot);

    // Prepare FAT32 probing state
    storage_fs_init(&g_storage_state);

    // Test memory allocation
    kprintf("Testing memory allocation...\n");
    void* test_ptr1 = kalloc(1024);
    void* test_ptr2 = kalloc(2048);
    kprintf("  Allocated test blocks: %p (1KB), %p (2KB)\n", test_ptr1, test_ptr2);
    kfree(test_ptr1);
    kfree(test_ptr2);
    kprintf("  Test blocks freed successfully\n");

    // Display framebuffer optimization status
    kprintf("\nFramebuffer Optimization Status:\n");
    console_show_fb_status();

    // Initialize and render system scrollbar (single source of truth)
    kprintf("\nInitializing visual scrollbar system...\n");
    static scrollbar_t system_scrollbar;
    if (scrollbar_init_system_default(&system_scrollbar) == 0) {
        scrollbar_render(&system_scrollbar);
        fb_flush_dirty_regions();
        kprintf("Visual scrollbar initialized and rendered successfully\n");
    } else {
        kprintf("Warning: Failed to initialize scrollbar\n");
    }

    // Initialize legacy PS/2 controller before keyboard driver (safe no-op if absent)
    ps2_init();
    if (ioapic_configure_legacy_irq(1, 0x21, IOAPIC_POLARITY_LOW, IOAPIC_TRIGGER_EDGE) != 0) {
        kprintf("IOAPIC: keyboard redirection not configured (absent or unmapped)\n");
    }
    keyboard_init();
    irq_enable(1);

    // Initialize mouse
    mouse_init();
    irq_enable(2);
    irq_enable(12);

    // Enable interrupts after everything is initialized
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled!\n");

    // Show ready prompt and enter shell loop
    shell_init();

    while (1) {
        int handled_input = shell_tick();
        xhci_boot_poll(&g_xhci_boot);
        storage_fs_poll(&g_storage_state);

        // Halt CPU when idle to avoid spinning at 100% while waiting for input
        if (!handled_input) {
            __asm__ volatile ("hlt");
        }
    }
}

