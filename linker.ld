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
    
    /* Read-only data - immediately after text, no gap */
    .rodata : ALIGN(4)
    {
        *(.rodata)
    }
    
    /* Data section - immediately after rodata, no gap */
    .data : ALIGN(4)
    {
        *(.data)
    }
    
    /* BSS section (uninitialized data) - after everything else */
    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss)
    }
}
