// LikeOS-64 ELF64 Loader Implementation
#include <kernel/elf.h>
#include <kernel/memory.h>
#include <kernel/sched.h>
#include <kernel/console.h>
#include <kernel/vfs.h>

// Validate an ELF64 static executable
int elf_validate(const void* data, size_t size) {
    if (!data || size < sizeof(Elf64_Ehdr)) {
        return -1;  // Too small
    }
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    
    // Check magic number
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return -2;  // Bad magic
    }
    
    // Check class (must be 64-bit)
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return -3;  // Not 64-bit
    }
    
    // Check endianness (must be little endian)
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return -4;  // Not little endian
    }
    
    // Check type (must be executable)
    if (ehdr->e_type != ET_EXEC) {
        return -5;  // Not executable
    }
    
    // Check machine (must be x86-64)
    if (ehdr->e_machine != EM_X86_64) {
        return -6;  // Not x86-64
    }
    
    // Check program header info
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -7;  // No program headers
    }
    
    // Validate program headers are within file
    uint64_t ph_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > size) {
        return -8;  // Program headers out of bounds
    }
    
    return 0;  // Valid
}

// Load a static ELF64 executable into user address space
int elf_load_user(const void* elf_data, size_t elf_size,
                  uint64_t* pml4, elf_load_result_t* result) {
    if (!elf_data || !pml4 || !result) {
        return -1;
    }
    
    int valid = elf_validate(elf_data, elf_size);
    if (valid != 0) {
        return valid;
    }
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf_data;
    const uint8_t* elf_bytes = (const uint8_t*)elf_data;
    
    result->entry_point = ehdr->e_entry;
    result->load_base = ~0ULL;
    result->load_end = 0;
    result->brk_start = 0;
    
    // Process program headers
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* phdr = (const Elf64_Phdr*)(elf_bytes + ehdr->e_phoff + 
                                                      i * ehdr->e_phentsize);
        
        // Only load PT_LOAD segments
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        
        // Validate segment bounds
        if (phdr->p_offset + phdr->p_filesz > elf_size) {
            return -9;  // Segment out of file bounds
        }
        
        // User space validation
        if (phdr->p_vaddr < USER_SPACE_START || 
            phdr->p_vaddr + phdr->p_memsz > USER_SPACE_END) {
            return -10;  // Invalid virtual address
        }
        
        // Track memory bounds
        if (phdr->p_vaddr < result->load_base) {
            result->load_base = phdr->p_vaddr;
        }
        uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
        if (seg_end > result->load_end) {
            result->load_end = seg_end;
        }
        
        // Calculate page flags
        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (phdr->p_flags & PF_W) {
            flags |= PAGE_WRITABLE;
        }
        if (!(phdr->p_flags & PF_X)) {
            flags |= PAGE_NO_EXECUTE;
        }
        
        // Map pages for this segment
        uint64_t vaddr_start = phdr->p_vaddr & ~0xFFFULL;  // Page-align down
        uint64_t vaddr_end = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFFULL;  // Page-align up
        
        for (uint64_t vaddr = vaddr_start; vaddr < vaddr_end; vaddr += PAGE_SIZE) {
            // Allocate physical page
            uint64_t phys = mm_allocate_physical_page();
            if (!phys) {
                return -11;  // Out of memory
            }
            
            // Zero the page via direct map
            mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
            
            // Copy data from ELF file if applicable
            uint64_t page_offset_in_seg = vaddr - phdr->p_vaddr;
            if (vaddr >= phdr->p_vaddr && page_offset_in_seg < phdr->p_filesz) {
                // Calculate how much to copy
                uint64_t copy_start = (vaddr < phdr->p_vaddr) ? 
                                      (phdr->p_vaddr - vaddr) : 0;
                uint64_t file_offset = phdr->p_offset + page_offset_in_seg;
                uint64_t remaining_filesz = phdr->p_filesz - page_offset_in_seg;
                uint64_t copy_len = (remaining_filesz > PAGE_SIZE - copy_start) ? 
                                    (PAGE_SIZE - copy_start) : remaining_filesz;
                
                if (copy_len > 0 && file_offset + copy_len <= elf_size) {
                    mm_memcpy((uint8_t*)phys_to_virt(phys) + copy_start, 
                             elf_bytes + file_offset, 
                             copy_len);
                }
            } else if (vaddr < phdr->p_vaddr) {
                // Page spans before segment start
                uint64_t offset_in_page = phdr->p_vaddr - vaddr;
                uint64_t copy_len = PAGE_SIZE - offset_in_page;
                if (copy_len > phdr->p_filesz) {
                    copy_len = phdr->p_filesz;
                }
                if (copy_len > 0) {
                    mm_memcpy((uint8_t*)phys_to_virt(phys) + offset_in_page,
                             elf_bytes + phdr->p_offset,
                             copy_len);
                }
            }
            
            // Map the page
            if (!mm_map_page_in_address_space(pml4, vaddr, phys, flags)) {
                mm_free_physical_page(phys);
                return -12;  // Failed to map
            }
        }
    }
    
    // Set brk_start to end of loaded segments (page-aligned)
    result->brk_start = (result->load_end + 0xFFF) & ~0xFFFULL;
    
    return 0;
}

