#ifndef _STRING_H
#define _STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// Memory functions
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
void* memchr(const void* s, int c, size_t n);

// String functions
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);
char* strtok(char* str, const char* delim);
char* strtok_r(char* str, const char* delim, char** saveptr);
char* strerror(int errnum);
int   strerror_r(int errnum, char *buf, size_t buflen);
char* strsignal(int sig);
size_t strnlen(const char* s, size_t maxlen);
char* strcasestr(const char* haystack, const char* needle);
int strcasecmp(const char* s1, const char* s2);
int strncasecmp(const char* s1, const char* s2, size_t n);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* s, const char* accept);
void* memrchr(const void* s, int c, size_t n);
char* stpcpy(char *dest, const char *src);
char* stpncpy(char *dest, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t siz);
size_t strlcat(char *dst, const char *src, size_t siz);
char *strsep(char **stringp, const char *delim);
void* memmem(const void* haystack, size_t haystacklen,
             const void* needle, size_t needlelen);
int strverscmp(const char* s1, const char* s2);
/* Locale-aware comparison; the C locale collates by byte value. */
int strcoll(const char* s1, const char* s2);
size_t strxfrm(char* dest, const char* src, size_t n);

#ifdef __cplusplus
}
#endif

/* The older, BSD-derived string functions: index, rindex, bcopy, strcasecmp
 * and the bit-scan ffs family.
 *
 * They belong in <strings.h> and are declared there.  This include is what
 * every other C library does as well, and it is not a convenience: those names
 * predate <string.h> and a great deal of code -- VTE among the packages here
 * -- calls ffs() or strcasecmp() having included only <string.h>, because that
 * has always worked.  Placed after the extern "C" block above because
 * <strings.h> opens one of its own. */
#include <strings.h>

#endif
