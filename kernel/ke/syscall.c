// LikeOS-64 System Call Handler
#include "../../include/kernel/console.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/status.h"
#include "../../include/kernel/elf.h"
#include "../../include/kernel/pipe.h"

// Validate user pointer is in user space
static bool validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x1000) return false;  // NULL-ish pointer
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
}

// Safe string length (bounded) from user space
static int user_strnlen(const char* user_str, size_t max_len, size_t* out_len) {
    if (!user_str || !out_len) {
        return -EFAULT;
    }
    for (size_t i = 0; i < max_len; i++) {
        if (!validate_user_ptr((uint64_t)user_str + i, 1)) {
            return -EFAULT;
        }
        if (user_str[i] == '\0') {
            *out_len = i;
            return 0;
        }
    }
    return -EINVAL;  // Too long
}

static int copy_user_string(const char* user_str, size_t max_len, char** out_str, size_t* out_len) {
    if (!user_str || !out_str) {
        return -EFAULT;
    }

    size_t len = 0;
    int ret = user_strnlen(user_str, max_len, &len);
    if (ret != 0) {
        return ret;
    }

    char* kstr = (char*)kalloc(len + 1);
    if (!kstr) {
        return -ENOMEM;
    }
    mm_memcpy(kstr, user_str, len);
    kstr[len] = '\0';

    *out_str = kstr;
    if (out_len) {
        *out_len = len;
    }
    return 0;
}

static void free_user_string_array(char** arr) {
    if (!arr) {
        return;
    }
    for (size_t i = 0; arr[i]; i++) {
        kfree(arr[i]);
    }
    kfree(arr);
}

static int copy_user_string_array(const char* const* user_arr, size_t max_count,
                                  size_t max_str_len, size_t max_total_bytes,
                                  char*** out_arr) {
    if (!out_arr) {
        return -EFAULT;
    }
    *out_arr = NULL;

    if (!user_arr) {
        return 0;
    }

    if (!validate_user_ptr((uint64_t)user_arr, sizeof(uint64_t))) {
        return -EFAULT;
    }

    char** karr = (char**)kalloc((max_count + 1) * sizeof(char*));
    if (!karr) {
        return -ENOMEM;
    }
    mm_memset(karr, 0, (max_count + 1) * sizeof(char*));

    size_t total = 0;
    for (size_t i = 0; i < max_count; i++) {
        const char* user_str = user_arr[i];
        if (!user_str) {
            karr[i] = NULL;
            *out_arr = karr;
            return 0;
        }
        if (!validate_user_ptr((uint64_t)user_arr + (i * sizeof(uint64_t)), sizeof(uint64_t))) {
            free_user_string_array(karr);
            return -EFAULT;
        }

        char* kstr = NULL;
        size_t len = 0;
        int ret = copy_user_string(user_str, max_str_len, &kstr, &len);
        if (ret != 0) {
            free_user_string_array(karr);
            return ret;
        }

        total += len + 1;
        if (total > max_total_bytes) {
            kfree(kstr);
            free_user_string_array(karr);
            return -EINVAL;
        }

        karr[i] = kstr;
    }

    free_user_string_array(karr);
    return -EINVAL;  // Too many entries
}

// Pipe read/write helpers
static int64_t pipe_read_to_user(pipe_end_t* end, uint64_t buf, uint64_t count) {
    if (!end || !end->pipe || !end->is_read) {
        return -EBADF;
    }
    pipe_t* pipe = end->pipe;

    if (count == 0) {
        return 0;
    }
    if (pipe->used == 0) {
        return (pipe->writers == 0) ? 0 : -EAGAIN;
    }

    size_t to_read = (count < pipe->used) ? count : pipe->used;
    size_t first = pipe->size - pipe->read_pos;
    if (first > to_read) {
        first = to_read;
    }

    mm_memcpy((void*)buf, pipe->buffer + pipe->read_pos, first);
    if (to_read > first) {
        mm_memcpy((void*)(buf + first), pipe->buffer, to_read - first);
    }

    pipe->read_pos = (pipe->read_pos + to_read) % pipe->size;
    pipe->used -= to_read;

    return (int64_t)to_read;
}

