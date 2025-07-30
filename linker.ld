/* LikeOS-64 Linker Script */
/* This script tells the linker how to arrange our kernel in memory */

ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 1MB (0x100000) */
    . = 0x100000;
    
    /* Text section (code) */
    .text : ALIGN(4096)
    {
        *(.text)
    }
    
    /* Read-only data */
    .rodata : ALIGN(4096)
    {
        *(.rodata)
    }
    
    /* Data section */
    .data : ALIGN(4096)
    {
        *(.data)
    }
    
    /* BSS section (uninitialized data) */
    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss)
    }
}
