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
#include "../../include/kernel/timer.h"
#include "../../include/kernel/stat.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/devfs.h"
#include "../../include/kernel/dirent.h"
#include "../../include/kernel/serial.h"
#include "../../include/kernel/percpu.h"
#include "../../include/kernel/smp.h"
#include "../../include/kernel/futex.h"
#include "../../include/kernel/acpi.h"
#include "../../include/kernel/fat32.h"

// Validate user pointer is in user space
static bool validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x10000) return false;  // Reject low addresses (NULL deref protection)
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
}

// SMAP-aware copy from user space to kernel space
// Returns 0 on success, -EFAULT on failure
static int copy_from_user(void* kernel_dst, const void* user_src, size_t len) {
    if (!validate_user_ptr((uint64_t)user_src, len)) {
        return -EFAULT;
    }
    if (!kernel_dst || len == 0) {
        return (len == 0) ? 0 : -EFAULT;
    }
    // Temporarily allow supervisor access to user pages (SMAP bypass)
    smap_disable();
    mm_memcpy(kernel_dst, user_src, len);
    // Re-enable SMAP protection
    smap_enable();
    return 0;
}

// SMAP-aware copy from kernel space to user space
// Returns 0 on success, -EFAULT on failure
static int copy_to_user(void* user_dst, const void* kernel_src, size_t len) {
    if (!validate_user_ptr((uint64_t)user_dst, len)) {
        return -EFAULT;
    }
    if (!kernel_src || len == 0) {
        return (len == 0) ? 0 : -EFAULT;
    }
    // Temporarily allow supervisor access to user pages (SMAP bypass)
    smap_disable();
    mm_memcpy(user_dst, kernel_src, len);
    // Re-enable SMAP protection
    smap_enable();
    return 0;
}

// Safe string length (bounded) from user space (SMAP-aware)
static int user_strnlen(const char* user_str, size_t max_len, size_t* out_len) {
    if (!user_str || !out_len) {
        return -EFAULT;
    }
    // Validate entire potential range first
    if (!validate_user_ptr((uint64_t)user_str, max_len)) {
        return -EFAULT;
    }
    // Temporarily allow user memory access
    smap_disable();
    size_t i;
    for (i = 0; i < max_len; i++) {
        if (user_str[i] == '\0') {
            *out_len = i;
            smap_enable();
            return 0;
        }
    }
    smap_enable();
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
    // Use copy_from_user for SMAP-aware copy
    if (copy_from_user(kstr, user_str, len) != 0) {
        kfree(kstr);
        return -EFAULT;
    }
    kstr[len] = '\0';

    *out_str = kstr;
    if (out_len) {
        *out_len = len;
    }
    return 0;
}

// Helper: Copy user path string directly into fixed kernel buffer (no allocation)
// Returns 0 on success, negative error on failure
static int copy_user_path(const char* user_path, char* kbuf, size_t kbuf_size) {
    if (!user_path || !kbuf || kbuf_size < 2) {
        return -EINVAL;
    }
    char* kstr = NULL;
    size_t len = 0;
    int ret = copy_user_string(user_path, kbuf_size - 1, &kstr, &len);
    if (ret != 0) {
        return ret;
    }
    for (size_t i = 0; i <= len; i++) {
        kbuf[i] = kstr[i];
    }
    kfree(kstr);
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
        // SMAP-aware read of user array element
        const char* user_str;
        smap_disable();
        user_str = user_arr[i];
        smap_enable();
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
    
    task_t* cur = sched_current();
    uint64_t flags;
    
    spin_lock_irqsave(&pipe->lock, &flags);
    
    // Block until data is available or all writers are gone
    while (pipe->used == 0) {
        if (pipe->writers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return 0;  // EOF - no more writers
        }
        
        // Non-blocking mode: return EAGAIN immediately
        if (end->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return -EAGAIN;
        }
        
        // Check for pending signals BEFORE blocking
        if (cur && signal_pending(cur)) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return -EINTR;
        }
        
        // Block waiting for data
        if (cur) {
            cur->state = TASK_BLOCKED;
            cur->wait_channel = pipe;  // Wait on the pipe
            spin_unlock_irqrestore(&pipe->lock, flags);
            sched_schedule();
            spin_lock_irqsave(&pipe->lock, &flags);
            // NOTE: Do NOT set cur->state = TASK_READY here!
            // sched_schedule() already set us to TASK_RUNNING on return.
            // Overwriting with TASK_READY causes SMP double-scheduling.
            cur->wait_channel = NULL;
            
            // Check if we were woken by a signal
            if (signal_pending(cur)) {
                spin_unlock_irqrestore(&pipe->lock, flags);
                return -EINTR;
            }
        } else {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return -EAGAIN;
        }
    }

    size_t to_read = (count < pipe->used) ? count : pipe->used;
    size_t first = pipe->size - pipe->read_pos;
    if (first > to_read) {
        first = to_read;
    }

    // SMAP-aware copy to user buffer
    smap_disable();
    mm_memcpy((void*)buf, pipe->buffer + pipe->read_pos, first);
    if (to_read > first) {
        mm_memcpy((void*)(buf + first), pipe->buffer, to_read - first);
    }
    smap_enable();

    pipe->read_pos = (pipe->read_pos + to_read) % pipe->size;
    pipe->used -= to_read;
    
    spin_unlock_irqrestore(&pipe->lock, flags);

    // Wake up writers outside the lock
    sched_wake_channel(pipe);

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

    task_t* cur = sched_current();
    uint64_t flags;

    spin_lock_irqsave(&pipe->lock, &flags);

    if (pipe->readers == 0) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        if (cur) {
            sched_signal_task(cur, SIGPIPE);
        }
        return -EPIPE;
    }

    // Block while pipe is full, waiting for readers to consume data
    while (pipe->used == pipe->size) {
        // Re-check readers (may have closed while we waited)
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (cur) {
                sched_signal_task(cur, SIGPIPE);
            }
            return -EPIPE;
        }

        // Check for pending signals BEFORE blocking
        if (cur && signal_pending(cur)) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return -EINTR;
        }

        // Block waiting for space
        if (cur) {
            cur->state = TASK_BLOCKED;
            cur->wait_channel = pipe;
            spin_unlock_irqrestore(&pipe->lock, flags);
            sched_schedule();
            spin_lock_irqsave(&pipe->lock, &flags);
            cur->wait_channel = NULL;

            // Check if we were woken by a signal
            if (signal_pending(cur)) {
                spin_unlock_irqrestore(&pipe->lock, flags);
                return -EINTR;
            }
        } else {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return -EAGAIN;
        }
    }

    size_t space = pipe->size - pipe->used;
    size_t to_write = (count < space) ? count : space;
    size_t first = pipe->size - pipe->write_pos;
    if (first > to_write) {
        first = to_write;
    }

    // SMAP-aware copy from user buffer
    smap_disable();
    mm_memcpy(pipe->buffer + pipe->write_pos, (void*)buf, first);
    if (to_write > first) {
        mm_memcpy(pipe->buffer, (void*)(buf + first), to_write - first);
    }
    smap_enable();

    pipe->write_pos = (pipe->write_pos + to_write) % pipe->size;
    pipe->used += to_write;
    
    spin_unlock_irqrestore(&pipe->lock, flags);
    
    // Wake up any readers blocked on this pipe
    sched_wake_channel(pipe);

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

// Forward declarations for helper syscalls used before definition
static int64_t sys_getpid(void);
static void sys_exit(uint64_t status);

// Simple umask (global for now)
static uint32_t g_umask = 0022;

// Minimal uname struct (kernel-side)
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} k_utsname_t;

typedef struct {
    long tv_sec;
    long tv_usec;
} k_timeval_t;

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
    
    // Security: Validate count to prevent excessive reads and overflow
    if (count == 0) return 0;
    if (count > (1024ULL * 1024 * 1024)) {
        return -EINVAL;  // Max 1GB per read call
    }
    
    if (!validate_user_ptr(buf, count)) {
        return -EFAULT;
    }
    
    vfs_file_t* file = NULL;
    if (fd < TASK_MAX_FDS) {
        file = cur->fd_table[fd];
    }
    if (!file && fd == STDIN_FD) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        if (tty && tty->fg_pgid == 0) {
            tty->fg_pgid = cur->pgid;
        }
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & ISIG)) {
            sched_signal_task(cur, SIGTTIN);
            return -EIO;
        }
        return tty_read(tty, (void*)buf, (long)count, 0);
    }
    if (!file) {
        return -EBADF;
    }
    
    // Check for console dup markers (magic pointers 1, 2, 3)
    uint64_t marker = (uint64_t)file;
    if (marker == 1) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        if (tty && tty->fg_pgid == 0) {
            tty->fg_pgid = cur->pgid;
        }
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & ISIG)) {
            sched_signal_task(cur, SIGTTIN);
            return -EIO;
        }
        return tty_read(tty, (void*)buf, (long)count, 0);
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

    // Respect open flags (deny read on write-only)
    if (file->flags & O_WRONLY) {
        return -EBADF;
    }

    return vfs_read(file, (void*)buf, (long)count);
}

// SYS_WRITE - write to file descriptor
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Security: Validate count to prevent excessive writes and overflow
    if (count == 0) return 0;
    if (count > (1024ULL * 1024 * 1024)) {
        return -EINVAL;  // Max 1GB per write call
    }
    
    if (!validate_user_ptr(buf, count)) {
        return -EFAULT;
    }
    
    vfs_file_t* file = NULL;
    if (fd < TASK_MAX_FDS) {
        file = cur->fd_table[fd];
    }
    if (!file && (fd == STDOUT_FD || fd == STDERR_FD)) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        if (tty && tty->fg_pgid == 0) {
            tty->fg_pgid = cur->pgid;
        }
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & TOSTOP)) {
            sched_signal_task(cur, SIGTTOU);
            return -EIO;
        }
        return tty_write(tty, (const void*)buf, (long)count);
    }
    if (!file) {
        return -EBADF;
    }
    
    // Check for console dup markers (magic pointers 1, 2, 3)
    uint64_t marker = (uint64_t)file;
    if (marker == 2 || marker == 3) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        if (tty && tty->fg_pgid == 0) {
            tty->fg_pgid = cur->pgid;
        }
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & TOSTOP)) {
            sched_signal_task(cur, SIGTTOU);
            return -EIO;
        }
        return tty_write(tty, (const void*)buf, (long)count);
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

    // Respect open flags (deny write on read-only)
    if ((file->flags & (O_WRONLY | O_RDWR | O_APPEND)) == 0) {
        return -EBADF;
    }

    // Write to filesystem if supported
    long wret = vfs_write(file, (const void*)buf, (long)count);
    if (wret < 0) {
        return wret;
    }
    return wret;
}

static int build_at_path(task_t* cur, int dirfd, const char* path, char* out, size_t out_size);
static int normalize_path(const char* base, const char* path, char* out, size_t out_size);

// Convert VFS status codes to negative errno values
static int vfs_status_to_errno(int st) {
    switch (st) {
        case ST_NOT_FOUND: return -ENOENT;
        case ST_NOMEM:     return -ENOMEM;
        case ST_INVALID:   return -EINVAL;
        case ST_IO:        return -EIO;
        case ST_EXISTS:    return -EEXIST;
        case ST_BUSY:      return -EBUSY;
        case ST_AGAIN:     return -EAGAIN;
        default:           return -EACCES;
    }
}

// SYS_OPEN - open a file
static int64_t sys_open(uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)flags; (void)mode;  // Currently ignore flags/mode
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (!validate_user_ptr(pathname, 1)) {
        return -EFAULT;
    }
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    int fd = alloc_fd(cur);
    if (fd < 0) {
        return fd;  // Error code
    }
    
    vfs_file_t* file = NULL;
    const char* path = kpath;
    char full[VFS_MAX_PATH];
    if (path[0] != '/') {
        int brest = build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
        if (brest != 0) return brest;
        path = full;
    }
    int ret;
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && (path[4] == '/' || path[4] == '\0')) {
        ret = devfs_open_for_task(path, (int)flags, &file, cur);
        if (ret == ST_OK && file) {
            file->refcount = 1;
            file->flags = (int)flags;
        }
    } else {
        ret = vfs_open(path, (int)flags, &file);
    }
    if (ret != ST_OK || file == NULL) {
        return vfs_status_to_errno(ret);
    }
    
    cur->fd_table[fd] = file;
    return fd;
}

