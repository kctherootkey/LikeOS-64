#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

/* Environment storage limits.
 * MAX_ENV_SIZE is the cap for ONE "NAME" or "value" string.  It was 4096,
 * which made libc's static env_names/env_values arrays (and execv's
 * conversion buffer) total ~2 MB of BSS — eagerly zero-faulted into every
 * process at exec, dominating process-start time.  The kernel caps the whole
 * environment at 16 KB total (copy_user_string_array), so 512 per string is
 * still generous. */
#define MAX_ENV_VARS 128
#define MAX_ENV_SIZE 512

/* Multibyte character maximum - single-byte locale only */
#define MB_CUR_MAX  1
#define MB_LEN_MAX  1

/* Multibyte conversions.  The C standard puts mbtowc/wctomb/mbstowcs/wcstombs
 * here as well as in <wchar.h>; the shared definition lives in one place so
 * the two headers cannot disagree. */
#include <bits/multibyte.h>

/* Multibyte length, single-byte locale semantics */
static inline int mblen(const char *s, size_t n)
{
    if (!s) return 0;         /* no state-dependent encodings */
    if (n == 0) return -1;
    return (*s != '\0') ? 1 : 0;
}

// Memory allocation
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
void* aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);

// String conversion
int atoi(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);
float strtof(const char* nptr, char** endptr);
long double strtold(const char* nptr, char** endptr);
double atof(const char* nptr);

// Process control
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

// Environment
char* getenv(const char* name);
int system(const char *command);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int clearenv(void);
int putenv(char* string);

/* Iterate all environment variables.
 * Call with *cookie = 0 to start; returns name/value pairs.
 * Returns 0 when no more variables. */
int env_iter(int *cookie, const char **name, const char **value);
/* Return count of environment variables */
int env_count(void);

/* Initialize libc env storage from envp[] (called by _start before main) */
void __libc_init_environ(char **envp);

// Path utilities
char* realpath(const char* path, char* resolved_path);

// Utilities
int abs(int n);
long labs(long n);

/* Quotient and remainder together; both truncate toward zero. */
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

long long llabs(long long j);
/* intmax_t comes from <stdint.h>, which is where it is defined; do not
 * redeclare it here. */
#include <stdint.h>
intmax_t imaxabs(intmax_t j);
div_t   div(int num, int den);
ldiv_t  ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);
/* Pseudo-random numbers.  rand() and random() share one generator (the
 * degree-31 additive-feedback one), so seeding either affects both.  Not for
 * anything security-related — use getrandom() for that. */
#define RAND_MAX 2147483647

int   rand(void);
void  srand(unsigned int seed);
int   rand_r(unsigned int* seedp);

/* The SVID 48-bit generator.  A different, independently-seeded sequence from
 * rand()/random() above -- seeding one does not affect the other.
 *
 * The e/n/j variants take the caller's own state and are reentrant; the plain
 * names share one global state and are not. */
double drand48(void);
long   lrand48(void);
long   mrand48(void);
double erand48(unsigned short xsubi[3]);
long   nrand48(unsigned short xsubi[3]);
long   jrand48(unsigned short xsubi[3]);
void   srand48(long seed);
unsigned short* seed48(unsigned short seed16v[3]);
void   lcong48(unsigned short param[7]);
long  random(void);
void  srandom(unsigned int seed);
char* initstate(unsigned int seed, char* state, size_t size);
char* setstate(char* state);

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void qsort_r(void* base, size_t nmemb, size_t size,
             int (*compar)(const void*, const void*, void*), void* arg);
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
int mkstemp(char* templ);
int mkstemps(char* templ, int suffixlen);
int mkostemp(char* templ, int oflags);
int mkostemps(char* templ, int suffixlen, int oflags);
char* mktemp(char* templ);
char* mkdtemp(char* templ);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* atexit / at_quick_exit */
int atexit(void (*func)(void));
int at_quick_exit(void (*func)(void));
void quick_exit(int status);

#endif
