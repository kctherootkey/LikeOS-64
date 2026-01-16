#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/scrollbar.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/ps2.h"
#include "../../include/kernel/ioapic.h"
#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/storage.h"
#include "../../include/kernel/shell.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/sched.h"

// Function prototypes
void KiSystemStartup(boot_info_t* boot_info);
void kernel_main(boot_info_t* boot_info);
static void spawn_user_test_task(void);
static void test_direct_iret_to_user(void);

// External symbols for user test program (from user_test.asm)
extern char user_test_start[];
extern char user_test_end[];

static xhci_boot_state_t g_xhci_boot;
static storage_fs_state_t g_storage_state;
static boot_info_t* g_boot_info;  // Global boot info pointer

// UEFI kernel entry point - called by bootloader
void kernel_main(boot_info_t* boot_info) {
    // Store boot info globally
    g_boot_info = boot_info;
    
    // Initialize console system with framebuffer info
    // Cast boot_framebuffer_info_t to framebuffer_info_t (compatible structure)
    console_init((framebuffer_info_t*)&boot_info->fb_info);
    // Initialize framebuffer optimization after console is ready
    console_init_fb_optimization();
    // Call the main system initialization
    KiSystemStartup(boot_info);
}

// Kernel Executive entry point
void KiSystemStartup(boot_info_t* boot_info) {
    // Console is already initialized by kernel_main()

    kprintf("\nLikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");
    kprintf("Higher Half Kernel loaded at virtual address %p\n", (void*)KiSystemStartup);

    console_set_color(10, 0); // Light Green on Black
    kprintf("\nKernel initialization complete!\n");
    console_set_color(15, 0); // White on Black

    // Initialize interrupt system
    kprintf("\nInitializing interrupt system...\n");
    interrupts_init();
    kprintf("Interrupt system initialized successfully\n");

    // Initialize memory management subsystem using boot info
    kprintf("\nInitializing Memory Management Subsystem...\n");
    kprintf("Boot info: %d memory map entries, %lu MB total usable\n",
            boot_info->mem_info.entry_count,
            boot_info->mem_info.total_memory / (1024 * 1024));
    
    // Use actual memory size from boot info, with 256MB minimum
    uint64_t memory_size = boot_info->mem_info.total_memory;
    if (memory_size < 256 * 1024 * 1024) {
        memory_size = 256 * 1024 * 1024;
    }
    
    MmDetectMemory();
    MmInitializePhysicalMemory(memory_size);
    MmInitializeVirtualMemory();
    MmInitializeHeap();
    MmPrintMemoryStats();
    
    // Initialize SYSCALL/SYSRET for user mode support
    kprintf("\nInitializing SYSCALL/SYSRET...\n");
    MmInitializeSyscall();

    // Initialize PCI and enumerate devices
    kprintf("\nInitializing PCI subsystem...\n");
    pci_init();
    int pci_count = pci_enumerate();
    kprintf("PCI enumeration complete (%d devices)\n", pci_count);
    pci_assign_unassigned_bars();

    // Initialize VFS
    vfs_init();

    // Locate and initialize XHCI/USB stack
    xhci_boot_init(&g_xhci_boot);

    // Prepare FAT32 probing state
    storage_fs_init(&g_storage_state);

    // Test memory allocation
    kprintf("Testing memory allocation...\n");
    void* test_ptr1 = kalloc(1024);
    void* test_ptr2 = kalloc(2048);
    kprintf("  Allocated test blocks: %p (1KB), %p (2KB)\n", test_ptr1, test_ptr2);
    kfree(test_ptr1);
    kfree(test_ptr2);
    kprintf("  Test blocks freed successfully\n");

    // Display framebuffer optimization status
    kprintf("\nFramebuffer Optimization Status:\n");
    console_show_fb_status();

    // Initialize and render system scrollbar (single source of truth)
    kprintf("\nInitializing visual scrollbar system...\n");
    static scrollbar_t system_scrollbar;
    if (scrollbar_init_system_default(&system_scrollbar) == 0) {
        scrollbar_render(&system_scrollbar);
        fb_flush_dirty_regions();
        kprintf("Visual scrollbar initialized and rendered successfully\n");
    } else {
        kprintf("Warning: Failed to initialize scrollbar\n");
    }

    // Initialize legacy PS/2 controller before keyboard driver (safe no-op if absent)
    // Use PIC routing for legacy IRQs unless LAPIC is fully initialized.
    ps2_init();
    keyboard_init();
    irq_enable(0);
    irq_enable(1);
    irq_enable(2); // Cascade line for slave PIC

    // Initialize mouse
    mouse_init();
    irq_enable(12);

    // Enable interrupts after everything is initialized
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled!\n");

    // One-time software IRQ test to verify IRQ handler path
    extern volatile uint64_t g_irq0_count;
    kprintf("Testing IRQ0 delivery...\n");
    __asm__ volatile ("int $0x20");
    kprintf("IRQ0 count after test: %llu\n", g_irq0_count);

    // Initialize scheduler (cooperative mode)
    sched_init();

    // Initialize and start PIT timer (100 Hz)
    timer_init(100);
    timer_start();

    // NOTE: test_direct_iret_to_user() is a destructive test that never returns.
    // It performs a raw IRET to user mode, bypassing the scheduler entirely.
    // This breaks the kernel execution flow. Use spawn_user_test_task() instead,
    // which properly integrates with the scheduler.
    // kprintf("\n=== Direct IRET to User Mode Test ===\n");
    // test_direct_iret_to_user();

    // Clear debug registers that may have been left by UEFI
    // This prevents spurious Debug exceptions (INT 1)
    __asm__ volatile (
        "xor %%rax, %%rax\n\t"
        "mov %%rax, %%dr0\n\t"
        "mov %%rax, %%dr1\n\t"
        "mov %%rax, %%dr2\n\t"
        "mov %%rax, %%dr3\n\t"
        "mov %%rax, %%dr6\n\t"
        "mov %%rax, %%dr7\n\t"
        : : : "rax"
    );
    kprintf("Debug registers cleared\n");

    // Spawn user mode test task
    spawn_user_test_task();

    // Show ready prompt and enter shell loop
    shell_init();

    while (1) {
        __asm__ volatile ("sti");
        int handled_input = shell_tick();
        xhci_boot_poll(&g_xhci_boot);
        storage_fs_poll(&g_storage_state);
        sched_run_ready();

        static uint64_t last_irq0 = 0;
        extern volatile uint64_t g_irq0_count;
        extern volatile uint64_t g_irq1_count;
        extern volatile uint64_t g_irq12_count;
        if (g_irq0_count - last_irq0 >= 100) {
            last_irq0 = g_irq0_count;
            kprintf("[IRQ] timer=%llu keyboard=%llu mouse=%llu\n",
                    g_irq0_count, g_irq1_count, g_irq12_count);
        }

        // Halt CPU when idle to avoid spinning at 100% while waiting for input
        if (!handled_input) {
            __asm__ volatile ("sti");
            __asm__ volatile ("hlt");
        }
    }
}