static int64_t pipe_write_from_user(pipe_end_t* end, uint64_t buf, uint64_t count) {
    if (!end || !end->pipe || end->is_read) {
        return -EBADF;
    }
    pipe_t* pipe = end->pipe;

    if (count == 0) {
        return 0;
    }
    if (pipe->readers == 0) {
        return -EAGAIN;
    }
    if (pipe->used == pipe->size) {
        return -EAGAIN;
    }

    size_t space = pipe->size - pipe->used;
    size_t to_write = (count < space) ? count : space;
    size_t first = pipe->size - pipe->write_pos;
    if (first > to_write) {
        first = to_write;
    }

    mm_memcpy(pipe->buffer + pipe->write_pos, (void*)buf, first);
    if (to_write > first) {
        mm_memcpy(pipe->buffer, (void*)(buf + first), to_write - first);
    }

    pipe->write_pos = (pipe->write_pos + to_write) % pipe->size;
    pipe->used += to_write;

    return (int64_t)to_write;
}

// Allocate a file descriptor for current task
static int alloc_fd(task_t* task) {
    // Start at 3 to skip stdin(0), stdout(1), stderr(2)
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (task->fd_table[i] == NULL) {
            return i;
        }
    }
    return -EMFILE;  // Too many open files
}

// Find free mmap region slot
static mmap_region_t* alloc_mmap_region(task_t* task) {
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        if (!task->mmap_regions[i].in_use) {
            return &task->mmap_regions[i];
        }
    }
    return NULL;
}

// Find mmap region by address
static mmap_region_t* find_mmap_region(task_t* task, uint64_t addr) {
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        mmap_region_t* r = &task->mmap_regions[i];
        if (r->in_use && addr >= r->start && addr < r->start + r->length) {
            return r;
        }
    }
    return NULL;
}

// SYS_READ - read from file descriptor
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (!validate_user_ptr(buf, count)) {
        return -EFAULT;
    }
    
    // Handle stdin specially (keyboard)
    if (fd == STDIN_FD) {
        char* ubuf = (char*)buf;
        size_t read_count = 0;
        
        while (read_count < count) {
            if (keyboard_buffer_has_data()) {
                ubuf[read_count++] = keyboard_get_char();
            } else {
                // Non-blocking: return what we have
                break;
            }
        }
        return (int64_t)read_count;
    }
    
    // Handle regular file descriptors
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
    }
    
    vfs_file_t* file = cur->fd_table[fd];
    
    // Check for console dup markers (magic pointers 1, 2, 3)
    uint64_t marker = (uint64_t)file;
    if (marker == 1) {
        // Dup'd stdin - read from keyboard
        char* ubuf = (char*)buf;
        size_t read_count = 0;
        while (read_count < count) {
            if (keyboard_buffer_has_data()) {
                ubuf[read_count++] = keyboard_get_char();
            } else {
                break;
            }
        }
        return (int64_t)read_count;
    } else if (marker == 2 || marker == 3) {
        // Can't read from stdout/stderr
        return -EBADF;
    }
    
    if (pipe_is_end(file)) {
        if (!validate_user_ptr(buf, count)) {
            return -EFAULT;
        }
        return pipe_read_to_user((pipe_end_t*)file, buf, count);
    }

    return vfs_read(file, (void*)buf, (long)count);
}

