/*
 * err.c - 4.4BSD err / warn family.
 *
 * Each routine writes "<progname>: <fmt>: <strerror(errno)>\n" to stderr
 * (errx/warnx omit the strerror suffix).  The err* variants exit with the
 * supplied status after printing.
 */
#include "../../include/err.h"
#include "../../include/stdio.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"
#include "../../include/errno.h"
#include "../../include/unistd.h"

extern char** environ;

const char* __progname = "";

void __libc_set_progname(const char* argv0) {
    if (!argv0 || !argv0[0]) return;
    /* Use the basename of argv[0], matching BSD err() behaviour. */
    const char* p = argv0;
    for (const char* q = argv0; *q; q++)
        if (*q == '/') p = q + 1;
    __progname = p;
}

static const char* _progname(void) {
    return (__progname && __progname[0]) ? __progname : "?";
}

void vwarn(const char* fmt, va_list ap) {
    int saved = errno;
    fprintf(stderr, "%s: ", _progname());
    if (fmt) vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(saved));
}

void vwarnx(const char* fmt, va_list ap) {
    fprintf(stderr, "%s: ", _progname());
    if (fmt) vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

void warnx(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

void verr(int eval, const char* fmt, va_list ap) {
    vwarn(fmt, ap);
    exit(eval);
}

void verrx(int eval, const char* fmt, va_list ap) {
    vwarnx(fmt, ap);
    exit(eval);
}

void err(int eval, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    verr(eval, fmt, ap);
    /* unreachable */
    va_end(ap);
}

void errx(int eval, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    verrx(eval, fmt, ap);
    /* unreachable */
    va_end(ap);
}
