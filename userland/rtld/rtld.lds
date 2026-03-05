/* ld-likeos.so linker script */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = SIZEOF_HEADERS;

    .hash           : { *(.hash) }
    .gnu.hash       : { *(.gnu.hash) }
    .dynsym         : { *(.dynsym) }
    .dynstr         : { *(.dynstr) }

    .rela.dyn       : { *(.rela.dyn) *(.rela.plt) }

    .plt            : { *(.plt) *(.plt.*) }
    .plt.got        : { *(.plt.got) }

    .text           : { *(.text .text.*) }
    .rodata         : { *(.rodata .rodata.*) }

    . = ALIGN(0x1000);

    .dynamic        : { *(.dynamic) }
    .got            : { *(.got) }
    .got.plt        : { *(.got.plt) }
    .data           : { *(.data .data.*) }
    .bss            : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
    }
}
