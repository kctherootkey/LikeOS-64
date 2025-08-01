#include "minimal_console.h"

// Minimal kernel entry point
// This is completely separate from the main kernel in kernel/ke/init.c
void kernel_main(framebuffer_info_t* framebuffer_info) {

//    int i = 1/0;

    // Initialize the minimal console system with framebuffer
    console_init(framebuffer_info);
    
    // Print the required message
    print_string("LikeOS-64 Kernel loaded");
    
    // Infinite halt loop - kernel execution complete
    while (1) {
        // Use inline assembly to halt the CPU
        // This will put the CPU in a low-power state until the next interrupt
        __asm__ volatile ("hlt");
    }
}

// Alternative entry point in case the bootloader expects _start
void _start(void* framebuffer_info) {
    kernel_main((framebuffer_info_t*)framebuffer_info);
}