// Helper: calculate string length
static size_t elf_strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

// Execute an ELF file from the filesystem
int elf_exec(const char* path, char* const argv[], char* const envp[], task_t** out_task) {
    if (!path) {
        kprintf("elf_exec: no path\n");
        return -1;
    }
    
    // Open the file
    vfs_file_t* file = NULL;
    int ret = vfs_open(path, 0, &file);
    if (ret != 0 || !file) {
        kprintf("elf_exec: cannot open '%s' (error %d)\n", path, ret);
        return -1;
    }
    
    // Get file size
    size_t file_size = vfs_size(file);
    
    if (file_size == 0 || file_size > 16 * 1024 * 1024) {  // 16MB max
        kprintf("elf_exec: invalid file size %u\n", (unsigned)file_size);
        vfs_close(file);
        return -2;
    }
    
    // Allocate buffer for ELF file
    void* elf_buf = kalloc(file_size);
    if (!elf_buf) {
        kprintf("elf_exec: cannot allocate %u bytes\n", (unsigned)file_size);
        vfs_close(file);
        return -3;
    }
    
    // Read entire file
    long bytes_read = vfs_read(file, elf_buf, file_size);
    vfs_close(file);
    
    if (bytes_read != (long)file_size) {
        kprintf("elf_exec: read error (got %ld, expected %u)\n", 
                bytes_read, (unsigned)file_size);
        kfree(elf_buf);
        return -4;
    }
    
    // Create user address space
    uint64_t* user_pml4 = mm_create_user_address_space();
    if (!user_pml4) {
        kprintf("elf_exec: cannot create address space\n");
        kfree(elf_buf);
        return -5;
    }
    
    // Load ELF
    elf_load_result_t load_result;
    ret = elf_load_user(elf_buf, file_size, user_pml4, &load_result);
    kfree(elf_buf);  // Done with ELF buffer
    
    if (ret != 0) {
        kprintf("elf_exec: load failed (error %d)\n", ret);
        mm_destroy_address_space(user_pml4);
        return -6;
    }
    
    // Set up stack
    #define USER_STACK_TOP   0x8000000ULL   // 128MB
    #define USER_STACK_SIZE  (64 * 1024)    // 64KB
    
    if (!mm_map_user_stack(user_pml4, USER_STACK_TOP, USER_STACK_SIZE)) {
        kprintf("elf_exec: cannot map stack\n");
        mm_destroy_address_space(user_pml4);
        return -7;
    }
    
    // Calculate argc
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    
    // Calculate envc
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }
    
    // We need to set up the stack with:
    //   [strings for argv and envp]
    //   [NULL terminator for envp]
    //   [envp pointers]
    //   [NULL terminator for argv]
    //   [argv pointers]
    //   [argc]  <- RSP points here at entry
    
    // Calculate total string space needed
    size_t strings_size = 0;
    for (int i = 0; i < argc; i++) {
        strings_size += elf_strlen(argv[i]) + 1;
    }
    for (int i = 0; i < envc; i++) {
        strings_size += elf_strlen(envp[i]) + 1;
    }
    
    // Calculate stack layout
    // Align strings to 8 bytes
    size_t strings_aligned = (strings_size + 7) & ~7ULL;
    size_t pointers_size = (argc + 1 + envc + 1) * 8;  // +1 for NULL terminators
    size_t total_size = 8 + pointers_size + strings_aligned;  // +8 for argc
    
    // Ensure 16-byte alignment for stack
    total_size = (total_size + 15) & ~15ULL;
    
    // Make sure our stack layout fits in one page (for simplicity)
    if (total_size > PAGE_SIZE) {
        kprintf("elf_exec: stack layout too large (%lu bytes)\n", (unsigned long)total_size);
        mm_destroy_address_space(user_pml4);
        return -7;
    }
    
    uint64_t stack_ptr = USER_STACK_TOP - total_size;
    
    // The top page of the stack is at USER_STACK_TOP - PAGE_SIZE
    uint64_t top_page_vaddr = USER_STACK_TOP - PAGE_SIZE;
    
    // Allocate a single page buffer to build the stack
    uint8_t* stack_setup = (uint8_t*)kalloc(PAGE_SIZE);
    if (!stack_setup) {
        kprintf("elf_exec: cannot allocate stack setup buffer\n");
        mm_destroy_address_space(user_pml4);
        return -8;
    }
    mm_memset(stack_setup, 0, PAGE_SIZE);
    
    // The buffer represents addresses [top_page_vaddr, USER_STACK_TOP)
    // stack_ptr is within this range, at offset (stack_ptr - top_page_vaddr)
    
    uint64_t* stack_base = (uint64_t*)(stack_setup + (stack_ptr - top_page_vaddr));
    
    // Strings go at the top of the page (end of buffer)
    // We'll copy strings from the end of the buffer downward
    uint8_t* str_dest = stack_setup + PAGE_SIZE;  // End of buffer
    uint64_t str_vaddr = USER_STACK_TOP;  // Virtual address tracking
    
    uint64_t* argv_ptrs = (uint64_t*)kalloc((argc + 1) * sizeof(uint64_t));
    uint64_t* envp_ptrs = (uint64_t*)kalloc((envc + 1) * sizeof(uint64_t));
    
    if (!argv_ptrs || !envp_ptrs) {
        kfree(stack_setup);
        if (argv_ptrs) kfree(argv_ptrs);
        if (envp_ptrs) kfree(envp_ptrs);
        mm_destroy_address_space(user_pml4);
        return -9;
    }
    
    // Copy strings from top of stack down
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = elf_strlen(argv[i]) + 1;
        str_dest -= len;
        str_vaddr -= len;
        mm_memcpy(str_dest, argv[i], len);
        argv_ptrs[i] = str_vaddr;
    }
    
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = elf_strlen(envp[i]) + 1;
        str_dest -= len;
        str_vaddr -= len;
        mm_memcpy(str_dest, envp[i], len);
        envp_ptrs[i] = str_vaddr;
    }
    
    // Align string pointer (not really needed since we use argv_ptrs/envp_ptrs)
    
    // Now build the pointer array
    // Layout from stack_ptr:
    //   [argc]
    //   [argv[0]] [argv[1]] ... [argv[argc-1]] [NULL]
    //   [envp[0]] [envp[1]] ... [envp[envc-1]] [NULL]
    
    uint64_t* sp = stack_base;
    *sp++ = argc;
    
    for (int i = 0; i < argc; i++) {
        *sp++ = argv_ptrs[i];
    }
    *sp++ = 0;  // NULL terminator for argv
    
    for (int i = 0; i < envc; i++) {
        *sp++ = envp_ptrs[i];
    }
    *sp++ = 0;  // NULL terminator for envp
    
    kfree(argv_ptrs);
    kfree(envp_ptrs);
    
    // Now copy the stack setup buffer to the actual stack pages
    // We need to write to the physical pages backing the stack
    // The stack is mapped at USER_STACK_TOP - USER_STACK_SIZE to USER_STACK_TOP
    // We need to copy our buffer to the top page
    
    // Get physical address of the top stack page and copy via direct map
    uint64_t phys = mm_get_physical_address_from_pml4(user_pml4, top_page_vaddr);
    if (phys) {
        mm_memcpy(phys_to_virt(phys), stack_setup, PAGE_SIZE);
    } else {
        kprintf("elf_exec: cannot get physical address for stack page\n");
    }
    
    kfree(stack_setup);
    
    // Create the user task
    task_t* task = sched_add_user_task(
        (task_entry_t)load_result.entry_point,
        NULL,
        user_pml4,
        stack_ptr,  // User stack pointer (with argc/argv/envp set up)
        0           // Auto-allocate kernel stack
    );
    
    if (!task) {
        kprintf("elf_exec: cannot create task\n");
        mm_destroy_address_space(user_pml4);
        return -10;
    }
    
    // Set up task memory management fields
    task->brk_start = load_result.brk_start;
    task->brk = load_result.brk_start;
    // Ensure stack top and mmap base use the aligned stack top, not the current SP
    task->user_stack_top = USER_STACK_TOP;
    task->mmap_base = USER_STACK_TOP - (4 * 1024 * 1024);
    
    // Set up parent relationship so the task can be reaped
    task_t* current = sched_current();
    if (current) {
        task->parent = current;
        sched_add_child(current, task);
        // Inherit pgid from parent (important for job control)
        task->pgid = current->pgid;
        task->sid = current->sid;
        // Inherit controlling tty
        task->ctty = current->ctty;
            // Inherit cwd from parent
            mm_memset(task->cwd, 0, sizeof(task->cwd));
            if (current->cwd[0]) {
                size_t i = 0;
                for (; current->cwd[i] && i < sizeof(task->cwd) - 1; ++i) {
                    task->cwd[i] = current->cwd[i];
                }
                task->cwd[i] = '\0';
            } else {
                task->cwd[0] = '/';
                task->cwd[1] = '\0';
            }
    }
    
    if (out_task) {
        *out_task = task;
    }

    return 0;
}

