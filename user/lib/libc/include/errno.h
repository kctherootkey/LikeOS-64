#ifndef _ERRNO_H
#define _ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

// Error codes
#define EPERM           1  /* Operation not permitted */
#define ENOENT          2  /* No such file or directory */
#define ESRCH           3  /* No such process */
#define EINTR           4  /* Interrupted system call */
#define EIO             5  /* I/O error */
#define ENXIO           6  /* No such device or address */
#define E2BIG           7  /* Argument list too long */
#define ENOEXEC         8  /* Exec format error */
#define EBADF           9  /* Bad file number */
#define ECHILD         10  /* No child processes */
#define EAGAIN         11  /* Try again */
#define ENOMEM         12  /* Out of memory */
#define EACCES         13  /* Permission denied */
#define EFAULT         14  /* Bad address */
#define ENOTBLK        15  /* Block device required */
#define EBUSY          16  /* Device or resource busy */
#define EEXIST         17  /* File exists */
#define EXDEV          18  /* Cross-device link */
#define ENODEV         19  /* No such device */
#define ENOTDIR        20  /* Not a directory */
#define EISDIR         21  /* Is a directory */
#define EINVAL         22  /* Invalid argument */
#define ENFILE         23  /* File table overflow */
#define EMFILE         24  /* Too many open files */
#define ENOTTY         25  /* Not a typewriter */
#define ETXTBSY        26  /* Text file busy */
#define EFBIG          27  /* File too large */
#define ENOSPC         28  /* No space left on device */
#define ESPIPE         29  /* Illegal seek */
#define EROFS          30  /* Read-only file system */
#define EMLINK         31  /* Too many links */
#define EPIPE          32  /* Broken pipe */
#define EDOM           33  /* Math argument out of domain of func */
#define ERANGE         34  /* Math result not representable */
#define EDEADLK        35  /* Resource deadlock would occur */
#define ENAMETOOLONG   36  /* File name too long */
#define ENOLCK         37  /* No record locks available */
#define ENOSYS         38  /* Function not implemented */
#define ENOTEMPTY      39  /* Directory not empty */
#define ELOOP          40  /* Too many symbolic links encountered */
#define EWOULDBLOCK    EAGAIN  /* Operation would block */
#define ENOMSG         42  /* No message of desired type */
#define EIDRM          43  /* Identifier removed */
#define ENOSTR         60  /* Device not a stream */
#define ENODATA        61  /* No data available */
#define ETIME          62  /* Timer expired */
#define ENOSR          63  /* Out of streams resources */
#define ENOLINK        67  /* Link has been severed */
#define EPROTO         71  /* Protocol error */
#define EMULTIHOP      72  /* Multihop attempted */
#define EBADMSG        74  /* Not a data message */
#define EOVERFLOW      75  /* Value too large for defined data type */
#define EILSEQ         84  /* Illegal byte sequence */
/* Never returned by this kernel.
 *
 * ERESTART is an internal marker meaning "restart this system call", used
 * between a signal handler and the syscall return path; it is not supposed to
 * reach userspace on any system.  It is defined here because portable code
 * tests for it anyway, defensively, alongside EINTR -- X.Org's fbdevhw retries
 * an interrupted FBIOBLANK on `case EINTR: case ERESTART:` and does not build
 * without the name.  The arm is simply never taken here. */
#define ERESTART       85  /* Interrupted system call should be restarted */
#define ENOTSOCK       88  /* Socket operation on non-socket */
#define EDESTADDRREQ   89  /* Destination address required */
#define EMSGSIZE       90  /* Message too long */
#define EPROTOTYPE     91  /* Protocol wrong type for socket */
#define ENOPROTOOPT    92  /* Protocol not available */
#define EPROTONOSUPPORT 93 /* Protocol not supported */
#define ESOCKTNOSUPPORT 94 /* Socket type not supported */
#define EOPNOTSUPP     95  /* Operation not supported on transport endpoint */
/* POSIX names this error ENOTSUP and the sockets interfaces name it
 * EOPNOTSUPP.  They are permitted to be the same value and on every system
 * that matters they are, so code that tests for one catches the other.
 * Portable software uses ENOTSUP freely; without it the build fails on a name
 * that was simply never spelled out here. */
#define ENOTSUP        EOPNOTSUPP
#define EPFNOSUPPORT   96  /* Protocol family not supported */
#define EAFNOSUPPORT   97  /* Address family not supported */
#define EADDRINUSE     98  /* Address already in use */
#define EADDRNOTAVAIL  99  /* Cannot assign requested address */
#define ENETDOWN       100 /* Network is down */
#define ENETUNREACH    101 /* Network is unreachable */
#define ENETRESET      102 /* Network dropped connection on reset */
#define ECONNABORTED   103 /* Software caused connection abort */
#define ECONNRESET     104 /* Connection reset by peer */
#define ENOBUFS        105 /* No buffer space available */
#define EISCONN        106 /* Transport endpoint is already connected */
#define ENOTCONN       107 /* Transport endpoint is not connected */
#define ETIMEDOUT      110 /* Connection timed out */
#define ECONNREFUSED   111 /* Connection refused */
#define EHOSTUNREACH   113 /* No route to host */
#define EALREADY       114 /* Operation already in progress */
#define EINPROGRESS    115 /* Operation now in progress */
#define ESTALE         116 /* Stale file handle */
#define ESHUTDOWN      108 /* Cannot send after transport endpoint shutdown */
#define ETOOMANYREFS   109 /* Too many references: cannot splice */
#define EHOSTDOWN      112 /* Host is down */
#define EUSERS         87  /* Too many users */
#define EDQUOT         122 /* Disk quota exceeded */
#define EREMOTE        66  /* Object is remote */
#define ECANCELED      125 /* Operation canceled */
#define EOWNERDEAD     130 /* Owner died */
#define ENOTRECOVERABLE 131 /* State not recoverable */

#ifdef __cplusplus
}
#endif

#endif