// SYS_OPENAT - open a file relative to dirfd
static int64_t sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags, uint64_t mode) {
    (void)mode;
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path string to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    char full[VFS_MAX_PATH];
    int ret;
    if (kpath[0] == '/') {
        size_t i = 0;
        for (; kpath[i] && i < sizeof(full) - 1; ++i) full[i] = kpath[i];
        full[i] = '\0';
    } else {
        ret = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
        if (ret != 0) return ret;
    }
    int fd = alloc_fd(cur);
    if (fd < 0) {
        return fd;
    }
    vfs_file_t* file = NULL;
    if (full[0] == '/' && full[1] == 'd' && full[2] == 'e' && full[3] == 'v' &&
        (full[4] == '/' || full[4] == '\0')) {
        ret = devfs_open_for_task(full, (int)flags, &file, cur);
        if (ret == ST_OK && file) {
            file->refcount = 1;
            file->flags = (int)flags;
        }
    } else {
        ret = vfs_open(full, (int)flags, &file);
    }
    if (ret != ST_OK || file == NULL) {
        return vfs_status_to_errno(ret);
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

static int64_t sys_stat_common(const char* path, uint64_t stat_buf, int validate_path) {
    if (!path || !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
        return -EFAULT;
    }
    if (validate_path && !validate_user_ptr((uint64_t)path, 1)) {
        return -EFAULT;
    }
    // Security: Zero the struct to prevent leaking uninitialized kernel stack data
    struct kstat st;
    mm_memset(&st, 0, sizeof(st));
    int ret = vfs_stat(path, &st);
    if (ret != ST_OK) {
        return (ret == ST_NOT_FOUND) ? -ENOENT : -EINVAL;
    }
    // Security: Use SMAP-aware copy to user
    if (copy_to_user((void*)stat_buf, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_stat(uint64_t pathname, uint64_t stat_buf) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    if (kpath[0] == '/') {
        return sys_stat_common(kpath, stat_buf, 0);
    }
    char full[VFS_MAX_PATH];
    int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
    if (ret != 0) return ret;
    return sys_stat_common(full, stat_buf, 0);
}

static int64_t sys_lstat(uint64_t pathname, uint64_t stat_buf) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    if (kpath[0] == '/') {
        return sys_stat_common(kpath, stat_buf, 0);
    }
    char full[VFS_MAX_PATH];
    int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
    if (ret != 0) return ret;
    return sys_stat_common(full, stat_buf, 0);
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_buf) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(stat_buf, sizeof(struct kstat))) {
        return -EFAULT;
    }
    // Security: Zero the struct to prevent leaking uninitialized kernel stack data
    struct kstat st;
    mm_memset(&st, 0, sizeof(st));
    st.st_nlink = 1;
    st.st_uid = 0;
    st.st_gid = 0;
    st.st_atime = 0;
    st.st_mtime = 0;
    st.st_ctime = 0;
    if (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD) {
        st.st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        st.st_size = 0;
        // Security: Use SMAP-aware copy to user
        return copy_to_user((void*)stat_buf, &st, sizeof(st));
    }
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) {
        return -EBADF;
    }
    vfs_file_t* file = cur->fd_table[fd];
    if (pipe_is_end(file)) {
        st.st_mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        st.st_size = 0;
        // Security: Use SMAP-aware copy to user
        return copy_to_user((void*)stat_buf, &st, sizeof(st));
    }
    if (devfs_fstat(file, &st) == 0) {
        // Security: Use SMAP-aware copy to user
        return copy_to_user((void*)stat_buf, &st, sizeof(st));
    }
    st.st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    st.st_size = vfs_size(file);
    // Security: Use SMAP-aware copy to user
    return copy_to_user((void*)stat_buf, &st, sizeof(st));
}

static int64_t sys_fstatat(uint64_t dirfd, uint64_t pathname, uint64_t stat_buf, uint64_t flags) {
    (void)flags;
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1) || !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
        return -EFAULT;
    }
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    char full[VFS_MAX_PATH];
    if (kpath[0] == '/') {
        size_t i = 0;
        for (; kpath[i] && i < sizeof(full) - 1; ++i) full[i] = kpath[i];
        full[i] = '\0';
    } else {
        int ret = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
        if (ret != 0) return ret;
    }
    return sys_stat_common(full, stat_buf, 0);
}

static int64_t sys_access(uint64_t pathname, uint64_t mode) {
    (void)mode;
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    const char* path = kpath;
    char full[VFS_MAX_PATH];
    if (path[0] != '/') {
        int retb = build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
        if (retb != 0) return retb;
        path = full;
    }
    struct kstat st;
    int ret = vfs_stat(path, &st);
    if (ret != ST_OK) {
        return (ret == ST_NOT_FOUND) ? -ENOENT : -EINVAL;
    }
    return 0;
}

static int64_t sys_faccessat(uint64_t dirfd, uint64_t pathname, uint64_t mode, uint64_t flags) {
    (void)mode; (void)flags;
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) {
        return -EFAULT;
    }
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    char full[VFS_MAX_PATH];
    if (kpath[0] == '/') {
        size_t i = 0;
        for (; kpath[i] && i < sizeof(full) - 1; ++i) full[i] = kpath[i];
        full[i] = '\0';
    } else {
        int ret = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
        if (ret != 0) return ret;
    }
    struct kstat st;
    int st_ret = vfs_stat(full, &st);
    if (st_ret != ST_OK) {
        return (st_ret == ST_NOT_FOUND) ? -ENOENT : -EINVAL;
    }
    return 0;
}

static int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (count == 0) return 0;
    if (!validate_user_ptr(dirp, count)) return -EFAULT;
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) return -EBADF;
    vfs_file_t* file = cur->fd_table[fd];
    uint64_t marker = (uint64_t)file;
    if (marker >= 1 && marker <= 3) return -ENOTDIR;
    if (pipe_is_end(file)) return -ENOTDIR;
    long ret = vfs_readdir(file, (void*)dirp, (long)count);
    if (ret == ST_UNSUPPORTED) return -ENOTDIR;
    return ret;
}

static int64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count) {
    return sys_getdents64(fd, dirp, count);
}

static int64_t sys_chdir(uint64_t pathname) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(pathname, 1)) {
        return -EFAULT;
    }
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    char full[VFS_MAX_PATH];
    const char* cwd = (cur->cwd[0] != 0) ? cur->cwd : "/";
    int ret = normalize_path(cwd, kpath, full, sizeof(full));
    if (ret != 0) return ret;
    struct kstat st;
    int vret = vfs_stat(full, &st);
    if (vret == ST_NOT_FOUND) return -ENOENT;
    if (vret != ST_OK) return -ENOTDIR;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
    // Update FAT32 layer's cwd cluster
    vfs_chdir(full);
    // Update task cwd string with canonical absolute path
    mm_memset(cur->cwd, 0, sizeof(cur->cwd));
    size_t i = 0;
    for (; full[i] && i < sizeof(cur->cwd) - 1; ++i) cur->cwd[i] = full[i];
    cur->cwd[i] = '\0';
    return 0;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (!validate_user_ptr(buf, size)) {
        return -EFAULT;
    }
    const char* src = (cur->cwd[0] != 0) ? cur->cwd : "/";
    size_t len = 0;
    while (src[len]) len++;
    if (size == 0 || len + 1 > size) {
        return -EINVAL;
    }
    if (copy_to_user((void*)buf, src, len + 1) < 0) {
        return -EFAULT;
    }
    return (int64_t)buf;
}

static int64_t sys_umask(uint64_t mask) {
    uint32_t old = g_umask;
    g_umask = (uint32_t)mask;
    return old;
}

static int64_t sys_getuid(void) { return 0; }
static int64_t sys_geteuid(void) { return 0; }
static int64_t sys_getgid(void) { return 0; }
static int64_t sys_getegid(void) { return 0; }

static int64_t sys_setuid(uint64_t uid) { return uid == 0 ? 0 : -EPERM; }
static int64_t sys_seteuid(uint64_t uid) { return uid == 0 ? 0 : -EPERM; }
static int64_t sys_setgid(uint64_t gid) { return gid == 0 ? 0 : -EPERM; }
static int64_t sys_setegid(uint64_t gid) { return gid == 0 ? 0 : -EPERM; }

static int64_t sys_getgroups(uint64_t size, uint64_t list) {
    if (size == 0) return 0;
    if (!validate_user_ptr(list, sizeof(int) * size)) return -EFAULT;
    return 0;
}

static int64_t sys_setgroups(uint64_t size, uint64_t list) {
    (void)size; (void)list;
    return -EPERM;
}

static int64_t sys_gethostname(uint64_t name, uint64_t len) {
    const char* host = "r00tbox";
    size_t hlen = 0;
    while (host[hlen]) hlen++;
    if (!validate_user_ptr(name, len)) return -EFAULT;
    if (len < hlen + 1) return -EINVAL;
    if (copy_to_user((void*)name, host, hlen + 1) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_uname(uint64_t buf) {
    if (!validate_user_ptr(buf, sizeof(k_utsname_t))) return -EFAULT;
    k_utsname_t u;
    mm_memset(&u, 0, sizeof(u));
    const char* sys = "LikeOS";
    const char* node = "r00tbox";
    const char* rel = "0.2-preempt-smp";
    const char* ver = "BlessedKitty";
    const char* mach = "x86_64";
    mm_memcpy(u.sysname, sys, 7);
    mm_memcpy(u.nodename, node, 8);
    mm_memcpy(u.release, rel, 16);
    mm_memcpy(u.version, ver, 13);
    mm_memcpy(u.machine, mach, 7);
    if (copy_to_user((void*)buf, &u, sizeof(u)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_time(uint64_t tloc) {
    uint64_t sec = timer_get_epoch();
    if (tloc && validate_user_ptr(tloc, sizeof(uint64_t))) {
        copy_to_user((void*)tloc, &sec, sizeof(sec));
    }
    return (int64_t)sec;
}

static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz) {
    (void)tz;
    if (!validate_user_ptr(tv, sizeof(k_timeval_t))) return -EFAULT;
    uint64_t ticks = timer_ticks();
    uint32_t freq = timer_get_frequency();
    k_timeval_t kv;
    kv.tv_sec = (long)timer_get_epoch();
    kv.tv_usec = (long)((ticks % freq) * (1000000 / freq));
    if (copy_to_user((void*)tv, &kv, sizeof(kv)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_fsync(uint64_t fd) {
    (void)fd;
    return 0;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t length) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) return -EBADF;
    vfs_file_t* file = cur->fd_table[fd];
    return vfs_truncate(file, (unsigned long)length);
}

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (fd >= TASK_MAX_FDS) return -EBADF;

    vfs_file_t* file = cur->fd_table[fd];

    // Handle console markers: only when fd_table entry is NULL
    // (implicit console fd). If fd_table has a real file/pipe, skip this.
    if (!file && (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD)) {
        if (cmd == 3) { // F_GETFL
            if (fd == STDIN_FD) return O_RDONLY;
            return O_WRONLY;
        }
        if (cmd == 4) { // F_SETFL
            return 0;
        }
        return -EINVAL;
    }

    if (!file) return -EBADF;

    if (pipe_is_end(file)) {
        pipe_end_t* end = (pipe_end_t*)file;
        if (cmd == 3) {
            uint32_t fl = end->is_read ? O_RDONLY : O_WRONLY;
            if (end->flags & O_NONBLOCK) fl |= O_NONBLOCK;
            return fl;
        }
        if (cmd == 4) {
            end->flags = (end->flags & ~O_NONBLOCK) | ((uint32_t)arg & O_NONBLOCK);
            return 0;
        }
        return -EINVAL;
    }

    if (cmd == 3) { // F_GETFL
        return file->flags;
    }
    if (cmd == 4) { // F_SETFL
        // only allow changing O_APPEND
        file->flags = (file->flags & ~O_APPEND) | (arg & O_APPEND);
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    vfs_file_t* file = NULL;
    if (fd < TASK_MAX_FDS) {
        file = cur->fd_table[fd];
    }
    if (!file && (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD)) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        return tty_ioctl(tty, (unsigned long)req, (void*)argp, cur);
    }
    if (!file) {
        return -EBADF;
    }
    return devfs_ioctl(file, (unsigned long)req, (void*)argp, cur);
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (pid == 0) {
        pid = (uint64_t)cur->id;
    }
    if (pgid == 0) {
        pgid = pid;
    }
    task_t* t = sched_find_task_by_id((uint32_t)pid);
    if (!t) {
        return -ESRCH;
    }
    t->pgid = (int)pgid;
    return 0;
}

static int64_t sys_getpgrp(void) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    return cur->pgid;
}

static int64_t sys_tcgetpgrp(uint64_t fd) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    vfs_file_t* file = NULL;
    if (fd < TASK_MAX_FDS) {
        file = cur->fd_table[fd];
    }
    tty_t* tty = NULL;
    if (!file && (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD)) {
        tty = cur->ctty ? cur->ctty : tty_get_console();
    }
    if (file) {
        tty = devfs_get_tty(file);
    }
    if (!tty) {
        return -ENOTTY;
    }
    return tty->fg_pgid;
}

static int64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgrp) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    vfs_file_t* file = NULL;
    if (fd < TASK_MAX_FDS) {
        file = cur->fd_table[fd];
    }
    tty_t* tty = NULL;
    if (!file && (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD)) {
        tty = cur->ctty ? cur->ctty : tty_get_console();
    }
    if (file) {
        tty = devfs_get_tty(file);
    }
    if (!tty) {
        return -ENOTTY;
    }
    tty->fg_pgid = (int)pgrp;
    return 0;
}

// Forward declaration for signal functions
extern ktimer_t timer_create_internal(task_t* task, clockid_t clockid, struct k_sigevent* sevp);
extern int timer_settime_internal(ktimer_t timerid, int flags, 
                                  const struct k_itimerspec* new_value,
                                  struct k_itimerspec* old_value);
extern int timer_gettime_internal(ktimer_t timerid, struct k_itimerspec* curr_value);
extern int timer_getoverrun_internal(ktimer_t timerid);
extern int timer_delete_internal(ktimer_t timerid);

static void kill_task(task_t* t, int sig) {
    if (!t) {
        return;
    }
    // Use sched_signal_task which properly handles SIGKILL/SIGSTOP
    // and other signals with their default actions
    sched_signal_task(t, sig);
}

static int64_t sys_kill(uint64_t pid, uint64_t sig) {
    if (sig > 64) return -EINVAL;
    if ((int64_t)pid < 0) {
        int pgid = -(int)pid;
        if (sig == 0) {
            return sched_pgid_exists(pgid) ? 0 : -ESRCH;
        }
        sched_signal_pgrp(pgid, (int)sig);
        return 0;
    }
    if (pid == 0) {
        /* PID 0 is the kernel idle task — cannot be signalled */
        return -EPERM;
    }
    task_t* t = sched_find_task_by_id((uint32_t)pid);
    if (!t) return -ESRCH;
    // Kernel tasks (idle, init, kernel threads) cannot be signalled
    if (t->privilege == TASK_KERNEL) return -EPERM;
    if (sig == 0) return 0;
    kill_task(t, (int)sig);
    return 0;
}

static int64_t sys_unlink(uint64_t pathname) {
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    int st = vfs_unlink(kpath);
    if (st == ST_OK) return 0;
    if (st == ST_NOT_FOUND) return -ENOENT;
    return -EINVAL;
}

