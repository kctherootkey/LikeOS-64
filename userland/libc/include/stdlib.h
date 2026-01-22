#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

// Memory allocation
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

// String conversion
int atoi(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);

// Process control
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

// Environment
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);

// Path utilities
char* realpath(const char* path, char* resolved_path);

// Utilities
int abs(int n);
long labs(long n);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif
