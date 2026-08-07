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

    /* PLT.
     *
     * The variants stay in SEPARATE output sections, exactly as the linker's
     * built-in script has them.  They are not interchangeable: the linker
     * generates unwind information for the PLT itself, one entry per PLT
     * section, each describing the range its section occupies.  Collapsed into
     * one output section with `*(.plt.*)` they all start at the same address,
     * so the generated entries describe overlapping ranges and the link fails
     * with ".eh_frame_hdr refers to overlapping FDEs".
     *
     * This was invisible until the unwind tables started being kept: without
     * --eh-frame-hdr the linker never builds the index that notices.
     */
    .plt : { *(.plt) *(.iplt) }
    .plt.got : { *(.plt.got) }
    .plt.sec : { *(.plt.sec) }

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

    /* Exception-handling tables.
     *
     * These used to be discarded, on the reasonable-looking grounds that a C
     * program never unwinds.  It costs nothing until something does: a C++
     * throw asks the runtime linker which object covers the faulting program
     * counter, the loader answers with that object's PT_GNU_EH_FRAME, and the
     * unwinder walks the tables it points at.  With the tables discarded there
     * is no such segment, the search finds nothing, and every throw ends in
     * std::terminate instead of the matching catch -- with nothing in the
     * failure naming the linker script.
     *
     * .eh_frame_hdr must appear in the output for the linker to emit the
     * PT_GNU_EH_FRAME program header that names it, which is the entry point to
     * all of this.  KEEP, because the tables are only ever referenced through
     * that header and --gc-sections would otherwise take them.
     *
     * Also what makes a backtrace possible at all, for anything that later
     * wants one.
     */
    .eh_frame_hdr : {
        *(.eh_frame_hdr)
        *(.eh_frame_entry .eh_frame_entry.*)
    }
    .eh_frame : {
        KEEP(*(.eh_frame))
        *(.eh_frame.*)
    }
    .gcc_except_table : {
        *(.gcc_except_table)
        *(.gcc_except_table.*)
    }

    /* Data segment.
     *
     * DATA_SEGMENT_ALIGN, not a plain ALIGN(4096).  It page-aligns the data
     * exactly the same way, but it additionally tells the linker that the data
     * segment starts HERE, which is what makes the RELRO region below possible:
     * PT_GNU_RELRO runs from this point to DATA_SEGMENT_RELRO_END, and with an
     * ordinary ALIGN the linker has no start to measure from and emits no such
     * segment at all -- silently, even when asked with -z relro.
     */
    . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));

    /* ---- RELRO: written while relocating, read-only afterwards ----
     *
     * Everything from here to DATA_SEGMENT_RELRO_END is written by the runtime
     * linker as it relocates and by nothing else for the rest of the process's
     * life, so ld-likeos.so mprotects it read-only once relocation finishes.
     *
     * That is worth the two page-aligned boundaries it costs.  A stray write
     * into the GOT is otherwise invisible where it happens: the program runs on
     * until it next calls through the damaged slot and then dies in a PLT stub,
     * with a crash dump that names the victim and says nothing about the
     * culprit.  Read-only, the write faults on the instruction that makes it.
     *
     * Thread-local storage sits inside the region -- .tdata is the initial
     * image copied into each new thread, never written through here.  It must
     * stay adjacent to .tbss and ahead of .data/.bss: the linker builds PT_TLS
     * from the pair, and without these output sections they land as orphans,
     * no PT_TLS is emitted, and every __thread variable in the program silently
     * reads whatever shares its address.
     */
    .tdata : {
        *(.tdata)
        *(.tdata.*)
        *(.gnu.linkonce.td.*)
    }
    .tbss : {
        *(.tbss)
        *(.tbss.*)
        *(.gnu.linkonce.tb.*)
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

    /* Constant data that happens to need a relocation -- vtables, string
     * tables of pointers.  Without an output section of its own it lands as an
     * orphan next to .data and stays writable for good. */
    .data.rel.ro : {
        *(.data.rel.ro.local*)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
        *(.gnu.linkonce.d.rel.ro.*)
    }

    .dynamic : { *(.dynamic) }

    /* The whole of .got.plt is inside the region, not just its three reserved
     * entries as in the stock script.  That is only correct because this
     * toolchain links -z now: with lazy binding the resolver writes each slot
     * on first call and the pages have to stay writable.  ld-likeos.so applies
     * RELRO only to objects that carry DF_BIND_NOW, so an object built without
     * it keeps a writable GOT rather than faulting on its first call. */
    .got : { *(.got) *(.igot) }
    .got.plt : { *(.got.plt) *(.igot.plt) }

    . = DATA_SEGMENT_RELRO_END (0, .);

    /* ---- writable for the life of the process ---- */

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

    /* Closes the data segment opened by DATA_SEGMENT_ALIGN.  The linker
     * rejects the one without the other. */
    . = DATA_SEGMENT_END (.);

    /DISCARD/ : {
        *(.note*)
        *(.comment)
    }
}