static int64_t sys_rename(uint64_t oldpath, uint64_t newpath) {
    if (!validate_user_ptr(oldpath, 1) || !validate_user_ptr(newpath, 1)) return -EFAULT;
    
    // Copy user paths to kernel buffers first
    char koldpath[VFS_MAX_PATH], knewpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)oldpath, koldpath, sizeof(koldpath));
    if (cret != 0) return cret;
    cret = copy_user_path((const char*)newpath, knewpath, sizeof(knewpath));
    if (cret != 0) return cret;
    
    int st = vfs_rename(koldpath, knewpath);
    if (st == ST_OK) return 0;
    if (st == ST_NOT_FOUND) return -ENOENT;
    return -EINVAL;
}

static int64_t sys_mkdir(uint64_t pathname, uint64_t mode) {
    (void)mode;
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    int st = vfs_mkdir(kpath, (unsigned int)mode);
    if (st == ST_OK) return 0;
    if (st == ST_EXISTS) return -EEXIST;
    if (st == ST_NOT_FOUND) return -ENOENT;
    if (st == ST_NOMEM) return -ENOMEM;
    if (st == ST_IO) return -EIO;
    return -EINVAL;
}
static int64_t sys_rmdir(uint64_t pathname) {
    if (!validate_user_ptr(pathname, 1)) return -EFAULT;
    
    // Copy user path to kernel buffer first
    char kpath[VFS_MAX_PATH];
    int cret = copy_user_path((const char*)pathname, kpath, sizeof(kpath));
    if (cret != 0) return cret;
    
    int st = vfs_rmdir(kpath);
    if (st == ST_OK) return 0;
    if (st == ST_NOT_FOUND) return -ENOENT;
    if (st == ST_NOMEM) return -ENOMEM;
    if (st == ST_IO) return -EIO;
    return -EINVAL;
}
static int64_t sys_link(uint64_t oldpath, uint64_t newpath) { (void)oldpath; (void)newpath; return -ENOSYS; }
static int64_t sys_symlink(uint64_t target, uint64_t linkpath) { (void)target; (void)linkpath; return -ENOSYS; }
static int64_t sys_readlink(uint64_t pathname, uint64_t buf, uint64_t bufsiz) { (void)pathname; (void)buf; (void)bufsiz; return -ENOSYS; }
// chmod/fchmod/chown/fchown: FAT32 has no permissions/ownership; silently succeed
static int64_t sys_chmod(uint64_t pathname, uint64_t mode) { (void)pathname; (void)mode; return 0; }
static int64_t sys_fchmod(uint64_t fd, uint64_t mode) { (void)fd; (void)mode; return 0; }
static int64_t sys_chown(uint64_t pathname, uint64_t owner, uint64_t group) { (void)pathname; (void)owner; (void)group; return 0; }
static int64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group) { (void)fd; (void)owner; (void)group; return 0; }
// utimensat: FAT32 timestamps are managed by the FS driver; silently succeed
static int64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times, uint64_t flags) {
    (void)dirfd; (void)pathname; (void)times; (void)flags; return 0;
}

// Userspace struct statfs layout (must match userland/libc/include/sys/vfs.h)
typedef struct {
    unsigned long f_type;
    unsigned long f_bsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_fsid;
    unsigned long f_namelen;
    unsigned long f_frsize;
    unsigned long f_flags;
    unsigned long f_spare[4];
} user_statfs_t;

// statfs: get filesystem statistics for the given path
static int64_t sys_statfs(uint64_t u_path, uint64_t u_buf) {
    if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
        return -EFAULT;

    char kpath[VFS_MAX_PATH];
    size_t plen;
    int err = user_strnlen((const char*)u_path, VFS_MAX_PATH, &plen);
    if (err) return err;
    err = copy_from_user(kpath, (const void*)u_path, plen + 1);
    if (err) return err;

    // devfs has no meaningful statfs - return ENOSYS
    if (kpath[0] == '/' && kpath[1] == 'd' && kpath[2] == 'e' &&
        kpath[3] == 'v' && (kpath[4] == '\0' || kpath[4] == '/'))
        return -ENOSYS;

    fat32_statfs_t kinfo;
    err = fat32_get_statfs(&kinfo);
    if (err) return err;

    // Translate kernel struct to userspace layout
    user_statfs_t uinfo;
    mm_memset(&uinfo, 0, sizeof(uinfo));
    uinfo.f_type    = kinfo.f_type;
    uinfo.f_bsize   = kinfo.f_bsize;
    uinfo.f_blocks  = kinfo.f_blocks;
    uinfo.f_bfree   = kinfo.f_bfree;
    uinfo.f_bavail  = kinfo.f_bavail;
    uinfo.f_files   = kinfo.f_files;
    uinfo.f_ffree   = kinfo.f_ffree;
    uinfo.f_fsid    = kinfo.f_fsid;
    uinfo.f_namelen = kinfo.f_namelen;
    uinfo.f_frsize  = kinfo.f_frsize;
    uinfo.f_flags   = 0;

    return copy_to_user((void*)u_buf, &uinfo, sizeof(uinfo));
}

// fstatfs: get filesystem statistics for an open file descriptor
static int64_t sys_fstatfs(uint64_t fd, uint64_t u_buf) {
    if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
        return -EFAULT;
    if (fd >= MAX_FDS) return -EBADF;

    task_t* cur = sched_current();
    if (!cur || !cur->fd_table[fd]) return -EBADF;

    // All real file descriptors sit on the FAT32 root FS
    fat32_statfs_t kinfo;
    int err = fat32_get_statfs(&kinfo);
    if (err) return err;

    user_statfs_t uinfo;
    mm_memset(&uinfo, 0, sizeof(uinfo));
    uinfo.f_type    = kinfo.f_type;
    uinfo.f_bsize   = kinfo.f_bsize;
    uinfo.f_blocks  = kinfo.f_blocks;
    uinfo.f_bfree   = kinfo.f_bfree;
    uinfo.f_bavail  = kinfo.f_bavail;
    uinfo.f_files   = kinfo.f_files;
    uinfo.f_ffree   = kinfo.f_ffree;
    uinfo.f_fsid    = kinfo.f_fsid;
    uinfo.f_namelen = kinfo.f_namelen;
    uinfo.f_frsize  = kinfo.f_frsize;
    uinfo.f_flags   = 0;

    return copy_to_user((void*)u_buf, &uinfo, sizeof(uinfo));
}

static int normalize_path(const char* base, const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size < 2) return -EINVAL;
    const char* base_path = (base && base[0]) ? base : "/";
    char combined[VFS_MAX_PATH];
    size_t ci = 0;

    if (path[0] == '/') {
        // Absolute path: copy as-is into combined
        while (path[ci] && ci < sizeof(combined) - 1) {
            combined[ci] = path[ci];
            ci++;
        }
    } else {
        // Relative path: base + '/' + path
        size_t bi = 0;
        while (base_path[bi] && ci < sizeof(combined) - 1) {
            combined[ci++] = base_path[bi++];
        }
        if (ci == 0 || combined[ci - 1] != '/') {
            if (ci < sizeof(combined) - 1) combined[ci++] = '/';
        }
        size_t pi = 0;
        while (path[pi] && ci < sizeof(combined) - 1) {
            combined[ci++] = path[pi++];
        }
    }
    combined[ci] = '\0';

    // Normalize combined into out
    size_t out_len = 0;
    size_t seg_stack[64];
    size_t seg_top = 0;

    out[out_len++] = '/';
    size_t i = 0;
    while (combined[i]) {
        while (combined[i] == '/') i++;
        if (!combined[i]) break;
        char segment[64];
        size_t si = 0;
        while (combined[i] && combined[i] != '/' && si < sizeof(segment) - 1) {
            segment[si++] = combined[i++];
        }
        segment[si] = '\0';

        if (segment[0] == '\0' || (segment[0] == '.' && segment[1] == '\0')) {
            continue;
        }
        if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
            if (seg_top > 0) {
                out_len = seg_stack[--seg_top];
                out[out_len] = '\0';
            } else {
                out_len = 1;
                out[1] = '\0';
            }
            continue;
        }

        if (out_len > 1 && out[out_len - 1] != '/') {
            if (out_len < out_size - 1) out[out_len++] = '/';
        }
        if (out_len >= out_size - 1) return -EINVAL;
        seg_stack[seg_top++] = out_len;
        for (size_t j = 0; j < si && out_len < out_size - 1; ++j) {
            out[out_len++] = segment[j];
        }
        out[out_len] = '\0';
        if (seg_top >= (sizeof(seg_stack) / sizeof(seg_stack[0]))) {
            return -EINVAL;
        }
    }

    if (out_len > 1 && out[out_len - 1] == '/') {
        out[out_len - 1] = '\0';
    } else {
        out[out_len] = '\0';
    }
    return 0;
}

