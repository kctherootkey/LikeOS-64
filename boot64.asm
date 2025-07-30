; 64-bit kernel entry point
; This assembly file provides the entry point for our C kernel

[BITS 64]

section .text
global _start
extern kernel_main

_start:
    ; Setup segment registers for 64-bit mode (redundant but safe)
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Setup stack pointer (ensure it's 16-byte aligned for x86_64 ABI)
    mov rsp, 0x200000       ; Stack at 2MB mark
    and rsp, ~0xF           ; Align to 16-byte boundary
    
    ; Clear direction flag
    cld

    ; Call our C kernel main function
    call kernel_main
    
    ; If kernel_main returns (it shouldn't), halt
hang:
    hlt
    jmp hang
