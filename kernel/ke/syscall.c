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
    if (pipe->used == 0) {
        return (pipe->writers == 0) ? 0 : -EAGAIN;
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

    // SMAP-aware copy from user buffer
    smap_disable();
    mm_memcpy(pipe->buffer + pipe->write_pos, (void*)buf, first);
    if (to_write > first) {
        mm_memcpy(pipe->buffer, (void*)(buf + first), to_write - first);
    }
    smap_enable();

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
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & ISIG)) {
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
        if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid && (tty->term.c_lflag & ISIG)) {
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
        return -EACCES;
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
    const char* host = "LikeOS";
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
    const char* node = "likeos";
    const char* rel = "0.2";
    const char* ver = "LikeOS-64";
    const char* mach = "x86_64";
    mm_memcpy(u.sysname, sys, 7);
    mm_memcpy(u.nodename, node, 7);
    mm_memcpy(u.release, rel, 4);
    mm_memcpy(u.version, ver, 9);
    mm_memcpy(u.machine, mach, 7);
    if (copy_to_user((void*)buf, &u, sizeof(u)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_time(uint64_t tloc) {
    uint64_t ticks = timer_ticks();
    uint64_t sec = ticks / 100;
    if (tloc && validate_user_ptr(tloc, sizeof(uint64_t))) {
        copy_to_user((void*)tloc, &sec, sizeof(sec));
    }
    return (int64_t)sec;
}

static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz) {
    (void)tz;
    if (!validate_user_ptr(tv, sizeof(k_timeval_t))) return -EFAULT;
    uint64_t ticks = timer_ticks();
    k_timeval_t kv;
    kv.tv_sec = (long)(ticks / 100);
    kv.tv_usec = (long)((ticks % 100) * 10000);
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
    if (fd >= TASK_MAX_FDS || cur->fd_table[fd] == NULL) return -EBADF;

    // Handle console markers
    if (fd == STDIN_FD || fd == STDOUT_FD || fd == STDERR_FD) {
        if (cmd == 3) { // F_GETFL
            if (fd == STDIN_FD) return O_RDONLY;
            return O_WRONLY;
        }
        if (cmd == 4) { // F_SETFL
            return 0;
        }
        return -EINVAL;
    }

    vfs_file_t* file = cur->fd_table[fd];
    if (pipe_is_end(file)) {
        pipe_end_t* end = (pipe_end_t*)file;
        if (cmd == 3) {
            return end->is_read ? O_RDONLY : O_WRONLY;
        }
        if (cmd == 4) {
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
    // Use new signal infrastructure - queue the signal for later delivery
    // The signal will be delivered when returning from this syscall
    siginfo_t info;
    mm_memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_USER;
    info.si_pid = sched_current() ? sched_current()->id : 0;
    signal_send(t, sig, &info);
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
        task_t* cur = sched_current();
        if (!cur) return -EFAULT;
        if (sig == 0) {
            return sched_pgid_exists(cur->pgid) ? 0 : -ESRCH;
        }
        sched_signal_pgrp(cur->pgid, (int)sig);
        return 0;
    }
    task_t* t = sched_find_task_by_id((uint32_t)pid);
    if (!t) return -ESRCH;
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
static int64_t sys_chmod(uint64_t pathname, uint64_t mode) { (void)pathname; (void)mode; return -ENOSYS; }
static int64_t sys_fchmod(uint64_t fd, uint64_t mode) { (void)fd; (void)mode; return -ENOSYS; }
static int64_t sys_chown(uint64_t pathname, uint64_t owner, uint64_t group) { (void)pathname; (void)owner; (void)group; return -ENOSYS; }
static int64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group) { (void)fd; (void)owner; (void)group; return -ENOSYS; }

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
    sched_yield();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// External: saved user context from syscall entry (in syscall.asm)
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
    // 1. Saved user callee-saved registers (RBP, RBX, R12-R15)
    // 2. IRET frame: SS, RSP, RFLAGS, CS, RIP (to return to userspace)
    // 3. RAX value (0 for child's fork return value)
    // 4. Saved callee-saved registers for ctx_switch_asm (r15-rbp)
    // 5. Return address (fork_child_return trampoline)
    
    uint64_t* k_sp = (uint64_t*)child->kernel_stack_top;
    k_sp = (uint64_t*)((uint64_t)k_sp & ~0xFUL);  // Align to 16 bytes
    
    // Push user callee-saved registers (fork_child_return will restore these)
    *(--k_sp) = syscall_saved_user_r15;
    *(--k_sp) = syscall_saved_user_r14;
    *(--k_sp) = syscall_saved_user_r13;
    *(--k_sp) = syscall_saved_user_r12;
    *(--k_sp) = syscall_saved_user_rbx;
    *(--k_sp) = syscall_saved_user_rbp;
    
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
        int status = 0;
        // Check if child exited due to signal (via exit_code > 128)
        if (child->exit_code >= 128 && child->exit_code < 256) {
            // Signaled exit: low 7 bits are signal number
            status = (child->exit_code - 128) & 0x7F;
        } else {
            // Normal exit: exit_code << 8
            status = (child->exit_code & 0xFF) << 8;
        }
        copy_to_user((void*)status_ptr, &status, sizeof(status));
    }
    
    int child_pid = child->id;
    
    // Reap the zombie - remove from scheduler and free
    sched_remove_task(child);
    
    return child_pid;
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

    free_user_string_array(kenvp);
    free_user_string_array(kargv);
    kfree(kpath);

    if (entry_point == 0) {
        // exec failed, return error to caller
        return -ENOEXEC;
    }

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

// SYS_GETPID - get process ID
static int64_t sys_getpid(void) {
    task_t* cur = sched_current();
    return cur ? cur->id : -1;
}

// SYS_YIELD - yield CPU
static int64_t sys_yield(void) {
    task_t* cur = sched_current();
    (void)cur;
    __asm__ volatile ("sti");
    sched_yield();
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
        uint64_t ticks = timeout.tv_sec * 100 + timeout.tv_nsec / 10000000;
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
        sched_yield();
        
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
        sched_yield();
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

// SYS_TGKILL - send signal to thread in thread group
static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig) {
    (void)tgid;  // We don't have thread groups, just use tid
    return sys_tkill(tid, sig);
}

// SYS_ALARM - set alarm clock
static int64_t sys_alarm(uint64_t seconds) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    uint64_t old_remaining = 0;
    
    // Calculate remaining time from old alarm
    if (cur->signals.alarm_ticks > 0) {
        uint64_t now = timer_ticks();
        if (cur->signals.alarm_ticks > now) {
            old_remaining = (cur->signals.alarm_ticks - now) / 100;  // ticks to seconds
        }
    }
    
    // Set new alarm
    if (seconds > 0) {
        cur->signals.alarm_ticks = timer_ticks() + seconds * 100;  // 100 Hz
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
        sched_yield();
        if (cur->has_exited) {
            return -EINTR;
        }
    }
    
    return -EINTR;  // pause always returns EINTR
}

// SYS_NANOSLEEP - sleep with nanosecond precision
static int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    
    struct k_timespec req;
    if (copy_from_user(&req, (void*)req_ptr, sizeof(struct k_timespec)) != 0) {
        return -EFAULT;
    }
    
    // Calculate ticks to sleep (100 Hz = 10ms per tick)
    uint64_t ticks = req.tv_sec * 100 + req.tv_nsec / 10000000;
    uint64_t start = timer_ticks();
    uint64_t end = start + ticks;
    
    cur->state = TASK_BLOCKED;
    
    while (timer_ticks() < end) {
        sched_yield();
        
        // Check for signal interruption
        if (signal_pending(cur)) {
            // Calculate remaining time
            if (rem_ptr) {
                uint64_t elapsed = timer_ticks() - start;
                uint64_t remaining = (elapsed < ticks) ? ticks - elapsed : 0;
                struct k_timespec rem;
                rem.tv_sec = remaining / 100;
                rem.tv_nsec = (remaining % 100) * 10000000;
                copy_to_user((void*)rem_ptr, &rem, sizeof(struct k_timespec));
            }
            cur->state = TASK_READY;
            return -EINTR;
        }
    }
    
    cur->state = TASK_READY;
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
            
        default:
            return -ENOSYS;
    }
}

