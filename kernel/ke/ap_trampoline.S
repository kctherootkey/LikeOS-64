; LikeOS-64 AP (Application Processor) Startup Trampoline
; This code runs in real mode, transitions to long mode,
; then jumps to the C entry point ap_entry()
;
; This code is POSITION-INDEPENDENT - it calculates its base address at runtime
; The SIPI vector determines where this code runs (typically 0x8000)
;
; Data layout (offsets from trampoline start):
;   0x108: PML4 physical address (64-bit)
;   0x110: Stack pointer (64-bit)
;   0x118: CPU ID (32-bit)
;   0x120: Entry point address (64-bit)

[bits 16]
section .text

global ap_trampoline_start
global ap_trampoline_end

; Export offsets as absolute symbols for C code
global ap_trampoline_pml4
global ap_trampoline_stack
global ap_trampoline_cpu_id
global ap_trampoline_entry
global ap_trampoline_gdt

ap_trampoline_start:
    ; Entry point - we're in real mode
    ; CS is set to the trampoline base >> 4
    cli
    cld
    
    ; Set DS = CS (our segment base)
    mov ax, cs
    mov ds, ax
    
    ; Calculate 32-bit linear base address: base = CS * 16
    ; Store in EBX for use throughout mode transitions
    xor ebx, ebx
    mov bx, cs
    shl ebx, 4          ; EBX = physical base address
    
    ; Patch GDT base address: gdt_start is at offset 0xD8 (see layout below)
    mov eax, ebx
    add eax, 0xD8       ; gdt_start offset
    mov [0x102], eax    ; Store at gdt_base (offset 0x102)
    
    ; Patch far jump target: protected_mode offset is calculated by assembler
    mov eax, ebx
    add eax, (protected_mode - ap_trampoline_start)
    mov [0xC8], eax     ; Store at pm_jump_target (offset 0xC8)
    
    ; Load GDT (gdt_ptr is at offset 0x100)
    lgdt [0x100]
    
    ; Clear ES and SS
    xor ax, ax
    mov es, ax
    mov ss, ax
    
    ; Enable protected mode (CR0.PE)
    mov eax, cr0
    or al, 1
    mov cr0, eax
    
    ; Far jump to protected mode using indirect far pointer at offset 0xC8
    ; o32 prefix ensures 32-bit offset is loaded
    db 0x66             ; Operand size override (32-bit)
    db 0xFF, 0x2E       ; JMP FAR [mem16]
    dw 0xC8             ; Offset of pm_jump_ptr

[bits 32]
protected_mode:
    ; Set up 32-bit segments (selector 0x20 = 32-bit data in our GDT)
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    
    ; EBX still contains our base address from real mode
    
    ; Patch long mode jump target: long_mode offset
    ; Calculate target address: base + (long_mode - ap_trampoline_start)
    mov eax, ebx
    add eax, (long_mode - ap_trampoline_start)
    ; Store at lm_jump_target offset 0xCE
    mov [ebx + 0xCE], eax
    
    ; Enable PAE (CR4.PAE)
    mov eax, cr4
    or eax, (1 << 5)    ; PAE
    or eax, (1 << 7)    ; PGE (Page Global Enable)
    mov cr4, eax
    
    ; Load PML4 address: [base + 0x108]
    mov eax, [ebx + 0x108]
    mov cr3, eax
    
    ; Enable long mode (EFER.LME)
    mov ecx, 0xC0000080  ; EFER MSR
    rdmsr
    or eax, (1 << 8)    ; LME (Long Mode Enable)
    or eax, (1 << 11)   ; NXE (No-Execute Enable)
    wrmsr
    
    ; Enable paging (CR0.PG) - this activates long mode
    mov eax, cr0
    or eax, (1 << 31)   ; PG
    or eax, (1 << 16)   ; WP (Write Protect)
    mov cr0, eax
    
    ; Far jump to 64-bit long mode code using patched jump pointer
    ; No stack needed - use indirect far jump
    jmp far [ebx + 0xCE]  ; lm_jump_ptr at offset 0xCE

[bits 64]
long_mode:
    ; We're now in 64-bit long mode!
    ; RBX still contains our base address (zero-extended from EBX)
    
    ; Load 64-bit segments (0x10 = data segment, same as kernel)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Load per-AP stack: [base + 0x110]
    mov rsp, [rbx + 0x110]
    
    ; Get CPU ID: [base + 0x118]
    mov edi, [rbx + 0x118]
    
    ; Jump to C entry point: [base + 0x120]
    mov rax, [rbx + 0x120]
    
    ; Call the C entry point with CPU ID in EDI
    call rax
    
    ; Should never return, but halt if it does
