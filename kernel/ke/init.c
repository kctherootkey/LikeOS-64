// LikeOS-64 Kernel Executive - Initialization
// Main kernel initialization and executive services

#include "../../include/kernel/console.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/memory.h"

// Function prototypes
void KiSystemStartup(void);
void kernel_main(framebuffer_info_t* fb_info);

// UEFI kernel entry point - called by bootloader
void kernel_main(framebuffer_info_t* fb_info) {
    // Initialize console system with framebuffer info
    console_init(fb_info);
    
    // Call the main system initialization
    KiSystemStartup();
}

// Kernel Executive entry point
void KiSystemStartup(void) {
    // Console is already initialized by kernel_main()
    
    // Print our boot message using kprintf
    kprintf("LikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");
    kprintf("Higher Half Kernel loaded at virtual address 0x%p\n", (void*)KiSystemStartup);
    
    // Set colored output
    console_set_color(10, 0); // Light Green on Black
    kprintf("\nKernel initialization complete!\n");
        
    // Reset to default colors
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
    
    // Print initial memory statistics
    MmPrintMemoryStats();

    // Test memory allocation
    kprintf("Testing memory allocation...\n");
    void* test_ptr1 = kalloc(1024);
    void* test_ptr2 = kalloc(2048);
    kprintf("  Allocated test blocks: 0x%p (1KB), 0x%p (2KB)\n", test_ptr1, test_ptr2);
    kfree(test_ptr1);
    kfree(test_ptr2);
    kprintf("  Test blocks freed successfully\n");

    // Initialize keyboard
    keyboard_init();
    
    // Enable keyboard IRQ (IRQ 1)
    irq_enable(1);
        
    // Enable interrupts after everything is initialized
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled!\n");
    
    // Show ready prompt
    console_set_color(11, 0); // Light Cyan on Black
    kprintf("\nSystem ready! Type to test keyboard input:\n");
    kprintf("> ");
    console_set_color(15, 0); // White on Black

    // Main input loop
    char c;
    while (1) {
        c = keyboard_get_char();

        if (c != 0) {
            if (c == '\n') {
                // Handle Enter key
                kprintf("\n> ");
            } else if (c == '\b') {
                // Handle Backspace key using proper console function
                console_backspace();
            } else if (c >= ' ' && c <= '~') {
                // Printable character
                kprintf("%c", c);
            }
        }
        
        // Halt CPU until next interrupt
        __asm__ volatile ("hlt");
    }
}