// Wrapper that handles signal delivery after syscall
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, 
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    // Save syscall context to current task (so signals work after yield)
    task_t* cur = sched_current();
    if (cur && cur->privilege == TASK_USER) {
        cur->syscall_rsp = syscall_saved_user_rsp;
        cur->syscall_rip = syscall_saved_user_rip;
        cur->syscall_rflags = syscall_saved_user_rflags;
        cur->syscall_rbp = syscall_saved_user_rbp;
        cur->syscall_rbx = syscall_saved_user_rbx;
        cur->syscall_r12 = syscall_saved_user_r12;
        cur->syscall_r13 = syscall_saved_user_r13;
        cur->syscall_r14 = syscall_saved_user_r14;
        cur->syscall_r15 = syscall_saved_user_r15;
    }
    
    int64_t ret = syscall_handler_inner(num, a1, a2, a3, a4, a5);
    
    // Check for pending signals before returning to userspace
    // Skip this for exit (task may be gone) and sigreturn (just restored context)
    if (num != SYS_EXIT && num != SYS_RT_SIGRETURN) {
        cur = sched_current();  // Re-fetch in case of fork
        if (cur && cur->privilege == TASK_USER && signal_pending(cur)) {
            signal_deliver(cur);
            // Check if signal_deliver terminated the task (e.g., SIG_DFL for SIGTERM)
            if (cur->has_exited || cur->state == TASK_ZOMBIE) {
                sched_yield();
                // Should not return here
            }
        }
    }
    
    return ret;
}