// SYS_WRITE - write to file descriptor
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (!validate_user_ptr(buf, count)) {
        return -EFAULT;
    }
    
    // Handle stdout/stderr (console)
    if (fd == STDOUT_FD || fd == STDERR_FD) {
        const char* ubuf = (const char*)buf;
        for (size_t i = 0; i < count; i++) {
            console_putchar(ubuf[i]);
        }
        return (int64_t)count;
    }
    
    // Regular file descriptors
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
    }
    
    vfs_file_t* file = cur->fd_table[fd];
    
    // Check for console dup markers (magic pointers 1, 2, 3)
    uint64_t marker = (uint64_t)file;
    if (marker == 2 || marker == 3) {
        // Dup'd stdout/stderr - write to console
        const char* ubuf = (const char*)buf;
        for (size_t i = 0; i < count; i++) {
            console_putchar(ubuf[i]);
        }
        return (int64_t)count;
    } else if (marker == 1) {
        // Can't write to stdin
        return -EBADF;
    }
    
    if (pipe_is_end(file)) {
        if (!validate_user_ptr(buf, count)) {
            return -EFAULT;
        }
        return pipe_write_from_user((pipe_end_t*)file, buf, count);
    }

    // Write to USB storage not currently supported (read-only filesystem)
    return -ENOSYS;
}

// SYS_OPEN - open a file
static int64_t sys_open(uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)flags; (void)mode;  // Currently ignore flags/mode
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (!validate_user_ptr(pathname, 1)) {
        return -EFAULT;
    }
    
    int fd = alloc_fd(cur);
    if (fd < 0) {
        return fd;  // Error code
    }
    
    vfs_file_t* file = NULL;
    int ret = vfs_open((const char*)pathname, &file);
    if (ret != ST_OK || file == NULL) {
        return -EACCES;
    }
    
    cur->fd_table[fd] = file;
    return fd;
}

// SYS_CLOSE - close a file descriptor
static int64_t sys_close(uint64_t fd) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Don't allow closing stdin/stdout/stderr
    if (fd < 3) {
        return -EBADF;
    }
    
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
    }
    
    vfs_file_t* file = cur->fd_table[fd];
    
    // Check for console dup markers - don't call vfs_close on them
    uint64_t marker = (uint64_t)file;
    if (marker >= 1 && marker <= 3) {
        // Console dup marker - just clear the entry
        cur->fd_table[fd] = NULL;
        return 0;
    }

    if (pipe_is_end(file)) {
        pipe_close_end((pipe_end_t*)file);
        cur->fd_table[fd] = NULL;
        return 0;
    }
    
    vfs_close(file);
    cur->fd_table[fd] = NULL;
    
    return 0;
}

// SYS_LSEEK - reposition file offset
static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // stdin/stdout/stderr are not seekable
    if (fd < 3) {
        return -ESPIPE;
    }
    
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
    }
    
    vfs_file_t* file = cur->fd_table[fd];
    
    // Check for console dup markers - not seekable
    uint64_t marker = (uint64_t)file;
    if (marker >= 1 && marker <= 3) {
        return -ESPIPE;
    }
    
    long result = vfs_seek(file, (long)offset, (int)whence);
    
    if (result < 0) {
        return -EINVAL;
    }
    
    return result;
}

// SYS_BRK - set program break
static int64_t sys_brk(uint64_t new_brk) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // If new_brk is 0, return current break
    if (new_brk == 0) {
        return (int64_t)cur->brk;
    }
    
    // Validate new break is reasonable
    if (new_brk < cur->brk_start) {
        return (int64_t)cur->brk;  // Can't shrink below start
    }
    
    // Don't let heap grow into stack area
    if (new_brk >= cur->user_stack_top - (2 * 1024 * 1024)) {
        return (int64_t)cur->brk;  // Would collide with stack
    }
    
    // Growing the heap
    if (new_brk > cur->brk) {
        uint64_t old_page = PAGE_ALIGN(cur->brk);
        uint64_t new_page = PAGE_ALIGN(new_brk);
        
        // Map new pages
        for (uint64_t addr = old_page; addr < new_page; addr += PAGE_SIZE) {
            uint64_t phys = mm_allocate_physical_page();
            if (!phys) {
                return (int64_t)cur->brk;  // Out of memory
            }
            mm_memset((void*)phys, 0, PAGE_SIZE);
            
            uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NO_EXECUTE;
            if (!mm_map_page_in_address_space(cur->pml4, addr, phys, flags)) {
                mm_free_physical_page(phys);
                return (int64_t)cur->brk;
            }
        }
    }
    // Shrinking the heap - could free pages but keep it simple for now
    
    cur->brk = new_brk;
    return (int64_t)new_brk;
}

