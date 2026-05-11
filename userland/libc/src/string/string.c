#include "../../include/string.h"
#include "../../include/stdlib.h"

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = s;
    unsigned char ch = (unsigned char)c;
    
    while (n--) {
        if (*p == ch) {
            return (void*)p;
        }
        p++;
    }
    return NULL;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n && (*d++ = *src++)) {
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) {
        d++;
    }
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (*d) {
        d++;
    }
    while (n && (*d++ = *src++)) {
        n--;
    }
    *d = '\0';
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    if (c == '\0') {
        return (char*)s;
    }
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) {
        return (char*)haystack;
    }
    
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        
        if (!*n) {
            return (char*)haystack;
        }
        
        haystack++;
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* new = malloc(len);
    if (new) {
        memcpy(new, s, len);
    }
    return new;
}

char* strtok(char* str, const char* delim) {
    static char* saved = NULL;
    
    if (str) {
        saved = str;
    }
    
    if (!saved) {
        return NULL;
    }
    
    // Skip leading delimiters
    while (*saved && strchr(delim, *saved)) {
        saved++;
    }
    
    if (!*saved) {
        return NULL;
    }
    
    char* token = saved;
    
    // Find end of token
    while (*saved && !strchr(delim, *saved)) {
        saved++;
    }
    
    if (*saved) {
        *saved++ = '\0';
    } else {
        saved = NULL;
    }
    
    return token;
}

char* strerror(int errnum) {
    switch (errnum) {
        case 0:   return "Success";
        case 1:   return "Operation not permitted";
        case 2:   return "No such file or directory";
        case 3:   return "No such process";
        case 4:   return "Interrupted system call";
        case 5:   return "Input/output error";
        case 6:   return "No such device or address";
        case 7:   return "Argument list too long";
        case 8:   return "Exec format error";
        case 9:   return "Bad file descriptor";
        case 10:  return "No child processes";
        case 11:  return "Resource temporarily unavailable";
        case 12:  return "Cannot allocate memory";
        case 13:  return "Permission denied";
        case 14:  return "Bad address";
        case 15:  return "Block device required";
        case 16:  return "Device or resource busy";
        case 17:  return "File exists";
        case 18:  return "Invalid cross-device link";
        case 19:  return "No such device";
        case 20:  return "Not a directory";
        case 21:  return "Is a directory";
        case 22:  return "Invalid argument";
        case 23:  return "Too many open files in system";
        case 24:  return "Too many open files";
        case 25:  return "Inappropriate ioctl for device";
        case 26:  return "Text file busy";
        case 27:  return "File too large";
        case 28:  return "No space left on device";
        case 29:  return "Illegal seek";
        case 30:  return "Read-only file system";
        case 31:  return "Too many links";
        case 32:  return "Broken pipe";
        case 33:  return "Numerical argument out of domain";
        case 34:  return "Numerical result out of range";
        case 35:  return "Resource deadlock avoided";
        case 36:  return "File name too long";
        case 37:  return "No locks available";
        case 38:  return "Function not implemented";
        case 39:  return "Directory not empty";
        case 40:  return "Too many levels of symbolic links";
        case 42:  return "No message of desired type";
        case 43:  return "Identifier removed";
        case 60:  return "Device not a stream";
        case 61:  return "No data available";
        case 62:  return "Timer expired";
        case 63:  return "Out of streams resources";
        case 67:  return "Link has been severed";
        case 71:  return "Protocol error";
        case 74:  return "Not a data message";
        case 75:  return "Value too large for defined data type";
        case 84:  return "Illegal byte sequence";
        case 88:  return "Socket operation on non-socket";
        case 89:  return "Destination address required";
        case 90:  return "Message too long";
        case 91:  return "Protocol wrong type for socket";
        case 92:  return "Protocol not available";
        case 93:  return "Protocol not supported";
        case 94:  return "Socket type not supported";
        case 95:  return "Operation not supported";
        case 97:  return "Address family not supported by protocol";
        case 98:  return "Address already in use";
        case 99:  return "Cannot assign requested address";
        case 100: return "Network is down";
        case 101: return "Network is unreachable";
        case 103: return "Software caused connection abort";
        case 104: return "Connection reset by peer";
        case 105: return "No buffer space available";
        case 106: return "Transport endpoint is already connected";
        case 107: return "Transport endpoint is not connected";
        case 110: return "Connection timed out";
        case 111: return "Connection refused";
        case 113: return "No route to host";
        case 114: return "Operation already in progress";
        case 115: return "Operation now in progress";
        case 116: return "Stale file handle";
        case 130: return "Owner died";
        case 131: return "State not recoverable";
        default: {
            static char buf[32];
            char *p = buf;
            int n = errnum;
            *p++ = 'E'; *p++ = 'r'; *p++ = 'r'; *p++ = 'o'; *p++ = 'r'; *p++ = ' ';
            if (n < 0) { *p++ = '-'; n = -n; }
            if (n >= 100) { *p++ = '0' + (n / 100); n %= 100; *p++ = '0' + (n / 10); n %= 10; }
            else if (n >= 10) { *p++ = '0' + (n / 10); n %= 10; }
            *p++ = '0' + n;
            *p = '\0';
            return buf;
        }
    }
}

