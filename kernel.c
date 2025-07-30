// LikeOS-64 Kernel
// A minimal 64-bit kernel that displays a message using VGA text mode

#include "kprintf.h"
#include "interrupts.h"
#include "keyboard.h"

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
    
    // Set colored output
    console_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\nKernel initialization complete!\n");
        
    // Reset to default colors
    console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    
    // Initialize interrupt system
    kprintf("\nInitializing interrupt system...\n");
    interrupts_init();
    
    // Initialize keyboard
    keyboard_init();
    
    // Enable keyboard IRQ (IRQ 1)
    irq_enable(1);
        
    // Enable interrupts after everything is initialized
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled!\n");
    
    // Show ready prompt
    console_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\nSystem ready! Type to test keyboard input:\n");
    kprintf("> ");
    console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    
    // Main input loop
    char c;
    while (1) {
        c = keyboard_get_char();

        if (c != 0) {
            if (c == '\n') {
                // Handle Enter key
                kprintf("\n> ");
            } else if (c == '\b') {
                // Handle Backspace key
                // Simple backspace implementation
                kprintf("\b \b");
            } else if (c >= ' ' && c <= '~') {
                // Printable character
                kprintf("%c", c);
            }
        }
        
        // Halt CPU until next interrupt
        __asm__ volatile ("hlt");
    }
}


