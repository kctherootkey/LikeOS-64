ENTRY(kernel_main)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* R-X: Read + Execute */
}

SECTIONS
{
    . = 0xFFFFFFFF80000000;
    
    kernel_text_start = .;
    .text : {
        *(.text)
    } :text
    kernel_text_end = .;
    
    /* Page-align to prevent .text/.rodata sharing pages */
    . = ALIGN(0x1000);
    
    kernel_rodata_start = .;
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text
    kernel_rodata_end = .;
    
    /* Page-align to prevent .rodata/.data sharing pages */
    . = ALIGN(0x1000);
    
    kernel_data_start = .;
    .data : {
        *(.data)
        *(.data.*)
    } :text
    kernel_data_end = .;
    
    /* Page-align to prevent .data/.bss sharing pages */
    . = ALIGN(0x1000);
    
    kernel_bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :text
    kernel_bss_end = .;
    
    kernel_end = .;
    
    /DISCARD/ : {
        *(.note.gnu.property)
        *(.comment)
        *(.eh_frame)
    }
}
