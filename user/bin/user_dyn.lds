/* LikeOS-64 User Program Linker Script — dynamically linked PIE */

ENTRY(_entry)

SECTIONS
{
    /* PIE: addresses start at 0, kernel adds base offset (0x400000) at load time */
    . = SIZEOF_HEADERS;

    /* Program interpreter (ld-likeos.so) */
    .interp : {
        *(.interp)
    }

    /* Dynamic linking metadata */
    .hash : { *(.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }

    /* Relocations */
    .rela.dyn : {
        *(.rela.dyn)
        *(.rela.init)
        *(.rela.text)
        *(.rela.rodata)
        *(.rela.data)
        *(.rela.bss)
    }
    .rela.plt : { *(.rela.plt) }

    /* PLT */
    .plt : { *(.plt) *(.plt.*) }

    /* Code */
    .text : {
        *(.text.start)
        *(.text)
        *(.text.*)
    }

    /* Read-only data */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /* Constructors / Destructors (for shared lib init) */
    .init_array : {
        __init_array_start = .;
        *(.init_array)
        *(.ctors)
        __init_array_end = .;
    }
    .fini_array : {
        __fini_array_start = .;
        *(.fini_array)
        *(.dtors)
        __fini_array_end = .;
    }

    /* Data segment — page-aligned so .dynamic is writable */
    . = ALIGN(4096);

    .dynamic : { *(.dynamic) }

    .got : { *(.got) }
    .got.plt : { *(.got.plt) }

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
        *(.eh_frame_hdr)
    }
}
