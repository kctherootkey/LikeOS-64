/*
 * err.h - 4.4BSD err / warn family for fatal and non-fatal diagnostics.
 */
#ifndef _ERR_H
#define _ERR_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void err(int eval, const char* fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void errx(int eval, const char* fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warnx(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void verr(int eval, const char* fmt, va_list ap) __attribute__((noreturn));
void verrx(int eval, const char* fmt, va_list ap) __attribute__((noreturn));
void vwarn(const char* fmt, va_list ap);
void vwarnx(const char* fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* _ERR_H */
