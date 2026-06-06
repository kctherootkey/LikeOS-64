/* strnlen.c - extra string functions for LikeOS libc */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

/* strcasestr - case-insensitive substring search */
char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen) return (char *)haystack;
    }
    return NULL;
}

/* strspn */
size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    for (; *s; s++) {
        const char *a;
        for (a = accept; *a; a++) {
            if (*s == *a) break;
        }
        if (!*a) break;
        count++;
    }
    return count;
}

/* strcspn */
size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    for (; *s; s++) {
        const char *r;
        for (r = reject; *r; r++) {
            if (*s == *r) return count;
        }
        count++;
    }
    return count;
}

/* strpbrk */
char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
    }
    return NULL;
}

/* memrchr - search backwards for byte */
void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) {
        --p;
        if (*p == (unsigned char)c) return (void *)p;
    }
    return NULL;
}

/* stpcpy */
char *stpcpy(char *dest, const char *src)
{
    while ((*dest = *src)) { dest++; src++; }
    return dest;
}

/* stpncpy */
char *stpncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    char *ret = &dest[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return ret;
}

/* strcasecmp / strncasecmp - case-insensitive compare (non-inline symbols) */
int strcasecmp(const char *s1, const char *s2)
{
    while (*s1 && *s2) {
        int d = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (d) return d;
        s1++; s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    while (n-- && *s1 && *s2) {
        int d = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (d) return d;
        s1++; s2++;
    }
    if (n == (size_t)-1) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}
