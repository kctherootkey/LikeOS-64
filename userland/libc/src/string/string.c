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