static int build_at_path(task_t* cur, int dirfd, const char* path, char* out, size_t out_size) {
    if (!cur || !path || !out || out_size < 2) return -EINVAL;
    if (dirfd != AT_FDCWD) {
        return -ENOTDIR;
    }
    const char* cwd = (cur->cwd[0] != 0) ? cur->cwd : "/";
    return normalize_path(cwd, path, out, out_size);
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
                // Out of physical memory - rollback pages we already mapped
                for (uint64_t cleanup = old_page; cleanup < addr; cleanup += PAGE_SIZE) {
                    mm_unmap_page_in_address_space(cur->pml4, cleanup);
                }
                return (int64_t)cur->brk;  // Return unchanged brk
            }
            // Zero page via direct map
            mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
            
            uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NO_EXECUTE;
            if (!mm_map_page_in_address_space(cur->pml4, addr, phys, flags)) {
                mm_free_physical_page(phys);
                // Rollback pages we already mapped
                for (uint64_t cleanup = old_page; cleanup < addr; cleanup += PAGE_SIZE) {
                    mm_unmap_page_in_address_space(cur->pml4, cleanup);
                }
                return (int64_t)cur->brk;  // Return unchanged brk
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
    
    // Security: Validate length - must be non-zero and reasonable
    if (length == 0) {
        return (int64_t)MAP_FAILED;
    }
    
    // Security: Prevent integer overflow when aligning length
    if (length > 0x7FFFFFFFFFFFFFF0ULL) {
        return (int64_t)MAP_FAILED;  // Would overflow during PAGE_ALIGN
    }
    
    // Round up length to page size
    length = PAGE_ALIGN(length);
    
    // Security: Prevent excessive allocation (max 2GB per mmap call)
    if (length > (2ULL * 1024 * 1024 * 1024)) {
        return (int64_t)MAP_FAILED;
    }
    
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
        // Security: Reject mappings below 64KB to prevent NULL deref exploits
        if (addr < 0x10000) {
            return (int64_t)MAP_FAILED;
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
        // Security: Reject mappings below 64KB to prevent NULL deref exploits
        if (cur->mmap_base < 0x10000) {
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
        
        // Zero page via direct map
        mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
        
        // For file-backed mappings, read content from file
        if (!is_anonymous && fd < TASK_MAX_FDS && cur->fd_table[fd]) {
            vfs_file_t* file = cur->fd_table[fd];
            // Seek to the correct position and read into direct-mapped address
            long file_off = (long)(offset + off);
            if (vfs_seek(file, file_off, SEEK_SET) >= 0) {
                vfs_read(file, phys_to_virt(phys), PAGE_SIZE);
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

    // Reserve the read end fd before allocating the write end
    cur->fd_table[fd_read] = (vfs_file_t*)read_end;

    int fd_write = alloc_fd(cur);
    if (fd_write < 0) {
        cur->fd_table[fd_read] = NULL;
        pipe_close_end(read_end);
        pipe_close_end(write_end);
        return fd_write;
    }

    cur->fd_table[fd_write] = (vfs_file_t*)write_end;

    // SMAP-aware write to user array
    int* user_pipefd = (int*)pipefd_ptr;
    smap_disable();
    user_pipefd[0] = fd_read;
    user_pipefd[1] = fd_write;
    smap_enable();

    return 0;
}

// SYS_EXIT - exit task
__attribute__((noreturn))
static void sys_exit(uint64_t status) {
    task_t* cur = sched_current();
    if (cur) {
        sched_mark_task_exited(cur, (int)status);
    }
    sched_schedule();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// DEPRECATED: These global externs are no longer used directly.
// Syscall context is now stored per-CPU in percpu_t and copied to task->syscall_*.
// Kept for backward compatibility but should not be referenced in new code.
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;

// External: user_mode_iret_trampoline from syscall.asm
extern void user_mode_iret_trampoline(void);
extern void fork_child_return(void);


// SYS_FORK - fork current process
static int64_t sys_fork(void) {
    task_t* cur = sched_current();
    if (!cur || cur->privilege != TASK_USER) {
        return -1;
    }
    
    // Use task-local copies of user context, not globals!
    // Globals can be overwritten if preemption switches to another task.
    uint64_t user_rip = cur->syscall_rip;         // Where to resume execution
    uint64_t user_rsp = cur->syscall_rsp;         // User stack pointer
    uint64_t user_rflags = cur->syscall_rflags;   // Saved RFLAGS
    
    // Create child with cloned address space and file descriptors
    task_t* child = sched_fork_current();
    if (!child) {
        return -1;  // Fork failed
    }
    
    // Set up child's kernel stack to return to userspace
    // When the child is scheduled, it will resume at user_rip with fork() returning 0
    // 
    // Stack layout (from top to bottom):
    // 1. Saved user callee-saved registers (RBP, RBX, R12-R15)
    // 2. IRET frame: SS, RSP, RFLAGS, CS, RIP (to return to userspace)
    // 3. RAX value (0 for child's fork return value)
    // 4. Saved callee-saved registers for ctx_switch_asm (r15-rbp)
    // 5. Return address (fork_child_return trampoline)
    
    uint64_t* k_sp = (uint64_t*)child->kernel_stack_top;
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL);  // Align to 16 bytes
    
    // Push user callee-saved registers (fork_child_return will restore these)
    // IMPORTANT: Use task-local copies (cur->syscall_*) not globals!
    // Globals can be overwritten by preemption switching to another task.
    *(--k_sp) = cur->syscall_r15;
    *(--k_sp) = cur->syscall_r14;
    *(--k_sp) = cur->syscall_r13;
    *(--k_sp) = cur->syscall_r12;
    *(--k_sp) = cur->syscall_rbx;
    *(--k_sp) = cur->syscall_rbp;
    
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
    *(--k_sp) = 0; // RBP (kernel)
    *(--k_sp) = 0; // RBX (kernel)
    *(--k_sp) = 0; // R12 (kernel)
    *(--k_sp) = 0; // R13 (kernel)
    *(--k_sp) = 0; // R14 (kernel)
    *(--k_sp) = 0; // R15 (kernel)
    
    child->sp = k_sp;
    
    // CRITICAL: Save child PID BEFORE enqueueing!
    // On SMP, another CPU might run the child, it exits, and dead_thread_reap
    // frees it before we return from sched_enqueue_ready. Accessing child->id
    // after enqueue would be use-after-free.
    int32_t child_pid = child->id;
    
    sched_enqueue_ready(child);
    
    // Set need_resched so the parent yields at the next opportunity,
    // giving the new child process a chance to start promptly.
    cur->need_resched = 1;
    
    // Parent returns child's PID (saved before enqueue to avoid use-after-free)
    return child_pid;
}

// SYS_WAIT4/SYS_WAITPID - wait for child process
// In a preemptive kernel, this BLOCKS until a child exits (unless WNOHANG)
static int64_t sys_waitpid(int64_t pid, uint64_t status_ptr, uint64_t options) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Check if there are any children at all
    if (!cur->first_child) {
        return -ECHILD;
    }
    
    // Loop until we find a zombie child or get interrupted
    while (1) {
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
            if (found && found->parent == cur) {
                if (found->has_exited) {
                    child = found;
                }
            } else if (!found || found->parent != cur) {
                // Child doesn't exist or isn't ours
                return -ECHILD;
            }
        }
        
        if (child) {
            // Found a zombie child - reap it
            int status = 0;
            if (child->exit_code >= 128 && child->exit_code < 256) {
                // Signaled exit: low 7 bits are signal number
                status = (child->exit_code - 128) & 0x7F;
            } else {
                // Normal exit: exit_code << 8
                status = (child->exit_code & 0xFF) << 8;
            }
            
            if (status_ptr && validate_user_ptr(status_ptr, sizeof(int))) {
                copy_to_user((void*)status_ptr, &status, sizeof(status));
            }
            
            int child_pid = child->id;
            sched_remove_task(child);
            return child_pid;
        }
        
        // No zombie child yet
        if (options & 1) {  // WNOHANG
            return 0;  // No child exited yet, return immediately
        }
        
        // Check for pending signal BEFORE blocking - avoids race condition
        if (signal_pending(cur)) {
            return -EINTR;
        }
        
        // Block until a child exits or we get a signal
        // CRITICAL: Disable interrupts to prevent race with child exit.
        // The child's exit path checks parent->state under IRQ-disabled section.
        // We must set BLOCKED atomically with respect to that check.
        uint64_t irq_flags = local_irq_save();
        
        // Re-check for zombie children under lock to close the race window
        // where child exits between our check above and setting BLOCKED
        bool found_zombie = false;
        task_t* zombie_check = cur->first_child;
        while (zombie_check) {
            if (zombie_check->has_exited) {
                found_zombie = true;
                break;
            }
            zombie_check = zombie_check->next_sibling;
        }
        
        if (found_zombie) {
            // A child exited while we were about to block - retry the loop
            local_irq_restore(irq_flags);
            continue;  // Jump to top of while(1) to reap the zombie
        }
        
        // No zombie found under lock, safe to block
        cur->state = TASK_BLOCKED;
        cur->wait_channel = cur;  // Waiting for our own children
        local_irq_restore(irq_flags);
        
        sched_schedule();
        // NOTE: Do NOT set cur->state = TASK_READY here!
        // When sched_schedule() returns, the scheduler has already set us
        // to TASK_RUNNING.  Overwriting with TASK_READY causes a race on SMP
        // where sched_wake_expired_sleepers sees READY + !on_rq and enqueues
        // us on another CPU while we're still running → double scheduling.
        cur->wait_channel = 0;
        
        // Check if we were woken by a signal
        if (signal_pending(cur)) {
            return -EINTR;
        }
        
        // Check if we were terminated
        if (cur->has_exited) {
            return -EINTR;
        }
        
        // Loop back to check for zombie children again
    }
}

// SYS_EXECVE - execute a new program, replacing current process image
// This is the POSIX-compliant version that replaces the current task
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

    uint64_t new_stack_ptr = 0;
    uint64_t entry_point = elf_exec_replace(kpath, kargv, kenvp, &new_stack_ptr);

    if (entry_point == 0) {
        // exec failed, return error to caller
        free_user_string_array(kenvp);
        free_user_string_array(kargv);
        kfree(kpath);
        return -ENOEXEC;
    }

    // Set task comm from basename of path (or argv[0])
    {
        task_t* cur = sched_current();
        if (cur) {
            const char* src = kpath;
            // Use basename
            const char* p = src;
            while (*p) { if (*p == '/') src = p + 1; p++; }
            int i;
            for (i = 0; i < 255 && src[i]; i++)
                cur->comm[i] = src[i];
            cur->comm[i] = '\0';
            // Build cmdline from argv (space-separated)
            int pos = 0;
            if (kargv) {
                for (int a = 0; kargv[a] && pos < 1023; a++) {
                    if (a > 0 && pos < 1023) cur->cmdline[pos++] = ' ';
                    for (int c = 0; kargv[a][c] && pos < 1023; c++)
                        cur->cmdline[pos++] = kargv[a][c];
                }
            }
            cur->cmdline[pos] = '\0';
            // Build environ from envp (space-separated)
            pos = 0;
            if (kenvp) {
                for (int a = 0; kenvp[a] && pos < 2047; a++) {
                    if (a > 0 && pos < 2047) cur->environ[pos++] = ' ';
                    for (int c = 0; kenvp[a][c] && pos < 2047; c++)
                        cur->environ[pos++] = kenvp[a][c];
                }
            }
            cur->environ[pos] = '\0';
        }
    }

    free_user_string_array(kenvp);
    free_user_string_array(kargv);
    kfree(kpath);

    // Success! Jump to the new program
    // We need to return to userspace at the new entry point with the new stack
    // Use inline assembly to set up IRET frame and jump
    __asm__ volatile (
        "cli\n\t"
        // Set up IRET frame on current stack
        "push $0x1B\n\t"        // SS (user data segment)
        "push %0\n\t"           // RSP (new user stack)
        "pushfq\n\t"            // RFLAGS
        "orq $0x200, (%%rsp)\n\t" // Enable interrupts in RFLAGS
        "push $0x23\n\t"        // CS (user code segment)
        "push %1\n\t"           // RIP (entry point)
        // Clear registers for clean start
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%rbp, %%rbp\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "iretq\n\t"
        :
        : "r"(new_stack_ptr), "r"(entry_point)
        : "memory"
    );
    
    // Should never reach here
    __builtin_unreachable();
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

// SYS_GETPID - get process ID (thread group ID)
// With thread groups, getpid() returns the tgid (thread group leader's ID)
// which is the same for all threads in the process.
static int64_t sys_getpid(void) {
    task_t* cur = sched_current();
    if (!cur) return -1;
    // Return tgid (thread group ID) which equals id for single-threaded processes
    return cur->tgid;
}

// SYS_YIELD - yield CPU to other runnable tasks (Linux-style)
// Moves current task to back of run queue and immediately reschedules.
// Returns 0 on success. In a preemptive kernel this is a hint to the
// scheduler that the caller is willing to give up its remaining timeslice.
static int64_t sys_yield(void) {
    task_t* cur = sched_current();
    if (!cur) {
        return 0;
    }
    
    // Reset time slice - we're voluntarily giving it up
    cur->remaining_ticks = 0;
    cur->state = TASK_READY;
    
    // Immediate reschedule (Linux does this via schedule())
    sched_schedule();
    
    return 0;
}

// ============================================================================
// Signal Syscalls
// ============================================================================

// SYS_RT_SIGACTION - set signal handler
static int64_t sys_rt_sigaction(uint64_t sig, uint64_t act_ptr, uint64_t oldact_ptr, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(kernel_sigset_t)) {
        return -EINVAL;
    }
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    if (sig_kernel_only(sig)) {
        return -EINVAL;  // Can't change SIGKILL/SIGSTOP
    }
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_sigaction* kact = &cur->signals.action[sig];
    
    // Copy old action if requested
    if (oldact_ptr) {
        if (copy_to_user((void*)oldact_ptr, kact, sizeof(struct k_sigaction)) != 0) {
            return -EFAULT;
        }
    }
    
    // Set new action if provided
    if (act_ptr) {
        struct k_sigaction newact;
        if (copy_from_user(&newact, (void*)act_ptr, sizeof(struct k_sigaction)) != 0) {
            return -EFAULT;
        }
        mm_memcpy(kact, &newact, sizeof(struct k_sigaction));
    }
    
    return 0;
}

// SYS_RT_SIGPROCMASK - change blocked signals
static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr, uint64_t oldset_ptr, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(kernel_sigset_t)) {
        return -EINVAL;
    }
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    kernel_sigset_t* blocked = &cur->signals.blocked;
    
    // Copy old mask if requested
    if (oldset_ptr) {
        if (copy_to_user((void*)oldset_ptr, blocked, sizeof(kernel_sigset_t)) != 0) {
            return -EFAULT;
        }
    }
    
    // Set new mask if provided
    if (set_ptr) {
        kernel_sigset_t newset;
        if (copy_from_user(&newset, (void*)set_ptr, sizeof(kernel_sigset_t)) != 0) {
            return -EFAULT;
        }
        
        switch (how) {
            case SIG_BLOCK:
                sigorset_k(blocked, blocked, &newset);
                break;
            case SIG_UNBLOCK:
                signandset_k(blocked, blocked, &newset);
                break;
            case SIG_SETMASK:
                *blocked = newset;
                break;
            default:
                return -EINVAL;
        }
        
        // Can't block SIGKILL or SIGSTOP
        sigdelset_k(blocked, SIGKILL);
        sigdelset_k(blocked, SIGSTOP);
    }
    
    return 0;
}

// SYS_RT_SIGPENDING - get pending signals
static int64_t sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(kernel_sigset_t)) {
        return -EINVAL;
    }
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    if (copy_to_user((void*)set_ptr, &cur->signals.pending, sizeof(kernel_sigset_t)) != 0) {
        return -EFAULT;
    }
    
    return 0;
}

// SYS_RT_SIGTIMEDWAIT - wait for signal with timeout
static int64_t sys_rt_sigtimedwait(uint64_t set_ptr, uint64_t info_ptr, uint64_t timeout_ptr, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(kernel_sigset_t)) {
        return -EINVAL;
    }
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    kernel_sigset_t wait_set;
    if (copy_from_user(&wait_set, (void*)set_ptr, sizeof(kernel_sigset_t)) != 0) {
        return -EFAULT;
    }
    
    struct k_timespec timeout;
    uint64_t deadline = 0;
    if (timeout_ptr) {
        if (copy_from_user(&timeout, (void*)timeout_ptr, sizeof(struct k_timespec)) != 0) {
            return -EFAULT;
        }
        uint32_t freq = timer_get_frequency();
        uint64_t ticks = timeout.tv_sec * freq + (uint64_t)timeout.tv_nsec * freq / 1000000000ULL;
        deadline = timer_ticks() + ticks;
    }
    
    // Check if any signals in wait_set are already pending
    while (1) {
        for (int sig = 1; sig < NSIG; sig++) {
            if (sigismember_k(&wait_set, sig) && sigismember_k(&cur->signals.pending, sig)) {
                // Found a signal
                siginfo_t info;
                signal_dequeue(cur, &wait_set, &info);
                
                if (info_ptr) {
                    if (copy_to_user((void*)info_ptr, &info, sizeof(siginfo_t)) != 0) {
                        return -EFAULT;
                    }
                }
                return sig;
            }
        }
        
        // Check timeout
        if (timeout_ptr && timer_ticks() >= deadline) {
            return -EAGAIN;
        }
        
        // Block task and wait
        cur->state = TASK_BLOCKED;
        sched_schedule();
        
        // Check if we should exit
        if (cur->has_exited) {
            return -EINTR;
        }
    }
}

// SYS_RT_SIGQUEUEINFO - queue signal with info
static int64_t sys_rt_sigqueueinfo(uint64_t pid, uint64_t sig, uint64_t info_ptr) {
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    
    task_t* target = sched_find_task_by_id((uint32_t)pid);
    if (!target) {
        return -ESRCH;
    }
    
    siginfo_t info;
    if (copy_from_user(&info, (void*)info_ptr, sizeof(siginfo_t)) != 0) {
        return -EFAULT;
    }
    
    // Enforce that si_code indicates user-originated
    info.si_code = SI_QUEUE;
    
    return signal_send(target, (int)sig, &info);
}

// SYS_RT_SIGSUSPEND - suspend until signal
static int64_t sys_rt_sigsuspend(uint64_t mask_ptr, uint64_t sigsetsize) {
    if (sigsetsize != sizeof(kernel_sigset_t)) {
        return -EINVAL;
    }
    
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    kernel_sigset_t newmask;
    if (copy_from_user(&newmask, (void*)mask_ptr, sizeof(kernel_sigset_t)) != 0) {
        return -EFAULT;
    }
    
    // Save current mask and set new one
    cur->signals.saved_mask = cur->signals.blocked;
    cur->signals.blocked = newmask;
    cur->signals.in_sigsuspend = 1;
    
    // Can't block SIGKILL/SIGSTOP
    sigdelset_k(&cur->signals.blocked, SIGKILL);
    sigdelset_k(&cur->signals.blocked, SIGSTOP);
    
    // Block until signal
    cur->state = TASK_BLOCKED;
    
    while (!signal_pending(cur)) {
        sched_schedule();
        if (cur->has_exited) {
            cur->signals.in_sigsuspend = 0;
            cur->signals.blocked = cur->signals.saved_mask;
            return -EINTR;
        }
    }
    
    // Restore mask
    cur->signals.in_sigsuspend = 0;
    cur->signals.blocked = cur->signals.saved_mask;
    
    return -EINTR;  // sigsuspend always returns EINTR
}

