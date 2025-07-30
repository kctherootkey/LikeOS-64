// LikeOS-64 Kernel
// A minimal 64-bit kernel that displays a message using VGA text mode

#include "kprintf.h"

// Function prototypes
void kernel_main(void);

// Kernel entry point
void kernel_main(void) {
    // Initialize console system
    console_init();
    
    // Print our boot message using kprintf
    kprintf("LikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");
    kprintf("Kernel loaded successfully at address 0x%p\n", (void*)0x100000);
    
    // Demonstrate various format specifiers
    kprintf("\nSystem Information:\n");
    kprintf("- Architecture: x86_64\n");
    kprintf("- Memory start: 0x%08x\n", 0x100000);
    kprintf("- Boot sectors: %d\n", 32);
    kprintf("- Kernel size: %u bytes\n", 4348);
    
    // Set colored output
    console_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\nKernel initialization complete!\n");
    
    console_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    kprintf("System ready for operation.\n");
    
    // Reset to default colors
    console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    
    // Halt the CPU (infinite loop)
    kprintf("\nHalting CPU...\n");
    while (1) {
        __asm__ volatile ("hlt");
    }
}


