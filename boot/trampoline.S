# Trampoline code for transitioning to higher half kernel
# This code runs in low memory (identity-mapped) and:
# 1. Loads the new page tables into CR3
# 2. Jumps to the kernel's higher half virtual address
#
# Note: NX bit is NOT enabled here because the bootloader page tables
# were set up without NX considerations. The kernel can enable NX later
# after setting up proper page tables with correct NX flags.
#
# Function signature:
# void trampoline_jump(uint64_t kernel_entry, void* boot_info, uint64_t pml4_addr);
#
# Arguments:
#   RDI = kernel_entry (virtual address)
#   RSI = boot_info (pointer)
#   RDX = pml4_addr (physical address of page tables)

.section .text
.global trampoline_jump
.type trampoline_jump, @function

trampoline_jump:
    # Save arguments to callee-saved registers
    mov %rdi, %rax          # Save kernel_entry in RAX
    mov %rsi, %rbx          # Save boot_info in RBX
    mov %rdx, %rcx          # Save pml4_addr in RCX
    
    # Enable PGE (Page Global Enable) in CR4
    # SMEP/SMAP are deferred to kernel initialization where we can safely
    # check CPUID and have a proper execution environment
    mov %cr4, %rdx
    or $0x80, %rdx          # Set bit 7 (PGE - Page Global Enable)
    mov %rdx, %cr4
    
    # Note: NX enablement is deferred to kernel initialization
    # because current page tables don't have NX flags properly configured.
    # Enabling NX here would cause all code pages to become non-executable.

    # Load new page tables into CR3
    mov %rcx, %cr3          # Load PML4 address into CR3

    # Flush TLB by reloading CR3
    mov %cr3, %rdx
    mov %rdx, %cr3
    
    # Set up function call for kernel
    # Kernel expects: void kernel_main(boot_info_t* boot_info)
    mov %rbx, %rdi          # Move boot_info to first argument register
    
    # Jump to kernel entry point (higher half virtual address)
    jmp *%rax               # Indirect jump to kernel_entry
    
    # Should never reach here
    hlt

.size trampoline_jump, . - trampoline_jump

.global trampoline_jump_end
trampoline_jump_end:
