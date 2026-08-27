; LikeOS-64 SYSCALL/SYSRET Entry Point

BITS 64
SECTION .text

extern syscall_handler
global syscall_entry

; Per-CPU offsets into percpu_t (must match struct percpu in percpu.h)
; These fields are right after the self-pointer at GS:0.
%define PERCPU_SYSCALL_KRSP         8    ; percpu_t::syscall_kernel_rsp
%define PERCPU_SYSCALL_URSP         16   ; percpu_t::syscall_user_rsp
%define PERCPU_SAVED_USER_RIP       24   ; percpu_t::syscall_saved_user_rip
%define PERCPU_SAVED_USER_RFLAGS    32   ; percpu_t::syscall_saved_user_rflags
%define PERCPU_SAVED_USER_RBP       40   ; percpu_t::syscall_saved_user_rbp
%define PERCPU_SAVED_USER_RBX       48   ; percpu_t::syscall_saved_user_rbx
%define PERCPU_SAVED_USER_R12       56   ; percpu_t::syscall_saved_user_r12
%define PERCPU_SAVED_USER_R13       64   ; percpu_t::syscall_saved_user_r13
%define PERCPU_SAVED_USER_R14       72   ; percpu_t::syscall_saved_user_r14
%define PERCPU_SAVED_USER_R15       80   ; percpu_t::syscall_saved_user_r15
%define PERCPU_SAVED_USER_RAX       88   ; percpu_t::syscall_saved_user_rax
%define PERCPU_SIGNAL_PENDING       96   ; percpu_t::syscall_signal_pending
; The rest of the user register file, saved only for sigreturn.  A syscall
; return may clobber these; a signal return may not.  See percpu.h.
%define PERCPU_SAVED_USER_RDI       112
%define PERCPU_SAVED_USER_RSI       120
%define PERCPU_SAVED_USER_RDX       128
%define PERCPU_SAVED_USER_RCX       136
%define PERCPU_SAVED_USER_R8        144
%define PERCPU_SAVED_USER_R9        152
%define PERCPU_SAVED_USER_R10       160
%define PERCPU_SAVED_USER_R11       168
%define PERCPU_SYSCALL_FRAME        176  ; percpu_t::syscall_user_frame
; Selectors for the IRETQ frame, matching the ones sched.c builds for a new
; task (CS 0x23, SS 0x1B).
%define USER_CS_SEL                 0x23
%define USER_SS_SEL                 0x1B

SECTION .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top:

SECTION .data
; Saved kernel RSP before syscall_handler call (for restoring after context switch)
syscall_saved_ksp:
    dq 0

; NOTE: The following global variables are DEPRECATED and kept only for backward
; compatibility with C code that hasn't been updated yet. The assembly code now
; uses per-CPU storage via GS: prefix for SMP safety.
global syscall_saved_user_rip
global syscall_saved_user_rsp
global syscall_saved_user_rflags
global syscall_saved_user_rbp
global syscall_saved_user_rbx
global syscall_saved_user_r12
global syscall_saved_user_r13
global syscall_saved_user_r14
global syscall_saved_user_r15
global syscall_saved_user_rax
syscall_saved_user_rip:
    dq 0
syscall_saved_user_rsp:
    dq 0
syscall_saved_user_rflags:
    dq 0
syscall_saved_user_rbp:
    dq 0
syscall_saved_user_rbx:
    dq 0
syscall_saved_user_r12:
    dq 0
syscall_saved_user_r13:
    dq 0
syscall_saved_user_r14:
    dq 0
syscall_saved_user_r15:
    dq 0
syscall_saved_user_rax:
    dq 0

; Signal delivery: per-CPU via GS:PERCPU_SIGNAL_PENDING (global kept for compat)
global syscall_signal_pending
syscall_signal_pending:
    dq 0

SECTION .text

