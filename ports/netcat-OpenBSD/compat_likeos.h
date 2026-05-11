/*
 * compat_likeos.h - LikeOS portability shims for the netcat port.
 *
 * Provides inline implementations and stubs for functions/constants that
 * are present on BSD but absent from the LikeOS userland C library.
 * All additions are guarded by __LIKEOS__ so the file is a no-op on a
 * standard system.
 */
#ifndef _COMPAT_LIKEOS_H
#define _COMPAT_LIKEOS_H

#ifdef __LIKEOS__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

/* ------------------------------------------------------------------
 * OpenBSD security syscalls — no-ops on LikeOS.
 * ------------------------------------------------------------------ */
static inline int unveil(const char *path, const char *permissions)
{
    (void)path; (void)permissions;
    return 0;
}

static inline int pledge(const char *promises, const char *execpromises)
{
    (void)promises; (void)execpromises;
    return 0;
}

/* ------------------------------------------------------------------
 * Routing-table selection — not supported.
 * ------------------------------------------------------------------ */
#define RT_TABLEID_MAX 255
static inline int setrtable(int rtableid)
{
    (void)rtableid;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------
 * strtonum — parse a long long in [minval, maxval]; on error set *errstr.
 * ------------------------------------------------------------------ */
static inline long long
strtonum(const char *numstr, long long minval, long long maxval,
         const char **errstrp)
{
    long long ll = 0;
    char *ep;
    int e;
    static const char *inv = "invalid";
    static const char *toosmall = "too small";
    static const char *toolarge = "too large";

    if (errstrp != NULL)
        *errstrp = NULL;
    if (minval > maxval) {
        if (errstrp) *errstrp = inv;
        return 0;
    }
    errno = 0;
    ll = strtoll(numstr, &ep, 10);
    e = errno;
    if (*numstr == '\0' || *ep != '\0') {
        if (errstrp) *errstrp = inv;
        return 0;
    }
    if (e == ERANGE || ll < minval) {
        if (errstrp) *errstrp = toosmall;
        return minval;
    }
    if (ll > maxval) {
        if (errstrp) *errstrp = toolarge;
        return maxval;
    }
    return ll;
}

/* ------------------------------------------------------------------
 * arc4random_uniform — uniform random in [0, upper_bound).
 * Uses the system getrandom-backed PRNG via our libc rand().
 * ------------------------------------------------------------------ */

/* Simple internal PRNG state (xorshift64) seeded from /dev/urandom or time. */
static unsigned long long _nc_prng_state = 0;

static inline unsigned long long _nc_rand64(void)
{
    if (_nc_prng_state == 0) {
        /* Seed from /dev/urandom if available, fall back to time. */
        int fd = open("/dev/urandom", 0 /* O_RDONLY */);
        if (fd >= 0) {
            read(fd, &_nc_prng_state, sizeof(_nc_prng_state));
            close(fd);
        }
        if (_nc_prng_state == 0) {
            /* Fallback: use address + PID as entropy source */
            struct timespec ts;
            (void)ts; /* may not have clock_gettime */
            _nc_prng_state = (unsigned long long)(size_t)&_nc_prng_state
                             ^ (unsigned long long)getpid() ^ 0xdeadbeefcafe0001ULL;
        }
    }
    /* xorshift64 */
    unsigned long long x = _nc_prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    _nc_prng_state = x;
    return x;
}

static inline unsigned int arc4random_uniform(unsigned int upper_bound)
{
    if (upper_bound < 2)
        return 0;
    /* Rejection sampling for uniform distribution. */
    unsigned int min = (unsigned int)(-(int)upper_bound) % upper_bound;
    unsigned int r;
    do {
        r = (unsigned int)_nc_rand64();
    } while (r < min);
    return r % upper_bound;
}

/* ------------------------------------------------------------------
 * explicit_bzero — zero memory in a way the compiler will not optimise away.
 * ------------------------------------------------------------------ */
static inline void explicit_bzero(void *s, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (len--)
        *p++ = 0;
}

/* ------------------------------------------------------------------
 * asprintf / vasprintf
 * ------------------------------------------------------------------ */
#ifndef HAVE_ASPRINTF
static inline int
vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { *strp = NULL; return -1; }
    *strp = (char *)malloc((size_t)n + 1);
    if (!*strp) return -1;
    vsnprintf(*strp, (size_t)n + 1, fmt, ap);
    return n;
}

static inline int
asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(strp, fmt, ap);
    va_end(ap);
    return n;
}
#endif /* HAVE_ASPRINTF */

/* ------------------------------------------------------------------
 * mktemp — create a unique filename from a template ending in XXXXXX.
 * Uses mkstemp internally and closes the fd.
 * ------------------------------------------------------------------ */
static inline char *mktemp(char *tmpl)
{
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return NULL;
    close(fd);
    /* Unlink so the caller can create a socket at this path. */
    unlink(tmpl);
    return tmpl;
}

/* ------------------------------------------------------------------
 * readpassphrase — read a passphrase from the terminal.
 * Simplified: prints prompt to stderr, reads from /dev/tty (or stdin).
 * ------------------------------------------------------------------ */
#define RPP_REQUIRE_TTY 0x02

