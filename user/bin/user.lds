/*
 * LikeOS User Program Linker Script
 *
 * Copyright (C) 2026 The LikeOS Project
 */

ENTRY(_entry)

SECTIONS
{
    /* Load at standard user address */
    . = 0x400000;
    
    .text : {
        /* Entry point must come first */
        *(.text.start)
        *(.text)
        *(.text.*)
    }
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : {
        *(.data)
        *(.data.*)
    }
    
    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    . = ALIGN(8);
    _end = .;
    
    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame)
    }
}
