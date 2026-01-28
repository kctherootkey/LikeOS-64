; LikeOS-64 Stack Switch Assembly
; Switches to a new stack and calls a function on it

section .text
global switch_stack_and_call

; void switch_stack_and_call(uint64_t new_rsp, void (*func)(void))
; Parameters:
;   rdi = new_rsp (new stack pointer)
;   rsi = func (function to call on new stack)
;
; This function switches to the new stack and calls the function.
; It does NOT return - the called function must continue execution.
switch_stack_and_call:
    ; Switch to the new stack
    mov rsp, rdi
    
    ; Clear rbp to mark stack bottom
    xor rbp, rbp
    
    ; Ensure stack is 16-byte aligned before call
    ; The call instruction will push 8 bytes, so we need RSP to be 8 mod 16
    and rsp, ~0xF
    sub rsp, 8
    
    ; Call the function (address is in rsi)
    call rsi
    
    ; The function should not return, but if it does, halt
.hang:
    hlt
    jmp .hang
