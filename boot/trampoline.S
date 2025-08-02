# Trampoline code for transitioning to higher half kernel
# This code runs in low memory (identity-mapped) and:
# 1. Loads the new page tables into CR3
# 2. Jumps to the kernel's higher half virtual address
#
# Function signature:
# void trampoline_jump(uint64_t kernel_entry, void* framebuffer_info, uint64_t pml4_addr);
#
# Arguments:
#   RDI = kernel_entry (virtual address)
#   RSI = framebuffer_info (pointer)
#   RDX = pml4_addr (physical address of page tables)

.section .text
.global trampoline_jump
.type trampoline_jump, @function

trampoline_jump:
    # Save arguments in non-volatile registers
    mov %rdi, %rax          # Save kernel_entry in RAX
    mov %rsi, %rbx          # Save framebuffer_info in RBX
    mov %rdx, %rcx          # Save pml4_addr in RCX
    
    # Enable PGE (Page Global Enable) in CR4
    mov %cr4, %rdx          # Read current CR4 value
    or $0x80, %rdx          # Set bit 7 (PGE)
    mov %rdx, %cr4          # Write back to CR4
    
    # Enable NX (No-Execute) bit in EFER MSR
    #mov $0xC0000080, %ecx   # EFER MSR number
    #rdmsr                   # Read EFER into EDX:EAX
    #or $0x800, %eax         # Set bit 11 (NXE - No-Execute Enable)
    #wrmsr                   # Write back to EFER

    # Load new page tables into CR3
    mov %rcx, %cr3          # Load PML4 address into CR3

    # Flush TLB by reloading CR3
    mov %cr3, %rdx
    mov %rdx, %cr3
    
    # Set up function call for kernel
    # Kernel expects: void kernel_main(framebuffer_info_t* fb_info)
    mov %rbx, %rdi          # Move framebuffer_info to first argument register
    
    # Jump to kernel entry point (higher half virtual address)
    jmp *%rax               # Indirect jump to kernel_entry
    
    # Should never reach here
    hlt

.size trampoline_jump, . - trampoline_jump

.global trampoline_jump_end
trampoline_jump_end:
