ENTRY(kernel_main)

SECTIONS
{
    . = 0xFFFFFFFF80000000;
    
    .text : {
        *(.text)
    }
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : {
        *(.data)
        *(.data.*)
    }
    
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    
    kernel_end = .;
    
    /DISCARD/ : {
        *(.note.gnu.property)
        *(.comment)
        *(.eh_frame)
    }
}
