; LikeOS-64 Boot - 64-bit Kernel Entry Point
; Provides the entry point and initial setup for the kernel executive

[BITS 64]

section .text
global _start
extern KiSystemStartup

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

    ; Call kernel executive startup
    call KiSystemStartup
    
    ; If KiSystemStartup returns (it shouldn't), halt
hang:
    hlt
    jmp hang