/* OpenBSD-style bounded copies. Always NUL-terminate when siz > 0
 * and return the length the caller would have produced (i.e. truncation
 * is signalled by a return value >= siz). */
size_t strlcpy(char *dst, const char *src, size_t siz) {
    const char *s = src;
    size_t n = siz;
    if (n != 0) {
        while (--n != 0) {
            if ((*dst++ = *s++) == '\0') break;
        }
    }
    if (n == 0) {
        if (siz != 0) *dst = '\0';
        while (*s++) ;
    }
    return (size_t)(s - src - 1);
}

size_t strlcat(char *dst, const char *src, size_t siz) {
    char *d = dst;
    const char *s = src;
    size_t n = siz;
    size_t dlen;
    while (n-- != 0 && *d != '\0') d++;
    dlen = (size_t)(d - dst);
    n = siz - dlen;
    if (n == 0) {
        size_t slen = 0;
        while (src[slen]) slen++;
        return dlen + slen;
    }
    while (*s != '\0') {
        if (n != 1) { *d++ = *s; n--; }
        s++;
    }
    *d = '\0';
    return dlen + (size_t)(s - src);
}

char *strsep(char **stringp, const char *delim) {
    char *s = *stringp;
    char *tok;
    const char *sp;
    int c, sc;
    if (s == 0) return 0;
    for (tok = s;;) {
        c = *s++;
        sp = delim;
        do {
            if ((sc = *sp++) == c) {
                if (c == 0) s = 0;
                else s[-1] = 0;
                *stringp = s;
                return tok;
            }
        } while (sc != 0);
    }
}

/* strsignal: short text description of signal number. */
char *strsignal(int sig) {
    static char buf[32];
    const char *m;
    switch (sig) {
    case 1:  m = "Hangup"; break;
    case 2:  m = "Interrupt"; break;
    case 3:  m = "Quit"; break;
    case 4:  m = "Illegal instruction"; break;
    case 5:  m = "Trace/breakpoint trap"; break;
    case 6:  m = "Aborted"; break;
    case 7:  m = "Bus error"; break;
    case 8:  m = "Floating point exception"; break;
    case 9:  m = "Killed"; break;
    case 10: m = "User defined signal 1"; break;
    case 11: m = "Segmentation fault"; break;
    case 12: m = "User defined signal 2"; break;
    case 13: m = "Broken pipe"; break;
    case 14: m = "Alarm clock"; break;
    case 15: m = "Terminated"; break;
    case 17: m = "Child exited"; break;
    case 18: m = "Continued"; break;
    case 19: m = "Stopped (signal)"; break;
    case 20: m = "Stopped"; break;
    case 21: m = "Stopped (tty input)"; break;
    case 22: m = "Stopped (tty output)"; break;
    case 23: m = "Urgent I/O condition"; break;
    case 24: m = "CPU time limit exceeded"; break;
    case 25: m = "File size limit exceeded"; break;
    case 26: m = "Virtual timer expired"; break;
    case 27: m = "Profiling timer expired"; break;
    case 28: m = "Window changed"; break;
    case 29: m = "I/O possible"; break;
    case 30: m = "Power failure"; break;
    case 31: m = "Bad system call"; break;
    default: {
        char *p = buf;
        const char *pre = "Unknown signal ";
        while (*pre) *p++ = *pre++;
        if (sig < 0) { *p++ = '-'; sig = -sig; }
        if (sig >= 100) { *p++ = '0' + (sig / 100); sig %= 100; *p++ = '0' + (sig / 10); sig %= 10; }
        else if (sig >= 10) { *p++ = '0' + (sig / 10); sig %= 10; }
        *p++ = '0' + sig;
        *p = '\0';
        return buf;
    }
    }
    return (char *)m;
}