// SYS_RT_SIGRETURN - return from signal handler
static int64_t sys_rt_sigreturn(void) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Restore context from the signal frame
    if (signal_restore_frame(cur) < 0) {
        kprintf("sys_rt_sigreturn: failed to restore frame\n");
        return -EFAULT;
    }
    
    // The return value will be ignored - we're restoring the original
    // context which includes the original RAX value
    return 0;
}

// SYS_SIGALTSTACK - set/get alternate signal stack
static int64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Copy old stack if requested
    if (old_ss_ptr) {
        if (copy_to_user((void*)old_ss_ptr, &cur->signals.altstack, sizeof(stack_t)) != 0) {
            return -EFAULT;
        }
    }
    
    // Set new stack if provided
    if (ss_ptr) {
        stack_t newss;
        if (copy_from_user(&newss, (void*)ss_ptr, sizeof(stack_t)) != 0) {
            return -EFAULT;
        }
        
        // Validate
        if (!(newss.ss_flags & SS_DISABLE)) {
            if (newss.ss_size < MINSIGSTKSZ) {
                return -ENOMEM;
            }
        }
        
        cur->signals.altstack = newss;
    }
    
    return 0;
}

// SYS_TKILL - send signal to specific thread
static int64_t sys_tkill(uint64_t tid, uint64_t sig) {
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    
    task_t* target = sched_find_task_by_id((uint32_t)tid);
    if (!target) {
        return -ESRCH;
    }
    
    siginfo_t info;
    mm_memset(&info, 0, sizeof(info));
    info.si_signo = (int)sig;
    info.si_code = SI_TKILL;
    info.si_pid = sched_current() ? sched_current()->id : 0;
    
    return signal_send(target, (int)sig, &info);
}

// SYS_TGKILL - send signal to thread in specific thread group
// This is the secure way to send signals to threads - validates that
// the target thread belongs to the specified thread group.
static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig) {
    if (tgid <= 0 || tid <= 0) {
        return -EINVAL;
    }
    
    if (sig < 0 || sig >= 65) {
        return -EINVAL;
    }
    
    // Find the target thread
    task_t* target = sched_find_task_by_id((uint32_t)tid);
    if (!target) {
        return -ESRCH;
    }
    
    // Validate that target belongs to the specified thread group
    if (target->tgid != (int)tgid) {
        return -ESRCH;  // Thread not in specified group
    }
    
    // sig == 0 is a permission check only
    if (sig == 0) {
        return 0;
    }
    
    // Build siginfo
    siginfo_t info;
    mm_memset(&info, 0, sizeof(info));
    info.si_signo = (int)sig;
    info.si_code = SI_TKILL;
    info.si_pid = sched_current()->tgid;
    
    return signal_send(target, (int)sig, &info);
}

// SYS_ALARM - set alarm clock
static int64_t sys_alarm(uint64_t seconds) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    uint64_t old_remaining = 0;
    
    // Calculate remaining time from old alarm
    uint32_t freq = timer_get_frequency();
    if (cur->signals.alarm_ticks > 0) {
        uint64_t now = timer_ticks();
        if (cur->signals.alarm_ticks > now) {
            old_remaining = (cur->signals.alarm_ticks - now) / freq;
        }
    }
    
    // Set new alarm
    if (seconds > 0) {
        cur->signals.alarm_ticks = timer_ticks() + seconds * freq;
    } else {
        cur->signals.alarm_ticks = 0;  // Cancel
    }
    
    return (int64_t)old_remaining;
}

// SYS_SETITIMER - set interval timer
static int64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr, uint64_t old_value_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_itimerval* timer;
    switch (which) {
        case ITIMER_REAL:
            timer = &cur->signals.itimer_real;
            break;
        case ITIMER_VIRTUAL:
            timer = &cur->signals.itimer_virtual;
            break;
        case ITIMER_PROF:
            timer = &cur->signals.itimer_prof;
            break;
        default:
            return -EINVAL;
    }
    
    // Copy old value if requested
    if (old_value_ptr) {
        if (copy_to_user((void*)old_value_ptr, timer, sizeof(struct k_itimerval)) != 0) {
            return -EFAULT;
        }
    }
    
    // Set new value if provided
    if (new_value_ptr) {
        if (copy_from_user(timer, (void*)new_value_ptr, sizeof(struct k_itimerval)) != 0) {
            return -EFAULT;
        }
    }
    
    return 0;
}

// SYS_GETITIMER - get interval timer
static int64_t sys_getitimer(uint64_t which, uint64_t curr_value_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_itimerval* timer;
    switch (which) {
        case ITIMER_REAL:
            timer = &cur->signals.itimer_real;
            break;
        case ITIMER_VIRTUAL:
            timer = &cur->signals.itimer_virtual;
            break;
        case ITIMER_PROF:
            timer = &cur->signals.itimer_prof;
            break;
        default:
            return -EINVAL;
    }
    
    if (copy_to_user((void*)curr_value_ptr, timer, sizeof(struct k_itimerval)) != 0) {
        return -EFAULT;
    }
    
    return 0;
}

// SYS_TIMER_CREATE - create POSIX timer
static int64_t sys_timer_create(uint64_t clockid, uint64_t sevp_ptr, uint64_t timerid_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_sigevent sevp;
    if (sevp_ptr) {
        if (copy_from_user(&sevp, (void*)sevp_ptr, sizeof(struct k_sigevent)) != 0) {
            return -EFAULT;
        }
    } else {
        // Default
        mm_memset(&sevp, 0, sizeof(sevp));
        sevp.sigev_notify = SIGEV_SIGNAL;
        sevp.sigev_signo = SIGALRM;
    }
    
    ktimer_t tid = timer_create_internal(cur, (clockid_t)clockid, &sevp);
    if (tid < 0) {
        return -EAGAIN;
    }
    
    if (copy_to_user((void*)timerid_ptr, &tid, sizeof(ktimer_t)) != 0) {
        timer_delete_internal(tid);
        return -EFAULT;
    }
    
    return 0;
}

// SYS_TIMER_SETTIME - set POSIX timer
static int64_t sys_timer_settime(uint64_t timerid, uint64_t flags, uint64_t new_value_ptr, uint64_t old_value_ptr) {
    struct k_itimerspec new_value, old_value;
    
    if (copy_from_user(&new_value, (void*)new_value_ptr, sizeof(struct k_itimerspec)) != 0) {
        return -EFAULT;
    }
    
    int ret = timer_settime_internal((ktimer_t)timerid, (int)flags, &new_value, 
                                     old_value_ptr ? &old_value : NULL);
    if (ret < 0) {
        return ret;
    }
    
    if (old_value_ptr) {
        if (copy_to_user((void*)old_value_ptr, &old_value, sizeof(struct k_itimerspec)) != 0) {
            return -EFAULT;
        }
    }
    
    return 0;
}

// SYS_TIMER_GETTIME - get POSIX timer
static int64_t sys_timer_gettime(uint64_t timerid, uint64_t curr_value_ptr) {
    struct k_itimerspec curr_value;
    
    int ret = timer_gettime_internal((ktimer_t)timerid, &curr_value);
    if (ret < 0) {
        return ret;
    }
    
    if (copy_to_user((void*)curr_value_ptr, &curr_value, sizeof(struct k_itimerspec)) != 0) {
        return -EFAULT;
    }
    
    return 0;
}

// SYS_TIMER_GETOVERRUN - get timer overrun count
static int64_t sys_timer_getoverrun(uint64_t timerid) {
    return timer_getoverrun_internal((ktimer_t)timerid);
}

// SYS_TIMER_DELETE - delete POSIX timer
static int64_t sys_timer_delete(uint64_t timerid) {
    return timer_delete_internal((ktimer_t)timerid);
}

// SYS_PAUSE - suspend until signal
static int64_t sys_pause(void) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    // Block until any signal arrives
    cur->state = TASK_BLOCKED;
    
    while (!signal_pending(cur)) {
        sched_schedule();
        if (cur->has_exited) {
            return -EINTR;
        }
    }
    
    return -EINTR;  // pause always returns EINTR
}

// SYS_NANOSLEEP - sleep with nanosecond precision
// Uses timer-based wakeup: set wakeup_tick and block, timer IRQ wakes us
static int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_timespec req;
    if (copy_from_user(&req, (void*)req_ptr, sizeof(struct k_timespec)) != 0) {
        return -EFAULT;
    }
    
    // Calculate ticks to sleep using measured timer frequency
    uint32_t freq = timer_get_frequency();
    uint64_t ticks = req.tv_sec * freq + (uint64_t)req.tv_nsec * freq / 1000000000ULL;
    if (ticks == 0 && (req.tv_sec > 0 || req.tv_nsec > 0)) {
        ticks = 1;  // At least 1 tick for any non-zero sleep
    }
    
    uint64_t start = timer_ticks();
    uint64_t end = start + ticks;
    
    // Loop until sleep time expires or interrupted by signal
    while (timer_ticks() < end) {
        // Check for pending signal BEFORE blocking
        if (signal_pending(cur)) {
            if (rem_ptr) {
                uint64_t now = timer_ticks();
                uint64_t elapsed = now - start;
                uint64_t remaining = (elapsed < ticks) ? ticks - elapsed : 0;
                struct k_timespec rem;
                rem.tv_sec = remaining / freq;
                rem.tv_nsec = (uint64_t)(remaining % freq) * 1000000000ULL / freq;
                copy_to_user((void*)rem_ptr, &rem, sizeof(struct k_timespec));
            }
            cur->wakeup_tick = 0;
            return -EINTR;
        }
        
        // Set wakeup timer and block - timer IRQ will wake us
        cur->wakeup_tick = end;
        cur->state = TASK_BLOCKED;
        sched_schedule();
        
        // Woken up - either by timer expiry or signal
        // Loop will check timer and signal conditions
    }
    
    cur->wakeup_tick = 0;
    return 0;
}

// SYS_CLOCK_GETTIME - get time from specified clock
static int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr) {
    if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
        return -EFAULT;
    }
    
    struct k_timespec tp;
    uint64_t ticks = timer_ticks();
    
    uint32_t freq = timer_get_frequency();
    switch (clk_id) {
        case 0:  // CLOCK_REALTIME
            tp.tv_sec = timer_get_epoch();
            tp.tv_nsec = (uint64_t)(ticks % freq) * 1000000000ULL / freq;
            break;
        case 1:  // CLOCK_MONOTONIC
            // Monotonic = uptime from boot
            tp.tv_sec = ticks / freq;
            tp.tv_nsec = (uint64_t)(ticks % freq) * 1000000000ULL / freq;
            break;
        case 2:  // CLOCK_PROCESS_CPUTIME_ID
        case 3:  // CLOCK_THREAD_CPUTIME_ID
            // Simplified: return same as monotonic for now
            tp.tv_sec = ticks / freq;
            tp.tv_nsec = (uint64_t)(ticks % freq) * 1000000000ULL / freq;
            break;
        default:
            return -EINVAL;
    }
    
    if (copy_to_user((void*)tp_ptr, &tp, sizeof(tp)) != 0) {
        return -EFAULT;
    }
    
    return 0;
}

// SYS_CLOCK_GETRES - get clock resolution
static int64_t sys_clock_getres(uint64_t clk_id, uint64_t res_ptr) {
    if (clk_id > 3) {
        return -EINVAL;
    }
    
    if (res_ptr) {
        if (!validate_user_ptr(res_ptr, sizeof(struct k_timespec))) {
            return -EFAULT;
        }
        
        struct k_timespec res;
        // Resolution = 1 tick in nanoseconds
        res.tv_sec = 0;
        res.tv_nsec = 1000000000 / timer_get_frequency();
        
        if (copy_to_user((void*)res_ptr, &res, sizeof(res)) != 0) {
            return -EFAULT;
        }
    }
    
    return 0;
}

// SYS_SIGNALFD / SYS_SIGNALFD4 - create signalfd (simplified stub)
static int64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t flags) {
    (void)fd;
    (void)mask_ptr;
    (void)flags;
    // signalfd is complex to implement fully - return ENOSYS for now
    return -ENOSYS;
}

// ============================================================================
// SMP/THREADING SYSCALLS - FULL IMPLEMENTATION
// ============================================================================

// Clone flags (Linux compatible)
#define CLONE_VM             0x00000100  // Share memory space
#define CLONE_FS             0x00000200  // Share filesystem info
#define CLONE_FILES          0x00000400  // Share file descriptors
#define CLONE_SIGHAND        0x00000800  // Share signal handlers
#define CLONE_PTRACE         0x00002000  // Allow tracing child
#define CLONE_VFORK          0x00004000  // vfork() semantics
#define CLONE_PARENT         0x00008000  // Same parent as cloner
#define CLONE_THREAD         0x00010000  // Same thread group
#define CLONE_NEWNS          0x00020000  // New mount namespace
#define CLONE_SYSVSEM        0x00040000  // Share SysV semaphore undo
#define CLONE_SETTLS         0x00080000  // Set TLS
#define CLONE_PARENT_SETTID  0x00100000  // Set parent's TID
#define CLONE_CHILD_CLEARTID 0x00200000  // Clear child's TID on exit
#define CLONE_DETACHED       0x00400000  // Unused
#define CLONE_UNTRACED       0x00800000  // Cannot force trace
#define CLONE_CHILD_SETTID   0x01000000  // Set child's TID
#define CLONE_STOPPED        0x02000000  // Start in TASK_STOPPED
#define CLONE_NEWUTS         0x04000000  // New UTS namespace
#define CLONE_NEWIPC         0x08000000  // New IPC namespace