.halt:
    cli
    hlt
    jmp .halt

; ============================================================================
; Data Layout (all offsets are from trampoline start):
;   0xC8: pm_jump_ptr (6 bytes: 4-byte offset + 2-byte selector) for 32-bit jump
;   0xCE: lm_jump_ptr (6 bytes: 4-byte offset + 2-byte selector) for 64-bit jump  
;   0xD8: gdt_start (40 bytes for 5 GDT entries)
;   0x100: gdt_ptr (6 bytes: 2-byte limit + 4-byte base)
;   0x108: ap_pml4 (8 bytes)
;   0x110: ap_stack (8 bytes)
;   0x118: ap_cpu_id (8 bytes)
;   0x120: ap_entry_addr (8 bytes)
; ============================================================================

; Padding to align pm_jump_ptr at offset 0xC8
times (0xC8 - ($ - ap_trampoline_start)) db 0x90

; Far pointer for protected mode jump (offset 0xC8)
pm_jump_ptr:
pm_jump_target:
    dd 0                ; Target address (patched at runtime)
    dw 0x18             ; 32-bit code segment selector

; Far pointer for long mode jump (offset 0xCE)
lm_jump_ptr:
lm_jump_target:
    dd 0                ; Target address (patched at runtime)
    dw 0x08             ; 64-bit code segment selector (same as kernel!)

; Padding to align GDT at offset 0xD8
times (0xD8 - ($ - ap_trampoline_start)) db 0

; GDT for AP startup (offset 0xD8)
; Layout matches kernel GDT for seamless transition:
;   0x00 = Null
;   0x08 = 64-bit code (kernel code segment) 
;   0x10 = Data segment (kernel data segment)
;   0x18 = 32-bit code (for protected mode transition only)
;   0x20 = 32-bit data (for protected mode transition only)
gdt_start:
    ; Null descriptor (selector 0x00)
    dq 0
    
    ; 64-bit code segment (selector 0x08) - same as kernel!
    dw 0xFFFF       ; Limit low (ignored in long mode)
    dw 0x0000       ; Base low
    db 0x00         ; Base mid
    db 0x9A         ; Access: present, ring 0, executable, readable
    db 0xAF         ; Granularity: 4K, long mode, limit high (matches kernel)
    db 0x00         ; Base high
    
    ; Data segment (selector 0x10) - same as kernel!
    dw 0xFFFF       ; Limit low
    dw 0x0000       ; Base low
    db 0x00         ; Base mid
    db 0x92         ; Access: present, ring 0, writable
    db 0xCF         ; Granularity: 4K, 32-bit, limit high (matches kernel)
    db 0x00         ; Base high
    
    ; 32-bit code segment (selector 0x18) - for PM transition only
    dw 0xFFFF       ; Limit low
    dw 0x0000       ; Base low
    db 0x00         ; Base mid
    db 0x9A         ; Access: present, ring 0, executable, readable
    db 0xCF         ; Granularity: 4K, 32-bit, limit high
    db 0x00         ; Base high
    
    ; 32-bit data segment (selector 0x20) - for PM transition only
    dw 0xFFFF       ; Limit low
    dw 0x0000       ; Base low
    db 0x00         ; Base mid
    db 0x92         ; Access: present, ring 0, writable
    db 0xCF         ; Granularity: 4K, 32-bit, limit high
    db 0x00         ; Base high
gdt_end:

; Padding to align gdt_ptr at offset 0x100
times (0x100 - ($ - ap_trampoline_start)) db 0

gdt_ptr:                             ; Offset 0x100
    dw gdt_end - gdt_start - 1  ; GDT limit
gdt_base:                            ; Offset 0x102
    dd 0            ; GDT base - patched at runtime

; Padding to ensure ap_pml4 is at offset 0x108
times (0x108 - ($ - ap_trampoline_start)) db 0

; Data area (these will be filled in by smp_boot_aps)
ap_pml4:        ; Offset 0x108 from start
    dq 0        ; PML4 physical address
    
ap_stack:       ; Offset 0x110 from start
    dq 0        ; Stack pointer for this AP
    
ap_cpu_id:      ; Offset 0x118 from start  
    dd 0        ; CPU ID for this AP
    dd 0        ; Padding
    
ap_entry_addr:  ; Offset 0x120 from start
    dq 0        ; Address of ap_entry() function

ap_trampoline_end:

; Export offsets for C code
ap_trampoline_pml4      equ 0x108
ap_trampoline_stack     equ 0x110
ap_trampoline_cpu_id    equ 0x118
ap_trampoline_entry     equ 0x120
ap_trampoline_gdt       equ gdt_ptr - ap_trampoline_start