// SYS_MMAP - map memory
static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    task_t* cur = sched_current();
    if (!cur) return (int64_t)MAP_FAILED;
    
    // Validate length
    if (length == 0) {
        return (int64_t)MAP_FAILED;
    }
    
    // Round up length to page size
    length = PAGE_ALIGN(length);
    
    // Find a free mmap region slot
    mmap_region_t* region = alloc_mmap_region(cur);
    if (!region) {
        return (int64_t)MAP_FAILED;
    }
    
    // Determine virtual address
    uint64_t vaddr;
    if (flags & MAP_FIXED) {
        if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
            return (int64_t)MAP_FAILED;  // Invalid fixed address
        }
        vaddr = addr;
    } else {
        // Allocate from mmap area (grows down from below stack)
        // Move base down first, then return the new base as the start of the mapped region
        cur->mmap_base -= length;
        if (cur->mmap_base < cur->brk + (4 * 1024 * 1024)) {
            // Too close to heap
            cur->mmap_base += length;  // Rollback
            return (int64_t)MAP_FAILED;
        }
        vaddr = cur->mmap_base;
    }
    
    // Calculate page flags
    uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) {
        page_flags |= PAGE_WRITABLE;
    }
    if (!(prot & PROT_EXEC)) {
        page_flags |= PAGE_NO_EXECUTE;
    }
    
    // Map pages
    bool is_anonymous = (flags & MAP_ANONYMOUS) || (int64_t)fd == -1;
    uint64_t pages_mapped = 0;
    
    for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
        uint64_t phys = mm_allocate_physical_page();
        if (!phys) {
            // Unmap already-mapped pages on failure
            for (uint64_t cleanup = 0; cleanup < off; cleanup += PAGE_SIZE) {
                mm_unmap_page_in_address_space(cur->pml4, vaddr + cleanup);
            }
            if (!(flags & MAP_FIXED)) {
                cur->mmap_base += length;  // Rollback
            }
            return (int64_t)MAP_FAILED;
        }
        
        mm_memset((void*)phys, 0, PAGE_SIZE);
        
        // For file-backed mappings, read content from file
        if (!is_anonymous && fd < TASK_MAX_FDS && cur->fd_table[fd]) {
            vfs_file_t* file = cur->fd_table[fd];
            // Seek to the correct position and read
            long file_off = (long)(offset + off);
            if (vfs_seek(file, file_off, SEEK_SET) >= 0) {
                vfs_read(file, (void*)phys, PAGE_SIZE);
            }
        }
        
        if (!mm_map_page_in_address_space(cur->pml4, vaddr + off, phys, page_flags)) {
            mm_free_physical_page(phys);
            // Unmap already-mapped pages on failure
            for (uint64_t cleanup = 0; cleanup < off; cleanup += PAGE_SIZE) {
                mm_unmap_page_in_address_space(cur->pml4, vaddr + cleanup);
            }
            if (!(flags & MAP_FIXED)) {
                cur->mmap_base += length;  // Rollback
            }
            return (int64_t)MAP_FAILED;
        }
        pages_mapped++;
    }
    
    // Record the mapping
    region->start = vaddr;
    region->length = length;
    region->prot = prot;
    region->flags = flags;
    region->fd = is_anonymous ? -1 : (int)fd;
    region->offset = offset;
    region->in_use = true;
    
    return (int64_t)vaddr;
}

