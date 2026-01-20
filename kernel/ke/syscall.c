// LikeOS-64 System Call Handler
#include "../../include/kernel/console.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/status.h"

// Validate user pointer is in user space
static bool validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x1000) return false;  // NULL-ish pointer
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
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
    
    // Regular file descriptors - VFS doesn't support write yet
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
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

// SYS_EXIT - exit task
__attribute__((noreturn))
static void sys_exit(uint64_t status) {
    (void)status;
    task_t* cur = sched_current();
    if (cur) {
        cur->state = TASK_ZOMBIE;
    }
    __asm__ volatile ("sti");
    sched_yield();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// SYS_GETPID - get process ID
static int64_t sys_getpid(void) {
    task_t* cur = sched_current();
    return cur ? cur->id : -1;
}

// SYS_YIELD - yield CPU
static int64_t sys_yield(void) {
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
            
        case SYS_BRK:
            return sys_brk(a1);
            
        case SYS_GETPID:
            return sys_getpid();
            
        case SYS_EXIT:
            sys_exit(a1);
            // Never returns
            
        case SYS_YIELD:
            return sys_yield();
            
        default:
            return -ENOSYS;
    }
}