// Forward declare from sched.c
extern void thread_group_add(task_t* leader, task_t* thread);
extern void thread_group_init(task_t* leader);
extern mm_struct_t* mm_struct_create(uint64_t* pml4);
extern void mm_struct_get(mm_struct_t* mm);
extern files_struct_t* files_struct_create(void);
extern files_struct_t* files_struct_clone(files_struct_t* src);
extern void files_struct_get(files_struct_t* files);
extern sighand_struct_t* sighand_struct_create(void);
extern sighand_struct_t* sighand_struct_clone(sighand_struct_t* src);
extern void sighand_struct_get(sighand_struct_t* sighand);
extern void task_set_fs_base(task_t* task, uint64_t base);

// PID allocator (from sched.c)
extern int g_next_id;
extern spinlock_t g_task_list_lock;

// SYS_CLONE - create a new thread or process
// Full implementation with all CLONE_* flags
static int64_t sys_clone(uint64_t flags, uint64_t child_stack, 
                         uint64_t parent_tidptr, uint64_t child_tidptr,
                         uint64_t tls) {
    task_t* cur = sched_current();
    if (!cur || cur->privilege != TASK_USER) {
        return -EPERM;
    }
    
    // Validate flag combinations
    // CLONE_THREAD requires CLONE_SIGHAND which requires CLONE_VM
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) {
        return -EINVAL;
    }
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
        return -EINVAL;
    }
    
    // Extract flag meanings
    bool share_vm = (flags & CLONE_VM) != 0;
    bool share_files = (flags & CLONE_FILES) != 0;
    bool share_sighand = (flags & CLONE_SIGHAND) != 0;
    bool is_thread = (flags & CLONE_THREAD) != 0;
    bool set_tls = (flags & CLONE_SETTLS) != 0;
    bool set_parent_tid = (flags & CLONE_PARENT_SETTID) != 0;
    bool set_child_tid = (flags & CLONE_CHILD_SETTID) != 0;
    bool clear_child_tid = (flags & CLONE_CHILD_CLEARTID) != 0;
    
    // For threads, child_stack is required
    if (is_thread && child_stack == 0) {
        return -EINVAL;
    }
    
    // Allocate child task structure
    task_t* child = (task_t*)kalloc(sizeof(task_t));
    if (!child) {
        return -ENOMEM;
    }
    
    // Allocate kernel stack for child
    uint8_t* k_stack_mem = (uint8_t*)kalloc(8192);
    if (!k_stack_mem) {
        kfree(child);
        return -ENOMEM;
    }
    // Zero the kernel stack to prevent stale data issues
    mm_memset(k_stack_mem, 0, 8192);
    uint64_t k_stack_top = ((uint64_t)(k_stack_mem + 8192)) & ~0xFUL;
    
    // Initialize child from parent
    mm_memcpy(child, cur, sizeof(task_t));
    
    // Assign unique ID
    uint64_t irq_flags;
    spin_lock_irqsave(&g_task_list_lock, &irq_flags);
    child->id = g_next_id++;
    spin_unlock_irqrestore(&g_task_list_lock, irq_flags);
    
    // Basic child setup
    child->state = TASK_READY;
    child->kernel_stack_top = k_stack_top;
    child->kernel_stack_base = k_stack_mem;
    child->rq_next = NULL;
    child->on_rq = false;
    child->wait_next = NULL;
    child->wait_channel = NULL;
    child->wakeup_tick = 0;
    child->need_resched = 0;
    child->remaining_ticks = SCHED_TIME_SLICE;
    child->preempt_frame = NULL;
    child->exit_code = 0;
    child->has_exited = false;
    child->is_fork_child = true;
    child->first_child = NULL;
    child->next_sibling = NULL;
    
    // Handle CLONE_VM (share address space)
    if (share_vm) {
        // Share parent's address space with reference counting
        if (cur->mm) {
            // Parent already uses mm_struct
            mm_struct_get(cur->mm);
            child->mm = cur->mm;
            child->pml4 = cur->mm->pml4;
        } else {
            // First time: create mm_struct from parent's legacy pml4
            cur->mm = mm_struct_create(cur->pml4);
            if (!cur->mm) {
                kfree(k_stack_mem);
                kfree(child);
                return -ENOMEM;
            }
            cur->mm->brk = cur->brk;
            cur->mm->brk_start = cur->brk_start;
            cur->mm->mmap_base = cur->mmap_base;
            
            mm_struct_get(cur->mm);
            child->mm = cur->mm;
            child->pml4 = cur->pml4;
        }
    } else {
        // COW clone of address space
        uint64_t* child_pml4 = mm_clone_address_space(cur->pml4);
        if (!child_pml4) {
            kfree(k_stack_mem);
            kfree(child);
            return -ENOMEM;
        }
        child->pml4 = child_pml4;
        child->mm = NULL;  // Use legacy fields
    }
    
    // Handle CLONE_FILES (share file descriptors)
    if (share_files) {
        if (cur->files) {
            files_struct_get(cur->files);
            child->files = cur->files;
        } else {
            // Create files_struct from parent's legacy fd_table
            cur->files = files_struct_create();
            if (!cur->files) {
                if (!share_vm && child->pml4) {
                    mm_destroy_address_space(child->pml4);
                }
                kfree(k_stack_mem);
                kfree(child);
                return -ENOMEM;
            }
            // Copy existing fd_table to files_struct
            for (int i = 0; i < TASK_MAX_FDS; i++) {
                cur->files->fd_table[i] = cur->fd_table[i];
            }
            
            files_struct_get(cur->files);
            child->files = cur->files;
        }
        // Clear legacy fd_table (now using files_struct)
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            child->fd_table[i] = NULL;
        }
    } else {
        // Clone file descriptors
        child->files = NULL;
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            vfs_file_t* src_fd = cur->files ? cur->files->fd_table[i] : cur->fd_table[i];
            if (src_fd) {
                uint64_t marker = (uint64_t)src_fd;
                if (marker >= 1 && marker <= 3) {
                    child->fd_table[i] = src_fd;
                } else if (pipe_is_end(src_fd)) {
                    child->fd_table[i] = (vfs_file_t*)pipe_dup_end((pipe_end_t*)src_fd);
                } else {
                    child->fd_table[i] = vfs_dup(src_fd);
                }
            } else {
                child->fd_table[i] = NULL;
            }
        }
    }
    
    // Handle CLONE_SIGHAND (share signal handlers)
    if (share_sighand) {
        if (cur->sighand) {
            sighand_struct_get(cur->sighand);
            child->sighand = cur->sighand;
        } else {
            // Create sighand_struct from parent's legacy signal handlers
            cur->sighand = sighand_struct_create();
            if (!cur->sighand) {
                // Cleanup and fail
                if (share_files && child->files) {
                    // files_struct_put would be called in cleanup
                }
                if (!share_vm && child->pml4) {
                    mm_destroy_address_space(child->pml4);
                }
                kfree(k_stack_mem);
                kfree(child);
                return -ENOMEM;
            }
            // Copy signal handlers
            for (int i = 0; i < 65; i++) {
                cur->sighand->action[i] = cur->signals.action[i];
            }
            
            sighand_struct_get(cur->sighand);
            child->sighand = cur->sighand;
        }
    } else {
        // Copy signal handlers (already done by memcpy)
        child->sighand = NULL;
        signal_fork_copy(child, cur);
    }
    
    // Handle CLONE_THREAD (same thread group)
    if (is_thread) {
        // Add to parent's thread group
        thread_group_add(cur->group_leader, child);
        child->exit_signal = 0;  // Threads don't send exit signal
        child->parent = cur->parent;  // Same parent as thread group leader
    } else {
        // New process (new thread group)
        thread_group_init(child);
        child->exit_signal = SIGCHLD;
        child->parent = cur;
        sched_add_child(cur, child);
    }
    
    // Handle CLONE_SETTLS
    if (set_tls) {
        task_set_fs_base(child, tls);
    } else {
        child->fs_base = 0;
    }
    
    // Handle CLONE_CHILD_CLEARTID
    if (clear_child_tid && child_tidptr) {
        if (validate_user_ptr(child_tidptr, sizeof(int))) {
            child->clear_child_tid = (uint64_t*)child_tidptr;
        }
    } else {
        child->clear_child_tid = NULL;
    }
    
    // Handle CLONE_CHILD_SETTID
    if (set_child_tid && child_tidptr) {
        if (validate_user_ptr(child_tidptr, sizeof(int))) {
            child->set_child_tid = (uint64_t*)child_tidptr;
            // This will be written when child starts
        }
    } else {
        child->set_child_tid = NULL;
    }
    
    // Handle CLONE_PARENT_SETTID
    if (set_parent_tid && parent_tidptr) {
        if (validate_user_ptr(parent_tidptr, sizeof(int))) {
            smap_disable();
            *(int*)parent_tidptr = child->id;
            smap_enable();
        }
    }
    
    // Clear robust list (not inherited)
    child->robust_list = NULL;
    child->robust_list_len = 0;
    
    // Copy mmap regions (if not sharing VM)
    if (!share_vm) {
        for (int i = 0; i < TASK_MAX_MMAP; i++) {
            child->mmap_regions[i] = cur->mmap_regions[i];
        }
    }
    
    // Assign to least-loaded CPU
    extern int g_smp_initialized;
    if (g_smp_initialized) {
        child->on_cpu = percpu_find_least_loaded_cpu();
    } else {
        child->on_cpu = 0;
    }
    
    // Set up child's kernel stack to return to userspace
    uint64_t user_rip = cur->syscall_rip;
    uint64_t user_rsp = child_stack ? child_stack : cur->syscall_rsp;
    uint64_t user_rflags = cur->syscall_rflags;
    
    uint64_t* k_sp = (uint64_t*)child->kernel_stack_top;
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL);
    
    // Push user callee-saved registers
    *(--k_sp) = cur->syscall_r15;
    *(--k_sp) = cur->syscall_r14;
    *(--k_sp) = cur->syscall_r13;
    *(--k_sp) = cur->syscall_r12;
    *(--k_sp) = cur->syscall_rbx;
    *(--k_sp) = cur->syscall_rbp;
    
    // IRET frame
    *(--k_sp) = 0x1B;               // SS
    *(--k_sp) = user_rsp;           // RSP
    *(--k_sp) = user_rflags | 0x200;// RFLAGS with IF
    *(--k_sp) = 0x23;               // CS
    *(--k_sp) = user_rip;           // RIP
    
    // Return value for child (0 or TID depending on CLONE_CHILD_SETTID)
    *(--k_sp) = 0;  // RAX = 0 for child
    
    // Kernel callee-saved for context switch
    *(--k_sp) = (uint64_t)fork_child_return;
    *(--k_sp) = 0; // RBP
    *(--k_sp) = 0; // RBX
    *(--k_sp) = 0; // R12
    *(--k_sp) = 0; // R13
    *(--k_sp) = 0; // R14
    *(--k_sp) = 0; // R15
    
    child->sp = k_sp;
    
    // Add to global task list
    spin_lock_irqsave(&g_task_list_lock, &irq_flags);
    extern void task_list_add(task_t* t);
    task_list_add(child);
    spin_unlock_irqrestore(&g_task_list_lock, irq_flags);
    
    // Handle CLONE_CHILD_SETTID: write TID to child's address space
    // This must be done after child is set up but before scheduling
    if (set_child_tid && child->set_child_tid) {
        smap_disable();
        *(int*)(child->set_child_tid) = child->id;
        smap_enable();
    }
    
    // IMPORTANT: Save child->id BEFORE enqueueing!
    // Once enqueued, another CPU might run and free the child before we read it.
    int64_t child_pid = child->id;
    
    // Enqueue child
    sched_enqueue_ready(child);
    
    // Set need_resched so the parent yields at the next opportunity,
    // giving the newly created thread a chance to start promptly.
    // This is the standard behavior: thread creation is a reschedule point.
    cur->need_resched = 1;
    
    return child_pid;
}

// SYS_VFORK - create child that shares parent's memory until exec/exit
static int64_t sys_vfork(void) {
    // For now, implement as regular fork
    // True vfork semantics would suspend parent until child execs or exits
    return sys_fork();
}

// SYS_EXIT_GROUP - terminate all threads in the process
static void sys_exit_group(uint64_t status) {
    task_t* cur = sched_current();
    if (!cur) {
        while(1) { __asm__ volatile("hlt"); }
    }
    
    task_t* leader = cur->group_leader;
    if (!leader) leader = cur;
    
    // Mark the group as exiting to prevent new threads
    leader->group_exiting = true;
    leader->group_exit_code = (int)status;
    
    // Signal all threads in the group to exit (except ourselves)
    task_t* t = leader;
    do {
        if (t != cur && !t->has_exited) {
            // Send SIGKILL to force immediate termination
            sched_signal_task(t, SIGKILL);
        }
        t = t->thread_group_next;
    } while (t != leader);
    
    // Now exit ourselves
    sched_mark_task_exited(cur, (int)status);
    sched_schedule();
    
    // Never returns
    while(1) { __asm__ volatile("hlt"); }
}

// SYS_GETTID - get thread ID (unique per thread)
static int64_t sys_gettid(void) {
    task_t* cur = sched_current();
    if (!cur) return -ESRCH;
    return cur->id;  // TID is always the unique task ID
}

// SYS_SET_TID_ADDRESS - set address for clear-on-exit notification
static int64_t sys_set_tid_address(uint64_t tidptr) {
    task_t* cur = sched_current();
    if (!cur) return -ESRCH;
    
    if (tidptr && validate_user_ptr(tidptr, sizeof(int))) {
        cur->clear_child_tid = (uint64_t*)tidptr;
    } else {
        cur->clear_child_tid = NULL;
    }
    
    return cur->id;
}

// Futex operations
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_TRYLOCK_PI    8
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128

