#include "../../include/stdlib.h"
#include "../../include/unistd.h"
#include "../../include/ctype.h"

void exit(int status) {
    _exit(status);
}

void abort(void) {
    _exit(1);
}

int atoi(const char* nptr) {
    return (int)atol(nptr);
}

long atol(const char* nptr) {
    return strtol(nptr, NULL, 10);
}

long long atoll(const char* nptr) {
    return strtoll(nptr, NULL, 10);
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

long strtol(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    long acc = 0;
    int neg = 0;
    
    // Skip whitespace
    while (is_space(*s)) {
        s++;
    }
    
    // Handle sign
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    // Auto-detect base
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    
    // Convert digits
    while (*s) {
        int digit;
        if (is_digit(*s)) {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'z') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'Z') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        
        if (digit >= base) {
            break;
        }
        
        acc = acc * base + digit;
        s++;
    }
    
    if (endptr) {
        *endptr = (char*)s;
    }
    
    return neg ? -acc : acc;
}

unsigned long strtoul(const char* nptr, char** endptr, int base) {
    // For simplicity, use strtol and cast
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char* nptr, char** endptr, int base) {
    // Similar to strtol but with long long
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    return (unsigned long long)strtoll(nptr, endptr, base);
}

char* getenv(const char* name) {
    // TODO: Implement environment variable support
    (void)name;
    return NULL;
}

int abs(int n) {
    return n < 0 ? -n : n;
}

long labs(long n) {
    return n < 0 ? -n : n;
}

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    // Simple bubble sort for now
    char* b = base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - i - 1; j++) {
            void* p1 = b + j * size;
            void* p2 = b + (j + 1) * size;
            if (compar(p1, p2) > 0) {
                // Swap
                for (size_t k = 0; k < size; k++) {
                    char tmp = ((char*)p1)[k];
                    ((char*)p1)[k] = ((char*)p2)[k];
                    ((char*)p2)[k] = tmp;
                }
            }
        }
    }
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    const char* b = base;
    size_t left = 0;
    size_t right = nmemb;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const void* p = b + mid * size;
        int cmp = compar(key, p);
        
        if (cmp == 0) {
            return (void*)p;
        } else if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    
    return NULL;
}