// SYS_MUNMAP - unmap memory
static int64_t sys_munmap(uint64_t addr, uint64_t length) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;

    if (addr == 0 || length == 0) {
        return -EINVAL;
    }
    if (addr & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }

    length = PAGE_ALIGN(length);

    mmap_region_t* region = find_mmap_region(cur, addr);
    if (!region) {
        return -EINVAL;
    }

    uint64_t region_end = region->start + region->length;
    if (addr < region->start || addr + length > region_end) {
        return -EINVAL;
    }

    for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
        mm_unmap_page_in_address_space(cur->pml4, addr + off);
    }

    if (addr == region->start && length == region->length) {
        region->in_use = false;
    } else if (addr == region->start) {
        region->start += length;
        region->length -= length;
    } else if (addr + length == region_end) {
        region->length -= length;
    } else {
        // Splitting regions not supported yet
        return -EINVAL;
    }

    return 0;
}

// SYS_PIPE - create a pipe
static int64_t sys_pipe(uint64_t pipefd_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;

    if (!validate_user_ptr(pipefd_ptr, sizeof(int) * 2)) {
        return -EFAULT;
    }

    pipe_t* pipe = pipe_create(4096);
    if (!pipe) {
        return -ENOMEM;
    }

    pipe_end_t* read_end = pipe_create_end(pipe, true);
    if (!read_end) {
        if (pipe->buffer) {
            kfree(pipe->buffer);
        }
        kfree(pipe);
        return -ENOMEM;
    }

    pipe_end_t* write_end = pipe_create_end(pipe, false);
    if (!write_end) {
        pipe_close_end(read_end);
        return -ENOMEM;
    }

    int fd_read = alloc_fd(cur);
    if (fd_read < 0) {
        pipe_close_end(read_end);
        pipe_close_end(write_end);
        return fd_read;
    }

    int fd_write = alloc_fd(cur);
    if (fd_write < 0) {
        pipe_close_end(read_end);
        pipe_close_end(write_end);
        return fd_write;
    }

    cur->fd_table[fd_read] = (vfs_file_t*)read_end;
    cur->fd_table[fd_write] = (vfs_file_t*)write_end;

    int* user_pipefd = (int*)pipefd_ptr;
    user_pipefd[0] = fd_read;
    user_pipefd[1] = fd_write;

    return 0;
}

// SYS_EXIT - exit task
__attribute__((noreturn))
static void sys_exit(uint64_t status) {
    task_t* cur = sched_current();
    if (cur) {
        // Store exit code
        cur->exit_code = (int)status;
        
        // Close all file descriptors
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            if (cur->fd_table[i]) {
                // Check for console dup markers (magic pointers 1, 2, 3)
                uint64_t marker = (uint64_t)cur->fd_table[i];
                if (marker >= 1 && marker <= 3) {
                    // Console marker - just clear, don't call vfs_close
                    cur->fd_table[i] = NULL;
                } else if (pipe_is_end(cur->fd_table[i])) {
                    pipe_close_end((pipe_end_t*)cur->fd_table[i]);
                    cur->fd_table[i] = NULL;
                } else {
                    vfs_close(cur->fd_table[i]);
                    cur->fd_table[i] = NULL;
                }
            }
        }
        
        // Reparent children to init
        sched_reparent_children(cur);

        // Mark as exited only after cleanup to avoid early reap races
        __asm__ volatile ("cli");
        cur->has_exited = true;
        cur->state = TASK_ZOMBIE;
    }
    sched_yield();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// External: saved user context from syscall entry (in syscall.asm)
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;

// External: user_mode_iret_trampoline from syscall.asm
extern void user_mode_iret_trampoline(void);
extern void fork_child_return(void);