// SYS_FUTEX - fast userspace mutex operations (uses hash-bucket implementation)
static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                         uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    (void)val3;  // Used for FUTEX_CMP_REQUEUE comparison value
    
    int cmd = op & ~FUTEX_PRIVATE_FLAG;
    
    if (!validate_user_ptr(uaddr, sizeof(uint32_t))) {
        return -EFAULT;
    }
    
    switch (cmd) {
        case FUTEX_WAIT: {
            // Convert timeout to nanoseconds
            uint64_t timeout_ns = 0;
            if (timeout) {
                // timeout points to struct timespec
                if (validate_user_ptr(timeout, sizeof(struct k_timespec))) {
                    smap_disable();
                    struct k_timespec* ts = (struct k_timespec*)timeout;
                    timeout_ns = (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
                    smap_enable();
                }
            }
            return futex_wait(uaddr, (uint32_t)val, timeout_ns);
        }
        
        case FUTEX_WAKE:
            return futex_wake(uaddr, (int)val);
        
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
            if (!validate_user_ptr(uaddr2, sizeof(uint32_t))) {
                return -EFAULT;
            }
            // For CMP_REQUEUE, val3 is the expected value to compare
            if (cmd == FUTEX_CMP_REQUEUE) {
                smap_disable();
                uint32_t curval = *(volatile uint32_t*)uaddr;
                smap_enable();
                if (curval != (uint32_t)val3) {
                    return -EAGAIN;
                }
            }
            // val = nr_wake, timeout = nr_requeue (reusing timeout arg)
            return futex_requeue(uaddr, uaddr2, (int)val, (int)timeout);
        
        default:
            return -ENOSYS;
    }
}

// SYS_SET_ROBUST_LIST - set robust futex list head
static int64_t sys_set_robust_list(uint64_t head, uint64_t len) {
    task_t* cur = sched_current();
    if (!cur) return -ESRCH;
    
    // Validate the length matches expected structure size
    if (len != sizeof(struct robust_list_head)) {
        return -EINVAL;
    }
    
    if (head && !validate_user_ptr(head, len)) {
        return -EFAULT;
    }
    
    cur->robust_list = (struct robust_list_head*)head;
    cur->robust_list_len = len;
    
    return 0;
}

// SYS_GET_ROBUST_LIST - get robust futex list head
static int64_t sys_get_robust_list(uint64_t pid, uint64_t head_ptr, uint64_t len_ptr) {
    task_t* target;
    
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) return -ESRCH;
    
    // Permission check: can only get own robust list or if privileged
    task_t* cur = sched_current();
    if (target != cur && target->parent != cur) {
        return -EPERM;
    }
    
    if (!validate_user_ptr(head_ptr, sizeof(void*)) ||
        !validate_user_ptr(len_ptr, sizeof(size_t))) {
        return -EFAULT;
    }
    
    smap_disable();
    *(struct robust_list_head**)head_ptr = target->robust_list;
    *(size_t*)len_ptr = target->robust_list_len;
    smap_enable();
    
    return 0;
}

// arch_prctl codes
#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

// SYS_ARCH_PRCTL - architecture-specific thread state
static int64_t sys_arch_prctl(uint64_t code, uint64_t addr) {
    task_t* cur = sched_current();
    if (!cur) return -ESRCH;
    
    switch (code) {
        case ARCH_SET_FS: {
            // Reject non-canonical addresses — wrmsr to MSR_FS_BASE
            // will #GP if bits 63:48 are not a sign-extension of bit 47.
            uint64_t top = addr >> 47;
            if (addr != 0 && top != 0 && top != 0x1FFFF) {
                return -EINVAL;
            }
            task_set_fs_base(cur, addr);
            // Apply immediately
            task_load_tls(cur);
            return 0;
        }
        
        case ARCH_GET_FS:
            if (!validate_user_ptr(addr, sizeof(uint64_t))) {
                return -EFAULT;
            }
            smap_disable();
            *(uint64_t*)addr = cur->fs_base;
            smap_enable();
            return 0;
        
        case ARCH_SET_GS: {
            // Reject non-canonical addresses
            uint64_t top = addr >> 47;
            if (addr != 0 && top != 0 && top != 0x1FFFF) {
                return -EINVAL;
            }
            cur->gs_base = addr;
            return 0;
        }
        
        case ARCH_GET_GS:
            if (!validate_user_ptr(addr, sizeof(uint64_t))) {
                return -EFAULT;
            }
            smap_disable();
            *(uint64_t*)addr = cur->gs_base;
            smap_enable();
            return 0;
        
        default:
            return -EINVAL;
    }
}

// Scheduling policies
#define SCHED_NORMAL    0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
#define SCHED_DEADLINE  6

// CPU set for affinity
#define CPU_SETSIZE 64
typedef struct {
    uint64_t bits[CPU_SETSIZE / 64];
} cpu_set_t;

// SYS_SCHED_SETAFFINITY - bind thread to specific CPUs
static int64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpusetsize, uint64_t mask_ptr) {
    (void)cpusetsize;
    
    if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
        return -EFAULT;
    }
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    // Read affinity mask from user
    smap_disable();
    uint64_t mask = *(uint64_t*)mask_ptr;
    smap_enable();
    
    // Validate: at least one CPU must be set
    if (mask == 0) {
        return -EINVAL;
    }
    
    // Store the full affinity mask (0 means all CPUs allowed)
    target->cpu_affinity = mask;
    
    // If current CPU is not in the new affinity mask, migrate to first allowed CPU
    if (!(mask & (1ULL << target->on_cpu))) {
        for (int i = 0; i < 64; i++) {
            if (mask & (1ULL << i)) {
                target->on_cpu = (uint32_t)i;
                // Mark for reschedule to migrate
                target->need_resched = 1;
                break;
            }
        }
    }
    
    return 0;
}

// SYS_SCHED_GETAFFINITY - get CPU affinity mask
static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize, uint64_t mask_ptr) {
    (void)cpusetsize;
    
    if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
        return -EFAULT;
    }
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    // Return stored affinity mask, or all CPUs if not set
    uint64_t mask = target->cpu_affinity;
    if (mask == 0) {
        // Affinity not set = all CPUs allowed, return mask with all online CPUs
        uint32_t cpu_count = smp_get_cpu_count();
        mask = (1ULL << cpu_count) - 1;
        if (mask == 0) mask = 1;  // At least CPU 0
    }
    
    smap_disable();
    *(uint64_t*)mask_ptr = mask;
    smap_enable();
    
    return sizeof(uint64_t);
}

// Scheduling parameters
struct sched_param {
    int sched_priority;
};

// SYS_SCHED_SETSCHEDULER - set scheduling policy
static int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy, uint64_t param_ptr) {
    (void)param_ptr;
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    // We only support SCHED_NORMAL for now
    if (policy != SCHED_NORMAL && policy != SCHED_RR) {
        return -EINVAL;
    }
    
    return 0;
}

// SYS_SCHED_GETSCHEDULER - get scheduling policy
static int64_t sys_sched_getscheduler(uint64_t pid) {
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    return SCHED_NORMAL;  // We use round-robin by default
}

// SYS_SCHED_SETPARAM - set scheduling parameters
static int64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr) {
    (void)param_ptr;
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    // Accept but ignore (we use fixed round-robin)
    return 0;
}

// SYS_SCHED_GETPARAM - get scheduling parameters
static int64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr) {
    if (!validate_user_ptr(param_ptr, sizeof(struct sched_param))) {
        return -EFAULT;
    }
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    struct sched_param param = { .sched_priority = 0 };
    
    smap_disable();
    *(struct sched_param*)param_ptr = param;
    smap_enable();
    
    return 0;
}

// SYS_SCHED_GET_PRIORITY_MAX - get max priority for policy
static int64_t sys_sched_get_priority_max(uint64_t policy) {
    switch (policy) {
        case SCHED_FIFO:
        case SCHED_RR:
            return 99;
        case SCHED_NORMAL:
        case SCHED_BATCH:
        case SCHED_IDLE:
            return 0;
        default:
            return -EINVAL;
    }
}

// SYS_SCHED_GET_PRIORITY_MIN - get min priority for policy
static int64_t sys_sched_get_priority_min(uint64_t policy) {
    switch (policy) {
        case SCHED_FIFO:
        case SCHED_RR:
            return 1;
        case SCHED_NORMAL:
        case SCHED_BATCH:
        case SCHED_IDLE:
            return 0;
        default:
            return -EINVAL;
    }
}

// SYS_SCHED_RR_GET_INTERVAL - get round-robin time quantum
static int64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr) {
    if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
        return -EFAULT;
    }
    
    task_t* target;
    if (pid == 0) {
        target = sched_current();
    } else {
        target = sched_find_task_by_id((uint32_t)pid);
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    // Return time slice (at 100Hz, 2 ticks = 20ms)
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 };  // 20ms
    
    smap_disable();
    *(struct k_timespec*)tp_ptr = ts;
    smap_enable();
    
    return 0;
}

// SYS_MPROTECT - change memory protection
static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    task_t* cur = sched_current();
    if (!cur) {
        return -ESRCH;
    }
    
    // Validate alignment
    if (addr & (PAGE_SIZE - 1)) {
        return -EINVAL;
    }
    
    // Round up length to page boundary
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Build page flags
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (prot & 0x2) {  // PROT_WRITE
        flags |= PAGE_WRITABLE;
    }
    if (!(prot & 0x4)) {  // !PROT_EXEC
        flags |= PAGE_NO_EXECUTE;
    }
    
    // Update page table entries
    uint64_t* pml4 = cur->pml4;
    if (!pml4) {
        return -EFAULT;
    }
    
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = addr + i * PAGE_SIZE;
        
        // Get current PTE
        uint64_t phys = mm_get_physical_address(vaddr);
        if (phys == 0) {
            // Page not mapped
            continue;
        }
        
        // Remap with new protection
        mm_map_page_in_address_space(pml4, vaddr, phys, flags);
    }
    
    // Flush TLB for modified pages on local CPU
    // Use virt_to_phys() for the PML4 pointer itself (not mm_get_physical_address)
    __asm__ volatile("mov %0, %%cr3" : : "r"(virt_to_phys(pml4)));
    
    // TLB shootdown: threads sharing this address space (CLONE_VM) may be running
    // on other CPUs with stale TLB entries. Broadcast invalidation to all CPUs.
    smp_tlb_shootdown_sync();
    
    return 0;
}

// Linux reboot() magic numbers and commands
#define LINUX_REBOOT_MAGIC1        0xfee1dead
#define LINUX_REBOOT_MAGIC2        672274793   // 0x28121969
#define LINUX_REBOOT_MAGIC2A       85072278
#define LINUX_REBOOT_MAGIC2B       369367448
#define LINUX_REBOOT_MAGIC2C       537993216

#define LINUX_REBOOT_CMD_RESTART   0x01234567
#define LINUX_REBOOT_CMD_HALT      0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON    0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF   0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART2  0xA1B2C3D4

static int g_cad_enabled = 0;  // Ctrl-Alt-Del behaviour

static int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, uint64_t arg) {
    // Validate magic numbers
    if ((uint32_t)magic1 != LINUX_REBOOT_MAGIC1)
        return -EINVAL;
    
    uint32_t m2 = (uint32_t)magic2;
    if (m2 != LINUX_REBOOT_MAGIC2 && m2 != LINUX_REBOOT_MAGIC2A &&
        m2 != LINUX_REBOOT_MAGIC2B && m2 != LINUX_REBOOT_MAGIC2C)
        return -EINVAL;
    
    // Only root (PID 1 shell or PID 0) can reboot - we don't have UID so check PID <= 2
    // In a simple OS, just allow it from any process for now
    
    switch ((uint32_t)cmd) {
        case LINUX_REBOOT_CMD_RESTART:
            kprintf("[REBOOT] System is going down for reboot NOW!\n");
            __asm__ volatile("cli");
            smp_halt_others();
            acpi_reset();
            for (;;) __asm__ volatile("hlt");
        
        case LINUX_REBOOT_CMD_HALT:
            kprintf("[HALT] System halted.\n");
            __asm__ volatile("cli");
            smp_halt_others();
            for (;;) __asm__ volatile("hlt");
        
        case LINUX_REBOOT_CMD_POWER_OFF:
            kprintf("[POWEROFF] Power down.\n");
            __asm__ volatile("cli");
            smp_halt_others();
            acpi_poweroff();
            for (;;) __asm__ volatile("hlt");
            
        case LINUX_REBOOT_CMD_CAD_ON:
            g_cad_enabled = 1;
            return 0;
            
        case LINUX_REBOOT_CMD_CAD_OFF:
            g_cad_enabled = 0;
            return 0;
            
        case LINUX_REBOOT_CMD_RESTART2: {
            // arg is a pointer to a command string (ignored in our impl)
            kprintf("[REBOOT] System is going down for reboot NOW!\n");
            __asm__ volatile("cli");
            smp_halt_others();
            acpi_reset();
            for (;;) __asm__ volatile("hlt");
        }
            
        default:
            return -EINVAL;
    }
}

