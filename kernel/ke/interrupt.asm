; LikeOS-64 Interrupt Handlers
; Assembly stubs for 64-bit interrupt handling

[BITS 64]

section .text

; External C functions
extern exception_handler
extern irq_handler

; Export symbols
global idt_flush

; Load IDT descriptor
idt_flush:
    mov rax, rdi    ; Get IDT descriptor pointer (first parameter in RDI)
    lidt [rax]      ; Load IDT
    ret

; Common interrupt handler macro
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli                 ; Disable interrupts
    push 0              ; Push dummy error code
    push %1             ; Push interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli                 ; Disable interrupts
    push %1             ; Push interrupt number
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    cli
    push 0              ; Push dummy error code
    push %2             ; Push IRQ number
    jmp irq_common_stub
%endmacro

extern exception_handler
extern irq_handler

; Common ISR stub for exceptions
isr_common_stub:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Call C exception handler
    mov rdi, rsp               ; Pass pointer to register structure
    call exception_handler
    
    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Clean up error code and interrupt number
    add rsp, 16
    
    ; Return from interrupt (interrupts automatically re-enabled)
    sti
    iretq

; Common IRQ stub
irq_common_stub:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Call C IRQ handler
    mov rdi, rsp               ; Pass pointer to register structure
    call irq_handler
    
    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Clean up error code and interrupt number
    add rsp, 16
    
    ; Return from interrupt (interrupts automatically re-enabled)
    iretq

; CPU Exception handlers
ISR_NOERRCODE 0         ; Division by zero
ISR_NOERRCODE 1         ; Debug exception
ISR_NOERRCODE 2         ; Non-maskable interrupt
ISR_NOERRCODE 3         ; Breakpoint exception
ISR_NOERRCODE 4         ; Into detected overflow
ISR_NOERRCODE 5         ; Out of bounds exception
ISR_NOERRCODE 6         ; Invalid opcode exception
ISR_NOERRCODE 7         ; No coprocessor exception
ISR_ERRCODE   8         ; Double fault
ISR_NOERRCODE 9         ; Coprocessor segment overrun
ISR_ERRCODE   10        ; Bad TSS
ISR_ERRCODE   11        ; Segment not present
ISR_ERRCODE   12        ; Stack fault
ISR_ERRCODE   13        ; General protection fault
ISR_ERRCODE   14        ; Page fault
ISR_NOERRCODE 15        ; Unknown interrupt exception
ISR_NOERRCODE 16        ; Coprocessor fault
ISR_ERRCODE   17        ; Alignment check exception
ISR_NOERRCODE 18        ; Machine check exception
ISR_NOERRCODE 19        ; Reserved
ISR_NOERRCODE 20        ; Reserved
ISR_NOERRCODE 21        ; Reserved
ISR_NOERRCODE 22        ; Reserved
ISR_NOERRCODE 23        ; Reserved
ISR_NOERRCODE 24        ; Reserved
ISR_NOERRCODE 25        ; Reserved
ISR_NOERRCODE 26        ; Reserved
ISR_NOERRCODE 27        ; Reserved
ISR_NOERRCODE 28        ; Reserved
ISR_NOERRCODE 29        ; Reserved
ISR_NOERRCODE 30        ; Reserved
ISR_NOERRCODE 31        ; Reserved

; IRQ handlers (32-47)
IRQ 0, 32              ; Timer
IRQ 1, 33              ; Keyboard
IRQ 2, 34              ; Cascade
IRQ 3, 35              ; COM2
IRQ 4, 36              ; COM1
IRQ 5, 37              ; LPT2
IRQ 6, 38              ; Floppy disk
IRQ 7, 39              ; LPT1
IRQ 8, 40              ; CMOS clock
IRQ 9, 41              ; Free
IRQ 10, 42              ; Free
IRQ 11, 43              ; Free
IRQ 12, 44              ; PS/2 mouse
IRQ 13, 45              ; FPU
IRQ 14, 46              ; Primary ATA
IRQ 15, 47              ; Secondary ATA

; ============================================================================
; IPI (Inter-Processor Interrupt) Handlers for SMP
; These are high-vector interrupts sent between CPUs via LAPIC
; ============================================================================

extern ipi_handler

; Common IPI stub - same register save/restore as IRQ but calls ipi_handler
ipi_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp               ; Pass pointer to register structure
    call ipi_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                ; Clean up vector number + dummy error code
    iretq

; Macro for IPI vector stubs (no error code, push vector number)
%macro IPI_STUB 1
global ipi_vector_%1
ipi_vector_%1:
    cli
    push 0              ; Dummy error code
    push %1             ; Push vector number
    jmp ipi_common_stub
%endmacro

; IPI vector stubs
IPI_STUB 0xFC           ; TLB shootdown
IPI_STUB 0xFD           ; Halt CPU
IPI_STUB 0xFE           ; Reschedule
IPI_STUB 0xFF           ; LAPIC spurious interrupt