// Direct test: IRET to user mode without scheduler
// This tests if IRET itself is working correctly
static void test_direct_iret_to_user(void) {
    // Create a user address space
    uint64_t* user_pml4 = MmCreateUserAddressSpace();
    if (!user_pml4) {
        kprintf("Failed to create user PML4\n");
        return;
    }
    
    // Allocate and map user code page
    #define USER_CODE_VADDR 0x400000
    uint64_t code_phys = MmAllocatePhysicalPage();
    if (!code_phys) {
        kprintf("Failed to allocate code page\n");
        return;
    }
    
    // Copy user code
    size_t user_code_size = (size_t)(user_test_end - user_test_start);
    mm_memcpy((void*)code_phys, user_test_start, user_code_size);
    
    // Map the code page
    uint64_t code_flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    MmMapPageInAddressSpace(user_pml4, USER_CODE_VADDR, code_phys, code_flags);
    
    // Map user stack
    #define USER_STACK_TOP_TEST 0x800000ULL
    MmMapUserStack(user_pml4, USER_STACK_TOP_TEST, 16 * 1024);
    
    // Verify the mappings
    uint64_t* pte = MmGetPageTableFromPml4(user_pml4, USER_CODE_VADDR, false);
    kprintf("Direct test: code PTE = %p\n", pte ? (void*)*pte : 0);
    
    // Switch to user's CR3
    kprintf("Direct test: switching CR3 to %p\n", user_pml4);
    MmSwitchAddressSpace(user_pml4);
    
    // Now do IRET directly
    // User RIP = 0x400000
    // User CS = 0x23
    // User RFLAGS = 0x202
    // User RSP = 0x7ffff8
    // User SS = 0x1b
    kprintf("Direct test: about to IRET to user mode (RIP=0x400000)\n");
    
    __asm__ volatile (
        "cli\n\t"                       // Disable interrupts
        "push $0x1b\n\t"                // SS
        "push $0x7ffff8\n\t"            // RSP
        "push $0x202\n\t"               // RFLAGS
        "push $0x23\n\t"                // CS
        "push $0x400000\n\t"            // RIP
        "iretq\n\t"
    );
    
    // Should never reach here
    kprintf("ERROR: Returned from user mode!\n");
}

