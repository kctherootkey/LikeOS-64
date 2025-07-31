; LikeOS-64 Bootloader
; A minimal bootloader that switches to 64-bit long mode and loads a kernel
; Assembled with NASM

[BITS 16]
[ORG 0x7C00]

start:
    ; Clear interrupts and setup segments
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; Stack grows downward from bootloader

    ; Print boot message
    mov si, boot_msg
    call print_string

    ; Load kernel from disk - VMware and QEMU compatibility
    mov dl, 0x00            ; Try floppy first (VMware)
    mov ah, 0x02
    mov al, 64              ; Load 64 sectors (32KB) to accommodate larger kernel
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov bx, 0x1000
    mov es, bx
    xor bx, bx
    int 0x13
    jnc .disk_loaded        ; Success with floppy
    
    ; Floppy failed, try hard disk (QEMU)
    mov dl, 0x80
    mov ah, 0x02
    mov al, 64              ; Load 64 sectors (32KB) to accommodate larger kernel
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov bx, 0x1000
    mov es, bx
    xor bx, bx
    int 0x13
    jc disk_error           ; Both failed
    
.disk_loaded:

    ; Setup GDT
    lgdt [gdt_descriptor]

    ; Enable A20 line (required for >1MB access)
    call enable_a20

    ; VMware-compatible protected mode transition
    ; Ensure interrupts are disabled and clear prefetch queue
    cli                     ; Ensure interrupts disabled
    
    ; Clear any pending interrupts and serialize instruction execution
    jmp $+2                 ; Short jump to clear prefetch queue
    
    ; Switch to protected mode
    mov eax, cr0
    or eax, 1               ; Set PE (Protection Enable) bit
    mov cr0, eax            ; Enter protected mode

    ; Immediate far jump to flush CPU pipeline and load new CS
    jmp CODE_SEG:protected_mode

disk_error:
    mov si, disk_error_msg
    call print_string
    hlt

enable_a20:
    ; Fast A20 Gate method (port 0x92) - most reliable for virtualization
    ; Check if A20 is already enabled first
    call check_a20
    cmp ax, 1
    je .a20_enabled
    
    ; Enable A20 using Fast A20 Gate
    in al, 0x92             ; Read from Fast A20 port
    test al, 2              ; Check if already enabled
    jnz .a20_enabled        ; Already enabled, skip
    or al, 2                ; Set bit 1 (A20 gate)
    out 0x92, al            ; Write back to enable A20
    
    ; Small delay to let A20 stabilize
    mov cx, 1000
.delay:
    nop
    loop .delay
    
.a20_enabled:
    ret

check_a20:
    ; Simple A20 test - check if we can access high memory
    push ds
    push es
    push si
    push di
    
    ; Point to low memory
    xor ax, ax
    mov ds, ax
    mov si, 0x7DFE
    
    ; Point to high memory (will wrap if A20 disabled)
    mov ax, 0xFFFF
    mov es, ax
    mov di, 0x7E0E
    
    ; Save original values
    mov al, [ds:si]
    mov ah, [es:di]
    
    ; Test with different values
    mov byte [ds:si], 0x55
    mov byte [es:di], 0xAA
    
    ; Check if they're different (A20 enabled)
    mov bl, [ds:si]
    mov bh, [es:di]
    
    ; Restore original values
    mov [ds:si], al
    mov [es:di], ah
    
    ; Return result
    cmp bl, bh
    je .a20_disabled
    mov ax, 1               ; A20 enabled
    jmp .done
.a20_disabled:
    mov ax, 0               ; A20 disabled
.done:
    pop di
    pop si
    pop es
    pop ds
    ret

print_string:
    mov ah, 0x0E
.loop:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .loop
.done:
    ret

; 32-bit protected mode code
[BITS 32]
protected_mode:
    ; Setup segments for protected mode
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000        ; Stack at 640KB

    ; Setup paging for long mode
    ; Clear page tables area
    mov edi, 0x70000        ; Page tables at 0x70000
    mov ecx, 0x3000 / 4     ; Clear 12KB (3 page tables)
    xor eax, eax
    rep stosd

    ; Setup page directory pointer table (PML4)
    mov dword [0x70000], 0x71003    ; Point to PDPT

    ; Setup page directory pointer (PDPT)
    mov dword [0x71000], 0x72003    ; Point to PD

    ; Setup page directory (PD) - identity map first 12MB
    mov dword [0x72000], 0x000083   ; 0-2MB
    mov dword [0x72008], 0x200083   ; 2-4MB
    mov dword [0x72010], 0x400083   ; 4-6MB
    mov dword [0x72018], 0x600083   ; 6-8MB
    mov dword [0x72020], 0x800083   ; 8-10MB
    mov dword [0x72028], 0xa00083   ; 10-12MB

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Set page table address
    mov eax, 0x70000
    mov cr3, eax

    ; Enable long mode
    mov ecx, 0xC0000080     ; EFER MSR
    rdmsr
    or eax, 1 << 8          ; Long mode enable
    wrmsr

    ; Enable paging (this activates long mode)
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Jump to 64-bit code
    jmp CODE64_SEG:long_mode

; 64-bit long mode code
[BITS 64]
long_mode:
    ; Update segment registers for long mode
    mov ax, DATA64_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Copy kernel to final location (1MB mark)
    mov rsi, 0x10000        ; Source: where we loaded kernel
    mov rdi, 0x100000       ; Destination: 1MB mark
    mov rcx, 0x8000         ; Copy 32KB (increased from 16KB)
    rep movsb
    
    ; If we reach here, kernel is properly loaded at 0x100000
    mov rax, 0x100000
    jmp rax

; GDT (Global Descriptor Table)
gdt_start:
    ; Null descriptor
    dq 0

gdt_code_32:
    ; 32-bit code segment
    dw 0xFFFF               ; Limit low
    dw 0x0000               ; Base low
    db 0x00                 ; Base middle
    db 10011010b            ; Access byte
    db 11001111b            ; Granularity
    db 0x00                 ; Base high

gdt_data_32:
    ; 32-bit data segment
    dw 0xFFFF               ; Limit low
    dw 0x0000               ; Base low
    db 0x00                 ; Base middle
    db 10010010b            ; Access byte
    db 11001111b            ; Granularity
    db 0x00                 ; Base high

gdt_code_64:
    ; 64-bit code segment
    dw 0x0000               ; Limit low (ignored)
    dw 0x0000               ; Base low (ignored)
    db 0x00                 ; Base middle (ignored)
    db 10011010b            ; Access byte
    db 10100000b            ; Granularity (L=1, D=0)
    db 0x00                 ; Base high (ignored)

gdt_data_64:
    ; 64-bit data segment
    dw 0x0000               ; Limit low (ignored)
    dw 0x0000               ; Base low (ignored)
    db 0x00                 ; Base middle (ignored)
    db 10010010b            ; Access byte
    db 00000000b            ; Granularity
    db 0x00                 ; Base high (ignored)

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; GDT size
    dd gdt_start                ; GDT address

; Constants for segment selectors
CODE_SEG    equ gdt_code_32 - gdt_start
DATA_SEG    equ gdt_data_32 - gdt_start
CODE64_SEG  equ gdt_code_64 - gdt_start
DATA64_SEG  equ gdt_data_64 - gdt_start

; Messages
boot_msg        db 'LikeOS-64 Bootloader Starting...', 13, 10, 0
disk_error_msg  db 'Disk read error!', 13, 10, 0

; Pad to 510 bytes and add boot signature
times 510-($-$$) db 0
dw 0xAA55