// SYS_FORK - fork current process
static int64_t sys_fork(void) {
    task_t* cur = sched_current();
    if (!cur || cur->privilege != TASK_USER) {
        return -1;
    }
    
    // Capture user context saved by syscall entry (from syscall.asm)
    uint64_t user_rip = syscall_saved_user_rip;      // Where to resume execution
    uint64_t user_rsp = syscall_saved_user_rsp;      // User stack pointer
    uint64_t user_rflags = syscall_saved_user_rflags; // Saved RFLAGS
    
    // Create child with cloned address space and file descriptors
    task_t* child = sched_fork_current();
    if (!child) {
        return -1;  // Fork failed
    }
    
    // Set up child's kernel stack to return to userspace
    // When the child is scheduled, it will resume at user_rip with fork() returning 0
    // 
    // Stack layout (from top to bottom):
    // 1. IRET frame: SS, RSP, RFLAGS, CS, RIP (to return to userspace)
    // 2. RAX value (0 for child's fork return value)
    // 3. Saved callee-saved registers for ctx_switch_asm (r15-rbp)
    // 4. Return address (fork_child_return trampoline)
    
    uint64_t* k_sp = (uint64_t*)child->kernel_stack_top;
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL);  // Align to 16 bytes
    
    // Push IRET frame (used by fork_child_return to return to userspace)
    *(--k_sp) = 0x1B;                    // SS: user data segment
    *(--k_sp) = user_rsp;                // User stack pointer
    *(--k_sp) = user_rflags | 0x200;     // RFLAGS with interrupts enabled
    *(--k_sp) = 0x23;                    // CS: user code segment
    *(--k_sp) = user_rip;                // Resume at parent's fork() call site
    
    // Push fork return value for child (0)
    *(--k_sp) = 0;  // RAX = 0 (child sees fork() return 0)
    
    // Push callee-saved registers (ctx_switch_asm will restore these)
    *(--k_sp) = (uint64_t)fork_child_return;  // Return address: sets RAX=0 and does IRET
    *(--k_sp) = 0; // RBP
    *(--k_sp) = 0; // RBX
    *(--k_sp) = 0; // R12
    *(--k_sp) = 0; // R13
    *(--k_sp) = 0; // R14
    *(--k_sp) = 0; // R15
    
    child->sp = k_sp;
    
    // Parent returns child's PID
    return child->id;
}

// SYS_WAIT4/SYS_WAITPID - wait for child process
static int64_t sys_waitpid(int64_t pid, uint64_t status_ptr, uint64_t options) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Find a zombie child
    task_t* child = NULL;
    
    if (pid == -1) {
        // Wait for any child
        task_t* c = cur->first_child;
        while (c) {
            if (c->has_exited) {
                child = c;
                break;
            }
            c = c->next_sibling;
        }
    } else if (pid > 0) {
        // Wait for specific child
        task_t* found = sched_find_task_by_id((uint32_t)pid);
        if (found && found->parent == cur && found->has_exited) {
            child = found;
        }
    }
    
    if (!child) {
        // Check if there are any children at all
        if (!cur->first_child) {
            return -ECHILD;
        }
        // Non-blocking: return EAGAIN if no zombie children
        if (options & 1) {  // WNOHANG = 1
            return 0;  // No child exited yet
        }
        return -EAGAIN;  // Would block - caller should poll
    }
    
    // Store status if requested
    if (status_ptr && validate_user_ptr(status_ptr, sizeof(int))) {
        // Linux-style status: exit_code << 8
        *(int*)status_ptr = (child->exit_code & 0xFF) << 8;
    }
    
    int child_pid = child->id;
    
    // Reap the zombie - remove from scheduler and free
    sched_remove_task(child);
    
    return child_pid;
}

// SYS_EXECVE - execute a new program (spawns and exits current)
static int64_t sys_execve(uint64_t pathname, uint64_t argv_ptr, uint64_t envp_ptr) {
    if (!validate_user_ptr(pathname, 1)) {
        return -EFAULT;
    }

    const char* user_path = (const char*)pathname;
    const char* const* user_argv = (const char* const*)argv_ptr;
    const char* const* user_envp = (const char* const*)envp_ptr;

    char* kpath = NULL;
    char** kargv = NULL;
    char** kenvp = NULL;

    int ret = copy_user_string(user_path, VFS_MAX_PATH, &kpath, NULL);
    if (ret != 0) {
        return ret;
    }

    ret = copy_user_string_array(user_argv, 128, 4096, 16384, &kargv);
    if (ret != 0) {
        kfree(kpath);
        return ret;
    }

    ret = copy_user_string_array(user_envp, 128, 4096, 16384, &kenvp);
    if (ret != 0) {
        free_user_string_array(kargv);
        kfree(kpath);
        return ret;
    }

    int exec_ret = elf_exec(kpath, kargv, kenvp, NULL);

    free_user_string_array(kenvp);
    free_user_string_array(kargv);
    kfree(kpath);

    if (exec_ret == 0) {
        sys_exit(0);
        return 0;
    }

    if (exec_ret == -1) return -EACCES;
    if (exec_ret == -3 || exec_ret == -5 || exec_ret == -8 || exec_ret == -10) return -ENOMEM;
    return -ENOEXEC;
}