// SYS_GETPROCINFO - retrieve info about all processes
// a1 = pointer to user-space procinfo_t array
// a2 = max number of entries the array can hold
// Returns: number of entries filled, or negative error
static int64_t sys_getprocinfo(uint64_t buf_ptr, uint64_t max_count) {
    if (max_count == 0) return 0;
    if (!validate_user_ptr(buf_ptr, max_count * sizeof(procinfo_t)))
        return -EFAULT;

    // Allocate a kernel-side buffer (limit to prevent DoS)
    if (max_count > 4096) max_count = 4096;
    size_t buf_size = max_count * sizeof(procinfo_t);
    procinfo_t* kbuf = (procinfo_t*)kalloc(buf_size);
    if (!kbuf) return -ENOMEM;
    mm_memset(kbuf, 0, buf_size);

    uint64_t freq = timer_get_frequency();
    if (freq == 0) freq = 100;

    uint64_t flags;
    int count = 0;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    
    // g_task_list_head is declared static in sched.c, but we can
    // iterate using sched_find_task_by_id or we use extern.
    // Actually we declared g_task_list_lock extern in sched.h, 
    // but not g_task_list_head. Let's just use a different approach:
    // iterate IDs from 0 upward.
    // Actually, let's access the list directly. We need to declare it extern.
    // For now, use the approach of iterating via sched_find_task_by_id
    // which acquires its own lock... but we already hold the lock.
    // Better: we declared an extern iterator in the header or iterate by PID.
    
    // We'll iterate PIDs. Not ideal but safe. sched_find_task_by_id
    // acquires the lock internally, so we must NOT hold it here.
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    
    // Iterate all possible PIDs (g_next_id is the next ID to assign)
    extern int g_next_id;
    int max_pid = g_next_id;
    
    for (int pid = 0; pid < max_pid && count < (int)max_count; pid++) {
        spin_lock_irqsave(&g_task_list_lock, &flags);
        task_t* t = sched_find_task_by_id_locked(pid);
        if (!t) {
            spin_unlock_irqrestore(&g_task_list_lock, flags);
            continue;
        }
        
        procinfo_t* p = &kbuf[count];
        p->pid = t->id;
        p->ppid = t->parent ? t->parent->id : 0;
        p->tgid = t->tgid;
        p->pgid = t->pgid;
        p->sid = t->sid;
        p->state = (int)t->state;
        p->nice = 0;
        p->nr_threads = t->group_leader ? t->group_leader->nr_threads : 1;
        p->on_cpu = t->on_cpu;
        p->exit_code = t->exit_code;
        p->tty_nr = t->ctty ? t->ctty->id : 0;  // Use actual tty id
        p->is_kernel = (t->privilege == TASK_KERNEL) ? 1 : 0;
        p->start_tick = t->start_tick;
        p->utime_ticks = t->utime_ticks;
        p->stime_ticks = t->stime_ticks;
        
        // UID/GID from signal state (where the kernel stores it)
        p->uid = 0;
        p->gid = 0;
        p->euid = 0;
        p->egid = 0;
        
        // VSZ: count pages mapped in user space (rough estimate)
        p->vsz = 0;
        p->rss = 0;
        if (t->privilege == TASK_USER) {
            // Estimate from brk and mmap
            if (t->brk > t->brk_start)
                p->vsz += (t->brk - t->brk_start);
            // User stack (assume 2MB)
            p->vsz += 2 * 1024 * 1024;
            // mmap regions
            for (int i = 0; i < TASK_MAX_MMAP; i++) {
                if (t->mmap_regions[i].in_use)
                    p->vsz += t->mmap_regions[i].length;
            }
            // RSS: rough estimate (VSZ/4096 as pages, assume all resident)
            p->rss = p->vsz / 4096;
        }
        
        // Copy comm
        for (int i = 0; i < 255 && t->comm[i]; i++)
            p->comm[i] = t->comm[i];
        p->comm[255] = '\0';
        
        // Copy cmdline
        for (int i = 0; i < 1023 && t->cmdline[i]; i++)
            p->cmdline[i] = t->cmdline[i];
        p->cmdline[1023] = '\0';
        
        // Copy environ
        for (int i = 0; i < 2047 && t->environ[i]; i++)
            p->environ[i] = t->environ[i];
        p->environ[2047] = '\0';
        
        // Copy cwd
        for (int i = 0; i < 255 && t->cwd[i]; i++)
            p->cwd[i] = t->cwd[i];
        p->cwd[255] = '\0';
        
        spin_unlock_irqrestore(&g_task_list_lock, flags);
        count++;
    }
    
    // Copy to user space
    int err = copy_to_user((void*)buf_ptr, kbuf, count * sizeof(procinfo_t));
    kfree(kbuf);
    
    if (err) return err;
    return count;
}

// Main syscall dispatcher (inner function)
static int64_t syscall_handler_inner(uint64_t num, uint64_t a1, uint64_t a2, 
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

        case SYS_STAT:
            return sys_stat(a1, a2);

        case SYS_LSTAT:
            return sys_lstat(a1, a2);

        case SYS_FSTAT:
            return sys_fstat(a1, a2);

        case SYS_ACCESS:
            return sys_access(a1, a2);

        case SYS_CHDIR:
            return sys_chdir(a1);

        case SYS_GETCWD:
            return sys_getcwd(a1, a2);

        case SYS_UMASK:
            return sys_umask(a1);

        case SYS_GETUID:
            return sys_getuid();

        case SYS_GETGID:
            return sys_getgid();

        case SYS_GETEUID:
            return sys_geteuid();

        case SYS_GETEGID:
            return sys_getegid();

        case SYS_SETUID:
            return sys_setuid(a1);

        case SYS_SETGID:
            return sys_setgid(a1);

        case SYS_SETEUID:
            return sys_seteuid(a1);

        case SYS_SETEGID:
            return sys_setegid(a1);

        case SYS_GETGROUPS:
            return sys_getgroups(a1, a2);

        case SYS_SETGROUPS:
            return sys_setgroups(a1, a2);

        case SYS_GETHOSTNAME:
            return sys_gethostname(a1, a2);

        case SYS_UNAME:
            return sys_uname(a1);

        case SYS_TIME:
            return sys_time(a1);

        case SYS_GETTIMEOFDAY:
            return sys_gettimeofday(a1, a2);

        case SYS_FSYNC:
            return sys_fsync(a1);

        case SYS_FTRUNCATE:
            return sys_ftruncate(a1, a2);

        case SYS_FCNTL:
            return sys_fcntl(a1, a2, a3);

        case SYS_IOCTL:
            return sys_ioctl(a1, a2, a3);

        case SYS_SETPGID:
            return sys_setpgid(a1, a2);

        case SYS_GETPGRP:
            return sys_getpgrp();

        case SYS_TCGETPGRP:
            return sys_tcgetpgrp(a1);

        case SYS_TCSETPGRP:
            return sys_tcsetpgrp(a1, a2);

        case SYS_KILL:
            return sys_kill(a1, a2);

        case SYS_UNLINK:
            return sys_unlink(a1);

        case SYS_RENAME:
            return sys_rename(a1, a2);

        case SYS_MKDIR:
            return sys_mkdir(a1, a2);

        case SYS_RMDIR:
            return sys_rmdir(a1);

        case SYS_LINK:
            return sys_link(a1, a2);

        case SYS_SYMLINK:
            return sys_symlink(a1, a2);

        case SYS_READLINK:
            return sys_readlink(a1, a2, a3);

        case SYS_CHMOD:
            return sys_chmod(a1, a2);

        case SYS_FCHMOD:
            return sys_fchmod(a1, a2);

        case SYS_CHOWN:
            return sys_chown(a1, a2, a3);
        case SYS_OPENAT:
            return sys_openat(a1, a2, a3, a4);
        case SYS_FSTATAT:
            return sys_fstatat(a1, a2, a3, a4);
        case SYS_FACCESSAT:
            return sys_faccessat(a1, a2, a3, a4);
        case SYS_GETDENTS64:
            return sys_getdents64(a1, a2, a3);
        case SYS_GETDENTS:
            return sys_getdents(a1, a2, a3);

        case SYS_FCHOWN:
            return sys_fchown(a1, a2, a3);

        case SYS_UTIMENSAT:
            return sys_utimensat(a1, a2, a3, a4);

        case SYS_STATFS:
            return sys_statfs(a1, a2);
        case SYS_FSTATFS:
            return sys_fstatfs(a1, a2);

        // Signal syscalls
        case SYS_RT_SIGACTION:
            return sys_rt_sigaction(a1, a2, a3, a4);
        case SYS_RT_SIGPROCMASK:
            return sys_rt_sigprocmask(a1, a2, a3, a4);
        case SYS_RT_SIGPENDING:
            return sys_rt_sigpending(a1, a2);
        case SYS_RT_SIGTIMEDWAIT:
            return sys_rt_sigtimedwait(a1, a2, a3, a4);
        case SYS_RT_SIGQUEUEINFO:
            return sys_rt_sigqueueinfo(a1, a2, a3);
        case SYS_RT_SIGSUSPEND:
            return sys_rt_sigsuspend(a1, a2);
        case SYS_RT_SIGRETURN:
            return sys_rt_sigreturn();
        case SYS_SIGALTSTACK:
            return sys_sigaltstack(a1, a2);
        case SYS_TKILL:
            return sys_tkill(a1, a2);
        case SYS_TGKILL:
            return sys_tgkill(a1, a2, a3);
        case SYS_ALARM:
            return sys_alarm(a1);
        case SYS_SETITIMER:
            return sys_setitimer(a1, a2, a3);
        case SYS_GETITIMER:
            return sys_getitimer(a1, a2);
        case SYS_TIMER_CREATE:
            return sys_timer_create(a1, a2, a3);
        case SYS_TIMER_SETTIME:
            return sys_timer_settime(a1, a2, a3, a4);
        case SYS_TIMER_GETTIME:
            return sys_timer_gettime(a1, a2);
        case SYS_TIMER_GETOVERRUN:
            return sys_timer_getoverrun(a1);
        case SYS_TIMER_DELETE:
            return sys_timer_delete(a1);
        case SYS_SIGNALFD:
            return sys_signalfd(a1, a2, a3);
        case SYS_PAUSE:
            return sys_pause();
        case SYS_NANOSLEEP:
            return sys_nanosleep(a1, a2);
        case SYS_CLOCK_GETTIME:
            return sys_clock_gettime(a1, a2);
        case SYS_CLOCK_GETRES:
            return sys_clock_getres(a1, a2);
            
        // SMP/Threading syscalls
        case SYS_CLONE:
            return sys_clone(a1, a2, a3, a4, a5);
        case SYS_VFORK:
            return sys_vfork();
        case SYS_EXIT_GROUP:
            sys_exit_group(a1);
            return 0;  // Never reached
        case SYS_GETTID:
            return sys_gettid();
        case SYS_SET_TID_ADDRESS:
            return sys_set_tid_address(a1);
        case SYS_FUTEX:
            return sys_futex(a1, a2, a3, a4, a5, 0);
        case SYS_SET_ROBUST_LIST:
            return sys_set_robust_list(a1, a2);
        case SYS_GET_ROBUST_LIST:
            return sys_get_robust_list(a1, a2, a3);
        case SYS_ARCH_PRCTL:
            return sys_arch_prctl(a1, a2);
        case SYS_SCHED_SETAFFINITY:
            return sys_sched_setaffinity(a1, a2, a3);
        case SYS_SCHED_GETAFFINITY:
            return sys_sched_getaffinity(a1, a2, a3);
        case SYS_SCHED_SETSCHEDULER:
            return sys_sched_setscheduler(a1, a2, a3);
        case SYS_SCHED_GETSCHEDULER:
            return sys_sched_getscheduler(a1);
        case SYS_SCHED_SETPARAM:
            return sys_sched_setparam(a1, a2);
        case SYS_SCHED_GETPARAM:
            return sys_sched_getparam(a1, a2);
        case SYS_SCHED_GET_PRIORITY_MAX:
            return sys_sched_get_priority_max(a1);
        case SYS_SCHED_GET_PRIORITY_MIN:
            return sys_sched_get_priority_min(a1);
        case SYS_SCHED_RR_GET_INTERVAL:
            return sys_sched_rr_get_interval(a1, a2);
        case SYS_MPROTECT:
            return sys_mprotect(a1, a2, a3);
            
        case SYS_REBOOT:
            return sys_reboot(a1, a2, a3, a4);
            
        case SYS_GETPROCINFO:
            return sys_getprocinfo(a1, a2);
            
        case SYS_MEMSTATS:
            mm_print_memory_stats();
            return 0;
            
        default:
            return -ENOSYS;
    }
}

// Wrapper that handles signal delivery after syscall
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, 
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    // CRITICAL: Interrupts are DISABLED when we enter (syscall_entry no longer does sti)
    // This prevents a race where:
    // 1. Task A enters syscall, writes to per-CPU storage
    // 2. Timer fires, preempts to task B  
    // 3. Task B makes syscall, overwrites per-CPU storage
    // 4. Task A resumes, reads corrupted values from per-CPU
    //
    // We snapshot the per-CPU values to task-local storage before enabling interrupts.
    task_t* cur = sched_current();
    percpu_t* cpu = this_cpu();
    
    if (cur && cur->privilege == TASK_USER) {
        // Read from per-CPU storage (set by syscall_entry in assembly)
        cur->syscall_rsp = cpu->syscall_user_rsp;
        cur->syscall_rip = cpu->syscall_saved_user_rip;
        cur->syscall_rflags = cpu->syscall_saved_user_rflags;
        cur->syscall_rbp = cpu->syscall_saved_user_rbp;
        cur->syscall_rbx = cpu->syscall_saved_user_rbx;
        cur->syscall_r12 = cpu->syscall_saved_user_r12;
        cur->syscall_r13 = cpu->syscall_saved_user_r13;
        cur->syscall_r14 = cpu->syscall_saved_user_r14;
        cur->syscall_r15 = cpu->syscall_saved_user_r15;
    }

    // NOW enable interrupts - per-CPU values are safely copied to task struct
    __asm__ volatile("sti" ::: "memory");
    
    int64_t ret = syscall_handler_inner(num, a1, a2, a3, a4, a5);
    
    // Check for pending signals before returning to userspace
    // Skip this for exit (task may be gone) and sigreturn (just restored context)
    if (num != SYS_EXIT && num != SYS_RT_SIGRETURN) {
        cur = sched_current();  // Re-fetch in case of fork
        if (cur && cur->privilege == TASK_USER && signal_pending(cur)) {
            // Save syscall return value so sigreturn can restore it
            cur->syscall_rax = (uint64_t)ret;
            signal_deliver(cur);
            // Check if signal_deliver terminated the task (e.g., SIG_DFL for SIGTERM)
            if (cur->has_exited || cur->state == TASK_ZOMBIE) {
                sched_schedule();
                // Should not return here
            }
        }
    }
    
    return ret;
}
