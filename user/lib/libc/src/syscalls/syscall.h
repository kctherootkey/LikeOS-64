/* Private: the syscall instruction wrappers.  The numbers live in the
   public <sys/syscall.h>. */
#include <sys/syscall.h>
// Syscall numbers (must match kernel)

// Process syscalls

// Extended syscalls

// Signal syscalls

// Debug/diagnostic syscalls (LikeOS specific)

// SMP/Threading syscalls (using 310+ to avoid conflicts)

// Scheduling syscalls (using 320+ to avoid conflicts)

// Memory protection

// System management

// Process information (LikeOS specific)

// Filesystem extended syscalls

// System information and kernel log

// Socket syscalls


// System V shared memory
#define XATTR_SYS_NOFOLLOW 0x40000000  /* OR'd into setxattr flags for lsetxattr */

// NET_GETINFO sub-commands
#define NET_GET_ARP_TABLE       1
#define NET_GET_ROUTE_TABLE     2
#define NET_GET_TCP_CONNECTIONS 3
#define NET_GET_UDP_SOCKETS     4
#define NET_GET_IFACE_STATS     5

// DHCP_CONTROL sub-commands
#define DHCP_CMD_DISCOVER   1
#define DHCP_CMD_RELEASE    2
#define DHCP_CMD_RENEW      3
#define DHCP_CMD_STATUS     4

// Kernel log control operations
#define SYSLOG_ACTION_READ       2
#define SYSLOG_ACTION_READ_ALL   3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR      5
#define SYSLOG_ACTION_SIZE_BUFFER 10

// Syscall wrapper - uses inline assembly to invoke syscall instruction
/* Every wrapper below passes ALL SIX argument registers, zeroing the ones the
 * caller did not supply.
 *
 * This is not cosmetic.  The kernel dispatcher reads a1..a6 unconditionally
 * and hands them to the handler, so a handler may use an argument that a
 * short wrapper never set — the register then still holds whatever the
 * compiler last left in it.  When such a stray value happens to be a valid
 * user pointer AND the handler writes through it, the kernel scribbles on
 * random process memory: waitpid() used syscall3 while SYS_WAIT4 treats the
 * 4th argument as a `struct rusage *` and writes 56 bytes through it, so
 * every reaped child smashed 56 bytes of the caller's heap (found via bash,
 * whose call site leaves a heap pointer in r10).
 *
 * Zeroing costs one instruction per unused register and makes the ABI
 * deterministic: an argument the caller did not pass reads as 0/NULL, which
 * is exactly what handlers treat as "absent". */
static inline long syscall0(long number) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(0L), "S"(0L), "d"(0L), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall1(long number, long arg1) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(0L), "d"(0L), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall2(long number, long arg1, long arg2) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(0L), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall3(long number, long arg1, long arg2, long arg3) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall4(long number, long arg1, long arg2, long arg3, long arg4) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall5(long number, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall6(long number, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