// SYS_GETPPID - get parent process ID
static int64_t sys_getppid(void) {
    task_t* cur = sched_current();
    if (!cur) return 0;
    return sched_get_ppid(cur);
}

// SYS_DUP - duplicate file descriptor
static int64_t sys_dup(uint64_t oldfd) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Find lowest available fd (start at 3 to preserve stdin/stdout/stderr semantics)
    int newfd = -1;
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (cur->fd_table[i] == NULL) {
            newfd = i;
            break;
        }
    }
    
    if (newfd < 0) {
        return -EMFILE;
    }
    
    // Handle stdin/stdout/stderr specially - they're virtual console fds
    if (oldfd == STDIN_FD || oldfd == STDOUT_FD || oldfd == STDERR_FD) {
        // For console fds, we create a "marker" that indicates this is a console dup
        // We'll use a special value - actually, let's just return the new fd
        // and handle reads/writes to it by checking if fd_table entry is NULL
        // For simplicity, return success and treat NULL entries for low fds as console
        // Actually, let's store a sentinel value or just treat the dup'd fd as console too
        // Simplest: just return the new fd number and let the caller use it
        // Since we're returning a new fd >= 3, we need write/read to handle it
        // For now, return the newfd and we'll make console fds work differently
        
        // Better approach: stdin/stdout/stderr always work, duping them gives
        // a new fd that also refers to console. Store NULL but mark it somehow.
        // Actually easiest: return a new fd number and make sys_write/sys_read
        // check if fd < 3 OR fd_table[fd] == NULL and fd was duped from console
        
        // For MVP: just return the newfd and have the user code work with it
        // We can't easily track "this fd is a console dup" without extra state
        // So let's return an error for now indicating dup of console not supported
        // OR we can be clever: store a magic pointer value for console
        
        // Let's use approach: store (vfs_file_t*)1, (vfs_file_t*)2 as markers
        // and check for these in read/write
        if (oldfd == STDIN_FD) {
            cur->fd_table[newfd] = (vfs_file_t*)1;  // Magic: stdin marker
        } else if (oldfd == STDOUT_FD) {
            cur->fd_table[newfd] = (vfs_file_t*)2;  // Magic: stdout marker  
        } else {
            cur->fd_table[newfd] = (vfs_file_t*)3;  // Magic: stderr marker
        }
        return newfd;
    }
    
    if (oldfd >= TASK_MAX_FDS || cur->fd_table[oldfd] == NULL) {
        return -EBADF;
    }
    
    // Check for magic console markers
    uint64_t marker = (uint64_t)cur->fd_table[oldfd];
    if (marker >= 1 && marker <= 3) {
        cur->fd_table[newfd] = cur->fd_table[oldfd];  // Copy the marker
        return newfd;
    }

    if (pipe_is_end(cur->fd_table[oldfd])) {
        pipe_end_t* new_end = pipe_dup_end((pipe_end_t*)cur->fd_table[oldfd]);
        if (!new_end) {
            return -ENOMEM;
        }
        cur->fd_table[newfd] = (vfs_file_t*)new_end;
        return newfd;
    }
    
    cur->fd_table[newfd] = vfs_dup(cur->fd_table[oldfd]);
    return newfd;
}