// Spawn a user-mode test task
static void spawn_user_test_task(void) {
    kprintf("\n=== Spawning User Mode Test Task ===\n");
    
    // Calculate size of user test program
    size_t user_code_size = (size_t)(user_test_end - user_test_start);
    kprintf("User test code: %zu bytes at kernel %p\n", user_code_size, user_test_start);
    
    if (user_code_size == 0 || user_code_size > 4096) {
        kprintf("ERROR: Invalid user code size\n");
        return;
    }
    
    kprintf("DEBUG: About to create user address space\n");
    
    // Create a new user address space
    uint64_t* user_pml4 = MmCreateUserAddressSpace();
    if (!user_pml4) {
        kprintf("ERROR: Failed to create user address space\n");
        return;
    }
    
    kprintf("DEBUG: User address space created, about to allocate code page\n");
    
    // Allocate and map user code at a fixed user address
    #define USER_CODE_VADDR 0x400000  // Standard user code start
    
    uint64_t code_phys = MmAllocatePhysicalPage();
    if (!code_phys) {
        kprintf("ERROR: Failed to allocate physical page for user code\n");
        MmDestroyAddressSpace(user_pml4);
        return;
    }
    
    // Copy user test code to the physical page
    kprintf("DEBUG: Copying %zu bytes from %p to phys %p\n", 
            user_code_size, user_test_start, (void*)code_phys);
    mm_memcpy((void*)code_phys, user_test_start, user_code_size);
    
    // Debug: verify the copy by reading back first 8 bytes
    uint8_t* check = (uint8_t*)code_phys;
    kprintf("DEBUG: First 8 bytes at phys: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            check[0], check[1], check[2], check[3], check[4], check[5], check[6], check[7]);
    uint8_t* orig = (uint8_t*)user_test_start;
    kprintf("DEBUG: First 8 bytes at src:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
            orig[0], orig[1], orig[2], orig[3], orig[4], orig[5], orig[6], orig[7]);
    
    // Map code page as user, read-only, executable
    // NOTE: PAGE_USER must be set, and PAGE_NO_EXECUTE must NOT be set for code execution
    uint64_t code_flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;  // Writable for now to test
    kprintf("DEBUG: code_flags = %p\n", (void*)code_flags);
    if (!MmMapPageInAddressSpace(user_pml4, USER_CODE_VADDR, code_phys, code_flags)) {
        kprintf("ERROR: Failed to map user code page\n");
        MmFreePhysicalPage(code_phys);
        MmDestroyAddressSpace(user_pml4);
        return;
    }
    
    // Debug: Read back the PTE to verify
    uint64_t* pte = MmGetPageTableFromPml4(user_pml4, USER_CODE_VADDR, false);
    if (pte) {
        kprintf("DEBUG: Code page PTE = %p\n", (void*)*pte);
        if (*pte & 0x8000000000000000ULL) {
            kprintf("DEBUG: WARNING - NX bit is SET on code page!\n");
        }
    }
    
    // Debug: Check all page table levels for USER bit
    uint64_t pml4_idx = (USER_CODE_VADDR >> 39) & 0x1FF;
    uint64_t pdpt_idx = (USER_CODE_VADDR >> 30) & 0x1FF;
    uint64_t pd_idx = (USER_CODE_VADDR >> 21) & 0x1FF;
    uint64_t pt_idx = (USER_CODE_VADDR >> 12) & 0x1FF;
    kprintf("DEBUG: Indices: PML4[%d] PDPT[%d] PD[%d] PT[%d]\n", 
            (int)pml4_idx, (int)pdpt_idx, (int)pd_idx, (int)pt_idx);
    kprintf("DEBUG: PML4[%d] = %p\n", (int)pml4_idx, (void*)user_pml4[pml4_idx]);
    if (user_pml4[pml4_idx] & 1) {
        uint64_t* pdpt = (uint64_t*)(user_pml4[pml4_idx] & ~0xFFFULL);
        kprintf("DEBUG: PDPT[%d] = %p\n", (int)pdpt_idx, (void*)pdpt[pdpt_idx]);
        if (pdpt[pdpt_idx] & 1) {
            uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
            kprintf("DEBUG: PD[%d] = %p\n", (int)pd_idx, (void*)pd[pd_idx]);
        }
    }
    
    kprintf("Mapped user code at vaddr %p -> phys %p\n", 
            (void*)USER_CODE_VADDR, (void*)code_phys);
    
    // Create user stack (16KB) 
    // Use a simpler address that's easier to map
    #define USER_STACK_TOP  0x800000ULL   // 8MB - simple address in low user space
    #define USER_STACK_SIZE (16 * 1024)
    
    if (!MmMapUserStack(user_pml4, USER_STACK_TOP, USER_STACK_SIZE)) {
        kprintf("ERROR: Failed to map user stack\n");
        MmFreePhysicalPage(code_phys);
        MmDestroyAddressSpace(user_pml4);
        return;
    }
    kprintf("Mapped user stack: %p - %p (%u KB)\n", 
            (void*)(USER_STACK_TOP - USER_STACK_SIZE), 
            (void*)USER_STACK_TOP,
            USER_STACK_SIZE / 1024);
    
    // Create the user task
    task_t* user_task = sched_add_user_task(
        (task_entry_t)USER_CODE_VADDR,  // Entry point in user space
        NULL,                            // No argument
        user_pml4,                       // User's page table
        USER_STACK_TOP,                  // User stack pointer
        0                                // Kernel stack allocated by sched_add_user_task
    );
    
    if (!user_task) {
        kprintf("ERROR: Failed to create user task\n");
        MmDestroyAddressSpace(user_pml4);
        return;
    }
    
    kprintf("User test task created: id=%d, entry=%p\n", 
            user_task->id, (void*)USER_CODE_VADDR);
    kprintf("=== User task ready, will run on next schedule ===\n\n");
}