static inline char *
readpassphrase(const char *prompt, char *buf, size_t bufsiz, int flags)
{
    (void)flags;
    int ttyfd = open("/dev/tty", 2 /* O_RDWR */);
    FILE *tty = (ttyfd >= 0) ? fdopen(ttyfd, "r+") : NULL;
    FILE *in  = tty ? tty : stdin;
    FILE *out = tty ? tty : stderr;

    if (!buf || bufsiz == 0)
        return NULL;

    fputs(prompt, out);
    fflush(out);

    /* Disable echo — best-effort, ignore failure */
    char *p = buf;
    size_t i = 0;
    int c;
    while (i < bufsiz - 1 && (c = fgetc(in)) != EOF && c != '\n') {
        p[i++] = (char)c;
    }
    p[i] = '\0';
    fputc('\n', out);

    if (tty) fclose(tty);
    return buf;
}

/* ------------------------------------------------------------------
 * b64_ntop — base64 encode src[0..srclength) into target[].
 * Returns number of bytes written or -1 on error.
 * ------------------------------------------------------------------ */
static inline int
b64_ntop(unsigned char const *src, size_t srclength,
         char *target, size_t targsize)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, j = 0;
    unsigned int v;

    for (i = 0; i < srclength; i += 3) {
        if (j + 4 >= targsize)
            return -1;
        v  = (unsigned int)src[i] << 16;
        v |= (i + 1 < srclength) ? (unsigned int)src[i+1] << 8 : 0;
        v |= (i + 2 < srclength) ? (unsigned int)src[i+2]      : 0;
        target[j++] = b64[(v >> 18) & 0x3f];
        target[j++] = b64[(v >> 12) & 0x3f];
        target[j++] = (i + 1 < srclength) ? b64[(v >> 6) & 0x3f] : '=';
        target[j++] = (i + 2 < srclength) ? b64[v & 0x3f]        : '=';
    }
    if (j >= targsize)
        return -1;
    target[j] = '\0';
    return (int)j;
}

/* ------------------------------------------------------------------
 * Telnet constants (normally from <arpa/telnet.h>).
 * ------------------------------------------------------------------ */
#ifndef IAC
#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#endif

/* ------------------------------------------------------------------
 * IP TOS / DSCP constants (normally from <netinet/ip.h>).
 * Values per RFC 2474 / RFC 791.
 * ------------------------------------------------------------------ */
#ifndef IPTOS_LOWDELAY
/* Legacy TOS bits */
#define IPTOS_LOWDELAY     0x10
#define IPTOS_THROUGHPUT   0x08
#define IPTOS_RELIABILITY  0x04

/* Precedence field */
#define IPTOS_PREC_NETCONTROL      0xe0
#define IPTOS_PREC_INTERNETCONTROL 0xc0
#define IPTOS_PREC_CRITIC_ECP      0xa0

/* DiffServ codepoints */
#define IPTOS_DSCP_CS0  0x00
#define IPTOS_DSCP_CS1  0x20
#define IPTOS_DSCP_CS2  0x40
#define IPTOS_DSCP_CS3  0x60
#define IPTOS_DSCP_CS4  0x80
#define IPTOS_DSCP_CS5  0xa0
#define IPTOS_DSCP_CS6  0xc0
#define IPTOS_DSCP_CS7  0xe0
#define IPTOS_DSCP_AF11 0x28
#define IPTOS_DSCP_AF12 0x30
#define IPTOS_DSCP_AF13 0x38
#define IPTOS_DSCP_AF21 0x48
#define IPTOS_DSCP_AF22 0x50
#define IPTOS_DSCP_AF23 0x58
#define IPTOS_DSCP_AF31 0x68
#define IPTOS_DSCP_AF32 0x70
#define IPTOS_DSCP_AF33 0x78
#define IPTOS_DSCP_AF41 0x88
#define IPTOS_DSCP_AF42 0x90
#define IPTOS_DSCP_AF43 0x98
#define IPTOS_DSCP_EF   0xb8
#define IPTOS_DSCP_VA   0xb4
#endif /* IPTOS_LOWDELAY */

/* ------------------------------------------------------------------
 * Socket / IP options not present in the LikeOS headers.
 * setsockopt() will simply ignore unknown options gracefully.
 * ------------------------------------------------------------------ */
#ifndef SO_DEBUG
#define SO_DEBUG        1
#endif

#ifndef SO_BINDANY
#define SO_BINDANY      44  /* OpenBSD extension — harmless stub */
#endif

#ifndef TCP_MD5SIG
#define TCP_MD5SIG      14
#endif

#ifndef IP_MINTTL
#define IP_MINTTL       21
#endif

#ifndef IPV6_TCLASS
#define IPV6_TCLASS     67
#endif

#ifndef IPV6_MINHOPCOUNT
#define IPV6_MINHOPCOUNT 73
#endif

/* ------------------------------------------------------------------
 * ENOBUFS may not be defined on every platform.
 * ------------------------------------------------------------------ */
#ifndef ENOBUFS
#define ENOBUFS ENOMEM
#endif

/* AF_INET6 is not in the LikeOS sys/socket.h yet; netinet/in.h uses it
 * only in comments and struct field descriptions.  Define it here so that
 * nc can compile and gracefully fail with EAFNOSUPPORT at runtime. */
#ifndef AF_INET6
#define AF_INET6 10
#define PF_INET6 AF_INET6
#endif

#endif /* __LIKEOS__ */
#endif /* _COMPAT_LIKEOS_H */