// SYS_DUP2 - duplicate file descriptor to specific fd
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (newfd >= TASK_MAX_FDS) {
        return -EBADF;
    }
    
    if (oldfd == newfd) {
        return newfd;
    }
    
    // Close newfd if it was open (but not if it's a console fd 0-2)
    if (newfd >= 3 && cur->fd_table[newfd]) {
        uint64_t marker = (uint64_t)cur->fd_table[newfd];
        if (marker >= 1 && marker <= 3) {
            // It's a console dup marker, just overwrite
        } else if (pipe_is_end(cur->fd_table[newfd])) {
            pipe_close_end((pipe_end_t*)cur->fd_table[newfd]);
        } else {
            vfs_close(cur->fd_table[newfd]);
        }
    }
    
    // Handle stdin/stdout/stderr specially - they're virtual console fds
    if (oldfd == STDIN_FD || oldfd == STDOUT_FD || oldfd == STDERR_FD) {
        if (oldfd == STDIN_FD) {
            cur->fd_table[newfd] = (vfs_file_t*)1;  // Magic: stdin marker
        } else if (oldfd == STDOUT_FD) {
            cur->fd_table[newfd] = (vfs_file_t*)2;  // Magic: stdout marker
        } else {
            cur->fd_table[newfd] = (vfs_file_t*)3;  // Magic: stderr marker
        }
        return newfd;
    }
    
    if (oldfd >= TASK_MAX_FDS || cur->fd_table[oldfd] == NULL) {
        return -EBADF;
    }
    
    // Check for magic console markers
    uint64_t marker = (uint64_t)cur->fd_table[oldfd];
    if (marker >= 1 && marker <= 3) {
        cur->fd_table[newfd] = cur->fd_table[oldfd];  // Copy the marker
        return newfd;
    }

    if (pipe_is_end(cur->fd_table[oldfd])) {
        pipe_end_t* new_end = pipe_dup_end((pipe_end_t*)cur->fd_table[oldfd]);
        if (!new_end) {
            return -ENOMEM;
        }
        cur->fd_table[newfd] = (vfs_file_t*)new_end;
        return newfd;
    }
    
    cur->fd_table[newfd] = vfs_dup(cur->fd_table[oldfd]);
    return newfd;
}

// SYS_GETPID - get process ID
static int64_t sys_getpid(void) {
    task_t* cur = sched_current();
    return cur ? cur->id : -1;
}

// SYS_YIELD - yield CPU
static int64_t sys_yield(void) {
    task_t* cur = sched_current();
    __asm__ volatile ("sti");
    sched_yield();
    return 0;
}

// Main syscall dispatcher
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, 
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (num) {
        case SYS_READ:
            return sys_read(a1, a2, a3);
            
        case SYS_WRITE:
            return sys_write(a1, a2, a3);
            
        case SYS_OPEN:
            return sys_open(a1, a2, a3);
            
        case SYS_CLOSE:
            return sys_close(a1);
            
        case SYS_LSEEK:
            return sys_lseek(a1, (int64_t)a2, a3);
            
        case SYS_MMAP:
            return sys_mmap(a1, a2, a3, a4, a5, 0);  // Note: 6th arg (offset) would need special handling

        case SYS_MUNMAP:
            return sys_munmap(a1, a2);
            
        case SYS_BRK:
            return sys_brk(a1);
            
        case SYS_GETPID:
            return sys_getpid();
            
        case SYS_FORK:
            return sys_fork();
            
        case SYS_WAIT4:
            return sys_waitpid((int64_t)a1, a2, a3);
            
        case SYS_GETPPID:
            return sys_getppid();

        case SYS_EXECVE:
            return sys_execve(a1, a2, a3);
            
        case SYS_DUP:
            return sys_dup(a1);
            
        case SYS_DUP2:
            return sys_dup2(a1, a2);
            
        case SYS_EXIT:
            sys_exit(a1);
            // Never returns

        case SYS_PIPE:
            return sys_pipe(a1);
            
        case SYS_YIELD:
            return sys_yield();
            
        default:
            return -ENOSYS;
    }
}