// Execute an ELF file, replacing the current task's memory (for execve semantics)
// Returns: new entry point on success, 0 on failure
// On success, also sets *out_stack_ptr to the new user stack pointer
uint64_t elf_exec_replace(const char* path, char* const argv[], char* const envp[], uint64_t* out_stack_ptr) {
    if (!path || !out_stack_ptr) {
        return 0;
    }
    
    task_t* current = sched_current();
    if (!current) {
        return 0;
    }
    
    // Open the file
    vfs_file_t* file = NULL;
    int ret = vfs_open(path, 0, &file);
    if (ret != 0 || !file) {
        return 0;
    }
    
    // Get file size
    size_t file_size = vfs_size(file);
    if (file_size == 0 || file_size > 16 * 1024 * 1024) {
        vfs_close(file);
        return 0;
    }
    
    // Allocate buffer for ELF file
    void* elf_buf = kalloc(file_size);
    if (!elf_buf) {
        vfs_close(file);
        return 0;
    }
    
    // Read entire file
    long bytes_read = vfs_read(file, elf_buf, file_size);
    vfs_close(file);
    
    if (bytes_read != (long)file_size) {
        kfree(elf_buf);
        return 0;
    }
    
    // Save the old address space to destroy later
    uint64_t* old_pml4 = current->pml4;
    
    // Create NEW user address space
    uint64_t* user_pml4 = mm_create_user_address_space();
    if (!user_pml4) {
        kfree(elf_buf);
        return 0;
    }
    
    // Load ELF into the new address space
    elf_load_result_t load_result;
    ret = elf_load_user(elf_buf, file_size, user_pml4, &load_result);
    kfree(elf_buf);
    
    if (ret != 0) {
        mm_destroy_address_space(user_pml4);
        return 0;
    }
    
    // Set up stack in new address space
    #define USER_STACK_TOP_EXEC   0x8000000ULL
    #define USER_STACK_SIZE_EXEC  (64 * 1024)
    
    if (!mm_map_user_stack(user_pml4, USER_STACK_TOP_EXEC, USER_STACK_SIZE_EXEC)) {
        mm_destroy_address_space(user_pml4);
        return 0;
    }
    
    // Calculate argc/envc
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }
    
    // Calculate string space needed
    size_t strings_size = 0;
    for (int i = 0; i < argc; i++) {
        strings_size += elf_strlen(argv[i]) + 1;
    }
    for (int i = 0; i < envc; i++) {
        strings_size += elf_strlen(envp[i]) + 1;
    }
    
    // Stack layout (similar to elf_exec)
    size_t total_size = strings_size + 
                        (envc + 1) * sizeof(uint64_t) +
                        (argc + 1) * sizeof(uint64_t) +
                        sizeof(uint64_t);
    
    if (total_size > PAGE_SIZE) {
        mm_destroy_address_space(user_pml4);
        return 0;
    }
    
    // Allocate temp buffer for stack setup
    uint8_t* stack_setup = kalloc(PAGE_SIZE);
    if (!stack_setup) {
        mm_destroy_address_space(user_pml4);
        return 0;
    }
    mm_memset(stack_setup, 0, PAGE_SIZE);
    
    // Build stack content
    uint64_t stack_base = USER_STACK_TOP_EXEC - USER_STACK_SIZE_EXEC;
    uint64_t stack_page_base = USER_STACK_TOP_EXEC - PAGE_SIZE;
    
    uint64_t string_area = stack_page_base;
    uint8_t* str_ptr = stack_setup;
    
    uint64_t argv_ptrs[128];
    uint64_t envp_ptrs[128];
    
    for (int i = 0; i < argc && i < 128; i++) {
        argv_ptrs[i] = string_area + (str_ptr - stack_setup);
        size_t len = elf_strlen(argv[i]);
        mm_memcpy(str_ptr, argv[i], len + 1);
        str_ptr += len + 1;
    }
    for (int i = 0; i < envc && i < 128; i++) {
        envp_ptrs[i] = string_area + (str_ptr - stack_setup);
        size_t len = elf_strlen(envp[i]);
        mm_memcpy(str_ptr, envp[i], len + 1);
        str_ptr += len + 1;
    }
    
    // Calculate where arrays go
    size_t strings_used = str_ptr - stack_setup;
    size_t array_space = (envc + 1 + argc + 1 + 1) * sizeof(uint64_t);
    size_t total_used = strings_used + array_space;
    total_used = (total_used + 15) & ~15ULL;
    
    uint64_t stack_ptr = USER_STACK_TOP_EXEC - total_used;
    size_t offset_in_page = PAGE_SIZE - total_used;
    
    uint64_t* sp = (uint64_t*)(stack_setup + offset_in_page);
    *sp++ = argc;
    for (int i = 0; i < argc; i++) {
        *sp++ = argv_ptrs[i];
    }
    *sp++ = 0;  // NULL terminator for argv
    for (int i = 0; i < envc; i++) {
        *sp++ = envp_ptrs[i];
    }
    *sp++ = 0;  // NULL terminator for envp
    
    // Copy stack page to physical memory via direct map
    uint64_t top_page_vaddr = USER_STACK_TOP_EXEC - PAGE_SIZE;
    uint64_t phys = mm_get_physical_address_from_pml4(user_pml4, top_page_vaddr);
    if (phys) {
        mm_memcpy(phys_to_virt(phys), stack_setup, PAGE_SIZE);
    }
    kfree(stack_setup);
    
    // Now switch the current task to use the new address space
    // First, switch CR3 to the new address space
    current->pml4 = user_pml4;
    
    // Update task's memory management fields
    current->brk_start = load_result.brk_start;
    current->brk = load_result.brk_start;
    current->user_stack_top = USER_STACK_TOP_EXEC;
    current->mmap_base = USER_STACK_TOP_EXEC - (4 * 1024 * 1024);
    
    // Close all file descriptors except 0, 1, 2
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (current->fd_table[i]) {
            vfs_close(current->fd_table[i]);
            current->fd_table[i] = NULL;
        }
    }
    
    // Switch to new address space now
    // Use mm_switch_address_space which correctly converts virtual to physical for CR3
    mm_switch_address_space(user_pml4);
    
    // Destroy old address space (after switching!)
    if (old_pml4) {
        mm_destroy_address_space(old_pml4);
    }
    
    *out_stack_ptr = stack_ptr;
    return load_result.entry_point;
}
