#include "../../include/stdlib.h"
#include "../../include/unistd.h"
#include "../../include/ctype.h"
#include "../../include/string.h"
#include "../../include/errno.h"
#include "../../include/stdio.h"

// Simple environment variable storage

static char env_names[MAX_ENV_VARS][MAX_ENV_SIZE];
static char env_values[MAX_ENV_VARS][MAX_ENV_SIZE];
static int g_env_count = 0;

void exit(int status) {
    fflush(stdout);
    fflush(stderr);
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

/* ── Floating-point string conversion ───────────────────────────────── */

double strtod(const char* nptr, char** endptr) {
    const char* s = nptr;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    /* Sign */
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    /* Handle special values: inf, infinity, nan */
    if ((s[0] == 'i' || s[0] == 'I') &&
        (s[1] == 'n' || s[1] == 'N') &&
        (s[2] == 'f' || s[2] == 'F')) {
        s += 3;
        if ((s[0] == 'i' || s[0] == 'I') &&
            (s[1] == 'n' || s[1] == 'N') &&
            (s[2] == 'i' || s[2] == 'I') &&
            (s[3] == 't' || s[3] == 'T') &&
            (s[4] == 'y' || s[4] == 'Y'))
            s += 5;
        if (endptr) *endptr = (char*)s;
        /* Use integer trick for infinity: 1.0/0.0 is +inf */
        double inf = 1e308 * 10.0;
        return neg ? -inf : inf;
    }
    if ((s[0] == 'n' || s[0] == 'N') &&
        (s[1] == 'a' || s[1] == 'A') &&
        (s[2] == 'n' || s[2] == 'N')) {
        s += 3;
        /* Skip optional (n-char-sequence) */
        if (*s == '(') {
            const char* p = s + 1;
            while (*p && *p != ')') p++;
            if (*p == ')') s = p + 1;
        }
        if (endptr) *endptr = (char*)s;
        double nan_val = 0.0 / 0.0;
        return nan_val;
    }

    /* Handle 0x hex float */
    int hex = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        /* Only if followed by hex digit or '.' */
        const char* t = s + 2;
        if ((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'f') ||
            (*t >= 'A' && *t <= 'F') || *t == '.') {
            hex = 1;
            s += 2;
        }
    }

    double result = 0.0;
    int got_digit = 0;

    if (hex) {
        /* Hex float: digits.digits p exponent */
        while (1) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            result = result * 16.0 + d;
            got_digit = 1;
            s++;
        }
        if (*s == '.') {
            s++;
            double frac = 1.0 / 16.0;
            while (1) {
                int d;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                else break;
                result += d * frac;
                frac /= 16.0;
                got_digit = 1;
                s++;
            }
        }
        /* Binary exponent: p[+-]digits */
        if (got_digit && (*s == 'p' || *s == 'P')) {
            s++;
            int exp_neg = 0;
            if (*s == '-') { exp_neg = 1; s++; }
            else if (*s == '+') { s++; }
            int exp_val = 0;
            while (*s >= '0' && *s <= '9') {
                exp_val = exp_val * 10 + (*s - '0');
                s++;
            }
            double base = 2.0;
            double mult = 1.0;
            while (exp_val > 0) {
                if (exp_val & 1) mult *= base;
                base *= base;
                exp_val >>= 1;
            }
            if (exp_neg) result /= mult;
            else result *= mult;
        }
    } else {
        /* Decimal float: digits.digits e exponent */
        while (*s >= '0' && *s <= '9') {
            result = result * 10.0 + (*s - '0');
            got_digit = 1;
            s++;
        }
        if (*s == '.') {
            s++;
            double frac = 0.1;
            while (*s >= '0' && *s <= '9') {
                result += (*s - '0') * frac;
                frac *= 0.1;
                got_digit = 1;
                s++;
            }
        }
        /* Decimal exponent: e[+-]digits */
        if (got_digit && (*s == 'e' || *s == 'E')) {
            s++;
            int exp_neg = 0;
            if (*s == '-') { exp_neg = 1; s++; }
            else if (*s == '+') { s++; }
            int exp_val = 0;
            int exp_digits = 0;
            while (*s >= '0' && *s <= '9') {
                exp_val = exp_val * 10 + (*s - '0');
                exp_digits = 1;
                s++;
            }
            if (exp_digits) {
                /* Apply power of 10 via repeated squaring */
                double base = 10.0;
                double mult = 1.0;
                int e = exp_val;
                while (e > 0) {
                    if (e & 1) mult *= base;
                    base *= base;
                    e >>= 1;
                }
                if (exp_neg) result /= mult;
                else result *= mult;
            }
        }
    }

    if (!got_digit) {
        /* No valid conversion; set endptr to nptr */
        if (endptr) *endptr = (char*)nptr;
        return 0.0;
    }

    if (endptr) *endptr = (char*)s;
    return neg ? -result : result;
}

float strtof(const char* nptr, char** endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char* nptr, char** endptr) {
    return (long double)strtod(nptr, endptr);
}

double atof(const char* nptr) {
    return strtod(nptr, NULL);
}