syscall_entry:
    mov [gs:PERCPU_SYSCALL_URSP], rsp
    ; Use per-CPU kernel stack (set by context switch on this CPU)
    mov rsp, [gs:PERCPU_SYSCALL_KRSP]
    ; DF=0 for kernel C code.  SYSCALL clears only the RFLAGS bits named in
    ; the FMASK MSR, and DF is not among them -- so a syscall issued while the
    ; caller had DF=1 (mid-`std`, e.g. a backward memmove) would run the whole
    ; kernel with backward string ops.  The user's DF returns via R11/sysret.
    cld

    push qword [gs:PERCPU_SYSCALL_URSP]
    push r11
    push rcx

    ; Save user context to PER-CPU storage for fork() and signal handling
    ; At this point: RCX = user RIP, R11 = user RFLAGS
    ; Using GS-relative addressing for SMP safety (each CPU has its own copy)
    mov [gs:PERCPU_SAVED_USER_RIP], rcx        ; RCX = user RIP
    mov [gs:PERCPU_SAVED_USER_RFLAGS], r11     ; R11 = user RFLAGS
    push rax                                    ; Temporarily save syscall number
    mov rax, [gs:PERCPU_SYSCALL_URSP]
    ; Note: user RSP is already in PERCPU_SYSCALL_URSP, no need to duplicate
    pop rax                                     ; Restore syscall number

    ; Save callee-saved registers to per-CPU storage for fork()
    mov [gs:PERCPU_SAVED_USER_RBP], rbp
    mov [gs:PERCPU_SAVED_USER_RBX], rbx
    mov [gs:PERCPU_SAVED_USER_R12], r12
    mov [gs:PERCPU_SAVED_USER_R13], r13
    mov [gs:PERCPU_SAVED_USER_R14], r14
    mov [gs:PERCPU_SAVED_USER_R15], r15

    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15

    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    ; Use RBP as frame pointer to remember where our saved registers are
    ; This way we can find them even after sched_yield context switches
    mov rbp, rsp                               ; RBP = top of our saved register area

    ; Publish that frame so C can read the user's ARGUMENT registers, which
    ; nothing else preserves: the per-CPU slots above keep only what a syscall
    ; or a signal return needs.  A debugger inspecting a tracee stopped inside
    ; a syscall has to see rdi/rsi/rdx/r8/r9/r10, and they live only here.
    ; Consumed by syscall_handler while interrupts are still off, because this
    ; slot belongs to the CPU and not to the task.
    mov [gs:PERCPU_SYSCALL_FRAME], rbp

    and rsp, ~0xF
    ; Pass the user's 6th syscall argument (original r9, saved at [rbp+40] per
    ; the stack layout documented in the return path) as the 7th C argument on
    ; the stack.  Needed for mmap's `offset`; the register shuffle above only
    ; delivers a1..a5 in registers.  Keep 16-byte alignment: after `sub 8`
    ; then `push`, rsp is 16-aligned at the `call`.  The pushed value is
    ; discarded by the `mov rsp, rbp` on every return path.
    mov rax, [rbp + 40]                        ; user arg6 (saved r9)
    sub rsp, 8
    push rax                                   ; C 7th arg (a6) at [rsp]
    ; NOTE: Interrupts remain DISABLED here. syscall_handler will enable them
    ; AFTER copying per-CPU values to task-local storage (to prevent race condition
    ; where another task overwrites our per-CPU data before we read it).
    call syscall_handler
    cli                                        ; Disable interrupts for return path

    ; Check if signal delivery modified the return context
    ; If per-CPU syscall_signal_pending is non-zero, use modified context for signal handler
    mov rdi, [gs:PERCPU_SIGNAL_PENDING]
    test rdi, rdi
    jnz .signal_return

    ; Normal syscall return path
    ; Restore RSP from RBP (which was preserved through any context switches
    ; since it's a callee-saved register)
    mov rsp, rbp

    ; Store return value where rax will be popped from
    ; Stack layout (from rsp):
    ;   0: r15, 8: r14, 16: r13, 24: r12, 32: r10, 40: r9, 48: r8
    ;   56: rbp, 64: rdi, 72: rsi, 80: rdx, 88: rbx, 96: rax
    ;   104: rcx, 112: r11, 120: user_rsp
    mov [rsp + 12*8], rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax

    pop rcx
    pop r11
    pop rsp

    o64 sysret

.signal_return:
    ; Signal handler or sigreturn path
    ; RDI has value from per-CPU syscall_signal_pending
    ; If RDI == -1 (0xFFFFFFFFFFFFFFFF), this is sigreturn - don't set RDI to signal
    ; Otherwise, RDI is the signal number for the handler

    ; Clear the per-CPU pending flag
    xor rax, rax
    mov [gs:PERCPU_SIGNAL_PENDING], rax

    ; Check if this is sigreturn (RDI == -1) or signal delivery
    cmp rdi, -1
    je .sigreturn_restore

    ; Signal handler call - RDI already has signal number
    ; Load modified context from per-CPU saved variables
    mov rcx, [gs:PERCPU_SAVED_USER_RIP]       ; Handler address -> RCX for SYSRET
    mov r11, [gs:PERCPU_SAVED_USER_RFLAGS]    ; RFLAGS -> R11 for SYSRET
    mov rsp, [gs:PERCPU_SYSCALL_URSP]         ; Signal frame on stack (stored in user_rsp)

    ; Restore callee-saved registers (handler expects these)
    mov rbp, [gs:PERCPU_SAVED_USER_RBP]
    mov rbx, [gs:PERCPU_SAVED_USER_RBX]
    mov r12, [gs:PERCPU_SAVED_USER_R12]
    mov r13, [gs:PERCPU_SAVED_USER_R13]
    mov r14, [gs:PERCPU_SAVED_USER_R14]
    mov r15, [gs:PERCPU_SAVED_USER_R15]

    ; Handler arguments: RDI is the signal number (loaded above); RSI and
    ; RDX carry the siginfo pointer and (NULL) ucontext that signal_setup_frame
    ; left in the saved-RSI/RDX slots.  Everything else is cleared.
    mov rsi, [gs:PERCPU_SAVED_USER_RSI]
    mov rdx, [gs:PERCPU_SAVED_USER_RDX]
    xor rax, rax
    xor r8, r8
    xor r9, r9
    xor r10, r10

    o64 sysret

.sigreturn_restore:
    ; Sigreturn: restore the COMPLETE user context, via IRETQ.
    ;
    ; SYSRET cannot be used here.  It takes the return RIP from RCX and RFLAGS
    ; from R11, so those two registers are destroyed by the return itself --
    ; and the code that was interrupted is not at a syscall boundary, so every
    ; register is live.  The previous version went further and zeroed
    ; RDI/RSI/RDX/R8/R9/R10 on the way out, which silently corrupted whatever
    ; the program was doing: a signal arriving between a register load and its
    ; use handed the interrupted function a zero.  xterm died calling
    ; VTRun(NULL) because RDI was wiped between the compare and the call, and
    ; the window for it is every instruction that is not a syscall.
    ;
    ; IRETQ reads RIP, CS, RFLAGS, RSP and SS from the stack and leaves the
    ; general-purpose registers alone, so all of them can be restored first.
    ;
    ; Still on the kernel stack here; the frame pushed below is consumed by the
    ; IRETQ, and the next syscall reloads RSP from PERCPU_SYSCALL_KRSP anyway.

    mov rax, [gs:PERCPU_SYSCALL_URSP]         ; user RSP for the IRET frame
    push qword USER_SS_SEL
    push rax
    push qword [gs:PERCPU_SAVED_USER_RFLAGS]
    push qword USER_CS_SEL
    push qword [gs:PERCPU_SAVED_USER_RIP]

    ; With the frame built, no GPR is needed any more: restore them all.
    mov rdi, [gs:PERCPU_SAVED_USER_RDI]
    mov rsi, [gs:PERCPU_SAVED_USER_RSI]
    mov rdx, [gs:PERCPU_SAVED_USER_RDX]
    mov rcx, [gs:PERCPU_SAVED_USER_RCX]
    mov r8,  [gs:PERCPU_SAVED_USER_R8]
    mov r9,  [gs:PERCPU_SAVED_USER_R9]
    mov r10, [gs:PERCPU_SAVED_USER_R10]
    mov r11, [gs:PERCPU_SAVED_USER_R11]
    mov rbp, [gs:PERCPU_SAVED_USER_RBP]
    mov rbx, [gs:PERCPU_SAVED_USER_RBX]
    mov r12, [gs:PERCPU_SAVED_USER_R12]
    mov r13, [gs:PERCPU_SAVED_USER_R13]
    mov r14, [gs:PERCPU_SAVED_USER_R14]
    mov r15, [gs:PERCPU_SAVED_USER_R15]
    mov rax, [gs:PERCPU_SAVED_USER_RAX]

    iretq

; IRET trampoline for user mode entry
global user_mode_iret_trampoline
user_mode_iret_trampoline:
    iretq

; Fork child return trampoline
; Stack layout when we get here (from RSP upward):
;   RAX value (0)
;   IRET frame: RIP, CS, RFLAGS, RSP, SS
;   User callee-saved: RBP, RBX, R12, R13, R14, R15
; We need to pop RAX, then load the callee-saved regs (after IRET frame), then iretq
global fork_child_return
extern sched_after_fork_child
fork_child_return:
    ; We just arrived from ctx_switch_asm (fresh fork/clone child, never
    ; scheduled before).  The scheduling function set in_context_switch = 1
    ; before ctx_switch_asm but the normal post-switch cleanup was never
    ; executed because we diverged here.  Call the C helper to clear the
    ; flag and process deferred zombies before entering user mode.
    ; This must happen before iretq enables interrupts (IF in RFLAGS),
    ; otherwise the first timer tick on this CPU would see
    ; in_context_switch == 1 and skip preemption forever.
    call sched_after_fork_child

    pop rax        ; Get fork return value (0)

    ; IRET frame starts at RSP (5 qwords: RIP, CS, RFLAGS, RSP, SS)
    ; User callee-saved regs are at RSP + 40 (after the 5 qwords)
    ; Order on stack: RBP, RBX, R12, R13, R14, R15 (pushed in reverse, so RBP is first)
    mov rbp, [rsp + 40]
    mov rbx, [rsp + 48]
    mov r12, [rsp + 56]
    mov r13, [rsp + 64]
    mov r14, [rsp + 72]
    mov r15, [rsp + 80]

    ; Clear volatile registers to prevent leaking kernel addresses to userspace.
    ; These regs are caller-saved so user code doesn't depend on their values,
    ; but leaving kernel pointers in them is an information leak.
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d

    iretq          ; Return to userspace

; Context switch between tasks
;
; RFLAGS is saved/restored along with the callee-saved GPRs because it holds
; per-task kernel state that the SysV ABI does not preserve across a call --
; specifically EFLAGS.AC, the SMAP override set by smap_disable()/stac.  Kernel
; code holds AC=1 across sleeping work (ext4/pagecache/USB read+write wrap their
; whole copy loop in smap_disable), and every sleep lands here.  Without saving
; it, AC leaks into the next task (silently disabling SMAP for unrelated kernel
; code) and, worse, is LOST on resume -- the interrupted copy then takes a SMAP
; #PF on a present user page and the process is killed with SIGSEGV.  A voluntary
; sleep has no IRET to restore RFLAGS for it, so it has to be saved here.
;
; IF is saved/restored with it, which is safe because each task now restores the
; IF *it* switched away with instead of inheriting whatever the previous task
; left.  Note the call sites differ: sched_schedule and sched_run_ready sti
; BEFORE ctx_switch_asm (IF=1), while sched_preempt switches from a hardware
; interrupt (IF=0) and lets its iretq epilogue restore RFLAGS.  Both resume
; correctly under popfq because the value is per-task.
;
; Hand-built frames for fresh tasks must push a matching RFLAGS slot between the
; return address and RBP -- see sched_add_task, sched_add_user_task,
; sched_init_ap, sys_fork and sys_clone.  They use 0x202 (reserved bit 1 + IF):
; a fresh task diverges to a trampoline and never reaches a post-switch sti, so
; IF has to be live in the frame or the task runs with interrupts off forever.
; This also makes the start-up IF deterministic -- previously a fresh task
; inherited IF=0 when it happened to be started from sched_preempt.
global ctx_switch_asm
ctx_switch_asm:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq

    ret
