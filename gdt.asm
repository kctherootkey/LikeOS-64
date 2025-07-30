; GDT Assembly functions for LikeOS-64
; 64-bit GDT loading functions

[BITS 64]

section .text

global gdt_flush

; Load GDT
gdt_flush:
    lgdt [rdi]          ; Load GDT from pointer in RDI
    
    ; Reload segment registers
    mov ax, 0x10        ; Data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Far return to reload CS
    push 0x08           ; Code segment selector
    lea rax, [rel .reload_cs]
    push rax
    retfq               ; Far return
    
.reload_cs:
    ret