char* getenv(const char* name) {
    if (!name) {
        return NULL;
    }
    
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(env_names[i], name) == 0) {
            return env_values[i];
        }
    }
    return NULL;
}

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !value || name[0] == '\0' || strchr(name, '=') != NULL) {
        return -1;
    }
    
    // Check if variable already exists
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(env_names[i], name) == 0) {
            if (overwrite) {
                strncpy(env_values[i], value, MAX_ENV_SIZE - 1);
                env_values[i][MAX_ENV_SIZE - 1] = '\0';
            }
            return 0;
        }
    }
    
    // Add new variable
    if (g_env_count >= MAX_ENV_VARS) {
        return -1;  // No space
    }
    
    strncpy(env_names[g_env_count], name, MAX_ENV_SIZE - 1);
    env_names[g_env_count][MAX_ENV_SIZE - 1] = '\0';
    strncpy(env_values[g_env_count], value, MAX_ENV_SIZE - 1);
    env_values[g_env_count][MAX_ENV_SIZE - 1] = '\0';
    g_env_count++;
    
    return 0;
}

int unsetenv(const char* name) {
    if (!name || name[0] == '\0' || strchr(name, '=') != NULL) {
        return -1;
    }
    
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(env_names[i], name) == 0) {
            // Move last entry to this position
            if (i < g_env_count - 1) {
                strcpy(env_names[i], env_names[g_env_count - 1]);
                strcpy(env_values[i], env_values[g_env_count - 1]);
            }
            g_env_count--;
            return 0;
        }
    }
    return 0;  // Not found is not an error
}

int clearenv(void) {
    g_env_count = 0;
    return 0;
}

int putenv(char* string) {
    if (!string) return -1;
    char* eq = strchr(string, '=');
    if (!eq) {
        /* No '=' means remove the variable */
        return unsetenv(string);
    }
    /* Split at '=' */
    size_t nlen = (size_t)(eq - string);
    if (nlen == 0 || nlen >= MAX_ENV_SIZE) return -1;
    char name[MAX_ENV_SIZE];
    memcpy(name, string, nlen);
    name[nlen] = '\0';
    return setenv(name, eq + 1, 1);
}

int env_iter(int *cookie, const char **name, const char **value) {
    if (!cookie || !name || !value) return 0;
    int idx = *cookie;
    if (idx < 0 || idx >= g_env_count) return 0;
    *name = env_names[idx];
    *value = env_values[idx];
    *cookie = idx + 1;
    return 1;
}

int env_count(void) {
    return g_env_count;
}

/*
 * Called from _start (crt0.S) before main() to populate the libc
 * static environment storage from the envp[] array that the kernel
 * placed on the stack.
 */
void __libc_init_environ(char **envp) {
    g_env_count = 0;
    if (!envp) return;
    for (int i = 0; envp[i] && g_env_count < MAX_ENV_VARS; i++) {
        char *eq = strchr(envp[i], '=');
        if (!eq) continue;
        size_t nlen = (size_t)(eq - envp[i]);
        if (nlen == 0 || nlen >= MAX_ENV_SIZE) continue;
        memcpy(env_names[g_env_count], envp[i], nlen);
        env_names[g_env_count][nlen] = '\0';
        strncpy(env_values[g_env_count], eq + 1, MAX_ENV_SIZE - 1);
        env_values[g_env_count][MAX_ENV_SIZE - 1] = '\0';
        g_env_count++;
    }
}

static int normalize_path(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size < 2) {
        return -1;
    }
    size_t out_len = 0;
    out[out_len++] = '/';

    size_t i = 0;
    while (in[i]) {
        while (in[i] == '/') i++;
        if (!in[i]) break;
        char segment[64];
        size_t seg_len = 0;
        while (in[i] && in[i] != '/' && seg_len < sizeof(segment) - 1) {
            segment[seg_len++] = in[i++];
        }
        segment[seg_len] = '\0';
        if (seg_len == 0 || (seg_len == 1 && segment[0] == '.')) {
            continue;
        }
        if (seg_len == 2 && segment[0] == '.' && segment[1] == '.') {
            if (out_len > 1) {
                out_len--;
                while (out_len > 1 && out[out_len - 1] != '/') {
                    out_len--;
                }
            }
            out[out_len] = '\0';
            continue;
        }
        if (out_len > 1 && out[out_len - 1] != '/') {
            if (out_len + 1 >= out_size) return -1;
            out[out_len++] = '/';
        }
        if (out_len + seg_len + 1 >= out_size) return -1;
        for (size_t s = 0; s < seg_len; ++s) {
            out[out_len++] = segment[s];
        }
        out[out_len] = '\0';
    }
    if (out_len > 1 && out[out_len - 1] == '/') {
        out[out_len - 1] = '\0';
    }
    return 0;
}

char* realpath(const char* path, char* resolved_path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    char tmp[512];
    if (path[0] == '/') {
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) {
            errno = EINVAL;
            return NULL;
        }
        size_t len = strlen(cwd);
        if (len + 1 >= sizeof(tmp)) {
            errno = ENOMEM;
            return NULL;
        }
        strcpy(tmp, cwd);
        if (len == 0 || tmp[len - 1] != '/') {
            tmp[len++] = '/';
            tmp[len] = '\0';
        }
        if (len + strlen(path) + 1 >= sizeof(tmp)) {
            errno = ENOMEM;
            return NULL;
        }
        strcpy(tmp + len, path);
    }

    char norm[512];
    if (normalize_path(tmp, norm, sizeof(norm)) != 0) {
        errno = ENOMEM;
        return NULL;
    }

    if (!resolved_path) {
        resolved_path = (char*)malloc(strlen(norm) + 1);
        if (!resolved_path) {
            errno = ENOMEM;
            return NULL;
        }
    }
    strcpy(resolved_path, norm);
    return resolved_path;
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
